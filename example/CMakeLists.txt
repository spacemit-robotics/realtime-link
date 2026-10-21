cmake_minimum_required(VERSION 3.16)
project(fpv_video_example LANGUAGES CXX)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

if(NOT TARGET video_transmission::fpv_pipeline)
    find_package(video_transmission CONFIG REQUIRED COMPONENTS pipeline)
endif()
find_package(yaml-cpp REQUIRED)

option(FPV_BUILD_SKY_EXAMPLE "Build native camera capture example" ON)
if(FPV_BUILD_SKY_EXAMPLE)
    add_executable(fpv_sky
        native/sky/main.cpp
        adapters/example_config.cpp
        adapters/camera_adapter.cpp
    )
    target_link_libraries(fpv_sky PRIVATE
        video_transmission::fpv_pipeline
        yaml-cpp
    )
    install(TARGETS fpv_sky RUNTIME DESTINATION bin)
endif()

add_executable(fpv_ground
    native/ground/main.cpp
    adapters/example_config.cpp
)
target_link_libraries(fpv_ground PRIVATE video_transmission::fpv_pipeline yaml-cpp)
target_compile_features(fpv_ground PRIVATE cxx_std_17)

include(CTest)
if(BUILD_TESTING)
    add_executable(fpv_pts_playout_test native/ground/pts_playout_test.cpp)
    target_compile_features(fpv_pts_playout_test PRIVATE cxx_std_17)
    target_compile_options(fpv_pts_playout_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME fpv_pts_playout COMMAND fpv_pts_playout_test)
    add_test(NAME fpv_playout_cli COMMAND ${CMAKE_COMMAND}
        -DFPV_GROUND=$<TARGET_FILE:fpv_ground>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/native/ground/pts_playout_cli_test.cmake)
    set_tests_properties(fpv_pts_playout fpv_playout_cli PROPERTIES TIMEOUT 10)
endif()

option(FPV_BUILD_GUI "Enable SDL2 realtime ground preview when available" ON)
if(FPV_BUILD_GUI)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FPV_SDL2 QUIET IMPORTED_TARGET sdl2>=2.0.16)
    endif()
    if(TARGET PkgConfig::FPV_SDL2)
        target_compile_definitions(fpv_ground PRIVATE FPV_HAS_GUI=1)
        target_link_libraries(fpv_ground PRIVATE PkgConfig::FPV_SDL2)
    else()
        message(WARNING "SDL2 >= 2.0.16 not found; fpv_ground --gui will be unavailable")
    endif()
endif()

install(TARGETS fpv_ground RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/fpv_video/config
    PATTERN "preview-ground.sh" EXCLUDE
)
install(PROGRAMS config/sites/oyang_k3/preview-ground.sh
    DESTINATION share/fpv_video/config/sites/oyang_k3
)
install(PROGRAMS tools/clock_probe.py tools/summarize_latency.py
    DESTINATION share/fpv_video/tools
)
install(DIRECTORY docs/ DESTINATION share/fpv_video/docs)
install(FILES README.md DESTINATION share/fpv_video)

option(FPV_BUILD_LATENCY_VIEWER "Build SDL/Wayland hardware-decoded measurement viewer" OFF)
if(FPV_BUILD_LATENCY_VIEWER)
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
    add_executable(fpv_ground_latency
        native/ground/latency_viewer.cpp
        adapters/example_config.cpp
        "${PRESENTATION_SOURCE}"
        "${PRESENTATION_HEADER}"
    )
    target_include_directories(fpv_ground_latency PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(fpv_ground_latency PRIVATE
        video_transmission::fpv_pipeline
        yaml-cpp
        PkgConfig::SDL2
        PkgConfig::WAYLAND
    )
    install(TARGETS fpv_ground_latency RUNTIME DESTINATION bin)
endif()
