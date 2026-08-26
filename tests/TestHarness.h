#pragma once
// Deliberately dependency-free. Swap for doctest or Catch2 when the suite
// grows; keeping it plain means `cmake --build . && ctest` works offline on a
// fresh clone with nothing to download.
#include <cmath>
#include <cstdio>
#include <cstdlib>

inline int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
    do {                                                                         \
        const double va = (a), vb = (b);                                         \
        if (std::fabs(va - vb) > (tol)) {                                        \
            std::printf("  FAIL %s:%d  %s (%g) != %s (%g)\n",                    \
                        __FILE__, __LINE__, #a, va, #b, vb);                     \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define RUN(fn)                                                                  \
    do { std::printf("[ run ] %s\n", #fn); fn(); } while (0)

#define TEST_MAIN_END                                                            \
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }     \
    std::printf("all passed\n");                                                 \
    return 0;
