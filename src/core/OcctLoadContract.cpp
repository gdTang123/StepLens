#include "core/OcctLoadContract.h"

#include <atomic>
#include <stdexcept>

namespace steplens {
namespace {

std::atomic<int> g_interfaceStaticState{0};

} // namespace

void initializeInterfaceStaticOnMainThread() {
    int expected = 0;
    if (!g_interfaceStaticState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        throw std::logic_error("Interface_Static 只能在 main() 单线程阶段初始化一次");
    }
}

std::mutex& xcafDocumentMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace steplens
