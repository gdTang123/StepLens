#include "ui/PropertyPanel.h"

#include <QCoreApplication>
#include <QLabel>
#include <QPointer>

#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <thread>

namespace steplens::ui {
namespace {

constexpr double kRelativeError = 0.001;

QString millimetersOf(const Bnd_Box& box, double lengthUnitToMm) {
    if (box.IsVoid() || !(lengthUnitToMm > 0.0) || !std::isfinite(lengthUnitToMm)) {
        return QStringLiteral("尚无值");
    }
    const gp_Pnt low = box.CornerMin();
    const gp_Pnt high = box.CornerMax();
    const double x = (high.X() - low.X()) * lengthUnitToMm;
    const double y = (high.Y() - low.Y()) * lengthUnitToMm;
    const double z = (high.Z() - low.Z()) * lengthUnitToMm;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return QStringLiteral("尚无值");
    }
    return QStringLiteral("%1 × %2 × %3 mm").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(z, 0, 'f', 3);
}

bool hasFace(const TopoDS_Shape& shape) {
    return TopExp_Explorer(shape, TopAbs_FACE).More();
}

bool hasClosedShell(const TopoDS_Shape& shape) {
    for (TopExp_Explorer explorer(shape, TopAbs_SHELL); explorer.More(); explorer.Next()) {
        if (BRep_Tool::IsClosed(explorer.Current())) {
            return true;
        }
    }
    return false;
}

QString scaledText(double modelValue, double unitScale, const QString& suffix) {
    const double shown = modelValue * unitScale;
    if (!std::isfinite(shown) || !(shown > 0.0)) {
        return QStringLiteral("不支持");
    }
    return QString::number(shown, 'g', 8) + QLatin1Char(' ') + suffix;
}

bool massUsable(double relativeError, double mass) {
    return std::isfinite(relativeError) && relativeError >= 0.0 && relativeError <= kRelativeError &&
           std::isfinite(mass) && mass > 0.0;
}

void computeMass(const TopoDS_Shape& shape, double lengthUnitToMm, QString* volume, QString* area) {
    *volume = QStringLiteral("不支持");
    *area = QStringLiteral("不支持");
    if (shape.IsNull() || !(lengthUnitToMm > 0.0) || !std::isfinite(lengthUnitToMm)) {
        return;
    }
    try {
        if (hasFace(shape)) {
            GProp_GProps props;
            const double error = BRepGProp::SurfaceProperties(shape, props, kRelativeError, Standard_True);
            if (massUsable(error, props.Mass())) {
                const double unit = lengthUnitToMm * lengthUnitToMm;
                *area = scaledText(props.Mass(), unit, QStringLiteral("mm²"));
            }
        }
        if (hasClosedShell(shape)) {
            GProp_GProps props;
            const double error =
                BRepGProp::VolumeProperties(shape, props, kRelativeError, Standard_True, Standard_True);
            if (massUsable(error, props.Mass())) {
                const double unit = lengthUnitToMm * lengthUnitToMm * lengthUnitToMm;
                *volume = scaledText(props.Mass(), unit, QStringLiteral("mm³"));
            }
        }
    } catch (const Standard_Failure&) {
        *volume = QStringLiteral("不支持");
        *area = QStringLiteral("不支持");
    }
}

} // namespace

PropertyPanel::PropertyPanel(QWidget* parent) : QDockWidget(tr("属性"), parent) {
    m_body = new QLabel(tr("未选择零件"), this);
    m_body->setWordWrap(true);
    m_body->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_body->setMargin(8);
    setWidget(m_body);
    setObjectName(QStringLiteral("propertyPanel"));
}

void PropertyPanel::publish() {
    m_body->setText(tr("名称：%1\n包围盒：%2\n体积：%3\n表面积：%4").arg(m_name, m_box, m_volume, m_area));
}

void PropertyPanel::finishMass(std::uint64_t generation, const QString& volume, const QString& area) {
    if (generation != m_generation) {
        return;
    }
    m_volume = volume;
    m_area = area;
    publish();
}

void PropertyPanel::clearNode() {
    ++m_generation;
    m_body->setText(tr("未选择零件"));
}

void PropertyPanel::showNode(const model::DocumentModel& document, model::NodeId id, const QString& name) {
    ++m_generation;
    m_name = name.isEmpty() ? tr("未命名") : name;
    m_box = QStringLiteral("尚无值");
    m_volume = QStringLiteral("不支持");
    m_area = QStringLiteral("不支持");
    TopoDS_Shape shape;
    if (id < document.nodeCount()) {
        m_box = millimetersOf(document.node(id).worldBox, document.lengthUnitToMm());
        try {
            shape = document.worldShapeOf(id);
        } catch (const Standard_Failure&) {
            shape.Nullify();
        } catch (const std::exception&) {
            shape.Nullify();
        }
    }
    if (shape.IsNull()) {
        publish();
        return;
    }
    m_volume = QStringLiteral("正在计算");
    m_area = QStringLiteral("正在计算");
    publish();
    const std::uint64_t generation = m_generation;
    const double unit = document.lengthUnitToMm();
    QPointer<PropertyPanel> self(this);
    std::thread([self, generation, shape, unit]() {
        QString volume;
        QString area;
        computeMass(shape, unit, &volume, &area);
        if (QCoreApplication::instance() == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, generation, volume, area]() {
                if (self != nullptr) {
                    self->finishMass(generation, volume, area);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace steplens::ui
