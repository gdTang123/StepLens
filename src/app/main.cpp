#include "app/MainWindow.h"
#include "core/AppPaths.h"
#include "core/CrashDump.h"
#include "core/Log.h"
#include "core/OcctLoadContract.h"
#include "core/Version.h"

#include <QApplication>
#include <QString>

#include <exception>
#include <string>

int main(int argc, char* argv[]) {
    try {
        // Interface_Static 只允许在 main() 的这个位置、单线程初始化一次。
        // T05 的加载器不调用 SetIVal / SetCVal。XCAFApp NewDocument / Close 走 xcafDocumentMutex()。
        steplens::initializeInterfaceStaticOnMainThread();
        steplens::Log::initialize(steplens::defaultWritableDirectory("logs"));
        // 链上 OCCT 之后，OSD::SetSignal 可能也会换掉未处理异常过滤器。
        // CrashDump::install 必须排在 OSD::SetSignal 之后。现在 core 还不调用 OSD。
        steplens::CrashDump::installDefault();
        steplens::Log::info("StepLens 启动");
    } catch (const std::exception& ex) {
        // 启动阶段还没有可用日志。窗口仍然打开。
        static_cast<void>(ex.what());
    }

#if defined(_WIN32)
    // OCCT 不走 ANGLE。必须在 QApplication 之前。
    QApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("StepLens"));
    QApplication::setApplicationVersion(QString::fromUtf8(std::string{steplens::kVersion}.c_str()));

    steplens::MainWindow window;
    window.show();
    return app.exec();
}
