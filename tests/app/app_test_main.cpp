// Entry point for the shell test binary.
//
// Separate from tests/test_main.cpp because these tests construct real widgets and
// therefore need a QApplication. The offscreen platform plugin keeps that headless,
// so this runs in CI and over SSH with no display attached.

#include "IconUtil.hpp"
#include "pe_test.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    // Must be set before QApplication is constructed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PhotoEditTests"));
    QApplication::setOrganizationName(QStringLiteral("MalloyTheDev"));

    // Mirrors main(): pe_app is a static library, so without this the bundled glyphs
    // are never registered. See IconUtil.hpp.
    pe::app::initIconResources();

    return pe_test::runAll();
}
