# Contributing to keel

Thanks for stopping by! keel is meant to stay small, so the most useful
contributions are usually bug fixes, better tests, clearer docs and making it
build in more places. Ideas from the "What's next" list in the README are very
welcome too.

## Building and running the tests

From the root of your clone:

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Any C++17 compiler should do. CI checks GCC, Clang and MSVC.

## A few house rules

Headers only, no dependencies. Everything lives in `include/keel/` and uses
nothing but the standard library. Even the test harness in `tests/check.hpp` is
homemade for this reason.

Don't hard-code time. If your change sleeps, reads the clock or uses randomness,
let callers swap it out (see `SleepFn`, `ClockFn` and `RandomFn`). That's what
keeps the test suite fast and predictable.

Add a test for whatever you change. Tests go in `tests/tests.cpp`, and they
shouldn't depend on real wall-clock time unless there's truly no other way
(timeout is the one exception right now).

Run `clang-format` before you commit. The repo's `.clang-format` has the
settings.

Tell us what problem you're solving. For a new feature, a couple of sentences
about the failure it helps with goes a long way.

## Found a bug?

Open an issue. The most helpful reports include a short snippet that shows the
problem, your compiler and its version, and what you expected to happen versus
what actually happened.

## Code of conduct

Please be kind. Everyone taking part is expected to follow the
[Code of Conduct](CODE_OF_CONDUCT.md).
