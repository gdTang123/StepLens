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
#include <cfenv>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#if defined(_WIN32)
#include <eh.h>
#include <float.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif
#endif

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

std::atomic<bool (*)()> g_saveHook{nullptr};
std::atomic<void (*)()> g_armHook{nullptr};
std::atomic<bool> g_restoreFailed{false};

void failClosedRestore(const char* where) noexcept {
    g_restoreFailed.store(true, std::memory_order_release);
    std::fprintf(stderr, "ScopedThreadSignal restore failed at %s (fail-closed)\n", where);
    std::fflush(stderr);
    std::terminate();
}

} // namespace

struct OcctBoundary::ScopedThreadSignal::SavedState {
    fenv_t env{};
    bool hasEnv = false;
#if defined(_WIN32)
    unsigned int control = 0;
    bool hasControl = false;
#if defined(_M_X64) || defined(__x86_64__)
    unsigned int mxcsr = 0;
    bool hasMxcsr = false;
#endif
    _se_translator_function translator = nullptr;
    bool hasSeh = false;
#endif
};

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
// MSVC x64：_controlfp_s 只影响 MXCSR，PC/IC 非法，会 assert / invalid-parameter。
// 该调用也不覆盖 DENORMAL 相关位。完整 MXCSR（含 DAZ/FTZ/status/mask）最后用 _mm_setcsr 写回，
// 避免被不完整的 control 字覆盖。
constexpr unsigned kControlFpRestoreMask = _MCW_EM | _MCW_RC | _MCW_DN;
#endif

bool OcctBoundary::ScopedThreadSignal::save(SavedState& state) {
    if (const auto hook = g_saveHook.load(std::memory_order_acquire); hook != nullptr) {
        if (!hook()) {
            return false;
        }
    }
    if (fegetenv(&state.env) != 0) {
        return false;
    }
    state.hasEnv = true;
#if defined(_WIN32)
    if (_controlfp_s(&state.control, 0, 0) != 0) {
        return false;
    }
    state.hasControl = true;
#if defined(_M_X64) || defined(__x86_64__)
    state.mxcsr = _mm_getcsr();
    state.hasMxcsr = true;
#endif
    auto noop = [](unsigned, struct _EXCEPTION_POINTERS*) {};
    const _se_translator_function previous = _set_se_translator(noop);
    state.translator = previous;
    state.hasSeh = true;
    _set_se_translator(previous);
#endif
    return true;
}

bool OcctBoundary::ScopedThreadSignal::restore(const SavedState& state) noexcept {
    bool ok = true;
    if (state.hasEnv && fesetenv(&state.env) != 0) {
        ok = false;
    }
#if defined(_WIN32)
    if (state.hasControl) {
        unsigned int ignored = 0;
#if defined(_M_X64) || defined(__x86_64__)
        if (_controlfp_s(&ignored, state.control, kControlFpRestoreMask) != 0) {
            ok = false;
        }
#else
        // 本补丁只交付 MSVC x64 合法 mask；其他 Windows 架构不用 PC/IC，避免非法参数。
        if (_controlfp_s(&ignored, state.control, _MCW_EM | _MCW_RC | _MCW_DN) != 0) {
            ok = false;
        }
#endif
    }
#if defined(_M_X64) || defined(__x86_64__)
    if (state.hasMxcsr) {
        _mm_setcsr(state.mxcsr);
    }
#endif
    if (state.hasSeh) {
        _set_se_translator(state.translator);
    }
#endif
    return ok;
}

OcctBoundary::ScopedThreadSignal::ScopedThreadSignal() : m_saved(nullptr) {
    // 先分配。bad_alloc 时尚未改 FP/SEH，不得报恢复失败或已污染。
    auto saved = std::unique_ptr<SavedState>(new SavedState());
    if (!save(*saved)) {
        throw std::runtime_error("ScopedThreadSignal: failed to save FP/SEH; not arming");
    }
    try {
        armWorkerThread();
        if (const auto hook = g_armHook.load(std::memory_order_acquire); hook != nullptr) {
            hook();
        }
    } catch (...) {
        if (!restore(*saved)) {
            failClosedRestore("constructor-rollback");
        }
        throw;
    }
    m_saved = saved.release();
}

OcctBoundary::ScopedThreadSignal::~ScopedThreadSignal() noexcept {
    if (m_saved == nullptr) {
        return;
    }
    SavedState* const saved = m_saved;
    m_saved = nullptr;
    const bool ok = restore(*saved);
    delete saved;
    if (!ok) {
        failClosedRestore("destructor");
    }
}

void OcctBoundary::setScopedThreadSignalSaveHookForTesting(bool (*hook)()) {
    g_saveHook.store(hook, std::memory_order_release);
}

void OcctBoundary::setScopedThreadSignalArmHookForTesting(void (*hook)()) {
    g_armHook.store(hook, std::memory_order_release);
}

bool OcctBoundary::scopedThreadSignalRestoreFailed() noexcept {
    return g_restoreFailed.load(std::memory_order_acquire);
}

void OcctBoundary::armWorkerThread() {
    OSD::SetThreadLocalSignal(OSD_SignalMode_Set, false);
    ++t_armCount;
}

int OcctBoundary::armCountOnThisThread() noexcept {
    return t_armCount;
}

#if defined(_WIN32)
static_assert(std::is_same_v<_se_translator_function, OcctBoundary::SehTranslatorForTesting>,
              "exchangeSehTranslatorForTesting must use the CRT _se_translator_function ABI");

OcctBoundary::SehTranslatorForTesting
OcctBoundary::exchangeSehTranslatorForTesting(SehTranslatorForTesting next) {
    return _set_se_translator(next);
}
#endif

std::unique_ptr<TaskScheduler> makeOcctTaskScheduler(unsigned threads) {
    return std::make_unique<TaskScheduler>(threads, [] { OcctBoundary::armWorkerThread(); });
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
