add_executable(airplay2_basic_server basic_server.cpp)
target_link_libraries(airplay2_basic_server PRIVATE airplay2::airplay2)

# Optional browser example: scan for AirPlay devices
add_executable(airplay2_browse browse_devices.cpp)
target_link_libraries(airplay2_browse PRIVATE airplay2::airplay2)
target_include_directories(airplay2_browse PRIVATE ${CMAKE_SOURCE_DIR}/src)

# macOS screen-mirroring receiver demo (Cocoa app, VideoToolbox decode)
if(AP2_MACOS AND NOT AP2_IOS)
    enable_language(OBJCXX)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(VIDEOTOOLBOX_LIBRARY VideoToolbox)
    find_library(COREMEDIA_LIBRARY CoreMedia)
    find_library(AVFOUNDATION_LIBRARY AVFoundation)
    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    # 打包成标准 .app bundle（Finder 可双击运行），需 Info.plist
    add_executable(airplay2_mac_mirror MACOSX_BUNDLE macos_mirror_demo.mm)
    target_link_libraries(airplay2_mac_mirror PRIVATE
        airplay2::airplay2
        ${COCOA_LIBRARY}
        ${VIDEOTOOLBOX_LIBRARY}
        ${COREMEDIA_LIBRARY}
        ${AVFOUNDATION_LIBRARY}
        ${AUDIOTOOLBOX_LIBRARY}
        ${COREVIDEO_LIBRARY}
        ${QUARTZCORE_LIBRARY}
    )
    set_target_properties(airplay2_mac_mirror PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.airplay2.mirror"
        MACOSX_BUNDLE_BUNDLE_NAME "AirPlay Mirror"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
        MACOSX_BUNDLE_ICON_FILE ""
    )
    # Cocoa 应用启用 ARC（自动引用计数），避免窗口/图层对象泄漏
    target_compile_options(airplay2_mac_mirror PRIVATE -fobjc-arc)
endif()
