#include "core/CancelToken.h"
#include "core/OcctLoadContract.h"
#include "core/ProgressSink.h"
#include "io/ILoader.h"
#include "model/DocumentModel.h"
#include "model/DocumentRelease.h"
#include "ui/AssemblyTreeModel.h"

#include <QCoreApplication>

#include <BRepPrimAPI_MakeBox.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>

namespace {

namespace model = steplens::model;
using steplens::ui::AssemblyTreeModel;

void ensureApplication() {
    static int argc = 1;
    static char name[] = "AssemblyTreeReleaseTest";
    static char* argv[] = {name, nullptr};
    static QCoreApplication application(argc, argv);
    (void)application;
}

int documentCount() {
    std::lock_guard lock(steplens::xcafDocumentMutex());
    return XCAFApp_Application::GetApplication()->NbDocuments();
}

std::shared_ptr<const model::DocumentModel> buildModel() {
    steplens::io::LoadedDocument loaded;
    {
        std::lock_guard lock(steplens::xcafDocumentMutex());
        XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", loaded.doc);
        XCAFDoc_DocumentTool::ShapeTool(loaded.doc->Main())
            ->AddShape(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape());
    }
    loaded.schema = "STEP";
    loaded.lengthUnitToMm = 1.0;
    steplens::ProgressSink progress;
    steplens::CancelToken cancel;
    auto result = model::DocumentModel::build(std::move(loaded), progress, cancel);
    return result.ok() ? std::move(result.value()) : nullptr;
}

struct ParentWithTree final : QObject {
    explicit ParentWithTree(std::shared_ptr<const model::DocumentModel> document)
        : member(std::move(document)), tree(new AssemblyTreeModel(this)) {
        tree->setDocument(member);
    }

    std::shared_ptr<const model::DocumentModel> member;
    AssemblyTreeModel* tree;
};

} // namespace

TEST(AssemblyTreeRelease, DroppingUnretiredTreeReferenceDoesNotCloseDisplayedDocument) {
    ensureApplication();
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    const int baseline = documentCount();
    auto document = buildModel();
    ASSERT_TRUE(document);
    ASSERT_EQ(documentCount(), baseline + 1);

    {
        AssemblyTreeModel tree;
        tree.setDocument(document);
        tree.setDocument(nullptr);
        EXPECT_EQ(model::documentsPendingRelease(), 0);
        EXPECT_EQ(documentCount(), baseline + 1);
    }
    document.reset();
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(documentCount(), baseline);
}

TEST(AssemblyTreeRelease, RetiredDocumentClosesAfterTreeDropsItsReference) {
    ensureApplication();
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    const int baseline = documentCount();
    auto document = buildModel();
    ASSERT_TRUE(document);
    AssemblyTreeModel tree;
    tree.setDocument(document);
    model::retireDocument(document);
    EXPECT_EQ(model::documentsPendingRelease(), 1);
    EXPECT_EQ(documentCount(), baseline + 1);

    tree.setDocument(nullptr);
    document.reset();
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_EQ(model::documentsPendingRelease(), 0);
    EXPECT_EQ(documentCount(), baseline);
}

// 这里只验证模型级 QObject 子树顺序；真实 MainWindow 成员/子树析构仍待产品 CI。
TEST(AssemblyTreeRelease, QObjectChildTreeDropsLastReferenceOffGuiThreadDuringParentTeardown) {
    ensureApplication();
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    auto document = buildModel();
    ASSERT_TRUE(document);
    const auto caller = std::this_thread::get_id();
    std::thread::id closeThread;
    model::setDocumentReleaseProbe([&](std::thread::id id) { closeThread = id; });
    {
        ParentWithTree parent(document);
        model::retireDocument(document);
        parent.member.reset();
        document.reset();
        EXPECT_EQ(model::documentsPendingRelease(), 1);
    }
    ASSERT_TRUE(model::waitForDocumentRelease(std::chrono::seconds(30)));
    EXPECT_NE(closeThread, caller);
    model::setDocumentReleaseProbe({});
}
