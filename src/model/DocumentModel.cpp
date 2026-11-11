#include "model/DocumentModel.h"

#include "core/Log.h"
#include "core/OcctLoadContract.h"
#include "io/HeapRelease.h"
#include "model/DocumentRelease.h"
#include "occtsupport/OcctBoundary.h"
#include "occtsupport/OcctExceptions.h"

#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <NCollection_Sequence.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Iterator.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace steplens::model {
namespace {

// 8.0 弃用了 TDF_LabelSequence 这个名字。7.9.3 里它就是这个序列。
using LabelSequence = NCollection_Sequence<TDF_Label>;

struct ColorKey {
    bool colored = false;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;

    friend bool operator<(const ColorKey& left, const ColorKey& right) {
        if (left.colored != right.colored) {
            return !left.colored && right.colored;
        }
        if (left.red != right.red) {
            return left.red < right.red;
        }
        if (left.green != right.green) {
            return left.green < right.green;
        }
        if (left.blue != right.blue) {
            return left.blue < right.blue;
        }
        return left.alpha < right.alpha;
    }
};

struct SourceEntity {
    TopoDS_Shape shape;
    TDF_Label label;
    Quantity_ColorRGBA color;
    bool hasColor = false;
    Bnd_Box bounds;
    gp_Pnt center;
};

struct BucketKey {
    int ix = 0;
    int iy = 0;
    int iz = 0;
    ColorKey color;

    friend bool operator<(const BucketKey& left, const BucketKey& right) {
        if (left.ix != right.ix) {
            return left.ix < right.ix;
        }
        if (left.iy != right.iy) {
            return left.iy < right.iy;
        }
        if (left.iz != right.iz) {
            return left.iz < right.iz;
        }
        return left.color < right.color;
    }
};

struct BuiltModel {
    std::optional<Error> error;
    std::vector<AssemblyNode> nodes;
    std::vector<PrototypeInfo> prototypes;
    std::vector<std::vector<NodeId>> leaves;
    Bnd_Box worldBox;
};

void closeDocument(opencascade::handle<TDocStd_Document>& doc) noexcept {
    if (doc.IsNull()) {
        return;
    }
    try {
        std::lock_guard lock(xcafDocumentMutex());
        if (!doc.IsNull()) {
            XCAFApp_Application::GetApplication()->Close(doc);
        }
    } catch (...) {
    }
    doc.Nullify();
    // 形状和文档上的空闲页一起还给系统。句柄已经空时也要回收，关闭路径刚清掉网格。
    io::releaseFreedHeap();
}

class DocumentOwner {
public:
    explicit DocumentOwner(opencascade::handle<TDocStd_Document> doc) : m_doc(std::move(doc)) {}

    ~DocumentOwner() {
        closeDocument(m_doc);
    }

    DocumentOwner(const DocumentOwner&) = delete;
    DocumentOwner& operator=(const DocumentOwner&) = delete;

    [[nodiscard]] const opencascade::handle<TDocStd_Document>& get() const {
        return m_doc;
    }

    void detach() {
        m_doc.Nullify();
    }

private:
    opencascade::handle<TDocStd_Document> m_doc;
};

[[nodiscard]] Error makeFailure(ErrorCode code, std::string message, std::string userMessage) {
    Log::error(message);
    return Error{code, std::move(message), std::move(userMessage)};
}

[[nodiscard]] double sanitizeUnit(double value) noexcept {
    if (!(value > 0.0)) {
        return 1.0;
    }
    return value;
}

[[nodiscard]] std::string labelKey(const TDF_Label& label) {
    if (label.IsNull()) {
        return {};
    }
    TCollection_AsciiString entry;
    TDF_Tool::Entry(label, entry);
    const char* text = entry.ToCString();
    if (text == nullptr) {
        return {};
    }
    return std::string{text};
}

[[nodiscard]] std::u16string toUtf16(const TCollection_ExtendedString& text) {
    const int length = text.Length();
    if (length <= 0) {
        return {};
    }
    std::u16string out(static_cast<std::size_t>(length), u'\0');
    for (int index = 1; index <= length; ++index) {
        out[static_cast<std::size_t>(index - 1)] = static_cast<char16_t>(text.Value(index));
    }
    return out;
}

[[nodiscard]] std::u16string readName(const TDF_Label& label) {
    if (label.IsNull()) {
        return {};
    }
    opencascade::handle<TDataStd_Name> name;
    if (!label.FindAttribute(TDataStd_Name::GetID(), name) || name.IsNull()) {
        return {};
    }
    return toUtf16(name->Get());
}

// XCAF 给引用自动写 "=>[0:1:1:2]" 这种标签路径。那不是文件里的零件名。
[[nodiscard]] bool isGeneratedName(const std::u16string& name) {
    return name.size() >= 3 && name[0] == u'=' && name[1] == u'>' && name[2] == u'[';
}

[[nodiscard]] std::u16string pickName(const TDF_Label& occurrence, const TDF_Label& definition) {
    const std::u16string fromOccurrence = readName(occurrence);
    if (!fromOccurrence.empty() && !isGeneratedName(fromOccurrence)) {
        return fromOccurrence;
    }
    const std::u16string fromDefinition = readName(definition);
    if (!fromDefinition.empty() && !isGeneratedName(fromDefinition)) {
        return fromDefinition;
    }
    return {};
}

[[nodiscard]] Quantity_ColorRGBA defaultFaceColor() {
    return Quantity_ColorRGBA{Quantity_Color{0.8, 0.8, 0.8, Quantity_TOC_RGB}, 1.0F};
}

[[nodiscard]] bool readLabelColor(const opencascade::handle<XCAFDoc_ColorTool>& colors,
                                  const TDF_Label& label, Quantity_ColorRGBA& color) {
    if (colors.IsNull() || label.IsNull()) {
        return false;
    }
    if (colors->GetColor(label, XCAFDoc_ColorSurf, color)) {
        return true;
    }
    return static_cast<bool>(colors->GetColor(label, XCAFDoc_ColorGen, color));
}

[[nodiscard]] bool readShapeColor(const opencascade::handle<XCAFDoc_ColorTool>& colors,
                                  const TopoDS_Shape& shape, Quantity_ColorRGBA& color) {
    if (colors.IsNull() || shape.IsNull()) {
        return false;
    }
    if (colors->GetColor(shape, XCAFDoc_ColorSurf, color)) {
        return true;
    }
    return static_cast<bool>(colors->GetColor(shape, XCAFDoc_ColorGen, color));
}

[[nodiscard]] ColorKey colorKeyOf(const Quantity_ColorRGBA& color, int bits) {
    ColorKey key;
    key.colored = bits > 0;
    if (!key.colored) {
        return key;
    }
    const auto channel = [bits](double value) {
        if (!(value > 0.0)) {
            value = 0.0;
        }
        if (value > 1.0) {
            value = 1.0;
        }
        int scaled = static_cast<int>(value * 255.0 + 0.5);
        if (scaled < 0) {
            scaled = 0;
        }
        if (scaled > 255) {
            scaled = 255;
        }
        if (bits < 8) {
            const int shift = 8 - bits;
            scaled = (scaled >> shift) << shift;
        }
        return static_cast<std::uint8_t>(scaled);
    };
    key.red = channel(color.GetRGB().Red());
    key.green = channel(color.GetRGB().Green());
    key.blue = channel(color.GetRGB().Blue());
    key.alpha = channel(static_cast<double>(color.Alpha()));
    return key;
}

[[nodiscard]] Bnd_Box boundOf(const TopoDS_Shape& shape) {
    Bnd_Box box;
    if (!shape.IsNull()) {
        BRepBndLib::Add(shape, box, false);
    }
    return box;
}

[[nodiscard]] bool finiteBox(const Bnd_Box& box, double& xmin, double& ymin, double& zmin, double& xmax,
                             double& ymax, double& zmax) {
    if (box.IsVoid() || box.IsOpen()) {
        return false;
    }
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    return true;
}

[[nodiscard]] gp_Pnt boxCenter(const Bnd_Box& box) {
    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    if (!finiteBox(box, xmin, ymin, zmin, xmax, ymax, zmax)) {
        return gp_Pnt{0.0, 0.0, 0.0};
    }
    return gp_Pnt{0.5 * (xmin + xmax), 0.5 * (ymin + ymax), 0.5 * (zmin + zmax)};
}

[[nodiscard]] int cellIndex(double value, double low, double high, int divisions) {
    if (divisions <= 1 || !(high > low)) {
        return 0;
    }
    double scale = (value - low) / (high - low);
    if (!(scale > 0.0)) {
        return 0;
    }
    if (scale >= 1.0) {
        return divisions - 1;
    }
    const int index = static_cast<int>(scale * static_cast<double>(divisions));
    if (index < 0) {
        return 0;
    }
    if (index >= divisions) {
        return divisions - 1;
    }
    return index;
}

void appendFaces(PrototypeInfo& info, const TopoDS_Shape& shape,
                 const opencascade::handle<XCAFDoc_ColorTool>& colors, const Quantity_ColorRGBA& fallback,
                 bool fallbackColored, std::map<ColorKey, std::size_t>& groupIndex) {
    if (shape.IsNull()) {
        return;
    }
    BRep_Builder builder;
    for (TopExp_Explorer faces{shape, TopAbs_FACE}; faces.More(); faces.Next()) {
        const TopoDS_Shape face = faces.Current();
        if (face.IsNull()) {
            continue;
        }
        ++info.faceCount;
        Quantity_ColorRGBA faceColor = fallback;
        bool colored = fallbackColored;
        Quantity_ColorRGBA looked;
        if (readShapeColor(colors, face, looked)) {
            faceColor = looked;
            colored = true;
        }
        const ColorKey key = colored ? colorKeyOf(faceColor, 8) : ColorKey{};
        const auto found = groupIndex.find(key);
        std::size_t index = 0;
        if (found == groupIndex.end()) {
            index = info.colorGroups.size();
            groupIndex.emplace(key, index);
            TopoDS_Compound compound;
            builder.MakeCompound(compound);
            info.colorGroups.emplace_back(faceColor, compound);
        } else {
            index = found->second;
        }
        builder.Add(info.colorGroups[index].second, face);
    }
}

void paintPrototype(PrototypeInfo& info, const opencascade::handle<XCAFDoc_ColorTool>& colors) {
    info.localBox = boundOf(info.shape);
    Quantity_ColorRGBA fallback = defaultFaceColor();
    bool fallbackColored = readLabelColor(colors, info.label, fallback);
    if (!fallbackColored) {
        fallback = defaultFaceColor();
    }
    std::map<ColorKey, std::size_t> groupIndex;
    appendFaces(info, info.shape, colors, fallback, fallbackColored, groupIndex);
}

void paintMembers(PrototypeInfo& info, const std::vector<SourceEntity>& entities,
                  const std::vector<std::size_t>& members,
                  const opencascade::handle<XCAFDoc_ColorTool>& colors) {
    info.localBox = boundOf(info.shape);
    std::map<ColorKey, std::size_t> groupIndex;
    for (std::size_t index : members) {
        const SourceEntity& entity = entities[index];
        const Quantity_ColorRGBA fallback = entity.hasColor ? entity.color : defaultFaceColor();
        appendFaces(info, entity.shape, colors, fallback, entity.hasColor, groupIndex);
    }
}

class CancelGate {
public:
    CancelGate(ProgressSink& progress, const CancelToken& cancel, std::uint32_t expected)
        : m_progress(progress), m_cancel(cancel), m_expected(std::max<std::uint32_t>(expected, 1)) {}

    void checkpoint() {
        ++m_visited;
        if (m_visited == 1 || (m_visited % kBuildCancelStride) == 0) {
            m_cancel.throwIfCancelled();
            publish();
        }
    }

    void finish() {
        m_cancel.throwIfCancelled();
        publish();
    }

private:
    void publish() {
        const std::uint32_t denom = std::max(m_expected, m_visited);
        float portion = static_cast<float>(m_visited) / static_cast<float>(denom);
        if (portion > 0.99F) {
            portion = 0.99F;
        }
        m_progress.setPresentFaceFraction(portion);
    }

    ProgressSink& m_progress;
    const CancelToken& m_cancel;
    std::uint32_t m_expected = 1;
    std::uint32_t m_visited = 0;
};

class StepWalker {
public:
    StepWalker(const opencascade::handle<XCAFDoc_ShapeTool>& shapes,
               const opencascade::handle<XCAFDoc_ColorTool>& colors, CancelGate& gate)
        : m_shapes(shapes), m_colors(colors), m_gate(gate) {}

    [[nodiscard]] BuiltModel run(const LabelSequence& roots) {
        BuiltModel built;
        if (roots.IsEmpty()) {
            built.error =
                makeFailure(ErrorCode::Internal, "document has no free shapes", "文件里没有可显示的形状");
            return built;
        }
        if (roots.Length() == 1) {
            addOccurrence(roots.Value(1), 0, TopLoc_Location{}, false);
        } else {
            addSyntheticRoot();
            for (int index = 1; index <= roots.Length(); ++index) {
                addOccurrence(roots.Value(index), 0, TopLoc_Location{}, true);
            }
            unionChildren(0);
        }
        if (m_nodes.empty()) {
            built.error =
                makeFailure(ErrorCode::Internal, "assembly walk produced no nodes", "文件里没有可显示的形状");
            return built;
        }
        built.nodes = std::move(m_nodes);
        built.prototypes = std::move(m_prototypes);
        built.leaves = std::move(m_leaves);
        built.worldBox = built.nodes.front().worldBox;
        return built;
    }

private:
    void addSyntheticRoot() {
        m_gate.checkpoint();
        AssemblyNode root;
        root.id = 0;
        root.parent = 0;
        root.isAssembly = true;
        root.prototype = kNoPrototype;
        m_nodes.push_back(std::move(root));
    }

    void addOccurrence(const TDF_Label& occurrence, NodeId parentId, const TopLoc_Location& parentWorld,
                       bool attach) {
        m_gate.checkpoint();
        if (occurrence.IsNull()) {
            return;
        }
        TDF_Label definition = occurrence;
        if (!XCAFDoc_ShapeTool::GetReferredShape(occurrence, definition) || definition.IsNull()) {
            definition = occurrence;
        }
        const TopLoc_Location world = parentWorld * XCAFDoc_ShapeTool::GetLocation(occurrence);
        const std::string key = labelKey(definition);
        const bool cycle = !key.empty() && std::find(m_stack.begin(), m_stack.end(), key) != m_stack.end();
        const bool assembly = !cycle && m_stack.size() < 256 && XCAFDoc_ShapeTool::IsAssembly(definition);

        AssemblyNode created;
        created.id = static_cast<NodeId>(m_nodes.size());
        created.parent = attach ? parentId : 0;
        created.worldLocation = world;
        created.name = pickName(occurrence, definition);
        if (!occurrence.IsEqual(definition)) {
            Quantity_ColorRGBA color;
            if (readLabelColor(m_colors, occurrence, color)) {
                created.instanceColor = color;
            }
        }

        if (assembly) {
            created.isAssembly = true;
            created.prototype = kNoPrototype;
            const NodeId id = created.id;
            m_nodes.push_back(std::move(created));
            if (attach) {
                m_nodes[parentId].children.push_back(id);
            }
            m_stack.push_back(key);
            LabelSequence components;
            if (!m_shapes.IsNull()) {
                m_shapes->GetComponents(definition, components);
            }
            for (int index = 1; index <= components.Length(); ++index) {
                addOccurrence(components.Value(index), id, world, true);
            }
            m_stack.pop_back();
            unionChildren(id);
            return;
        }

        const PrototypeId prototypeId = intern(definition);
        created.isAssembly = false;
        created.prototype = prototypeId;
        const NodeId id = created.id;
        m_nodes.push_back(std::move(created));
        if (attach) {
            m_nodes[parentId].children.push_back(id);
        }
        if (prototypeId < m_prototypes.size()) {
            ++m_prototypes[prototypeId].instanceCount;
            m_leaves[prototypeId].push_back(id);
            TopoDS_Shape located = m_prototypes[prototypeId].shape;
            located.Move(world);
            m_nodes[id].worldBox = boundOf(located);
        }
    }

    void unionChildren(NodeId id) {
        Bnd_Box box;
        for (NodeId child : m_nodes[id].children) {
            if (!m_nodes[child].worldBox.IsVoid()) {
                box.Add(m_nodes[child].worldBox);
            }
        }
        m_nodes[id].worldBox = box;
    }

    [[nodiscard]] PrototypeId intern(const TDF_Label& definition) {
        const std::string key = labelKey(definition);
        const auto found = m_byLabel.find(key);
        if (found != m_byLabel.end()) {
            return found->second;
        }
        PrototypeInfo info;
        info.id = static_cast<PrototypeId>(m_prototypes.size());
        info.label = definition;
        info.shape = XCAFDoc_ShapeTool::GetShape(definition);
        if (!info.shape.IsNull()) {
            info.shape.Location(TopLoc_Location{});
        }
        paintPrototype(info, m_colors);
        const PrototypeId id = info.id;
        m_byLabel.emplace(key, id);
        m_prototypes.push_back(std::move(info));
        m_leaves.emplace_back();
        return id;
    }

    opencascade::handle<XCAFDoc_ShapeTool> m_shapes;
    opencascade::handle<XCAFDoc_ColorTool> m_colors;
    CancelGate& m_gate;
    std::vector<AssemblyNode> m_nodes;
    std::vector<PrototypeInfo> m_prototypes;
    std::vector<std::vector<NodeId>> m_leaves;
    std::map<std::string, PrototypeId> m_byLabel;
    std::vector<std::string> m_stack;
};

void collectEntities(const TopoDS_Shape& shape, const TDF_Label& label,
                     const opencascade::handle<XCAFDoc_ColorTool>& colors, int depth, CancelGate& gate,
                     std::vector<SourceEntity>& out) {
    if (shape.IsNull()) {
        return;
    }
    const bool explode =
        depth < 32 && (shape.ShapeType() == TopAbs_COMPOUND || shape.ShapeType() == TopAbs_COMPSOLID);
    if (explode) {
        std::vector<TopoDS_Shape> children;
        for (TopoDS_Iterator it{shape}; it.More(); it.Next()) {
            if (!it.Value().IsNull()) {
                children.push_back(it.Value());
            }
        }
        if (children.size() > 1) {
            for (const TopoDS_Shape& child : children) {
                collectEntities(child, label, colors, depth + 1, gate, out);
            }
            return;
        }
    }

    gate.checkpoint();
    SourceEntity entity;
    entity.shape = shape;
    entity.label = label;
    entity.hasColor =
        readShapeColor(colors, shape, entity.color) || readLabelColor(colors, label, entity.color);
    entity.bounds = boundOf(shape);
    entity.center = boxCenter(entity.bounds);
    out.push_back(std::move(entity));
}

[[nodiscard]] TopoDS_Shape compoundOf(const std::vector<SourceEntity>& entities,
                                      const std::vector<std::size_t>& members, CancelGate& gate) {
    if (members.size() == 1) {
        gate.checkpoint();
        return entities[members.front()].shape;
    }
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (std::size_t index : members) {
        gate.checkpoint();
        if (!entities[index].shape.IsNull()) {
            builder.Add(compound, entities[index].shape);
        }
    }
    return compound;
}

[[nodiscard]] BuiltModel buildIges(const opencascade::handle<XCAFDoc_ColorTool>& colors,
                                   const LabelSequence& roots, CancelGate& gate) {
    BuiltModel built;
    std::vector<SourceEntity> entities;
    for (int index = 1; index <= roots.Length(); ++index) {
        const TDF_Label label = roots.Value(index);
        collectEntities(XCAFDoc_ShapeTool::GetShape(label), label, colors, 0, gate, entities);
    }
    if (entities.empty()) {
        built.error =
            makeFailure(ErrorCode::Internal, "IGES document has no shapes", "文件里没有可显示的形状");
        return built;
    }

    std::map<BucketKey, std::vector<std::size_t>> buckets;
    if (entities.size() <= kIgesVirtualPrototypeBudget) {
        for (std::size_t index = 0; index < entities.size(); ++index) {
            BucketKey key;
            key.ix = static_cast<int>(index);
            key.color = entities[index].hasColor ? colorKeyOf(entities[index].color, 8) : ColorKey{};
            buckets[key].push_back(index);
        }
    } else {
        Bnd_Box global;
        for (const SourceEntity& entity : entities) {
            if (!entity.bounds.IsVoid()) {
                global.Add(entity.bounds);
            }
        }
        double xmin = 0.0;
        double ymin = 0.0;
        double zmin = 0.0;
        double xmax = 0.0;
        double ymax = 0.0;
        double zmax = 0.0;
        const bool finite = finiteBox(global, xmin, ymin, zmin, xmax, ymax, zmax);
        const auto regroup = [&](int bits) {
            buckets.clear();
            for (std::size_t index = 0; index < entities.size(); ++index) {
                BucketKey key;
                if (finite) {
                    const gp_Pnt center = entities[index].center;
                    key.ix = cellIndex(center.X(), xmin, xmax, kIgesSpatialDivisions);
                    key.iy = cellIndex(center.Y(), ymin, ymax, kIgesSpatialDivisions);
                    key.iz = cellIndex(center.Z(), zmin, zmax, kIgesSpatialDivisions);
                }
                key.color = entities[index].hasColor ? colorKeyOf(entities[index].color, bits) : ColorKey{};
                buckets[key].push_back(index);
            }
        };
        int bits = 8;
        regroup(bits);
        while (buckets.size() > kIgesVirtualPrototypeBudget && bits > 0) {
            bits = bits > 2 ? bits - 2 : 0;
            regroup(bits);
        }
        while (buckets.size() > kIgesVirtualPrototypeBudget) {
            auto last = std::prev(buckets.end());
            auto into = buckets.begin();
            if (into == last) {
                break;
            }
            into->second.insert(into->second.end(), last->second.begin(), last->second.end());
            buckets.erase(last);
        }
    }

    built.prototypes.reserve(buckets.size());
    built.leaves.reserve(buckets.size());
    Bnd_Box world;
    for (const auto& entry : buckets) {
        const std::vector<std::size_t>& members = entry.second;
        if (members.empty()) {
            continue;
        }
        const SourceEntity& first = entities[members.front()];
        PrototypeInfo info;
        info.id = static_cast<PrototypeId>(built.prototypes.size());
        info.instanceCount = 1;
        info.shape = compoundOf(entities, members, gate);
        if (members.size() == 1) {
            info.label = first.label;
            paintPrototype(info, colors);
        } else {
            paintMembers(info, entities, members, colors);
        }
        if (!info.localBox.IsVoid()) {
            world.Add(info.localBox);
        }
        built.leaves.push_back(std::vector<NodeId>{0});
        built.prototypes.push_back(std::move(info));
    }
    if (built.prototypes.size() > kIgesVirtualPrototypeBudget) {
        built.error = makeFailure(ErrorCode::Internal, "IGES virtual prototype budget exceeded", "内部错误");
        return built;
    }

    AssemblyNode root;
    root.id = 0;
    root.parent = 0;
    root.isAssembly = built.prototypes.size() != 1;
    root.prototype = built.prototypes.size() == 1 ? 0 : kNoPrototype;
    root.worldBox = world;
    if (entities.size() == 1) {
        root.name = pickName(entities.front().label, entities.front().label);
    }
    built.nodes.push_back(std::move(root));
    built.worldBox = world;
    return built;
}

[[nodiscard]] BuiltModel assemble(const opencascade::handle<TDocStd_Document>& doc, const std::string& schema,
                                  ProgressSink& progress, const CancelToken& cancel) {
    BuiltModel built;
    if (doc.IsNull()) {
        built.error = makeFailure(ErrorCode::Internal, "null XCAF document", "没有可读取的文档");
        return built;
    }
    const opencascade::handle<XCAFDoc_ShapeTool> shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const opencascade::handle<XCAFDoc_ColorTool> colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    if (shapes.IsNull()) {
        built.error = makeFailure(ErrorCode::Internal, "ShapeTool missing", "没有可读取的文档");
        return built;
    }
    LabelSequence roots;
    shapes->GetFreeShapes(roots);
    if (roots.IsEmpty()) {
        built.error =
            makeFailure(ErrorCode::Internal, "document has no free shapes", "文件里没有可显示的形状");
        return built;
    }
    CancelGate gate{progress, cancel, static_cast<std::uint32_t>(std::max(roots.Length(), 1))};
    if (schema == "IGES") {
        built = buildIges(colors, roots, gate);
    } else {
        StepWalker walker{shapes, colors, gate};
        built = walker.run(roots);
    }
    if (!built.error) {
        gate.finish();
    }
    return built;
}

} // namespace

DocumentModel::DocumentModel(BuildKey, opencascade::handle<TDocStd_Document> doc, double lengthUnitToMm)
    : m_doc(std::move(doc)), m_lengthUnitToMm(sanitizeUnit(lengthUnitToMm)) {}

DocumentModel::~DocumentModel() {
    // 探针看到的就是这次 Close 所在的线程。GUI 不直接析构文档，最后一份引用在后台 retirer 上丢掉。
    noteDocumentDestructorThread();
    // 形状和网格先丢掉，再关 XCAF。closeDocument 把空闲堆还给系统。
    m_nodes.clear();
    m_prototypes.clear();
    m_leaves.clear();
    closeDocument(m_doc);
}

Result<std::shared_ptr<const DocumentModel>>
DocumentModel::build(io::LoadedDocument document, ProgressSink& progress, const CancelToken& cancel) {
    occt::OcctBoundary::armWorkerThread();
    auto result = occt::catchOcctAsResult(
        [&]() -> Result<std::shared_ptr<const DocumentModel>> {
            DocumentOwner owner{std::move(document.doc)};
            const double unit = document.lengthUnitToMm;
            BuiltModel built = assemble(owner.get(), document.schema, progress, cancel);
            if (built.error) {
                return *built.error;
            }
            if (built.nodes.empty()) {
                return makeFailure(ErrorCode::Internal, "empty model", "文件里没有可显示的形状");
            }
            auto model = std::make_shared<DocumentModel>(BuildKey{}, owner.get(), unit);
            owner.detach();
            model->m_nodes = std::move(built.nodes);
            model->m_prototypes = std::move(built.prototypes);
            model->m_leaves = std::move(built.leaves);
            model->m_worldBox = std::move(built.worldBox);
            return std::shared_ptr<const DocumentModel>{std::move(model)};
        },
        ErrorCode::Internal, "建立模型时出错");
    if (result.ok() || !cancel.isCancelled()) {
        return result;
    }
    if (result.error().code == ErrorCode::Cancelled || result.error().code == ErrorCode::OutOfMemory) {
        return result;
    }
    return Error{ErrorCode::Cancelled, result.error().message, "已取消"};
}

const AssemblyNode& DocumentModel::node(NodeId id) const {
    if (id >= m_nodes.size()) {
        throw std::out_of_range("node id");
    }
    return m_nodes[id];
}

std::size_t DocumentModel::nodeCount() const noexcept {
    return m_nodes.size();
}

const PrototypeInfo& DocumentModel::prototype(PrototypeId id) const {
    if (id >= m_prototypes.size()) {
        throw std::out_of_range("prototype id");
    }
    return m_prototypes[id];
}

std::span<const PrototypeInfo> DocumentModel::prototypes() const noexcept {
    return m_prototypes;
}

std::span<const NodeId> DocumentModel::leafNodesOf(PrototypeId id) const {
    if (id >= m_leaves.size()) {
        throw std::out_of_range("prototype id");
    }
    return m_leaves[id];
}

const Bnd_Box& DocumentModel::worldBox() const noexcept {
    return m_worldBox;
}

double DocumentModel::lengthUnitToMm() const noexcept {
    return m_lengthUnitToMm;
}

TopoDS_Shape DocumentModel::worldShapeOf(NodeId id) const {
    const AssemblyNode& current = node(id);
    if (current.prototype != kNoPrototype) {
        return prototype(current.prototype).shape.Moved(current.worldLocation);
    }
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    if (!current.children.empty()) {
        for (NodeId child : current.children) {
            const TopoDS_Shape childShape = worldShapeOf(child);
            if (!childShape.IsNull()) {
                builder.Add(compound, childShape);
            }
        }
    } else {
        for (const PrototypeInfo& info : m_prototypes) {
            if (!info.shape.IsNull()) {
                builder.Add(compound, info.shape);
            }
        }
    }
    if (!current.worldLocation.IsIdentity()) {
        compound.Move(current.worldLocation);
    }
    return compound;
}

} // namespace steplens::model
