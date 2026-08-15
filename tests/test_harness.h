/*!
 * @file test_harness.h
 * @brief 轻量级单元测试框架 —— 零依赖、单头文件
 *
 * 为什么不用 Google Test / Catch2？
 *   - 跨平台 CI 中 FetchContent 需要联网，可能因网络/代理/缓存失败
 *   - AirPlay 2 项目本身没有外部依赖，保持"纯本地构建"的风格
 *
 * 用法示例：
 *   #include "test_harness.h"
 *   TEST(MySuite, Factorial) {
 *       EXPECT_EQ(fact(0), 1);
 *       EXPECT_TRUE(fact(3) == 6);
 *   }
 *   TEST_MAIN();  // 在一个 .cpp 中放一次
 */
#ifndef AIRPLAY2_TEST_HARNESS_H
#define AIRPLAY2_TEST_HARNESS_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <chrono>

namespace airplay2test {

/*!
 * @brief 单个测试注册条目
 */
struct TestEntry {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};

/*!
 * @brief 全局测试注册表（单例，避免静态初始化顺序 fiasco）
 */
inline std::vector<TestEntry>& registry() {
    static std::vector<TestEntry> r;
    return r;
}

/*!
 * @brief 失败统计
 */
struct FailureRecord {
    const char* file;
    int line;
    std::string message;
};

inline std::vector<FailureRecord>& failures() {
    static std::vector<FailureRecord> f;
    return f;
}
/*!
 * @brief 失败计数（每个可执行文件全局单例）
 */
inline int& current_failures() {
    static int c = 0;
    return c;
}
/*!
 * @brief 当前执行中的 suite / test 名（断言失败时用来打印）
 */
inline const char*& current_suite_storage() { static const char* s = nullptr; return s; }
inline const char*& current_test_storage()  { static const char* t = nullptr; return t; }
inline const char* current_suite() { return current_suite_storage(); }
inline const char* current_test()  { return current_test_storage(); }
inline void set_current(const char* s, const char* t) {
    current_suite_storage() = s;
    current_test_storage()  = t;
}

/*!
 * @brief 注册辅助：构造时把 fn 推进 registry
 */
struct TestRegistrar {
    TestRegistrar(const char* suite, const char* name, std::function<void()> fn) {
        registry().push_back({suite, name, std::move(fn)});
    }
};

// ---- 断言工具 ----

/*!
 * @brief 记录一次断言失败
 */
inline void record_fail(const char* file, int line, const std::string& msg) {
    failures().push_back({file, line, msg});
    ++current_failures();
    fprintf(stderr, "  ✗ [%s::%s] %s:%d %s\n", current_suite(), current_test(), file, line, msg.c_str());
}

/*!
 * @brief 将任意值格式化为字符串（支持算术类型 + std::string + vector<uint8_t>）
 */
template <typename T>
inline std::string tostr(const T& v) {
    if constexpr (std::is_same_v<T, std::string>) return "\"" + v + "\"";
    else if constexpr (std::is_integral_v<T>) return std::to_string(v);
    else if constexpr (std::is_floating_point_v<T>) return std::to_string(v);
    else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
    else return "<object>";
}
inline std::string tostr(const uint8_t* v, size_t n) {
    char buf[16]; std::string s = "[";
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ", ";
        std::snprintf(buf, sizeof(buf), "0x%02x", v[i]);
        s += buf;
    }
    s += "]";
    return s;
}
inline std::string tostr(const std::vector<uint8_t>& v) { return tostr(v.data(), v.size()); }

/*!
 * @brief 逐字节比较两个二进制区域
 */
inline bool bytes_eq(const void* a, const void* b, size_t n) {
    return std::memcmp(a, b, n) == 0;
}

} // namespace airplay2test

// ---- 宏 ----
#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b)  TEST_CAT2(a, b)

/*!
 * @brief 定义一个测试用例。
 * 用法: TEST(SuiteName, CaseName) { ... }
 */
#define TEST(suite, name)                                                                       \
    static void TEST_CAT(testfn_, TEST_CAT(suite, TEST_CAT(_, name)))();                        \
    static ::airplay2test::TestRegistrar TEST_CAT(reg_, TEST_CAT(suite, TEST_CAT(_, name)))(    \
        #suite, #name, TEST_CAT(testfn_, TEST_CAT(suite, TEST_CAT(_, name))));                 \
    static void TEST_CAT(testfn_, TEST_CAT(suite, TEST_CAT(_, name)))()

/*!
 * @brief 断言辅助：condition == true 才通过
 */
#define EXPECT_TRUE(cond) do {                                                                       \
    if (!(cond)) {                                                                                   \
        ::airplay2test::record_fail(__FILE__, __LINE__, std::string("EXPECT_TRUE: ") + #cond);       \
    }                                                                                                \
} while (0)
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b) do {                                                                              \
    auto&& _a = (a); auto&& _b = (b);                                                                     \
    if (!((_a) == (_b))) {                                                                                \
        std::string msg = "EXPECT_EQ: " #a " == " #b " -> got " + ::airplay2test::tostr(_a) +              \
                          " vs " + ::airplay2test::tostr(_b);                                             \
        ::airplay2test::record_fail(__FILE__, __LINE__, msg);                                             \
    }                                                                                                     \
} while (0)

#define EXPECT_NE(a, b) do {                                                                              \
    auto&& _a = (a); auto&& _b = (b);                                                                     \
    if ((_a) == (_b)) {                                                                                   \
        std::string msg = "EXPECT_NE: " #a " != " #b " -> both equal " + ::airplay2test::tostr(_a);        \
        ::airplay2test::record_fail(__FILE__, __LINE__, msg);                                             \
    }                                                                                                     \
} while (0)

#define EXPECT_LT(a, b) do {                                                                              \
    auto&& _a = (a); auto&& _b = (b);                                                                     \
    if (!((_a) < (_b))) {                                                                                 \
        std::string msg = "EXPECT_LT: " #a " < " #b " -> got " + ::airplay2test::tostr(_a) +              \
                          " vs " + ::airplay2test::tostr(_b);                                             \
        ::airplay2test::record_fail(__FILE__, __LINE__, msg);                                             \
    }                                                                                                     \
} while (0)

#define EXPECT_GT(a, b) do {                                                                              \
    auto&& _a = (a); auto&& _b = (b);                                                                     \
    if (!((_a) > (_b))) {                                                                                 \
        std::string msg = "EXPECT_GT: " #a " > " #b " -> got " + ::airplay2test::tostr(_a) +              \
                          " vs " + ::airplay2test::tostr(_b);                                             \
        ::airplay2test::record_fail(__FILE__, __LINE__, msg);                                             \
    }                                                                                                     \
} while (0)

#define EXPECT_LE(a, b) do {                                                                              \
    auto&& _a = (a); auto&& _b = (b);                                                                     \
    if (!((_a) <= (_b))) {                                                                                \
        std::string msg = "EXPECT_LE: " #a " <= " #b " -> got " + ::airplay2test::tostr(_a) +             \
                          " vs " + ::airplay2test::tostr(_b);                                             \
        ::airplay2test::record_fail(__FILE__, __LINE__, msg);                                             \
    }                                                                                                     \
} while (0)

#define EXPECT_GE(a, b) do {                                                                              \
    auto&& _a = (a); auto&& _b = (b);                                                                     \
    if (!((_a) >= (_b))) {                                                                                \
        std::string msg = "EXPECT_GE: " #a " >= " #b " -> got " + ::airplay2test::tostr(_a) +             \
                          " vs " + ::airplay2test::tostr(_b);                                             \
        ::airplay2test::record_fail(__FILE__, __LINE__, msg);                                             \
    }                                                                                                     \
} while (0)

#define EXPECT_STREQ(a, b) do {                                                                           \
    std::string _sa(a); std::string _sb(b);                                                               \
    if (_sa != _sb) {                                                                                     \
        std::string msg = std::string("EXPECT_STREQ: \"") + _sa + "\" == \"" + _sb + "\"";                \
        ::airplay2test::record_fail(__FILE__, __LINE__, msg);                                             \
    }                                                                                                     \
} while (0)

#define EXPECT_BYTES_EQ(a, b, n) do {                                                                     \
    const void* _a = (a); const void* _b = (b); size_t _n = (n);                                          \
    if (!::airplay2test::bytes_eq(_a, _b, _n)) {                                                          \
        std::string msg = "EXPECT_BYTES_EQ: " #a " == " #b " (len " + std::to_string(_n) + ")";           \
        ::airplay2test::record_fail(__FILE__, __LINE__, msg);                                             \
    }                                                                                                     \
} while (0)

/*!
 * @brief 放一个测试可执行文件的 main(). 必须且仅在一个 .cpp 中调用一次.
 * @return 0 全部通过, 非零为失败用例数
 */
#define TEST_MAIN()                                                                                       \
    int main(int argc, char** argv) {                                                                     \
        (void)argc; (void)argv;                                                                           \
        int total_failed = 0;                                                                             \
        int total_passed = 0;                                                                             \
        std::string filter_suite;                                                                         \
        std::string filter_test;                                                                          \
        if (argc >= 2) filter_suite = argv[1];                                                            \
        if (argc >= 3) filter_test  = argv[2];                                                            \
        auto t0 = std::chrono::high_resolution_clock::now();                                              \
        for (auto& t : ::airplay2test::registry()) {                                                      \
            if (!filter_suite.empty() && filter_suite != t.suite) continue;                               \
            if (!filter_test.empty()  && filter_test  != t.name)  continue;                               \
            ::airplay2test::set_current(t.suite, t.name);                                                 \
            int before = ::airplay2test::current_failures();                                              \
            printf("  RUN  %s::%s ... ", t.suite, t.name);                                                \
            fflush(stdout);                                                                               \
            try {                                                                                         \
                t.fn();                                                                                   \
            } catch (const std::exception& e) {                                                           \
                fprintf(stderr, "\n  EXCEPTION in %s::%s: %s\n", t.suite, t.name, e.what());              \
                ::airplay2test::record_fail(__FILE__, __LINE__, std::string("exception: ") + e.what());   \
            } catch (...) {                                                                               \
                fprintf(stderr, "\n  EXCEPTION in %s::%s: unknown\n", t.suite, t.name);                   \
                ::airplay2test::record_fail(__FILE__, __LINE__, "unknown exception");                     \
            }                                                                                             \
            int after = ::airplay2test::current_failures();                                               \
            if (after == before) { printf("OK\n"); ++total_passed; }                                      \
            else { printf("FAILED\n"); ++total_failed; }                                                  \
        }                                                                                                 \
        auto t1 = std::chrono::high_resolution_clock::now();                                              \
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();                           \
        printf("\n=== Test Summary: %d passed, %d failed (%.1f ms) ===\n", total_passed, total_failed, ms);\
        return total_failed;                                                                              \
    }

#endif // AIRPLAY2_TEST_HARNESS_H
