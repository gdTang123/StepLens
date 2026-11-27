#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "model/DocumentModel.h"
#include "ui/AssemblyTreeFilterProxy.h"
#include "ui/AssemblyTreeModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDataStd_Name.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <gtest/gtest.h>

namespace {

using namespace steplens;

void ensureApplication() {
    static int argc = 1;
    static char name[] = "AssemblyTreeFilterProxyTest";
    static char* argv[] = {name, nullptr};
    static QCoreApplication application(argc, argv);
    (void)application;
}

std::shared_ptr<const model::DocumentModel>
buildThreeLevel(const char16_t* parentName, const char16_t* childName, const char16_t* grandName) {
    opencascade::handle<TDocStd_Document> document;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", document);
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const TDF_Label part = shapes->AddShape(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape(), false);
    static_cast<void>(part.ForgetAttribute(TDataStd_Name::GetID()));
    const TDF_Label root = shapes->NewShape();
    TDataStd_Name::Set(root, TCollection_ExtendedString(parentName));
    const TDF_Label childAsm = shapes->NewShape();
    TDataStd_Name::Set(childAsm, TCollection_ExtendedString(childName));
    shapes->AddComponent(root, childAsm, TopLoc_Location{});
    const TDF_Label leaf = shapes->AddComponent(childAsm, part, TopLoc_Location{});
    TDataStd_Name::Set(leaf, TCollection_ExtendedString(grandName));

    io::LoadedDocument loaded;
    loaded.doc = std::move(document);
    loaded.lengthUnitToMm = 1.0;
    loaded.schema = "AP214";
    ProgressSink progress;
    progress.reset(1);
    auto result = model::DocumentModel::build(std::move(loaded), progress, CancelToken{});
    if (!result.ok()) {
        return {};
    }
    return result.value();
}

int visibleRows(const QAbstractItemModel& model, const QModelIndex& parent = {}) {
    return model.rowCount(parent);
}

} // namespace

TEST(AssemblyTreeFilterProxy, ChineseParentChildGrandchildHitKeepsAncestors) {
    ensureApplication();
    const auto document = buildThreeLevel(u"总装", u"子部件", u"零件甲");
    ASSERT_TRUE(document);
    ui::AssemblyTreeModel source;
    source.setDocument(document);
    ui::AssemblyTreeFilterProxy proxy;
    proxy.setSourceModel(&source);

    proxy.setNameFilter(QStringLiteral("零件甲"));
    ASSERT_EQ(visibleRows(proxy), 1);
    const QModelIndex root = proxy.index(0, 0);
    ASSERT_TRUE(root.isValid());
    EXPECT_EQ(proxy.data(root).toString(), QStringLiteral("总装"));
    ASSERT_EQ(visibleRows(proxy, root), 1);
    const QModelIndex child = proxy.index(0, 0, root);
    EXPECT_EQ(proxy.data(child).toString(), QStringLiteral("子部件"));
    ASSERT_EQ(visibleRows(proxy, child), 1);
    EXPECT_EQ(proxy.data(proxy.index(0, 0, child)).toString(), QStringLiteral("零件甲"));

    const QModelIndex sourceLeaf =
        source.indexOf(static_cast<model::NodeId>(proxy.mapToSource(proxy.index(0, 0, child)).internalId()));
    ASSERT_TRUE(sourceLeaf.isValid());
    const QModelIndex proxyLeaf = proxy.mapFromSource(sourceLeaf);
    ASSERT_TRUE(proxyLeaf.isValid());
    EXPECT_EQ(proxy.mapToSource(proxyLeaf), sourceLeaf);
}

TEST(AssemblyTreeFilterProxy, EnglishMissAndClearRestoresFullTree) {
    ensureApplication();
    const auto document = buildThreeLevel(u"RootAsm", u"ChildAsm", u"LeafPart");
    ASSERT_TRUE(document);
    ui::AssemblyTreeModel source;
    source.setDocument(document);
    ui::AssemblyTreeFilterProxy proxy;
    proxy.setSourceModel(&source);

    const int fullTop = visibleRows(proxy);
    ASSERT_EQ(fullTop, 1);
    proxy.setNameFilter(QStringLiteral("NoSuchName"));
    EXPECT_EQ(visibleRows(proxy), 0);

    proxy.setNameFilter({});
    EXPECT_EQ(visibleRows(proxy), fullTop);
    const QModelIndex root = proxy.index(0, 0);
    ASSERT_TRUE(root.isValid());
    EXPECT_EQ(proxy.data(root).toString(), QStringLiteral("RootAsm"));
    EXPECT_GE(visibleRows(proxy, root), 1);
}

TEST(AssemblyTreeFilterProxy, VisibilityUsesSourceIndex) {
    ensureApplication();
    qRegisterMetaType<model::NodeId>("steplens::model::NodeId");
    const auto document = buildThreeLevel(u"Root", u"Branch", u"Needle");
    ASSERT_TRUE(document);
    ui::AssemblyTreeModel source;
    source.setDocument(document);
    ui::AssemblyTreeFilterProxy proxy;
    proxy.setSourceModel(&source);
    proxy.setNameFilter(QStringLiteral("Needle"));

    const QModelIndex proxyRoot = proxy.index(0, 0);
    const QModelIndex proxyBranch = proxy.index(0, 0, proxyRoot);
    const QModelIndex proxyLeaf = proxy.index(0, 0, proxyBranch);
    const QModelIndex sourceLeaf = proxy.mapToSource(proxyLeaf);
    ASSERT_TRUE(sourceLeaf.isValid());

    QSignalSpy spy(&source, &ui::AssemblyTreeModel::visibilityChanged);
    ASSERT_TRUE(source.setData(sourceLeaf, Qt::Unchecked, Qt::CheckStateRole));
    EXPECT_EQ(source.data(sourceLeaf, Qt::CheckStateRole).toInt(), Qt::Unchecked);
    EXPECT_FALSE(spy.isEmpty());
}
