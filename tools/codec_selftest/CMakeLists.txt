cmake_minimum_required(VERSION 3.20)
project(codec_selftest LANGUAGES C CXX)

# Builds the shim's codec layer and decoder verbatim, so a pass here exercises
# the same code the game loads.
add_executable(codec_selftest
    main.cpp
    log_stub.cpp
    ../../src/video_codec_shim.cpp
    ../../src/indeo/ffcompat.c
    ../../src/indeo/indeo_decode.c
    ../../src/indeo/indeo5.c
    ../../src/indeo/ivi.c
    ../../src/indeo/ivi_dsp.c
)

target_include_directories(codec_selftest PRIVATE ../../src ../../src/indeo)
target_compile_definitions(codec_selftest PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_features(codec_selftest PRIVATE cxx_std_17)
target_link_libraries(codec_selftest PRIVATE vfw32 winmm)
