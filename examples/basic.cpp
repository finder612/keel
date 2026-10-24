// A small, runnable tour of keel against a simulated flaky backend.
//
//   cmake -B build && cmake --build build
//   ./build/examples/keel_example
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "keel/keel.hpp"

using keel::Backoff;
using keel::CircuitBreaker;
using keel::CircuitConfig;
using keel::Duration;
using keel::RateLimiter;
using keel::RetryPolicy;

// A stand-in for a database/service call that fails its first `fail_times`
// invocations, then succeeds.
struct FlakyBackend {
    int fail_times;
    int calls = 0;

    std::string query() {
        ++calls;
        if (calls <= fail_times) {
            throw std::runtime_error("connection reset (attempt " +
                                     std::to_string(calls) + ")");
        }
        return "rows[42]";
    }
};

int main() {
    // 1) retry with exponential backoff + jitter, observing each attempt.
    FlakyBackend backend{/*fail_times=*/2};
    RetryPolicy policy;
    policy.max_attempts(5)
        .backoff(Backoff::exponential(Duration{50}).with_jitter().cap(Duration{2000}))
        .on_retry([](std::size_t attempt, const std::exception& e, Duration delay) {
            std::cout << "  retry #" << attempt << " after error: " << e.what()
                      << " (waiting " << delay.count() << "ms)\n";
        });

    std::cout << "retry:\n";
    std::string rows = keel::retry(policy, [&] { return backend.query(); });
    std::cout << "  success -> " << rows << "\n\n";

    // 2) circuit breaker: after 3 straight failures it trips and fails fast.
    std::cout << "circuit breaker:\n";
    CircuitBreaker cb{CircuitConfig{}.with_failure_threshold(3)};
    FlakyBackend dead{/*fail_times=*/1000};
    for (int i = 0; i < 5; ++i) {
        try {
            cb.execute([&] { return dead.query(); });
        } catch (const keel::circuit_open_error&) {
            std::cout << "  call " << (i + 1) << ": short-circuited (breaker open)\n";
        } catch (const std::exception& e) {
            std::cout << "  call " << (i + 1) << ": failed -> " << e.what() << "\n";
        }
    }
    std::cout << "  backend was actually called " << dead.calls << " times\n\n";

    // 3) fallback: serve a cached value when the primary path throws.
    std::cout << "fallback:\n";
    std::string value = keel::with_fallback(
        []() -> std::string { throw std::runtime_error("primary down"); },
        [] { return std::string{"cached value"}; });
    std::cout << "  served -> " << value << "\n\n";

    // 4) timeout: bound a slow call.
    std::cout << "timeout:\n";
    try {
        keel::timeout(Duration{50}, [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return 0;
        });
    } catch (const keel::timeout_error& e) {
        std::cout << "  " << e.what() << "\n\n";
    }

    // 5) rate limiter: 2-token burst, no refill within this loop.
    std::cout << "rate limiter (burst of 2):\n";
    RateLimiter limiter{/*rate_per_second=*/1.0, /*burst=*/2.0};
    for (int i = 1; i <= 4; ++i) {
        const bool ok = limiter.try_acquire();
        std::cout << "  request " << i << ": " << (ok ? "allowed" : "throttled")
                  << "\n";
    }
    std::cout << "\n";

    // 6) single-flight: five threads want the same row at the same time, but
    //    only one of them actually goes to the "database".
    std::cout << "single-flight:\n";
    keel::SingleFlight<std::string, std::string> loads;
    std::atomic<int> db_calls{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 5; ++i) {
        callers.emplace_back([&] {
            loads.run("user:1", [&] {
                ++db_calls;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return std::string{"alice"};
            });
        });
    }
    for (auto& t : callers) t.join();
    std::cout << "  5 callers, database queried " << db_calls << " time(s)\n";

    return 0;
}
