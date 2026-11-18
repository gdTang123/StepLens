// 非 MSVC 宿主编译检查：对 OcctBoundary.h 的 _WIN32 声明启用全局 struct 标签兼容。
// 本文件由 CMake 在非 MSVC 目标上定义 _WIN32 后单独编译，用来抓住类体内
// 前向声明造成的 OcctBoundary::_EXCEPTION_POINTERS 嵌套类型。
//
// 受控边界（明示，不是 Windows SDK / eh.h 替代品）：
// - 不包含 <eh.h>，不声明 _se_translator_function，不安装名为 eh.h 的同形头。
// - 只用头文件自己的全局 struct _EXCEPTION_POINTERS 前向声明，以及 ::_EXCEPTION_POINTERS。
// - 不验证 __cdecl、MSVC CRT ABI，也不能代替在真实 <eh.h> 下对
//   _se_translator_function 的 std::is_same 检查（那份检查在 OcctBoundary.cpp
//   与 Windows 测试 TU 中，本机无 MSVC / 无 eh.h，未在本宿主执行）。
// - 验证：交换 API 的函数指针与全局标签相同；sehSentinel 和无捕获 lambda 可传入。

#if !defined(_WIN32)
#error "SehTranslatorGlobalTagCheck.cpp must be compiled with _WIN32"
#endif

#include "occtsupport/OcctBoundary.h"

#include <type_traits>

void sehSentinel(unsigned, struct ::_EXCEPTION_POINTERS*) {}

static_assert(std::is_same_v<steplens::occt::OcctBoundary::SehTranslatorForTesting,
                             void (*)(unsigned, struct ::_EXCEPTION_POINTERS*)>);
static_assert(
    std::is_convertible_v<decltype(&sehSentinel), steplens::occt::OcctBoundary::SehTranslatorForTesting>);

[[maybe_unused]] const steplens::occt::OcctBoundary::SehTranslatorForTesting kSentinelFn = sehSentinel;
[[maybe_unused]] const steplens::occt::OcctBoundary::SehTranslatorForTesting kNoopFn =
    [](unsigned, struct ::_EXCEPTION_POINTERS*) {};
