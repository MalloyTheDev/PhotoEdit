cmake_minimum_required(VERSION 3.24)

project(PhotoEdit
    VERSION 0.1.0
    DESCRIPTION "PhotoEdit - a flagship-level, Photoshop-class image editor"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PHOTOEDIT_BUILD_APP   "Build the Qt6 GUI application shell" ON)
option(PHOTOEDIT_BUILD_TESTS "Build unit tests"                    ON)
option(PHOTOEDIT_WERROR      "Treat warnings as errors"            OFF)

# ---------------------------------------------------------------------------
# Warning flags (the engine is held to a strict bar)
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /permissive- /EHsc /Zc:__cplusplus)
    if(PHOTOEDIT_WERROR)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(PHOTOEDIT_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# clang against the MSVC STL: turn off the STL's vectorized algorithms (#197)
#
# MSVC STL 14.51 admits a type into the vectorized std::find path when the
# compiler builtin behind _Is_same_and_builtin_trivially_equality_comparable says
# the type is trivially equality comparable, but _Find_vectorized only implements
# a size dispatch for 1, 2, 4 and 8 byte elements. clang answers that builtin
# `true` for a larger trivially-comparable aggregate where cl.exe does not, so a
# std::find over one lands in the dispatch's else branch and hits
# `static_assert(false, "unexpected size")`. pe::OutlineSegment (two Points, 16
# bytes, defaulted operator==) is exactly that shape, and it took out the whole
# test binary in both clang lanes: the engine library compiled, the tests did not.
#
# _USE_STD_VECTOR_ALGORITHMS is the STL's own opt-out (it honours a user-provided
# value, see xutility), so this selects the scalar implementations rather than
# patching around the upstream defect at each call site. Scalar std::find is
# correct, and nothing here is in a measured hot path.
#
# Scoped deliberately: Clang + a SIMULATE_ID of MSVC is only true when clang is
# targeting the MSVC STL, which is the broken configuration. mingw/GCC, clang
# against libstdc++/libc++ on the CI Linux lanes, and cl.exe itself all keep the
# vectorized algorithms, since none of them reproduce this.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    add_compile_definitions(_USE_STD_VECTOR_ALGORITHMS=0)
endif()

# ---------------------------------------------------------------------------
# Subprojects
#
# The dependency direction is strictly one-way:
#
#     app  ->  core
#
# core has NO dependency on Qt or any UI toolkit, so the entire engine can be
# unit-tested headless. See docs/01-master-architecture.md.
# ---------------------------------------------------------------------------
add_subdirectory(src/core)

if(PHOTOEDIT_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(PHOTOEDIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "PhotoEdit ${PROJECT_VERSION}")
message(STATUS "  Build app:   ${PHOTOEDIT_BUILD_APP}")
message(STATUS "  Build tests: ${PHOTOEDIT_BUILD_TESTS}")
