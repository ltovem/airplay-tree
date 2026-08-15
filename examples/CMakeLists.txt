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
    add_executable(airplay2_mac_mirror macos_mirror_demo.mm)
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
endif()
