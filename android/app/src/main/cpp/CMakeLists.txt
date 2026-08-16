# AirPlay Mirror —— JNI 库 CMake 配置
# 通过 add_subdirectory 把 airplay2lib 源码整体编进来，再链接进 libmirror_jni.so。
cmake_minimum_required(VERSION 3.15)
project(mirror_app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 关闭库自带的示例/测试（Android 上不可构建/不需要）
set(AIRPLAY2_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AIRPLAY2_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# airplay2lib 根目录 = 本文件向上 5 级
set(AIRPLAY2LIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
add_subdirectory("${AIRPLAY2LIB_ROOT}" airplay2lib)

add_library(mirror_jni SHARED mirror_jni.cpp)
target_include_directories(mirror_jni PRIVATE
    "${AIRPLAY2LIB_ROOT}/include")
target_link_libraries(mirror_jni
    airplay2
    log)
