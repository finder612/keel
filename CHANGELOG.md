# Changelog

## 1.3.0

New:

- `RetryBudget`, which caps retries as a share of real traffic so a struggling
  service doesn't get buried by a retry storm. Plug it into a policy with
  `RetryPolicy::budget()`.
- `SingleFlight`, which collapses identical concurrent calls into one, to stop
  cache stampedes. Includes `forget(key)` for results you know are stale.
- `keel::error`, a common base for everything keel throws on its own.
- `RetryPolicy::retry_on<E...>()`, `max_elapsed()` and `clock()`.
- `Backoff::custom()` for your own delay schedule.
- `CircuitConfig::with_failure_if()`, `with_max_half_open_calls()` and
  `with_on_state_change()`, plus `CircuitBreaker::reset()`, `force_open()`,
  `retry_after()`, `try_acquire()` permits and `to_string(CircuitState)`.
- `circuit_open_error::retry_after()` and `rate_limited_error::retry_after()`.
- `timeout()` passes a `CancelToken` to functions that accept one, and takes
  an optional `Executor` so work can run on your own thread pool.
  `timeout_error::limit()` reports the limit that was hit.
- Fallbacks may take the `const std::exception&` that triggered them.
- `RateLimiter::time_until_available()`, and multi-token `execute()`.
- CI now runs AddressSanitizer (with leak and UB checks) and ThreadSanitizer.

Fixed:

- A half-open circuit breaker let every waiting call through at once; it now
  admits `max_half_open_calls` (1 by default).
- A slow call that started before the breaker changed state could have its
  outcome counted against the new state. Such late outcomes are now ignored.
- `RateLimiter` treated a zero or negative rate or burst inconsistently; both
  are now clamped to zero.

Everything from 1.2.0 keeps working unchanged.

## 1.2.0

First public release: `retry` and `retry_while` with fixed, linear and
exponential backoff and jitter, `CircuitBreaker`, `with_fallback`, `timeout`
and `RateLimiter`.
