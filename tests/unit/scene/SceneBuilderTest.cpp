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
#include "occtsupport/occt_compat.h"
#include "scene/SceneBuilder.h"

#include <AIS_ConnectedInteractive.hxx>
#include <AIS_InteractiveContext.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <SelectMgr_Selection.hxx>
#include <Standard_Failure.hxx>
#include <TDataStd_Name.hxx>
#include <TopExp_Explorer.hxx>
#include <V3d_Viewer.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

std::vector<steplens::mesh::MeshBatch> tessellate(const steplens::model::DocumentModel& model) {
    auto scheduler = steplens::occt::makeOcctTaskScheduler(2);
    steplens::mesh::Tessellator tessellator(*scheduler, steplens::mesh::MeshPolicy{});
    steplens::ProgressSink sink;
    sink.reset(1);
    std::vector<steplens::mesh::MeshBatch> batches;
    const auto result = tessellator.run(
        model, steplens::mesh::MeshLevel::SchemeB,
        [&](steplens::mesh::MeshBatch&& batch) { batches.push_back(std::move(batch)); }, sink,
        steplens::CancelToken{});
    EXPECT_TRUE(result.ok()) << result.error().message;
    return batches;
}

void drain(steplens::scene::SceneBuilder& builder, int& pumps, std::int64_t& maxStallUs) {
    while (builder.pending() > 0) {
        builder.pump();
        ++pumps;
        const auto stall = builder.lastPumpDuration().count();
        if (stall > maxStallUs) {
            maxStallUs = stall;
        }
        EXPECT_LE(stall, steplens::scene::SceneBuilder::kStallWarn.count());
    }
}

opencascade::handle<AIS_InteractiveContext> makeTestContext() {
    try {
        opencascade::handle<Aspect_DisplayConnection> display = new Aspect_DisplayConnection();
        opencascade::handle<OpenGl_GraphicDriver> driver = new OpenGl_GraphicDriver(display, false);
        opencascade::handle<V3d_Viewer> viewer = new V3d_Viewer(driver);
        viewer->SetDefaultLights();
        viewer->SetLightOn();
        // 不给 NeutralWindow 绑空的 X 窗口。SetWindow 会去问 0 号窗口，xvfb 直接把进程打死。
        // Display(update=false) 不需要窗口，只把对象登记进上下文。
        opencascade::handle<AIS_InteractiveContext> context = new AIS_InteractiveContext(viewer);
        steplens::mesh::MeshBatch empty;
        opencascade::handle<steplens::scene::PrototypePrs> probe =
            new steplens::scene::PrototypePrs(empty, TopoDS_Shape{});
        opencascade::handle<AIS_ConnectedInteractive> instance = new AIS_ConnectedInteractive();
        instance->Connect(probe);
        context->Display(instance, steplens::scene::PrototypePrs::ShadedWithEdges, 0, false);
        context->RemoveAll(false);
        return context;
    } catch (const Standard_Failure& failure) {
        std::cout << "scene viewer unavailable: " << steplens::occt::failureMessage(failure) << "\n";
        return {};
    } catch (const std::exception& ex) {
        std::cout << "scene viewer unavailable: " << ex.what() << "\n";
        return {};
    } catch (...) {
        std::cout << "scene viewer unavailable\n";
        return {};
    }
}

opencascade::handle<AIS_InteractiveContext> contextOrEmpty() {
    const auto context = makeTestContext();
#if !defined(_WIN32)
    EXPECT_FALSE(context.IsNull()) << "Linux contract must create an AIS context";
#endif
    return context;
}

struct OwnerSample {
    opencascade::handle<AIS_InteractiveObject> selectable;
    double ownerX = 0.0;
    double centerX = 0.0;
};

OwnerSample sampleSelection(const opencascade::handle<AIS_InteractiveObject>& object) {
    OwnerSample sample;
    if (object.IsNull()) {
        return sample;
    }
    if (!object->HasSelection(0)) {
        object->RecomputePrimitives(0);
    }
    if (!object->HasSelection(0)) {
        return sample;
    }
    const opencascade::handle<SelectMgr_Selection>& selection = object->Selection(0);
    if (selection.IsNull()) {
        return sample;
    }
    for (const auto& entity : selection->Entities()) {
        if (entity.IsNull()) {
            continue;
        }
        const auto sensitive = entity->BaseSensitive();
        if (sensitive.IsNull() || sensitive->OwnerId().IsNull()) {
            continue;
        }
        sample.selectable =
            opencascade::handle<AIS_InteractiveObject>::DownCast(sensitive->OwnerId()->Selectable());
        sample.ownerX = sensitive->OwnerId()->Location().Transformation().TranslationPart().X();
        sample.centerX = sensitive->CenterOfGeometry().X();
        break;
    }
    return sample;
}

} // namespace

TEST(SelectionMap, SiblingInstancesStayDistinct) {
    steplens::mesh::MeshBatch batch;
    batch.id = 7;
    opencascade::handle<steplens::scene::PrototypePrs> prototype =
        new steplens::scene::PrototypePrs(batch, TopoDS_Shape{});
    gp_Trsf first;
    first.SetTranslation(gp_Vec{0.0, 0.0, 0.0});
    gp_Trsf second;
    second.SetTranslation(gp_Vec{40.0, 0.0, 0.0});
    opencascade::handle<AIS_ConnectedInteractive> left = new AIS_ConnectedInteractive();
    opencascade::handle<AIS_ConnectedInteractive> right = new AIS_ConnectedInteractive();
    left->Connect(prototype, first);
    right->Connect(prototype, second);

    steplens::scene::SelectionMap map;
    map.bind(1, left);
    map.bind(2, right);

    EXPECT_TRUE(left->HasConnection());
    EXPECT_TRUE(right->HasConnection());
    EXPECT_EQ(left->ConnectedTo(), prototype);
    EXPECT_EQ(right->ConnectedTo(), prototype);
    ASSERT_TRUE(map.nodeOf(left).has_value());
    ASSERT_TRUE(map.nodeOf(right).has_value());
    EXPECT_EQ(*map.nodeOf(left), 1u);
    EXPECT_EQ(*map.nodeOf(right), 2u);
    EXPECT_NE(*map.nodeOf(left), *map.nodeOf(right));
    EXPECT_FALSE(map.nodeOf(prototype).has_value());
    EXPECT_EQ(map.objectOf(1), left);
    EXPECT_EQ(map.objectOf(2), right);
    EXPECT_NEAR(left->Transformation().TranslationPart().X(), 0.0, 1.0e-9);
    EXPECT_NEAR(right->Transformation().TranslationPart().X(), 40.0, 1.0e-9);
}

TEST(SceneBuilder, SamePrototypeIsPresentedOnceAndPickReturnsInstance) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const TDF_Label part = shapes->AddShape(boxAt(0.0, 0.0, 0.0), false);
    const TDF_Label other = shapes->AddShape(boxAt(0.0, 80.0, 0.0), false);
    const TDF_Label root = shapes->NewShape();
    constexpr int kCopies = 2;
    for (int index = 0; index < kCopies; ++index) {
        gp_Trsf trsf;
        trsf.SetTranslation(gp_Vec{index * 40.0, 0.0, 0.0});
        shapes->AddComponent(root, part, TopLoc_Location{trsf});
    }
    gp_Trsf otherTrsf;
    otherTrsf.SetTranslation(gp_Vec{0.0, 0.0, 25.0});
    shapes->AddComponent(root, other, TopLoc_Location{otherTrsf});
    shapes->UpdateAssemblies();

    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 2u);
    const auto sharedId =
        model->prototype(0).instanceCount == 2u ? model->prototype(0).id : model->prototype(1).id;
    const auto soloId = sharedId == model->prototype(0).id ? model->prototype(1).id : model->prototype(0).id;
    const auto sharedLeaves = model->leafNodesOf(sharedId);
    const auto soloLeaves = model->leafNodesOf(soloId);
    ASSERT_EQ(sharedLeaves.size(), 2u);
    ASSERT_EQ(soloLeaves.size(), 1u);
    ASSERT_NE(sharedLeaves[0], sharedId);
    ASSERT_NE(sharedLeaves[1], sharedId);
    ASSERT_NE(sharedLeaves[0], sharedLeaves[1]);

    auto batches = tessellate(*model);
    ASSERT_EQ(batches.size(), 2u);

    auto context = contextOrEmpty();
    steplens::scene::SceneBuilder builder(context);
    int firstFrames = 0;
    builder.setFirstFrameCallback([&] { ++firstFrames; });
    builder.reset(model);
    for (auto& batch : batches) {
        builder.enqueue(std::move(batch));
    }
    int pumps = 0;
    std::int64_t maxStallUs = 0;
    drain(builder, pumps, maxStallUs);
    EXPECT_FALSE(builder.failed()) << builder.lastError();
    EXPECT_EQ(firstFrames, 1);
    EXPECT_EQ(builder.pending(), 0);
    EXPECT_GT(pumps, 0);
    EXPECT_LE(maxStallUs, steplens::scene::SceneBuilder::kStallWarn.count());
    std::cout << "scene present pumps=" << pumps << " max_stall_us=" << maxStallUs
              << " display=" << (context.IsNull() ? 0 : 1) << "\n";

    const auto shared = builder.sharedPrototype(sharedId);
    const auto solo = builder.sharedPrototype(soloId);
    ASSERT_FALSE(shared.IsNull());
    ASSERT_FALSE(solo.IsNull());
    EXPECT_NE(shared, solo);

    const auto left =
        opencascade::handle<AIS_ConnectedInteractive>::DownCast(builder.objectOf(sharedLeaves[0]));
    const auto right =
        opencascade::handle<AIS_ConnectedInteractive>::DownCast(builder.objectOf(sharedLeaves[1]));
    const auto third =
        opencascade::handle<AIS_ConnectedInteractive>::DownCast(builder.objectOf(soloLeaves[0]));
    ASSERT_FALSE(left.IsNull());
    ASSERT_FALSE(right.IsNull());
    ASSERT_FALSE(third.IsNull());
    EXPECT_TRUE(left->HasConnection());
    EXPECT_TRUE(right->HasConnection());
    EXPECT_EQ(left->ConnectedTo(), shared);
    EXPECT_EQ(right->ConnectedTo(), shared);
    EXPECT_EQ(third->ConnectedTo(), solo);
    EXPECT_NE(left->ConnectedTo(), third->ConnectedTo());

    EXPECT_FALSE(builder.nodeOf(shared).has_value());
    EXPECT_FALSE(builder.nodeOf(solo).has_value());
    ASSERT_TRUE(builder.nodeOf(left).has_value());
    ASSERT_TRUE(builder.nodeOf(right).has_value());
    ASSERT_TRUE(builder.nodeOf(third).has_value());
    EXPECT_EQ(*builder.nodeOf(left), sharedLeaves[0]);
    EXPECT_EQ(*builder.nodeOf(right), sharedLeaves[1]);
    EXPECT_EQ(*builder.nodeOf(third), soloLeaves[0]);
    EXPECT_NE(*builder.nodeOf(left), *builder.nodeOf(right));
    EXPECT_NE(*builder.nodeOf(left), sharedId);

    const OwnerSample leftPick = sampleSelection(left);
    const OwnerSample rightPick = sampleSelection(right);
    const OwnerSample thirdPick = sampleSelection(third);
    EXPECT_EQ(leftPick.selectable, left);
    EXPECT_EQ(rightPick.selectable, right);
    EXPECT_EQ(thirdPick.selectable, third);
    EXPECT_NE(leftPick.selectable, shared);
    EXPECT_NE(rightPick.selectable, leftPick.selectable);
    const auto leftNode = builder.nodeOf(leftPick.selectable);
    const auto rightNode = builder.nodeOf(rightPick.selectable);
    ASSERT_TRUE(leftNode.has_value());
    ASSERT_TRUE(rightNode.has_value());
    EXPECT_EQ(*leftNode, sharedLeaves[0]);
    EXPECT_EQ(*rightNode, sharedLeaves[1]);

    const double leftX = model->node(sharedLeaves[0]).worldLocation.Transformation().TranslationPart().X();
    const double rightX = model->node(sharedLeaves[1]).worldLocation.Transformation().TranslationPart().X();
    EXPECT_NEAR(left->Transformation().TranslationPart().X(), leftX, 1.0e-6);
    EXPECT_NEAR(right->Transformation().TranslationPart().X(), rightX, 1.0e-6);
    EXPECT_GT(std::abs(rightX - leftX), 1.0);
    std::cout << "scene pick left_node=" << *leftNode << " right_node=" << *rightNode
              << " owner_x=" << leftPick.ownerX << "," << rightPick.ownerX << " center_x=" << leftPick.centerX
              << "," << rightPick.centerX << "\n";
    EXPECT_NEAR(leftPick.ownerX, leftX, 1.0e-4);
    EXPECT_NEAR(rightPick.ownerX, rightX, 1.0e-4);
    EXPECT_NEAR(leftPick.centerX, rightPick.centerX, 1.0e-4);

    if (!context.IsNull()) {
        EXPECT_FALSE(context->IsDisplayed(shared));
        EXPECT_FALSE(context->IsDisplayed(solo));
        EXPECT_TRUE(context->IsDisplayed(left));
        EXPECT_TRUE(context->IsDisplayed(right));
        EXPECT_GT(shared->presentationGroupCount(), 0);
        builder.setNodeVisible(sharedLeaves[0], false);
        EXPECT_FALSE(context->IsDisplayed(left));
        EXPECT_TRUE(context->IsDisplayed(right));
        builder.highlightNode(sharedLeaves[1]);
        EXPECT_TRUE(context->IsSelected(right));
        EXPECT_FALSE(context->IsSelected(left));
    }

    builder.reset(nullptr);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(SceneBuilder, FrameBudgetKeepsStallUnderLimit) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const TDF_Label part = shapes->AddShape(boxAt(0.0, 0.0, 0.0), false);
    const TDF_Label root = shapes->NewShape();
    constexpr int kCopies = 8;
    for (int index = 0; index < kCopies; ++index) {
        gp_Trsf trsf;
        trsf.SetTranslation(gp_Vec{index * 40.0, 0.0, 0.0});
        shapes->AddComponent(root, part, TopLoc_Location{trsf});
    }
    shapes->UpdateAssemblies();
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 1u);
    ASSERT_EQ(model->leafNodesOf(0).size(), static_cast<std::size_t>(kCopies));
    auto batches = tessellate(*model);
    ASSERT_EQ(batches.size(), 1u);
    steplens::mesh::MeshBatch saved = batches[0];

    auto context = contextOrEmpty();
    steplens::scene::SceneBuilder builder(context);
    builder.reset(model);
    builder.enqueue(std::move(batches[0]));
    int pumps = 0;
    std::int64_t maxStallUs = 0;
    drain(builder, pumps, maxStallUs);
    EXPECT_FALSE(builder.failed()) << builder.lastError();
    EXPECT_LE(maxStallUs, steplens::scene::SceneBuilder::kStallWarn.count());
    std::cout << "scene default budget pumps=" << pumps << " max_stall_us=" << maxStallUs << "\n";

    std::set<const AIS_InteractiveObject*> prototypes;
    for (const auto leaf : model->leafNodesOf(0)) {
        const auto connected =
            opencascade::handle<AIS_ConnectedInteractive>::DownCast(builder.objectOf(leaf));
        ASSERT_FALSE(connected.IsNull());
        ASSERT_TRUE(connected->HasConnection());
        prototypes.insert(connected->ConnectedTo().get());
        EXPECT_EQ(builder.nodeOf(connected), std::optional<steplens::model::NodeId>{leaf});
    }
    EXPECT_EQ(prototypes.size(), 1u);

    builder.reset(model);
    int created = 0;
    builder.setInstanceProbe([&] {
        // 每个实例占住约 3 ms。默认预算 8 ms，一帧挂不完 8 个实例。
        const auto spinStart = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - spinStart < std::chrono::milliseconds(3)) {
        }
        ++created;
    });
    builder.enqueue(std::move(saved));
    builder.pump();
    const int firstPumpInstances = created;
    int slicedPumps = 1;
    std::int64_t slicedMaxUs = builder.lastPumpDuration().count();
    EXPECT_LE(slicedMaxUs, steplens::scene::SceneBuilder::kStallWarn.count());
    while (builder.pending() > 0) {
        builder.pump();
        ++slicedPumps;
        const auto stall = builder.lastPumpDuration().count();
        if (stall > slicedMaxUs) {
            slicedMaxUs = stall;
        }
        EXPECT_LE(stall, steplens::scene::SceneBuilder::kStallWarn.count());
    }
    EXPECT_FALSE(builder.failed()) << builder.lastError();
    EXPECT_EQ(created, kCopies);
    EXPECT_GT(firstPumpInstances, 0);
    EXPECT_LT(firstPumpInstances, kCopies);
    EXPECT_GT(slicedPumps, 1);
    std::cout << "scene sliced budget pumps=" << slicedPumps << " first_instances=" << firstPumpInstances
              << " max_stall_us=" << slicedMaxUs << " instances=" << created << "\n";

    builder.reset(nullptr);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(SceneBuilder, InstanceColorUsesItsOwnPrototypeAndSharesArrays) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    auto colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    const TDF_Label part = shapes->AddShape(boxAt(0.0, 0.0, 0.0), false);
    const TDF_Label root = shapes->NewShape();
    gp_Trsf plainTrsf;
    plainTrsf.SetTranslation(gp_Vec{0.0, 0.0, 0.0});
    shapes->AddComponent(root, part, TopLoc_Location{plainTrsf});
    gp_Trsf coloredTrsf;
    coloredTrsf.SetTranslation(gp_Vec{50.0, 0.0, 0.0});
    const TDF_Label coloredUse = shapes->AddComponent(root, part, TopLoc_Location{coloredTrsf});
    colors->SetColor(coloredUse, Quantity_Color{1.0, 0.0, 0.0, Quantity_TOC_RGB}, XCAFDoc_ColorGen);
    shapes->UpdateAssemblies();

    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 1u);
    const auto leaves = model->leafNodesOf(0);
    ASSERT_EQ(leaves.size(), 2u);
    const auto& first = model->node(leaves[0]);
    const steplens::model::NodeId plainId = first.instanceColor.has_value() ? leaves[1] : leaves[0];
    const steplens::model::NodeId coloredId = first.instanceColor.has_value() ? leaves[0] : leaves[1];
    ASSERT_TRUE(model->node(coloredId).instanceColor.has_value());
    ASSERT_FALSE(model->node(plainId).instanceColor.has_value());

    auto batches = tessellate(*model);
    ASSERT_EQ(batches.size(), 1u);
    steplens::scene::SceneBuilder builder(contextOrEmpty());
    builder.reset(model);
    builder.enqueue(std::move(batches[0]));
    int pumps = 0;
    std::int64_t maxStallUs = 0;
    drain(builder, pumps, maxStallUs);
    EXPECT_FALSE(builder.failed()) << builder.lastError();
    EXPECT_LE(maxStallUs, steplens::scene::SceneBuilder::kStallWarn.count());

    const auto shared = builder.sharedPrototype(0);
    ASSERT_FALSE(shared.IsNull());
    const auto plain = opencascade::handle<AIS_ConnectedInteractive>::DownCast(builder.objectOf(plainId));
    const auto colored =
        opencascade::handle<steplens::scene::PrototypePrs>::DownCast(builder.objectOf(coloredId));
    ASSERT_FALSE(plain.IsNull());
    ASSERT_FALSE(colored.IsNull());
    EXPECT_EQ(plain->ConnectedTo(), shared);
    EXPECT_NE(colored, shared);
    ASSERT_FALSE(colored->mesh().groups.empty());
    ASSERT_FALSE(shared->mesh().groups.empty());
    EXPECT_EQ(colored->mesh().groups[0].triangles, shared->mesh().groups[0].triangles);
    EXPECT_NEAR(colored->mesh().groups[0].color.GetRGB().Red(), 1.0, 1.0e-6);
    ASSERT_TRUE(builder.nodeOf(colored).has_value());
    ASSERT_TRUE(builder.nodeOf(plain).has_value());
    EXPECT_EQ(*builder.nodeOf(colored), coloredId);
    EXPECT_EQ(*builder.nodeOf(plain), plainId);
    EXPECT_NE(*builder.nodeOf(colored), *builder.nodeOf(plain));

    builder.reset(nullptr);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(SceneBuilder, ColorGroupsFollowMeshBatch) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    auto colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    const TopoDS_Shape box = boxAt(0.0, 0.0, 0.0);
    shapes->AddShape(box, false);
    const Quantity_Color red{1.0, 0.0, 0.0, Quantity_TOC_RGB};
    const Quantity_Color blue{0.0, 0.0, 1.0, Quantity_TOC_RGB};
    int faceIndex = 0;
    for (TopExp_Explorer faces{box, TopAbs_FACE}; faces.More(); faces.Next(), ++faceIndex) {
        const Quantity_Color& color = faceIndex < 3 ? red : blue;
        EXPECT_TRUE(colors->SetColor(faces.Current(), color, XCAFDoc_ColorSurf));
    }
    auto model = buildModel(std::move(doc));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 1u);
    ASSERT_EQ(model->prototype(0).colorGroups.size(), 2u);
    auto batches = tessellate(*model);
    ASSERT_EQ(batches.size(), 1u);
    ASSERT_EQ(batches[0].groups.size(), 2u);
    EXPECT_NE(batches[0].groups[0].color.GetRGB().Red(), batches[0].groups[1].color.GetRGB().Red());
    const auto groupCount = batches[0].groups.size();

    auto context = contextOrEmpty();
    steplens::scene::SceneBuilder builder(context);
    builder.reset(model);
    builder.enqueue(std::move(batches[0]));
    int pumps = 0;
    std::int64_t maxStallUs = 0;
    drain(builder, pumps, maxStallUs);
    EXPECT_FALSE(builder.failed()) << builder.lastError();
    const auto shared = builder.sharedPrototype(0);
    ASSERT_FALSE(shared.IsNull());
    ASSERT_EQ(shared->mesh().groups.size(), groupCount);
    if (!context.IsNull()) {
        const auto leaves = model->leafNodesOf(0);
        ASSERT_FALSE(leaves.empty());
        EXPECT_TRUE(context->IsDisplayed(builder.objectOf(leaves[0])));
        EXPECT_FALSE(context->IsDisplayed(shared));
        EXPECT_GE(shared->presentationGroupCount(), static_cast<int>(groupCount));
    }
    std::cout << "scene color groups=" << shared->mesh().groups.size()
              << " presentation_groups=" << shared->presentationGroupCount() << " max_stall_us=" << maxStallUs
              << "\n";

    builder.reset(nullptr);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}
