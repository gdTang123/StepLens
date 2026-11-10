#pragma once

// T07 方案 B 单级弦高。公式是架构 5.5.1 的 clamp，系数用摸底里的方案 B：
// c = 0.002，c_g = 0.0003，角度 25°。粗（0.004）/ 细（0.001）是 T07+，这里不算。

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "mesh/MeshBatch.h"

#include <cstdint>

namespace steplens::mesh {

// 局部系数 × 原型包围盒对角线。
inline constexpr double kSchemeBLocalCoefficient = 0.002;
// 全局系数 × 整个模型包围盒对角线，当作上限。摸底在粗 0.0005 和细 0.0002 之间取的。
inline constexpr double kSchemeBGlobalCoefficient = 0.0003;
inline constexpr double kSchemeBAngleDegrees = 25.0;
// 下限，毫米。再换算成模型单位。
inline constexpr double kDeflectionMinMm = 0.005;
// OCCT IMeshTools_Parameters::RelMinSize()。
inline constexpr double kMinSizeToDeflection = 0.1;
// 面数超过这个数的原型单独跑，并打开 OCCT 内部按面并行。和摸底的 2000 一致。
inline constexpr std::uint32_t kLargePrototypeFaces = 2000;

struct MeshParams {
    double deflection = 0.0;
    double angleRad = 0.0;
    bool inParallel = false;
};

class MeshPolicy {
public:
    explicit MeshPolicy(std::uint32_t largeFaceCount = kLargePrototypeFaces) noexcept;

    [[nodiscard]] std::uint32_t largeFaceCount() const noexcept {
        return m_largeFaceCount;
    }

    // lengthUnitToMm 用 DocumentModel 的值。d_min 是 0.005 mm，要换成模型单位再和包围盒比较。
    [[nodiscard]] MeshParams paramsFor(const model::PrototypeInfo& prototype, const Bnd_Box& world,
                                       double lengthUnitToMm, MeshLevel level) const;

private:
    std::uint32_t m_largeFaceCount = kLargePrototypeFaces;
};

} // namespace steplens::mesh
