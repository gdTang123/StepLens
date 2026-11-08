#pragma once

#include <mutex>

namespace steplens {

// T05 接口约定（G1-4）。core 不链接 OCCT，这里只钉住调用规则，不实现 StepLoader。
//
// Interface_Static：
// 全局静态参数只能在 main() 里、任何工作线程出现之前初始化一次。
// initializeInterfaceStaticOnMainThread() 再调用一次就抛 std::logic_error。
// 这里不调用 Interface_Static::SetIVal / SetCVal。加载线程也不许改这些全局参数。
//
// XCAFApp：
// XCAFApp_Application::GetApplication()->NewDocument 和 Close 必须串行，
// 并且只用 xcafDocumentMutex() 这一把进程内的锁。不要再各写一把。

void initializeInterfaceStaticOnMainThread();

[[nodiscard]] std::mutex& xcafDocumentMutex();

} // namespace steplens
