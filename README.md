# keel

A small header-only C++17 library for making backend calls survive bad days:
retries with backoff, retry budgets, a circuit breaker, fallbacks, timeouts, a
rate limiter and request coalescing. No dependencies beyond the standard
library.

Current version: **1.3.0**

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

| Setter              | What it does                                           |
|---------------------|--------------------------------------------------------|
| `max_attempts(n)`   | Total attempts, counting the first one. Default is 3.  |
| `backoff(b)`        | How long to wait between attempts. Default is exponential from 100ms. |
| `retry_on<E...>()`  | Only retry these exception types (and their subclasses). |
| `retry_if(pred)`    | Only retry when `pred(exception)` returns true. For anything `retry_on` can't express. |
| `max_elapsed(d)`    | Total time budget for the whole thing, waits included. No retry starts if its wait would go over. |
| `budget(b)`         | Share a `RetryBudget` (see below).                     |
| `on_retry(cb)`      | Called before each wait with `(attempt, exception, delay)`. Handy for logging. |
| `sleeper(fn)`       | Replaces the actual waiting. Tests pass a no-op.       |
| `random(fn)`        | Replaces the jitter RNG. Tests pass a fixed value.     |
| `clock(fn)`         | Replaces the clock used by `max_elapsed`.              |

Usually you know exactly which errors are worth another try, so say so:

```cpp
policy.retry_on<db::connection_lost, db::deadlock>()
      .max_elapsed(Duration{2000});  // and never spend more than 2s overall
```

Some APIs don't throw, they return a status code. For those there's
`retry_while`, which keeps going while the result looks like a failure:

```cpp
auto status = retry_while(policy, [&] { return api.poll(); },
                          [](Status s) { return s == Status::Pending; });
```

### Retry budget

Here's the uncomfortable truth about retries: "at most 3 attempts" protects a
single call, not the service you're calling. When that service starts to
struggle, every client hits its limit at the same time, and suddenly it's
getting three times the traffic right when it can least take it. That's a
retry storm, and it turns a small hiccup into an outage.

A `RetryBudget` fixes this by looking at all calls together. It counts normal
requests and retries over a sliding window and only approves a retry while
retries stay under a share of real traffic:

```cpp
// Shared by every call to the same dependency.
RetryBudget budget{RetryBudgetConfig{}
                       .with_retry_ratio(0.2)           // retries add at most 20%
                       .with_min_retries_per_second(5)  // but always allow a few
                       .with_window(Duration{10000})};

RetryPolicy policy;
policy.max_attempts(3).budget(budget);
```

When the budget runs out, `retry` gives up and rethrows the error instead of
piling on, and `retry_while` returns the result it has. The per-second floor is
there so that a quiet service with little traffic can still retry now and then.
The policy only keeps a reference to the budget, so the budget has to outlive
the calls that use it.

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

If none of those fit, describe your own schedule. `cap` and `with_jitter`
still work on top of it:

```cpp
// Fast first retry, then settle at one second.
auto b = Backoff::custom([](std::size_t attempt) {
    return attempt == 1 ? Duration{50} : Duration{1000};
}).with_jitter(0.2);
```

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

It's safe to share one breaker between threads. A few more knobs:

- `with_failure_if(pred)` decides what counts as a failure. A "user not found"
  isn't an outage, so you probably don't want it to trip the breaker. Errors
  the predicate rejects pass straight through and leave the breaker alone.
- `with_max_half_open_calls(n)` limits how many trial calls can run at once
  while the breaker is testing the water (1 by default). Without this, the
  moment the cool-down ends, all your waiting traffic would rush the service
  that just came back.
- `with_on_state_change(cb)` is called on every transition, which is the
  natural place for a log line or a metric. `to_string(state)` gives you a
  readable name.
- `reset()` closes the breaker by hand, `force_open()` opens it (say, for
  planned maintenance).

When the breaker says no, `circuit_open_error::retry_after()` tells you
roughly how long until it will let calls through again, which is handy for a
`Retry-After` header.

If your call doesn't fit in a lambda (a callback, an async completion), take a
permit and report back later:

```cpp
if (auto permit = cb.try_acquire()) {
    // Permits are move-only; a shared_ptr lets them ride in a copyable callback.
    auto p = std::make_shared<CircuitBreaker::Permit>(std::move(*permit));
    client.async_get(url, [p](Response r) {
        if (r.ok()) p->success(); else p->failure();
    });
} else {
    reply_service_unavailable();
}
```

A permit that's dropped without a report simply frees its slot, so a forgotten
callback can't leave the breaker stuck.

### Fallback

Sometimes a stale answer beats no answer. `with_fallback` runs the primary path
and, if it throws, runs the backup instead:

```cpp
auto user = with_fallback([&] { return db.load_user(id); },
                          [&] { return cache.load_user(id); });
```

If the fallback takes a `const std::exception&`, it gets the error that
triggered it, so it can log it or choose an answer based on what went wrong:

```cpp
auto price = with_fallback([&] { return pricing.quote(item); },
                           [&](const std::exception& e) {
                               log.warn("pricing down: {}", e.what());
                               return item.list_price;
                           });
```

`with_fallback_if` takes a predicate if you only want to fall back for certain
kinds of errors.

### Timeout

```cpp
auto rows = timeout(Duration{500}, [&] { return db.query("..."); });
```

Throws `timeout_error` if the call takes longer than the limit.

One honest caveat: C++ has no way to forcibly stop a running function, so on
timeout the work keeps going in the background until it finishes on its own.
Two things help with that.

If your function takes a `CancelToken`, keel cancels it the moment the
deadline passes, so longer jobs can check it and stop early:

```cpp
auto report = timeout(Duration{500}, [&](CancelToken token) {
    Report r;
    for (auto& shard : shards) {
        if (token.cancelled()) break;  // give up gracefully
        r.add(scan(shard));
    }
    return r;
});
```

And by default every call gets its own thread. If you already have a thread
pool, hand it over so keel doesn't create threads behind your back:

```cpp
Executor on_pool = [&](std::function<void()> work) { pool.post(std::move(work)); };
auto rows = timeout(Duration{500}, [&] { return db.query("..."); }, on_pool);
```

Either way, work that outlives its deadline mustn't touch objects that might
be gone by then. If your database driver or socket has its own timeout setting,
prefer that.

### RateLimiter

A token bucket. Tokens refill at a steady rate up to a burst limit, and each
call spends one. Useful when you're the one who could overload somebody else.

```cpp
RateLimiter limiter{/*rate_per_second=*/100.0, /*burst=*/20.0};

if (limiter.try_acquire()) { /* send the request */ }

// or let it throw rate_limited_error when you're over the limit:
auto result = limiter.execute([&] { return call_service(); });
```

Not every call costs the same, so both `try_acquire` and `execute` take an
optional token count (`limiter.try_acquire(10.0)` for a batch of ten). And
instead of guessing how long to back off, ask: `time_until_available()` returns
the wait, and `rate_limited_error::retry_after()` carries the same number.

Also thread-safe.

### SingleFlight

A popular cache entry expires, and a thousand requests that all need it go to
the database at the same moment, every one of them asking for the exact same
row. That's a cache stampede, and it can knock a database over all by itself.

`SingleFlight` lets the first caller for a key do the work. Anyone else who asks
for the same key while that call is still running simply waits and gets the
same answer (or the same exception):

```cpp
SingleFlight<std::string, User> loads;

User load_user(const std::string& id) {
    if (auto cached = cache.get(id)) return *cached;
    return loads.run(id, [&] {
        User user = db.load_user(id);
        cache.put(id, user);
        return user;
    });
}
```

It doesn't cache anything itself. As soon as a call finishes, the next caller
for that key starts a fresh one, so it works alongside your cache rather than
replacing it. `in_flight()` and `waiters(key)` tell you how much work it's
saving, if you want that on a dashboard.

If you just wrote new data and the call already in flight would return the old
version, `forget(key)` detaches it: whoever is already waiting still gets its
result, but the next caller starts fresh.

Two things to keep in mind: the value type has to be copyable, since every
waiter gets its own copy, and a function must not call `run` for the same key
it's computing, or it will end up waiting for itself.

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

## Telling keel's errors apart from yours

Everything keel throws on its own, `circuit_open_error`, `rate_limited_error`
and `timeout_error`, derives from `keel::error` (which is a
`std::runtime_error`). So "keel refused or gave up" can be handled in one
place, separately from your actual failures:

```cpp
try {
    return handle(request);
} catch (const keel::circuit_open_error& e) {
    return Response::unavailable(e.retry_after());
} catch (const keel::error& e) {
    return Response::busy();
}
```

## Making it fit your code

keel tries hard not to make decisions for you. Here's every place you can plug
in your own behavior:

| You want to change...           | Use                                                         |
|---------------------------------|-------------------------------------------------------------|
| which errors get retried        | `retry_on<E...>()` or `retry_if(pred)`                      |
| the shape of the waits          | `Backoff::custom(fn)`, plus `cap` / `with_jitter`           |
| how waiting happens             | `sleeper(fn)`, e.g. to yield to your event loop             |
| where time comes from           | `clock(fn)` on the policy, a `ClockFn` for breakers, limiters and budgets |
| the randomness in jitter        | `random(fn)`                                                |
| what counts as an outage        | `CircuitConfig::with_failure_if(pred)`                      |
| logging and metrics             | `on_retry(cb)`, `with_on_state_change(cb)`, `waiters(key)`, `budget.retries()` |
| where timed work runs           | an `Executor` for `timeout`                                 |
| how keys are hashed and compared| `SingleFlight<Key, Value, Hash, KeyEqual>`                  |
| breaker control from outside    | `try_acquire()` permits, `reset()`, `force_open()`          |

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

You need a C++17 compiler. CI runs on GCC, Clang (Linux and macOS) and MSVC,
plus two sanitizer builds on every change: AddressSanitizer with leak and
undefined-behavior checks, and ThreadSanitizer for data races.

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
