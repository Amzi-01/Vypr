cmake_minimum_required(VERSION 3.20)
project(sash C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(SDL3 REQUIRED CONFIG)

add_executable(sash-host src/main.c src/shm.c src/msg.c)
target_include_directories(sash-host PRIVATE src ../include)
target_link_libraries(sash-host PRIVATE SDL3::SDL3)

# The session daemon: owns the region, the agent link, and which windows stream.
add_executable(sashd src/sashd.c src/shm.c src/msg.c)
target_include_directories(sashd PRIVATE src ../include)

# The stand-in guest. Built alongside the host because it is how the host is
# tested, not an optional extra.
add_executable(sash-testsrc ../tools/sash-testsrc.c src/shm.c)
target_include_directories(sash-testsrc PRIVATE src ../include)
target_link_libraries(sash-testsrc PRIVATE m)

# The guest agent's publish path, compiled for Linux and run against the host
# client. Proves the frame handoff without a Windows toolchain.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(sash-testagent ../tools/sash-testagent.cpp ../guest/src/publisher.cpp src/shm.c src/msg.c)
target_include_directories(sash-testagent PRIVATE src ../include ../guest/src)
