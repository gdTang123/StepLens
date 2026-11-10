#pragma once

// T07 显示数据包。工作线程填好，以后的 GUI 线程只消费。这里没有 AIS 对象。
// 单级方案 B（c = 0.002）。粗/细两级是 T07+，不在这个类型里。

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "model/DocumentModel.h"

#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>

#include <cstdint>
#include <vector>

namespace steplens::mesh {

// T07 只有方案 B 这一级。Coarse / Fine 留给 T07+，现在加进去会让调用方以为能做两级。
enum class MeshLevel : std::uint8_t { SchemeB };

struct ColorGroupArrays {
    Quantity_ColorRGBA color;
    opencascade::handle<Graphic3d_ArrayOfTriangles> triangles;
    opencascade::handle<Graphic3d_ArrayOfSegments> edges;
};

struct MeshBatch {
    model::PrototypeId id = 0;
    MeshLevel level = MeshLevel::SchemeB;
    std::vector<ColorGroupArrays> groups;
    // 这个原型的三角网格数，不乘实例数。实例共用这一份。
    std::uint64_t triangleCount = 0;
    std::uint64_t approxBytes = 0;
};

// 一次 run 的合计。triangleCount 是原型网格（每份只算一次）。
// instanceTriangleCount 按实例展开，对应真正要画的量。
// meshCalls 是 BRepMesh_IncrementalMesh 的构造次数，成功时应等于原型数。
struct MeshStats {
    std::uint64_t triangleCount = 0;
    std::uint64_t instanceTriangleCount = 0;
    std::uint64_t approxBytes = 0;
    std::uint32_t prototypesMeshed = 0;
    std::uint32_t meshCalls = 0;
    std::uint32_t facesWithoutTriangles = 0;
};

} // namespace steplens::mesh
