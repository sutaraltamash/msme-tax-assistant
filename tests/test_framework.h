// test_framework.h — minimal self-contained assert harness for Phase 1.
//
// Deliberately dependency-free: no gtest/gmock. Enough to prove the
// deterministic core (StateMatrix math, RuleEngine triggers, GSTR output)
// with meaningful failure messages, and to run under CTest without any
// framework plumbing.

#pragma once

#include <cstdio>
#include <cstdlib>

namespace testfw {

struct TestCase
{
    const char* name;
    void (*fn)();
};

inline int& checks()   { static int n = 0; return n; }
inline int& failures() { static int n = 0; return n; }

} // namespace testfw

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        ++testfw::checks();                                                    \
        if (!(cond))                                                           \
        {                                                                      \
            ++testfw::failures();                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do                                                                         \
    {                                                                          \
        ++testfw::checks();                                                    \
        auto const va = (a);                                                   \
        auto const vb = (b);                                                   \
        if (!(va == vb))                                                       \
        {                                                                      \
            ++testfw::failures();                                              \
            std::printf("  FAIL %s:%d: %s == %s  (got %lld vs %lld)\n",        \
                        __FILE__, __LINE__, #a, #b,                           \
                        static_cast<long long>(va), static_cast<long long>(vb)); \
        }                                                                      \
    } while (0)

#define CHECK_THROWS(expr, extype)                                             \
    do                                                                         \
    {                                                                          \
        ++testfw::checks();                                                    \
        bool _threw_ = false;                                                  \
        try                                                                     \
        {                                                                      \
            (void)(expr);                                                      \
        }                                                                      \
        catch (const extype&)                                                  \
        {                                                                      \
            _threw_ = true;                                                    \
        }                                                                      \
        catch (...)                                                            \
        {                                                                      \
        }                                                                      \
        if (!_threw_)                                                          \
        {                                                                      \
            ++testfw::failures();                                              \
            std::printf("  FAIL %s:%d: %s did not throw %s\n", __FILE__,       \
                        __LINE__, #expr, #extype);                             \
        }                                                                      \
    } while (0)

#define TEST(name) static void test_##name()
#define RUN_TEST(name)    { #name, &test_##name }
#define TESTS_BEGIN static const testfw::TestCase kTests[] = {
#define TESTS_END                                                           \
    };                                                                       \
    int main()                                                               \
    {                                                                        \
        for (const auto& t : kTests)                                         \
        {                                                                    \
            t.fn();                                                          \
        }                                                                    \
        const int fails = testfw::failures();                                \
        std::printf("\n%d checks, %d failures\n", testfw::checks(), fails);  \
        return fails == 0 ? EXIT_SUCCESS : EXIT_FAILURE;                     \
    }