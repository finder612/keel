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

#include "keel/common.hpp"

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
