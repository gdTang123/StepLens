#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "core/CancelToken.h"
#include "core/OcctLoadContract.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "mesh/Tessellator.h"
#include "model/DocumentModel.h"
#include "occtsupport/OcctBoundary.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

int documentCount() {
    std::lock_guard lock(steplens::xcafDocumentMutex());
    return XCAFApp_Application::GetApplication()->NbDocuments();
}

opencascade::handle<TDocStd_Document> newDocument() {
    std::lock_guard lock(steplens::xcafDocumentMutex());
    opencascade::handle<TDocStd_Document> doc;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
    return doc;
}

steplens::io::LoadedDocument loadedFrom(opencascade::handle<TDocStd_Document> doc) {
    steplens::io::LoadedDocument loaded;
    loaded.doc = std::move(doc);
    loaded.lengthUnitToMm = 1.0;
    loaded.schema = "AP214";
    loaded.fileBytes = 1;
    return loaded;
}

std::shared_ptr<const steplens::model::DocumentModel>
requireModel(steplens::Result<std::shared_ptr<const steplens::model::DocumentModel>> result) {
    if (!result.ok()) {
        ADD_FAILURE() << result.error().message;
        return {};
    }
    return result.value();
}

std::shared_ptr<const steplens::model::DocumentModel> buildModel(opencascade::handle<TDocStd_Document> doc) {
    steplens::ProgressSink sink;
    sink.reset(1);
    return requireModel(
        steplens::model::DocumentModel::build(loadedFrom(std::move(doc)), sink, steplens::CancelToken{}));
}

TopoDS_Shape boxAt(double x, double y, double z, double size = 10.0) {
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), size, size, size).Shape();
}

struct ProbeState {
    std::mutex mutex;
    std::unordered_map<steplens::model::PrototypeId, int> depth;
    int maxDepth = 0;
    int violations = 0;
    std::vector<steplens::model::PrototypeId> order;
    float previous = -1.f;
    bool monotonic = true;
    bool clamped = true;
    int progressSamples = 0;
};

void noteProgress(ProbeState& state, float value) {
    if (!(value >= 0.f) || value > 1.f) {
        state.clamped = false;
    }
    if (state.previous >= 0.f && value + 1.0e-6f < state.previous) {
        state.monotonic = false;
    }
    if (value > state.previous) {
        state.previous = value;
    }
    ++state.progressSamples;
}

void enterProbe(ProbeState& state, steplens::ProgressSink& sink, steplens::model::PrototypeId id,
                std::chrono::milliseconds pause) {
    {
        std::lock_guard lock(state.mutex);
        int& slot = state.depth[id];
        ++slot;
        if (slot > 1) {
            ++state.violations;
        }
        int active = 0;
        for (const auto& entry : state.depth) {
            if (entry.second > 0) {
                ++active;
            }
        }
        state.maxDepth = std::max(state.maxDepth, active);
        state.order.push_back(id);
        noteProgress(state, sink.fraction());
    }
    if (pause.count() > 0) {
        std::this_thread::sleep_for(pause);
    }
    std::lock_guard lock(state.mutex);
    --state.depth[id];
}

std::optional<std::filesystem::path> samplesDirectory() {
#if defined(_WIN32)
    constexpr DWORD kCapacity = 32767;
    wchar_t buffer[kCapacity];
    const DWORD length = GetEnvironmentVariableW(L"STEPLENS_SAMPLES", buffer, kCapacity);
    if (length == 0 || length >= kCapacity) {
        return std::nullopt;
    }
    return std::filesystem::path{buffer};
#else
    const char* root = std::getenv("STEPLENS_SAMPLES");
    if (root == nullptr || root[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path{root};
#endif
}

std::optional<std::filesystem::path> findNamedSample(const std::filesystem::path& root,
                                                     const std::string& name) {
    const std::filesystem::path direct = root / name;
    if (std::filesystem::exists(direct)) {
        return direct;
    }
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end; it != end;
         it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        constexpr int kMaxDepth = 3;
        if (it.depth() > kMaxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(error) && it->path().filename().string() == name) {
            return it->path();
        }
    }
    return std::nullopt;
}

bool shapeHasDoublePrecisionNear(const TopoDS_Shape& shape, double x, double tolerance) {
    bool found = false;
    for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
        TopLoc_Location location;
        const auto triangulation = BRep_Tool::Triangulation(TopoDS::Face(faces.Current()), location);
        if (triangulation.IsNull()) {
            continue;
        }
        EXPECT_TRUE(triangulation->IsDoublePrecision());
        for (int node = 1; node <= triangulation->NbNodes(); ++node) {
            const gp_Pnt point = triangulation->Node(node).Transformed(location.Transformation());
            if (std::abs(point.X() - x) <= tolerance) {
                found = true;
            }
        }
    }
    return found;
}

} // namespace

TEST(MeshPolicy, SchemeBClampsDeflectionAndMarksLargePrototypes) {
    steplens::mesh::MeshPolicy policy;
    steplens::model::PrototypeInfo info;
    info.faceCount = steplens::mesh::kLargePrototypeFaces;
    info.localBox.Update(0.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    Bnd_Box world;
    world.Update(0.0, 0.0, 0.0, 1000.0, 0.0, 0.0);

    const auto small = policy.paramsFor(info, world, 1.0, steplens::mesh::MeshLevel::SchemeB);
    EXPECT_NEAR(small.deflection, 0.02, 1.0e-12);
    EXPECT_NEAR(small.angleRad, steplens::mesh::kSchemeBAngleDegrees * std::numbers::pi_v<double> / 180.0,
                1.0e-12);
    EXPECT_FALSE(small.inParallel);

    info.faceCount = steplens::mesh::kLargePrototypeFaces + 1;
    EXPECT_TRUE(policy.paramsFor(info, world, 1.0, steplens::mesh::MeshLevel::SchemeB).inParallel);

    steplens::model::PrototypeInfo tiny;
    tiny.faceCount = 6;
    tiny.localBox.Update(0.0, 0.0, 0.0, 0.01, 0.0, 0.0);
    Bnd_Box tinyWorld;
    tinyWorld.Update(0.0, 0.0, 0.0, 0.01, 0.0, 0.0);
    const auto floored = policy.paramsFor(tiny, tinyWorld, 1.0, steplens::mesh::MeshLevel::SchemeB);
    EXPECT_NEAR(floored.deflection, steplens::mesh::kDeflectionMinMm, 1.0e-12);

    const auto inches = policy.paramsFor(tiny, world, 25.4, steplens::mesh::MeshLevel::SchemeB);
    EXPECT_NEAR(inches.deflection, steplens::mesh::kDeflectionMinMm / 25.4, 1.0e-12);
    EXPECT_FALSE(inches.inParallel);
}

TEST(Tessellator, SamePrototypeIsTriangulatedOnce) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const TDF_Label part = shapes->AddShape(boxAt(1.0e6, 2.0, 3.0), false);
    const TDF_Label root = shapes->NewShape();
    constexpr int kInstances = 3;
    for (int index = 0; index < kInstances; ++index) {
        gp_Trsf trsf;
        trsf.SetTranslation(gp_Vec{index * 40.0, 0.0, 0.0});
        shapes->AddComponent(root, part, TopLoc_Location{trsf});
    }
    shapes->UpdateAssemblies();
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 1u);
    EXPECT_EQ(model->prototype(0).instanceCount, static_cast<std::uint32_t>(kInstances));

    auto scheduler = steplens::occt::makeOcctTaskScheduler(2);
    steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{});
    ProbeState probe;
    steplens::ProgressSink sink;
    sink.reset(1);
    std::vector<steplens::mesh::MeshBatch> batches;
    tessellator.setExclusiveProbe([&](steplens::model::PrototypeId id) {
        std::lock_guard lock(probe.mutex);
        probe.order.push_back(id);
    });
    const auto result = tessellator.run(
        *model, steplens::mesh::MeshLevel::SchemeB,
        [&](steplens::mesh::MeshBatch&& batch) { batches.push_back(std::move(batch)); }, sink,
        steplens::CancelToken{});
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().meshCalls, 1u);
    EXPECT_EQ(result.value().prototypesMeshed, 1u);
    EXPECT_EQ(batches.size(), 1u);
    EXPECT_EQ(probe.order.size(), 1u);
    EXPECT_EQ(batches[0].id, model->prototype(0).id);
    EXPECT_EQ(batches[0].level, steplens::mesh::MeshLevel::SchemeB);
    EXPECT_GT(result.value().triangleCount, 0u);
    EXPECT_EQ(result.value().instanceTriangleCount,
              result.value().triangleCount * static_cast<std::uint64_t>(kInstances));
    EXPECT_EQ(batches[0].triangleCount, result.value().triangleCount);
    EXPECT_GT(result.value().approxBytes, 0u);
    ASSERT_FALSE(batches[0].groups.empty());
    EXPECT_FALSE(batches[0].groups[0].triangles.IsNull());
    EXPECT_GT(batches[0].groups[0].triangles->VertexNumber(), 0);
    EXPECT_TRUE(shapeHasDoublePrecisionNear(model->prototype(0).shape, 1.0e6, 1.0e-5));
    EXPECT_GE(sink.fraction(), 0.f);
    EXPECT_LE(sink.fraction(), 1.f);
    EXPECT_EQ(sink.phase(), steplens::ProgressPhase::Presenting);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(Tessellator, DistinctPrototypesOverlapAndStayExclusive) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    constexpr int kParts = 4;
    for (int index = 0; index < kParts; ++index) {
        shapes->AddShape(boxAt(index * 40.0, 0.0, 0.0), false);
    }
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), static_cast<std::size_t>(kParts));

    auto scheduler = steplens::occt::makeOcctTaskScheduler(4);
    steplens::mesh::MeshPolicy policy(std::numeric_limits<std::uint32_t>::max());
    steplens::mesh::Tessellator tessellator(*scheduler, policy);
    steplens::ProgressSink sink;
    sink.reset(1);
    ProbeState probe;
    tessellator.setExclusiveProbe(
        [&](steplens::model::PrototypeId id) { enterProbe(probe, sink, id, std::chrono::milliseconds{40}); });
    std::mutex batchMutex;
    std::set<steplens::model::PrototypeId> seen;
    const auto result = tessellator.run(
        *model, steplens::mesh::MeshLevel::SchemeB,
        [&](steplens::mesh::MeshBatch&& batch) {
            std::lock_guard lock(batchMutex);
            seen.insert(batch.id);
        },
        sink, steplens::CancelToken{});
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().meshCalls, static_cast<std::uint32_t>(kParts));
    EXPECT_EQ(result.value().prototypesMeshed, static_cast<std::uint32_t>(kParts));
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kParts));
    EXPECT_EQ(probe.violations, 0);
    EXPECT_GT(probe.maxDepth, 1);
    EXPECT_TRUE(probe.monotonic);
    EXPECT_TRUE(probe.clamped);
    EXPECT_GT(probe.progressSamples, 0);
    {
        std::lock_guard lock(probe.mutex);
        noteProgress(probe, sink.fraction());
    }
    EXPECT_TRUE(probe.monotonic);
    EXPECT_TRUE(probe.clamped);
    EXPECT_NEAR(sink.fraction(), 1.f, 1.0e-5f);
    EXPECT_EQ(sink.phase(), steplens::ProgressPhase::Presenting);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(Tessellator, SchedulesHeavierPrototypesFirstAndSerializesLargeOnes) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const TDF_Label root = shapes->NewShape();
    const int copies[] = {1, 4, 2};
    for (int part = 0; part < 3; ++part) {
        const TDF_Label label = shapes->AddShape(boxAt(0.0, part * 40.0, 0.0), false);
        for (int copy = 0; copy < copies[part]; ++copy) {
            gp_Trsf trsf;
            trsf.SetTranslation(gp_Vec{copy * 30.0, part * 40.0, 0.0});
            shapes->AddComponent(root, label, TopLoc_Location{trsf});
        }
    }
    shapes->UpdateAssemblies();
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 3u);

    auto scheduler = steplens::occt::makeOcctTaskScheduler(4);
    steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{0});
    steplens::ProgressSink sink;
    sink.reset(1);
    ProbeState probe;
    tessellator.setExclusiveProbe(
        [&](steplens::model::PrototypeId id) { enterProbe(probe, sink, id, std::chrono::milliseconds{20}); });
    const auto result = tessellator.run(
        *model, steplens::mesh::MeshLevel::SchemeB, [](steplens::mesh::MeshBatch&&) {}, sink,
        steplens::CancelToken{});
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().meshCalls, 3u);
    EXPECT_EQ(probe.maxDepth, 1);
    ASSERT_EQ(probe.order.size(), 3u);
    std::vector<std::uint64_t> weights;
    for (const auto id : probe.order) {
        const auto& info = model->prototype(id);
        weights.push_back(static_cast<std::uint64_t>(info.faceCount) * info.instanceCount);
    }
    EXPECT_GT(weights[0], weights[1]);
    EXPECT_GT(weights[1], weights[2]);
    EXPECT_TRUE(probe.monotonic);
    EXPECT_TRUE(probe.clamped);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(Tessellator, CancelStopsLaterPrototypes) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    constexpr int kParts = 4;
    for (int index = 0; index < kParts; ++index) {
        shapes->AddShape(boxAt(index * 25.0, 0.0, 0.0), false);
    }
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);

    auto scheduler = steplens::occt::makeOcctTaskScheduler(2);
    steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{0});
    steplens::ProgressSink sink;
    sink.reset(32);
    steplens::CancelSource source;
    int started = 0;
    tessellator.setExclusiveProbe([&](steplens::model::PrototypeId) {
        ++started;
        source.requestStop();
        sink.beginCancel();
    });
    int batches = 0;
    const auto result = tessellator.run(
        *model, steplens::mesh::MeshLevel::SchemeB, [&](steplens::mesh::MeshBatch&&) { ++batches; }, sink,
        source.token());
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, steplens::ErrorCode::Cancelled);
    EXPECT_EQ(started, 1);
    EXPECT_EQ(batches, 0);
    EXPECT_TRUE(sink.cancelFrozen());
    EXPECT_GE(sink.fraction(), 0.f);
    EXPECT_LE(sink.fraction(), 1.f);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(Tessellator, CancelBeforeRunDeliversNothing) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    shapes->AddShape(boxAt(0.0, 0.0, 0.0), false);
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);

    auto scheduler = steplens::occt::makeOcctTaskScheduler(2);
    steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{});
    steplens::ProgressSink sink;
    sink.reset(8);
    steplens::CancelSource source;
    source.requestStop();
    sink.beginCancel();
    int batches = 0;
    const auto result = tessellator.run(
        *model, steplens::mesh::MeshLevel::SchemeB, [&](steplens::mesh::MeshBatch&&) { ++batches; }, sink,
        source.token());
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, steplens::ErrorCode::Cancelled);
    EXPECT_EQ(batches, 0);
    EXPECT_TRUE(sink.cancelFrozen());
    EXPECT_GE(sink.fraction(), 0.f);
    EXPECT_LE(sink.fraction(), 1.f);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(Tessellator, ColorGroupsBecomeSeparateArrays) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    auto colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    shapes->AddShape(box, false);
    const Quantity_Color red{1.0, 0.0, 0.0, Quantity_TOC_RGB};
    const Quantity_Color blue{0.0, 0.0, 1.0, Quantity_TOC_RGB};
    int index = 0;
    for (TopExp_Explorer faces{box, TopAbs_FACE}; faces.More(); faces.Next(), ++index) {
        const Quantity_Color& color = index < 3 ? red : blue;
        EXPECT_TRUE(colors->SetColor(faces.Current(), color, XCAFDoc_ColorSurf));
    }
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 1u);
    ASSERT_EQ(model->prototype(0).colorGroups.size(), 2u);

    auto scheduler = steplens::occt::makeOcctTaskScheduler(2);
    steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{});
    steplens::ProgressSink sink;
    sink.reset(1);
    std::vector<steplens::mesh::MeshBatch> batches;
    const auto result = tessellator.run(
        *model, steplens::mesh::MeshLevel::SchemeB,
        [&](steplens::mesh::MeshBatch&& batch) { batches.push_back(std::move(batch)); }, sink,
        steplens::CancelToken{});
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().meshCalls, 1u);
    ASSERT_EQ(batches.size(), 1u);
    ASSERT_EQ(batches[0].groups.size(), 2u);
    int colored = 0;
    for (const auto& group : batches[0].groups) {
        ASSERT_FALSE(group.triangles.IsNull());
        EXPECT_GT(group.triangles->VertexNumber(), 0);
        EXPECT_FALSE(group.edges.IsNull());
        const double redChannel = group.color.GetRGB().Red();
        EXPECT_TRUE(redChannel > 0.9 || group.color.GetRGB().Blue() > 0.9);
        ++colored;
    }
    EXPECT_EQ(colored, 2);
    EXPECT_GT(batches[0].triangleCount, 0u);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(Tessellator, SharedFacePrototypesDoNotMeshTogether) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    BRep_Builder builder;
    TopoDS_Compound left;
    TopoDS_Compound right;
    builder.MakeCompound(left);
    builder.MakeCompound(right);
    const TopoDS_Shape boxA = boxAt(0.0, 0.0, 0.0);
    const TopoDS_Shape boxB = boxAt(40.0, 0.0, 0.0);
    int index = 0;
    for (TopExp_Explorer faces(boxA, TopAbs_FACE); faces.More(); faces.Next(), ++index) {
        builder.Add(left, faces.Current());
        if (index == 0) {
            builder.Add(right, faces.Current());
        }
    }
    for (TopExp_Explorer faces(boxB, TopAbs_FACE); faces.More(); faces.Next()) {
        builder.Add(right, faces.Current());
    }
    shapes->AddShape(left, false);
    shapes->AddShape(right, false);
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 2u);

    std::set<const void*> leftFaces;
    for (TopExp_Explorer faces(model->prototype(0).shape, TopAbs_FACE); faces.More(); faces.Next()) {
        leftFaces.insert(faces.Current().TShape().get());
    }
    bool shared = false;
    for (TopExp_Explorer faces(model->prototype(1).shape, TopAbs_FACE); faces.More(); faces.Next()) {
        if (leftFaces.contains(faces.Current().TShape().get())) {
            shared = true;
        }
    }
    ASSERT_TRUE(shared);

    auto scheduler = steplens::occt::makeOcctTaskScheduler(4);
    steplens::mesh::MeshPolicy policy(std::numeric_limits<std::uint32_t>::max());
    steplens::mesh::Tessellator tessellator(*scheduler, policy);
    steplens::ProgressSink sink;
    sink.reset(1);
    ProbeState probe;
    tessellator.setExclusiveProbe(
        [&](steplens::model::PrototypeId id) { enterProbe(probe, sink, id, std::chrono::milliseconds{40}); });
    const auto result = tessellator.run(
        *model, steplens::mesh::MeshLevel::SchemeB, [](steplens::mesh::MeshBatch&&) {}, sink,
        steplens::CancelToken{});
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().meshCalls, 2u);
    EXPECT_EQ(probe.order.size(), 2u);
    EXPECT_EQ(probe.violations, 0);
    EXPECT_EQ(probe.maxDepth, 1);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(Tessellator, X02InstanceTrianglesStayWithinBudget) {
    // 样例不在工作区时跳过。设置了 STEPLENS_SAMPLES 且能找到文件时必须量，不能静默放过：
    //   STEPLENS_SAMPLES=/path/to/samples ctest --test-dir build/occt-contract -R 'Tessellator.X02'
    //   --output-on-failure
    const auto root = samplesDirectory();
    if (!root) {
        GTEST_SKIP() << "STEPLENS_SAMPLES 未设置，X-02 三角形上限这次不量";
    }
    const auto sample = findNamedSample(*root, "Scania-Engine-V8-XT-Turbo.step");
    if (!sample) {
        GTEST_SKIP() << "STEPLENS_SAMPLES 里没有 Scania-Engine-V8-XT-Turbo.step，X-02 三角形上限这次不量";
    }

    const int before = documentCount();
    steplens::ProgressSink sink;
    sink.reset(static_cast<std::uint64_t>(std::filesystem::file_size(*sample)));
    auto loader = steplens::io::makeLoader(*sample);
    ASSERT_TRUE(static_cast<bool>(loader));
    auto loaded = loader->load(*sample, {}, sink, steplens::CancelToken{});
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    auto model = requireModel(
        steplens::model::DocumentModel::build(std::move(loaded.value()), sink, steplens::CancelToken{}));
    ASSERT_TRUE(model);
    ASSERT_GT(model->prototypes().size(), 1u);

    auto scheduler = steplens::occt::makeOcctTaskScheduler();
    steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{});
    const auto result = tessellator.run(
        *model, steplens::mesh::MeshLevel::SchemeB, [](steplens::mesh::MeshBatch&&) {}, sink,
        steplens::CancelToken{});
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().meshCalls, static_cast<std::uint32_t>(model->prototypes().size()));
    EXPECT_EQ(result.value().prototypesMeshed, result.value().meshCalls);
    EXPECT_GT(result.value().instanceTriangleCount, result.value().triangleCount);
    constexpr std::uint64_t kX02InstanceTriangleBudget = 5000000;
    EXPECT_LE(result.value().instanceTriangleCount, kX02InstanceTriangleBudget);
    EXPECT_GE(sink.fraction(), 0.f);
    EXPECT_LE(sink.fraction(), 1.f);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}
