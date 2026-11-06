#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "occtsupport/OcctBoundary.h"

#include "occtsupport/occt_compat.h"

#include <OSD.hxx>
#include <OSD_Exception_ACCESS_VIOLATION.hxx>

#include <atomic>
#include <exception>
#include <string>
#include <string_view>

namespace steplens::occt {
namespace {

std::atomic<bool>& contaminatedFlag() noexcept {
    static std::atomic<bool> flag{false};
    return flag;
}

void markProcessContaminated() noexcept {
    contaminatedFlag().store(true, std::memory_order_release);
}

Result<int> contaminatedResult(const Standard_Failure& failure) {
    markProcessContaminated();
    return Error{ErrorCode::Internal, failureMessage(failure), std::string{kProcessContaminatedUserMessage}};
}

} // namespace

bool processContaminated() noexcept {
    return contaminatedFlag().load(std::memory_order_acquire);
}

namespace {

// 每个工作线程自己的计数。入口钩子跑过之后，该线程的第一个任务才能看到非 0。
thread_local int t_armCount = 0;

} // namespace

void OcctBoundary::armWorkerThread() {
    OSD::SetThreadLocalSignal(OSD_SignalMode_Set, false);
    ++t_armCount;
}

int OcctBoundary::armCountOnThisThread() noexcept {
    return t_armCount;
}

#if defined(_MSC_VER)
#pragma optimize("", off)
#endif
Result<int> OcctBoundary::probeNullPointerAccess() {
    // 调用方应在线程入口已经 arm。这里再调用一次，漏掉入口时仍能把访问违例转成异常。
    // try 和空指针写在同一个 /EHa 函数里，避免异常穿过别的翻译单元。
    armWorkerThread();
    try {
        constexpr int kProbeWrite = 1;
        int* pointer = nullptr;
        *pointer = kProbeWrite;
        return 1;
    } catch (const OSD_Exception_ACCESS_VIOLATION& failure) {
        return contaminatedResult(failure);
    } catch (const Standard_Failure& failure) {
        const char* text = failureMessage(failure);
        if (std::string_view{text}.find("ACCESS VIOLATION") != std::string_view::npos) {
            return contaminatedResult(failure);
        }
        return Error{ErrorCode::Internal, text, "内部错误"};
    } catch (const std::exception& ex) {
        const char* text = ex.what();
        if (text == nullptr || text[0] == '\0') {
            text = "unknown";
        }
        return Error{ErrorCode::Internal, text, "内部错误"};
    } catch (...) {
        return Error{ErrorCode::Internal, "unknown exception", "内部错误"};
    }
}
#if defined(_MSC_VER)
#pragma optimize("", on)
#endif

} // namespace steplens::occt
