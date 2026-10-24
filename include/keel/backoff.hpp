// Backoff policies: how long to wait between retry attempts.
//
// A Backoff is a small value type you build with a named constructor and tune
// with chained modifiers:
//
//   auto b = keel::Backoff::exponential(keel::Duration{100})  // 100ms base, x2
//                .cap(keel::Duration{5000})                 // never above 5s
//                .with_jitter();                          // full jitter
//
//   b.delay_for(1);  // delay after the 1st failed attempt
//   b.delay_for(2);  // ... the 2nd, and so on
#ifndef KEEL_BACKOFF_HPP
#define KEEL_BACKOFF_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "keel/common.hpp"

namespace keel {

class Backoff {
public:
    // A constant delay between every attempt.
    static Backoff fixed(Duration base) {
        return Backoff(Kind::Fixed, to_ms(base), 1.0);
    }

    // A delay that grows linearly: base, 2*base, 3*base, ...
    static Backoff linear(Duration base) {
        return Backoff(Kind::Linear, to_ms(base), 1.0);
    }

    // A delay that grows geometrically: base, base*factor, base*factor^2, ...
    static Backoff exponential(Duration base, double factor = 2.0) {
        return Backoff(Kind::Exponential, to_ms(base), factor);
    }

    // Your own schedule: `fn(attempt)` returns the delay after the attempt-th
    // failure (1-based). cap() and with_jitter() still apply on top, so you
    // only describe the shape.
    //
    //   // Fibonacci-ish: 100, 100, 200, 300, 500, ...
    //   auto b = Backoff::custom([](std::size_t n) { return Duration{100 * fib(n)}; });
    static Backoff custom(std::function<Duration(std::size_t attempt)> fn) {
        Backoff b(Kind::Custom, 0.0, 1.0);
        b.custom_ = std::move(fn);
        return b;
    }

    // Clamp every computed delay to at most `max_delay`. Strongly recommended
    // with exponential backoff to avoid unbounded waits (and overflow).
    Backoff& cap(Duration max_delay) {
        cap_ms_ = to_ms(max_delay);
        return *this;
    }

    // Randomize the delay to avoid a thundering herd. `ratio` in [0, 1] is the
    // fraction of the delay that is randomized: 1.0 (the default) is "full
    // jitter", a uniform pick in [0, delay]; 0.5 keeps half fixed and jitters
    // the rest.
    Backoff& with_jitter(double ratio = 1.0) {
        jitter_ratio_ = std::clamp(ratio, 0.0, 1.0);
        return *this;
    }

    // The delay after the `attempt`-th failed attempt (1-based).
    Duration delay_for(std::size_t attempt, const RandomFn& rng) const {
        if (attempt < 1) attempt = 1;

        double raw = 0.0;
        switch (kind_) {
            case Kind::Fixed:
                raw = base_ms_;
                break;
            case Kind::Linear:
                raw = base_ms_ * static_cast<double>(attempt);
                break;
            case Kind::Exponential:
                raw = base_ms_ * std::pow(factor_, static_cast<double>(attempt - 1));
                break;
            case Kind::Custom:
                raw = custom_ ? to_ms(custom_(attempt)) : 0.0;
                break;
        }

        // Guard against overflow / non-finite growth before clamping.
        if (!std::isfinite(raw) || raw < 0.0) {
            raw = (cap_ms_ >= 0.0) ? cap_ms_ : kNoCapCeiling;
        }
        if (cap_ms_ >= 0.0 && raw > cap_ms_) raw = cap_ms_;

        if (jitter_ratio_ > 0.0) {
            double r = rng ? rng() : default_random();
            if (r < 0.0) r = 0.0;
            if (r >= 1.0) r = std::nextafter(1.0, 0.0);
            const double low = raw * (1.0 - jitter_ratio_);
            raw = low + (raw - low) * r;
        }

        if (raw < 0.0) raw = 0.0;
        return Duration{static_cast<Duration::rep>(std::llround(raw))};
    }

    // Convenience overload using the default random source.
    Duration delay_for(std::size_t attempt) const {
        return delay_for(attempt, RandomFn{});
    }

private:
    enum class Kind : std::uint8_t { Fixed, Linear, Exponential, Custom };

    Backoff(Kind kind, double base_ms, double factor)
        : kind_(kind), base_ms_(base_ms), factor_(factor) {}

    static double to_ms(Duration d) { return static_cast<double>(d.count()); }

    // Ceiling used when exponential growth overflows and no cap was set, so the
    // result stays representable (~115 days).
    static constexpr double kNoCapCeiling = 1e10;

    Kind kind_;
    double base_ms_;
    double factor_;
    double cap_ms_ = -1.0;       // < 0 means "no cap"
    double jitter_ratio_ = 0.0;  // 0 means "no jitter"
    std::function<Duration(std::size_t)> custom_;  // only used by Kind::Custom
};

}  // namespace keel

#endif  // KEEL_BACKOFF_HPP
