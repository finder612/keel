// timeout(): give a single operation a time budget.
//
//   auto rows = keel::timeout(keel::Duration{500}, [&] { return db.query("..."); });
//
// If the operation finishes within the limit, its result is returned (or its
// exception rethrown). Otherwise timeout_error is thrown.
//
// IMPORTANT: on timeout the operation is NOT cancelled. It keeps running in a
// detached thread until it finishes on its own. Only use this with work that is
// safe to abandon: it must not touch caller-local objects that may be
// destroyed after the timeout, and it should free its own resources when it
// eventually completes. For work that supports real cancellation, prefer the
// operation's native deadline (e.g. a socket or query timeout) instead.
#ifndef KEEL_TIMEOUT_HPP
#define KEEL_TIMEOUT_HPP

#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

#include "keel/common.hpp"

namespace keel {

// Thrown by timeout() when the operation exceeds its budget.
class timeout_error : public std::runtime_error {
public:
    timeout_error() : std::runtime_error("keel: operation timed out") {}
};

template <class Fn>
auto timeout(Duration limit, Fn&& fn) -> decltype(fn()) {
    using Result = decltype(fn());

    std::packaged_task<Result()> task(std::forward<Fn>(fn));
    std::future<Result> future = task.get_future();

    // Detached, so the std::future destructor does not block waiting for the
    // task to finish (which would defeat the timeout).
    std::thread(std::move(task)).detach();

    if (future.wait_for(limit) != std::future_status::ready) throw timeout_error{};
    return future.get();
}

}  // namespace keel

#endif  // KEEL_TIMEOUT_HPP
