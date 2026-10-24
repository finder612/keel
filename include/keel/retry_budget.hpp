// RetryBudget: cap retries as a share of real traffic.
//
// Limiting attempts per call is not enough to protect a struggling service.
// When it starts failing, every client retries at once and the load doubles or
// triples right when the service can least handle it (a "retry storm").
//
// A retry budget looks at the whole stream of calls instead. It counts original
// requests and retries over a sliding window, and only allows a retry while
// retries stay below `retry_ratio` of requests. A small per-second floor lets
// low-traffic callers still retry occasionally.
//
//   keel::RetryBudget budget{keel::RetryBudgetConfig{}
//                                .with_retry_ratio(0.2)        // at most +20% load
//                                .with_min_retries_per_second(5)};
//
//   keel::RetryPolicy policy;
//   policy.max_attempts(3).budget(budget);   // share one budget across calls
//
// The budget is thread-safe and meant to be shared by every call to the same
// dependency. The clock is injectable for deterministic tests.
#ifndef KEEL_RETRY_BUDGET_HPP
#define KEEL_RETRY_BUDGET_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

#include "keel/common.hpp"

namespace keel {

struct RetryBudgetConfig {
    // Retries allowed per original request. 0.2 means retries may add at most
    // 20% on top of normal traffic.
    double retry_ratio = 0.2;
    // Retries per second that are always allowed, however little traffic
    // there is. Without it, a quiet service could never retry at all.
    double min_retries_per_second = 10.0;
    // How far back requests and retries are counted.
    Duration window = Duration{10000};

    RetryBudgetConfig& with_retry_ratio(double ratio) {
        retry_ratio = ratio;
        return *this;
    }
    RetryBudgetConfig& with_min_retries_per_second(double n) {
        min_retries_per_second = n;
        return *this;
    }
    RetryBudgetConfig& with_window(Duration d) {
        window = d;
        return *this;
    }
};

class RetryBudget {
public:
    explicit RetryBudget(RetryBudgetConfig config = {}, ClockFn clock = {})
        : config_(config),
          now_(clock ? std::move(clock) : ClockFn{&default_now}),
          origin_(now_()) {
        const Duration::rep width =
            config_.window.count() / static_cast<Duration::rep>(kBuckets);
        bucket_width_ = Duration{std::max<Duration::rep>(width, 1)};
    }

    RetryBudget(const RetryBudget&) = delete;
    RetryBudget& operator=(const RetryBudget&) = delete;

    // Count one original (non-retry) request.
    void record_request() {
        std::lock_guard<std::mutex> guard(mutex_);
        ++current_bucket().requests;
    }

    // Ask to spend one retry. Returns true and records it if the budget allows,
    // false if retrying now would exceed the budget.
    bool try_spend_retry() {
        std::lock_guard<std::mutex> guard(mutex_);
        Bucket& bucket = current_bucket();
        const Totals totals = sum_window();
        if (static_cast<double>(totals.retries) + 1.0 > allowance(totals.requests)) {
            return false;
        }
        ++bucket.retries;
        return true;
    }

    // Requests counted in the current window.
    std::size_t requests() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return sum_window().requests;
    }

    // Retries counted in the current window.
    std::size_t retries() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return sum_window().retries;
    }

private:
    // The window is split into this many buckets; older buckets drop off as
    // time moves on, so the window slides in steps of window / kBuckets.
    static constexpr std::size_t kBuckets = 10;

    struct Bucket {
        std::int64_t slot = -1;
        std::size_t requests = 0;
        std::size_t retries = 0;
    };

    struct Totals {
        std::size_t requests = 0;
        std::size_t retries = 0;
    };

    std::int64_t current_slot() const {
        const auto elapsed = now_() - origin_;
        const auto slot = static_cast<std::int64_t>(elapsed / bucket_width_);
        return slot < 0 ? 0 : slot;
    }

    // Caller must hold the lock.
    Bucket& current_bucket() {
        const std::int64_t slot = current_slot();
        Bucket& bucket = buckets_[static_cast<std::size_t>(slot) % kBuckets];
        if (bucket.slot != slot) bucket = Bucket{slot, 0, 0};
        return bucket;
    }

    // Caller must hold the lock.
    Totals sum_window() const {
        const std::int64_t slot = current_slot();
        const std::int64_t oldest = slot - static_cast<std::int64_t>(kBuckets);
        Totals totals;
        for (const Bucket& bucket : buckets_) {
            if (bucket.slot > oldest && bucket.slot <= slot) {
                totals.requests += bucket.requests;
                totals.retries += bucket.retries;
            }
        }
        return totals;
    }

    double allowance(std::size_t requests) const {
        const double window_seconds = static_cast<double>(config_.window.count()) / 1000.0;
        const double floor = config_.min_retries_per_second * window_seconds;
        return std::max(floor, config_.retry_ratio * static_cast<double>(requests));
    }

    RetryBudgetConfig config_;
    ClockFn now_;
    TimePoint origin_;
    Duration bucket_width_{1};

    mutable std::mutex mutex_;
    std::array<Bucket, kBuckets> buckets_{};
};

}  // namespace keel

#endif  // KEEL_RETRY_BUDGET_HPP
