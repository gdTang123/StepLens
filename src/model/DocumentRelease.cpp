#include "model/DocumentRelease.h"

#include "io/HeapRelease.h"
#include "model/DocumentModel.h"
#include "occtsupport/OcctBoundary.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
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
        {
            Lock lock(m_mutex);
            m_stopping = true;
        }
        m_condition.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        // 必须在 join 之后再摘掉堆探针：join 期间仍可能 Close，过早清空会让
        // releaseFreedHeap 在 pending>0 时 trim。registerHeapProbe 析构晚于 Retirer，
        // 若不摘掉，之后的 releaseFreedHeap 会进入已销毁的 function-local instance()。
        io::setHeapReleasePendingProbe(nullptr);
    }

    void retire(std::shared_ptr<const DocumentModel> model) {
        if (!model) {
            return;
        }
        markRetired(const_cast<DocumentModel*>(model.get()));
        model.reset();
    }

    void handoff(std::shared_ptr<const DocumentModel> model) noexcept {
        model.reset();
    }

    void destroy(const DocumentModel* model) noexcept {
        if (!model) {
            return;
        }
        auto* mutableModel = const_cast<DocumentModel*>(model);
        markRetired(mutableModel);
        if (std::this_thread::get_id() == m_thread.get_id()) {
            finish(model);
            return;
        }
        // 必要性：destroy 是 noexcept，可能在 GUI 最后引用上执行；mutex.lock 可能抛，
        // 旧的无限重试会让 GUI 忙转并掩盖永久错误。无分配 CAS 入队也覆盖原 deque
        // 的分配失败。复杂度高于 mutex 链表，只为这条释放路径。
        // 算法：先 markRetired；非 retirer 线程 CAS 推入 m_incoming。retirer 用
        // exchange(nullptr) 整批摘头，waitForPushers（activePushers==0）后才反转并
        // finish/delete。activePushers 以 seq_cst 包住 CAS 窗口，避免摘头后节点释放、
        // 地址复用再入栈导致 ABA。通知若丢失，后台 wait_for 短超时兜底。
        m_activePushers.fetch_add(1, std::memory_order_seq_cst);
        DocumentModel* head = m_incoming.load(std::memory_order_seq_cst);
        do {
            mutableModel->m_releaseNext = head;
        } while (!m_incoming.compare_exchange_weak(head, mutableModel, std::memory_order_release,
                                                   std::memory_order_relaxed));
        if (m_activePushers.fetch_sub(1, std::memory_order_seq_cst) == 1) {
            m_condition.notify_all();
        }
        m_condition.notify_one();
    }

    int count() const noexcept {
        return m_pending.load(std::memory_order_acquire);
    }

    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_mutex);
        return m_condition.wait_for(lock, timeout, [this] { return count() == 0; });
    }

    void setProbe(std::function<void(std::thread::id)> probe) {
        Lock lock(m_mutex);
        m_probe = std::move(probe);
    }

    void setGate(DocumentReleaseGate gate) noexcept {
        m_gate.store(gate, std::memory_order_release);
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

private:
    void markRetired(DocumentModel* model) noexcept {
        // 先 tentative 记票再发布 retired，避免 wait/isSettled 在 retired 已可见而 pending=0 时提前成功。
        // CAS 失败时撤销临时票；service 生命周期也因此只会在真实释放完成后观察到归零。
        m_pending.fetch_add(1, std::memory_order_acq_rel);
        bool expected = false;
        if (!model->m_retired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            m_pending.fetch_sub(1, std::memory_order_acq_rel);
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
        // 在同一把 retirer 锁上减票再 notify，避免 wait/drain 丢失唤醒。
        {
            Lock lock(m_mutex);
            m_pending.fetch_sub(1, std::memory_order_acq_rel);
            // finish 只在模型析构（含 destroy/Close）完成后减 live。
            m_live.fetch_sub(1, std::memory_order_acq_rel);
        }
        m_condition.notify_all();
    }

    void waitForPushers() {
        // 旧批次可能被正在 CAS 的入队者暂存；等待只发生在 retirer，超时覆盖无锁 notify 的竞态。
        std::unique_lock lock(m_mutex);
        while (m_activePushers.load(std::memory_order_seq_cst) != 0) {
            m_condition.wait_for(lock, std::chrono::milliseconds(20));
        }
    }

    void run() noexcept {
        occt::OcctBoundary::armWorkerThread();
        for (;;) {
            DocumentModel* model = nullptr;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait_for(lock, std::chrono::milliseconds(20), [this] {
                    return m_incoming.load(std::memory_order_acquire) ||
                           (m_stopping && m_live.load(std::memory_order_acquire) == 0);
                });
                if (!m_incoming.load(std::memory_order_acquire) && m_stopping &&
                    m_live.load(std::memory_order_acquire) == 0 &&
                    m_activePushers.load(std::memory_order_seq_cst) == 0) {
                    return;
                }
            }
            model = m_incoming.exchange(nullptr, std::memory_order_seq_cst);
            if (!model)
                continue;
            waitForPushers();
            // 反转 CAS 栈，维持一批入队的 FIFO 回收次序。
            DocumentModel* reversed = nullptr;
            while (model) {
                DocumentModel* next = model->m_releaseNext;
                model->m_releaseNext = reversed;
                reversed = model;
                model = next;
            }
            model = reversed;
            while (model) {
                DocumentModel* next = model->m_releaseNext;
                finish(model);
                model = next;
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::atomic<DocumentModel*> m_incoming{nullptr};
    std::atomic<unsigned> m_activePushers{0};
    std::function<void(std::thread::id)> m_probe;
    std::atomic<DocumentReleaseGate> m_gate{nullptr};
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
// 静态析构。真实 MainWindow 成员/子树析构顺序本机尚未验证；QObject 仿真树和快退子进程
// 不覆盖主窗退出。~Retirer 时也不应 new 模型，若发生属于契约外。
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
void noteDocumentDestructorThread() {
    instance().noteDestructorThread();
}

} // namespace steplens::model
