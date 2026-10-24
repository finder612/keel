# keel

Resilience primitives for C++ backends — **retry**, **backoff**, **circuit
breaker**, **fallback**, **timeout**, and a **rate limiter** — in a small,
header-only, dependency-free library (C++17).

Backend and database calls fail: connections reset, replicas lag, services time
out. `keel` gives you the standard patterns for handling that gracefully,
with an API that stays out of the way:

```cpp
#include "keel/keel.hpp"
using namespace keel;

RetryPolicy policy;
policy.max_attempts(5)
      .backoff(Backoff::exponential(Duration{50}).with_jitter().cap(Duration{2000}))
      .retry_if([](const std::exception& e) { return is_transient(e); });

auto rows = retry(policy, [&] { return db.query("SELECT ..."); });
```

## Why

- **The patterns you actually need.** Retry with configurable backoff + jitter,
  a thread-safe circuit breaker, and fallbacks — nothing more.
- **Deterministically testable.** Clocks, sleepers and the jitter RNG are all
  injectable, so *your* retry logic can be unit-tested without real waiting.
- **Header-only, zero dependencies.** Just the standard library (plus threads).
- **Small and readable.** A handful of focused headers you can audit yourself.

## Install

### Copy the headers

Everything lives under [`include/keel/`](include/keel). Copy the folder
into your project and `#include "keel/keel.hpp"`.

### CMake (`add_subdirectory`)

Vendor the repository (for example as a git submodule) and add it to your
build:

```cmake
add_subdirectory(third_party/keel)
target_link_libraries(your_target PRIVATE keel::keel)
```

### CMake (installed)

```bash
cmake -B build -DKEEL_BUILD_TESTS=OFF
cmake --build build --target install
```

```cmake
find_package(keel REQUIRED)
target_link_libraries(your_target PRIVATE keel::keel)
```

## The pieces

### `retry(policy, fn)`

Re-runs `fn` when it throws a `std::exception` the policy accepts, waiting
between attempts according to a `Backoff`. Returns whatever `fn` returns
(including `void`); rethrows the last exception once attempts are exhausted.

| `RetryPolicy` setter | Meaning                                                  |
|----------------------|----------------------------------------------------------|
| `max_attempts(n)`    | Total attempts including the first (default 3).          |
| `backoff(b)`         | The wait policy between attempts (default exp. 100ms).   |
| `retry_if(pred)`     | Retry only when `pred(exception)` is true.               |
| `on_retry(cb)`       | Observe each retry: `(attempt, exception, delay)`.       |
| `sleeper(fn)`        | Override how waits happen (tests inject a no-op).        |
| `random(fn)`         | Override the jitter RNG (tests inject a fixed value).    |

### `Backoff`

```cpp
Backoff::fixed(Duration{100});                       // 100ms every time
Backoff::linear(Duration{100});                      // 100, 200, 300, ...
Backoff::exponential(Duration{100}, /*factor=*/2.0); // 100, 200, 400, ...
```

Tunable with `.cap(max)` (clamp the delay) and `.with_jitter(ratio)` (randomize
to avoid a thundering herd; `ratio = 1.0` is full jitter). `delay_for(attempt)`
returns the delay for a given 1-based attempt.

### `CircuitBreaker`

A thread-safe `Closed → Open → HalfOpen` breaker that fails fast with
`circuit_open_error` while a dependency is unhealthy.

```cpp
CircuitBreaker cb{CircuitConfig{}
                      .with_failure_threshold(5)    // trip after 5 failures
                      .with_success_threshold(1)    // close after 1 trial success
                      .with_reset_timeout(Duration{30000})};

auto value = cb.execute([&] { return call_service(); });
```

### `with_fallback(primary, fallback)`

Run `primary`; if it throws, run `fallback` instead. `with_fallback_if` takes a
predicate so you only fall back for specific exceptions.

```cpp
auto user = with_fallback([&] { return db.load_user(id); },
                          [&] { return cache.load_user(id); });
```

### `retry_while(policy, fn, is_failure)`

Value-based retry, for APIs that signal failure with a return value instead of
an exception. Re-runs `fn` while `is_failure(result)` is true, waiting per the
policy's backoff, and returns the last result.

```cpp
auto status = retry_while(policy, [&] { return api.poll(); },
                          [](Status s) { return s == Status::Pending; });
```

### `timeout(limit, fn)`

Give a single call a time budget, throwing `timeout_error` if it runs long.

```cpp
auto rows = timeout(Duration{500}, [&] { return db.query("..."); });
```

> **Note:** on timeout the operation is *not* cancelled — it keeps running in a
> detached thread until it finishes. Only use this with work that is safe to
> abandon; for real cancellation prefer the operation's native deadline (e.g. a
> socket or query timeout).

### `RateLimiter`

A thread-safe token-bucket limiter. Tokens refill at `rate_per_second` up to
`burst`; each call consumes one.

```cpp
RateLimiter limiter{/*rate_per_second=*/100.0, /*burst=*/20.0};

if (limiter.try_acquire()) { /* send request */ }
auto result = limiter.execute([&] { return call_service(); });  // or throw rate_limited_error
```

## Testing your own resilience logic

Because time is injectable, you can assert on retry behavior without sleeping:

```cpp
std::vector<long long> waited;
RetryPolicy p;
p.max_attempts(3)
 .backoff(Backoff::exponential(Duration{10}))
 .sleeper([&](Duration d) { waited.push_back(d.count()); });  // no real sleep

// ... run retry(p, flaky) and assert waited == {10, 20}
```

The circuit breaker takes a `ClockFn` you can advance by hand; see
[`tests/tests.cpp`](tests/tests.cpp) for worked examples.

## Build the tests and examples

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/examples/keel_example
```

Requires a C++17 compiler. CI covers GCC, Clang and MSVC.

## Composing the primitives

Each primitive takes and returns a callable, so they nest directly — no special
combinator needed. For example, "retry through a circuit breaker, falling back
to a cache":

```cpp
auto user = with_fallback(
    [&] {
        return retry(policy, [&] {
            return cb.execute([&] { return db.load_user(id); });
        });
    },
    [&] { return cache.load_user(id); });
```

## Roadmap

- A combined `pipeline` builder to express the composition above declaratively.
- Decorrelated-jitter backoff (AWS-style).
- An awaitable/coroutine-friendly surface.

Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT — see [LICENSE](LICENSE).
