# Linux aarch64 (ARM64) cross-compilation toolchain for airplay2lib
#
# Usage:
#   cmake -S . -B build-arm64 \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Linux-aarch64.cmake
#
# 要求本机已安装 aarch64-linux-gnu-{gcc,g++}（Ubuntu: apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu）
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 只在交叉编译阶段告诉代码不要跑示例（示例需要本机执行）
set(AIRPLAY2_BUILD_EXAMPLES OFF CACHE BOOL "Build examples for arm64 cross")
