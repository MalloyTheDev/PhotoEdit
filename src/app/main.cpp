#include "MainWindow.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QSettings>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PhotoEdit"));
    QApplication::setOrganizationName(QStringLiteral("MalloyTheDev"));

    // Apply the theme before any widgets are shown; honor the last choice. Nocturne
    // (the flagship blue-grey) is the default for a fresh profile; themeFromInt folds
    // an unrecognised persisted id rather than trusting the stored value.
    const int saved =
        QSettings()
            .value(QStringLiteral("theme"), static_cast<int>(pe::app::ThemeId::Nocturne))
            .toInt();
    pe::app::applyTheme(app, pe::app::themeFromInt(saved));

    pe::app::MainWindow window;
    window.show();

    return QApplication::exec();
}
