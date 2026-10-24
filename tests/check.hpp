// A tiny, dependency-free test harness for the keel test suite.
//
// Register a case with TEST("name") { ... } and assert with CHECK(expr).
// Running the resulting binary executes every registered case and prints a
// summary; the process exits non-zero if anything failed (so CI catches it).
#ifndef KEEL_TESTS_CHECK_HPP
#define KEEL_TESTS_CHECK_HPP

#include <cstdio>
#include <vector>

namespace check_detail {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

inline const char*& current_case() {
    static const char* name = "";
    return name;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report(const char* expr, const char* file, int line) {
    ++failures();
    std::printf("  FAIL [%s]  %s\n        at %s:%d\n", current_case(), expr, file,
                line);
}

}  // namespace check_detail

#define KEEL_CONCAT_(a, b) a##b
#define KEEL_CONCAT(a, b) KEEL_CONCAT_(a, b)

#define TEST(name)                                                        \
    static void KEEL_CONCAT(keel_test_, __LINE__)();                      \
    static ::check_detail::Registrar KEEL_CONCAT(keel_reg_, __LINE__)(    \
        name, &KEEL_CONCAT(keel_test_, __LINE__));                        \
    static void KEEL_CONCAT(keel_test_, __LINE__)()

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) ::check_detail::report(#expr, __FILE__, __LINE__);   \
    } while (0)

inline int main_impl() {
    using namespace check_detail;
    int total = static_cast<int>(registry().size());
    for (auto& c : registry()) {
        current_case() = c.name;
        c.fn();
    }
    if (failures() == 0) {
        std::printf("OK: all %d test case(s) passed\n", total);
        return 0;
    }
    std::printf("FAILED: %d check(s) failed across %d case(s)\n", failures(), total);
    return 1;
}

int main() { return main_impl(); }

#endif  // KEEL_TESTS_CHECK_HPP
