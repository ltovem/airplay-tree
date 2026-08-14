# Android NDK toolchain file for airplay2lib
#
# Usage:
#   cmake -S . -B build-android \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Android.cmake \
#       -DANDROID_ABI=arm64-v8a \
#       -DANDROID_PLATFORM=android-21 \
#       -DANDROID_NDK=/path/to/ndk
#
# 若使用 GitHub Actions runner 预装的 NDK，可直接调用系统自带 ndk-bundle
# 或通过 ANDROID_NDK_LATEST_HOME 环境变量（由 actions/setup-android / runner 预设）。

if(NOT DEFINED ANDROID_NDK)
    if(DEFINED ENV{ANDROID_NDK_LATEST_HOME})
        set(ANDROID_NDK "$ENV{ANDROID_NDK_LATEST_HOME}" CACHE PATH "Android NDK root (auto-detected)")
    elseif(DEFINED ENV{ANDROID_NDK_HOME})
        set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}" CACHE PATH "Android NDK root (auto-detected)")
    elseif(DEFINED ENV{ANDROID_HOME} AND EXISTS "$ENV{ANDROID_HOME}/ndk-bundle")
        set(ANDROID_NDK "$ENV{ANDROID_HOME}/ndk-bundle" CACHE PATH "Android NDK root (auto-detected)")
    endif()
endif()

# 调用 NDK 自带的官方 CMake 工具链（这是 Android 官方推荐的做法）
if(DEFINED ANDROID_NDK AND EXISTS "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
else()
    message(FATAL_ERROR
        "ANDROID_NDK is not set or does not contain build/cmake/android.toolchain.cmake.\n"
        "Please point -DANDROID_NDK=... at a valid Android NDK installation (>= r21).\n"
        "Env vars inspected: ANDROID_NDK_LATEST_HOME, ANDROID_NDK_HOME, ANDROID_HOME/ndk-bundle.")
endif()

# 偏好静态运行库（减少发布时的 .so 依赖）
set(ANDROID_STL "c++_static" CACHE STRING "Android STL variant")
