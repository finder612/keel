// with_fallback() — run a primary operation, and if it throws, run a fallback.
//
//   // Serve from cache when the database is unreachable.
//   auto user = keel::with_fallback(
//       [&] { return db.load_user(id); },
//       [&] { return cache.load_user(id); });
//
// The fallback must return a type convertible to the primary's return type.
// Only std::exception-derived failures trigger the fallback; anything else
// propagates. Use with_fallback_if to restrict which exceptions are caught.
#ifndef KEEL_FALLBACK_HPP
#define KEEL_FALLBACK_HPP

#include <exception>
#include <utility>

namespace keel {

template <class Primary, class Fallback>
auto with_fallback(Primary&& primary, Fallback&& fallback) -> decltype(primary()) {
    try {
        return primary();
    } catch (const std::exception&) {
        return fallback();
    }
}

template <class Primary, class Fallback, class Filter>
auto with_fallback_if(Primary&& primary, Fallback&& fallback, Filter&& filter)
    -> decltype(primary()) {
    try {
        return primary();
    } catch (const std::exception& e) {
        if (filter(e)) return fallback();
        throw;
    }
}

}  // namespace keel

#endif  // KEEL_FALLBACK_HPP
