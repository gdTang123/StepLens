#include "core/CancelToken.h"
#include "core/OcctLoadContract.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "model/DocumentModel.h"
#include "model/DocumentRelease.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

int main() {
    steplens::io::LoadedDocument loaded;
    {
        std::lock_guard lock(steplens::xcafDocumentMutex());
        XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", loaded.doc);
        XCAFDoc_DocumentTool::ShapeTool(loaded.doc->Main())
            ->AddShape(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape());
    }
    loaded.schema = "STEP";
    loaded.lengthUnitToMm = 1.0;
    steplens::ProgressSink progress;
    steplens::CancelToken cancel;
    auto result = steplens::model::DocumentModel::build(std::move(loaded), progress, cancel);
    if (!result.ok()) {
        return 2;
    }
    // 子进程自己设置延迟，避免污染父测试环境。pending 在 reset 后、Close 完成前保持 >0。
#if defined(_WIN32)
    SetEnvironmentVariableA("STEPLENS_RELEASE_DELAY_MS", "2500");
#else
    setenv("STEPLENS_RELEASE_DELAY_MS", "2500", 1);
#endif
    result.value().reset();
    // 与 app/main.cpp 使用同一判断：仍有释放票时直接跳过静态析构。
    if (steplens::model::documentsPendingRelease() > 0) {
        std::quick_exit(0);
    }
    return 3;
}
