/*!
 * @file test_main.cpp
 * @brief 单测可执行文件入口：TEST_MAIN() 必须且仅在一个 .cpp 中出现一次。
 *
 * 所有测试用例通过 test_harness.h 的 TEST() 宏分散在 test_*.cpp 中，
 * 在静态初始化阶段自动注册到 registry()。main() 依次执行全部，
 * 按 exit code 返回失败个数 (0 = 全部通过)。
 */
#include "test_harness.h"

TEST_MAIN();
