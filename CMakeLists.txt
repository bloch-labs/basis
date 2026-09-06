cmake_minimum_required(VERSION 3.28)

project(
    Bloch
    LANGUAGES CXX
)

option(
    BLOCH_WARNINGS_AS_ERRORS
    "Treat compiler warnings as errors"
    OFF
)

add_library(bloch_project_warnings INTERFACE)
add_library(bloch::project_warnings ALIAS bloch_project_warnings)

target_compile_options(
    bloch_project_warnings
    INTERFACE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
        "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->"
)

if(BLOCH_WARNINGS_AS_ERRORS)
    target_compile_options(
        bloch_project_warnings
        INTERFACE
            "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>"
            "$<$<CXX_COMPILER_ID:MSVC>:/WX>"
    )
endif()

add_subdirectory(src)

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()