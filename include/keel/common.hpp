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
#define KEEL_VERSION_MINOR 3
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

// Base class for every error keel itself throws when it refuses or abandons a
// call (circuit_open_error, rate_limited_error, timeout_error). Catch this to
// handle "keel said no" in one place, separately from your own errors.
class error : public std::runtime_error {
public:
    explicit error(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace keel

#endif  // KEEL_COMMON_HPP
