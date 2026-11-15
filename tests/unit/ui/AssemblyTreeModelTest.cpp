#include "core/CancelToken.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "model/DocumentModel.h"
#include "ui/AssemblyTreeModel.h"

#include <QAbstractItemModelTester>
#include <QElapsedTimer>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDataStd_Name.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace {

using namespace steplens;

std::shared_ptr<const model::DocumentModel> buildModel(std::size_t instanceCount, bool withAssembly = true) {
    opencascade::handle<TDocStd_Document> document;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", document);
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const TDF_Label part = shapes->AddShape(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape(), false);
    const TDF_Label root = withAssembly ? shapes->NewShape() : part;
    if (withAssembly)
        TDataStd_Name::Set(root, TCollection_ExtendedString{"总装"});
    for (std::size_t i = 0; i < instanceCount; ++i) {
        const TDF_Label instance = shapes->AddComponent(root, part, TopLoc_Location{});
        if (i == 0)
            TDataStd_Name::Set(instance, TCollection_ExtendedString{"已命名零件"});
    }
    io::LoadedDocument loaded;
    loaded.doc = std::move(document);
    loaded.lengthUnitToMm = 1.0;
    loaded.schema = "AP214";
    ProgressSink progress;
    progress.reset(1);
    auto result = model::DocumentModel::build(std::move(loaded), progress, CancelToken{});
    if (!result.ok())
        return {};
    return result.value();
}

void visit(const ui::AssemblyTreeModel& tree, const QModelIndex& parent, std::vector<model::NodeId>& ids) {
    const int count = tree.rowCount(parent);
    for (int row = 0; row < count; ++row) {
        const QModelIndex child = tree.index(row, 0, parent);
        ids.push_back(static_cast<model::NodeId>(child.internalId()));
        visit(tree, child, ids);
    }
}

} // namespace

TEST(AssemblyTreeModel, HierarchyNamesVisibilityAndNodeMapping) {
    const auto document = buildModel(2);
    ASSERT_TRUE(document);
    ui::AssemblyTreeModel tree;
    QAbstractItemModelTester tester(&tree, QAbstractItemModelTester::FailureReportingMode::QtTest);
    tree.setDocument(document);

    const QModelIndex root = tree.index(0, 0);
    ASSERT_TRUE(root.isValid());
    EXPECT_EQ(tree.data(root).toString(), QStringLiteral("总装"));
    ASSERT_EQ(tree.rowCount(root), 2);
    const QModelIndex named = tree.index(0, 0, root);
    const QModelIndex unnamed = tree.index(1, 0, root);
    EXPECT_EQ(tree.data(named).toString(), QStringLiteral("已命名零件"));
    EXPECT_EQ(tree.data(unnamed).toString(), QStringLiteral("未命名-3"));
    EXPECT_EQ(tree.indexOf(static_cast<model::NodeId>(unnamed.internalId())), unnamed);
    EXPECT_EQ(tree.parent(unnamed), root);

    ASSERT_TRUE(tree.setData(root, Qt::Unchecked, Qt::CheckStateRole));
    EXPECT_EQ(tree.data(named, Qt::CheckStateRole).toInt(), Qt::Unchecked);
    EXPECT_EQ(tree.data(unnamed, Qt::CheckStateRole).toInt(), Qt::Unchecked);
    ASSERT_TRUE(tree.setData(root, Qt::Checked, Qt::CheckStateRole));
    EXPECT_EQ(tree.data(named, Qt::CheckStateRole).toInt(), Qt::Checked);
}

TEST(AssemblyTreeModel, UnstructuredDocumentIsOneRoot) {
    const auto document = buildModel(0, false);
    ASSERT_TRUE(document);
    ui::AssemblyTreeModel tree;
    QAbstractItemModelTester tester(&tree, QAbstractItemModelTester::FailureReportingMode::QtTest);
    tree.setDocument(document);
    EXPECT_EQ(tree.rowCount(), 1);
    EXPECT_EQ(tree.rowCount(tree.index(0, 0)), 0);
}

TEST(AssemblyTreeModel, TwoThousandNodesExpandAndMapWithinBudget) {
    const auto document = buildModel(1999);
    ASSERT_TRUE(document);
    ui::AssemblyTreeModel tree;
    QAbstractItemModelTester tester(&tree, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QElapsedTimer timer;
    timer.start();
    tree.setDocument(document);
    std::vector<model::NodeId> ids;
    ids.reserve(document->nodeCount());
    visit(tree, {}, ids);
    ASSERT_EQ(ids.size(), document->nodeCount());
    for (const auto id : ids)
        ASSERT_TRUE(tree.indexOf(id).isValid());
    const qint64 elapsed = timer.elapsed();
    EXPECT_LT(elapsed, 200) << "2000 节点展开与双向查找耗时 " << elapsed << " ms";
}
