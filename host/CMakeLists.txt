cmake_minimum_required(VERSION 3.20)
project(vypr C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(SDL3 REQUIRED CONFIG)

add_executable(vypr-window src/main.c src/shm.c src/msg.c src/present.c src/present_gpu.c src/present_render.c src/dragsource.c)
target_include_directories(vypr-window PRIVATE src ../include)
target_link_libraries(vypr-window PRIVATE SDL3::SDL3)

# Offering a drag is per display server, and SDL has no call for it, so each
# backend talks to the server itself. Wayland is optional: without its headers
# the client still builds and simply refuses to drag files out.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
endif()
if(WAYLAND_CLIENT_FOUND)
    target_sources(vypr-window PRIVATE src/dragsource_wayland.c)
    target_compile_definitions(vypr-window PRIVATE VYPR_HAVE_WAYLAND=1)
    target_include_directories(vypr-window PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
    target_link_libraries(vypr-window PRIVATE ${WAYLAND_CLIENT_LIBRARIES})
    message(STATUS "drag source: wayland (${WAYLAND_CLIENT_VERSION})")
else()
    message(STATUS "drag source: none - wayland-client not found")
endif()

# The session daemon: owns the region, the agent link, and which windows stream.
add_executable(vyprd src/vyprd.c src/shm.c src/msg.c)
target_include_directories(vyprd PRIVATE src ../include)

# The stand-in guest. Built alongside the host because it is how the host is
# tested, not an optional extra.
add_executable(vypr-testsrc ../tools/vypr-testsrc.c src/shm.c)
target_include_directories(vypr-testsrc PRIVATE src ../include)
target_link_libraries(vypr-testsrc PRIVATE m)

# The guest agent's publish path, compiled for Linux and run against the host
# client. Proves the frame handoff without a Windows toolchain.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(vypr-testagent ../tools/vypr-testagent.cpp ../guest/src/publisher.cpp src/shm.c src/msg.c)
target_include_directories(vypr-testagent PRIVATE src ../include ../guest/src)
