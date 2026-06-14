#pragma once

// A tiny, zero-dependency test harness. Enough to drive TDD of the engine core
// without pulling in a framework. If we later want richer features (fixtures,
// parameterization), we can swap in Catch2/doctest behind the same PE_TEST API.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace pe_test {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& currentFailures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

inline int runAll() {
    int failedCases = 0;
    for (const auto& c : registry()) {
        const int before = currentFailures();
        std::printf("[ run  ] %s\n", c.name.c_str());
        c.fn();
        if (currentFailures() == before) {
            std::printf("[  ok  ] %s\n", c.name.c_str());
        } else {
            std::printf("[ FAIL ] %s\n", c.name.c_str());
            ++failedCases;
        }
    }
    std::printf("\n%zu case(s), %d failed, %d check failure(s)\n",
                registry().size(), failedCases, currentFailures());
    return failedCases == 0 ? 0 : 1;
}

inline bool nearly(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

} // namespace pe_test

#define PE_TEST(name)                                                        \
    static void name();                                                      \
    static ::pe_test::Registrar pe_reg_##name(#name, name);                  \
    static void name()

// Variadic so that brace-init expressions containing commas
// (e.g. PE_CHECK(Rect{0,0,-1,5}.isEmpty())) pass through as a single condition.
#define PE_CHECK(...)                                                        \
    do {                                                                     \
        if (!(__VA_ARGS__)) {                                                \
            ++::pe_test::currentFailures();                                  \
            std::printf("    CHECK failed: %s  (%s:%d)\n", #__VA_ARGS__,     \
                        __FILE__, __LINE__);                                 \
        }                                                                    \
    } while (0)

#define PE_CHECK_EQ(a, b)                                                    \
    do {                                                                     \
        if (!((a) == (b))) {                                                 \
            ++::pe_test::currentFailures();                                  \
            std::printf("    CHECK_EQ failed: %s == %s  (%s:%d)\n", #a, #b,  \
                        __FILE__, __LINE__);                                 \
        }                                                                    \
    } while (0)

#define PE_CHECK_NEAR(a, b)                                                  \
    do {                                                                     \
        if (!::pe_test::nearly((a), (b))) {                                  \
            ++::pe_test::currentFailures();                                  \
            std::printf("    CHECK_NEAR failed: %s ~= %s  (%s:%d)\n", #a,    \
                        #b, __FILE__, __LINE__);                             \
        }                                                                    \
    } while (0)
