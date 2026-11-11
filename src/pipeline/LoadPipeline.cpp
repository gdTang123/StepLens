#include "pipeline/LoadPipeline.h"

#include "core/LoadCoordinator.h"
#include "core/LoadSession.h"
#include "core/Log.h"
#include "io/ILoader.h"
#include "mesh/MeshPolicy.h"
#include "mesh/Tessellator.h"
#include "model/DocumentModel.h"
#include "model/DocumentRelease.h"
#include "occtsupport/OcctBoundary.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace steplens {
namespace {

constexpr auto kStallBudget = std::chrono::seconds(3);
constexpr auto kPauseSlice = std::chrono::milliseconds(50);

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// 协调器在替换尚未开始的请求时会直接丢掉 lambda。
// 计数放在 shared_ptr 的析构里，没跑到的请求也会减回去，GUI 才不会一直等。
struct JobTicket {
    std::shared_ptr<std::atomic<int>> live;

    JobTicket() = default;
    explicit JobTicket(std::shared_ptr<std::atomic<int>> counter) : live(std::move(counter)) {}

    JobTicket(const JobTicket&) = delete;
    JobTicket& operator=(const JobTicket&) = delete;

    JobTicket(JobTicket&& other) noexcept : live(std::move(other.live)) {}
    JobTicket& operator=(JobTicket&& other) noexcept {
        if (this != &other) {
            release();
            live = std::move(other.live);
        }
        return *this;
    }

    ~JobTicket() {
        release();
    }

    void release() noexcept {
        if (!live) {
            return;
        }
        live->fetch_sub(1, std::memory_order_acq_rel);
        live.reset();
    }
};

} // namespace

struct LoadPipeline::State {
    // coordinator 放在最后：析构时先 join 加载线程，这时锁和队列都还在。
    std::mutex mutex;
    std::shared_ptr<std::atomic<int>> live = std::make_shared<std::atomic<int>>(0);
    std::uint64_t generation = 0;
    std::uint64_t doneGeneration = 0;
    bool success = false;
    bool adopted = false;
    bool haveDisplay = false;
    bool hasModel = false;
    LoadActivity activity = LoadActivity::Idle;
    std::filesystem::path requestFile;
    std::filesystem::path displayedFile;
    std::shared_ptr<LoadSession> session;
    std::shared_ptr<const model::DocumentModel> model;
    std::vector<mesh::MeshBatch> batches;
    std::optional<Error> failure;
    std::function<void(const CancelToken&)> probe;
    bool failAfterPublish = false;
    LoadCoordinator coordinator;
};

LoadPipeline::LoadPipeline(SceneHost& host) : m_host(host), m_state(std::make_unique<State>()) {}

LoadPipeline::~LoadPipeline() {
    std::shared_ptr<LoadSession> session;
    std::shared_ptr<const model::DocumentModel> dropped;
    {
        std::lock_guard lock(m_state->mutex);
        ++m_state->generation;
        session = std::move(m_state->session);
        dropped = std::move(m_state->model);
        m_state->hasModel = false;
        m_state->batches.clear();
    }
    model::retireDocument(std::move(dropped));
    if (session) {
        session->cancel();
    }
}

bool LoadPipeline::generationCurrent(std::uint64_t generation) const {
    std::lock_guard lock(m_state->mutex);
    return generation == m_state->generation;
}

void LoadPipeline::finishWorker(std::uint64_t generation, bool ok, std::optional<Error> error) {
    std::lock_guard lock(m_state->mutex);
    if (generation != m_state->generation) {
        return;
    }
    if (error.has_value()) {
        m_state->failure = std::move(error);
        m_state->success = false;
    } else {
        m_state->success = ok;
        if (ok) {
            m_state->failure.reset();
        }
    }
    m_state->doneGeneration = generation;
}

void LoadPipeline::setBeforePublishForTest(std::function<void(const CancelToken&)> probe) {
    std::lock_guard lock(m_state->mutex);
    m_state->probe = std::move(probe);
}

void LoadPipeline::setFailAfterPublishForTest(bool fail) {
    std::lock_guard lock(m_state->mutex);
    m_state->failAfterPublish = fail;
}

void LoadPipeline::open(const std::filesystem::path& file, std::chrono::milliseconds startPause) {
    std::error_code existsCode;
    const bool regular = std::filesystem::is_regular_file(file, existsCode);
    if (!regular || existsCode) {
        std::lock_guard lock(m_state->mutex);
        m_state->failure =
            Error{ErrorCode::FileNotFound, "not a regular file", "找不到文件，或这不是 STEP/IGES 文件"};
        return;
    }

    std::error_code sizeCode;
    const auto bytesOnDisk = std::filesystem::file_size(file, sizeCode);
    const auto bytes = sizeCode ? std::uint64_t{0} : static_cast<std::uint64_t>(bytesOnDisk);

    std::shared_ptr<LoadSession> previous;
    std::uint64_t generation = 0;
    std::shared_ptr<JobTicket> ticket;
    {
        std::lock_guard lock(m_state->mutex);
        generation = ++m_state->generation;
        previous = std::move(m_state->session);
        m_state->requestFile = file;
        m_state->success = false;
        m_state->hasModel = false;
        m_state->model.reset();
        m_state->batches.clear();
        m_state->failure.reset();
        m_state->adopted = false;
        m_state->activity = LoadActivity::Loading;
        m_state->live->fetch_add(1, std::memory_order_acq_rel);
        ticket = std::make_shared<JobTicket>(m_state->live);
    }
    if (previous) {
        previous->cancel();
    }

    Log::info(std::string("打开 ") + pathUtf8(file));
    const auto pause = startPause;
    m_state->coordinator.submit(LoadCoordinator::LoadRequest{
        bytes, {}, [this, generation, file, pause, ticket](const std::shared_ptr<LoadSession>& session) {
            std::optional<Error> error;
            bool ok = false;
            bool publish = true;
            try {
                {
                    std::lock_guard lock(m_state->mutex);
                    if (generation != m_state->generation) {
                        publish = false;
                    } else {
                        m_state->session = session;
                    }
                }
                while (publish && model::documentsPendingRelease() > 0) {
                    if (!generationCurrent(generation) || session->token().isCancelled()) {
                        publish = false;
                        break;
                    }
                    std::this_thread::sleep_for(kPauseSlice);
                }
                auto left = pause;
                while (publish && left.count() > 0) {
                    if (!generationCurrent(generation) || session->token().isCancelled()) {
                        publish = false;
                        break;
                    }
                    const auto slice = left < kPauseSlice ? left : kPauseSlice;
                    std::this_thread::sleep_for(slice);
                    left -= slice;
                }
                if (!publish || !generationCurrent(generation) || session->token().isCancelled()) {
                    publish = false;
                } else {
                    session->progress().setStage(LoadStage::Reading);
                    auto loader = io::makeLoader(file);
                    auto loaded = loader->load(file, {}, session->progress(), session->token());
                    if (!generationCurrent(generation) || session->token().isCancelled() ||
                        (!loaded.ok() && loaded.error().code == ErrorCode::Cancelled)) {
                        publish = false;
                    } else if (!loaded.ok()) {
                        error = loaded.error();
                    } else {
                        session->progress().setStage(LoadStage::BuildingModel);
                        auto built = model::DocumentModel::build(std::move(loaded.value()),
                                                                 session->progress(), session->token());
                        if (!generationCurrent(generation) || session->token().isCancelled() ||
                            (!built.ok() && built.error().code == ErrorCode::Cancelled)) {
                            publish = false;
                        } else if (!built.ok()) {
                            error = built.error();
                        } else {
                            std::function<void(const CancelToken&)> probe;
                            {
                                std::lock_guard lock(m_state->mutex);
                                probe = m_state->probe;
                            }
                            if (probe) {
                                probe(session->token());
                            }
                            if (!generationCurrent(generation) || session->token().isCancelled()) {
                                publish = false;
                            } else {
                                auto model = built.value();
                                {
                                    std::lock_guard lock(m_state->mutex);
                                    if (generation != m_state->generation) {
                                        publish = false;
                                    } else {
                                        m_state->model = model;
                                        m_state->hasModel = true;
                                    }
                                }
                                if (publish) {
                                    bool failAfterPublish = false;
                                    {
                                        std::lock_guard lock(m_state->mutex);
                                        failAfterPublish = m_state->failAfterPublish;
                                    }
                                    if (failAfterPublish) {
                                        error = Error{ErrorCode::MeshFailed, "mesh failed after publish",
                                                      "生成显示失败"};
                                    } else {
                                        session->progress().setStage(LoadStage::MeshingCoarse);
                                        auto scheduler = occt::makeOcctTaskScheduler();
                                        mesh::Tessellator tessellator(*scheduler, mesh::MeshPolicy{});
                                        auto stats = tessellator.run(
                                            *model, mesh::MeshLevel::SchemeB,
                                            [&](mesh::MeshBatch&& batch) {
                                                std::lock_guard lock(m_state->mutex);
                                                if (generation != m_state->generation) {
                                                    return;
                                                }
                                                m_state->batches.push_back(std::move(batch));
                                            },
                                            session->progress(), session->token());
                                        if (!generationCurrent(generation) ||
                                            session->token().isCancelled() ||
                                            (!stats.ok() && stats.error().code == ErrorCode::Cancelled)) {
                                            publish = false;
                                        } else if (!stats.ok()) {
                                            error = stats.error();
                                        } else {
                                            session->progress().setStage(LoadStage::Ready);
                                            ok = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& ex) {
                error = Error{ErrorCode::Internal, ex.what(), "打开文件时出错"};
                ok = false;
                publish = true;
            } catch (...) {
                error = Error{ErrorCode::Internal, "unknown exception", "打开文件时出错"};
                ok = false;
                publish = true;
            }

            {
                std::lock_guard lock(m_state->mutex);
                if (m_state->session == session && generation != m_state->generation) {
                    m_state->session.reset();
                }
            }
            if (publish) {
                finishWorker(generation, ok, std::move(error));
            }
        }});
}

void LoadPipeline::cancel() {
    std::shared_ptr<LoadSession> session;
    std::shared_ptr<const model::DocumentModel> dropped;
    bool adopted = false;
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->activity != LoadActivity::Loading && m_state->activity != LoadActivity::Ready) {
            return;
        }
        if (m_state->activity == LoadActivity::Loading) {
            ++m_state->generation;
            session = m_state->session;
            dropped = std::move(m_state->model);
            m_state->hasModel = false;
            m_state->batches.clear();
            m_state->success = false;
        }
        // 新模型已经显示出来再取消：回到空闲方块，不留上一个文件。
        adopted = m_state->adopted || m_state->haveDisplay;
        m_state->adopted = false;
        if (adopted) {
            m_state->haveDisplay = false;
            m_state->displayedFile.clear();
        }
        m_state->activity = LoadActivity::Releasing;
    }
    if (session) {
        session->cancel();
    }
    model::retireDocument(std::move(dropped));
    if (adopted) {
        m_host.clear();
    }
    Log::info("取消打开");
}

void LoadPipeline::tick() {
    std::shared_ptr<const model::DocumentModel> model;
    std::vector<mesh::MeshBatch> batches;
    bool adopt = false;
    bool failed = false;
    bool success = false;
    bool done = false;
    bool adopted = false;
    int live = 0;
    LoadActivity activity = LoadActivity::Idle;
    std::shared_ptr<const model::DocumentModel> retireMailbox;
    {
        std::lock_guard lock(m_state->mutex);
        live = m_state->live->load(std::memory_order_acquire);
        activity = m_state->activity;
        done = m_state->doneGeneration == m_state->generation;
        success = m_state->success;
        failed = m_state->failure.has_value();
        adopted = m_state->adopted;
        if (m_state->activity == LoadActivity::Loading && done && failed && !success) {
            const bool dropPartial = m_state->adopted;
            m_state->adopted = false;
            if (dropPartial) {
                m_state->haveDisplay = false;
                m_state->displayedFile.clear();
            }
            m_state->activity = m_state->haveDisplay ? LoadActivity::Ready : LoadActivity::Idle;
            m_state->batches.clear();
            retireMailbox = std::move(m_state->model);
            m_state->hasModel = false;
            if (dropPartial) {
                adopted = true;
            }
        }
        if (m_state->activity == LoadActivity::Loading && m_state->hasModel && !m_state->adopted) {
            model = std::move(m_state->model);
            m_state->hasModel = false;
            adopt = true;
        }
        if (m_state->activity == LoadActivity::Loading) {
            batches.swap(m_state->batches);
        }
        activity = m_state->activity;
    }

    model::retireDocument(std::move(retireMailbox));
    if (failed && done && !success && adopted && !adopt) {
        m_host.clear();
        return;
    }
    if (adopt && model) {
        m_host.reset(std::move(model));
        std::lock_guard lock(m_state->mutex);
        if (m_state->activity != LoadActivity::Loading) {
            adopt = false;
        } else {
            m_state->adopted = true;
            m_state->haveDisplay = true;
            adopted = true;
            activity = m_state->activity;
        }
    }
    if (activity == LoadActivity::Loading) {
        for (mesh::MeshBatch& batch : batches) {
            m_host.enqueue(std::move(batch));
        }
        if (adopted) {
            m_host.pump();
        }
    }

    const int pending = adopted ? m_host.pending() : 0;
    bool fit = false;
    {
        std::lock_guard lock(m_state->mutex);
        const bool drained = m_state->batches.empty() && pending == 0;
        if (m_state->activity == LoadActivity::Loading && done && success && adopted && drained) {
            m_state->activity = LoadActivity::Ready;
            m_state->displayedFile = m_state->requestFile;
            fit = true;
        } else if (m_state->activity == LoadActivity::Releasing && live == 0 &&
                   model::documentsPendingRelease() == 0) {
            m_state->activity = m_state->haveDisplay ? LoadActivity::Ready : LoadActivity::Idle;
        }
    }
    if (fit) {
        m_host.fitAll();
    }
}

LoadPipeline::Snapshot LoadPipeline::snapshot() const {
    std::shared_ptr<LoadSession> session;
    Snapshot shot;
    int live = 0;
    {
        std::lock_guard lock(m_state->mutex);
        shot.activity = m_state->activity;
        shot.file =
            m_state->activity == LoadActivity::Loading ? m_state->requestFile : m_state->displayedFile;
        session = m_state->session;
        live = m_state->live->load(std::memory_order_acquire);
    }
    const int pendingRelease = model::documentsPendingRelease();
    shot.waitingForRelease = pendingRelease > 0 || shot.activity == LoadActivity::Releasing ||
                             (shot.activity == LoadActivity::Loading && !session && live > 1);
    // 后台 Close 时不要把进度读数清成 0。waitingForRelease 只决定状态栏文案。
    if (session) {
        const ProgressSink& progress = session->progress();
        shot.progress = progress.phase();
        shot.fraction = progress.fraction();
        shot.elapsedSeconds = progress.elapsedSinceLoadStart();
        shot.overEstimate = progress.overEstimate();
        shot.stalled = progress.unchangedFor(kStallBudget);
    } else if (shot.activity == LoadActivity::Loading && !shot.waitingForRelease) {
        shot.progress = ProgressPhase::ReadingBytes;
    }
    return shot;
}

std::optional<Error> LoadPipeline::takeFailure() {
    std::lock_guard lock(m_state->mutex);
    if (!m_state->failure.has_value()) {
        return std::nullopt;
    }
    Error error = std::move(*m_state->failure);
    m_state->failure.reset();
    return error;
}

bool LoadPipeline::isSettled() const noexcept {
    return m_state->live->load(std::memory_order_acquire) == 0 && model::documentsPendingRelease() == 0;
}

bool LoadPipeline::waitUntilSettled(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!isSettled()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace steplens
