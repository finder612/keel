// CircuitBreaker: stop hammering a failing dependency.
//
// A breaker has three states:
//
//   Closed    calls pass through; consecutive failures are counted. After
//             `failure_threshold` of them the breaker trips to Open.
//   Open      calls fail fast with circuit_open_error, without touching the
//             dependency. After `reset_timeout` elapses the breaker moves to
//             HalfOpen.
//   HalfOpen  a limited number of trial calls are allowed. `success_threshold`
//             consecutive successes close the breaker; any failure re-opens it.
//
//   keel::CircuitBreaker cb{keel::CircuitConfig{}
//                             .with_failure_threshold(5)
//                             .with_reset_timeout(keel::Duration{30000})};
//   auto value = cb.execute([&] { return call_service(); });
//
// The breaker is thread-safe. The clock is injectable for deterministic tests.
#ifndef KEEL_CIRCUIT_BREAKER_HPP
#define KEEL_CIRCUIT_BREAKER_HPP

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "keel/common.hpp"

namespace keel {

enum class CircuitState { Closed, Open, HalfOpen };

// Thrown by CircuitBreaker::execute when the breaker is Open.
class circuit_open_error : public std::runtime_error {
public:
    circuit_open_error() : std::runtime_error("keel: circuit is open") {}
};

struct CircuitConfig {
    // Consecutive failures in Closed before tripping to Open.
    std::size_t failure_threshold = 5;
    // Consecutive successes in HalfOpen before closing again.
    std::size_t success_threshold = 1;
    // How long to stay Open before allowing trial calls (HalfOpen).
    Duration reset_timeout = Duration{30000};

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
};

class CircuitBreaker {
public:
    explicit CircuitBreaker(CircuitConfig config = {}, ClockFn clock = {})
        : config_(config), now_(clock ? std::move(clock) : ClockFn{&default_now}) {}

    // The current logical state. Reading it may move an elapsed Open breaker to
    // HalfOpen, so it is not a pure observer.
    CircuitState state() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return eval_state();
    }

    std::size_t consecutive_failures() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return failures_;
    }

    // Run `fn` through the breaker. Fails fast with circuit_open_error when
    // Open; otherwise records the outcome and rethrows any exception from `fn`.
    template <class Fn>
    auto execute(Fn&& fn) -> decltype(fn()) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (eval_state() == CircuitState::Open) throw circuit_open_error{};
        }

        using Result = decltype(fn());
        if constexpr (std::is_void_v<Result>) {
            try {
                fn();
            } catch (...) {
                record_failure();
                throw;
            }
            record_success();
        } else {
            try {
                Result value = fn();
                record_success();
                return value;
            } catch (...) {
                record_failure();
                throw;
            }
        }
    }

private:
    // Advances Open -> HalfOpen when the reset timeout has elapsed. Caller must
    // hold the lock.
    CircuitState eval_state() const {
        if (state_ == CircuitState::Open &&
            (now_() - opened_at_) >= config_.reset_timeout) {
            state_ = CircuitState::HalfOpen;
            half_open_successes_ = 0;
        }
        return state_;
    }

    void record_success() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (eval_state() == CircuitState::HalfOpen) {
            if (++half_open_successes_ >= config_.success_threshold) {
                state_ = CircuitState::Closed;
                failures_ = 0;
                half_open_successes_ = 0;
            }
        } else {
            failures_ = 0;
        }
    }

    void record_failure() {
        std::lock_guard<std::mutex> guard(mutex_);
        const CircuitState s = eval_state();
        if (s == CircuitState::HalfOpen) {
            trip();
        } else if (s == CircuitState::Closed) {
            if (++failures_ >= config_.failure_threshold) trip();
        }
    }

    // Caller must hold the lock.
    void trip() {
        state_ = CircuitState::Open;
        opened_at_ = now_();
        half_open_successes_ = 0;
    }

    CircuitConfig config_;
    ClockFn now_;

    mutable std::mutex mutex_;
    mutable CircuitState state_ = CircuitState::Closed;
    mutable std::size_t failures_ = 0;
    mutable std::size_t half_open_successes_ = 0;
    mutable TimePoint opened_at_{};
};

}  // namespace keel

#endif  // KEEL_CIRCUIT_BREAKER_HPP
