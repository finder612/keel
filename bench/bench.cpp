// keel micro-benchmarks: the overhead each component adds to a call.
//
// Every benchmark wraps a trivial function, so the numbers are keel's own cost
// per call, not the cost of the work inside. Each one runs several times and
// the median is reported. Build in Release:
//
//   cmake -B build -DCMAKE_BUILD_TYPE=Release -DKEEL_BUILD_BENCHMARKS=ON
//   cmake --build build --config Release
//   ./build/bench/keel_bench
//
// Numbers depend heavily on the machine, compiler and OS; compare runs on the
// same box, not against other people's numbers.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "keel/keel.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Keeps results alive so the optimizer can't delete the work being measured.
volatile int g_sink = 0;

inline int work(int i) { return i ^ 0x5a; }

constexpr int kRepeats = 5;

template <class Body>
double median_ns_per_op(std::size_t iterations, Body body) {
    std::vector<double> runs;
    body(iterations / 10 + 1);  // warm up caches and branch predictors
    for (int r = 0; r < kRepeats; ++r) {
        const auto start = Clock::now();
        body(iterations);
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start);
        runs.push_back(elapsed.count() / static_cast<double>(iterations));
    }
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
}

// Same, but `threads` threads run the body at once; reports wall time per
// operation across all of them (lower is better, contention shows up here).
template <class Body>
double median_ns_per_op_threaded(std::size_t iterations_per_thread, int threads, Body body) {
    std::vector<double> runs;
    for (int r = 0; r < kRepeats; ++r) {
        std::atomic<bool> go{false};
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                body(iterations_per_thread);
            });
        }
        const auto start = Clock::now();
        go.store(true, std::memory_order_release);
        for (auto& t : pool) t.join();
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start);
        runs.push_back(elapsed.count() /
                       static_cast<double>(iterations_per_thread * static_cast<std::size_t>(threads)));
    }
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
}

void report(const char* name, double ns) { std::printf("  %-44s %10.1f ns/op\n", name, ns); }

}  // namespace

int main() {
    constexpr std::size_t kFast = 2'000'000;
    constexpr std::size_t kSlow = 200'000;
    constexpr int kThreads = 8;

    std::printf("keel %d.%d.%d benchmarks (median of %d runs)\n\n", KEEL_VERSION_MAJOR,
                KEEL_VERSION_MINOR, KEEL_VERSION_PATCH, kRepeats);

    std::printf("single thread:\n");

    report("baseline: plain call", median_ns_per_op(kFast, [](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i) g_sink = work(static_cast<int>(i));
           }));

    {
        keel::RetryPolicy policy;
        report("retry (succeeds first time)", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = keel::retry(policy, [&] { return work(static_cast<int>(i)); });
               }));
    }
    {
        keel::RetryBudget budget;
        keel::RetryPolicy policy;
        policy.budget(budget);
        report("retry + RetryBudget", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = keel::retry(policy, [&] { return work(static_cast<int>(i)); });
               }));
    }
    report("with_fallback (primary succeeds)", median_ns_per_op(kFast, [](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i)
                   g_sink = keel::with_fallback([&] { return work(static_cast<int>(i)); },
                                                [] { return 0; });
           }));
    {
        keel::CircuitBreaker cb;
        report("CircuitBreaker::execute (consecutive)", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = cb.execute([&] { return work(static_cast<int>(i)); });
               }));
    }
    {
        keel::CircuitBreaker cb{keel::CircuitConfig{}.with_count_window(100)};
        report("CircuitBreaker::execute (count window)", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = cb.execute([&] { return work(static_cast<int>(i)); });
               }));
    }
    {
        keel::CircuitBreaker cb{keel::CircuitConfig{}
                                    .with_time_window(keel::Duration{60000})
                                    .with_slow_calls(keel::Duration{1000})};
        report("CircuitBreaker::execute (time window+slow)", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = cb.execute([&] { return work(static_cast<int>(i)); });
               }));
    }
    {
        keel::RateLimiter limiter{1e12, 1e12};
        report("RateLimiter::try_acquire", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) g_sink = limiter.try_acquire() ? 1 : 0;
               }));
    }
    {
        keel::Bulkhead bulkhead{keel::BulkheadConfig{}.with_max_concurrent(64)};
        report("Bulkhead::execute", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = bulkhead.execute([&] { return work(static_cast<int>(i)); });
               }));
    }
    {
        keel::AdaptiveLimiter limiter{keel::GradientLimit{}};
        report("AdaptiveLimiter::execute (gradient)", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = limiter.execute([&] { return work(static_cast<int>(i)); });
               }));
    }
    {
        keel::SingleFlight<int, int> flight;
        report("SingleFlight::run (no contention)", median_ns_per_op(kFast, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) {
                       const int key = static_cast<int>(i & 1023);
                       g_sink = flight.run(key, [&] { return work(key); });
                   }
               }));
    }
    {
        keel::Executor inline_executor = [](std::function<void()> job) { job(); };
        report("timeout (inline executor)", median_ns_per_op(kSlow, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = keel::timeout(keel::Duration{1000},
                                              [&] { return work(static_cast<int>(i)); },
                                              inline_executor);
               }));
    }
    report("timeout (thread per call)", median_ns_per_op(kSlow / 20, [](std::size_t n) {
               for (std::size_t i = 0; i < n; ++i)
                   g_sink = keel::timeout(keel::Duration{1000},
                                          [&] { return work(static_cast<int>(i)); });
           }));

    std::printf("\n%d threads sharing one instance:\n", kThreads);
    {
        keel::CircuitBreaker cb;
        report("CircuitBreaker::execute", median_ns_per_op_threaded(kSlow, kThreads, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = cb.execute([&] { return work(static_cast<int>(i)); });
               }));
    }
    {
        keel::Bulkhead bulkhead{keel::BulkheadConfig{}.with_max_concurrent(64)};
        report("Bulkhead::execute", median_ns_per_op_threaded(kSlow, kThreads, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = bulkhead.execute([&] { return work(static_cast<int>(i)); });
               }));
    }
    {
        keel::RateLimiter limiter{1e12, 1e12};
        report("RateLimiter::try_acquire", median_ns_per_op_threaded(kSlow, kThreads, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i) g_sink = limiter.try_acquire() ? 1 : 0;
               }));
    }
    {
        keel::RetryBudget budget;
        keel::RetryPolicy policy;
        policy.budget(budget);
        report("retry + RetryBudget", median_ns_per_op_threaded(kSlow, kThreads, [&](std::size_t n) {
                   for (std::size_t i = 0; i < n; ++i)
                       g_sink = keel::retry(policy, [&] { return work(static_cast<int>(i)); });
               }));
    }
    return 0;
}
