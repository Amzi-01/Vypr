cmake_minimum_required(VERSION 3.20)
project(sash-agent CXX)

# C++20 for std::atomic_ref, which is what lets publisher.cpp use the shared C
# layout without turning its fields into std::atomic members.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR
        "sash-agent needs MSVC. C++/WinRT's generated headers and the WGC interop "
        "headers are not usable from mingw, so cross-compiling from Linux is not "
        "an option here - build inside the guest.")
endif()

add_executable(sash-agent
    src/main.cpp
    src/audio.cpp
    src/capture.cpp
    src/control.cpp
    src/input.cpp
    src/ivshmem.cpp
    src/publisher.cpp
    src/windows_list.cpp
)

target_include_directories(sash-agent PRIVATE src ../include)

# /await and the WinRT headers come from the Windows SDK; no vcpkg needed.
target_compile_definitions(sash-agent PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(sash-agent PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus /bigobj)

target_link_libraries(sash-agent PRIVATE
    d3d11 dxgi windowsapp setupapi ws2_32 dwmapi user32 gdi32
)
