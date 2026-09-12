# ---------------------------------------------------------------------------
# photoedit - the Qt6 desktop application shell.
#
# This is the ONLY place Qt is allowed. It depends on PhotoEdit::Core for all
# engine functionality. Keep UI logic here; keep image/document logic in core.
#
# The shell is split into a `pe_app` library plus a thin `main.cpp` executable,
# mirroring pe_core. The split exists so the shell can be linked by a test
# binary: as a bare executable none of these translation units were reachable
# from tests, so nothing in src/app had any regression guard. See ADR-0008.
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)

qt_standard_project_setup()
set(CMAKE_AUTORCC ON)  # compile icons.qrc (qt_standard_project_setup enables MOC/UIC, not RCC)

qt_add_library(pe_app STATIC
    MainWindow.cpp
    MainWindow.hpp
    BusyTask.cpp
    BusyTask.hpp
    CanvasView.cpp
    CanvasView.hpp
    CurveEditorWidget.cpp
    CurveEditorWidget.hpp
    CanvasSizeDialog.cpp
    CanvasSizeDialog.hpp
    ImageSizeDialog.cpp
    ImageSizeDialog.hpp
    DimensionSpin.hpp
    NewDocumentDialog.cpp
    NewDocumentDialog.hpp
    CurvesDialog.cpp
    CurvesDialog.hpp
    EffectDialog.cpp
    EffectDialog.hpp
    ExportDialog.cpp
    ExportDialog.hpp
    GroupedSlidersDialog.cpp
    GroupedSlidersDialog.hpp
    TextDialog.cpp
    TextDialog.hpp
    TextRender.cpp
    TextRender.hpp
    LayersPanel.cpp
    LayersPanel.hpp
    HistoryPanel.cpp
    HistoryPanel.hpp
    ColorPanel.cpp
    ColorPanel.hpp
    SwatchesPanel.cpp
    SwatchesPanel.hpp
    AdjustmentsPanel.cpp
    AdjustmentsPanel.hpp
    ChannelsPanel.cpp
    HistogramPanel.cpp
    HistogramPanel.hpp
    ChannelsPanel.hpp
    GradientsPanel.cpp
    GradientsPanel.hpp
    PropertiesPanel.cpp
    PropertiesPanel.hpp
    Theme.cpp
    Theme.hpp
    IconUtil.cpp
    IconUtil.hpp
    icons.qrc
)

add_library(PhotoEdit::App ALIAS pe_app)

# PUBLIC: the headers are the shell's surface for the test binary, and consumers
# of pe_app need Qt and the engine on their own include/link lines.
target_include_directories(pe_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pe_app
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Svg
        PhotoEdit::Core)

# The colour-management UI (Edit > Assign / Convert Profile) builds ColorProfile handles, whose
# factory lives in the lcms2-only ColorProfile.cpp. Compile the dialog only when pe_core was built
# with lcms2; MainWindow.cpp guards its use with the same PHOTOEDIT_HAVE_LCMS2 macro, so the app
# still builds (just without those two menu items) on a toolchain that has no lcms2.
get_target_property(_pe_core_defs pe_core INTERFACE_COMPILE_DEFINITIONS)
if(_pe_core_defs MATCHES "PHOTOEDIT_HAVE_LCMS2")
    target_sources(pe_app PRIVATE ColorProfileDialog.cpp ColorProfileDialog.hpp)
endif()

# The entry point only constructs the window; everything it touches lives in pe_app.
qt_add_executable(photoedit main.cpp)

target_link_libraries(photoedit PRIVATE PhotoEdit::App)

set_target_properties(photoedit PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE OFF)
