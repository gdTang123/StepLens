#include "app/MainWindow.h"
#include "core/Version.h"

#include <QApplication>
#include <QString>

#include <string>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("StepLens"));
    QApplication::setApplicationVersion(QString::fromUtf8(std::string{steplens::kVersion}.c_str()));

    steplens::MainWindow window;
    window.show();
    return app.exec();
}
