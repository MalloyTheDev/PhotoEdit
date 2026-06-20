# ---------------------------------------------------------------------------
# pe_core - the headless PhotoEdit engine.
#
# Hard rule: this target MUST NOT link or include Qt, or any GUI toolkit.
# Everything here must be unit-testable without a display. The app shell
# (src/app) depends on this library; never the reverse.
# ---------------------------------------------------------------------------
add_library(pe_core
    src/BlendMode.cpp
    src/PixelBuffer.cpp
    src/Layer.cpp
    src/PixelLayer.cpp
    src/SolidColorLayer.cpp
    src/TextLayer.cpp
    src/GroupLayer.cpp
    src/Compositor.cpp
    src/Document.cpp
    src/History.cpp
    src/Commands.cpp
    src/ViewTransform.cpp
    src/CanvasRenderer.cpp
    src/Brush.cpp
    src/PaintToolController.cpp
    src/Selection.cpp
    src/Mask.cpp
    src/Adjustment.cpp
    src/AdjustmentLayer.cpp
    src/Filter.cpp
    src/Histogram.cpp
    src/Channels.cpp
    src/NativeFormat.cpp
    src/DocumentIO.cpp
    src/Psd.cpp

    src/AutoTone.cpp
    src/ColorSpace.cpp
)

add_library(PhotoEdit::Core ALIAS pe_core)

target_include_directories(pe_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(pe_core PUBLIC cxx_std_20)

set_target_properties(pe_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

# Force every optional library off regardless of what is installed, for a
# deterministic dependency-free build (the CI no-deps lane sets this). Without it,
# auto-detection would still pick up ubiquitous base libraries like zlib that happen
# to be present on the runner, so the "no deps" property could not be guaranteed.
option(PHOTOEDIT_NO_OPTIONAL_DEPS "Build the engine with all optional libraries disabled" OFF)

# ---------------------------------------------------------------------------
# Optional color management (lcms2). Detected gracefully: when Little-CMS 2 is
# available the ICC color module is compiled and PHOTOEDIT_HAVE_LCMS2 is defined;
# otherwise the engine builds without it (callers guard usage with the macro). This
# keeps the dependency-free CI lanes green until lcms2 is provisioned on each.
# See ADR-0004 and docs/systems/15-color-management.md.
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LCMS2 IMPORTED_TARGET lcms2)
endif()
if(NOT LCMS2_FOUND)
    # Fallback for toolchains without pkg-config (e.g. vcpkg): plain find_*.
    find_path(LCMS2_INCLUDE_DIR NAMES lcms2.h)
    find_library(LCMS2_LIBRARY NAMES lcms2 liblcms2)
    if(LCMS2_INCLUDE_DIR AND LCMS2_LIBRARY)
        set(LCMS2_FOUND TRUE)
    endif()
endif()

if(LCMS2_FOUND AND NOT PHOTOEDIT_NO_OPTIONAL_DEPS)
    target_sources(pe_core PRIVATE src/ColorProfile.cpp src/ColorTransform.cpp src/ColorEngine.cpp
                                   src/ColorOps.cpp)
    target_compile_definitions(pe_core PUBLIC PHOTOEDIT_HAVE_LCMS2=1)
    if(TARGET PkgConfig::LCMS2)
        target_link_libraries(pe_core PUBLIC PkgConfig::LCMS2)
    else()
        target_include_directories(pe_core PUBLIC ${LCMS2_INCLUDE_DIR})
        target_link_libraries(pe_core PUBLIC ${LCMS2_LIBRARY})
    endif()
    message(STATUS "  Color mgmt:  lcms2 ${LCMS2_VERSION}")
else()
    message(STATUS "  Color mgmt:  DISABLED (lcms2 not found)")
endif()

# ---------------------------------------------------------------------------
# Optional PNG I/O (libpng). Same graceful detection as lcms2 above: when libpng
# is available the PNG codec is compiled and PHOTOEDIT_HAVE_PNG is defined;
# otherwise the engine builds without it (callers guard with the macro). See
# docs/systems/16-file-formats.md.
# ---------------------------------------------------------------------------
if(PkgConfig_FOUND)
    pkg_check_modules(PNG IMPORTED_TARGET libpng)
endif()
if(NOT PNG_FOUND)
    find_path(PNG_INCLUDE_DIR NAMES png.h)
    find_library(PNG_LIBRARY NAMES png png16 libpng libpng16)
    if(PNG_INCLUDE_DIR AND PNG_LIBRARY)
        set(PNG_FOUND TRUE)
    endif()
endif()

if(PNG_FOUND AND NOT PHOTOEDIT_NO_OPTIONAL_DEPS)
    target_sources(pe_core PRIVATE src/Png.cpp)
    target_compile_definitions(pe_core PUBLIC PHOTOEDIT_HAVE_PNG=1)
    if(TARGET PkgConfig::PNG)
        target_link_libraries(pe_core PUBLIC PkgConfig::PNG)
    else()
        target_include_directories(pe_core PUBLIC ${PNG_INCLUDE_DIR})
        target_link_libraries(pe_core PUBLIC ${PNG_LIBRARY})
    endif()
    message(STATUS "  PNG I/O:     libpng ${PNG_VERSION}")
else()
    message(STATUS "  PNG I/O:     DISABLED (libpng not found)")
endif()

# ---------------------------------------------------------------------------
# Optional JPEG I/O (libjpeg-turbo's TurboJPEG API). TurboJPEG ships no pkg-config
# file, so we detect via find_path/find_library. Same graceful pattern as above.
# ---------------------------------------------------------------------------
find_path(TURBOJPEG_INCLUDE_DIR NAMES turbojpeg.h)
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg libturbojpeg)
if(TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIBRARY AND NOT PHOTOEDIT_NO_OPTIONAL_DEPS)
    target_sources(pe_core PRIVATE src/Jpeg.cpp)
    target_compile_definitions(pe_core PUBLIC PHOTOEDIT_HAVE_JPEG=1)
    target_include_directories(pe_core PUBLIC ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(pe_core PUBLIC ${TURBOJPEG_LIBRARY})
    message(STATUS "  JPEG I/O:    libjpeg-turbo (TurboJPEG)")
else()
    message(STATUS "  JPEG I/O:    DISABLED (libturbojpeg not found)")
endif()

# ---------------------------------------------------------------------------
# Optional TIFF I/O (libtiff). pkg-config module is libtiff-4. Same graceful pattern.
# ---------------------------------------------------------------------------
if(PkgConfig_FOUND)
    pkg_check_modules(TIFF IMPORTED_TARGET libtiff-4)
endif()
if(NOT TIFF_FOUND)
    find_path(TIFF_INCLUDE_DIR NAMES tiffio.h)
    find_library(TIFF_LIBRARY NAMES tiff libtiff)
    if(TIFF_INCLUDE_DIR AND TIFF_LIBRARY)
        set(TIFF_FOUND TRUE)
    endif()
endif()
if(TIFF_FOUND AND NOT PHOTOEDIT_NO_OPTIONAL_DEPS)
    target_sources(pe_core PRIVATE src/Tiff.cpp)
    target_compile_definitions(pe_core PUBLIC PHOTOEDIT_HAVE_TIFF=1)
    if(TARGET PkgConfig::TIFF)
        target_link_libraries(pe_core PUBLIC PkgConfig::TIFF)
    else()
        target_include_directories(pe_core PUBLIC ${TIFF_INCLUDE_DIR})
        target_link_libraries(pe_core PUBLIC ${TIFF_LIBRARY})
    endif()
    message(STATUS "  TIFF I/O:    libtiff ${TIFF_VERSION}")
else()
    message(STATUS "  TIFF I/O:    DISABLED (libtiff not found)")
endif()

# ---------------------------------------------------------------------------
# Optional WebP I/O (libwebp). pkg-config module is libwebp. Same graceful pattern.
# ---------------------------------------------------------------------------
if(PkgConfig_FOUND)
    pkg_check_modules(WEBP IMPORTED_TARGET libwebp)
endif()
if(NOT WEBP_FOUND)
    find_path(WEBP_INCLUDE_DIR NAMES webp/decode.h)
    find_library(WEBP_LIBRARY NAMES webp libwebp)
    if(WEBP_INCLUDE_DIR AND WEBP_LIBRARY)
        set(WEBP_FOUND TRUE)
    endif()
endif()
if(WEBP_FOUND AND NOT PHOTOEDIT_NO_OPTIONAL_DEPS)
    target_sources(pe_core PRIVATE src/Webp.cpp)
    target_compile_definitions(pe_core PUBLIC PHOTOEDIT_HAVE_WEBP=1)
    if(TARGET PkgConfig::WEBP)
        target_link_libraries(pe_core PUBLIC PkgConfig::WEBP)
    else()
        target_include_directories(pe_core PUBLIC ${WEBP_INCLUDE_DIR})
        target_link_libraries(pe_core PUBLIC ${WEBP_LIBRARY})
    endif()
    message(STATUS "  WebP I/O:    libwebp ${WEBP_VERSION}")
else()
    message(STATUS "  WebP I/O:    DISABLED (libwebp not found)")
endif()

# ---------------------------------------------------------------------------
# Optional zlib — compresses pixel blocks in the native .pedoc format. The format
# stays readable without it (blocks written uncompressed); a compressed block can
# only be read back when zlib is present. Same graceful pattern as above.
# ---------------------------------------------------------------------------
if(PkgConfig_FOUND)
    pkg_check_modules(ZLIBPC IMPORTED_TARGET zlib)
endif()
if(ZLIBPC_FOUND AND NOT PHOTOEDIT_NO_OPTIONAL_DEPS)
    target_compile_definitions(pe_core PUBLIC PHOTOEDIT_HAVE_ZLIB=1)
    target_link_libraries(pe_core PUBLIC PkgConfig::ZLIBPC)
    message(STATUS "  Compression: zlib ${ZLIBPC_VERSION}")
else()
    find_path(ZLIB_INCLUDE_DIR NAMES zlib.h)
    find_library(ZLIB_LIBRARY NAMES z zlib zlibstatic)
    if(ZLIB_INCLUDE_DIR AND ZLIB_LIBRARY AND NOT PHOTOEDIT_NO_OPTIONAL_DEPS)
        target_compile_definitions(pe_core PUBLIC PHOTOEDIT_HAVE_ZLIB=1)
        target_include_directories(pe_core PUBLIC ${ZLIB_INCLUDE_DIR})
        target_link_libraries(pe_core PUBLIC ${ZLIB_LIBRARY})
        message(STATUS "  Compression: zlib")
    else()
        message(STATUS "  Compression: DISABLED (zlib not found)")
    endif()
endif()
