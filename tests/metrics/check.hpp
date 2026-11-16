#pragma once
// Minimal test helpers for tests/metrics (no external framework, works offline).
// CHECK is used instead of assert() so tests still run in builds that define NDEBUG.
//
//   void test_something() { CHECK(x == 1); CHECK_NEAR(y, 2.0, 1e-9); }
//   int main() { RUN(test_something); return check::result(); }

#include <cmath>
#include <cstdio>

namespace check {
inline int& failures() {
    static int count = 0;
    return count;
}
inline int result() {
    if (failures() == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d CHECK(S) FAILED\n", failures());
    return 1;
}
}  // namespace check

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++check::failures();                                                          \
        }                                                                                 \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                                      \
    do {                                                                                       \
        const double check_a_ = (actual), check_e_ = (expected);                               \
        if (!(std::fabs(check_a_ - check_e_) <= (tol))) {                                      \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %.6g, expected %.6g +- %g\n", \
                         __FILE__, __LINE__, #actual, check_a_, check_e_, (double)(tol));      \
            ++check::failures();                                                               \
        }                                                                                      \
    } while (0)

#define RUN(test_fn)                                                                              \
    do {                                                                                          \
        const int check_before_ = check::failures();                                              \
        test_fn();                                                                                \
        std::printf("[%s] %s\n", check::failures() == check_before_ ? " OK " : "FAIL", #test_fn); \
    } while (0)
