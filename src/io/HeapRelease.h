#pragma once

// 把已经 free 掉的堆页还给操作系统。
// Windows：_heapmin()（<malloc.h>）。
// glibc：malloc_trim(0)。其它 libc 没有等价接口，调用是空操作。
// 后台还有文档没 Close 完时不 trim，留给 LoadPipeline::tick 在归零后做。

namespace steplens::io {

// 返回还在后台释放队列里的文档份数。没注册时 releaseFreedHeap 照常回收。
using HeapReleasePendingProbe = int (*)() noexcept;

void setHeapReleasePendingProbe(HeapReleasePendingProbe probe) noexcept;

void releaseFreedHeap() noexcept;

} // namespace steplens::io
