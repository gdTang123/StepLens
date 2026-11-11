#pragma once

// 把已经 free 掉的堆页还给操作系统。
// Windows：_heapmin()（<malloc.h>）。
// glibc：malloc_trim(0)。其它 libc 没有等价接口，调用是空操作。
// 读取器析构和文档关闭都会走这里。效果见 T16 对 U10 的测量。

namespace steplens::io {

void releaseFreedHeap() noexcept;

} // namespace steplens::io
