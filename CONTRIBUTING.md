# Contributing to keel

Thanks for your interest! keel is intentionally small and focused on a few
well-understood resilience patterns. Fixes, tests, docs, portability work, and
the items on the roadmap are all welcome.

## Getting started

From the root of your clone:

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++17 compiler (GCC, Clang or MSVC).

## Guidelines

- **Keep it header-only and dependency-free** (standard library + threads
  only). The whole library is under `include/keel/`; the test harness in
  `tests/check.hpp` is dependency-free on purpose too.
- **Keep time injectable.** Anything that sleeps, reads the clock, or uses
  randomness must accept an override (`SleepFn`, `ClockFn`, `RandomFn`) so it
  can be tested deterministically. New features follow the same rule.
- **Add deterministic tests.** Every behavior change needs a case in
  `tests/tests.cpp` that passes on GCC, Clang and MSVC and does not rely on
  real wall-clock time.
- **Match the style.** Run `clang-format` with the repo's `.clang-format`
  before committing.
- **Explain the "why".** New components should come with a short description of
  the failure mode they address.

## Reporting bugs

Open an issue with a minimal reproduction, the compiler and version, and what
you expected vs. what happened.

## Code of conduct

By participating you agree to abide by the [Code of Conduct](CODE_OF_CONDUCT.md).
