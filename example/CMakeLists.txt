cmake_minimum_required(VERSION 3.16)
project(video_transmission_examples LANGUAGES CXX)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

find_package(yaml-cpp REQUIRED)

add_executable(video_transmission_sky_reference reference/sky.cpp reference/config.cpp)
add_executable(video_transmission_ground_reference reference/ground.cpp reference/config.cpp)
target_link_libraries(video_transmission_sky_reference PRIVATE video_transmission::video_transmission yaml-cpp)
target_link_libraries(video_transmission_ground_reference PRIVATE video_transmission::video_transmission yaml-cpp)
if(VIDEO_TRANSMISSION_INSTALL_EXAMPLES)
    install(TARGETS video_transmission_sky_reference video_transmission_ground_reference RUNTIME DESTINATION bin)
    install(FILES config/reference_sky.yaml config/reference_ground.yaml DESTINATION share/video_transmission/config)
endif()

# Regenerate this directory's CTest manifest even when the optional pipeline
# is disabled; otherwise an existing build tree can retain legacy tests.
include(CTest)
if(NOT VIDEO_TRANSMISSION_BUILD_PIPELINE)
    return()
endif()

option(VIDEO_TRANSMISSION_BUILD_SKY_EXAMPLE "Build native camera capture example" ON)
if(VIDEO_TRANSMISSION_BUILD_SKY_EXAMPLE)
    add_executable(video_transmission_sky
        native/sky/main.cpp
        adapters/example_config.cpp
        adapters/camera_adapter.cpp
    )
    target_link_libraries(video_transmission_sky PRIVATE
        video_transmission::pipeline
        yaml-cpp
    )
    if(VIDEO_TRANSMISSION_INSTALL_EXAMPLES)
        install(TARGETS video_transmission_sky RUNTIME DESTINATION bin)
    endif()
endif()

add_executable(video_transmission_ground
    native/ground/main.cpp
    adapters/example_config.cpp
)
target_link_libraries(video_transmission_ground PRIVATE video_transmission::pipeline yaml-cpp)
target_compile_features(video_transmission_ground PRIVATE cxx_std_17)
if(VIDEO_TRANSMISSION_INSTALL_EXAMPLES)
    install(TARGETS video_transmission_ground RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
    add_executable(video_transmission_pts_playout_test native/ground/pts_playout_test.cpp)
    target_compile_features(video_transmission_pts_playout_test PRIVATE cxx_std_17)
    target_compile_options(video_transmission_pts_playout_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME video_transmission_pts_playout COMMAND video_transmission_pts_playout_test)
    add_test(NAME video_transmission_playout_cli COMMAND ${CMAKE_COMMAND}
        -DVIDEO_TRANSMISSION_GROUND=$<TARGET_FILE:video_transmission_ground>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/native/ground/pts_playout_cli_test.cmake)
    set_tests_properties(video_transmission_pts_playout video_transmission_playout_cli PROPERTIES TIMEOUT 10)
endif()

option(VIDEO_TRANSMISSION_BUILD_GUI "Enable SDL2 realtime ground preview when available" ON)
set(VIDEO_TRANSMISSION_HAS_GUI 0)
if(VIDEO_TRANSMISSION_BUILD_GUI)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(VIDEO_TRANSMISSION_SDL2 QUIET IMPORTED_TARGET sdl2>=2.0.16)
    endif()
    if(TARGET PkgConfig::VIDEO_TRANSMISSION_SDL2)
        set(VIDEO_TRANSMISSION_HAS_GUI 1)
        target_link_libraries(video_transmission_ground PRIVATE PkgConfig::VIDEO_TRANSMISSION_SDL2)
    else()
        message(WARNING "SDL2 >= 2.0.16 not found; video_transmission_ground --gui will be unavailable")
    endif()
endif()
target_compile_definitions(video_transmission_ground PRIVATE
    VIDEO_TRANSMISSION_HAS_GUI=${VIDEO_TRANSMISSION_HAS_GUI})


option(VIDEO_TRANSMISSION_BUILD_LATENCY_VIEWER "Build SDL/Wayland hardware-decoded measurement viewer" OFF)
if(VIDEO_TRANSMISSION_BUILD_LATENCY_VIEWER)
    enable_language(C)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(PRESENTATION_XML
        "${WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml"
    )
    set(PRESENTATION_HEADER
        "${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.h"
    )
    set(PRESENTATION_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/presentation-time-protocol.c")
    add_custom_command(
        OUTPUT "${PRESENTATION_HEADER}" "${PRESENTATION_SOURCE}"
        COMMAND "${WAYLAND_SCANNER}" client-header
            "${PRESENTATION_XML}" "${PRESENTATION_HEADER}"
        COMMAND "${WAYLAND_SCANNER}" private-code
            "${PRESENTATION_XML}" "${PRESENTATION_SOURCE}"
        DEPENDS "${PRESENTATION_XML}"
        VERBATIM
    )
    add_executable(video_transmission_ground_latency
        native/ground/latency_viewer.cpp
        adapters/example_config.cpp
        "${PRESENTATION_SOURCE}"
        "${PRESENTATION_HEADER}"
    )
    target_include_directories(video_transmission_ground_latency PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(video_transmission_ground_latency PRIVATE
        video_transmission::pipeline
        yaml-cpp
        PkgConfig::SDL2
        PkgConfig::WAYLAND
    )
endif()
