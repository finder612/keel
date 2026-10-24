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

#include "keel/common.hpp"

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
