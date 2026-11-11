#include "scene/SceneBuilder.h"

#include "core/Log.h"
#include "occtsupport/occt_compat.h"

#include <AIS_ConnectedInteractive.hxx>
#include <Standard_Failure.hxx>

#include <exception>
#include <format>
#include <limits>
#include <utility>

namespace steplens::scene {
namespace {

constexpr int kWholeSelection = 0;

int clampCount(std::size_t count) {
    constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (count > kMax) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(count);
}

} // namespace

SceneBuilder::SceneBuilder(opencascade::handle<AIS_InteractiveContext> context) : m_ctx(std::move(context)) {}

SceneBuilder::~SceneBuilder() {
    try {
        if (!m_ctx.IsNull()) {
            m_ctx->RemoveAll(false);
        }
    } catch (...) {
    }
    m_map.clear();
    m_overrides.clear();
    m_protos.clear();
    m_queue.clear();
    m_ctx.Nullify();
}

void SceneBuilder::reset(std::shared_ptr<const model::DocumentModel> model) {
    m_queue.clear();
    m_seen.clear();
    m_hidden.clear();
    m_finished = 0;
    m_firstNotified = false;
    m_failed = false;
    m_lastError.clear();
    try {
        if (!m_ctx.IsNull()) {
            m_ctx->RemoveAll(false);
        }
    } catch (const Standard_Failure& failure) {
        fail(steplens::occt::failureMessage(failure));
    } catch (const std::exception& ex) {
        fail(ex.what());
    } catch (...) {
        fail("SceneBuilder reset failed");
    }
    m_map.clear();
    m_overrides.clear();
    m_protos.clear();
    m_model = std::move(model);
    if (m_model) {
        m_protos.assign(m_model->prototypes().size(), {});
        m_seen.assign(m_model->prototypes().size(), 0);
    }
    notifyPending();
    finishIfReady();
}

void SceneBuilder::enqueue(mesh::MeshBatch&& batch) {
    if (!m_model) {
        fail("SceneBuilder enqueue without a document");
        return;
    }
    if (batch.id >= m_protos.size()) {
        fail(std::format("SceneBuilder batch prototype {} is out of range", batch.id));
        return;
    }
    if (m_seen[batch.id] != 0) {
        return;
    }
    m_seen[batch.id] = 1;
    PendingBatch pending;
    pending.batch = std::move(batch);
    m_queue.push_back(std::move(pending));
    notifyPending();
}

void SceneBuilder::pump() {
    const auto start = std::chrono::steady_clock::now();
    int steps = 0;
    while (!m_queue.empty()) {
        if (steps > 0 && std::chrono::steady_clock::now() - start >= m_budget) {
            break;
        }
        PendingBatch& item = m_queue.front();
        bool more = false;
        try {
            more = takeOneStep(item);
        } catch (const Standard_Failure& failure) {
            fail(steplens::occt::failureMessage(failure));
            more = false;
        } catch (const std::exception& ex) {
            fail(ex.what());
            more = false;
        } catch (...) {
            fail("SceneBuilder pump failed");
            more = false;
        }
        ++steps;
        if (!more) {
            m_queue.pop_front();
            ++m_finished;
        }
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    m_lastPump = elapsed;
    if (elapsed > m_maxPump) {
        m_maxPump = elapsed;
    }
#ifndef NDEBUG
    if (elapsed >= kStallWarn) {
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        Log::warn(std::format("SceneBuilder pump stalled for {} ms", millis));
    }
#endif
    notifyPending();
    finishIfReady();
}

void SceneBuilder::setNodeVisible(model::NodeId id, bool visible) {
    if (!m_model || id >= m_model->nodeCount()) {
        return;
    }
    applyVisible(id, visible);
}

void SceneBuilder::highlightNode(model::NodeId id) {
    if (!m_model || m_ctx.IsNull() || id >= m_model->nodeCount()) {
        return;
    }
    m_ctx->ClearSelected(false);
    selectTree(id);
}

std::optional<model::NodeId>
SceneBuilder::nodeOf(const opencascade::handle<AIS_InteractiveObject>& object) const {
    return m_map.nodeOf(object);
}

opencascade::handle<AIS_InteractiveObject> SceneBuilder::objectOf(model::NodeId id) const {
    return m_map.objectOf(id);
}

opencascade::handle<PrototypePrs> SceneBuilder::sharedPrototype(model::PrototypeId id) const {
    if (id >= m_protos.size()) {
        return {};
    }
    return m_protos[id];
}

void SceneBuilder::setFirstFrameCallback(std::function<void()> callback) {
    m_onFirstFrame = std::move(callback);
}

void SceneBuilder::setPendingCallback(std::function<void(int)> callback) {
    m_onPending = std::move(callback);
}

void SceneBuilder::setFrameBudget(std::chrono::microseconds budget) {
    if (budget.count() < 0) {
        budget = std::chrono::microseconds::zero();
    }
    m_budget = budget;
}

void SceneBuilder::setInstanceProbe(std::function<void()> probe) {
    m_probe = std::move(probe);
}

int SceneBuilder::pending() const {
    return clampCount(m_queue.size());
}

bool SceneBuilder::takeOneStep(PendingBatch& item) {
    if (!item.prototypeReady) {
        createPrototype(item);
        item.prototypeReady = true;
        return !m_model->leafNodesOf(item.batch.id).empty();
    }
    const auto leaves = m_model->leafNodesOf(item.batch.id);
    if (item.nextInstance >= leaves.size()) {
        return false;
    }
    createInstance(item, leaves[item.nextInstance]);
    ++item.nextInstance;
    return item.nextInstance < leaves.size();
}

void SceneBuilder::createPrototype(PendingBatch& item) {
    const model::PrototypeInfo& info = m_model->prototype(item.batch.id);
    m_protos[item.batch.id] = new PrototypePrs(item.batch, info.shape);
}

void SceneBuilder::createInstance(PendingBatch& item, model::NodeId id) {
    const model::AssemblyNode& node = m_model->node(id);
    const gp_Trsf transform = node.worldLocation.Transformation();
    opencascade::handle<AIS_InteractiveObject> displayed;
    if (node.instanceColor.has_value()) {
        const model::PrototypeInfo& info = m_model->prototype(item.batch.id);
        opencascade::handle<PrototypePrs> colored =
            new PrototypePrs(recolored(item.batch, *node.instanceColor), info.shape);
        colored->SetLocalTransformation(transform);
        m_overrides.push_back(colored);
        displayed = colored;
    } else {
        opencascade::handle<AIS_ConnectedInteractive> instance = new AIS_ConnectedInteractive();
        instance->Connect(m_protos[item.batch.id], transform);
        displayed = instance;
    }
    m_map.bind(id, displayed);
    if (!hidden(id)) {
        show(displayed);
    }
    if (m_probe) {
        m_probe();
    }
}

void SceneBuilder::show(const opencascade::handle<AIS_InteractiveObject>& object) {
    if (m_ctx.IsNull() || object.IsNull()) {
        return;
    }
    m_ctx->Display(object, static_cast<int>(m_mode), kWholeSelection, false);
}

void SceneBuilder::erase(const opencascade::handle<AIS_InteractiveObject>& object) {
    if (m_ctx.IsNull() || object.IsNull()) {
        return;
    }
    if (m_ctx->IsDisplayed(object)) {
        m_ctx->Erase(object, false);
    }
}

void SceneBuilder::applyVisible(model::NodeId id, bool visible) {
    const model::AssemblyNode& node = m_model->node(id);
    if (node.prototype != model::kNoPrototype) {
        if (visible) {
            m_hidden.erase(id);
        } else {
            m_hidden.insert(id);
        }
        const auto object = m_map.objectOf(id);
        if (!object.IsNull()) {
            if (visible) {
                show(object);
            } else {
                erase(object);
            }
        }
        return;
    }
    for (const model::NodeId child : node.children) {
        applyVisible(child, visible);
    }
}

void SceneBuilder::selectTree(model::NodeId id) {
    const model::AssemblyNode& node = m_model->node(id);
    if (node.prototype != model::kNoPrototype) {
        const auto object = m_map.objectOf(id);
        if (!object.IsNull()) {
            m_ctx->AddOrRemoveSelected(object, false);
        }
        return;
    }
    for (const model::NodeId child : node.children) {
        selectTree(child);
    }
}

void SceneBuilder::finishIfReady() {
    if (m_firstNotified || !m_model || !m_queue.empty()) {
        return;
    }
    if (m_finished != m_model->prototypes().size()) {
        return;
    }
    m_firstNotified = true;
    if (m_onFirstFrame) {
        m_onFirstFrame();
    }
}

void SceneBuilder::notifyPending() {
    if (m_onPending) {
        m_onPending(pending());
    }
}

void SceneBuilder::fail(std::string message) {
    m_failed = true;
    m_lastError = std::move(message);
    Log::warn(m_lastError);
}

mesh::MeshBatch SceneBuilder::recolored(const mesh::MeshBatch& batch, const Quantity_ColorRGBA& color) const {
    mesh::MeshBatch copy = batch;
    for (mesh::ColorGroupArrays& group : copy.groups) {
        group.color = color;
    }
    return copy;
}

bool SceneBuilder::hidden(model::NodeId id) const {
    return m_hidden.find(id) != m_hidden.end();
}

} // namespace steplens::scene
