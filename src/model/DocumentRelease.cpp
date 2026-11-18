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

void lockWithoutThrow(std::mutex& mutex) noexcept {
    for (;;) {
        try {
            mutex.lock();
            return;
        } catch (...) {
            std::this_thread::yield();
        }
    }
}

class Lock {
public:
    explicit Lock(std::mutex& mutex) noexcept : m_mutex(mutex) {
        lockWithoutThrow(m_mutex);
    }
    ~Lock() {
        m_mutex.unlock();
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    std::mutex& m_mutex;
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
    }

    void retire(std::shared_ptr<const DocumentModel> model) {
        if (!model) {
            return;
        }
        bool expected = false;
        if (model->m_retired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            m_pending.fetch_add(1, std::memory_order_release);
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
        auto* mutableModel = const_cast<DocumentModel*>(model);
        bool expected = false;
        if (mutableModel->m_retired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            m_pending.fetch_add(1, std::memory_order_release);
        }
        if (std::this_thread::get_id() == m_thread.get_id()) {
            finish(model);
            return;
        }
        {
            Lock lock(m_mutex);
            mutableModel->m_releaseNext = nullptr;
            if (m_tail) {
                m_tail->m_releaseNext = mutableModel;
            } else {
                m_head = mutableModel;
            }
            m_tail = mutableModel;
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
            m_live.fetch_sub(1, std::memory_order_acq_rel);
        }
        m_condition.notify_all();
    }

    void run() noexcept {
        occt::OcctBoundary::armWorkerThread();
        for (;;) {
            const DocumentModel* model = nullptr;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock, [this] {
                    return m_head || (m_stopping && m_live.load(std::memory_order_acquire) == 0);
                });
                if (!m_head && m_stopping && m_live.load(std::memory_order_acquire) == 0) {
                    return;
                }
                model = m_head;
                m_head = model->m_releaseNext;
                if (!m_head) {
                    m_tail = nullptr;
                }
            }
            finish(model);
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_condition;
    DocumentModel* m_head = nullptr;
    DocumentModel* m_tail = nullptr;
    std::function<void(std::thread::id)> m_probe;
    std::atomic<DocumentReleaseGate> m_gate{nullptr};
    std::atomic<int> m_pending{0};
    std::atomic<int> m_live{0};
    bool m_stopping = false;
    std::thread m_thread;
};

namespace {
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
} registerHeapProbe;

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
