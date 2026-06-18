# ---------------------------------------------------------------------------
# photoedit - the Qt6 desktop application shell.
#
# This is the ONLY place Qt is allowed. It depends on PhotoEdit::Core for all
# engine functionality. Keep UI logic here; keep image/document logic in core.
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)

qt_standard_project_setup()
set(CMAKE_AUTORCC ON)  # compile icons.qrc (qt_standard_project_setup enables MOC/UIC, not RCC)

qt_add_executable(photoedit
    main.cpp
    MainWindow.cpp
    MainWindow.hpp
    CanvasView.cpp
    CanvasView.hpp
    EffectDialog.cpp
    EffectDialog.hpp
    ExportDialog.cpp
    ExportDialog.hpp
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
    PropertiesPanel.cpp
    PropertiesPanel.hpp
    Theme.cpp
    Theme.hpp
    IconUtil.cpp
    IconUtil.hpp
    icons.qrc
)

target_link_libraries(photoedit
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Svg
        PhotoEdit::Core)

set_target_properties(photoedit PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE OFF)
