// keel 1.4.0, single-header edition.
//
// Resilience primitives for backend and database calls in modern C++.
// Copy this one file into your project and #include it.
//
// GENERATED FILE, do not edit. It is built from include/keel/ by
// tools/amalgamate.py; change the headers there and regenerate.
//
// Copyright (c) 2026 the keel authors.
// SPDX-License-Identifier: MIT

// keel: umbrella header.
//
// Include this to pull in the whole library, or include the individual headers
// (keel/retry.hpp, keel/circuit_breaker.hpp, ...) for finer control.
//
// SPDX-License-Identifier: MIT
#ifndef KEEL_KEEL_HPP
#define KEEL_KEEL_HPP

// ---- begin keel/adaptive_limiter.hpp -----------------------------------
// AdaptiveLimiter: a concurrency limit that finds the right value by itself.
//
// "At most 50 requests in flight" is a guess, and the right number keeps
// changing: it depends on the database's load, on the query mix, on what else
// is running. Set it too high and a slow dependency builds a queue that hurts
// everyone; set it too low and you waste capacity.
//
// An adaptive limiter treats this like TCP treats congestion. It watches how
// long calls take and whether they fail from overload, and nudges the limit up
// while things are healthy and down as soon as latency climbs or calls start
// timing out. Calls over the current limit are rejected at once with
// limit_exceeded_error, which is much cheaper than letting them queue.
//
//   keel::AdaptiveLimiter db_limit{keel::GradientLimit{},
//                                  keel::AdaptiveLimiterConfig{}
//                                      .with_initial_limit(20)
//                                      .with_max_limit(200)};
//
//   auto rows = db_limit.execute([&] { return db.query("..."); });
//
// Three algorithms ship with keel (AimdLimit, VegasLimit, GradientLimit, see
// below), and you can write your own by deriving from LimitAlgorithm. The idea
// and the algorithms follow Netflix's concurrency-limits library.
//
// The limiter is thread-safe. The clock is injectable for deterministic tests.
#ifndef KEEL_ADAPTIVE_LIMITER_HPP
#define KEEL_ADAPTIVE_LIMITER_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

// ---- begin keel/common.hpp ---------------------------------------------
// keel: resilience primitives for backend and database calls in modern C++.
//
// Copyright (c) 2026 the keel authors.
// SPDX-License-Identifier: MIT
//
// Shared type aliases and the default clock / random sources. All of these are
// injectable so the higher-level components can be tested deterministically,
// without real sleeping or wall-clock time.
//
// Requires C++17.
#ifndef KEEL_COMMON_HPP
#define KEEL_COMMON_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>

#define KEEL_VERSION_MAJOR 1
#define KEEL_VERSION_MINOR 4
#define KEEL_VERSION_PATCH 0

namespace keel {

// Delays and timeouts are expressed in milliseconds throughout the library.
using Duration = std::chrono::milliseconds;

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

// A source of randomness returning a value in [0, 1). Injected into backoff so
// jitter is reproducible under test.
using RandomFn = std::function<double()>;

// A source of "now". Injected into the circuit breaker so state transitions can
// be driven by a controllable clock in tests.
using ClockFn = std::function<TimePoint()>;

// Default randomness: a thread-local Mersenne Twister. Good enough for jitter;
// not for cryptography.
inline double default_random() {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(gen);
}

inline TimePoint default_now() { return SteadyClock::now(); }

namespace detail {

// A clock that skips the std::function call when no custom clock was given.
// Reading time sits on hot paths, so the indirect call is worth avoiding.
class Clock {
public:
    explicit Clock(ClockFn fn = {}) : fn_(std::move(fn)) {}
    TimePoint now() const { return fn_ ? fn_() : SteadyClock::now(); }

private:
    ClockFn fn_;
};

}  // namespace detail

// Base class for every error keel itself throws when it refuses or abandons a
// call (circuit_open_error, rate_limited_error, timeout_error). Catch this to
// handle "keel said no" in one place, separately from your own errors.
class error : public std::runtime_error {
public:
    explicit error(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace keel

#endif  // KEEL_COMMON_HPP
// ---- end keel/common.hpp -----------------------------------------------

// See circuit_breaker.hpp: the success path after an always-throwing call is
// legitimately unreachable, and MSVC's C4702 about it is noise in a header.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

namespace keel {

// Thrown by AdaptiveLimiter::execute when the current limit is reached.
class limit_exceeded_error : public error {
public:
    limit_exceeded_error() : error("keel: concurrency limit reached") {}
};

// What the limiter learned from one finished call.
struct LimitSample {
    // How long the call took.
    SteadyClock::duration rtt;
    // Calls in flight when this one started, itself included.
    std::size_t in_flight;
    // True if the call failed in a way that suggests overload (a timeout, a
    // "too busy" error). Ordinary errors are not samples at all.
    bool dropped;
};

// Decides the next limit from the current one and a sample. Implement this to
// plug in your own strategy. Calls are serialized by the limiter, so an
// implementation does not need to be thread-safe. The result is clamped to
// the limiter's [min_limit, max_limit].
class LimitAlgorithm {
public:
    virtual ~LimitAlgorithm() = default;
    virtual double update(double limit, const LimitSample& sample) = 0;
};

namespace detail {

inline double to_seconds(SteadyClock::duration d) {
    return std::chrono::duration<double>(d).count();
}

// A call that used less than half the limit says nothing about whether the
// limit is too low, so most algorithms don't grow on it.
inline bool app_limited(double limit, const LimitSample& s) {
    return static_cast<double>(s.in_flight) * 2.0 < limit;
}

}  // namespace detail

// Additive increase, multiplicative decrease: +1 per healthy call, x0.9 per
// dropped one. Simple and predictable; reacts to failures, not to latency
// (unless you set a timeout, which turns slow calls into drops).
class AimdLimit : public LimitAlgorithm {
public:
    AimdLimit& with_increase(double n) {
        increase_ = n;
        return *this;
    }
    AimdLimit& with_backoff_ratio(double r) {
        backoff_ratio_ = std::clamp(r, 0.0, 1.0);
        return *this;
    }
    // Calls slower than this count as dropped. Zero (default) disables it.
    AimdLimit& with_timeout(Duration d) {
        timeout_ = d;
        return *this;
    }

    double update(double limit, const LimitSample& s) override {
        const bool too_slow = timeout_.count() > 0 && s.rtt > timeout_;
        if (s.dropped || too_slow) return limit * backoff_ratio_;
        if (detail::app_limited(limit, s)) return limit;
        return limit + increase_;
    }

private:
    double increase_ = 1.0;
    double backoff_ratio_ = 0.9;
    Duration timeout_{0};
};

// Delay-based, after TCP Vegas. Remembers the fastest response seen (the
// "no load" latency) and estimates how many calls are queueing from how much
// slower the current one was. Few queued: grow. Many queued: shrink. Reacts
// to rising latency before anything actually fails.
class VegasLimit : public LimitAlgorithm {
public:
    // Grow while the estimated queue is below alpha * log10(limit)...
    VegasLimit& with_alpha(double a) {
        alpha_ = a;
        return *this;
    }
    // ...and shrink once it is above beta * log10(limit).
    VegasLimit& with_beta(double b) {
        beta_ = b;
        return *this;
    }
    // Forget the no-load latency every n samples, so a permanent change in
    // the backend (a new index, a slower replica) is eventually learned.
    VegasLimit& with_probe_every(std::size_t n) {
        probe_every_ = n;
        return *this;
    }
    // Fraction of each step to apply, from 0 (frozen) to 1 (full step).
    VegasLimit& with_smoothing(double s) {
        smoothing_ = std::clamp(s, 0.0, 1.0);
        return *this;
    }

    double update(double limit, const LimitSample& s) override {
        const double rtt = detail::to_seconds(s.rtt);
        if (rtt <= 0.0) return limit;

        if (probe_every_ > 0 && ++samples_ % probe_every_ == 0) no_load_rtt_ = 0.0;
        if (no_load_rtt_ <= 0.0 || rtt < no_load_rtt_) no_load_rtt_ = rtt;

        const double step = std::max(1.0, std::log10(limit));
        double next = limit;
        if (s.dropped) {
            next = limit - step;
        } else if (detail::app_limited(limit, s)) {
            return limit;
        } else {
            const double queued = limit * (1.0 - no_load_rtt_ / rtt);
            if (queued < alpha_ * step) {
                next = limit + step;
            } else if (queued > beta_ * step) {
                next = limit - step;
            }
        }
        return limit + (next - limit) * smoothing_;
    }

private:
    double alpha_ = 3.0;
    double beta_ = 6.0;
    double smoothing_ = 1.0;
    std::size_t probe_every_ = 1000;
    std::size_t samples_ = 0;
    double no_load_rtt_ = 0.0;
};

// Gradient-based, after Netflix's Gradient2. Compares a short-term average of
// latency with a long-term one. While they agree the limit creeps up; when
// recent calls get slower than usual, the limit is scaled down in proportion.
// Copes better than Vegas with backends whose normal latency drifts.
class GradientLimit : public LimitAlgorithm {
public:
    // How much slower than the long-term average recent calls may get before
    // the limit starts shrinking (1.5 = 50% slower).
    GradientLimit& with_tolerance(double t) {
        tolerance_ = std::max(1.0, t);
        return *this;
    }
    // Headroom added on top of the scaled limit, so it can keep probing up.
    GradientLimit& with_queue_size(double q) {
        queue_size_ = std::max(0.0, q);
        return *this;
    }
    // Fraction of each change to apply, from 0 (frozen) to 1 (full change).
    GradientLimit& with_smoothing(double s) {
        smoothing_ = std::clamp(s, 0.0, 1.0);
        return *this;
    }
    // Sample counts for the short and long averages.
    GradientLimit& with_windows(std::size_t short_window, std::size_t long_window) {
        short_alpha_ = 2.0 / (static_cast<double>(std::max<std::size_t>(short_window, 1)) + 1.0);
        long_alpha_ = 2.0 / (static_cast<double>(std::max<std::size_t>(long_window, 1)) + 1.0);
        return *this;
    }

    double update(double limit, const LimitSample& s) override {
        const double rtt = detail::to_seconds(s.rtt);
        if (rtt <= 0.0) return limit;

        short_rtt_ = short_rtt_ <= 0.0 ? rtt : short_rtt_ + short_alpha_ * (rtt - short_rtt_);
        long_rtt_ = long_rtt_ <= 0.0 ? rtt : long_rtt_ + long_alpha_ * (rtt - long_rtt_);
        // After a slow spell the long average lags far behind; pull it toward
        // the present faster so the limit can recover.
        if (long_rtt_ > 2.0 * short_rtt_) long_rtt_ *= 0.95;

        if (s.dropped) return limit * 0.9;
        if (detail::app_limited(limit, s)) return limit;

        const double gradient = std::clamp(tolerance_ * long_rtt_ / short_rtt_, 0.5, 1.0);
        const double next = limit * gradient + queue_size_;
        return limit + (next - limit) * smoothing_;
    }

private:
    double tolerance_ = 1.5;
    double queue_size_ = 4.0;
    double smoothing_ = 0.2;
    double short_alpha_ = 2.0 / 11.0;   // ~10 samples
    double long_alpha_ = 2.0 / 601.0;   // ~600 samples
    double short_rtt_ = 0.0;
    double long_rtt_ = 0.0;
};

struct AdaptiveLimiterConfig {
    double initial_limit = 20.0;
    double min_limit = 1.0;
    double max_limit = 1000.0;
    // Which exceptions mean "overloaded" and should push the limit down.
    // Empty means all of them. Exceptions it rejects (a validation error, a
    // "not found") are not used as samples at all. Non-std exceptions always
    // count as drops.
    std::function<bool(const std::exception&)> drop_if;
    // Called with (old, new) whenever the whole-number limit changes, outside
    // the limiter's lock. Handy for a metric. It must not throw.
    std::function<void(double, double)> on_limit_change;

    AdaptiveLimiterConfig& with_initial_limit(double n) {
        initial_limit = n;
        return *this;
    }
    AdaptiveLimiterConfig& with_min_limit(double n) {
        min_limit = n;
        return *this;
    }
    AdaptiveLimiterConfig& with_max_limit(double n) {
        max_limit = n;
        return *this;
    }
    AdaptiveLimiterConfig& with_drop_if(std::function<bool(const std::exception&)> f) {
        drop_if = std::move(f);
        return *this;
    }
    AdaptiveLimiterConfig& with_on_limit_change(std::function<void(double, double)> f) {
        on_limit_change = std::move(f);
        return *this;
    }
};

class AdaptiveLimiter {
    enum class Outcome : std::uint8_t { Success, Dropped, Ignored };

public:
    // One admitted call, for code that cannot use execute(). Report how it
    // went; a permit dropped without a report frees its slot and teaches the
    // limiter nothing.
    class Permit {
    public:
        Permit(Permit&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              start_(other.start_),
              in_flight_(other.in_flight_) {}
        Permit& operator=(Permit&&) = delete;
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;

        ~Permit() {
            try {
                report(Outcome::Ignored);
            } catch (...) {  // NOLINT(bugprone-empty-catch): destructors must not throw
            }
        }

        // The call worked; its latency is a sample.
        void success() { report(Outcome::Success); }
        // The call failed from overload (timed out, was told "too busy").
        void dropped() { report(Outcome::Dropped); }
        // The call is done but says nothing about load.
        void ignore() { report(Outcome::Ignored); }

    private:
        friend class AdaptiveLimiter;
        Permit(AdaptiveLimiter* owner, TimePoint start, std::size_t in_flight)
            : owner_(owner), start_(start), in_flight_(in_flight) {}

        void report(Outcome outcome) {
            if (AdaptiveLimiter* owner = std::exchange(owner_, nullptr)) {
                owner->complete(start_, in_flight_, outcome);
            }
        }

        AdaptiveLimiter* owner_;
        TimePoint start_;
        std::size_t in_flight_;
    };

    // Takes any LimitAlgorithm by value, e.g. AdaptiveLimiter{VegasLimit{}}.
    template <class Algorithm,
              class = std::enable_if_t<std::is_base_of_v<LimitAlgorithm, std::decay_t<Algorithm>>>>
    explicit AdaptiveLimiter(Algorithm&& algorithm, AdaptiveLimiterConfig config = {},
                             ClockFn clock = {})
        : AdaptiveLimiter(std::make_unique<std::decay_t<Algorithm>>(std::forward<Algorithm>(algorithm)),
                          std::move(config), std::move(clock)) {}

    // For an algorithm chosen at runtime.
    explicit AdaptiveLimiter(std::unique_ptr<LimitAlgorithm> algorithm,
                             AdaptiveLimiterConfig config = {}, ClockFn clock = {})
        : algorithm_(std::move(algorithm)),
          config_(std::move(config)),
          clock_(std::move(clock)) {
        config_.min_limit = std::max(1.0, config_.min_limit);
        config_.max_limit = std::max(config_.min_limit, config_.max_limit);
        limit_ = clamp(config_.initial_limit);
    }

    AdaptiveLimiter(const AdaptiveLimiter&) = delete;
    AdaptiveLimiter& operator=(const AdaptiveLimiter&) = delete;

    // Admit a call if the limit allows; empty otherwise. Never waits.
    std::optional<Permit> try_acquire() {
        std::size_t in_flight = 0;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (in_flight_ >= allowed()) return std::nullopt;
            in_flight = ++in_flight_;
        }
        return Permit(this, clock_.now(), in_flight);
    }

    // Run `fn` if the limit allows, otherwise throw limit_exceeded_error. The
    // call's latency feeds the algorithm; exceptions count as drops unless
    // drop_if says otherwise.
    template <class Fn>
    auto execute(Fn&& fn) -> decltype(fn()) {
        std::optional<Permit> permit = try_acquire();
        if (!permit) throw limit_exceeded_error{};

        using Result = decltype(fn());
        try {
            if constexpr (std::is_void_v<Result>) {
                fn();
                permit->success();
            } else {
                Result value = fn();
                permit->success();
                return value;
            }
        } catch (const std::exception& e) {
            if (!config_.drop_if || config_.drop_if(e))
                permit->dropped();
            else
                permit->ignore();
            throw;
        } catch (...) {
            permit->dropped();
            throw;
        }
    }

    // The current limit. Calls are admitted while in_flight() < floor(limit).
    double limit() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return limit_;
    }

    std::size_t in_flight() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return in_flight_;
    }

private:
    // Caller must hold the lock.
    std::size_t allowed() const { return std::max<std::size_t>(1, static_cast<std::size_t>(limit_)); }

    double clamp(double limit) const {
        if (!std::isfinite(limit)) return config_.min_limit;
        return std::clamp(limit, config_.min_limit, config_.max_limit);
    }

    void complete(TimePoint start, std::size_t in_flight, Outcome outcome) {
        double old_limit = 0.0;
        double new_limit = 0.0;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            --in_flight_;
            if (outcome == Outcome::Ignored) return;

            const LimitSample sample{clock_.now() - start, in_flight, outcome == Outcome::Dropped};
            const double proposed = algorithm_->update(limit_, sample);
            old_limit = limit_;
            if (std::isfinite(proposed)) limit_ = clamp(proposed);
            new_limit = limit_;
        }
        if (config_.on_limit_change &&
            static_cast<std::size_t>(old_limit) != static_cast<std::size_t>(new_limit)) {
            config_.on_limit_change(old_limit, new_limit);
        }
    }

    std::unique_ptr<LimitAlgorithm> algorithm_;
    AdaptiveLimiterConfig config_;
    detail::Clock clock_;

    mutable std::mutex mutex_;
    double limit_ = 1.0;
    std::size_t in_flight_ = 0;
};

}  // namespace keel

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

#endif  // KEEL_ADAPTIVE_LIMITER_HPP
// ---- end keel/adaptive_limiter.hpp -------------------------------------
// ---- begin keel/backoff.hpp --------------------------------------------
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
// ---- end keel/backoff.hpp ----------------------------------------------
// ---- begin keel/bulkhead.hpp -------------------------------------------
// Bulkhead: cap how many calls to one dependency can run at the same time.
//
// The name comes from ships: watertight compartments keep one breach from
// sinking the whole hull. In a service, one slow dependency can quietly tie up
// every worker thread you have, and then everything is down, not just the
// part that talks to it. A bulkhead gives each dependency a fixed number of
// slots. When they're all taken, extra callers wait briefly in a small queue,
// or are turned away at once with bulkhead_full_error.
//
//   keel::Bulkhead payments{keel::BulkheadConfig{}
//                               .with_max_concurrent(8)   // 8 calls at once
//                               .with_max_queue(16)       // 16 more may wait...
//                               .with_max_wait(keel::Duration{50})};  // ...up to 50ms
//
//   auto receipt = payments.execute([&] { return gateway.charge(order); });
//
// For callback-style code, take a Permit and keep it alive for as long as the
// call is running; dropping it frees the slot.
//
// The bulkhead is thread-safe. Waiting callers are woken as slots free up, but
// not in strict arrival order.
#ifndef KEEL_BULKHEAD_HPP
#define KEEL_BULKHEAD_HPP

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>


namespace keel {

// Thrown by Bulkhead::execute when no slot became free in time.
class bulkhead_full_error : public error {
public:
    bulkhead_full_error() : error("keel: bulkhead is full") {}
};

struct BulkheadConfig {
    // Calls allowed to run at the same time.
    std::size_t max_concurrent = 10;
    // Callers allowed to wait for a slot when all are taken. Zero means
    // nobody waits: a full bulkhead rejects immediately.
    std::size_t max_queue = 0;
    // How long a waiting caller of execute() waits before giving up.
    Duration max_wait = Duration{0};

    BulkheadConfig& with_max_concurrent(std::size_t n) {
        max_concurrent = n;
        return *this;
    }
    BulkheadConfig& with_max_queue(std::size_t n) {
        max_queue = n;
        return *this;
    }
    BulkheadConfig& with_max_wait(Duration d) {
        max_wait = d;
        return *this;
    }
};

class Bulkhead {
public:
    // Holds one slot until it is destroyed or release() is called.
    class Permit {
    public:
        Permit(Permit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        Permit& operator=(Permit&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        ~Permit() { release(); }

        // Give the slot back early. Safe to call more than once.
        void release() noexcept {
            if (Bulkhead* owner = std::exchange(owner_, nullptr)) owner->release_slot();
        }

    private:
        friend class Bulkhead;
        explicit Permit(Bulkhead* owner) : owner_(owner) {}

        Bulkhead* owner_;
    };

    explicit Bulkhead(BulkheadConfig config = {}) : config_(config) {}

    Bulkhead(const Bulkhead&) = delete;
    Bulkhead& operator=(const Bulkhead&) = delete;

    // Take a slot if one is free right now; never waits.
    std::optional<Permit> try_acquire() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (in_flight_ >= config_.max_concurrent) return std::nullopt;
        ++in_flight_;
        return Permit(this);
    }

    // Take a slot, waiting up to `max_wait` for one if there is room in the
    // queue. Empty if the queue is full or the wait ran out.
    std::optional<Permit> acquire_for(Duration max_wait) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (in_flight_ < config_.max_concurrent) {
            ++in_flight_;
            return Permit(this);
        }
        if (max_wait.count() <= 0 || waiting_ >= config_.max_queue) return std::nullopt;

        ++waiting_;
        const bool got_slot = slot_freed_.wait_for(
            lock, max_wait, [this] { return in_flight_ < config_.max_concurrent; });
        --waiting_;
        if (!got_slot) return std::nullopt;
        ++in_flight_;
        return Permit(this);
    }

    // Run `fn` in a slot, waiting up to the configured max_wait for one.
    // Throws bulkhead_full_error if none frees up. The slot is given back when
    // `fn` returns or throws.
    template <class Fn>
    auto execute(Fn&& fn) -> decltype(fn()) {
        std::optional<Permit> permit = acquire_for(config_.max_wait);
        if (!permit) throw bulkhead_full_error{};
        return fn();
    }

    // Calls currently holding a slot.
    std::size_t in_flight() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return in_flight_;
    }

    // Callers currently waiting for a slot.
    std::size_t waiting() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return waiting_;
    }

    // Free slots right now.
    std::size_t available() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return in_flight_ >= config_.max_concurrent ? 0 : config_.max_concurrent - in_flight_;
    }

private:
    void release_slot() noexcept {
        bool someone_waiting = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            --in_flight_;
            someone_waiting = waiting_ > 0;
        }
        // Waiters register under the same lock before they block, so if none
        // is counted here, none can miss this wake-up. Skipping the notify on
        // the common, uncontended path saves a kernel round trip.
        if (someone_waiting) slot_freed_.notify_one();
    }

    const BulkheadConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable slot_freed_;
    std::size_t in_flight_ = 0;
    std::size_t waiting_ = 0;
};

}  // namespace keel

#endif  // KEEL_BULKHEAD_HPP
// ---- end keel/bulkhead.hpp ---------------------------------------------
// ---- begin keel/circuit_breaker.hpp ------------------------------------
// CircuitBreaker: stop hammering a failing dependency.
//
// A breaker has three states:
//
//   Closed    calls pass through; consecutive failures are counted. After
//             `failure_threshold` of them the breaker trips to Open.
//   Open      calls fail fast with circuit_open_error, without touching the
//             dependency. After `reset_timeout` elapses the breaker moves to
//             HalfOpen.
//   HalfOpen  up to `max_half_open_calls` trial calls may run at once; extra
//             callers are rejected. `success_threshold` successful trials close
//             the breaker; any failed trial opens it again.
//
//   keel::CircuitBreaker cb{keel::CircuitConfig{}
//                               .with_failure_threshold(5)
//                               .with_reset_timeout(keel::Duration{30000})
//                               .with_failure_if([](const std::exception& e) {
//                                   return !is_not_found(e);  // a 404 is not an outage
//                               })};
//   auto value = cb.execute([&] { return call_service(); });
//
// For callback-style or async code that cannot wrap the call in execute(),
// take a Permit with try_acquire() and report the outcome on it later.
//
// The breaker is thread-safe. The clock is injectable for deterministic tests.
#ifndef KEEL_CIRCUIT_BREAKER_HPP
#define KEEL_CIRCUIT_BREAKER_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>


// When the wrapped function always throws (common with test doubles), the
// success path after it is legitimately unreachable and MSVC says so (C4702)
// once it inlines the call. That is noise in a header, so silence it here.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

namespace keel {

enum class CircuitState : std::uint8_t { Closed, Open, HalfOpen };

inline const char* to_string(CircuitState s) noexcept {
    switch (s) {
        case CircuitState::Closed:
            return "closed";
        case CircuitState::Open:
            return "open";
        case CircuitState::HalfOpen:
            return "half-open";
    }
    return "unknown";
}

// How the breaker decides to trip while Closed.
enum class FailureWindow : std::uint8_t {
    // After `failure_threshold` failures in a row. Simple; the default.
    Consecutive,
    // On the failure (or slow-call) rate over the last `window_calls` calls.
    Count,
    // On the failure (or slow-call) rate over the last `window_duration`.
    Time,
};

// Thrown by CircuitBreaker::execute when the breaker refuses a call.
class circuit_open_error : public error {
public:
    explicit circuit_open_error(Duration retry_after = Duration{0})
        : error("keel: circuit is open"), retry_after_(retry_after) {}

    // Roughly how long until the breaker lets trial calls through again. Zero
    // when it is already half-open and only turned this call away because the
    // trial slots were taken.
    Duration retry_after() const noexcept { return retry_after_; }

private:
    Duration retry_after_;
};

struct CircuitConfig {
    // Consecutive failures in Closed before tripping to Open.
    std::size_t failure_threshold = 5;
    // Successful trial calls in HalfOpen before closing again.
    std::size_t success_threshold = 1;
    // How long to stay Open before allowing trial calls (HalfOpen).
    Duration reset_timeout = Duration{30000};
    // How many trial calls may run at the same time while HalfOpen. Keeping
    // this small is what stops a recovering service from being flooded.
    std::size_t max_half_open_calls = 1;
    // Decides which exceptions count as failures. Empty means all of them.
    // Exceptions it rejects (think "user not found") pass through untouched
    // and do not affect the breaker. Non-std exceptions always count.
    std::function<bool(const std::exception&)> failure_if;
    // Called after every state change, outside the breaker's lock, from
    // whichever thread caused it. Use it for logging and metrics. It must not
    // throw.
    std::function<void(CircuitState from, CircuitState to)> on_state_change;

    // --- Rate-based tripping (see FailureWindow) -----------------------------
    // Counting failures in a row works for hard outages, but real trouble often
    // looks like "a third of calls fail" or "everything got slow". A count or
    // time window catches that.
    FailureWindow window = FailureWindow::Consecutive;
    // Calls kept by a Count window.
    std::size_t window_calls = 100;
    // Span covered by a Time window (tracked in 10 buckets).
    Duration window_duration = Duration{60000};
    // A window must hold at least this many calls before it can trip the
    // breaker, so one early failure can't open it.
    std::size_t minimum_calls = 10;
    // Trip when this fraction of calls in the window failed (0.5 = half).
    double failure_rate_threshold = 0.5;
    // Calls slower than this count as slow. Zero (default) turns slow-call
    // tracking off. A slow trial call in HalfOpen counts as a failure.
    Duration slow_call_duration = Duration{0};
    // Trip when this fraction of calls in the window were slow.
    double slow_call_rate_threshold = 1.0;

    CircuitConfig& with_count_window(std::size_t calls) {
        window = FailureWindow::Count;
        window_calls = calls;
        return *this;
    }
    CircuitConfig& with_time_window(Duration span) {
        window = FailureWindow::Time;
        window_duration = span;
        return *this;
    }
    CircuitConfig& with_minimum_calls(std::size_t n) {
        minimum_calls = n;
        return *this;
    }
    CircuitConfig& with_failure_rate_threshold(double rate) {
        failure_rate_threshold = rate;
        return *this;
    }
    CircuitConfig& with_slow_calls(Duration slower_than, double rate_threshold = 1.0) {
        slow_call_duration = slower_than;
        slow_call_rate_threshold = rate_threshold;
        return *this;
    }

    CircuitConfig& with_failure_threshold(std::size_t n) {
        failure_threshold = n;
        return *this;
    }
    CircuitConfig& with_success_threshold(std::size_t n) {
        success_threshold = n;
        return *this;
    }
    CircuitConfig& with_reset_timeout(Duration d) {
        reset_timeout = d;
        return *this;
    }
    CircuitConfig& with_max_half_open_calls(std::size_t n) {
        max_half_open_calls = n;
        return *this;
    }
    CircuitConfig& with_failure_if(std::function<bool(const std::exception&)> f) {
        failure_if = std::move(f);
        return *this;
    }
    CircuitConfig& with_on_state_change(std::function<void(CircuitState, CircuitState)> f) {
        on_state_change = std::move(f);
        return *this;
    }
};

// A snapshot of what the breaker currently knows.
struct CircuitMetrics {
    CircuitState state = CircuitState::Closed;
    std::size_t consecutive_failures = 0;
    // The rest is only tracked with a Count or Time window; zero otherwise.
    std::size_t calls = 0;
    std::size_t failures = 0;
    std::size_t slow_calls = 0;
    double failure_rate = 0.0;
    double slow_call_rate = 0.0;
};

namespace detail {

// Recent call outcomes for rate-based tripping: either the last N calls (a
// ring buffer) or the last T of time (fixed buckets). Not thread-safe; the
// breaker guards it.
class OutcomeWindow {
public:
    struct Totals {
        std::size_t calls = 0;
        std::size_t failures = 0;
        std::size_t slow = 0;
    };

    void configure(FailureWindow mode, std::size_t calls, Duration span) {
        mode_ = mode;
        if (mode_ == FailureWindow::Count) {
            ring_.assign(std::max<std::size_t>(calls, 1), 0);
        } else if (mode_ == FailureWindow::Time) {
            const Duration::rep width = span.count() / static_cast<Duration::rep>(kBuckets);
            bucket_width_ = Duration{std::max<Duration::rep>(width, 1)};
        }
        reset();
    }

    void record(TimePoint now, bool failure, bool slow) {
        if (mode_ == FailureWindow::Count) {
            const std::uint8_t flags = static_cast<std::uint8_t>((failure ? kFailure : 0) |
                                                                 (slow ? kSlow : 0));
            if (filled_ == ring_.size()) {
                remove(ring_[next_]);
            } else {
                ++filled_;
            }
            ring_[next_] = flags;
            add(flags);
            next_ = (next_ + 1) % ring_.size();
        } else if (mode_ == FailureWindow::Time) {
            Bucket& bucket = current_bucket(now);
            ++bucket.calls;
            if (failure) ++bucket.failures;
            if (slow) ++bucket.slow;
        }
    }

    Totals totals(TimePoint now) const {
        if (mode_ == FailureWindow::Count) return totals_;
        Totals t;
        if (mode_ != FailureWindow::Time) return t;
        const std::int64_t slot = slot_of(now);
        for (const Bucket& b : buckets_) {
            if (b.slot > slot - static_cast<std::int64_t>(kBuckets) && b.slot <= slot) {
                t.calls += b.calls;
                t.failures += b.failures;
                t.slow += b.slow;
            }
        }
        return t;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), std::uint8_t{0});
        next_ = 0;
        filled_ = 0;
        totals_ = Totals{};
        buckets_.fill(Bucket{});
    }

private:
    static constexpr std::uint8_t kFailure = 1;
    static constexpr std::uint8_t kSlow = 2;
    static constexpr std::size_t kBuckets = 10;

    struct Bucket {
        std::int64_t slot = -1;
        std::size_t calls = 0;
        std::size_t failures = 0;
        std::size_t slow = 0;
    };

    void add(std::uint8_t flags) {
        ++totals_.calls;
        if (flags & kFailure) ++totals_.failures;
        if (flags & kSlow) ++totals_.slow;
    }
    void remove(std::uint8_t flags) {
        --totals_.calls;
        if (flags & kFailure) --totals_.failures;
        if (flags & kSlow) --totals_.slow;
    }

    std::int64_t slot_of(TimePoint now) const {
        const auto slot = static_cast<std::int64_t>(now.time_since_epoch() / bucket_width_);
        return slot < 0 ? 0 : slot;
    }

    Bucket& current_bucket(TimePoint now) {
        const std::int64_t slot = slot_of(now);
        Bucket& bucket = buckets_[static_cast<std::size_t>(slot) % kBuckets];
        if (bucket.slot != slot) bucket = Bucket{slot, 0, 0, 0};
        return bucket;
    }

    FailureWindow mode_ = FailureWindow::Consecutive;
    // Count window.
    std::vector<std::uint8_t> ring_;
    std::size_t next_ = 0;
    std::size_t filled_ = 0;
    Totals totals_;
    // Time window.
    Duration bucket_width_{1};
    std::array<Bucket, kBuckets> buckets_{};
};

}  // namespace detail

class CircuitBreaker {
    enum class Outcome : std::uint8_t { Success, Failure, Ignored };

    // An admitted call: the generation it belongs to, and when it started
    // (only measured when slow-call tracking is on).
    struct Call {
        std::uint64_t ticket = 0;
        TimePoint start{};
    };

public:
    // Permission to make one call, for code that cannot use execute(). Report
    // how the call went with success() or failure(). A permit dropped without a
    // report counts as neither, and simply frees its slot.
    class Permit {
    public:
        Permit(Permit&& other) noexcept
            : breaker_(std::exchange(other.breaker_, nullptr)), call_(other.call_) {}
        Permit& operator=(Permit&&) = delete;
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;

        ~Permit() {
            try {
                report(Outcome::Ignored);
            } catch (...) {  // NOLINT(bugprone-empty-catch): destructors must not throw
            }
        }

        void success() { report(Outcome::Success); }
        void failure() { report(Outcome::Failure); }

    private:
        friend class CircuitBreaker;
        Permit(CircuitBreaker* breaker, Call call) : breaker_(breaker), call_(call) {}

        void report(Outcome outcome) {
            if (CircuitBreaker* b = std::exchange(breaker_, nullptr)) b->record(call_, outcome);
        }

        CircuitBreaker* breaker_;
        Call call_;
    };

    explicit CircuitBreaker(CircuitConfig config = {}, ClockFn clock = {})
        : config_(std::move(config)),
          clock_(std::move(clock)) {
        window_.configure(config_.window, config_.window_calls, config_.window_duration);
    }

    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;

    // The current state. Reading it may move an expired Open breaker to
    // HalfOpen (and fire on_state_change), so it is not a pure observer.
    CircuitState state() const {
        Transitions changes;
        CircuitState s;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            LazyNow now(clock_);
            refresh(now, changes);
            s = state_;
        }
        notify(changes);
        return s;
    }

    // Everything the breaker currently knows, in one consistent snapshot.
    CircuitMetrics metrics() const {
        Transitions changes;
        CircuitMetrics m;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            LazyNow now(clock_);
            refresh(now, changes);
            const detail::OutcomeWindow::Totals t = window_.totals(window_time(now));
            m.state = state_;
            m.consecutive_failures = failures_;
            m.calls = t.calls;
            m.failures = t.failures;
            m.slow_calls = t.slow;
            if (t.calls > 0) {
                m.failure_rate = static_cast<double>(t.failures) / static_cast<double>(t.calls);
                m.slow_call_rate = static_cast<double>(t.slow) / static_cast<double>(t.calls);
            }
        }
        notify(changes);
        return m;
    }

    std::size_t consecutive_failures() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return failures_;
    }

    // How long until an Open breaker allows trial calls; zero otherwise.
    Duration retry_after() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return state_ == CircuitState::Open ? remaining_open_time() : Duration{0};
    }

    // Run `fn` through the breaker. Throws circuit_open_error without calling
    // `fn` when the breaker refuses; otherwise records the outcome and returns
    // the result or rethrows `fn`'s exception.
    template <class Fn>
    auto execute(Fn&& fn) -> decltype(fn()) {
        using Result = decltype(fn());
        const Call call = admit();

        // On success the outcome is recorded outside the try block, so an
        // exception from on_state_change is never mistaken for a call failure.
        if constexpr (std::is_void_v<Result>) {
            run_guarded(call, fn);
            record(call, Outcome::Success);
        } else {
            Result value = run_guarded(call, fn);
            record(call, Outcome::Success);
            return value;
        }
    }

    // Ask for a Permit; empty when the breaker refuses the call right now.
    std::optional<Permit> try_acquire() {
        Transitions changes;
        Call call;
        bool admitted = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            LazyNow now(clock_);
            refresh(now, changes);
            admitted = admit_locked(call.ticket);
        }
        notify(changes);
        if (!admitted) return std::nullopt;
        call.start = call_start();
        return Permit(this, call);
    }

    // Close the breaker and clear its counters, e.g. after a manual fix.
    void reset() {
        Transitions changes;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            transition(CircuitState::Closed, changes);
            failures_ = 0;  // also when it was already closed
        }
        notify(changes);
    }

    // Open the breaker now, e.g. for planned maintenance. It recovers through
    // HalfOpen after reset_timeout like after any other trip.
    void force_open() {
        Transitions changes;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            LazyNow now(clock_);
            trip(now, changes);
        }
        notify(changes);
    }

private:
    // A tiny fixed-size list of state changes made under the lock, delivered to
    // on_state_change after the lock is released. One locked section makes at
    // most two changes (Open -> HalfOpen, then HalfOpen -> Open or Closed).
    struct Transitions {
        std::array<std::pair<CircuitState, CircuitState>, 2> items{};
        std::size_t count = 0;
        void add(CircuitState from, CircuitState to) {
            if (count < items.size()) items[count++] = {from, to};
        }
    };

    template <class Fn>
    decltype(auto) run_guarded(const Call& call, Fn& fn) {
        try {
            return fn();
        } catch (const std::exception& e) {
            record(call, classify(e));
            throw;
        } catch (...) {
            record(call, Outcome::Failure);
            throw;
        }
    }

    // Reads the clock at most once, and only if something actually asks. In
    // the default setup a successful call never needs the time at all, and on
    // some platforms a clock read costs more than the rest of the call.
    class LazyNow {
    public:
        explicit LazyNow(const detail::Clock& clock) : clock_(clock) {}
        TimePoint get() {
            if (!read_) {
                value_ = clock_.now();
                read_ = true;
            }
            return value_;
        }

    private:
        const detail::Clock& clock_;
        TimePoint value_{};
        bool read_ = false;
    };

    // Only a time window cares when a call happened.
    TimePoint window_time(LazyNow& now) const {
        return config_.window == FailureWindow::Time ? now.get() : TimePoint{};
    }

    bool tracks_slow_calls() const { return config_.slow_call_duration.count() > 0; }

    // Reading the clock costs a little, so only do it when it's needed.
    TimePoint call_start() const { return tracks_slow_calls() ? clock_.now() : TimePoint{}; }

    Outcome classify(const std::exception& e) const {
        if (!config_.failure_if) return Outcome::Failure;
        return config_.failure_if(e) ? Outcome::Failure : Outcome::Ignored;
    }

    // Lets a call in or throws circuit_open_error. Returns the generation the
    // call belongs to, so its outcome can be matched to the right state later.
    Call admit() {
        Transitions changes;
        Call call;
        bool admitted = false;
        Duration wait{0};
        {
            std::lock_guard<std::mutex> guard(mutex_);
            LazyNow now(clock_);
            refresh(now, changes);
            admitted = admit_locked(call.ticket);
            if (!admitted && state_ == CircuitState::Open) wait = remaining_open_time(now.get());
        }
        notify(changes);
        if (!admitted) throw circuit_open_error(wait);
        call.start = call_start();
        return call;
    }

    // Caller must hold the lock.
    bool admit_locked(std::uint64_t& ticket) {
        if (state_ == CircuitState::Open) return false;
        if (state_ == CircuitState::HalfOpen) {
            if (half_open_in_flight_ >= config_.max_half_open_calls) return false;
            ++half_open_in_flight_;
        }
        ticket = generation_;
        return true;
    }

    void record(const Call& call, Outcome outcome) {
        Transitions changes;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            LazyNow now(clock_);
            refresh(now, changes);
            // A call that started under an earlier state (say, a slow success
            // that began before the breaker tripped) says nothing about the
            // current one, so its outcome is dropped.
            if (call.ticket == generation_) {
                const bool slow =
                    tracks_slow_calls() && now.get() - call.start > config_.slow_call_duration;
                apply(outcome, slow, now, changes);
            }
        }
        notify(changes);
    }

    // Caller must hold the lock.
    void apply(Outcome outcome, bool slow, LazyNow& now, Transitions& changes) {
        if (state_ == CircuitState::Closed) {
            if (outcome == Outcome::Ignored) return;
            const bool failure = outcome == Outcome::Failure;
            failures_ = failure ? failures_ + 1 : 0;

            if (config_.window == FailureWindow::Consecutive) {
                if (failure && failures_ >= config_.failure_threshold) trip(now, changes);
            } else {
                window_.record(window_time(now), failure, slow);
                if (window_says_trip(now)) trip(now, changes);
            }
        } else if (state_ == CircuitState::HalfOpen) {
            if (half_open_in_flight_ > 0) --half_open_in_flight_;
            if (outcome == Outcome::Ignored) return;
            // A trial that "worked" but was too slow means the service is not
            // really back yet.
            if (outcome == Outcome::Failure || slow) {
                trip(now, changes);
            } else if (++half_open_successes_ >= config_.success_threshold) {
                transition(CircuitState::Closed, changes);
            }
        }
    }

    // Caller must hold the lock.
    bool window_says_trip(LazyNow& now) const {
        const detail::OutcomeWindow::Totals t = window_.totals(window_time(now));
        std::size_t needed = std::max<std::size_t>(config_.minimum_calls, 1);
        // A count window can never hold more than window_calls calls.
        if (config_.window == FailureWindow::Count) {
            needed = std::min(needed, std::max<std::size_t>(config_.window_calls, 1));
        }
        if (t.calls < needed) return false;

        const double calls = static_cast<double>(t.calls);
        if (static_cast<double>(t.failures) / calls >= config_.failure_rate_threshold) return true;
        return tracks_slow_calls() &&
               static_cast<double>(t.slow) / calls >= config_.slow_call_rate_threshold;
    }

    // Moves an expired Open breaker to HalfOpen. Caller must hold the lock.
    void refresh(LazyNow& now, Transitions& changes) const {
        if (state_ == CircuitState::Open && now.get() - opened_at_ >= config_.reset_timeout) {
            transition(CircuitState::HalfOpen, changes);
        }
    }

    // Caller must hold the lock. Restarts the open timer even if already open.
    void trip(LazyNow& now, Transitions& changes) {
        transition(CircuitState::Open, changes);
        opened_at_ = now.get();
    }

    // Caller must hold the lock. Every real state change starts a new
    // generation and resets the counters, which belong to one state only.
    void transition(CircuitState to, Transitions& changes) const {
        if (state_ == to) return;
        changes.add(state_, to);
        state_ = to;
        ++generation_;
        failures_ = 0;
        half_open_successes_ = 0;
        half_open_in_flight_ = 0;
        window_.reset();
    }

    // Caller must hold the lock.
    Duration remaining_open_time() const { return remaining_open_time(clock_.now()); }
    Duration remaining_open_time(TimePoint now) const {
        const auto left = opened_at_ + config_.reset_timeout - now;
        if (left <= SteadyClock::duration::zero()) return Duration{0};
        return std::chrono::ceil<Duration>(left);
    }

    void notify(const Transitions& changes) const {
        if (!config_.on_state_change) return;
        for (std::size_t i = 0; i < changes.count; ++i) {
            config_.on_state_change(changes.items[i].first, changes.items[i].second);
        }
    }

    const CircuitConfig config_;
    const detail::Clock clock_;

    // Mutable because state() is const but may advance Open to HalfOpen once
    // the reset timeout has passed. All of these are guarded by mutex_.
    mutable std::mutex mutex_;
    mutable CircuitState state_ = CircuitState::Closed;
    mutable std::uint64_t generation_ = 0;
    mutable std::size_t failures_ = 0;
    mutable std::size_t half_open_successes_ = 0;
    mutable std::size_t half_open_in_flight_ = 0;
    mutable detail::OutcomeWindow window_;
    TimePoint opened_at_{};
};

}  // namespace keel

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

#endif  // KEEL_CIRCUIT_BREAKER_HPP
// ---- end keel/circuit_breaker.hpp --------------------------------------
// ---- begin keel/fallback.hpp -------------------------------------------
// with_fallback(): run a primary operation, and if it throws, run a fallback.
//
//   // Serve from cache when the database is unreachable.
//   auto user = keel::with_fallback(
//       [&] { return db.load_user(id); },
//       [&] { return cache.load_user(id); });
//
// The fallback may also take the exception, to log it or to pick an answer
// based on what went wrong:
//
//   auto price = keel::with_fallback(
//       [&] { return pricing.quote(item); },
//       [&](const std::exception& e) {
//           log.warn("pricing down: {}", e.what());
//           return item.list_price;
//       });
//
// The fallback must return a type convertible to the primary's return type.
// Only std::exception-derived failures trigger the fallback; anything else
// propagates. Use with_fallback_if to restrict which exceptions are caught.
#ifndef KEEL_FALLBACK_HPP
#define KEEL_FALLBACK_HPP

#include <exception>
#include <type_traits>
#include <utility>

namespace keel {

namespace detail {

template <class Fallback>
decltype(auto) call_fallback(Fallback& fallback, const std::exception& e) {
    if constexpr (std::is_invocable_v<Fallback&, const std::exception&>) {
        return fallback(e);
    } else {
        return fallback();
    }
}

}  // namespace detail

template <class Primary, class Fallback>
auto with_fallback(Primary&& primary, Fallback&& fallback) -> decltype(primary()) {
    try {
        return primary();
    } catch (const std::exception& e) {
        return detail::call_fallback(fallback, e);
    }
}

template <class Primary, class Fallback, class Filter>
auto with_fallback_if(Primary&& primary, Fallback&& fallback, Filter&& filter)
    -> decltype(primary()) {
    try {
        return primary();
    } catch (const std::exception& e) {
        if (filter(e)) return detail::call_fallback(fallback, e);
        throw;
    }
}

}  // namespace keel

#endif  // KEEL_FALLBACK_HPP
// ---- end keel/fallback.hpp ---------------------------------------------
// ---- begin keel/rate_limiter.hpp ---------------------------------------
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
// ---- end keel/rate_limiter.hpp -----------------------------------------
// ---- begin keel/retry.hpp ----------------------------------------------
// retry(): re-run a fallible operation according to a RetryPolicy.
//
//   keel::RetryPolicy policy;
//   policy.max_attempts(5)
//         .backoff(keel::Backoff::exponential(keel::Duration{50}).with_jitter().cap(keel::Duration{2000}))
//         .retry_on<db::connection_error, db::timeout_error>()
//         .max_elapsed(keel::Duration{3000});
//
//   auto rows = keel::retry(policy, [&] { return db.query("SELECT ..."); });
//
// The operation is retried when it throws a std::exception that the policy's
// filter accepts. Exceptions not derived from std::exception always propagate.
// Retrying stops, and the last exception is rethrown, as soon as any limit is
// reached: the attempt count, the total time budget, or the RetryBudget.
#ifndef KEEL_RETRY_HPP
#define KEEL_RETRY_HPP

#include <cstddef>
#include <exception>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

// ---- begin keel/retry_budget.hpp ---------------------------------------
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
          clock_(std::move(clock)),
          origin_(clock_.now()) {
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
        const auto elapsed = clock_.now() - origin_;
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
    detail::Clock clock_;
    TimePoint origin_;
    Duration bucket_width_{1};

    mutable std::mutex mutex_;
    std::array<Bucket, kBuckets> buckets_{};
};

}  // namespace keel

#endif  // KEEL_RETRY_BUDGET_HPP
// ---- end keel/retry_budget.hpp -----------------------------------------

namespace keel {

// Decides whether a thrown exception is worth retrying.
using ExceptionFilter = std::function<bool(const std::exception&)>;

// Observer invoked before each wait: (attempt number, the exception, the delay).
using RetryHandler =
    std::function<void(std::size_t, const std::exception&, Duration)>;

// Performs the wait between attempts. Injected so tests need not really sleep,
// and so code running on an event loop can plug in its own waiting.
using SleepFn = std::function<void(Duration)>;

inline void default_sleep(Duration d) {
    if (d.count() > 0) std::this_thread::sleep_for(d);
}

class RetryPolicy {
public:
    // --- Fluent setters -----------------------------------------------------

    // Total number of attempts, including the first. Values < 1 are treated
    // as 1 by retry().
    RetryPolicy& max_attempts(std::size_t n) {
        max_attempts_ = n;
        return *this;
    }
    RetryPolicy& backoff(Backoff b) {
        backoff_ = std::move(b);
        return *this;
    }
    // Retry only when `f(exception)` returns true.
    RetryPolicy& retry_if(ExceptionFilter f) {
        filter_ = std::move(f);
        return *this;
    }
    // Shorthand for retry_if: retry only exceptions of (or derived from) the
    // listed types, e.g. retry_on<db::timeout, db::connection_lost>().
    template <class... Exceptions>
    RetryPolicy& retry_on() {
        static_assert(sizeof...(Exceptions) > 0, "retry_on<> needs at least one type");
        static_assert((std::is_base_of_v<std::exception, Exceptions> && ...),
                      "retry_on<> types must derive from std::exception");
        filter_ = [](const std::exception& e) {
            return ((dynamic_cast<const Exceptions*>(&e) != nullptr) || ...);
        };
        return *this;
    }
    // Called before each wait. Handy for logging and metrics.
    RetryPolicy& on_retry(RetryHandler h) {
        on_retry_ = std::move(h);
        return *this;
    }
    // Total time budget across all attempts and waits, measured from the first
    // attempt. A retry whose wait would cross it is not made. Zero (the
    // default) means no limit.
    RetryPolicy& max_elapsed(Duration d) {
        max_elapsed_ = d;
        return *this;
    }
    // Share a RetryBudget with this policy. Each call then counts as a request,
    // and each retry must be approved by the budget. The policy only keeps a
    // reference, so the budget has to outlive every call that uses it.
    RetryPolicy& budget(RetryBudget& b) {
        budget_ = &b;
        return *this;
    }
    // Replace how waits happen (tests pass a no-op).
    RetryPolicy& sleeper(SleepFn s) {
        sleeper_ = std::move(s);
        return *this;
    }
    // Replace the jitter RNG (tests pass a fixed value).
    RetryPolicy& random(RandomFn r) {
        random_ = std::move(r);
        return *this;
    }
    // Replace the clock used for max_elapsed.
    RetryPolicy& clock(ClockFn c) {
        clock_ = std::move(c);
        return *this;
    }

    // --- Accessors ------------------------------------------------------------

    std::size_t max_attempts() const { return max_attempts_; }
    const Backoff& backoff() const { return backoff_; }
    const ExceptionFilter& exception_filter() const { return filter_; }
    const RetryHandler& retry_handler() const { return on_retry_; }
    Duration max_elapsed() const { return max_elapsed_; }
    RetryBudget* retry_budget() const { return budget_; }
    const SleepFn& sleep_fn() const { return sleeper_; }
    const RandomFn& random_fn() const { return random_; }
    const ClockFn& clock_fn() const { return clock_; }

private:
    std::size_t max_attempts_ = 3;
    Backoff backoff_ = Backoff::exponential(Duration{100});
    ExceptionFilter filter_;           // empty => retry every std::exception
    RetryHandler on_retry_;            // empty => no observer
    Duration max_elapsed_{0};          // zero => no time limit
    RetryBudget* budget_ = nullptr;    // null => no budget
    SleepFn sleeper_;                  // empty => default_sleep
    RandomFn random_;                  // empty => default_random
    ClockFn clock_;                    // empty => default_now
};

namespace detail {

// The bookkeeping shared by retry() and retry_while(): counts attempts, checks
// every limit in one place, and performs the waits.
class RetryLoop {
public:
    explicit RetryLoop(const RetryPolicy& policy)
        : policy_(policy),
          attempts_(policy.max_attempts() == 0 ? std::size_t{1} : policy.max_attempts()) {
        if (policy_.max_elapsed().count() > 0) start_ = now();
        if (RetryBudget* budget = policy_.retry_budget()) budget->record_request();
    }

    std::size_t attempt() const { return attempt_; }

    // After a failed attempt: may we try again? If so, returns true and sets
    // `delay` to the wait before the next attempt. Checks are ordered so the
    // budget is only spent when a retry will actually happen.
    bool next(Duration& delay) const {
        if (attempt_ >= attempts_) return false;
        delay = policy_.backoff().delay_for(attempt_, policy_.random_fn());
        if (policy_.max_elapsed().count() > 0 &&
            (now() - start_) + delay > policy_.max_elapsed()) {
            return false;
        }
        RetryBudget* budget = policy_.retry_budget();
        return !(budget && !budget->try_spend_retry());
    }

    void wait(Duration delay) {
        const SleepFn& sleep = policy_.sleep_fn();
        if (sleep)
            sleep(delay);
        else
            default_sleep(delay);
        ++attempt_;
    }

private:
    TimePoint now() const {
        const ClockFn& clock = policy_.clock_fn();
        return clock ? clock() : default_now();
    }

    const RetryPolicy& policy_;
    std::size_t attempts_;
    std::size_t attempt_ = 1;
    TimePoint start_{};
};

}  // namespace detail

// Run `fn` under `policy`, returning whatever `fn` returns (including void).
template <class Fn>
auto retry(const RetryPolicy& policy, Fn&& fn) -> decltype(fn()) {
    detail::RetryLoop loop(policy);
    for (;;) {
        try {
            return fn();
        } catch (const std::exception& e) {
            const ExceptionFilter& filter = policy.exception_filter();
            if (filter && !filter(e)) throw;

            Duration delay{0};
            if (!loop.next(delay)) throw;

            if (policy.retry_handler()) policy.retry_handler()(loop.attempt(), e, delay);
            loop.wait(delay);
        }
    }
}

// Value-based retry: re-run `fn` while `is_failure(result)` is true, waiting
// between attempts according to the policy. Returns the last result (even if
// it still looks like a failure once a limit is hit). Use this for APIs that
// signal failure with a return value rather than an exception.
//
// Differences from retry(): exceptions thrown by `fn` propagate immediately and
// are not retried here, and the exception-oriented retry_if / retry_on /
// on_retry settings do not apply. Every other limit does. `fn` must return a
// non-void value.
template <class Fn, class IsFailure>
auto retry_while(const RetryPolicy& policy, Fn&& fn, IsFailure&& is_failure)
    -> decltype(fn()) {
    detail::RetryLoop loop(policy);
    for (;;) {
        auto result = fn();
        if (!is_failure(result)) return result;

        Duration delay{0};
        if (!loop.next(delay)) return result;
        loop.wait(delay);
    }
}

}  // namespace keel

#endif  // KEEL_RETRY_HPP
// ---- end keel/retry.hpp ------------------------------------------------
// ---- begin keel/single_flight.hpp --------------------------------------
// SingleFlight: collapse identical concurrent calls into one.
//
// A popular cache entry expires, and a thousand requests that all need it hit
// the database at the same moment, each asking for exactly the same row. That
// is a cache stampede, and it can take a database down on its own.
//
// SingleFlight lets the first caller for a key do the work. Anyone who asks
// for the same key while that call is still running does not start their own;
// they wait and get the same result, or the same exception.
//
//   keel::SingleFlight<std::string, User> loads;
//
//   User load_user(const std::string& id) {
//       return loads.run(id, [&] { return db.load_user(id); });
//   }
//
// Nothing is cached: once a call finishes, the next caller for that key starts
// a fresh one. Pair it with your cache rather than replacing it.
//
// Value must be copyable, since every waiter receives its own copy. Do not
// call run() for a key from inside the function that is computing that same
// key; it would wait for itself forever.
#ifndef KEEL_SINGLE_FLIGHT_HPP
#define KEEL_SINGLE_FLIGHT_HPP

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace keel {

template <class Key, class Value, class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>>
class SingleFlight {
    static_assert(!std::is_void_v<Value>, "SingleFlight needs a non-void Value");
    static_assert(!std::is_reference_v<Value>, "SingleFlight<Key, T&> is not supported");

public:
    SingleFlight() = default;
    SingleFlight(const SingleFlight&) = delete;
    SingleFlight& operator=(const SingleFlight&) = delete;

    // Run `fn` for `key`, or join a call for `key` that is already running.
    // Returns the result; rethrows the exception if the call failed.
    template <class Fn>
    Value run(const Key& key, Fn&& fn) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto it = calls_.find(key);
        if (it != calls_.end()) {
            Call& call = it->second;
            // The shared state is only created once somebody actually joins,
            // so a call nobody else asked for never pays for it.
            if (!call.promise) {
                call.promise.emplace();
                call.future = call.promise->get_future().share();
            }
            ++call.waiters;
            std::shared_future<Value> future = call.future;
            lock.unlock();
            return future.get();
        }

        const std::uint64_t id = ++next_id_;
        start_call(key, id);
        lock.unlock();

        try {
            Value value = fn();
            publish(key, id, [&](std::promise<Value>& p) { p.set_value(value); });
            return value;
        } catch (...) {
            const std::exception_ptr error = std::current_exception();
            publish(key, id, [&](std::promise<Value>& p) { p.set_exception(error); });
            throw;
        }
    }

    // Detach the running call for `key`, if any, so the next caller starts a
    // fresh one instead of joining it. Callers already waiting still get the
    // old call's result. Use it when you know that result is already stale,
    // for example right after the underlying data was written.
    void forget(const Key& key) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = calls_.find(key);
        if (it == calls_.end()) return;
        // Waiters still need the result, so keep their promise alive by id.
        if (it->second.promise) detached_.emplace(it->second.id, std::move(*it->second.promise));
        recycle(it);
    }

    // Number of keys with a call currently running.
    std::size_t in_flight() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return calls_.size();
    }

    // How many callers have joined the running call for `key` instead of
    // starting their own (0 if nothing is running). Handy for metrics.
    std::size_t waiters(const Key& key) const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = calls_.find(key);
        return it == calls_.end() ? 0 : it->second.waiters;
    }

private:
    struct Call {
        std::uint64_t id = 0;  // tells apart calls that reused the same key
        std::optional<std::promise<Value>> promise;  // created when the first waiter joins
        std::shared_future<Value> future;
        std::size_t waiters = 0;
    };

    using Map = std::unordered_map<Key, Call, Hash, KeyEqual>;

    // Map nodes kept for reuse. With a steady set of hot keys this means no
    // allocation per call for the map itself.
    static constexpr std::size_t kMaxSpareNodes = 64;

    // Caller must hold the lock.
    void start_call(const Key& key, std::uint64_t id) {
        if constexpr (std::is_copy_assignable_v<Key>) {
            if (!spare_.empty()) {
                typename Map::node_type node = std::move(spare_.back());
                spare_.pop_back();
                node.key() = key;
                node.mapped().id = id;
                calls_.insert(std::move(node));
                return;
            }
        }
        Call call;
        call.id = id;
        calls_.emplace(key, std::move(call));
    }

    // Caller must hold the lock. Removes the entry, keeping its node if there
    // is room.
    void recycle(typename Map::iterator it) {
        typename Map::node_type node = calls_.extract(it);
        if (spare_.size() < kMaxSpareNodes) {
            node.mapped() = Call{};
            spare_.push_back(std::move(node));
        }
    }

    // Hands the outcome to anyone who joined call `id`, then retires it. After
    // forget() the key may already point at a newer call, which is left alone.
    template <class Deliver>
    void publish(const Key& key, std::uint64_t id, Deliver deliver) {
        std::optional<std::promise<Value>> promise;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = calls_.find(key);
            if (it != calls_.end() && it->second.id == id) {
                promise = std::move(it->second.promise);
                recycle(it);
            } else {
                auto detached = detached_.find(id);
                if (detached != detached_.end()) {
                    promise = std::move(detached->second);
                    detached_.erase(detached);
                }
            }
        }
        // Delivered outside the lock: waking waiters shouldn't block new calls.
        if (promise) deliver(*promise);
    }

    mutable std::mutex mutex_;
    Map calls_;
    std::vector<typename Map::node_type> spare_;
    std::unordered_map<std::uint64_t, std::promise<Value>> detached_;
    std::uint64_t next_id_ = 0;
};

}  // namespace keel

#endif  // KEEL_SINGLE_FLIGHT_HPP
// ---- end keel/single_flight.hpp ----------------------------------------
// ---- begin keel/timeout.hpp --------------------------------------------
// timeout(): give a single operation a time budget.
//
//   auto rows = keel::timeout(keel::Duration{500}, [&] { return db.query("..."); });
//
// If the operation finishes within the limit, its result is returned (or its
// exception rethrown). Otherwise timeout_error is thrown.
//
// C++ cannot forcibly stop a running function, so on timeout the operation
// keeps running in the background until it returns on its own. Two things
// make that manageable:
//
//   * Cooperative cancellation. If your function takes a keel::CancelToken, the
//     token is cancelled the moment the deadline passes, so long-running work
//     can check it and bail out early:
//
//       keel::timeout(keel::Duration{500}, [](keel::CancelToken token) {
//           for (auto& chunk : chunks) {
//               if (token.cancelled()) return partial;
//               process(chunk);
//           }
//           return all;
//       });
//
//   * Your own executor. By default every call gets a fresh detached thread.
//     Pass an Executor to run the work on a thread pool you already have.
//
// Whatever runs past the deadline must not touch caller-local objects that may
// be gone by then. Prefer an operation's native deadline (a socket or query
// timeout) when it has one.
#ifndef KEEL_TIMEOUT_HPP
#define KEEL_TIMEOUT_HPP

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>


namespace keel {

// Thrown by timeout() when the operation exceeds its budget.
class timeout_error : public error {
public:
    explicit timeout_error(Duration limit = Duration{0})
        : error("keel: operation timed out after " + std::to_string(limit.count()) + "ms"),
          limit_(limit) {}

    // The limit that was exceeded.
    Duration limit() const noexcept { return limit_; }

private:
    Duration limit_;
};

// A cancellation flag shared by all of its copies. Cheap to copy.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    bool cancelled() const noexcept { return flag_->load(std::memory_order_acquire); }
    void cancel() noexcept { flag_->store(true, std::memory_order_release); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// Runs a unit of work somewhere else, e.g. by posting it to a thread pool.
// The work must eventually run exactly once; if it is dropped, the caller just
// sees a timeout.
using Executor = std::function<void(std::function<void()>)>;

// The default: a new detached thread per call.
inline void default_executor(std::function<void()> work) {
    std::thread(std::move(work)).detach();
}

namespace detail {

template <class Fn, class = void>
struct timeout_result {
    using type = std::invoke_result_t<Fn&>;
};

template <class Fn>
struct timeout_result<Fn, std::enable_if_t<std::is_invocable_v<Fn&, CancelToken>>> {
    using type = std::invoke_result_t<Fn&, CancelToken>;
};

// Starts `task` on the executor and waits up to `limit` for it. The task is
// kept alive by a shared_ptr, so nothing the background work needs is
// destroyed if we stop waiting.
template <class Result, class OnTimeout>
Result run_with_deadline(Duration limit, const std::shared_ptr<std::packaged_task<Result()>>& task,
                         const Executor& executor, OnTimeout on_timeout) {
    std::future<Result> future = task->get_future();

    // Captured by copy rather than moved: the cost is one reference-count bump,
    // and it keeps static analyzers able to follow who owns the task.
    std::function<void()> job = [task] { (*task)(); };
    if (executor)
        executor(std::move(job));
    else
        default_executor(std::move(job));

    if (future.wait_for(limit) != std::future_status::ready) {
        on_timeout();
        throw timeout_error(limit);
    }
    return future.get();
}

}  // namespace detail

template <class Fn>
auto timeout(Duration limit, Fn&& fn, const Executor& executor = {})
    -> typename detail::timeout_result<std::decay_t<Fn>>::type {
    using Work = std::decay_t<Fn>;
    using Result = typename detail::timeout_result<Work>::type;

    // The task owns its own copy of `fn`. A CancelToken (one more allocation)
    // is only created for functions that actually take one.
    if constexpr (std::is_invocable_v<Work&, CancelToken>) {
        CancelToken token;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            [work = Work(std::forward<Fn>(fn)), token]() mutable -> Result { return work(token); });
        return detail::run_with_deadline<Result>(limit, task, executor,
                                                 [&token] { token.cancel(); });
    } else {
        auto task = std::make_shared<std::packaged_task<Result()>>(Work(std::forward<Fn>(fn)));
        return detail::run_with_deadline<Result>(limit, task, executor, [] {});
    }
}

}  // namespace keel

#endif  // KEEL_TIMEOUT_HPP
// ---- end keel/timeout.hpp ----------------------------------------------

#endif  // KEEL_KEEL_HPP
