#include "MainWindow.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QSettings>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PhotoEdit"));
    QApplication::setOrganizationName(QStringLiteral("MalloyTheDev"));

    // Apply the dark pro theme before any widgets are shown; honor the last choice.
    const int saved =
        QSettings()
            .value(QStringLiteral("theme"), static_cast<int>(pe::app::ThemeId::Graphite))
            .toInt();
    const pe::app::ThemeId theme = saved == static_cast<int>(pe::app::ThemeId::Slate)
                                       ? pe::app::ThemeId::Slate
                                       : pe::app::ThemeId::Graphite;
    pe::app::applyTheme(app, theme);

    pe::app::MainWindow window;
    window.show();

    return QApplication::exec();
}
