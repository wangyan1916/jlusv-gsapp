#include <QApplication>
#include "app_window.h"

int main(int argc, char *argv[]) {
    // 启用高 DPI 支持
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    app.setApplicationName("JLUSV GroundStation");
    app.setOrganizationName("JLUSV");

    jlusv::AppWindow window;
    window.show();

    return app.exec();
}
