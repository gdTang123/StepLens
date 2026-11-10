#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../core/TempDir.h"
#include "core/CancelToken.h"
#include "core/OcctLoadContract.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "model/DocumentModel.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_StepModelType.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDataStd_Name.hxx>
#include <TopExp_Explorer.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <set>
#include <string>
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

steplens::io::LoadedDocument loadedFrom(opencascade::handle<TDocStd_Document> doc, double unit,
                                        std::string schema) {
    steplens::io::LoadedDocument loaded;
    loaded.doc = std::move(doc);
    loaded.lengthUnitToMm = unit;
    loaded.schema = std::move(schema);
    loaded.fileBytes = 1;
    return loaded;
}

TopoDS_Shape boxAt(double x, double y, double z, double dx = 10.0, double dy = 20.0, double dz = 30.0) {
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec{x, y, z});
    return BRepPrimAPI_MakeBox(dx, dy, dz).Shape().Moved(TopLoc_Location{trsf});
}

int faceCountOf(const TopoDS_Shape& shape) {
    int count = 0;
    for (TopExp_Explorer faces{shape, TopAbs_FACE}; faces.More(); faces.Next()) {
        ++count;
    }
    return count;
}

void expectNearBox(const Bnd_Box& box, double xmin, double ymin, double zmin, double xmax, double ymax,
                   double zmax) {
    ASSERT_FALSE(box.IsVoid());
    double x0 = 0.0;
    double y0 = 0.0;
    double z0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double z1 = 0.0;
    box.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x0, xmin, 1.0e-5);
    EXPECT_NEAR(y0, ymin, 1.0e-5);
    EXPECT_NEAR(z0, zmin, 1.0e-5);
    EXPECT_NEAR(x1, xmax, 1.0e-5);
    EXPECT_NEAR(y1, ymax, 1.0e-5);
    EXPECT_NEAR(z1, zmax, 1.0e-5);
}

std::optional<std::filesystem::path> samplesDirectory() {
#if defined(_WIN32)
    // getenv 在 MSVC /WX 下是 C4996。宽字符也躲开中文路径。
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

std::shared_ptr<const steplens::model::DocumentModel>
requireModel(steplens::Result<std::shared_ptr<const steplens::model::DocumentModel>> result) {
    if (!result.ok()) {
        ADD_FAILURE() << result.error().message;
        return {};
    }
    return result.value();
}

} // namespace

TEST(DocumentModel, StepAssemblyDedupsPrototypeAndKeepsWorldBoxes) {
    const int before = documentCount();
    auto doc = newDocument();
    ASSERT_FALSE(doc.IsNull());
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    auto colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    ASSERT_FALSE(shapes.IsNull());

    const TopoDS_Shape partShape = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    const TDF_Label part = shapes->AddShape(partShape, false);
    const char16_t boltChars[] = {static_cast<char16_t>(0x87BA), static_cast<char16_t>(0x6813), u'\0'};
    TDataStd_Name::Set(part, TCollection_ExtendedString{boltChars});

    const TDF_Label sub = shapes->NewShape();
    TDataStd_Name::Set(sub, TCollection_ExtendedString{"Sub"});
    gp_Trsf partInSub;
    partInSub.SetTranslation(gp_Vec{5.0, 0.0, 0.0});
    shapes->AddComponent(sub, part, TopLoc_Location{partInSub});

    const TDF_Label root = shapes->NewShape();
    TDataStd_Name::Set(root, TCollection_ExtendedString{"Asm"});
    gp_Trsf subInRoot;
    subInRoot.SetTranslation(gp_Vec{0.0, 7.0, 0.0});
    shapes->AddComponent(root, sub, TopLoc_Location{subInRoot});
    gp_Trsf boltInRoot;
    boltInRoot.SetTranslation(gp_Vec{0.0, 0.0, 3.0});
    const TDF_Label namedUse = shapes->AddComponent(root, part, TopLoc_Location{boltInRoot});
    TDataStd_Name::Set(namedUse, TCollection_ExtendedString{"Bolt-A"});
    colors->SetColor(namedUse, Quantity_Color{1.0, 0.0, 0.0, Quantity_TOC_RGB}, XCAFDoc_ColorGen);

    steplens::ProgressSink sink;
    sink.reset(1234);
    auto model = requireModel(steplens::model::DocumentModel::build(
        loadedFrom(doc, 25.4, "AUTOMOTIVE_DESIGN"), sink, steplens::CancelToken{}));
    ASSERT_TRUE(model);
    EXPECT_EQ(sink.fileBytes(), 1234u);
    EXPECT_DOUBLE_EQ(model->lengthUnitToMm(), 25.4);
    // 根装配 + 子装配 + 两处螺栓实例。两处实例共用一个原型。
    EXPECT_EQ(model->nodeCount(), 4u);
    EXPECT_EQ(model->prototypes().size(), 1u);
    EXPECT_EQ(model->prototype(0).instanceCount, 2u);
    EXPECT_EQ(model->leafNodesOf(0).size(), 2u);
    EXPECT_EQ(model->prototype(0).faceCount, 6u);
    EXPECT_TRUE(model->prototype(0).shape.Location().IsIdentity());
    EXPECT_EQ(faceCountOf(model->prototype(0).shape), 6u);

    const auto& asmNode = model->node(0);
    EXPECT_TRUE(asmNode.isAssembly);
    EXPECT_EQ(asmNode.prototype, steplens::model::kNoPrototype);
    EXPECT_EQ(asmNode.name, std::u16string{u"Asm"});
    EXPECT_EQ(asmNode.children.size(), 2u);

    const auto& subNode = model->node(asmNode.children[0]);
    EXPECT_TRUE(subNode.isAssembly);
    EXPECT_EQ(subNode.name, std::u16string{u"Sub"});
    EXPECT_EQ(subNode.parent, 0u);
    ASSERT_EQ(subNode.children.size(), 1u);

    const auto& nested = model->node(subNode.children[0]);
    EXPECT_FALSE(nested.isAssembly);
    EXPECT_EQ(nested.prototype, 0u);
    EXPECT_EQ(nested.name, std::u16string{u"\u87BA\u6813"});
    const gp_XYZ nestedAt = nested.worldLocation.Transformation().TranslationPart();
    EXPECT_NEAR(nestedAt.X(), 5.0, 1.0e-6);
    EXPECT_NEAR(nestedAt.Y(), 7.0, 1.0e-6);
    EXPECT_NEAR(nestedAt.Z(), 0.0, 1.0e-6);
    EXPECT_FALSE(nested.instanceColor.has_value());
    expectNearBox(nested.worldBox, 5.0, 7.0, 0.0, 15.0, 27.0, 30.0);

    const auto& direct = model->node(asmNode.children[1]);
    EXPECT_EQ(direct.name, std::u16string{u"Bolt-A"});
    EXPECT_EQ(direct.prototype, nested.prototype);
    ASSERT_TRUE(direct.instanceColor.has_value());
    EXPECT_NEAR(direct.instanceColor->GetRGB().Red(), 1.0, 1.0e-6);
    EXPECT_NEAR(direct.instanceColor->GetRGB().Green(), 0.0, 1.0e-6);
    const gp_XYZ directAt = direct.worldLocation.Transformation().TranslationPart();
    EXPECT_NEAR(directAt.X(), 0.0, 1.0e-6);
    EXPECT_NEAR(directAt.Y(), 0.0, 1.0e-6);
    EXPECT_NEAR(directAt.Z(), 3.0, 1.0e-6);
    expectNearBox(direct.worldBox, 0.0, 0.0, 3.0, 10.0, 20.0, 33.0);
    expectNearBox(model->worldBox(), 0.0, 0.0, 0.0, 15.0, 27.0, 33.0);

    const TopoDS_Shape nestedWorld = model->worldShapeOf(nested.id);
    EXPECT_NEAR(nestedWorld.Location().Transformation().TranslationPart().X(), 5.0, 1.0e-6);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(DocumentModel, StepFacesGroupByColor) {
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

    steplens::ProgressSink sink;
    sink.reset(1);
    auto model = requireModel(steplens::model::DocumentModel::build(loadedFrom(std::move(doc), 1.0, "AP214"),
                                                                    sink, steplens::CancelToken{}));
    ASSERT_TRUE(model);
    ASSERT_EQ(model->prototypes().size(), 1u);
    const auto& info = model->prototype(0);
    EXPECT_EQ(info.faceCount, 6u);
    ASSERT_EQ(info.colorGroups.size(), 2u);
    int grouped = 0;
    for (const auto& group : info.colorGroups) {
        const int faces = faceCountOf(group.second);
        EXPECT_EQ(faces, 3);
        grouped += faces;
        const double redChannel = group.first.GetRGB().Red();
        EXPECT_TRUE(redChannel > 0.9 || group.first.GetRGB().Blue() > 0.9);
    }
    EXPECT_EQ(grouped, 6);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(DocumentModel, StepFreeShapesStaySeparate) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    for (int index = 0; index < 5; ++index) {
        shapes->AddShape(boxAt(index * 40.0, 0.0, 0.0, 1.0, 1.0, 1.0), false);
    }
    steplens::ProgressSink sink;
    sink.reset(1);
    auto model = requireModel(steplens::model::DocumentModel::build(loadedFrom(std::move(doc), 1.0, "AP203"),
                                                                    sink, steplens::CancelToken{}));
    ASSERT_TRUE(model);
    // 多个自由零件上面加一个合成根，不按 IGES 规则合并。
    EXPECT_EQ(model->nodeCount(), 6u);
    EXPECT_EQ(model->prototypes().size(), 5u);
    EXPECT_TRUE(model->node(0).isAssembly);
    EXPECT_EQ(model->node(0).children.size(), 5u);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(DocumentModel, IgesFewEntitiesKeepOneRootAndSeparatePrototypes) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    auto colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    const Quantity_Color palette[] = {
        Quantity_Color{1.0, 0.0, 0.0, Quantity_TOC_RGB},
        Quantity_Color{0.0, 1.0, 0.0, Quantity_TOC_RGB},
        Quantity_Color{0.0, 0.0, 1.0, Quantity_TOC_RGB},
    };
    for (int index = 0; index < 3; ++index) {
        const TDF_Label label = shapes->AddShape(boxAt(index * 100.0, 0.0, 0.0), false);
        colors->SetColor(label, palette[index], XCAFDoc_ColorGen);
    }
    steplens::ProgressSink sink;
    sink.reset(1);
    auto model = requireModel(steplens::model::DocumentModel::build(loadedFrom(std::move(doc), 1.0, "IGES"),
                                                                    sink, steplens::CancelToken{}));
    ASSERT_TRUE(model);
    EXPECT_EQ(model->nodeCount(), 1u);
    EXPECT_TRUE(model->node(0).children.empty());
    EXPECT_EQ(model->prototypes().size(), 3u);
    int faces = 0;
    for (const auto& info : model->prototypes()) {
        EXPECT_EQ(info.instanceCount, 1u);
        ASSERT_EQ(model->leafNodesOf(info.id).size(), 1u);
        EXPECT_EQ(model->leafNodesOf(info.id)[0], 0u);
        EXPECT_EQ(info.colorGroups.size(), 1u);
        faces += static_cast<int>(info.faceCount);
    }
    EXPECT_EQ(faces, 18);
    double xmax = 0.0;
    double ignored = 0.0;
    model->worldBox().Get(ignored, ignored, ignored, xmax, ignored, ignored);
    EXPECT_GT(xmax, 200.0);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(DocumentModel, IgesChunkingCapsVirtualPrototypes) {
    constexpr int kEntities = 1001;
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    auto colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    for (int index = 0; index < kEntities; ++index) {
        const int gx = index % 16;
        const int gy = (index / 16) % 16;
        const int gz = index / 256;
        const TDF_Label label =
            shapes->AddShape(boxAt(gx * 10.0, gy * 10.0, gz * 10.0, 1.0, 1.0, 1.0), false);
        const double red = static_cast<double>(index % 251) / 250.0;
        const double green = static_cast<double>((index * 3) % 251) / 250.0;
        const double blue = static_cast<double>((index * 7) % 251) / 250.0;
        colors->SetColor(label, Quantity_Color{red, green, blue, Quantity_TOC_RGB}, XCAFDoc_ColorGen);
    }
    steplens::ProgressSink sink;
    sink.reset(1);
    steplens::CancelSource source;
    auto model = requireModel(
        steplens::model::DocumentModel::build(loadedFrom(std::move(doc), 1.0, "IGES"), sink, source.token()));
    ASSERT_TRUE(model);
    EXPECT_EQ(model->nodeCount(), 1u);
    EXPECT_TRUE(model->node(0).children.empty());
    EXPECT_LE(model->prototypes().size(),
              static_cast<std::size_t>(steplens::model::kIgesVirtualPrototypeBudget));
    EXPECT_LT(model->prototypes().size(), static_cast<std::size_t>(kEntities));
    EXPECT_GT(model->prototypes().size(), 0u);
    std::uint32_t faces = 0;
    std::uint32_t grouped = 0;
    std::set<int> redBuckets;
    for (const auto& info : model->prototypes()) {
        faces += info.faceCount;
        EXPECT_EQ(model->leafNodesOf(info.id).size(), 1u);
        for (const auto& group : info.colorGroups) {
            grouped += static_cast<std::uint32_t>(faceCountOf(group.second));
            redBuckets.insert(static_cast<int>(group.first.GetRGB().Red() * 255.0 + 0.5));
        }
    }
    EXPECT_EQ(faces, static_cast<std::uint32_t>(kEntities * 6));
    EXPECT_EQ(grouped, faces);
    EXPECT_GT(redBuckets.size(), 1u);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(DocumentModel, CancelDuringBuildReleasesDocument) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    for (int index = 0; index < 40; ++index) {
        shapes->AddShape(boxAt(index * 3.0, 0.0, 0.0, 1.0, 1.0, 1.0), false);
    }
    steplens::ProgressSink sink;
    sink.reset(8);
    steplens::CancelSource source;
    source.requestStop();
    sink.beginCancel();
    const auto result =
        steplens::model::DocumentModel::build(loadedFrom(std::move(doc), 1.0, "AP214"), sink, source.token());
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, steplens::ErrorCode::Cancelled);
    EXPECT_EQ(documentCount(), before);
    EXPECT_TRUE(sink.cancelFrozen());
}

TEST(DocumentModel, StepRoundTripKeepsInstanceCount) {
    const int before = documentCount();
    auto doc = newDocument();
    auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const TDF_Label part = shapes->AddShape(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(), false);
    TDataStd_Name::Set(part, TCollection_ExtendedString{"Peg"});
    const TDF_Label root = shapes->NewShape();
    gp_Trsf left;
    left.SetTranslation(gp_Vec{0.0, 0.0, 0.0});
    gp_Trsf right;
    right.SetTranslation(gp_Vec{5.0, 0.0, 0.0});
    shapes->AddComponent(root, part, TopLoc_Location{left});
    shapes->AddComponent(root, part, TopLoc_Location{right});
    shapes->UpdateAssemblies();

    steplens::test::TempDir dir;
    const auto path = dir.path() / "asm.stp";
    STEPCAFControl_Writer writer;
    ASSERT_TRUE(writer.Transfer(doc, STEPControl_AsIs));
    const std::u8string utf8 = path.u8string();
    const std::string bytes(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    ASSERT_EQ(writer.Write(bytes.c_str()), IFSelect_RetDone);
    {
        std::lock_guard lock(steplens::xcafDocumentMutex());
        XCAFApp_Application::GetApplication()->Close(doc);
    }
    doc.Nullify();

    steplens::ProgressSink sink;
    sink.reset(static_cast<std::uint64_t>(std::filesystem::file_size(path)));
    auto loader = steplens::io::makeLoader(path);
    ASSERT_TRUE(loader);
    auto loaded = loader->load(path, {}, sink, steplens::CancelToken{});
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    const float afterLoad = sink.fraction();
    auto model = requireModel(
        steplens::model::DocumentModel::build(std::move(loaded.value()), sink, steplens::CancelToken{}));
    ASSERT_TRUE(model);
    EXPECT_GE(sink.fraction(), afterLoad);
    EXPECT_EQ(model->prototypes().size(), 1u);
    EXPECT_EQ(model->prototype(0).instanceCount, 2u);
    EXPECT_EQ(model->nodeCount(), 3u);
    EXPECT_GT(model->lengthUnitToMm(), 0.0);
    model.reset();
    EXPECT_EQ(documentCount(), before);
}

TEST(DocumentModel, SampleStepCountsWhenPresent) {
    const auto root = samplesDirectory();
    if (!root) {
        GTEST_SKIP() << "STEPLENS_SAMPLES 未设置";
    }
    const std::filesystem::path& dir = *root;
    const auto screw = dir / "screw.step";
    const auto assembly = dir / "as1-ac-214.stp";
    if (!std::filesystem::exists(screw) || !std::filesystem::exists(assembly)) {
        GTEST_SKIP() << "S-01/S-03 不在 STEPLENS_SAMPLES";
    }

    const auto loadOne = [](const std::filesystem::path& file)
        -> steplens::Result<std::shared_ptr<const steplens::model::DocumentModel>> {
        steplens::ProgressSink sink;
        sink.reset(static_cast<std::uint64_t>(std::filesystem::file_size(file)));
        auto loader = steplens::io::makeLoader(file);
        EXPECT_TRUE(static_cast<bool>(loader));
        if (!loader) {
            return steplens::Error{steplens::ErrorCode::Internal, "null loader", "内部错误"};
        }
        auto loaded = loader->load(file, {}, sink, steplens::CancelToken{});
        EXPECT_TRUE(loaded.ok()) << loaded.error().message;
        if (!loaded.ok()) {
            return loaded.error();
        }
        return steplens::model::DocumentModel::build(std::move(loaded.value()), sink,
                                                     steplens::CancelToken{});
    };

    const int before = documentCount();
    auto screwModel = requireModel(loadOne(screw));
    ASSERT_TRUE(screwModel);
    // S-01 screw.step：单零件，文本统计 PRODUCT/NAUO = 1/0。
    EXPECT_EQ(screwModel->nodeCount(), 1u);
    EXPECT_EQ(screwModel->prototypes().size(), 1u);
    EXPECT_EQ(screwModel->prototype(0).instanceCount, 1u);
    screwModel.reset();

    auto assemblyModel = requireModel(loadOne(assembly));
    ASSERT_TRUE(assemblyModel);
    std::uint32_t instances = 0;
    for (const auto& info : assemblyModel->prototypes()) {
        instances += info.instanceCount;
    }
    // S-03 as1-ac-214.stp，OCCT 7.9.3 转成 XCAF 后的实测：
    // 28 个节点（10 个装配 + 18 个叶子），5 个原型，18 次实例引用。
    // 文本里的 PRODUCT/NAUO（9/13）只是粗统计，这里以装配树为准。
    EXPECT_EQ(assemblyModel->nodeCount(), 28u);
    EXPECT_EQ(assemblyModel->prototypes().size(), 5u);
    EXPECT_EQ(instances, 18u);
    assemblyModel.reset();
    EXPECT_EQ(documentCount(), before);
}
