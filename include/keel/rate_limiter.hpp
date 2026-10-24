// RateLimiter — a thread-safe token-bucket limiter.
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
// The clock is injectable for deterministic tests.
#ifndef KEEL_RATE_LIMITER_HPP
#define KEEL_RATE_LIMITER_HPP

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "keel/common.hpp"

namespace keel {

// Thrown by RateLimiter::execute when no token is available.
class rate_limited_error : public std::runtime_error {
public:
    rate_limited_error() : std::runtime_error("keel: rate limit exceeded") {}
};

class RateLimiter {
public:
    RateLimiter(double rate_per_second, double burst, ClockFn clock = {})
        : rate_(rate_per_second),
          capacity_(burst),
          tokens_(burst),
          now_(clock ? std::move(clock) : ClockFn{&default_now}),
          last_(now_()) {}

    // Consume `tokens` if available; returns whether it succeeded.
    bool try_acquire(double tokens = 1.0) {
        std::lock_guard<std::mutex> guard(mutex_);
        refill();
        if (tokens_ >= tokens) {
            tokens_ -= tokens;
            return true;
        }
        return false;
    }

    // Run `fn` if a token is available, otherwise throw rate_limited_error.
    template <class Fn>
    auto execute(Fn&& fn) -> decltype(fn()) {
        if (!try_acquire(1.0)) throw rate_limited_error{};
        return fn();
    }

    // Current number of available tokens (after refilling for elapsed time).
    double available() {
        std::lock_guard<std::mutex> guard(mutex_);
        refill();
        return tokens_;
    }

private:
    // Caller must hold the lock.
    void refill() {
        const TimePoint now = now_();
        const double elapsed = std::chrono::duration<double>(now - last_).count();
        if (elapsed > 0.0) {
            tokens_ = std::min(capacity_, tokens_ + elapsed * rate_);
            last_ = now;
        }
    }

    double rate_;
    double capacity_;
    double tokens_;
    ClockFn now_;
    TimePoint last_;
    std::mutex mutex_;
};

}  // namespace keel

#endif  // KEEL_RATE_LIMITER_HPP
