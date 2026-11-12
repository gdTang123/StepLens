#pragma once

// releaseScene 只拆掉 SceneBuilder。宿主对象继续活着，LoadPipeline 握着的 SceneHost& 不会悬空。
// 上下文重建之后 bind 记新的 context 指针，下一次再造 builder。

namespace steplens {

struct SceneHostBinding {
    bool hostAlive = true;
    bool builderAlive = false;
    const void* context = nullptr;

    void bind(const void* next) noexcept {
        context = next;
        builderAlive = next != nullptr;
    }

    void releaseScene() noexcept {
        builderAlive = false;
        context = nullptr;
    }
};

} // namespace steplens
