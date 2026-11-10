#include "mesh/MeshPolicy.h"

#include <Precision.hxx>

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace steplens::mesh {
namespace {

[[nodiscard]] double diagonalOf(const Bnd_Box& box) {
    if (box.IsVoid()) {
        return 0.0;
    }
    const double square = box.SquareExtent();
    if (!(square > 0.0) || !std::isfinite(square)) {
        return 0.0;
    }
    return std::sqrt(square);
}

[[nodiscard]] double schemeBAngleRad() {
    constexpr double kHalfCircleDegrees = 180.0;
    return kSchemeBAngleDegrees * std::numbers::pi_v<double> / kHalfCircleDegrees;
}

// 先套全局上限，再抬到 d_min。模型比 d_min 还小时下限胜出，避免弦高变成 0。
[[nodiscard]] double schemeBDeflection(double localDiagonal, double worldDiagonal, double lengthUnitToMm) {
    double unit = lengthUnitToMm;
    if (!(unit > 0.0) || !std::isfinite(unit)) {
        unit = 1.0;
    }
    const double minimum = kDeflectionMinMm / unit;
    double deflection = kSchemeBLocalCoefficient * localDiagonal;
    if (worldDiagonal > 0.0) {
        const double cap = kSchemeBGlobalCoefficient * worldDiagonal;
        if (deflection > cap) {
            deflection = cap;
        }
    }
    if (!(deflection > minimum)) {
        deflection = minimum;
    }
    // BRepMesh 拒绝小于 Precision::Confusion 的弦高。极大的 lengthUnitToMm 会把 0.005 mm 缩没。
    const double floor = Precision::Confusion() * 2.0;
    if (!(deflection > floor)) {
        deflection = floor;
    }
    return deflection;
}

[[noreturn]] void unhandledMeshLevel(MeshLevel level) {
    switch (level) {
    case MeshLevel::SchemeB:
        break;
    }
    throw std::logic_error("unhandled mesh level");
}

} // namespace

MeshPolicy::MeshPolicy(std::uint32_t largeFaceCount) noexcept : m_largeFaceCount(largeFaceCount) {}

MeshParams MeshPolicy::paramsFor(const model::PrototypeInfo& prototype, const Bnd_Box& world,
                                 double lengthUnitToMm, MeshLevel level) const {
    switch (level) {
    case MeshLevel::SchemeB: {
        MeshParams params;
        params.deflection =
            schemeBDeflection(diagonalOf(prototype.localBox), diagonalOf(world), lengthUnitToMm);
        params.angleRad = schemeBAngleRad();
        params.inParallel = prototype.faceCount > m_largeFaceCount;
        return params;
    }
    default:
        unhandledMeshLevel(level);
    }
}

} // namespace steplens::mesh
