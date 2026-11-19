// 最小受控实验（非 StepLens 测试、不含 OCCT）：
// 函数期静态对象析构里 notify_all + join 一个在 CV 上等待的 worker；
// 生产者线程由 Retirer 持有：join worker、放行门闩、join producer 之后才关闭日志。
// 不 detach、不用限时猜测 fclose。不提前放行、不加测试超时到产品测试里。
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
std::atomic<int> inflight{0};
std::atomic<int> live{0};
int* head = nullptr;
bool stopping = false;
std::mutex mu;
std::condition_variable cv;
FILE* logf = nullptr;

// All logging holds mu for the entire fprintf. No FILE* is used outside the lock.
void emitHeld(const char* ev) {
    if (!logf) {
        return;
    }
    std::fprintf(logf, "event=%s inflight=%d live=%d head=%d stopping=%d\n", ev, inflight.load(),
                 live.load(), head != nullptr ? 1 : 0, stopping ? 1 : 0);
    std::fflush(logf);
}

void traceHeld(const char* ev) {
    emitHeld(ev);
}

void trace(const char* ev) {
    std::lock_guard lock(mu);
    emitHeld(ev);
}

void closeLog() {
    std::lock_guard lock(mu);
    if (logf) {
        std::fclose(logf);
        logf = nullptr;
    }
}

void producer();

struct Retirer {
    Retirer() : worker([this] { run(); }) {}
    void startProducer() {
        producer_thread = std::thread(producer);
    }
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
        if (producer_thread.joinable())
            producer_thread.join();
        trace("retirer-end");
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
            delete batch;
            live.fetch_sub(1, std::memory_order_acq_rel);
            cv.notify_all();
            trace("worker-finish-leave");
        }
    }
    std::thread worker;
    std::thread producer_thread;
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
    instance().startProducer();
    while (!enqueued.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    trace("main-return-to-static-dtors");
    return 0;
}
