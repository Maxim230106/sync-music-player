#include <QApplication>

#include "src/ui/mainwindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("SyncSound");
    QApplication::setOrganizationName("letnyapractica");

    MainWindow window;
    window.show();

    return app.exec();
}
