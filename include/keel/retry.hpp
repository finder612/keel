// retry() — re-run a fallible operation according to a RetryPolicy.
//
//   keel::RetryPolicy policy;
//   policy.max_attempts(5)
//         .backoff(keel::Backoff::exponential(keel::Duration{50}).with_jitter().cap(keel::Duration{2000}))
//         .retry_if([](const std::exception&) { return true; });
//
//   auto rows = keel::retry(policy, [&] { return db.query("SELECT ..."); });
//
// The operation is retried when it throws a std::exception that the policy's
// filter accepts. Exceptions not derived from std::exception always propagate.
// When attempts are exhausted the last exception is rethrown.
#ifndef KEEL_RETRY_HPP
#define KEEL_RETRY_HPP

#include <cstddef>
#include <exception>
#include <functional>
#include <thread>

#include "keel/backoff.hpp"
#include "keel/common.hpp"

namespace keel {

// Decides whether a thrown exception is worth retrying.
using ExceptionFilter = std::function<bool(const std::exception&)>;

// Observer invoked before each wait: (attempt number, the exception, the delay).
using RetryHandler =
    std::function<void(std::size_t, const std::exception&, Duration)>;

// Performs the wait between attempts. Injected so tests need not really sleep.
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
        backoff_ = b;
        return *this;
    }
    RetryPolicy& retry_if(ExceptionFilter f) {
        filter_ = std::move(f);
        return *this;
    }
    RetryPolicy& on_retry(RetryHandler h) {
        on_retry_ = std::move(h);
        return *this;
    }
    RetryPolicy& sleeper(SleepFn s) {
        sleeper_ = std::move(s);
        return *this;
    }
    RetryPolicy& random(RandomFn r) {
        random_ = std::move(r);
        return *this;
    }

    // --- Accessors (used by retry) -----------------------------------------

    std::size_t max_attempts() const { return max_attempts_; }
    const Backoff& backoff() const { return backoff_; }
    const ExceptionFilter& exception_filter() const { return filter_; }
    const RetryHandler& retry_handler() const { return on_retry_; }
    const SleepFn& sleep_fn() const { return sleeper_; }
    const RandomFn& random_fn() const { return random_; }

private:
    std::size_t max_attempts_ = 3;
    Backoff backoff_ = Backoff::exponential(Duration{100});
    ExceptionFilter filter_;   // empty => retry every std::exception
    RetryHandler on_retry_;    // empty => no observer
    SleepFn sleeper_;          // empty => default_sleep
    RandomFn random_;          // empty => default_random
};

// Run `fn` under `policy`, returning whatever `fn` returns (including void).
template <class Fn>
auto retry(const RetryPolicy& policy, Fn&& fn) -> decltype(fn()) {
    const std::size_t attempts =
        policy.max_attempts() == 0 ? std::size_t{1} : policy.max_attempts();

    std::size_t attempt = 1;
    for (;;) {
        try {
            return fn();
        } catch (const std::exception& e) {
            const auto& filter = policy.exception_filter();
            const bool retryable = filter ? filter(e) : true;
            if (attempt >= attempts || !retryable) throw;

            const Duration delay = policy.backoff().delay_for(attempt, policy.random_fn());
            if (policy.retry_handler()) policy.retry_handler()(attempt, e, delay);

            const auto& sleep = policy.sleep_fn();
            if (sleep)
                sleep(delay);
            else
                default_sleep(delay);

            ++attempt;
        }
    }
}

// Value-based retry: re-run `fn` while `is_failure(result)` is true, waiting
// between attempts according to the policy's backoff. Returns the last result
// (even if it still looks like a failure after the final attempt). Use this for
// APIs that signal failure with a return value rather than an exception.
//
// Differences from retry(): exceptions thrown by `fn` propagate immediately and
// are not retried here, and the policy's retry_if / on_retry hooks do not apply
// (they are exception-oriented). `fn` must return a non-void value.
template <class Fn, class IsFailure>
auto retry_while(const RetryPolicy& policy, Fn&& fn, IsFailure&& is_failure)
    -> decltype(fn()) {
    const std::size_t attempts =
        policy.max_attempts() == 0 ? std::size_t{1} : policy.max_attempts();

    std::size_t attempt = 1;
    for (;;) {
        auto result = fn();
        if (attempt >= attempts || !is_failure(result)) return result;

        const Duration delay = policy.backoff().delay_for(attempt, policy.random_fn());
        const auto& sleep = policy.sleep_fn();
        if (sleep)
            sleep(delay);
        else
            default_sleep(delay);

        ++attempt;
    }
}

}  // namespace keel

#endif  // KEEL_RETRY_HPP
