#include "ui/ToolbarIcons.h"

#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QVector>

#include <functional>

namespace steplens::ui {
namespace {

QIcon paintIcon(int size, const std::function<void(QPainter&, const QRectF&)>& draw) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box(1.5, 1.5, size - 3.0, size - 3.0);
    QPen pen(QColor(32, 36, 44), 1.6);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    draw(painter, box);
    return QIcon(pixmap);
}

} // namespace

QIcon toolbarIcon(ToolbarGlyph glyph, int size) {
    switch (glyph) {
    case ToolbarGlyph::Open:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRoundedRect(r.adjusted(1, 3, -1, -1), 2, 2);
            p.drawLine(QPointF(r.left() + 2, r.top() + 3), QPointF(r.center().x(), r.top() + 3));
            p.drawLine(QPointF(r.center().x(), r.top() + 3), QPointF(r.center().x() + 2, r.top() + 1));
            p.drawLine(QPointF(r.center().x() + 2, r.top() + 1), QPointF(r.right() - 1, r.top() + 1));
        });
    case ToolbarGlyph::Fit:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            const qreal m = r.width() * 0.22;
            p.drawRect(r.adjusted(m, m, -m, -m));
            p.drawLine(r.topLeft(), r.topLeft() + QPointF(m, 0));
            p.drawLine(r.topLeft(), r.topLeft() + QPointF(0, m));
            p.drawLine(r.topRight(), r.topRight() + QPointF(-m, 0));
            p.drawLine(r.topRight(), r.topRight() + QPointF(0, m));
            p.drawLine(r.bottomLeft(), r.bottomLeft() + QPointF(m, 0));
            p.drawLine(r.bottomLeft(), r.bottomLeft() + QPointF(0, -m));
            p.drawLine(r.bottomRight(), r.bottomRight() + QPointF(-m, 0));
            p.drawLine(r.bottomRight(), r.bottomRight() + QPointF(0, -m));
        });
    case ToolbarGlyph::ViewFront:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRect(r.adjusted(3, 3, -3, -3));
            p.drawText(r, Qt::AlignCenter, QStringLiteral("前"));
        });
    case ToolbarGlyph::ViewBack:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRect(r.adjusted(3, 3, -3, -3));
            p.drawText(r, Qt::AlignCenter, QStringLiteral("后"));
        });
    case ToolbarGlyph::ViewLeft:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRect(r.adjusted(3, 3, -3, -3));
            p.drawText(r, Qt::AlignCenter, QStringLiteral("左"));
        });
    case ToolbarGlyph::ViewRight:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRect(r.adjusted(3, 3, -3, -3));
            p.drawText(r, Qt::AlignCenter, QStringLiteral("右"));
        });
    case ToolbarGlyph::ViewTop:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRect(r.adjusted(3, 3, -3, -3));
            p.drawText(r, Qt::AlignCenter, QStringLiteral("上"));
        });
    case ToolbarGlyph::ViewBottom:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRect(r.adjusted(3, 3, -3, -3));
            p.drawText(r, Qt::AlignCenter, QStringLiteral("下"));
        });
    case ToolbarGlyph::ViewIso:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            const QPointF a(r.center().x(), r.top() + 2);
            const QPointF b(r.right() - 2, r.center().y());
            const QPointF c(r.center().x(), r.bottom() - 2);
            const QPointF d(r.left() + 2, r.center().y());
            p.drawPolygon(QPolygonF{QVector<QPointF>{a, b, c, d}});
            p.drawLine(a, c);
            p.drawLine(d, b);
        });
    case ToolbarGlyph::ShadeWithEdges:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.setBrush(QColor(180, 188, 198, 160));
            p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 2, 2);
            p.setBrush(Qt::NoBrush);
            p.drawLine(r.left() + 4, r.center().y(), r.right() - 4, r.center().y());
            p.drawLine(r.center().x(), r.top() + 4, r.center().x(), r.bottom() - 4);
        });
    case ToolbarGlyph::ShadeOnly:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.setBrush(QColor(180, 188, 198, 200));
            p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 2, 2);
        });
    case ToolbarGlyph::Wireframe:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 2, 2);
            p.drawLine(r.left() + 4, r.top() + 4, r.right() - 4, r.bottom() - 4);
            p.drawLine(r.left() + 4, r.bottom() - 4, r.right() - 4, r.top() + 4);
        });
    case ToolbarGlyph::Distance:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawEllipse(QRectF(r.left() + 2, r.center().y() - 2, 4, 4));
            p.drawEllipse(QRectF(r.right() - 6, r.center().y() - 2, 4, 4));
            p.drawLine(QPointF(r.left() + 6, r.center().y()), QPointF(r.right() - 6, r.center().y()));
        });
    case ToolbarGlyph::Angle:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            const QPointF o(r.left() + 4, r.bottom() - 4);
            p.drawLine(o, QPointF(r.right() - 3, r.bottom() - 4));
            p.drawLine(o, QPointF(r.left() + 4, r.top() + 3));
            p.drawArc(QRectF(o.x() - 6, o.y() - 6, 12, 12), 0 * 16, 90 * 16);
        });
    case ToolbarGlyph::ClearMeasure:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawEllipse(r.adjusted(2, 2, -2, -2));
            p.drawLine(r.topLeft() + QPointF(4, 4), r.bottomRight() - QPointF(4, 4));
        });
    case ToolbarGlyph::Screenshot:
        return paintIcon(size, [](QPainter& p, const QRectF& r) {
            p.drawRoundedRect(r.adjusted(1, 4, -1, -2), 2, 2);
            p.drawRect(QRectF(r.center().x() - 3, r.top() + 1, 6, 4));
            p.drawEllipse(QRectF(r.center().x() - 3.5, r.center().y() - 2.5, 7, 7));
        });
    }
    return {};
}

} // namespace steplens::ui
