#pragma once

// 把 T05 读取、T06 DocumentModel、T07 Tessellator 串起来，再把网格交给 SceneHost。
// open / cancel / tick 只在 GUI 线程调用。tick 里才会 pump，工作线程碰不到 SceneBuilder。
// 软取消只请求停止并冻住进度，不等待后台。后台释放完之前，下一次 open 在 LoadCoordinator 里排队。

#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "core/Result.h"
#include "pipeline/SceneHost.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace steplens {

enum class LoadActivity : std::uint8_t { Idle, Loading, Releasing, Ready, Failed };

class LoadPipeline {
public:
    struct Snapshot {
        LoadActivity activity = LoadActivity::Idle;
        ProgressPhase progress = ProgressPhase::Idle;
        float fraction = 0.f;
        double elapsedSeconds = 0.0;
        bool overEstimate = false;
        bool stalled = false;
        bool waitingForRelease = false;
        std::filesystem::path file;
    };

    explicit LoadPipeline(SceneHost& host);
    ~LoadPipeline();

    LoadPipeline(const LoadPipeline&) = delete;
    LoadPipeline& operator=(const LoadPipeline&) = delete;

    // startPause 只给截图钩子：工作线程在真正读取前睡觉，遮罩来得及画出来。
    // 生产路径传 0。取消会把这段等待拆开，不用等满。
    void open(const std::filesystem::path& file, std::chrono::milliseconds startPause = {});
    void cancel();
    void tick();

    [[nodiscard]] Snapshot snapshot() const;
    // 失败对话框只弹一次。没有新的失败就返回空。
    [[nodiscard]] std::optional<Error> takeFailure();
    [[nodiscard]] bool isSettled() const noexcept;
    [[nodiscard]] bool waitUntilSettled(std::chrono::milliseconds timeout) const;

    // 测试缝。在模型交给界面之前、仍在加载线程上调用。可以看 CancelToken，不要碰 SceneHost。
    void setBeforePublishForTest(std::function<void(const CancelToken&)> probe);

private:
    struct State;

    void finishWorker(std::uint64_t generation, bool ok, std::optional<Error> error);
    [[nodiscard]] bool generationCurrent(std::uint64_t generation) const;

    SceneHost& m_host;
    std::unique_ptr<State> m_state;
};

} // namespace steplens
