# Dependency-free unit tests for the engine core. No GUI, no network, no external
# test framework: this lets the full engine be validated in any CI lane,
# including the headless Linux core-dev lane.
add_executable(pe_core_tests
    test_main.cpp
    core/test_geometry.cpp
    core/test_color.cpp
    core/test_tile.cpp
    core/test_blend.cpp
    core/test_pixelbuffer.cpp
    core/test_tilestore.cpp
    core/test_layer.cpp
    core/test_compositor.cpp
    core/test_document.cpp
    core/test_history.cpp
    core/test_viewtransform.cpp
    core/test_canvasrenderer.cpp
    core/test_brush.cpp
    core/test_selection.cpp
    core/test_mask.cpp
    core/test_adjustment.cpp
)

target_link_libraries(pe_core_tests PRIVATE PhotoEdit::Core)
target_include_directories(pe_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME pe_core_tests COMMAND pe_core_tests)
