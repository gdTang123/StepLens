#include "app/MainWindow.h"
#include "core/AppPaths.h"
#include "core/CrashDump.h"
#include "core/Log.h"
#include "core/Version.h"

#include <QApplication>
#include <QString>

#include <exception>
#include <string>

int main(int argc, char* argv[]) {
    try {
        steplens::Log::initialize(steplens::defaultWritableDirectory("logs"));
        // 链上 OCCT 之后，OSD::SetSignal 可能也会换掉未处理异常过滤器。
        // CrashDump::install 必须排在 OSD::SetSignal 之后。现在 core 还不调用 OSD。
        steplens::CrashDump::installDefault();
        steplens::Log::info("StepLens 启动");
    } catch (const std::exception& ex) {
        // 启动阶段还没有可用日志。窗口仍然打开。
        static_cast<void>(ex.what());
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("StepLens"));
    QApplication::setApplicationVersion(QString::fromUtf8(std::string{steplens::kVersion}.c_str()));

    steplens::MainWindow window;
    window.show();
    return app.exec();
}
