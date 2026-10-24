// retry(): re-run a fallible operation according to a RetryPolicy.
//
//   keel::RetryPolicy policy;
//   policy.max_attempts(5)
//         .backoff(keel::Backoff::exponential(keel::Duration{50}).with_jitter().cap(keel::Duration{2000}))
//         .retry_on<db::connection_error, db::timeout_error>()
//         .max_elapsed(keel::Duration{3000});
//
//   auto rows = keel::retry(policy, [&] { return db.query("SELECT ..."); });
//
// The operation is retried when it throws a std::exception that the policy's
// filter accepts. Exceptions not derived from std::exception always propagate.
// Retrying stops, and the last exception is rethrown, as soon as any limit is
// reached: the attempt count, the total time budget, or the RetryBudget.
#ifndef KEEL_RETRY_HPP
#define KEEL_RETRY_HPP

#include <cstddef>
#include <exception>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

#include "keel/backoff.hpp"
#include "keel/common.hpp"
#include "keel/retry_budget.hpp"

namespace keel {

// Decides whether a thrown exception is worth retrying.
using ExceptionFilter = std::function<bool(const std::exception&)>;

// Observer invoked before each wait: (attempt number, the exception, the delay).
using RetryHandler =
    std::function<void(std::size_t, const std::exception&, Duration)>;

// Performs the wait between attempts. Injected so tests need not really sleep,
// and so code running on an event loop can plug in its own waiting.
using SleepFn = std::function<void(Duration)>;

inline void default_sleep(Duration d) {
    if (d.count() > 0) std::this_thread::sleep_for(d);
}

class RetryPolicy {
public:
    // --- Fluent setters -----------------------------------------------------

    // Total number of attempts, including the first. Values < 1 are treated
    // as 1 by retry().
    RetryPolicy& max_attempts(std::size_t n) {
        max_attempts_ = n;
        return *this;
    }
    RetryPolicy& backoff(Backoff b) {
        backoff_ = std::move(b);
        return *this;
    }
    // Retry only when `f(exception)` returns true.
    RetryPolicy& retry_if(ExceptionFilter f) {
        filter_ = std::move(f);
        return *this;
    }
    // Shorthand for retry_if: retry only exceptions of (or derived from) the
    // listed types, e.g. retry_on<db::timeout, db::connection_lost>().
    template <class... Exceptions>
    RetryPolicy& retry_on() {
        static_assert(sizeof...(Exceptions) > 0, "retry_on<> needs at least one type");
        static_assert((std::is_base_of_v<std::exception, Exceptions> && ...),
                      "retry_on<> types must derive from std::exception");
        filter_ = [](const std::exception& e) {
            return ((dynamic_cast<const Exceptions*>(&e) != nullptr) || ...);
        };
        return *this;
    }
    // Called before each wait. Handy for logging and metrics.
    RetryPolicy& on_retry(RetryHandler h) {
        on_retry_ = std::move(h);
        return *this;
    }
    // Total time budget across all attempts and waits, measured from the first
    // attempt. A retry whose wait would cross it is not made. Zero (the
    // default) means no limit.
    RetryPolicy& max_elapsed(Duration d) {
        max_elapsed_ = d;
        return *this;
    }
    // Share a RetryBudget with this policy. Each call then counts as a request,
    // and each retry must be approved by the budget. The policy only keeps a
    // reference, so the budget has to outlive every call that uses it.
    RetryPolicy& budget(RetryBudget& b) {
        budget_ = &b;
        return *this;
    }
    // Replace how waits happen (tests pass a no-op).
    RetryPolicy& sleeper(SleepFn s) {
        sleeper_ = std::move(s);
        return *this;
    }
    // Replace the jitter RNG (tests pass a fixed value).
    RetryPolicy& random(RandomFn r) {
        random_ = std::move(r);
        return *this;
    }
    // Replace the clock used for max_elapsed.
    RetryPolicy& clock(ClockFn c) {
        clock_ = std::move(c);
        return *this;
    }

    // --- Accessors ------------------------------------------------------------

    std::size_t max_attempts() const { return max_attempts_; }
    const Backoff& backoff() const { return backoff_; }
    const ExceptionFilter& exception_filter() const { return filter_; }
    const RetryHandler& retry_handler() const { return on_retry_; }
    Duration max_elapsed() const { return max_elapsed_; }
    RetryBudget* retry_budget() const { return budget_; }
    const SleepFn& sleep_fn() const { return sleeper_; }
    const RandomFn& random_fn() const { return random_; }
    const ClockFn& clock_fn() const { return clock_; }

private:
    std::size_t max_attempts_ = 3;
    Backoff backoff_ = Backoff::exponential(Duration{100});
    ExceptionFilter filter_;           // empty => retry every std::exception
    RetryHandler on_retry_;            // empty => no observer
    Duration max_elapsed_{0};          // zero => no time limit
    RetryBudget* budget_ = nullptr;    // null => no budget
    SleepFn sleeper_;                  // empty => default_sleep
    RandomFn random_;                  // empty => default_random
    ClockFn clock_;                    // empty => default_now
};

namespace detail {

// The bookkeeping shared by retry() and retry_while(): counts attempts, checks
// every limit in one place, and performs the waits.
class RetryLoop {
public:
    explicit RetryLoop(const RetryPolicy& policy)
        : policy_(policy),
          attempts_(policy.max_attempts() == 0 ? std::size_t{1} : policy.max_attempts()) {
        if (policy_.max_elapsed().count() > 0) start_ = now();
        if (RetryBudget* budget = policy_.retry_budget()) budget->record_request();
    }

    std::size_t attempt() const { return attempt_; }

    // After a failed attempt: may we try again? If so, returns true and sets
    // `delay` to the wait before the next attempt. Checks are ordered so the
    // budget is only spent when a retry will actually happen.
    bool next(Duration& delay) const {
        if (attempt_ >= attempts_) return false;
        delay = policy_.backoff().delay_for(attempt_, policy_.random_fn());
        if (policy_.max_elapsed().count() > 0 &&
            (now() - start_) + delay > policy_.max_elapsed()) {
            return false;
        }
        RetryBudget* budget = policy_.retry_budget();
        return !(budget && !budget->try_spend_retry());
    }

    void wait(Duration delay) {
        const SleepFn& sleep = policy_.sleep_fn();
        if (sleep)
            sleep(delay);
        else
            default_sleep(delay);
        ++attempt_;
    }

private:
    TimePoint now() const {
        const ClockFn& clock = policy_.clock_fn();
        return clock ? clock() : default_now();
    }

    const RetryPolicy& policy_;
    std::size_t attempts_;
    std::size_t attempt_ = 1;
    TimePoint start_{};
};

}  // namespace detail

// Run `fn` under `policy`, returning whatever `fn` returns (including void).
template <class Fn>
auto retry(const RetryPolicy& policy, Fn&& fn) -> decltype(fn()) {
    detail::RetryLoop loop(policy);
    for (;;) {
        try {
            return fn();
        } catch (const std::exception& e) {
            const ExceptionFilter& filter = policy.exception_filter();
            if (filter && !filter(e)) throw;

            Duration delay{0};
            if (!loop.next(delay)) throw;

            if (policy.retry_handler()) policy.retry_handler()(loop.attempt(), e, delay);
            loop.wait(delay);
        }
    }
}

// Value-based retry: re-run `fn` while `is_failure(result)` is true, waiting
// between attempts according to the policy. Returns the last result (even if
// it still looks like a failure once a limit is hit). Use this for APIs that
// signal failure with a return value rather than an exception.
//
// Differences from retry(): exceptions thrown by `fn` propagate immediately and
// are not retried here, and the exception-oriented retry_if / retry_on /
// on_retry settings do not apply. Every other limit does. `fn` must return a
// non-void value.
template <class Fn, class IsFailure>
auto retry_while(const RetryPolicy& policy, Fn&& fn, IsFailure&& is_failure)
    -> decltype(fn()) {
    detail::RetryLoop loop(policy);
    for (;;) {
        auto result = fn();
        if (!is_failure(result)) return result;

        Duration delay{0};
        if (!loop.next(delay)) return result;
        loop.wait(delay);
    }
}

}  // namespace keel

#endif  // KEEL_RETRY_HPP
