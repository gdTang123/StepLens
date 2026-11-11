#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "model/DocumentModel.h"
#include "model/DocumentRelease.h"
#include "occtsupport/OcctBoundary.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

namespace steplens::model {
namespace {

// 只给退出测试用。后台线程在 reset 之前睡一会儿，main 才能看到 documentsPendingRelease() > 0。
// 生产环境不设这个变量。
int releaseDelayMs() {
#if defined(_WIN32)
    char buffer[16] = {};
    const DWORD count = GetEnvironmentVariableA("STEPLENS_RELEASE_DELAY_MS", buffer, sizeof(buffer));
    if (count == 0 || count >= sizeof(buffer)) {
        return 0;
    }
    const char* text = buffer;
#else
    const char* text = std::getenv("STEPLENS_RELEASE_DELAY_MS");
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
#endif
    int value = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }
        value = value * 10 + (*cursor - '0');
        if (value > 5000) {
            return 5000;
        }
    }
    return value;
}

class DocumentRetirer {
public:
    DocumentRetirer() : m_thread([this] { run(); }) {}

    ~DocumentRetirer() {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    DocumentRetirer(const DocumentRetirer&) = delete;
    DocumentRetirer& operator=(const DocumentRetirer&) = delete;

    void enqueue(std::shared_ptr<const DocumentModel> model) {
        if (!model) {
            return;
        }
        m_pending.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(model));
        }
        m_cv.notify_one();
    }

    void setProbe(std::function<void(std::thread::id)> probe) {
        std::lock_guard lock(m_mutex);
        m_probe = std::move(probe);
    }

    void invokeProbe() {
        std::function<void(std::thread::id)> probe;
        {
            std::lock_guard lock(m_mutex);
            probe = m_probe;
        }
        if (!probe) {
            return;
        }
        try {
            probe(std::this_thread::get_id());
        } catch (...) {
        }
    }

    [[nodiscard]] int pending() const noexcept {
        return m_pending.load(std::memory_order_acquire);
    }

private:
    void run() {
        occt::OcctBoundary::armWorkerThread();
        for (;;) {
            std::shared_ptr<const DocumentModel> model;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty()) {
                    return;
                }
                model = std::move(m_queue.back());
                m_queue.pop_back();
            }
            const int delayMs = releaseDelayMs();
            if (delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
            model.reset();
            m_pending.fetch_sub(1, std::memory_order_acq_rel);
            m_cv.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::shared_ptr<const DocumentModel>> m_queue;
    std::function<void(std::thread::id)> m_probe;
    std::atomic<int> m_pending{0};
    bool m_stop = false;
    std::thread m_thread;
};

DocumentRetirer& retirer() {
    static DocumentRetirer instance;
    return instance;
}

} // namespace

void retireDocument(std::shared_ptr<const DocumentModel> model) {
    retirer().enqueue(std::move(model));
}

int documentsPendingRelease() noexcept {
    return retirer().pending();
}

bool waitForDocumentRelease(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (documentsPendingRelease() != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void setDocumentReleaseProbe(std::function<void(std::thread::id)> probe) {
    retirer().setProbe(std::move(probe));
}

void noteDocumentDestructorThread() {
    retirer().invokeProbe();
}

} // namespace steplens::model
