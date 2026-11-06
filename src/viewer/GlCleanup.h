#pragma once

namespace steplens {

// 清理计划只看资源是否还在、context 是否有效、当前线程是否已经 current。
// 不看上一帧有没有画成功。makeCurrent 和 doneCurrent 成对出现。
struct GlCleanupInput {
    bool resourcesAlive = false;
    bool contextValid = false;
    bool alreadyCurrent = false;
};

struct GlCleanupPlan {
    bool makeCurrent = false;
    bool releaseResources = false;
    bool doneCurrent = false;
};

[[nodiscard]] constexpr GlCleanupPlan planGlCleanup(GlCleanupInput input) noexcept {
    GlCleanupPlan plan;
    if (!input.resourcesAlive) {
        return plan;
    }
    plan.releaseResources = true;
    if (input.contextValid && !input.alreadyCurrent) {
        plan.makeCurrent = true;
        plan.doneCurrent = true;
    }
    return plan;
}

} // namespace steplens
