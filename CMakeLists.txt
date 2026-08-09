cmake_minimum_required(VERSION 3.20)
project(Battlezone15Shim LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Battlezone 1.5 Shim currently requires MSVC.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Battlezone 1.5 is 32-bit. Configure with: cmake -S . -B build -A Win32")
endif()

add_library(winmm SHARED
    src/dllmain.cpp
    src/fullscreen_fix.cpp
    src/movie_fix.cpp
    src/shim_log.cpp
    src/winmm_proxy.cpp
    src/winmm.def
)

target_compile_features(winmm PRIVATE cxx_std_17)
target_compile_definitions(winmm PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(winmm PRIVATE src)
target_link_libraries(winmm PRIVATE version)

set_target_properties(winmm PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
)
