#pragma once

// T08 场景。只在 GUI 线程调用。AIS_InteractiveContext 不是线程安全的，本类没有锁。
// 架构草图写成 QObject，由 QTimer(0) 调 pump。无头契约测试不链 Qt，所以这里不继承 QObject。
// GUI 上的拥有者（以后的加载编排）每个事件循环调用一次 pump()。信号改成回调，
// 参数只有空和 int，不传 OCCT 对象。
// 原型对象不 Display。没有实例色的实例用 AIS_ConnectedInteractive 引用同一份 PrototypePrs。
// 实例自己有颜色时，ConnectedInteractive 改不了颜色，改为单独一个 PrototypePrs，三角形数组仍共用。
// replaceMesh（粗/细两级）不在这里。

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "scene/PrototypePrs.h"
#include "scene/SelectionMap.h"

#include <AIS_InteractiveContext.hxx>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace steplens::scene {

class SceneBuilder {
public:
    static constexpr std::chrono::microseconds kDefaultFrameBudget{8000};
    static constexpr std::chrono::microseconds kStallWarn{50000};

    explicit SceneBuilder(opencascade::handle<AIS_InteractiveContext> context);
    ~SceneBuilder();

    SceneBuilder(const SceneBuilder&) = delete;
    SceneBuilder& operator=(const SceneBuilder&) = delete;
    SceneBuilder(SceneBuilder&&) = delete;
    SceneBuilder& operator=(SceneBuilder&&) = delete;

    // 清掉旧的 AIS 对象。context 上 RemoveAll(false)，连同上下文里别人放的对象一起清。
    void reset(std::shared_ptr<const model::DocumentModel> model);
    // 队列里留到 pump。同一原型的第二份批次直接丢掉（两级替换是 T08+）。
    void enqueue(mesh::MeshBatch&& batch);
    // 做到这一帧的时间预算用完为止。至少处理一个创建步骤，避免预算为 0 时卡死。
    void pump();

    void setNodeVisible(model::NodeId id, bool visible);
    void highlightNode(model::NodeId id);

    [[nodiscard]] std::optional<model::NodeId>
    nodeOf(const opencascade::handle<AIS_InteractiveObject>& object) const;
    [[nodiscard]] opencascade::handle<AIS_InteractiveObject> objectOf(model::NodeId id) const;
    [[nodiscard]] const SelectionMap& selectionMap() const {
        return m_map;
    }
    [[nodiscard]] opencascade::handle<PrototypePrs> sharedPrototype(model::PrototypeId id) const;

    void setFirstFrameCallback(std::function<void()> callback);
    void setPendingCallback(std::function<void(int)> callback);

    // 测试缝。默认 8 ms。生产路径用默认值。
    void setFrameBudget(std::chrono::microseconds budget);
    // 测试缝。每挂上一个实例调用一次。生产路径不设置。
    void setInstanceProbe(std::function<void()> probe);

    [[nodiscard]] int pending() const;
    [[nodiscard]] std::chrono::microseconds lastPumpDuration() const {
        return m_lastPump;
    }
    [[nodiscard]] std::chrono::microseconds maxPumpDuration() const {
        return m_maxPump;
    }
    [[nodiscard]] bool failed() const {
        return m_failed;
    }
    [[nodiscard]] const std::string& lastError() const {
        return m_lastError;
    }

private:
    struct PendingBatch {
        mesh::MeshBatch batch;
        bool prototypeReady = false;
        std::size_t nextInstance = 0;
    };

    // 返回 true 表示这份批次还有剩余步骤。
    bool takeOneStep(PendingBatch& item);
    void createPrototype(PendingBatch& item);
    void createInstance(PendingBatch& item, model::NodeId id);
    void show(const opencascade::handle<AIS_InteractiveObject>& object);
    void erase(const opencascade::handle<AIS_InteractiveObject>& object);
    void applyVisible(model::NodeId id, bool visible);
    void selectTree(model::NodeId id);
    void finishIfReady();
    void notifyPending();
    void fail(std::string message);
    [[nodiscard]] mesh::MeshBatch recolored(const mesh::MeshBatch& batch,
                                            const Quantity_ColorRGBA& color) const;
    [[nodiscard]] bool hidden(model::NodeId id) const;

    opencascade::handle<AIS_InteractiveContext> m_ctx;
    std::shared_ptr<const model::DocumentModel> m_model;
    std::vector<opencascade::handle<PrototypePrs>> m_protos;
    std::vector<opencascade::handle<PrototypePrs>> m_overrides;
    SelectionMap m_map;
    std::deque<PendingBatch> m_queue;
    std::vector<std::uint8_t> m_seen;
    std::unordered_set<model::NodeId> m_hidden;
    PrototypePrs::Mode m_mode = PrototypePrs::ShadedWithEdges;
    std::chrono::microseconds m_budget = kDefaultFrameBudget;
    std::function<void()> m_onFirstFrame;
    std::function<void(int)> m_onPending;
    std::function<void()> m_probe;
    std::chrono::microseconds m_lastPump{0};
    std::chrono::microseconds m_maxPump{0};
    std::size_t m_finished = 0;
    bool m_firstNotified = false;
    bool m_failed = false;
    std::string m_lastError;
};

} // namespace steplens::scene
