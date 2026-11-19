// 最小受控实验（非 StepLens 测试、不含 OCCT）：
// 函数期静态对象析构里 notify_all + join 一个在 CV 上等待的 worker；
// 另有 detached 生产者在入队后、notify 前停在门闩上，join 之后才放行。
// 若仅此结构在 Windows 上 3s 内不能退出，则问题在静态析构/CV/join，不必先怪 XCAF Close。
// 若此结构能退出而完整测试不能，再把怀疑放到 worker 醒来后的 finish/Close。
// 本文件不改产品、不提前放行、不加测试超时到产品测试里。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {
std::atomic<bool> release_gate{false};
std::atomic<bool> idle_in_wait{false};
std::atomic<bool> enqueued{false};
std::atomic<bool> producer_log_done{false};
std::atomic<int> inflight{0};
std::atomic<int> live{0};
int* head = nullptr;
bool stopping = false;
std::mutex mu;
std::condition_variable cv;
FILE* logf = nullptr;

void emit(const char* ev, int headSnap, int stopSnap, FILE* out) {
    if (!out) {
        return;
    }
    std::fprintf(out, "event=%s inflight=%d live=%d head=%d stopping=%d\n", ev, inflight.load(),
                 live.load(), headSnap, stopSnap);
    std::fflush(out);
}

// Must already hold mu. Used on the worker wait path so we do not re-lock.
void traceHeld(const char* ev) {
    emit(ev, head != nullptr ? 1 : 0, stopping ? 1 : 0, logf);
}

void trace(const char* ev) {
    FILE* out = nullptr;
    int headSnap = 0;
    int stopSnap = 0;
    {
        std::lock_guard lock(mu);
        out = logf;
        headSnap = head != nullptr ? 1 : 0;
        stopSnap = stopping ? 1 : 0;
    }
    if (!out) {
        return;
    }
    std::fprintf(out, "event=%s inflight=%d live=%d head=%d stopping=%d\n", ev, inflight.load(), live.load(),
                 headSnap, stopSnap);
    std::fflush(out);
}

void closeLog() {
    FILE* out = nullptr;
    {
        std::lock_guard lock(mu);
        out = logf;
        logf = nullptr;
    }
    if (out) {
        std::fclose(out);
    }
}

struct Retirer {
    Retirer() : worker([this] { run(); }) {}
    ~Retirer() {
        trace("retirer-stop");
        {
            std::lock_guard lock(mu);
            stopping = true;
        }
        cv.notify_all();
        trace("retirer-join-begin");
        if (worker.joinable())
            worker.join();
        trace("retirer-join-end");
        release_gate.store(true, std::memory_order_release);
        trace("gate-release");
        std::unique_lock lock(mu);
        while (inflight.load(std::memory_order_acquire) != 0) {
            cv.wait_for(lock, std::chrono::milliseconds(20),
                        [] { return inflight.load(std::memory_order_acquire) == 0; });
        }
        lock.unlock();
        trace("retirer-end");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!producer_log_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        closeLog();
    }
    void run() {
        for (;;) {
            int* batch = nullptr;
            {
                std::unique_lock lock(mu);
                traceHeld("worker-wait-enter");
                idle_in_wait.store(true, std::memory_order_release);
                cv.wait(lock, [] { return head != nullptr || (stopping && live.load() == 0); });
                idle_in_wait.store(false, std::memory_order_release);
                traceHeld("worker-wait-leave");
                if (!head && stopping && live.load() == 0)
                    return;
                batch = head;
                head = nullptr;
            }
            // 刻意不做 Close；只模拟“取走队列项并结束一份 live”。
            delete batch;
            live.fetch_sub(1, std::memory_order_acq_rel);
            cv.notify_all();
            trace("worker-finish-leave");
        }
    }
    std::thread worker;
};

Retirer& instance() {
    static Retirer r;
    return r;
}

void producer() {
    inflight.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(mu);
        head = new int{1};
        enqueued.store(true, std::memory_order_release);
    }
    trace("producer-enqueue-gate");
    while (!release_gate.load(std::memory_order_acquire))
        std::this_thread::yield();
    trace("producer-gate-release");
    cv.notify_one();
    inflight.fetch_sub(1, std::memory_order_acq_rel);
    cv.notify_all();
    trace("producer-notify-returned");
    producer_log_done.store(true, std::memory_order_release);
}
} // namespace

int main() {
    const char* path = std::getenv("STEPLENS_DIAG_EXPERIMENT_LOG");
    if (path == nullptr || *path == '\0') {
        path = "static-dtor-join-gate.log";
    }
    FILE* opened = std::fopen(path, "w");
    if (!opened) {
        std::fprintf(stderr, "failed to open experiment log: %s\n", path);
        return 2;
    }
    {
        std::lock_guard lock(mu);
        logf = opened;
    }
    instance();
    live.store(1, std::memory_order_release);
    while (!idle_in_wait.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::thread(producer).detach();
    while (!enqueued.load(std::memory_order_acquire))
        std::this_thread::yield();
    // 给 worker 一个时间窗：若它在无 notify 时因谓词重入而取走行，会在 stop 前把 live 打成 0。
    // 产品测试里 worker 应已睡在 wait 中，只能靠 ~Retirer 的 notify_all 醒来。
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    trace("main-return-to-static-dtors");
    return 0; // 进入静态析构：~Retirer；不在此处关闭日志句柄。
}
