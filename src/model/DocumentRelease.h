#pragma once

// handoffDocument 只受控放下一份引用，不记票；retireDocument 首次退役时记一票。
// Retirer 在进程退出时继续接收尾投递，等所有模型 Close 完成后再退出。快退不改，也不声称 Close 已完成。

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace steplens::model {

class DocumentModel;

void retireDocument(std::shared_ptr<const DocumentModel> model);
void handoffDocument(std::shared_ptr<const DocumentModel> model) noexcept;
void destroyDocumentModel(const DocumentModel* model) noexcept;
void noteDocumentConstructed() noexcept;

[[nodiscard]] int documentsPendingRelease() noexcept;

// 等到后台 Close 结束。超时返回 false。
[[nodiscard]] bool waitForDocumentRelease(std::chrono::milliseconds timeout);

// 测试缝。在后台线程、DocumentModel 析构开始时调用。可以堵住，用来制造“释放还没完”。
// 生产路径不设置。换测试前清成空函数。
void setDocumentReleaseProbe(std::function<void(std::thread::id)> probe);

// 测试缝。后台线程取出一份文档之后、减少 documentsPendingRelease() 之前调用。
// 空指针时只做一次加载，不进回调。生产路径保持空指针。
// 回调可以堵住，这样计数在回调返回前不会下降。回调里不能再等这次释放，否则和后台线程互相等。
using DocumentReleaseGate = void (*)();
void setDocumentReleaseGate(DocumentReleaseGate gate) noexcept;

// DocumentModel 析构一开始就调用。探针看到的线程就是 Close 所在的线程。
void noteDocumentDestructorThread();

} // namespace steplens::model
