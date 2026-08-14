cmake_minimum_required(VERSION 3.15)
project(airplay2lib VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 包含平台检测模块
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Platform)

# 编译选项
if(MSVC)
    add_compile_options(/W4 /WX-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
    else()
        add_compile_options(-O2)
    endif()
endif()

# 平台特定定义
if(AP2_WINDOWS)
    add_compile_definitions(AP2_PLATFORM_WINDOWS=1)
elseif(AP2_MACOS)
    add_compile_definitions(AP2_PLATFORM_MACOS=1)
elseif(AP2_IOS)
    add_compile_definitions(AP2_PLATFORM_IOS=1)
elseif(AP2_ANDROID)
    add_compile_definitions(AP2_PLATFORM_ANDROID=1)
elseif(AP2_LINUX)
    add_compile_definitions(AP2_PLATFORM_LINUX=1)
endif()

# 源文件收集
set(AIRPLAY2_PUBLIC_HEADERS
    include/airplay2/airplay2.h
    include/airplay2/airplay_server.h
    include/airplay2/airplay_session.h
    include/airplay2/airplay_config.h
    include/airplay2/audio_renderer.h
    include/airplay2/version.h
)

set(AIRPLAY2_SOURCES
    src/airplay2.cpp
    src/platform/platform_socket.cpp
    src/platform/platform_thread.cpp
    src/platform/platform_time.cpp
    src/mdns/mdns_publisher.cpp
    src/mdns/mdns_browser.cpp
    src/net/http_parser.cpp
    src/net/http_server.cpp
    src/net/rtsp_server.cpp
    src/net/rtp_receiver.cpp
    src/codec/alac_decoder.cpp
    src/codec/audio_buffer.cpp
    src/core/airplay_server_impl.cpp
    src/core/airplay_session_impl.cpp
    src/core/airplay_pairing.cpp
)

# 创建库目标
add_library(airplay2 ${AIRPLAY2_SOURCES})
add_library(airplay2::airplay2 ALIAS airplay2)

target_include_directories(airplay2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 平台链接库
if(AP2_WINDOWS)
    target_link_libraries(airplay2 PRIVATE ws2_32 iphlpapi)
elseif(AP2_MACOS OR AP2_IOS)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(CFNETWORK_LIBRARY CFNetwork)
    find_library(SECURITY_LIBRARY Security)
    find_library(DNSSERVICES_LIBRARY DNSSD)
    target_link_libraries(airplay2 PRIVATE
        ${COREFOUNDATION_LIBRARY}
        ${CFNETWORK_LIBRARY}
        ${SECURITY_LIBRARY}
        pthread
    )
    if(DNSSERVICES_LIBRARY)
        target_link_libraries(airplay2 PRIVATE ${DNSSERVICES_LIBRARY})
    endif()
elseif(AP2_ANDROID)
    target_link_libraries(airplay2 PRIVATE log)
else() # Linux
    target_link_libraries(airplay2 PRIVATE pthread avahi-client avahi-common)
endif()

# 构建示例
option(AIRPLAY2_BUILD_EXAMPLES "Build example applications" ON)
if(AIRPLAY2_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 安装配置
install(TARGETS airplay2
    EXPORT airplay2Targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/airplay2 DESTINATION include)

install(EXPORT airplay2Targets
    FILE airplay2Targets.cmake
    NAMESPACE airplay2::
    DESTINATION lib/cmake/airplay2
)
