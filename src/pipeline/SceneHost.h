#pragma once

// GUI 线程上的场景入口。LoadPipeline 不认识 AIS，避免无头测试去链 TKOpenGl。
// enqueue / pump / reset / clear / fitAll 都由调用方在 GUI 线程执行。
// 工作线程只把 MeshBatch 放进流水线自己的队列，不能调用这里。

#include "mesh/MeshBatch.h"

#include <memory>

namespace steplens {

class SceneHost {
public:
    virtual ~SceneHost() = default;

    SceneHost(const SceneHost&) = delete;
    SceneHost& operator=(const SceneHost&) = delete;

    virtual void reset(std::shared_ptr<const model::DocumentModel> model) = 0;
    virtual void enqueue(mesh::MeshBatch batch) = 0;
    virtual void pump() = 0;
    virtual void clear() = 0;
    virtual void fitAll() = 0;
    [[nodiscard]] virtual int pending() const = 0;

protected:
    SceneHost() = default;
};

} // namespace steplens
