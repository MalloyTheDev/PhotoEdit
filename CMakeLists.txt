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
