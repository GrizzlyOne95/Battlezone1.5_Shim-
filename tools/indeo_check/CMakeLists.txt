cmake_minimum_required(VERSION 3.20)
project(indeo_check LANGUAGES C)

# Builds the decoder exactly as the shim does, so a pass here means the shim's
# copy is correct too.
add_executable(indeo_check
    main.c
    ../../src/indeo/ffcompat.c
    ../../src/indeo/indeo_decode.c
    ../../src/indeo/ivi.c
    ../../src/indeo/ivi_dsp.c
    ../../src/indeo/indeo5.c
)

target_include_directories(indeo_check PRIVATE ../../src/indeo)
target_compile_definitions(indeo_check PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
