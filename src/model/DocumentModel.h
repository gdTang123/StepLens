#pragma once

// T06：把 io::LoadedDocument 收成不可变的装配树和原型表。
// 在加载线程调用 build，返回 shared_ptr<const> 之后任意线程只读。
// 三角化写网格（T07）、场景 AIS（T08）、装配树界面和测量都不在这里。
// IGES 顶层实体超过 kIgesVirtualPrototypeBudget 时，按颜色加 kIgesSpatialDivisions³
// 空间网格收成虚拟原型，数量不超过该上限；装配树只留一个根（PRD P0-6）。

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "io/ILoader.h"

#include <Quantity_ColorRGBA.hxx>

#include <Bnd_Box.hxx>
#include <TDF_Label.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace steplens::model {

using NodeId = std::uint32_t;
using PrototypeId = std::uint32_t;

// 0 可以是合法的原型编号。没有原型时用这个值。
inline constexpr PrototypeId kNoPrototype = std::numeric_limits<PrototypeId>::max();

// 建树时每隔这么多个节点看一次 CancelToken，结束前再看一次。
inline constexpr std::uint32_t kBuildCancelStride = 32;

// X-07 一类文件的虚拟原型上限，也是以后 AIS 对象数的上限。
inline constexpr std::uint32_t kIgesVirtualPrototypeBudget = 1000;

// 空间分块。8×8×8 = 512 格，再叠加颜色；超过预算时降低颜色精度，最后只留格子。
inline constexpr int kIgesSpatialDivisions = 8;

struct AssemblyNode {
    NodeId id = 0;
    NodeId parent = 0;
    std::vector<NodeId> children;
    std::u16string name;
    bool isAssembly = false;
    PrototypeId prototype = kNoPrototype;
    TopLoc_Location worldLocation;
    std::optional<Quantity_ColorRGBA> instanceColor;
    Bnd_Box worldBox;
};

struct PrototypeInfo {
    PrototypeId id = 0;
    // 原型定义标签。只在这份 DocumentModel 还活着（还握着 XCAF 文档）时有效。
    // 上层按 NodeId / PrototypeId 找节点，不要自己保存标签。
    TDF_Label label;
    TopoDS_Shape shape;
    std::uint32_t faceCount = 0;
    std::uint32_t instanceCount = 0;
    Bnd_Box localBox;
    // 同一颜色的面合成一个复合体，留给以后的三角化做绘制批次。
    std::vector<std::pair<Quantity_ColorRGBA, TopoDS_Compound>> colorGroups;
};

class DocumentModel {
public:
    class BuildKey {
        friend class DocumentModel;
        BuildKey() = default;
    };

    DocumentModel(BuildKey, opencascade::handle<TDocStd_Document> doc, double lengthUnitToMm);
    ~DocumentModel();

    DocumentModel(const DocumentModel&) = delete;
    DocumentModel& operator=(const DocumentModel&) = delete;
    DocumentModel(DocumentModel&&) = delete;
    DocumentModel& operator=(DocumentModel&&) = delete;

    // doc 的所有权转到返回的模型里，关闭时走 xcafDocumentMutex()。
    // 不调用 ProgressSink::reset。进度只在“生成显示”这一段里往前走。
    [[nodiscard]] static Result<std::shared_ptr<const DocumentModel>>
    build(io::LoadedDocument document, ProgressSink& progress, const CancelToken& cancel);

    [[nodiscard]] const AssemblyNode& node(NodeId id) const;
    [[nodiscard]] std::size_t nodeCount() const noexcept;
    [[nodiscard]] const PrototypeInfo& prototype(PrototypeId id) const;
    [[nodiscard]] std::span<const PrototypeInfo> prototypes() const noexcept;
    [[nodiscard]] std::span<const NodeId> leafNodesOf(PrototypeId id) const;
    [[nodiscard]] const Bnd_Box& worldBox() const noexcept;
    [[nodiscard]] double lengthUnitToMm() const noexcept;
    [[nodiscard]] TopoDS_Shape worldShapeOf(NodeId id) const;

private:
    opencascade::handle<TDocStd_Document> m_doc;
    std::vector<AssemblyNode> m_nodes;
    std::vector<PrototypeInfo> m_prototypes;
    std::vector<std::vector<NodeId>> m_leaves;
    Bnd_Box m_worldBox;
    double m_lengthUnitToMm = 1.0;
};

} // namespace steplens::model
