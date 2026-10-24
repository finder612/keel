// Bulkhead: cap how many calls to one dependency can run at the same time.
//
// The name comes from ships: watertight compartments keep one breach from
// sinking the whole hull. In a service, one slow dependency can quietly tie up
// every worker thread you have, and then everything is down, not just the
// part that talks to it. A bulkhead gives each dependency a fixed number of
// slots. When they're all taken, extra callers wait briefly in a small queue,
// or are turned away at once with bulkhead_full_error.
//
//   keel::Bulkhead payments{keel::BulkheadConfig{}
//                               .with_max_concurrent(8)   // 8 calls at once
//                               .with_max_queue(16)       // 16 more may wait...
//                               .with_max_wait(keel::Duration{50})};  // ...up to 50ms
//
//   auto receipt = payments.execute([&] { return gateway.charge(order); });
//
// For callback-style code, take a Permit and keep it alive for as long as the
// call is running; dropping it frees the slot.
//
// The bulkhead is thread-safe. Waiting callers are woken as slots free up, but
// not in strict arrival order.
#ifndef KEEL_BULKHEAD_HPP
#define KEEL_BULKHEAD_HPP

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

#include "keel/common.hpp"

namespace keel {

// Thrown by Bulkhead::execute when no slot became free in time.
class bulkhead_full_error : public error {
public:
    bulkhead_full_error() : error("keel: bulkhead is full") {}
};

struct BulkheadConfig {
    // Calls allowed to run at the same time.
    std::size_t max_concurrent = 10;
    // Callers allowed to wait for a slot when all are taken. Zero means
    // nobody waits: a full bulkhead rejects immediately.
    std::size_t max_queue = 0;
    // How long a waiting caller of execute() waits before giving up.
    Duration max_wait = Duration{0};

    BulkheadConfig& with_max_concurrent(std::size_t n) {
        max_concurrent = n;
        return *this;
    }
    BulkheadConfig& with_max_queue(std::size_t n) {
        max_queue = n;
        return *this;
    }
    BulkheadConfig& with_max_wait(Duration d) {
        max_wait = d;
        return *this;
    }
};

class Bulkhead {
public:
    // Holds one slot until it is destroyed or release() is called.
    class Permit {
    public:
        Permit(Permit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        Permit& operator=(Permit&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        ~Permit() { release(); }

        // Give the slot back early. Safe to call more than once.
        void release() noexcept {
            if (Bulkhead* owner = std::exchange(owner_, nullptr)) owner->release_slot();
        }

    private:
        friend class Bulkhead;
        explicit Permit(Bulkhead* owner) : owner_(owner) {}

        Bulkhead* owner_;
    };

    explicit Bulkhead(BulkheadConfig config = {}) : config_(config) {}

    Bulkhead(const Bulkhead&) = delete;
    Bulkhead& operator=(const Bulkhead&) = delete;

    // Take a slot if one is free right now; never waits.
    std::optional<Permit> try_acquire() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (in_flight_ >= config_.max_concurrent) return std::nullopt;
        ++in_flight_;
        return Permit(this);
    }

    // Take a slot, waiting up to `max_wait` for one if there is room in the
    // queue. Empty if the queue is full or the wait ran out.
    std::optional<Permit> acquire_for(Duration max_wait) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (in_flight_ < config_.max_concurrent) {
            ++in_flight_;
            return Permit(this);
        }
        if (max_wait.count() <= 0 || waiting_ >= config_.max_queue) return std::nullopt;

        ++waiting_;
        const bool got_slot = slot_freed_.wait_for(
            lock, max_wait, [this] { return in_flight_ < config_.max_concurrent; });
        --waiting_;
        if (!got_slot) return std::nullopt;
        ++in_flight_;
        return Permit(this);
    }

    // Run `fn` in a slot, waiting up to the configured max_wait for one.
    // Throws bulkhead_full_error if none frees up. The slot is given back when
    // `fn` returns or throws.
    template <class Fn>
    auto execute(Fn&& fn) -> decltype(fn()) {
        std::optional<Permit> permit = acquire_for(config_.max_wait);
        if (!permit) throw bulkhead_full_error{};
        return fn();
    }

    // Calls currently holding a slot.
    std::size_t in_flight() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return in_flight_;
    }

    // Callers currently waiting for a slot.
    std::size_t waiting() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return waiting_;
    }

    // Free slots right now.
    std::size_t available() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return in_flight_ >= config_.max_concurrent ? 0 : config_.max_concurrent - in_flight_;
    }

private:
    void release_slot() noexcept {
        bool someone_waiting = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            --in_flight_;
            someone_waiting = waiting_ > 0;
        }
        // Waiters register under the same lock before they block, so if none
        // is counted here, none can miss this wake-up. Skipping the notify on
        // the common, uncontended path saves a kernel round trip.
        if (someone_waiting) slot_freed_.notify_one();
    }

    const BulkheadConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable slot_freed_;
    std::size_t in_flight_ = 0;
    std::size_t waiting_ = 0;
};

}  // namespace keel

#endif  // KEEL_BULKHEAD_HPP
