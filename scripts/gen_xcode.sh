#!/usr/bin/env bash
# =============================================================================
# gen_xcode.sh — 依据 CMakeLists.txt 重新生成 Xcode 工程（build-xcode）
#
# 用途：CMakeLists.txt 增删源文件/改链接后，Xcode 工程不会自动更新，
#       必须重新 configure。本脚本完成「同步最新源码 → 生成 .xcodeproj →
#       构建 Debug」全流程，保证 Xcode 里点 Run 跑的与 cmake 构建同一份代码。
#
# 用法：./scripts/gen_xcode.sh          （默认生成 + 构建 Debug）
#       ./scripts/gen_xcode.sh --no-build （只生成工程，不构建）
#       ./scripts/gen_xcode.sh --config Release （构建 Release）
# =============================================================================
set -euo pipefail

# 切到项目根目录（脚本位于 scripts/ 下）
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
BUILD_DIR="${ROOT}/build-xcode"

CONFIG="Debug"
DO_BUILD=true
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build)   DO_BUILD=false ;;
        --config)     CONFIG="${2:-Debug}"; shift ;;
        --config=*)   CONFIG="${1#--config=}" ;;
        *)            echo "未知参数: $1"; exit 1 ;;
    esac
    shift
done

echo "==> 项目根目录: $ROOT"
echo "==> 生成 Xcode 工程: $BUILD_DIR  (CMakeLists.txt: $ROOT/CMakeLists.txt)"

# 重新 configure：CMake 会对比 CMakeLists.txt 与工程文件，
# 自动把新增/删除的源文件同步进 .xcodeproj。
# --fresh 强制全量重生成，避免陈旧缓存导致文件列表不同步。
cmake -S "$ROOT" -B "$BUILD_DIR" -G Xcode --fresh \
    -DCMAKE_BUILD_TYPE=Release \
    -DAIRPLAY2_BUILD_EXAMPLES=ON \
    -DAIRPLAY2_BUILD_TESTS=ON \
    -DAIRPLAY2_USE_AVAHI=ON

echo "==> 工程已生成: $BUILD_DIR/airplay2lib.xcodeproj"
echo "==> 校验最新源文件是否同步进工程:"
for f in src/codec/aac_decoder.cpp src/codec/aac_decoder.h \
         src/net/http_server.cpp src/platform/platform_thread.cpp; do
    if grep -q "$(basename "$f")" "$BUILD_DIR/airplay2lib.xcodeproj/project.pbxproj"; then
        echo "    OK  $f"
    else
        echo "    !!  $f 不在 Xcode 工程中（请检查 CMakeLists.txt）"
    fi
done

if [ "$DO_BUILD" = true ]; then
    echo "==> xcodebuild -configuration $CONFIG ..."
    xcodebuild -project "$BUILD_DIR/airplay2lib.xcodeproj" \
        -scheme airplay2_mac_mirror \
        -configuration "$CONFIG" \
        build 2>&1 | tail -5
    echo "==> 完成：产物在 $BUILD_DIR/examples/$CONFIG/airplay2_mac_mirror.app"
else
    echo "==> 跳过构建（--no-build）"
fi
