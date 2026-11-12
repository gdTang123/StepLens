#include "app/MainWindow.h"
#include "core/AppPaths.h"
#include "core/CrashDump.h"
#include "core/Log.h"
#include "core/OcctLoadContract.h"
#include "core/Version.h"
#include "io/HeapRelease.h"
#include "model/DocumentRelease.h"

#include <QApplication>
#include <QGuiApplication>
#include <QString>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cstdlib>
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
    // 每监视器 V2。Qt 6 的入口也会设；这里提前设上，125% 和 150% 按系统缩放，不取整到 100% 或 200%。
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 的值是 (HANDLE)-4。
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetDpiContext = int(__stdcall*)(void*);
        auto setDpiContext =
            reinterpret_cast<SetDpiContext>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setDpiContext != nullptr) {
            static_cast<void>(setDpiContext(reinterpret_cast<void*>(static_cast<std::intptr_t>(-4))));
        }
    }
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("StepLens"));
    QApplication::setApplicationVersion(QString::fromUtf8(std::string{steplens::kVersion}.c_str()));

    int code = 0;
    {
        // 先拆主窗口，让 SceneBuilder / LoadPipeline 把最后一份文档交到后台。
        // QApplication 还活着，Qt 对象可以正常析构。
        steplens::MainWindow window;
        window.show();
        const QStringList arguments = QApplication::arguments();
        for (int index = 1; index < arguments.size(); ++index) {
            const QString argument = arguments.at(index);
            if (argument.startsWith(QLatin1Char('-'))) {
                continue;
            }
            window.openPath(argument);
            break;
        }
        code = app.exec();
    }
    // 窗口已经拆完。若 Close 还在后台跑，不能再往下拆 QApplication 和 OCCT 静态对象。
    if (steplens::model::documentsPendingRelease() > 0) {
        steplens::Log::info("退出时文档仍在后台关闭，结束进程");
        steplens::Log::shutdown();
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(), 0);
#else
        std::quick_exit(0);
#endif
    }
    steplens::io::releaseFreedHeap();
    return code;
}
