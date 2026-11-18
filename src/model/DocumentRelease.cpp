#include "model/DocumentRelease.h"

#include "core/LifecycleTrace.h"
#include "io/HeapRelease.h"
#include "model/DocumentModel.h"
#include "occtsupport/OcctBoundary.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace steplens::model {
namespace {

class Lock {
public:
    explicit Lock(std::mutex& mutex) : m_lock(mutex) {}
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    std::unique_lock<std::mutex> m_lock;
};

unsigned releaseDelayMs() noexcept {
    const char* value = nullptr;
#if defined(_WIN32)
    char buffer[32]{};
    const DWORD size = GetEnvironmentVariableA("STEPLENS_RELEASE_DELAY_MS", buffer, sizeof(buffer));
    if (size == 0 || size >= sizeof(buffer)) {
        return 0;
    }
    value = buffer;
#else
    value = std::getenv("STEPLENS_RELEASE_DELAY_MS");
#endif
    if (!value || *value == '\0') {
        return 0;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0') {
        return 0;
    }
    return static_cast<unsigned>(parsed > 5000 ? 5000 : parsed);
}

} // namespace

class Retirer {
public:
    Retirer() : m_thread([this] { run(); }) {}

    ~Retirer() {
        lifecycleTrace("retirer-stop", "pending=" + std::to_string(count()));
        {
            Lock lock(m_mutex);
            m_stopping = true;
        }
        m_condition.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        lifecycleTrace("retirer-join-end");
        // 已准入 producer 覆盖到最后一次通知；归零后不再通知 cv。这里用限时等待，
        // 不把“再读一次计数”当成门闩，也不依赖 inFlight 归零之后的唤醒。
        {
            std::unique_lock lock(m_mutex);
            while (m_inFlight.load(std::memory_order_acquire) != 0) {
                m_condition.wait_for(lock, std::chrono::milliseconds(20),
                                     [this] { return m_inFlight.load(std::memory_order_acquire) == 0; });
            }
        }
        // 不在析构中访问 XCAF/Log：Retirer 可能早于 locApp/Log 构造，
        // 此处取证会变成对已毁依赖的访问。本改动只撤探针，不改变 Close 与静态寿命顺序。
        io::setHeapReleasePendingProbe(nullptr);
        lifecycleTrace("retirer-end");
    }

    void retire(std::shared_ptr<const DocumentModel> model) {
        if (!model) {
            return;
        }
        InFlight active(*this);
        {
            Lock lock(m_mutex);
            markRetiredLocked(const_cast<DocumentModel*>(model.get()));
        }
        model.reset();
    }

    void handoff(std::shared_ptr<const DocumentModel> model) noexcept {
        model.reset();
    }

    void destroy(const DocumentModel* model) noexcept {
        if (!model) {
            return;
        }
        InFlight active(*this);
        auto* mutableModel = const_cast<DocumentModel*>(model);
        if (std::this_thread::get_id() == m_thread.get_id()) {
            {
                Lock lock(m_mutex);
                markRetiredLocked(mutableModel);
            }
            finish(model);
            return;
        }
        DocumentReleaseEnqueueGate gate = nullptr;
        {
            Lock lock(m_mutex);
            markRetiredLocked(mutableModel);
            mutableModel->m_releaseNext = nullptr;
            if (m_tail) {
                m_tail->m_releaseNext = mutableModel;
            } else {
                m_head = mutableModel;
            }
            m_tail = mutableModel;
            gate = m_enqueueGate.load(std::memory_order_acquire);
        }
        lifecycleTrace("document-enqueue", std::to_string(reinterpret_cast<std::uintptr_t>(model)));
        // 测试门和通知都在锁外；InFlight 覆盖回调与 notify 返回。
        if (gate) {
            try {
                gate();
            } catch (...) {
            }
        }
        lifecycleTrace("producer-notify");
        m_condition.notify_one();
        lifecycleTrace("producer-notify-returned");
    }

    int count() const noexcept {
        return m_pending.load(std::memory_order_acquire);
    }

    bool wait(std::chrono::milliseconds timeout) {
        // pending 的线性化点是 markRetiredLocked / finish 持有的同一把队列锁。
        std::unique_lock lock(m_mutex);
        return m_condition.wait_for(lock, timeout, [this] { return count() == 0; });
    }

    void setProbe(std::function<void(std::thread::id)> probe) {
        std::function<void(std::thread::id)> old;
        {
            Lock lock(m_mutex);
            old = std::move(m_probe);
            m_probe = std::move(probe);
        }
    }

    void setGate(DocumentReleaseGate gate) noexcept {
        m_gate.store(gate, std::memory_order_release);
    }

    void setEnqueueGate(DocumentReleaseEnqueueGate gate) noexcept {
        m_enqueueGate.store(gate, std::memory_order_release);
    }

    void noteDestructorThread() {
        std::function<void(std::thread::id)> probe;
        {
            Lock lock(m_mutex);
            probe = m_probe;
        }
        if (probe) {
            try {
                probe(std::this_thread::get_id());
            } catch (...) {
            }
        }
    }

    void constructed() noexcept {
        m_live.fetch_add(1, std::memory_order_release);
    }

    std::thread::id workerThread() const noexcept {
        return m_thread.get_id();
    }

private:
    class InFlight {
    public:
        explicit InFlight(Retirer& owner) : m_owner(owner) {
            // 必须先于 producer 的首次 mutex/cv 访问登记；停机锁失败或对象不可恢复时
            // destroy() 为 noexcept，标准策略是终止进程，不在 GUI 线程兜底删除模型。
            m_owner.m_inFlight.fetch_add(1, std::memory_order_acq_rel);
        }
        ~InFlight() {
            // 最后一次 service 访问是 notify；必须在 notify 返回之后才撤销准入。
            // 归零后不再碰 cv，避免与 ~Retirer 拆成员竞态。
            m_owner.m_condition.notify_all();
            lifecycleTrace("producer-inflight-release");
            m_owner.m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
        }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;

    private:
        Retirer& m_owner;
    };

    void markRetiredLocked(DocumentModel* model) noexcept {
        // 同一把队列锁内首次退役才记一票；先加票再标记，避免无锁观察看到 retired 已发布而 pending 仍为 0。
        if (!model->m_retired.load(std::memory_order_relaxed)) {
            m_pending.fetch_add(1, std::memory_order_release);
            model->m_retired.store(true, std::memory_order_release);
        }
    }

    void finish(const DocumentModel* model) noexcept {
        const unsigned delay = releaseDelayMs();
        if (delay != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        if (auto gate = m_gate.load(std::memory_order_acquire)) {
            try {
                gate();
            } catch (...) {
            }
        }
        delete model;
        {
            Lock lock(m_mutex);
            m_pending.fetch_sub(1, std::memory_order_acq_rel);
            m_live.fetch_sub(1, std::memory_order_acq_rel);
        }
        m_condition.notify_all();
    }

    void run() noexcept {
        occt::OcctBoundary::armWorkerThread();
        for (;;) {
            DocumentModel* batch = nullptr;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock, [this] {
                    return m_head != nullptr || (m_stopping && m_live.load(std::memory_order_acquire) == 0);
                });
                if (!m_head && m_stopping && m_live.load(std::memory_order_acquire) == 0) {
                    return;
                }
                batch = m_head;
                m_head = nullptr;
                m_tail = nullptr;
            }
            while (batch) {
                DocumentModel* next = batch->m_releaseNext;
                batch->m_releaseNext = nullptr;
                finish(batch);
                batch = next;
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_condition;
    DocumentModel* m_head = nullptr;
    DocumentModel* m_tail = nullptr;
    std::atomic<int> m_inFlight{0};
    std::function<void(std::thread::id)> m_probe;
    std::atomic<DocumentReleaseGate> m_gate{nullptr};
    std::atomic<DocumentReleaseEnqueueGate> m_enqueueGate{nullptr};
    std::atomic<int> m_pending{0};
    std::atomic<int> m_live{0};
    bool m_stopping = false;
    std::thread m_thread;
};

namespace {
// instance() 是首次 noteDocumentConstructed/retire/destroy/wait 时建立的 function-local static。
// OCCT V7_8_1 XCAFApp_Application.cxx 的 GetApplication() 使用 static Handle locApp；
// Log.cpp 的 state() 使用 function-local static LogState。构造顺序取决于首次调用，
// 静态析构按已完成构造的反序执行。常见产品路径 NewDocument/GetApplication 先于模型
// ctor/instance()，故 Retirer 析构时 locApp 仍在；测试若先 waitForDocumentRelease 则
// 顺序相反。跨 TU 没有更强保证。正常 return 且 live/pending 已归零时，Retirer 只回收
// 空闲线程，不应再 Close；pending>0 时 main.cpp 用 quick_exit/TerminateProcess 跳过
// 静态析构。真实 MainWindow 成员/子树析构顺序由 STEPLENS_LIFECYCLE_LOG 取证；QObject
// 仿真树和 ReleaseQuickExitMain 不覆盖主窗退出。~Retirer 时也不应 new 模型，若发生属于契约外。
Retirer& instance() {
    static Retirer retirer;
    return retirer;
}

int heapPending() noexcept {
    return instance().count();
}
struct RegisterHeapProbe {
    RegisterHeapProbe() {
        io::setHeapReleasePendingProbe(heapPending);
    }
} registerHeapProbe; // 与 instance() 同 TU：动态初始化先于 Retirer，析构晚于 Retirer。

} // namespace

void retireDocument(std::shared_ptr<const DocumentModel> model) {
    instance().retire(std::move(model));
}
void handoffDocument(std::shared_ptr<const DocumentModel> model) noexcept {
    instance().handoff(std::move(model));
}
void destroyDocumentModel(const DocumentModel* model) noexcept {
    instance().destroy(model);
}
void noteDocumentConstructed() noexcept {
    instance().constructed();
}
int documentsPendingRelease() noexcept {
    return instance().count();
}
bool waitForDocumentRelease(std::chrono::milliseconds timeout) {
    return instance().wait(timeout);
}
void setDocumentReleaseProbe(std::function<void(std::thread::id)> probe) {
    instance().setProbe(std::move(probe));
}
void setDocumentReleaseGate(DocumentReleaseGate gate) noexcept {
    instance().setGate(gate);
}
void setDocumentReleaseEnqueueGate(DocumentReleaseEnqueueGate gate) noexcept {
    instance().setEnqueueGate(gate);
}
void noteDocumentDestructorThread() {
    instance().noteDestructorThread();
}
std::thread::id documentReleaseWorkerThread() {
    return instance().workerThread();
}

} // namespace steplens::model
