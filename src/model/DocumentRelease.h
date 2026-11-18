#pragma once

// handoffDocument 只受控放下一份引用，不记票；retireDocument 首次退役时记一票。
// Retirer 在进程退出时继续接收尾投递，等所有模型 Close 完成后再退出。快退不改，也不声称 Close 已完成。

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace steplens::model {

class DocumentModel;

void retireDocument(std::shared_ptr<const DocumentModel> model);
void handoffDocument(std::shared_ptr<const DocumentModel> model) noexcept;
void destroyDocumentModel(const DocumentModel* model) noexcept;
void noteDocumentConstructed() noexcept;

// 测试缝：仅影响下一次模型交接，在 owner.detach() 后用局部失败 allocator 走三参数 shared_ptr。
void failNextDocumentControlBlockForTesting(bool fail) noexcept;
// 最近一次控制块失败是否进入了局部 allocator::allocate（真实标准库路径）。
[[nodiscard]] bool lastDocumentControlBlockAllocatorFailed() noexcept;
// 测试缝：在真实 XCAF Close 调用前注入异常；析构仍会完成清理。
void failNextDocumentCloseForTesting(bool fail) noexcept;
// 只报告最近一次 XCAF Close 是否失败；pending 归零仅说明释放任务结束，不代表 Close 成功。
[[nodiscard]] bool lastDocumentCloseFailed() noexcept;
// 进程内累计 Close 失败次数，只增不减；测试应比较调用前后的差值。
[[nodiscard]] std::uint64_t documentCloseFailureCount() noexcept;

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

// 测试缝：在节点入队后、producer 最后一次通知之前调用，可阻塞以验证停机寿命顺序。
using DocumentReleaseEnqueueGate = void (*)();
void setDocumentReleaseEnqueueGate(DocumentReleaseEnqueueGate gate) noexcept;

// DocumentModel 析构一开始就调用。探针看到的线程就是 Close 所在的线程。
void noteDocumentDestructorThread();

// 后台 Close 线程。定向测试用来排除 GUI 与全部 producer，不作为生产停机 API。
[[nodiscard]] std::thread::id documentReleaseWorkerThread();

} // namespace steplens::model
