#pragma once

// 最后一份 DocumentModel 的析构会 Close XCAF 文档。X-02 上这一步大约 1.2–1.9 秒，
// 不能放在 GUI 线程。调用方把 shared_ptr 交进来，后台线程再丢掉这份引用。
// 不看 use_count：调用方这份一定在后台释放，避免在 GUI 上变成最后一份。
// documentsPendingRelease() 是还在队列里、尚未 reset 的份数，算进“还活着的文档”。

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace steplens::model {

class DocumentModel;

void retireDocument(std::shared_ptr<const DocumentModel> model);

[[nodiscard]] int documentsPendingRelease() noexcept;

// 等到后台 Close 结束。超时返回 false。
[[nodiscard]] bool waitForDocumentRelease(std::chrono::milliseconds timeout);

// 测试缝。在后台线程、真正 reset shared_ptr 之前调用。可以堵住，用来制造“释放还没完”。
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
