#include "MainWindow.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PhotoEdit"));
    QApplication::setOrganizationName(QStringLiteral("MalloyTheDev"));

    pe::app::MainWindow window;
    window.show();

    return QApplication::exec();
}
