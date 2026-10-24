// keel test suite. Everything lives in one translation unit because the
// tiny harness in check.hpp defines main(). All timing is driven by injected
// clocks / sleepers / RNGs, so the suite is fully deterministic and never
// actually sleeps.
#include <chrono>
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
    bool timed_out = false;
    try {
        keel::timeout(Duration{20}, []() -> int {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return 1;
        });
    } catch (const keel::timeout_error&) {
        timed_out = true;
    }
    CHECK(timed_out);
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
