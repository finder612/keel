# keel

A small header-only C++17 library for making backend calls survive bad days:
retries with backoff, a circuit breaker, fallbacks, timeouts and a rate limiter.
No dependencies beyond the standard library.

Current version: **1.2.0**

## Why this exists

If you've written a service that talks to a database or another service over
the network, you've written retry loops. Probably more than once, probably
slightly differently each time, and probably with a `sleep` in there that made
the tests slow.

keel is those loops written once, carefully. You describe what should happen
when a call fails, and keel does it:

```cpp
#include "keel/keel.hpp"
using namespace keel;

RetryPolicy policy;
policy.max_attempts(5)
      .backoff(Backoff::exponential(Duration{50}).with_jitter().cap(Duration{2000}))
      .retry_if([](const std::exception& e) { return is_transient(e); });

auto rows = retry(policy, [&] { return db.query("SELECT ..."); });
```

A few things we cared about:

- It covers the usual patterns and stops there. You won't find a framework
  here, just functions and small classes you call directly.
- Time is injectable everywhere. Clocks, sleeps and the jitter RNG can all be
  swapped out, so you can unit-test your retry logic without actually waiting.
- It's a handful of short headers. You can read the whole thing in one sitting
  and know exactly what it does.

## Getting it into your project

The simplest way: copy [`include/keel/`](include/keel) into your project and
`#include "keel/keel.hpp"`. That's it.

If you use CMake, you can vendor the repo (a git submodule works well) and add
it as a subdirectory:

```cmake
add_subdirectory(third_party/keel)
target_link_libraries(your_target PRIVATE keel::keel)
```

Or install it once and find it like any other package:

```bash
cmake -B build -DKEEL_BUILD_TESTS=OFF
cmake --build build --target install
```

```cmake
find_package(keel REQUIRED)
target_link_libraries(your_target PRIVATE keel::keel)
```

## What's inside

### retry

`retry(policy, fn)` calls `fn`, and if it throws a `std::exception` your policy
considers retryable, waits and tries again. You get back whatever `fn` returns
(`void` works too). If every attempt fails, the last exception is rethrown so
nothing gets swallowed.

| Setter            | What it does                                           |
|-------------------|--------------------------------------------------------|
| `max_attempts(n)` | Total attempts, counting the first one. Default is 3.  |
| `backoff(b)`      | How long to wait between attempts. Default is exponential from 100ms. |
| `retry_if(pred)`  | Only retry when `pred(exception)` returns true.        |
| `on_retry(cb)`    | Called before each wait with `(attempt, exception, delay)`. Handy for logging. |
| `sleeper(fn)`     | Replaces the actual waiting. Tests pass a no-op.       |
| `random(fn)`      | Replaces the jitter RNG. Tests pass a fixed value.     |

Some APIs don't throw, they return a status code. For those there's
`retry_while`, which keeps going while the result looks like a failure:

```cpp
auto status = retry_while(policy, [&] { return api.poll(); },
                          [](Status s) { return s == Status::Pending; });
```

### Backoff

```cpp
Backoff::fixed(Duration{100});                       // 100ms every time
Backoff::linear(Duration{100});                      // 100, 200, 300, ...
Backoff::exponential(Duration{100}, /*factor=*/2.0); // 100, 200, 400, ...
```

Add `.cap(max)` so exponential growth doesn't run away, and `.with_jitter()`
so a hundred clients that failed at the same moment don't all retry at the same
moment too. `with_jitter(1.0)` picks anywhere between zero and the full delay;
smaller ratios keep part of the delay fixed.

### CircuitBreaker

When a dependency is down, retrying harder only makes things worse. A circuit
breaker notices repeated failures and starts rejecting calls immediately with
`circuit_open_error`, giving the other side room to recover. After a cool-down
it lets a trial call through, and if that works, traffic resumes.

```cpp
CircuitBreaker cb{CircuitConfig{}
                      .with_failure_threshold(5)    // open after 5 failures in a row
                      .with_success_threshold(1)    // close after 1 good trial call
                      .with_reset_timeout(Duration{30000})};

auto value = cb.execute([&] { return call_service(); });
```

It's safe to share one breaker between threads.

### Fallback

Sometimes a stale answer beats no answer. `with_fallback` runs the primary path
and, if it throws, runs the backup instead:

```cpp
auto user = with_fallback([&] { return db.load_user(id); },
                          [&] { return cache.load_user(id); });
```

`with_fallback_if` takes a predicate if you only want to fall back for certain
kinds of errors.

### Timeout

```cpp
auto rows = timeout(Duration{500}, [&] { return db.query("..."); });
```

Throws `timeout_error` if the call takes longer than the limit.

One honest caveat: C++ has no way to forcibly stop a running function, so on
timeout the work keeps going in a background thread until it finishes on its
own. Only use this for work that's safe to abandon, and make sure it doesn't
hold references to things that might be gone by then. If your database driver
or socket has its own timeout setting, prefer that.

### RateLimiter

A token bucket. Tokens refill at a steady rate up to a burst limit, and each
call spends one. Useful when you're the one who could overload somebody else.

```cpp
RateLimiter limiter{/*rate_per_second=*/100.0, /*burst=*/20.0};

if (limiter.try_acquire()) { /* send the request */ }

// or let it throw rate_limited_error when you're over the limit:
auto result = limiter.execute([&] { return call_service(); });
```

Also thread-safe.

## Putting them together

Everything takes a callable and returns its result, so the pieces simply nest.
Here's "retry through a circuit breaker, and serve from cache if all else
fails":

```cpp
auto user = with_fallback(
    [&] {
        return retry(policy, [&] {
            return cb.execute([&] { return db.load_user(id); });
        });
    },
    [&] { return cache.load_user(id); });
```

## Testing code that uses keel

Since nothing in keel insists on real time, your tests don't have to either:

```cpp
std::vector<long long> waited;
RetryPolicy p;
p.max_attempts(3)
 .backoff(Backoff::exponential(Duration{10}))
 .sleeper([&](Duration d) { waited.push_back(d.count()); });  // records, doesn't sleep

// run retry(p, flaky) and check that waited == {10, 20}
```

The circuit breaker and rate limiter accept a clock you can move forward by
hand. [`tests/tests.cpp`](tests/tests.cpp) has plenty of examples.

## Building the tests and the example

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/examples/keel_example
```

You need a C++17 compiler. CI runs on GCC, Clang (Linux and macOS) and MSVC.

## What's next

Things we'd like to add:

- A small builder for the nesting shown above, so the whole setup reads as one
  declaration.
- Decorrelated jitter, the variant AWS recommends.
- Support for coroutines.

If any of that sounds interesting, or you've hit a bug, see
[CONTRIBUTING.md](CONTRIBUTING.md). Issues and pull requests are welcome.

## License

MIT. See [LICENSE](LICENSE).
