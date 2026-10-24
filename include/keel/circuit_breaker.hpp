// CircuitBreaker: stop hammering a failing dependency.
//
// A breaker has three states:
//
//   Closed    calls pass through; consecutive failures are counted. After
//             `failure_threshold` of them the breaker trips to Open.
//   Open      calls fail fast with circuit_open_error, without touching the
//             dependency. After `reset_timeout` elapses the breaker moves to
//             HalfOpen.
//   HalfOpen  up to `max_half_open_calls` trial calls may run at once; extra
//             callers are rejected. `success_threshold` successful trials close
//             the breaker; any failed trial opens it again.
//
//   keel::CircuitBreaker cb{keel::CircuitConfig{}
//                               .with_failure_threshold(5)
//                               .with_reset_timeout(keel::Duration{30000})
//                               .with_failure_if([](const std::exception& e) {
//                                   return !is_not_found(e);  // a 404 is not an outage
//                               })};
//   auto value = cb.execute([&] { return call_service(); });
//
// For callback-style or async code that cannot wrap the call in execute(),
// take a Permit with try_acquire() and report the outcome on it later.
//
// The breaker is thread-safe. The clock is injectable for deterministic tests.
#ifndef KEEL_CIRCUIT_BREAKER_HPP
#define KEEL_CIRCUIT_BREAKER_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "keel/common.hpp"

// When the wrapped function always throws (common with test doubles), the
// success path after it is legitimately unreachable and MSVC says so (C4702)
// once it inlines the call. That is noise in a header, so silence it here.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

namespace keel {

enum class CircuitState : std::uint8_t { Closed, Open, HalfOpen };

inline const char* to_string(CircuitState s) noexcept {
    switch (s) {
        case CircuitState::Closed:
            return "closed";
        case CircuitState::Open:
            return "open";
        case CircuitState::HalfOpen:
            return "half-open";
    }
    return "unknown";
}

// Thrown by CircuitBreaker::execute when the breaker refuses a call.
class circuit_open_error : public error {
public:
    explicit circuit_open_error(Duration retry_after = Duration{0})
        : error("keel: circuit is open"), retry_after_(retry_after) {}

    // Roughly how long until the breaker lets trial calls through again. Zero
    // when it is already half-open and only turned this call away because the
    // trial slots were taken.
    Duration retry_after() const noexcept { return retry_after_; }

private:
    Duration retry_after_;
};

struct CircuitConfig {
    // Consecutive failures in Closed before tripping to Open.
    std::size_t failure_threshold = 5;
    // Successful trial calls in HalfOpen before closing again.
    std::size_t success_threshold = 1;
    // How long to stay Open before allowing trial calls (HalfOpen).
    Duration reset_timeout = Duration{30000};
    // How many trial calls may run at the same time while HalfOpen. Keeping
    // this small is what stops a recovering service from being flooded.
    std::size_t max_half_open_calls = 1;
    // Decides which exceptions count as failures. Empty means all of them.
    // Exceptions it rejects (think "user not found") pass through untouched
    // and do not affect the breaker. Non-std exceptions always count.
    std::function<bool(const std::exception&)> failure_if;
    // Called after every state change, outside the breaker's lock, from
    // whichever thread caused it. Use it for logging and metrics. It must not
    // throw.
    std::function<void(CircuitState from, CircuitState to)> on_state_change;

    CircuitConfig& with_failure_threshold(std::size_t n) {
        failure_threshold = n;
        return *this;
    }
    CircuitConfig& with_success_threshold(std::size_t n) {
        success_threshold = n;
        return *this;
    }
    CircuitConfig& with_reset_timeout(Duration d) {
        reset_timeout = d;
        return *this;
    }
    CircuitConfig& with_max_half_open_calls(std::size_t n) {
        max_half_open_calls = n;
        return *this;
    }
    CircuitConfig& with_failure_if(std::function<bool(const std::exception&)> f) {
        failure_if = std::move(f);
        return *this;
    }
    CircuitConfig& with_on_state_change(std::function<void(CircuitState, CircuitState)> f) {
        on_state_change = std::move(f);
        return *this;
    }
};

class CircuitBreaker {
    enum class Outcome : std::uint8_t { Success, Failure, Ignored };

public:
    // Permission to make one call, for code that cannot use execute(). Report
    // how the call went with success() or failure(). A permit dropped without a
    // report counts as neither, and simply frees its slot.
    class Permit {
    public:
        Permit(Permit&& other) noexcept
            : breaker_(std::exchange(other.breaker_, nullptr)), ticket_(other.ticket_) {}
        Permit& operator=(Permit&&) = delete;
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;

        ~Permit() {
            if (!breaker_) return;
            try {
                breaker_->record(ticket_, Outcome::Ignored);
            } catch (...) {  // NOLINT(bugprone-empty-catch): destructors must not throw
            }
        }

        void success() { report(Outcome::Success); }
        void failure() { report(Outcome::Failure); }

    private:
        friend class CircuitBreaker;
        Permit(CircuitBreaker* breaker, std::uint64_t ticket)
            : breaker_(breaker), ticket_(ticket) {}

        void report(Outcome outcome) {
            if (CircuitBreaker* b = std::exchange(breaker_, nullptr)) b->record(ticket_, outcome);
        }

        CircuitBreaker* breaker_;
        std::uint64_t ticket_;
    };

    explicit CircuitBreaker(CircuitConfig config = {}, ClockFn clock = {})
        : config_(std::move(config)),
          now_(clock ? std::move(clock) : ClockFn{&default_now}) {}

    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;

    // The current state. Reading it may move an expired Open breaker to
    // HalfOpen (and fire on_state_change), so it is not a pure observer.
    CircuitState state() const {
        Transitions changes;
        CircuitState s;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            refresh(changes);
            s = state_;
        }
        notify(changes);
        return s;
    }

    std::size_t consecutive_failures() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return failures_;
    }

    // How long until an Open breaker allows trial calls; zero otherwise.
    Duration retry_after() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return state_ == CircuitState::Open ? remaining_open_time() : Duration{0};
    }

    // Run `fn` through the breaker. Throws circuit_open_error without calling
    // `fn` when the breaker refuses; otherwise records the outcome and returns
    // the result or rethrows `fn`'s exception.
    template <class Fn>
    auto execute(Fn&& fn) -> decltype(fn()) {
        using Result = decltype(fn());
        const std::uint64_t ticket = admit();

        // On success the outcome is recorded outside the try block, so an
        // exception from on_state_change is never mistaken for a call failure.
        if constexpr (std::is_void_v<Result>) {
            run_guarded(ticket, fn);
            record(ticket, Outcome::Success);
        } else {
            Result value = run_guarded(ticket, fn);
            record(ticket, Outcome::Success);
            return value;
        }
    }

    // Ask for a Permit; empty when the breaker refuses the call right now.
    std::optional<Permit> try_acquire() {
        Transitions changes;
        std::uint64_t ticket = 0;
        bool admitted = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            refresh(changes);
            admitted = admit_locked(ticket);
        }
        notify(changes);
        if (!admitted) return std::nullopt;
        return Permit(this, ticket);
    }

    // Close the breaker and clear its counters, e.g. after a manual fix.
    void reset() {
        Transitions changes;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            transition(CircuitState::Closed, changes);
            failures_ = 0;  // also when it was already closed
        }
        notify(changes);
    }

    // Open the breaker now, e.g. for planned maintenance. It recovers through
    // HalfOpen after reset_timeout like after any other trip.
    void force_open() {
        Transitions changes;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            trip(changes);
        }
        notify(changes);
    }

private:
    // A tiny fixed-size list of state changes made under the lock, delivered to
    // on_state_change after the lock is released. One locked section makes at
    // most two changes (Open -> HalfOpen, then HalfOpen -> Open or Closed).
    struct Transitions {
        std::array<std::pair<CircuitState, CircuitState>, 2> items{};
        std::size_t count = 0;
        void add(CircuitState from, CircuitState to) {
            if (count < items.size()) items[count++] = {from, to};
        }
    };

    template <class Fn>
    decltype(auto) run_guarded(std::uint64_t ticket, Fn& fn) {
        try {
            return fn();
        } catch (const std::exception& e) {
            record(ticket, classify(e));
            throw;
        } catch (...) {
            record(ticket, Outcome::Failure);
            throw;
        }
    }

    Outcome classify(const std::exception& e) const {
        if (!config_.failure_if) return Outcome::Failure;
        return config_.failure_if(e) ? Outcome::Failure : Outcome::Ignored;
    }

    // Lets a call in or throws circuit_open_error. Returns the generation the
    // call belongs to, so its outcome can be matched to the right state later.
    std::uint64_t admit() {
        Transitions changes;
        std::uint64_t ticket = 0;
        bool admitted = false;
        Duration wait{0};
        {
            std::lock_guard<std::mutex> guard(mutex_);
            refresh(changes);
            admitted = admit_locked(ticket);
            if (!admitted && state_ == CircuitState::Open) wait = remaining_open_time();
        }
        notify(changes);
        if (!admitted) throw circuit_open_error(wait);
        return ticket;
    }

    // Caller must hold the lock.
    bool admit_locked(std::uint64_t& ticket) {
        if (state_ == CircuitState::Open) return false;
        if (state_ == CircuitState::HalfOpen) {
            if (half_open_in_flight_ >= config_.max_half_open_calls) return false;
            ++half_open_in_flight_;
        }
        ticket = generation_;
        return true;
    }

    void record(std::uint64_t ticket, Outcome outcome) {
        Transitions changes;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            refresh(changes);
            // A call that started under an earlier state (say, a slow success
            // that began before the breaker tripped) says nothing about the
            // current one, so its outcome is dropped.
            if (ticket == generation_) apply(outcome, changes);
        }
        notify(changes);
    }

    // Caller must hold the lock.
    void apply(Outcome outcome, Transitions& changes) {
        if (state_ == CircuitState::Closed) {
            if (outcome == Outcome::Success) {
                failures_ = 0;
            } else if (outcome == Outcome::Failure) {
                if (++failures_ >= config_.failure_threshold) trip(changes);
            }
        } else if (state_ == CircuitState::HalfOpen) {
            if (half_open_in_flight_ > 0) --half_open_in_flight_;
            if (outcome == Outcome::Success) {
                if (++half_open_successes_ >= config_.success_threshold) {
                    transition(CircuitState::Closed, changes);
                }
            } else if (outcome == Outcome::Failure) {
                trip(changes);
            }
        }
    }

    // Moves an expired Open breaker to HalfOpen. Caller must hold the lock.
    void refresh(Transitions& changes) const {
        if (state_ == CircuitState::Open && now_() - opened_at_ >= config_.reset_timeout) {
            transition(CircuitState::HalfOpen, changes);
        }
    }

    // Caller must hold the lock. Restarts the open timer even if already open.
    void trip(Transitions& changes) {
        transition(CircuitState::Open, changes);
        opened_at_ = now_();
    }

    // Caller must hold the lock. Every real state change starts a new
    // generation and resets the counters, which belong to one state only.
    void transition(CircuitState to, Transitions& changes) const {
        if (state_ == to) return;
        changes.add(state_, to);
        state_ = to;
        ++generation_;
        failures_ = 0;
        half_open_successes_ = 0;
        half_open_in_flight_ = 0;
    }

    // Caller must hold the lock.
    Duration remaining_open_time() const {
        const auto left = opened_at_ + config_.reset_timeout - now_();
        if (left <= SteadyClock::duration::zero()) return Duration{0};
        return std::chrono::ceil<Duration>(left);
    }

    void notify(const Transitions& changes) const {
        if (!config_.on_state_change) return;
        for (std::size_t i = 0; i < changes.count; ++i) {
            config_.on_state_change(changes.items[i].first, changes.items[i].second);
        }
    }

    const CircuitConfig config_;
    const ClockFn now_;

    // Mutable because state() is const but may advance Open to HalfOpen once
    // the reset timeout has passed. All of these are guarded by mutex_.
    mutable std::mutex mutex_;
    mutable CircuitState state_ = CircuitState::Closed;
    mutable std::uint64_t generation_ = 0;
    mutable std::size_t failures_ = 0;
    mutable std::size_t half_open_successes_ = 0;
    mutable std::size_t half_open_in_flight_ = 0;
    TimePoint opened_at_{};
};

}  // namespace keel

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

#endif  // KEEL_CIRCUIT_BREAKER_HPP
