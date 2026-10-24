// keel test suite. Everything lives in one translation unit because the
// tiny harness in check.hpp defines main(). Timing is driven by injected
// clocks / sleepers / RNGs wherever possible. Only the timeout and SingleFlight
// tests use real threads, and they synchronize explicitly instead of relying
// on lucky timing.
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "keel/keel.hpp"
#include "check.hpp"

using keel::Backoff;
using keel::CircuitBreaker;
using keel::CircuitConfig;
using keel::CircuitState;
using keel::Duration;
using keel::RateLimiter;
using keel::RetryBudget;
using keel::RetryBudgetConfig;
using keel::RetryPolicy;
using keel::TimePoint;

namespace {

// A sleeper that records the delays it is asked to wait, without sleeping.
struct RecordingSleeper {
    std::vector<long long> delays;
    void operator()(Duration d) { delays.push_back(d.count()); }
};

// A fake clock the test can advance by hand.
struct FakeClock {
    TimePoint now{};
    TimePoint operator()() const { return now; }
    void advance(Duration d) { now += d; }
};

// Waits (bounded) until `pred` holds, so concurrent tests don't race.
template <class Pred>
bool wait_until(Pred pred) {
    for (int i = 0; i < 5000; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// True if `fn` throws an E (and nothing else).
template <class E, class Fn>
bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Backoff
// ---------------------------------------------------------------------------

TEST("Backoff: fixed is constant") {
    auto b = Backoff::fixed(Duration{100});
    CHECK(b.delay_for(1).count() == 100);
    CHECK(b.delay_for(5).count() == 100);
}

TEST("Backoff: linear grows by base each attempt") {
    auto b = Backoff::linear(Duration{100});
    CHECK(b.delay_for(1).count() == 100);
    CHECK(b.delay_for(2).count() == 200);
    CHECK(b.delay_for(3).count() == 300);
}

TEST("Backoff: exponential doubles by default") {
    auto b = Backoff::exponential(Duration{100});
    CHECK(b.delay_for(1).count() == 100);
    CHECK(b.delay_for(2).count() == 200);
    CHECK(b.delay_for(3).count() == 400);
    CHECK(b.delay_for(4).count() == 800);
}

TEST("Backoff: cap clamps the delay") {
    auto b = Backoff::exponential(Duration{100}).cap(Duration{250});
    CHECK(b.delay_for(1).count() == 100);
    CHECK(b.delay_for(2).count() == 200);
    CHECK(b.delay_for(3).count() == 250);  // raw 400 -> capped
    CHECK(b.delay_for(9).count() == 250);
}

TEST("Backoff: full jitter picks within [0, delay]") {
    auto b = Backoff::fixed(Duration{100}).with_jitter(1.0);
    CHECK(b.delay_for(1, [] { return 0.0; }).count() == 0);
    CHECK(b.delay_for(1, [] { return 0.5; }).count() == 50);
}

TEST("Backoff: partial jitter keeps a fixed floor") {
    auto b = Backoff::fixed(Duration{100}).with_jitter(0.5);
    // floor = 50, jittered part spans [50, 100].
    CHECK(b.delay_for(1, [] { return 0.0; }).count() == 50);
    CHECK(b.delay_for(1, [] { return 0.5; }).count() == 75);
}

// ---------------------------------------------------------------------------
// retry
// ---------------------------------------------------------------------------

TEST("retry: returns immediately on success, no sleeping") {
    RecordingSleeper sleeper;
    RetryPolicy policy;
    policy.max_attempts(3).sleeper(std::ref(sleeper));

    int calls = 0;
    int value = keel::retry(policy, [&] {
        ++calls;
        return 42;
    });
    CHECK(value == 42);
    CHECK(calls == 1);
    CHECK(sleeper.delays.empty());
}

TEST("retry: retries until success and waits per the backoff") {
    RecordingSleeper sleeper;
    RetryPolicy policy;
    policy.max_attempts(5)
        .backoff(Backoff::exponential(Duration{10}))
        .sleeper(std::ref(sleeper));

    int calls = 0;
    int value = keel::retry(policy, [&] {
        if (++calls < 3) throw std::runtime_error("transient");
        return 7;
    });
    CHECK(value == 7);
    CHECK(calls == 3);
    CHECK(sleeper.delays.size() == 2);
    CHECK(sleeper.delays[0] == 10);  // after attempt 1
    CHECK(sleeper.delays[1] == 20);  // after attempt 2
}

TEST("retry: rethrows the last exception when attempts are exhausted") {
    RecordingSleeper sleeper;
    RetryPolicy policy;
    policy.max_attempts(2).backoff(Backoff::fixed(Duration{5})).sleeper(std::ref(sleeper));

    int calls = 0;
    bool threw = false;
    try {
        keel::retry(policy, [&]() -> int {
            ++calls;
            throw std::runtime_error("always");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(calls == 2);
    CHECK(sleeper.delays.size() == 1);  // only one wait, between the 2 attempts
}

TEST("retry: retry_if can veto a retry") {
    RecordingSleeper sleeper;
    RetryPolicy policy;
    policy.max_attempts(5).sleeper(std::ref(sleeper)).retry_if(
        [](const std::exception& e) {
            return std::string{e.what()} != "fatal";
        });

    int calls = 0;
    bool threw = false;
    try {
        keel::retry(policy, [&]() -> int {
            ++calls;
            throw std::runtime_error("fatal");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(calls == 1);  // not retried
    CHECK(sleeper.delays.empty());
}

TEST("retry: on_retry observes each attempt") {
    RecordingSleeper sleeper;
    std::vector<std::size_t> seen;
    RetryPolicy policy;
    policy.max_attempts(4)
        .backoff(Backoff::fixed(Duration{1}))
        .sleeper(std::ref(sleeper))
        .on_retry([&](std::size_t attempt, const std::exception&, Duration) {
            seen.push_back(attempt);
        });

    int calls = 0;
    keel::retry(policy, [&] {
        if (++calls < 4) throw std::runtime_error("x");
        return 0;
    });
    CHECK(seen.size() == 3);
    CHECK(seen[0] == 1 && seen[1] == 2 && seen[2] == 3);
}

TEST("retry: supports void-returning operations") {
    RetryPolicy policy;
    policy.max_attempts(3).sleeper([](Duration) {});
    int calls = 0;
    keel::retry(policy, [&] {
        if (++calls < 2) throw std::runtime_error("x");
    });
    CHECK(calls == 2);
}

// ---------------------------------------------------------------------------
// CircuitBreaker
// ---------------------------------------------------------------------------

TEST("CircuitBreaker: trips to Open after the failure threshold") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}.with_failure_threshold(3), std::ref(clock)};

    for (int i = 0; i < 3; ++i) {
        try {
            cb.execute([]() -> int { throw std::runtime_error("boom"); });
        } catch (const std::runtime_error&) {
        }
    }
    CHECK(cb.state() == CircuitState::Open);

    // While Open, calls fail fast without invoking the operation.
    bool called = false;
    bool fast_failed = false;
    try {
        cb.execute([&]() -> int {
            called = true;
            return 1;
        });
    } catch (const keel::circuit_open_error&) {
        fast_failed = true;
    }
    CHECK(fast_failed);
    CHECK(!called);
}

TEST("CircuitBreaker: a success resets the failure count while Closed") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}.with_failure_threshold(3), std::ref(clock)};

    auto fail = [&] {
        try {
            cb.execute([]() -> int { throw std::runtime_error("boom"); });
        } catch (const std::runtime_error&) {
        }
    };
    fail();
    fail();
    cb.execute([] { return 1; });  // success resets
    fail();
    fail();
    CHECK(cb.state() == CircuitState::Closed);  // only 2 consecutive again
    CHECK(cb.consecutive_failures() == 2);
}

TEST("CircuitBreaker: Open -> HalfOpen -> Closed after reset timeout") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}
                          .with_failure_threshold(1)
                          .with_success_threshold(1)
                          .with_reset_timeout(Duration{1000}),
                      std::ref(clock)};

    try {
        cb.execute([]() -> int { throw std::runtime_error("boom"); });
    } catch (const std::runtime_error&) {
    }
    CHECK(cb.state() == CircuitState::Open);

    clock.advance(Duration{1000});
    CHECK(cb.state() == CircuitState::HalfOpen);

    int v = cb.execute([] { return 99; });  // trial succeeds -> closes
    CHECK(v == 99);
    CHECK(cb.state() == CircuitState::Closed);
}

TEST("CircuitBreaker: a failure in HalfOpen re-opens the breaker") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}
                          .with_failure_threshold(1)
                          .with_reset_timeout(Duration{1000}),
                      std::ref(clock)};

    try {
        cb.execute([]() -> int { throw std::runtime_error("boom"); });
    } catch (const std::runtime_error&) {
    }
    clock.advance(Duration{1000});
    CHECK(cb.state() == CircuitState::HalfOpen);

    try {
        cb.execute([]() -> int { throw std::runtime_error("still bad"); });
    } catch (const std::runtime_error&) {
    }
    CHECK(cb.state() == CircuitState::Open);
}

// ---------------------------------------------------------------------------
// fallback
// ---------------------------------------------------------------------------

TEST("with_fallback: returns the primary result on success") {
    int v = keel::with_fallback([] { return 1; }, [] { return 2; });
    CHECK(v == 1);
}

TEST("with_fallback: uses the fallback when the primary throws") {
    int v = keel::with_fallback([]() -> int { throw std::runtime_error("x"); },
                              [] { return 2; });
    CHECK(v == 2);
}

TEST("with_fallback_if: only falls back for matching exceptions") {
    auto is_transient = [](const std::exception& e) {
        return std::string{e.what()} == "transient";
    };

    int v = keel::with_fallback_if(
        []() -> int { throw std::runtime_error("transient"); },
        [] { return 5; }, is_transient);
    CHECK(v == 5);

    bool rethrew = false;
    try {
        keel::with_fallback_if([]() -> int { throw std::runtime_error("fatal"); },
                             [] { return 5; }, is_transient);
    } catch (const std::runtime_error&) {
        rethrew = true;
    }
    CHECK(rethrew);
}

// ---------------------------------------------------------------------------
// retry_while (value-based)
// ---------------------------------------------------------------------------

TEST("retry_while: retries until the result is no longer a failure") {
    RecordingSleeper sleeper;
    RetryPolicy policy;
    policy.max_attempts(5)
        .backoff(Backoff::exponential(Duration{10}))
        .sleeper(std::ref(sleeper));

    int calls = 0;
    int value = keel::retry_while(
        policy, [&] { return ++calls; }, [](int v) { return v < 3; });
    CHECK(value == 3);
    CHECK(calls == 3);
    CHECK(sleeper.delays.size() == 2);
    CHECK(sleeper.delays[0] == 10 && sleeper.delays[1] == 20);
}

TEST("retry_while: returns the last result even if still failing") {
    RetryPolicy policy;
    policy.max_attempts(2).sleeper([](Duration) {});
    int calls = 0;
    int value = keel::retry_while(
        policy, [&] { return ++calls; }, [](int) { return true; });
    CHECK(value == 2);  // exhausted; last value returned
    CHECK(calls == 2);
}

// ---------------------------------------------------------------------------
// timeout
// ---------------------------------------------------------------------------

TEST("timeout: returns the result when the operation finishes in time") {
    int v = keel::timeout(Duration{1000}, [] { return 7; });
    CHECK(v == 7);
}

TEST("timeout: throws timeout_error when the operation is too slow") {
    // Shared with the background work, which outlives this call on timeout.
    auto finished = std::make_shared<std::atomic<bool>>(false);
    bool timed_out = false;
    try {
        keel::timeout(Duration{20}, [finished]() -> int {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            finished->store(true);
            return 1;
        });
    } catch (const keel::timeout_error& e) {
        timed_out = true;
        CHECK(e.limit().count() == 20);
    }
    CHECK(timed_out);
    // The abandoned work still runs to completion in the background. Wait for
    // it so the process does not exit with work in flight (keeps leak checks
    // honest).
    CHECK(wait_until([&] { return finished->load(); }));
}

TEST("timeout: propagates the operation's own exception") {
    bool threw = false;
    try {
        keel::timeout(Duration{1000}, []() -> int {
            throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// RateLimiter
// ---------------------------------------------------------------------------

TEST("RateLimiter: allows a burst, then rejects until refilled") {
    FakeClock clock;
    RateLimiter limiter{/*rate_per_second=*/1.0, /*burst=*/2.0, std::ref(clock)};

    CHECK(limiter.try_acquire());   // 2 -> 1
    CHECK(limiter.try_acquire());   // 1 -> 0
    CHECK(!limiter.try_acquire());  // empty

    clock.advance(Duration{1000});  // +1 token
    CHECK(limiter.try_acquire());
    CHECK(!limiter.try_acquire());
}

TEST("RateLimiter: refill is capped at the burst size") {
    FakeClock clock;
    RateLimiter limiter{/*rate_per_second=*/10.0, /*burst=*/3.0, std::ref(clock)};
    CHECK(limiter.try_acquire(3.0));  // drain
    CHECK(!limiter.try_acquire());

    clock.advance(Duration{10000});  // would add 100 tokens, capped at 3
    CHECK(limiter.available() == 3.0);
}

TEST("RateLimiter: execute runs or throws rate_limited_error") {
    FakeClock clock;
    RateLimiter limiter{1.0, 1.0, std::ref(clock)};
    CHECK(limiter.execute([] { return 42; }) == 42);

    bool rejected = false;
    try {
        limiter.execute([] { return 1; });
    } catch (const keel::rate_limited_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

// ---------------------------------------------------------------------------
// RetryBudget
// ---------------------------------------------------------------------------

TEST("RetryBudget: allows retries up to the ratio of requests") {
    FakeClock clock;
    RetryBudget budget{RetryBudgetConfig{}
                           .with_retry_ratio(0.5)
                           .with_min_retries_per_second(0.0),
                       std::ref(clock)};

    for (int i = 0; i < 4; ++i) budget.record_request();
    CHECK(budget.try_spend_retry());   // 1 of 2
    CHECK(budget.try_spend_retry());   // 2 of 2
    CHECK(!budget.try_spend_retry());  // over budget
    CHECK(budget.requests() == 4);
    CHECK(budget.retries() == 2);
}

TEST("RetryBudget: the per-second floor allows retries with no traffic") {
    FakeClock clock;
    // 0.2 retries/s over a 10s window = 2 retries, whatever the traffic.
    RetryBudget budget{RetryBudgetConfig{}
                           .with_retry_ratio(0.0)
                           .with_min_retries_per_second(0.2)
                           .with_window(Duration{10000}),
                       std::ref(clock)};

    CHECK(budget.try_spend_retry());
    CHECK(budget.try_spend_retry());
    CHECK(!budget.try_spend_retry());
}

TEST("RetryBudget: old activity slides out of the window") {
    FakeClock clock;
    RetryBudget budget{RetryBudgetConfig{}
                           .with_retry_ratio(0.0)
                           .with_min_retries_per_second(0.1)  // 1 per 10s
                           .with_window(Duration{10000}),
                       std::ref(clock)};

    CHECK(budget.try_spend_retry());
    CHECK(!budget.try_spend_retry());

    clock.advance(Duration{11000});
    CHECK(budget.retries() == 0);
    CHECK(budget.try_spend_retry());
}

TEST("retry: stops early when the budget is exhausted") {
    FakeClock clock;
    RetryBudget budget{RetryBudgetConfig{}
                           .with_retry_ratio(0.0)
                           .with_min_retries_per_second(0.0),
                       std::ref(clock)};
    RecordingSleeper sleeper;
    RetryPolicy policy;
    policy.max_attempts(5).sleeper(std::ref(sleeper)).budget(budget);

    int calls = 0;
    bool threw = false;
    try {
        keel::retry(policy, [&]() -> int {
            ++calls;
            throw std::runtime_error("down");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(calls == 1);  // no retries allowed at all
    CHECK(sleeper.delays.empty());
    CHECK(budget.requests() == 1);
}

TEST("retry: a generous budget does not get in the way") {
    FakeClock clock;
    RetryBudget budget{RetryBudgetConfig{}, std::ref(clock)};  // floor of 100 retries
    RetryPolicy policy;
    policy.max_attempts(3).sleeper([](Duration) {}).budget(budget);

    int calls = 0;
    int v = keel::retry(policy, [&] {
        if (++calls < 3) throw std::runtime_error("x");
        return 5;
    });
    CHECK(v == 5);
    CHECK(budget.retries() == 2);
}

TEST("retry_while: returns the current result when the budget says no") {
    FakeClock clock;
    RetryBudget budget{RetryBudgetConfig{}
                           .with_retry_ratio(0.0)
                           .with_min_retries_per_second(0.0),
                       std::ref(clock)};
    RetryPolicy policy;
    policy.max_attempts(5).sleeper([](Duration) {}).budget(budget);

    int calls = 0;
    int v = keel::retry_while(
        policy, [&] { return ++calls; }, [](int) { return true; });
    CHECK(v == 1);
    CHECK(calls == 1);
}

// ---------------------------------------------------------------------------
// SingleFlight
// ---------------------------------------------------------------------------

TEST("SingleFlight: sequential calls each run (nothing is cached)") {
    keel::SingleFlight<std::string, int> flight;
    int calls = 0;
    CHECK(flight.run("k", [&] { return ++calls; }) == 1);
    CHECK(flight.run("k", [&] { return ++calls; }) == 2);
    CHECK(flight.in_flight() == 0);
}

TEST("SingleFlight: concurrent callers for one key share a single call") {
    keel::SingleFlight<std::string, int> flight;
    std::atomic<int> calls{0};

    std::promise<void> started;
    std::future<void> started_future = started.get_future();
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    int leader_result = 0;
    int follower_result = 0;

    std::thread leader([&] {
        leader_result = flight.run("user:1", [&] {
            ++calls;
            started.set_value();
            released.wait();
            return 42;
        });
    });
    started_future.wait();

    std::thread follower([&] {
        follower_result = flight.run("user:1", [&] {
            ++calls;
            return 7;
        });
    });

    const bool joined = wait_until([&] { return flight.waiters("user:1") == 1; });
    release.set_value();
    leader.join();
    follower.join();

    CHECK(joined);
    CHECK(calls == 1);
    CHECK(leader_result == 42);
    CHECK(follower_result == 42);
    CHECK(flight.in_flight() == 0);
}

TEST("SingleFlight: waiters receive the leader's exception") {
    keel::SingleFlight<int, int> flight;

    std::promise<void> started;
    std::future<void> started_future = started.get_future();
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    bool leader_threw = false;
    bool follower_threw = false;

    std::thread leader([&] {
        try {
            flight.run(1, [&]() -> int {
                started.set_value();
                released.wait();
                throw std::runtime_error("db down");
            });
        } catch (const std::runtime_error&) {
            leader_threw = true;
        }
    });
    started_future.wait();

    std::thread follower([&] {
        try {
            flight.run(1, [] { return 0; });
        } catch (const std::runtime_error&) {
            follower_threw = true;
        }
    });

    const bool joined = wait_until([&] { return flight.waiters(1) == 1; });
    release.set_value();
    leader.join();
    follower.join();

    CHECK(joined);
    CHECK(leader_threw);
    CHECK(follower_threw);
}

TEST("SingleFlight: different keys do not wait for each other") {
    keel::SingleFlight<std::string, int> flight;

    std::promise<void> started;
    std::future<void> started_future = started.get_future();
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    std::thread slow([&] {
        flight.run("a", [&] {
            started.set_value();
            released.wait();
            return 1;
        });
    });
    started_future.wait();

    // "b" runs right away even though "a" is still in flight.
    int b = flight.run("b", [] { return 2; });
    CHECK(b == 2);
    CHECK(flight.in_flight() == 1);

    release.set_value();
    slow.join();
    CHECK(flight.in_flight() == 0);
}

// ---------------------------------------------------------------------------
// keel::error hierarchy
// ---------------------------------------------------------------------------

TEST("errors: every keel rejection derives from keel::error") {
    CHECK(throws<keel::error>([] { throw keel::circuit_open_error{}; }));
    CHECK(throws<keel::error>([] { throw keel::rate_limited_error{}; }));
    CHECK(throws<keel::error>([] { throw keel::timeout_error{}; }));
    // ...and still from std::runtime_error, as in 1.2.
    CHECK(throws<std::runtime_error>([] { throw keel::circuit_open_error{}; }));
}

// ---------------------------------------------------------------------------
// Backoff::custom
// ---------------------------------------------------------------------------

TEST("Backoff: custom schedules still honor cap") {
    auto b = Backoff::custom([](std::size_t n) {
                 return Duration{static_cast<Duration::rep>(n * n) * 10};
             }).cap(Duration{50});
    CHECK(b.delay_for(1).count() == 10);
    CHECK(b.delay_for(2).count() == 40);
    CHECK(b.delay_for(3).count() == 50);  // raw 90, capped
}

// ---------------------------------------------------------------------------
// RetryPolicy: retry_on and max_elapsed
// ---------------------------------------------------------------------------

namespace {
struct transient_error : std::runtime_error {
    transient_error() : std::runtime_error("transient") {}
};
struct connection_lost : transient_error {};
}  // namespace

TEST("retry_on: retries listed types and their subclasses only") {
    RetryPolicy policy;
    policy.max_attempts(3).sleeper([](Duration) {}).retry_on<transient_error>();

    int calls = 0;
    int v = keel::retry(policy, [&] {
        if (++calls < 3) throw connection_lost{};  // subclass of transient_error
        return 1;
    });
    CHECK(v == 1);
    CHECK(calls == 3);

    calls = 0;
    CHECK(throws<std::logic_error>([&] {
        keel::retry(policy, [&]() -> int {
            ++calls;
            throw std::logic_error("bug");
        });
    }));
    CHECK(calls == 1);  // not retried
}

TEST("retry_on: accepts several types") {
    RetryPolicy policy;
    policy.max_attempts(3).sleeper([](Duration) {}).retry_on<std::logic_error, transient_error>();
    int calls = 0;
    keel::retry(policy, [&] {
        ++calls;
        if (calls == 1) throw std::logic_error("a");
        if (calls == 2) throw transient_error{};
        return 0;
    });
    CHECK(calls == 3);
}

TEST("max_elapsed: gives up before a wait would cross the time budget") {
    FakeClock clock;
    RetryPolicy policy;
    policy.max_attempts(10)
        .backoff(Backoff::exponential(Duration{10}))  // 10, 20, 40, ...
        .max_elapsed(Duration{25})
        .clock(std::ref(clock))
        .sleeper([&](Duration d) { clock.advance(d); });

    int calls = 0;
    CHECK(throws<std::runtime_error>([&] {
        keel::retry(policy, [&]() -> int {
            ++calls;
            throw std::runtime_error("x");
        });
    }));
    // Attempt 1 fails at t=0, wait 10 (ends t=10, fits). Attempt 2 fails at
    // t=10; waiting 20 would end at t=30 > 25, so it stops there.
    CHECK(calls == 2);
}

// ---------------------------------------------------------------------------
// CircuitBreaker: new controls
// ---------------------------------------------------------------------------

TEST("CircuitBreaker: failure_if lets business errors pass without tripping") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}
                          .with_failure_threshold(2)
                          .with_failure_if([](const std::exception& e) {
                              return dynamic_cast<const std::logic_error*>(&e) == nullptr;
                          }),
                      std::ref(clock)};

    for (int i = 0; i < 5; ++i) {
        CHECK(throws<std::logic_error>(
            [&] { cb.execute([]() -> int { throw std::logic_error("not found"); }); }));
    }
    CHECK(cb.state() == CircuitState::Closed);
    CHECK(cb.consecutive_failures() == 0);

    for (int i = 0; i < 2; ++i) {
        CHECK(throws<std::runtime_error>(
            [&] { cb.execute([]() -> int { throw std::runtime_error("down"); }); }));
    }
    CHECK(cb.state() == CircuitState::Open);
}

TEST("CircuitBreaker: HalfOpen admits only max_half_open_calls at once") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}
                          .with_failure_threshold(1)
                          .with_reset_timeout(Duration{100})
                          .with_max_half_open_calls(1),
                      std::ref(clock)};
    cb.force_open();
    clock.advance(Duration{100});

    bool inner_rejected = false;
    Duration inner_retry_after{-1};
    int v = cb.execute([&] {
        // A second call while the single trial slot is taken is turned away.
        try {
            cb.execute([] { return 0; });
        } catch (const keel::circuit_open_error& e) {
            inner_rejected = true;
            inner_retry_after = e.retry_after();
        }
        return 7;
    });
    CHECK(v == 7);
    CHECK(inner_rejected);
    CHECK(inner_retry_after.count() == 0);
    CHECK(cb.state() == CircuitState::Closed);  // the trial succeeded
}

TEST("CircuitBreaker: outcomes from an earlier state are ignored") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}.with_failure_threshold(5), std::ref(clock)};
    // The call starts while Closed; the breaker is opened while it runs.
    cb.execute([&] {
        cb.force_open();
        return 1;
    });
    // Its late success must not count toward anything in the new state.
    CHECK(cb.state() == CircuitState::Open);
}

TEST("CircuitBreaker: on_state_change reports every transition") {
    FakeClock clock;
    std::vector<std::pair<CircuitState, CircuitState>> seen;
    CircuitBreaker cb{CircuitConfig{}
                          .with_failure_threshold(1)
                          .with_reset_timeout(Duration{100})
                          .with_on_state_change([&](CircuitState from, CircuitState to) {
                              seen.emplace_back(from, to);
                          }),
                      std::ref(clock)};

    CHECK(throws<std::runtime_error>(
        [&] { cb.execute([]() -> int { throw std::runtime_error("x"); }); }));
    clock.advance(Duration{100});
    cb.execute([] { return 0; });

    CHECK(seen.size() == 3);
    CHECK(seen.size() == 3 &&
          seen[0] == std::make_pair(CircuitState::Closed, CircuitState::Open) &&
          seen[1] == std::make_pair(CircuitState::Open, CircuitState::HalfOpen) &&
          seen[2] == std::make_pair(CircuitState::HalfOpen, CircuitState::Closed));
    CHECK(std::string{keel::to_string(CircuitState::HalfOpen)} == "half-open");
}

TEST("CircuitBreaker: circuit_open_error tells you when to come back") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}.with_reset_timeout(Duration{1000}), std::ref(clock)};
    cb.force_open();
    clock.advance(Duration{400});

    Duration retry_after{0};
    try {
        cb.execute([] { return 0; });
    } catch (const keel::circuit_open_error& e) {
        retry_after = e.retry_after();
    }
    CHECK(retry_after.count() == 600);
    CHECK(cb.retry_after().count() == 600);
}

TEST("CircuitBreaker: reset and force_open") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}, std::ref(clock)};
    cb.force_open();
    CHECK(cb.state() == CircuitState::Open);
    cb.reset();
    CHECK(cb.state() == CircuitState::Closed);
    CHECK(cb.execute([] { return 3; }) == 3);
}

TEST("CircuitBreaker: Permit reports outcomes for callback-style code") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}.with_failure_threshold(2), std::ref(clock)};

    for (int i = 0; i < 2; ++i) {
        auto permit = cb.try_acquire();
        CHECK(permit.has_value());
        if (permit) permit->failure();
    }
    CHECK(cb.state() == CircuitState::Open);
    CHECK(!cb.try_acquire().has_value());  // refused while open
}

TEST("CircuitBreaker: a dropped Permit frees its HalfOpen slot") {
    FakeClock clock;
    CircuitBreaker cb{CircuitConfig{}
                          .with_reset_timeout(Duration{100})
                          .with_max_half_open_calls(1),
                      std::ref(clock)};
    cb.force_open();
    clock.advance(Duration{100});

    {
        auto permit = cb.try_acquire();
        CHECK(permit.has_value());
        CHECK(!cb.try_acquire().has_value());  // slot taken
    }  // dropped without a report
    CHECK(cb.state() == CircuitState::HalfOpen);
    CHECK(cb.try_acquire().has_value());  // slot is free again
}

// ---------------------------------------------------------------------------
// with_fallback receiving the exception
// ---------------------------------------------------------------------------

TEST("with_fallback: the fallback can inspect the exception") {
    std::string v = keel::with_fallback(
        []() -> std::string { throw std::runtime_error("db down"); },
        [](const std::exception& e) { return std::string{"fallback: "} + e.what(); });
    CHECK(v == "fallback: db down");
}

// ---------------------------------------------------------------------------
// timeout: cancellation and executors
// ---------------------------------------------------------------------------

TEST("timeout: cancels the token when the deadline passes") {
    auto stopped_early = std::make_shared<std::atomic<bool>>(false);
    CHECK(throws<keel::timeout_error>([&] {
        keel::timeout(Duration{20}, [stopped_early](keel::CancelToken token) {
            for (int i = 0; i < 5000; ++i) {
                if (token.cancelled()) {
                    stopped_early->store(true);
                    return i;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return -1;
        });
    }));
    CHECK(wait_until([&] { return stopped_early->load(); }));
}

TEST("timeout: runs work on a custom executor") {
    std::atomic<int> submitted{0};
    keel::Executor executor = [&](std::function<void()> work) {
        ++submitted;
        std::thread(std::move(work)).detach();
    };
    int v = keel::timeout(Duration{1000}, [] { return 9; }, executor);
    CHECK(v == 9);
    CHECK(submitted == 1);
}

// ---------------------------------------------------------------------------
// RateLimiter: wait times
// ---------------------------------------------------------------------------

TEST("RateLimiter: reports how long until tokens are available") {
    FakeClock clock;
    RateLimiter limiter{/*rate_per_second=*/2.0, /*burst=*/1.0, std::ref(clock)};
    CHECK(limiter.time_until_available() == Duration{0});
    CHECK(limiter.try_acquire());
    CHECK(limiter.time_until_available().count() == 500);
    CHECK(limiter.time_until_available(5.0) == Duration::max());  // bigger than the bucket

    Duration retry_after{0};
    try {
        limiter.execute([] { return 0; });
    } catch (const keel::rate_limited_error& e) {
        retry_after = e.retry_after();
    }
    CHECK(retry_after.count() == 500);
}

TEST("RateLimiter: execute can charge several tokens") {
    FakeClock clock;
    RateLimiter limiter{1.0, 3.0, std::ref(clock)};
    CHECK(limiter.execute([] { return 1; }, 3.0) == 1);
    CHECK(throws<keel::rate_limited_error>([&] { limiter.execute([] { return 1; }); }));
}

// ---------------------------------------------------------------------------
// SingleFlight::forget
// ---------------------------------------------------------------------------

TEST("SingleFlight: forget lets the next caller start a fresh call") {
    keel::SingleFlight<std::string, int> flight;
    std::atomic<int> calls{0};

    std::promise<void> started1;
    std::promise<void> started2;
    std::promise<void> release1;
    std::promise<void> release2;
    std::future<void> started1_f = started1.get_future();
    std::future<void> started2_f = started2.get_future();
    std::shared_future<void> released1 = release1.get_future().share();
    std::shared_future<void> released2 = release2.get_future().share();

    int first = 0;
    int second = 0;
    std::thread a([&] {
        first = flight.run("k", [&] {
            ++calls;
            started1.set_value();
            released1.wait();
            return 1;
        });
    });
    started1_f.wait();
    flight.forget("k");  // the running result is now considered stale

    std::thread b([&] {
        second = flight.run("k", [&] {
            ++calls;
            started2.set_value();
            released2.wait();
            return 2;
        });
    });
    started2_f.wait();

    release1.set_value();
    a.join();
    // The old call finishing must not remove the new call's entry.
    const std::size_t in_flight_after_old = flight.in_flight();

    release2.set_value();
    b.join();
    CHECK(in_flight_after_old == 1);
    CHECK(calls == 2);
    CHECK(first == 1);
    CHECK(second == 2);
    CHECK(flight.in_flight() == 0);
}
