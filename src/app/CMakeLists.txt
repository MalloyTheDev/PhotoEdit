# ---------------------------------------------------------------------------
# photoedit - the Qt6 desktop application shell.
#
# This is the ONLY place Qt is allowed. It depends on PhotoEdit::Core for all
# engine functionality. Keep UI logic here; keep image/document logic in core.
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup()

qt_add_executable(photoedit
    main.cpp
    MainWindow.cpp
    MainWindow.hpp
    CanvasView.cpp
    CanvasView.hpp
)

target_link_libraries(photoedit
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        PhotoEdit::Core)

set_target_properties(photoedit PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE OFF)
