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
    core/test_snapshot.cpp
    core/test_cowescapes.cpp
    core/test_history.cpp
    core/test_viewtransform.cpp
    core/test_canvasrenderer.cpp
    core/test_brush.cpp
    core/test_regionbrush.cpp
    core/test_brushtiling.cpp
    core/test_painttool.cpp
    core/test_selection.cpp
    core/test_setselection.cpp
    core/test_crop.cpp
    core/test_canvassize.cpp
    core/test_layergroup.cpp
    core/test_mask.cpp
    core/test_masktiling.cpp
    core/test_maskcommands.cpp
    core/test_maskbrush.cpp
    core/test_livestroke.cpp
    core/test_adjustment.cpp
    core/test_textlayer.cpp
    core/test_filter.cpp
    core/test_hittest.cpp
    core/test_golden.cpp
    core/test_filtertiling.cpp
    core/test_histogram.cpp
    core/test_channels.cpp
    core/test_gradient.cpp
    core/test_resample.cpp
    core/test_resamplelayer.cpp
    core/test_resamplemask.cpp
    core/test_imagesize.cpp
    core/test_clipboard.cpp
    core/test_merge.cpp
    core/test_autotone.cpp
    core/test_colorspace.cpp
    core/test_colorprofile.cpp
    core/test_colortransform.cpp
    core/test_colorengine.cpp
    core/test_colorops.cpp
    core/test_channels.cpp
    core/test_png.cpp
    core/test_jpeg.cpp
    core/test_tiff.cpp
    core/test_webp.cpp
    core/test_psd.cpp
    core/test_nativeformat.cpp
    core/test_documentio.cpp
)

# The snapshot/copy-on-write isolation tests run a real worker thread against a live
# document, which is the only way to prove the fork barrier actually holds. std::thread
# needs an explicit pthread link on Linux; on Windows it is built in.
find_package(Threads REQUIRED)
target_link_libraries(pe_core_tests PRIVATE PhotoEdit::Core Threads::Threads)
target_include_directories(pe_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Golden references live in the SOURCE tree, not the build tree: they are committed
# fixtures, and PE_GOLDEN_UPDATE rewrites them in place so a regeneration shows up as a
# reviewable diff rather than a build artifact nobody sees.
target_compile_definitions(pe_core_tests
    PRIVATE PE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
# Committed .pedoc files written under an older writer policy. They exist to prove the
# reader keeps reading them, so they are never regenerated to make a test pass.
target_compile_definitions(pe_core_tests
    PRIVATE PE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME pe_core_tests COMMAND pe_core_tests)

# ---------------------------------------------------------------------------
# Shell tests. Gated on PHOTOEDIT_BUILD_APP because the headless core lanes
# configure with the app off and no Qt present; pe_app does not exist there.
#
# Same zero-dependency harness as the engine tests: these link pe_app but need
# no QApplication, so they run in any lane that builds the shell.
# ---------------------------------------------------------------------------
if(PHOTOEDIT_BUILD_APP)
    # Its own entry point rather than tests/test_main.cpp: these construct real
    # widgets, so they need a QApplication (created on the offscreen platform).
    add_executable(pe_app_tests
        app/app_test_main.cpp
        app/test_theme.cpp
        app/test_mainwindow.cpp
        app/test_document_guard.cpp
        app/test_pixelformatnames.cpp
        app/test_iconutil.cpp
        app/test_canvasview.cpp
        app/test_layerspanel.cpp
        app/test_swatchespanel.cpp
        app/test_adjustmentspanel.cpp
        app/test_channelspanel.cpp
        app/test_gradientspanel.cpp
        app/test_clipboard.cpp
        app/test_mergeui.cpp
        app/test_canvassizeui.cpp
        app/test_imagesizeui.cpp
        app/test_colorui.cpp
        app/test_autotoneui.cpp
        app/test_histogrampanel.cpp
        app/test_newdocument.cpp
        app/test_busytask.cpp
    )

    target_link_libraries(pe_app_tests PRIVATE PhotoEdit::App)
    target_include_directories(pe_app_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME pe_app_tests COMMAND pe_app_tests)
endif()
