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
#include <type_traits>
#include <unordered_map>
#include <utility>

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

        // One hash lookup either finds the running call or reserves the slot.
        auto [it, inserted] = calls_.try_emplace(key);
        if (!inserted) {
            ++it->second.waiters;
            std::shared_future<Value> future = it->second.future;
            lock.unlock();
            return future.get();
        }

        // Only the caller that does the work pays for the shared state.
        std::promise<Value> promise;
        const std::uint64_t id = ++next_id_;
        try {
            it->second.id = id;
            it->second.future = promise.get_future().share();
        } catch (...) {
            calls_.erase(it);
            throw;
        }
        lock.unlock();

        // Remove the entry before publishing the result, so callers arriving
        // after completion start a fresh call instead of reusing this one.
        try {
            Value value = fn();
            finish(key, id);
            promise.set_value(value);
            return value;
        } catch (...) {
            finish(key, id);
            promise.set_exception(std::current_exception());
            throw;
        }
    }

    // Detach the running call for `key`, if any, so the next caller starts a
    // fresh one instead of joining it. Callers already waiting still get the
    // old call's result. Use it when you know that result is already stale,
    // for example right after the underlying data was written.
    void forget(const Key& key) {
        std::lock_guard<std::mutex> guard(mutex_);
        calls_.erase(key);
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
        std::shared_future<Value> future;
        std::size_t waiters = 0;
    };

    // Removes the entry only if it still belongs to call `id`; after forget()
    // the key may already point at a newer call that must be left alone.
    void finish(const Key& key, std::uint64_t id) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = calls_.find(key);
        if (it != calls_.end() && it->second.id == id) calls_.erase(it);
    }

    mutable std::mutex mutex_;
    std::unordered_map<Key, Call, Hash, KeyEqual> calls_;
    std::uint64_t next_id_ = 0;
};

}  // namespace keel

#endif  // KEEL_SINGLE_FLIGHT_HPP
