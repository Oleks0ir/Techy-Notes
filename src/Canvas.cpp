#include "Canvas.h"
#include <QDateTime>
#include <QPainter>
#include <QTouchEvent>
#include <QtMath>

Canvas::Canvas(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setTabletTracking(true);   // pen hover events: palm rejection now, "+" buttons later
}

void Canvas::zoomAt(QPointF a, double f) {
    f = qBound(0.1 / scale, f, 40 / scale);   // keep scale within 0.1..40 px/mm
    scale *= f;
    offset = a - (a - offset) * f;
    update();
}

bool Canvas::event(QEvent *e) {
    if (e->type() != QEvent::TouchBegin && e->type() != QEvent::TouchUpdate && e->type() != QEvent::TouchEnd)
        return QWidget::event(e);
    // ponytail: palm rejection = ignore touch for 500 ms after any pen event; use proximity events if palms leak
    if (QDateTime::currentMSecsSinceEpoch() - lastPenMs < 500) return true;
    const auto pts = static_cast<QTouchEvent *>(e)->points();
    QPointF c, lc;
    for (const auto &p : pts) { c += p.position(); lc += p.lastPosition(); }
    c /= pts.size();
    lc /= pts.size();
    offset += c - lc;
    if (pts.size() >= 2) {
        double d = QLineF(pts[0].position(), pts[1].position()).length();
        double ld = QLineF(pts[0].lastPosition(), pts[1].lastPosition()).length();
        if (ld > 0) zoomAt(c, d / ld);
    }
    update();
    return true;   // accepted touch => Qt synthesizes no mouse events
}

void Canvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x5a, 0x5a, 0x5a));
    p.translate(offset);
    p.scale(scale, scale);
    p.setPen(QPen(QColor(0xb0, 0xb0, 0xb0), 0));   // cosmetic 1px page lines
    for (QPoint c : sheet.pages) {
        QRectF r = sheet.pageRect(c);
        p.fillRect(r, Qt::white);
        p.drawRect(r);
    }
}

void Canvas::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier) return zoomAt(e->position(), qPow(1.0015, e->angleDelta().y()));
    QPointF d = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 2 : QPointF(e->pixelDelta());
    if (e->modifiers() & Qt::ShiftModifier && d.x() == 0) d = {d.y(), 0};
    offset += d;
    update();
}

void Canvas::mousePressEvent(QMouseEvent *e) { lastMouse = e->position(); }

void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (e->buttons() & Qt::MiddleButton) {
        offset += e->position() - lastMouse;
        update();
    }
    lastMouse = e->position();
}

void Canvas::tabletEvent(QTabletEvent *e) {
    lastPenMs = QDateTime::currentMSecsSinceEpoch();
    e->accept();   // pen draws in milestone 2; accepting stops synthesized mouse events
}
