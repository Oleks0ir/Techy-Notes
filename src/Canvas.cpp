#include "Canvas.h"
#include <QDateTime>
#include <QGuiApplication>
#include <QPainter>
#include <QTouchEvent>
#include <QtMath>

static const QColor kBackground(0x5a, 0x5a, 0x5a);

Canvas::Canvas(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setTabletTracking(true);   // pen hover events: palm rejection now, "+" buttons later
    settle.setSingleShot(true);
    settle.setInterval(150);   // re-render this long after the last pan/zoom step
    connect(&settle, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void Canvas::viewChanged() {
    settle.start();
    update();
}

void Canvas::zoomAt(QPointF a, double f) {
    f = qBound(0.1 / scale, f, 40 / scale);   // keep scale within 0.1..40 px/mm
    scale *= f;
    offset = a - (a - offset) * f;
    viewChanged();
}

bool Canvas::event(QEvent *e) {
    if (e->type() == QEvent::TouchCancel && touchDrawing) {
        end();
        touchDrawing = false;
    }
    if (e->type() != QEvent::TouchBegin && e->type() != QEvent::TouchUpdate && e->type() != QEvent::TouchEnd)
        return QWidget::event(e);
    // ponytail: palm rejection = ignore touch for 500 ms after any pen event; use proximity events if palms leak
    if (QDateTime::currentMSecsSinceEpoch() - lastPenMs < 500) return true;
    const auto *te = static_cast<QTouchEvent *>(e);
    const auto pts = te->points();
    if (fingerDraws && (touchDrawing || (e->type() == QEvent::TouchBegin && pts.size() == 1))) {
        if (pts.size() == 1 && !te->isEndEvent()) {
            if (!touchDrawing && active == None) begin(pts[0].position(), 1, te->timestamp(), tool);
            else if (touchDrawing) move(pts[0].position(), 1, te->timestamp());
            touchDrawing = active != None;
            return true;
        }
        // finger lifted, or a second finger turns the gesture into pan/zoom: keep what was drawn so far
        if (touchDrawing) end();
        touchDrawing = false;
        if (te->isEndEvent()) return true;
    }
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
    viewChanged();
    return true;   // accepted touch => Qt synthesizes no mouse events
}

void Canvas::renderCache() {
    qreal dpr = devicePixelRatioF();
    cache = QPixmap(size() * dpr);
    cache.setDevicePixelRatio(dpr);
    cache.fill(kBackground);
    cacheScale = scale;
    cacheOffset = offset;
    cacheDirty = false;
    QPainter p(&cache);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(offset);
    p.scale(scale, scale);
    p.setPen(QPen(QColor(0xb0, 0xb0, 0xb0), 0));   // cosmetic 1px page lines
    for (QPoint c : sheet.pages) {
        QRectF r = sheet.pageRect(c);
        p.fillRect(r, Qt::white);
        p.drawRect(r);
    }
    // ponytail: redraws every stroke, tile cache / spatial index when big sheets stutter
    for (const Stroke &s : sheet.strokes) drawStroke(p, s);
    if (active == Pen) drawStroke(p, cur);
}

void Canvas::paintIntoCache(const Stroke &s, int from) {
    if (cacheDirty) return update();   // full re-render will include it
    QPainter p(&cache);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(cacheOffset);
    p.scale(cacheScale, cacheScale);
    QPolygonF touched;
    for (int i = qMax(from - 1, 0); i < s.pts.size(); ++i) touched << s.pts[i].pos();
    for (int i = from; i < s.pts.size(); ++i) drawSegment(p, s, i);
    QRectF dirty = touched.boundingRect();
    double m = s.width / 2 + 1 / cacheScale;
    update(QRectF((dirty.topLeft() - QPointF(m, m)) * scale + offset, (dirty.bottomRight() + QPointF(m, m)) * scale + offset)
               .toAlignedRect());
}

void Canvas::paintEvent(QPaintEvent *) {
    bool moved = cacheScale != scale || cacheOffset != offset;
    if (cacheDirty || cache.deviceIndependentSize() != QSizeF(size()) || (moved && !settle.isActive())) {
        renderCache();
        moved = false;
    }
    QPainter p(this);
    if (moved) {   // mid-gesture: stretch the old cache instead of re-rendering every frame
        p.fillRect(rect(), kBackground);
        double f = scale / cacheScale;
        p.translate(offset - cacheOffset * f);
        p.scale(f, f);
    }
    p.drawPixmap(0, 0, cache);
    if (active == Line) {
        p.resetTransform();
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(offset);
        p.scale(scale, scale);
        drawStroke(p, cur);
    }
}

void Canvas::begin(QPointF s, float pr, qint64 t, Tool tl) {
    active = tl;
    before = sheet.strokes;
    changed = false;
    QPointF w = toWorld(s);
    Pt pt{float(w.x()), float(w.y()), pr, t};
    if (tl == Pen) {
        cur = {{pt}, color, width};
        paintIntoCache(cur, 0);
    } else if (tl == Line) {
        pt.p = 1;
        cur = {{pt, pt}, color, width};
    } else if (tl == Eraser) {
        erase(w);
    }
}

void Canvas::move(QPointF s, float pr, qint64 t) {
    QPointF w = toWorld(s);
    if (active == Pen) {
        cur.pts.append({float(w.x()), float(w.y()), pr, t});
        paintIntoCache(cur, cur.pts.size() - 1);
    } else if (active == Line) {
        if (snapLines || QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) w = snap15(cur.pts[0].pos(), w);
        cur.pts[1] = {float(w.x()), float(w.y()), 1, t};
        update();
    } else if (active == Eraser) {
        erase(w);
    }
}

void Canvas::end() {
    if (active == Pen || (active == Line && cur.pts[0].pos() != cur.pts[1].pos())) {
        sheet.strokes.append(cur);
        if (active == Line) paintIntoCache(cur, 0);
        changed = true;
    }
    active = None;
    update();
    if (!changed) return;
    undoStack.append(before);
    if (undoStack.size() > 100) undoStack.removeFirst();
    redoStack.clear();
}

void Canvas::erase(QPointF w) {
    // ponytail: point hit-test per event, fast swipes can skip thin strokes; test the swept segment if that bites
    if (sheet.strokes.removeIf([&](const Stroke &s) { return hits(s, w, 4 / scale); })) {
        changed = cacheDirty = true;
        update();
    }
}

void Canvas::undo() {
    if (undoStack.isEmpty() || active != None) return;
    redoStack.append(sheet.strokes);
    sheet.strokes = undoStack.takeLast();
    cacheDirty = true;
    update();
}

void Canvas::redo() {
    if (redoStack.isEmpty() || active != None) return;
    undoStack.append(sheet.strokes);
    sheet.strokes = redoStack.takeLast();
    cacheDirty = true;
    update();
}

void Canvas::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier) return zoomAt(e->position(), qPow(1.0015, e->angleDelta().y()));
    QPointF d = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 2 : QPointF(e->pixelDelta());
    if (e->modifiers() & Qt::ShiftModifier && d.x() == 0) d = {d.y(), 0};
    offset += d;
    viewChanged();
}

// Mouse events made up from touch input (by Qt or Windows): fingers are handled in event() only.
static bool fromTouch(const QMouseEvent *e) {
    return e->source() != Qt::MouseEventNotSynthesized || e->device()->type() != QInputDevice::DeviceType::Mouse;
}

void Canvas::mousePressEvent(QMouseEvent *e) {
    if (fromTouch(e)) return;
    lastMouse = e->position();
    if (e->button() == Qt::LeftButton && active == None) begin(e->position(), 1, e->timestamp(), tool);
}

void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (fromTouch(e)) return;
    if (e->buttons() & Qt::MiddleButton) {
        offset += e->position() - lastMouse;
        viewChanged();
    }
    lastMouse = e->position();
    if (active != None) move(e->position(), 1, e->timestamp());
}

void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (!fromTouch(e) && e->button() == Qt::LeftButton && active != None) end();
}

void Canvas::tabletEvent(QTabletEvent *e) {
    lastPenMs = QDateTime::currentMSecsSinceEpoch();
    e->accept();   // accepting stops synthesized mouse events
    if (e->type() == QEvent::TabletPress && active == None) {
        bool eraserEnd = e->pointerType() == QPointingDevice::PointerType::Eraser;
        // Calibration knob: which pen buttons count as "barrel" differs per device/driver.
        bool barrel = barrelErases && (e->buttons() & (Qt::RightButton | Qt::MiddleButton));
        begin(e->position(), e->pressure(), e->timestamp(), eraserEnd || barrel ? Eraser : tool);
    } else if (e->type() == QEvent::TabletMove && active != None) {
        move(e->position(), e->pressure(), e->timestamp());
    } else if (e->type() == QEvent::TabletRelease && active != None) {
        end();
    }
}
