// SingleFlight: collapse identical concurrent calls into one.
//
// A popular cache entry expires, and a thousand requests that all need it hit
// the database at the same moment, each asking for exactly the same row. That
// is a cache stampede, and it can take a database down on its own.
//
// SingleFlight lets the first caller for a key do the work. Anyone who asks
// for the same key while that call is still running does not start their own;
// they wait and get the same result, or the same exception.
//
//   keel::SingleFlight<std::string, User> loads;
//
//   User load_user(const std::string& id) {
//       return loads.run(id, [&] { return db.load_user(id); });
//   }
//
// Nothing is cached: once a call finishes, the next caller for that key starts
// a fresh one. Pair it with your cache rather than replacing it.
//
// Value must be copyable, since every waiter receives its own copy. Do not
// call run() for a key from inside the function that is computing that same
// key; it would wait for itself forever.
#ifndef KEEL_SINGLE_FLIGHT_HPP
#define KEEL_SINGLE_FLIGHT_HPP

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace keel {

template <class Key, class Value, class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>>
class SingleFlight {
    static_assert(!std::is_void_v<Value>, "SingleFlight needs a non-void Value");
    static_assert(!std::is_reference_v<Value>, "SingleFlight<Key, T&> is not supported");

public:
    SingleFlight() = default;
    SingleFlight(const SingleFlight&) = delete;
    SingleFlight& operator=(const SingleFlight&) = delete;

    // Run `fn` for `key`, or join a call for `key` that is already running.
    // Returns the result; rethrows the exception if the call failed.
    template <class Fn>
    Value run(const Key& key, Fn&& fn) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto it = calls_.find(key);
        if (it != calls_.end()) {
            Call& call = it->second;
            // The shared state is only created once somebody actually joins,
            // so a call nobody else asked for never pays for it.
            if (!call.promise) {
                call.promise.emplace();
                call.future = call.promise->get_future().share();
            }
            ++call.waiters;
            std::shared_future<Value> future = call.future;
            lock.unlock();
            return future.get();
        }

        const std::uint64_t id = ++next_id_;
        start_call(key, id);
        lock.unlock();

        try {
            Value value = fn();
            publish(key, id, [&](std::promise<Value>& p) { p.set_value(value); });
            return value;
        } catch (...) {
            const std::exception_ptr error = std::current_exception();
            publish(key, id, [&](std::promise<Value>& p) { p.set_exception(error); });
            throw;
        }
    }

    // Detach the running call for `key`, if any, so the next caller starts a
    // fresh one instead of joining it. Callers already waiting still get the
    // old call's result. Use it when you know that result is already stale,
    // for example right after the underlying data was written.
    void forget(const Key& key) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = calls_.find(key);
        if (it == calls_.end()) return;
        // Waiters still need the result, so keep their promise alive by id.
        if (it->second.promise) detached_.emplace(it->second.id, std::move(*it->second.promise));
        recycle(it);
    }

    // Number of keys with a call currently running.
    std::size_t in_flight() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return calls_.size();
    }

    // How many callers have joined the running call for `key` instead of
    // starting their own (0 if nothing is running). Handy for metrics.
    std::size_t waiters(const Key& key) const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = calls_.find(key);
        return it == calls_.end() ? 0 : it->second.waiters;
    }

private:
    struct Call {
        std::uint64_t id = 0;  // tells apart calls that reused the same key
        std::optional<std::promise<Value>> promise;  // created when the first waiter joins
        std::shared_future<Value> future;
        std::size_t waiters = 0;
    };

    using Map = std::unordered_map<Key, Call, Hash, KeyEqual>;

    // Map nodes kept for reuse. With a steady set of hot keys this means no
    // allocation per call for the map itself.
    static constexpr std::size_t kMaxSpareNodes = 64;

    // Caller must hold the lock.
    void start_call(const Key& key, std::uint64_t id) {
        if constexpr (std::is_copy_assignable_v<Key>) {
            if (!spare_.empty()) {
                typename Map::node_type node = std::move(spare_.back());
                spare_.pop_back();
                node.key() = key;
                node.mapped().id = id;
                calls_.insert(std::move(node));
                return;
            }
        }
        Call call;
        call.id = id;
        calls_.emplace(key, std::move(call));
    }

    // Caller must hold the lock. Removes the entry, keeping its node if there
    // is room.
    void recycle(typename Map::iterator it) {
        typename Map::node_type node = calls_.extract(it);
        if (spare_.size() < kMaxSpareNodes) {
            node.mapped() = Call{};
            spare_.push_back(std::move(node));
        }
    }

    // Hands the outcome to anyone who joined call `id`, then retires it. After
    // forget() the key may already point at a newer call, which is left alone.
    template <class Deliver>
    void publish(const Key& key, std::uint64_t id, Deliver deliver) {
        std::optional<std::promise<Value>> promise;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = calls_.find(key);
            if (it != calls_.end() && it->second.id == id) {
                promise = std::move(it->second.promise);
                recycle(it);
            } else {
                auto detached = detached_.find(id);
                if (detached != detached_.end()) {
                    promise = std::move(detached->second);
                    detached_.erase(detached);
                }
            }
        }
        // Delivered outside the lock: waking waiters shouldn't block new calls.
        if (promise) deliver(*promise);
    }

    mutable std::mutex mutex_;
    Map calls_;
    std::vector<typename Map::node_type> spare_;
    std::unordered_map<std::uint64_t, std::promise<Value>> detached_;
    std::uint64_t next_id_ = 0;
};

}  // namespace keel

#endif  // KEEL_SINGLE_FLIGHT_HPP
