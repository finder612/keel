// timeout(): give a single operation a time budget.
//
//   auto rows = keel::timeout(keel::Duration{500}, [&] { return db.query("..."); });
//
// If the operation finishes within the limit, its result is returned (or its
// exception rethrown). Otherwise timeout_error is thrown.
//
// C++ cannot forcibly stop a running function, so on timeout the operation
// keeps running in the background until it returns on its own. Two things
// make that manageable:
//
//   * Cooperative cancellation. If your function takes a keel::CancelToken, the
//     token is cancelled the moment the deadline passes, so long-running work
//     can check it and bail out early:
//
//       keel::timeout(keel::Duration{500}, [](keel::CancelToken token) {
//           for (auto& chunk : chunks) {
//               if (token.cancelled()) return partial;
//               process(chunk);
//           }
//           return all;
//       });
//
//   * Your own executor. By default every call gets a fresh detached thread.
//     Pass an Executor to run the work on a thread pool you already have.
//
// Whatever runs past the deadline must not touch caller-local objects that may
// be gone by then. Prefer an operation's native deadline (a socket or query
// timeout) when it has one.
#ifndef KEEL_TIMEOUT_HPP
#define KEEL_TIMEOUT_HPP

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "keel/common.hpp"

namespace keel {

// Thrown by timeout() when the operation exceeds its budget.
class timeout_error : public error {
public:
    explicit timeout_error(Duration limit = Duration{0})
        : error("keel: operation timed out after " + std::to_string(limit.count()) + "ms"),
          limit_(limit) {}

    // The limit that was exceeded.
    Duration limit() const noexcept { return limit_; }

private:
    Duration limit_;
};

// A cancellation flag shared by all of its copies. Cheap to copy.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    bool cancelled() const noexcept { return flag_->load(std::memory_order_acquire); }
    void cancel() noexcept { flag_->store(true, std::memory_order_release); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// Runs a unit of work somewhere else, e.g. by posting it to a thread pool.
// The work must eventually run exactly once; if it is dropped, the caller just
// sees a timeout.
using Executor = std::function<void(std::function<void()>)>;

// The default: a new detached thread per call.
inline void default_executor(std::function<void()> work) {
    std::thread(std::move(work)).detach();
}

namespace detail {

template <class Fn, class = void>
struct timeout_result {
    using type = std::invoke_result_t<Fn&>;
};

template <class Fn>
struct timeout_result<Fn, std::enable_if_t<std::is_invocable_v<Fn&, CancelToken>>> {
    using type = std::invoke_result_t<Fn&, CancelToken>;
};

// Starts `task` on the executor and waits up to `limit` for it. The task is
// kept alive by a shared_ptr, so nothing the background work needs is
// destroyed if we stop waiting.
template <class Result, class OnTimeout>
Result run_with_deadline(Duration limit, const std::shared_ptr<std::packaged_task<Result()>>& task,
                         const Executor& executor, OnTimeout on_timeout) {
    std::future<Result> future = task->get_future();

    // Captured by copy rather than moved: the cost is one reference-count bump,
    // and it keeps static analyzers able to follow who owns the task.
    std::function<void()> job = [task] { (*task)(); };
    if (executor)
        executor(std::move(job));
    else
        default_executor(std::move(job));

    if (future.wait_for(limit) != std::future_status::ready) {
        on_timeout();
        throw timeout_error(limit);
    }
    return future.get();
}

}  // namespace detail

template <class Fn>
auto timeout(Duration limit, Fn&& fn, const Executor& executor = {})
    -> typename detail::timeout_result<std::decay_t<Fn>>::type {
    using Work = std::decay_t<Fn>;
    using Result = typename detail::timeout_result<Work>::type;

    // The task owns its own copy of `fn`. A CancelToken (one more allocation)
    // is only created for functions that actually take one.
    if constexpr (std::is_invocable_v<Work&, CancelToken>) {
        CancelToken token;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            [work = Work(std::forward<Fn>(fn)), token]() mutable -> Result { return work(token); });
        return detail::run_with_deadline<Result>(limit, task, executor,
                                                 [&token] { token.cancel(); });
    } else {
        auto task = std::make_shared<std::packaged_task<Result()>>(Work(std::forward<Fn>(fn)));
        return detail::run_with_deadline<Result>(limit, task, executor, [] {});
    }
}

}  // namespace keel

#endif  // KEEL_TIMEOUT_HPP
