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
