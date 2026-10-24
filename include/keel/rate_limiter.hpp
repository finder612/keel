// RateLimiter: a thread-safe token-bucket limiter.
//
// Tokens refill continuously at `rate_per_second`, up to a maximum of `burst`
// tokens. Each call consumes tokens; when the bucket is empty, calls are
// rejected until it refills. This smooths outbound load on a dependency while
// still allowing short bursts.
//
//   keel::RateLimiter limiter{/*rate_per_second=*/100.0, /*burst=*/20.0};
//   if (limiter.try_acquire()) { /* send request */ }
//   // or:
//   auto result = limiter.execute([&] { return call_service(); });
//
// Calls can cost more than one token (try_acquire(5.0) for a batch of five),
// and time_until_available() tells you how long to wait instead of guessing.
//
// The clock is injectable for deterministic tests.
#ifndef KEEL_RATE_LIMITER_HPP
#define KEEL_RATE_LIMITER_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>

#include "keel/common.hpp"

namespace keel {

// Thrown by RateLimiter::execute when no token is available.
class rate_limited_error : public error {
public:
    explicit rate_limited_error(Duration retry_after = Duration{0})
        : error("keel: rate limit exceeded"), retry_after_(retry_after) {}

    // How long until enough tokens will have refilled. Duration::max() if the
    // request can never succeed (it costs more than the whole bucket).
    Duration retry_after() const noexcept { return retry_after_; }

private:
    Duration retry_after_;
};

class RateLimiter {
public:
    RateLimiter(double rate_per_second, double burst, ClockFn clock = {})
        : rate_(std::max(rate_per_second, 0.0)),
          capacity_(std::max(burst, 0.0)),
          tokens_(capacity_),
          clock_(std::move(clock)),
          last_(clock_.now()) {}

    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    // Consume `tokens` if available; returns whether it succeeded.
    bool try_acquire(double tokens = 1.0) {
        Duration ignored{0};
        return take(tokens, ignored);
    }

    // Run `fn` if `tokens` are available, otherwise throw rate_limited_error.
    template <class Fn>
    auto execute(Fn&& fn, double tokens = 1.0) -> decltype(fn()) {
        Duration wait{0};
        if (!take(tokens, wait)) throw rate_limited_error(wait);
        return fn();
    }

    // How long until `tokens` could be acquired. Zero if they are available
    // now, Duration::max() if they never will be.
    Duration time_until_available(double tokens = 1.0) {
        std::lock_guard<std::mutex> guard(mutex_);
        refill();
        return wait_time(tokens);
    }

    // Current number of available tokens (after refilling for elapsed time).
    double available() {
        std::lock_guard<std::mutex> guard(mutex_);
        refill();
        return tokens_;
    }

private:
    bool take(double tokens, Duration& wait) {
        std::lock_guard<std::mutex> guard(mutex_);
        refill();
        if (tokens <= tokens_) {
            tokens_ -= std::max(tokens, 0.0);
            return true;
        }
        wait = wait_time(tokens);
        return false;
    }

    // Caller must hold the lock.
    void refill() {
        const TimePoint now = clock_.now();
        const double elapsed = std::chrono::duration<double>(now - last_).count();
        if (elapsed > 0.0) {
            tokens_ = std::min(capacity_, tokens_ + elapsed * rate_);
            last_ = now;
        }
    }

    // Caller must hold the lock.
    Duration wait_time(double tokens) const {
        if (tokens <= tokens_) return Duration{0};
        if (tokens > capacity_ || rate_ <= 0.0) return Duration::max();
        const double seconds = (tokens - tokens_) / rate_;
        return Duration{static_cast<Duration::rep>(std::ceil(seconds * 1000.0))};
    }

    double rate_;
    double capacity_;
    double tokens_;
    detail::Clock clock_;
    TimePoint last_;
    std::mutex mutex_;
};

}  // namespace keel

#endif  // KEEL_RATE_LIMITER_HPP
