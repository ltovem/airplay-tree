# ============================================================
# tests/CMakeLists.txt
# 单元测试构建：5 个独立可执行文件，每个模块一个，便于并行构建 + 单独定位失败
# ============================================================

# 统一编译选项：对测试也开启 -Wall/-Wextra，保证测试代码自身干净
if(MSVC)
    add_compile_options(/W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)
endif()

# 构建 helper：每个测试可执行文件 = 一个 test_*.cpp + test_main.cpp
# 这样各模块互相独立，链接失败也能定位到具体模块。
set(AP2_TEST_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}                # test_harness.h
    ${CMAKE_SOURCE_DIR}/src                    # 内部 headers (crypto/*, util/*, ...)
    ${CMAKE_SOURCE_DIR}/include                # 公开 airplay2 头
)

# ---------- crypto 测试 ----------
add_executable(test_crypto
    test_main.cpp
    test_crypto.cpp
)
target_include_directories(test_crypto PRIVATE ${AP2_TEST_INCLUDES})
target_link_libraries(test_crypto PRIVATE airplay2)
add_test(NAME Crypto COMMAND test_crypto)

# ---------- util (plist) 测试 ----------
add_executable(test_plist
    test_main.cpp
    test_plist.cpp
)
target_include_directories(test_plist PRIVATE ${AP2_TEST_INCLUDES})
target_link_libraries(test_plist PRIVATE airplay2)
add_test(NAME Plist COMMAND test_plist)

# ---------- codec (AudioBuffer + NAL) 测试 ----------
add_executable(test_codec
    test_main.cpp
    test_codec.cpp
)
target_include_directories(test_codec PRIVATE ${AP2_TEST_INCLUDES})
target_link_libraries(test_codec PRIVATE airplay2)
add_test(NAME Codec COMMAND test_codec)

# ---------- net (HTTP/RTSP parser + RTCP + Timing) 测试 ----------
add_executable(test_net
    test_main.cpp
    test_net.cpp
)
target_include_directories(test_net PRIVATE ${AP2_TEST_INCLUDES})
target_link_libraries(test_net PRIVATE airplay2)
add_test(NAME Net COMMAND test_net)

# ---------- core (Pairing + FairPlay) 测试 ----------
add_executable(test_core
    test_main.cpp
    test_core.cpp
)
target_include_directories(test_core PRIVATE ${AP2_TEST_INCLUDES})
target_link_libraries(test_core PRIVATE airplay2)
add_test(NAME Core COMMAND test_core)
