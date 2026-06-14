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
    src/TileStore.cpp
    src/Layer.cpp
    src/PixelLayer.cpp
    src/SolidColorLayer.cpp
    src/GroupLayer.cpp
    src/Compositor.cpp
    src/Document.cpp
    src/History.cpp
    src/Commands.cpp
    src/ViewTransform.cpp
    src/CanvasRenderer.cpp
)

add_library(PhotoEdit::Core ALIAS pe_core)

target_include_directories(pe_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(pe_core PUBLIC cxx_std_20)

set_target_properties(pe_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
