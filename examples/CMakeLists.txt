add_executable(airplay2_basic_server basic_server.cpp)
target_link_libraries(airplay2_basic_server PRIVATE airplay2::airplay2)

# Optional browser example: scan for AirPlay devices
add_executable(airplay2_browse browse_devices.cpp)
target_link_libraries(airplay2_browse PRIVATE airplay2::airplay2)
target_include_directories(airplay2_browse PRIVATE ${CMAKE_SOURCE_DIR}/src)
