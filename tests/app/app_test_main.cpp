// Entry point for the shell test binary.
//
// Separate from tests/test_main.cpp because these tests construct real widgets and
// therefore need a QApplication. The offscreen platform plugin keeps that headless,
// so this runs in CI and over SSH with no display attached.

#include "IconUtil.hpp"
#include "pe_test.hpp"

#include <cstdio>

#include <QApplication>

int main(int argc, char** argv) {
    // Unbuffered, so the last line in the log is the test that was actually running.
    // Block buffering attributes a crash to whichever case the buffer last flushed,
    // which can be dozens of tests earlier.
    setvbuf(stdout, nullptr, _IONBF, 0);
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
