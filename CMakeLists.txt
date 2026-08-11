cmake_minimum_required(VERSION 3.20)
project(Battlezone15Shim LANGUAGES C CXX)

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
    src/movie_geometry_probe.cpp
    src/movie_present_fix.cpp
    src/shim_log.cpp
    src/video_codec_shim.cpp
    src/winmm_proxy.cpp
    src/winmm.def

    # Indeo Video 5 decoder (FFmpeg, LGPL 2.1+) -- see src/indeo/README.md
    src/indeo_log_bridge.cpp
    src/indeo/ffcompat.c
    src/indeo/indeo_decode.c
    src/indeo/indeo5.c
    src/indeo/ivi.c
    src/indeo/ivi_dsp.c
)

target_compile_features(winmm PRIVATE cxx_std_17)
target_compile_definitions(winmm PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(winmm PRIVATE src)
target_link_libraries(winmm PRIVATE version dwmapi vfw32)

set_target_properties(winmm PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
)
