#include "Canvas.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QGuiApplication>
#include <QPainter>
#include <QTouchEvent>
#include <QtMath>
#include <algorithm>

static const QColor kBackground(0x5a, 0x5a, 0x5a);

Canvas::Canvas(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setTabletTracking(true);   // pen hover: palm rejection, "+" buttons
    setMouseTracking(true);    // mouse hover: "+" buttons
    setFocusPolicy(Qt::ClickFocus);
    settle.setSingleShot(true);
    settle.setInterval(150);   // re-render this long after the last pan/zoom step
    connect(&settle, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    hold.setSingleShot(true);
    hold.setInterval(500);
    connect(&hold, &QTimer::timeout, this, [this] {   // armed: ring shown, the menu opens when the finger lifts
        if (touchDrawing) cancel();   // finger drawing: drop what the press started
        touchDrawing = false;
        holdFired = true;
        update();
    });
    autosave.setSingleShot(true);
    autosave.setInterval(1000);
    connect(&autosave, &QTimer::timeout, this, &Canvas::save);
    setEnabled(false);         // until a sheet is open
}

void Canvas::modified() {
    refresh(sheet, autoPages);
    dirty = true;
    autosave.start();
}

void Canvas::save() {
    autosave.stop();
    if (!dirty || path.isEmpty()) return;
    dirty = !saveSheet(sheet, path);
    if (dirty) QMessageBox::warning(this, "Techy-Draw", "Could not save " + path);
}

bool Canvas::open(const QString &file) {
    save();
    if (dirty || active != None) return false;   // never drop unsaved ink
    Sheet s;
    bool ok = loadSheet(s, file);
    sheet = s;
    path = ok ? file : QString();
    undoStack.clear();
    redoStack.clear();
    sel.clear();
    cacheDirty = true;
    setEnabled(ok);
    update();
    return ok;
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
    if (e->type() == QEvent::Leave && plusOn) {
        plusOn = false;
        update();
    }
    if (e->type() == QEvent::TouchCancel) {
        hold.stop();
        holdFired = false;
        if (touchDrawing) end();
        touchDrawing = false;
    }
    if (e->type() != QEvent::TouchBegin && e->type() != QEvent::TouchUpdate && e->type() != QEvent::TouchEnd)
        return QWidget::event(e);
    if (QApplication::activePopupWidget()) {   // a menu is open: not ours, Qt turns it into a mouse click on the menu
        e->ignore();
        return false;
    }
    // ponytail: palm rejection = ignore touch for 500 ms after any pen event; use proximity events if palms leak
    if (QDateTime::currentMSecsSinceEpoch() - lastPenMs < 500) return true;
    const auto *te = static_cast<QTouchEvent *>(e);
    const auto pts = te->points();
    // one finger held still for 0.5 s, then lifted, opens the page menu. Not while held: once a menu is open,
    // Qt turns the held finger into mouse events, and its lift closed the menu right away.
    if (e->type() == QEvent::TouchBegin && pts.size() == 1) {
        holdAt = pts[0].position();
        holdFired = false;
        hold.start();
    } else if (te->isEndEvent() || pts.size() != 1 || QLineF(pts[0].position(), holdAt).length() > 10) {
        hold.stop();
        if (holdFired && te->isEndEvent() && pts.size() == 1 && QLineF(pts[0].position(), holdAt).length() <= 10)
            QTimer::singleShot(0, this, [this] { pageMenu(holdAt); });   // after the lift is fully handled
        if (holdFired) update();
        holdFired = false;
    }
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
    cachePages = sheet.pages.size();
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
    for (int i = 0; i < sheet.strokes.size(); ++i)
        if (!sel.contains(i)) drawStroke(p, sheet.strokes[i]);
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
    if (cacheDirty || cachePages != sheet.pages.size() || cache.deviceIndependentSize() != QSizeF(size()) || (moved && !settle.isActive())) {
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
    p.resetTransform();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(offset);
    p.scale(scale, scale);
    QPoint ghost = focused() + QPoint(1, 0);   // the page right of the focused one; ink on it makes it real
    if (autoPages && sheet.pages.contains(focused()) && !sheet.pages.contains(ghost)) {
        p.setPen(QPen(QColor(0xd0, 0xd0, 0xd0), 0, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 40));
        p.drawRect(sheet.pageRect(ghost));
    }
    if (active == Line) drawStroke(p, cur);
    if (!sel.isEmpty()) {
        p.save();
        p.translate(dragBy);
        for (int i : sel) drawStroke(p, sheet.strokes[i]);
        p.restore();
        p.setPen(QPen(QColor(0x1565c0), 0, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(selBounds().translated(dragBy));
    }
    if ((active == Lasso || active == Rect) && !moving && !lasso.isEmpty()) {
        p.setPen(QPen(Qt::darkGray, 0, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        if (active == Rect) p.drawRect(QRectF(lasso.first(), lasso.last()).normalized());
        else p.drawPolyline(lasso);
    }
    p.resetTransform();
    if (holdFired) {   // finger long-press armed: lift for the page menu
        p.setPen(QPen(QColor(0x1565c0), 3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(holdAt, 24, 24);
    }
    if (plusOn) {   // fixed screen size
        QPointF c = plusAt * scale + offset;
        p.setPen(QPen(QColor(0x1565c0), 2));
        p.setBrush(Qt::white);
        p.drawEllipse(c, 12, 12);
        p.drawLine(c - QPointF(6, 0), c + QPointF(6, 0));
        p.drawLine(c - QPointF(0, 6), c + QPointF(0, 6));
    }
}

void Canvas::select(const QSet<int> &s) {
    if (s.isEmpty() && sel.isEmpty()) return;
    sel = s;
    cacheDirty = true;   // selected strokes leave/rejoin the cache
    update();
}

QRectF Canvas::selBounds() const {
    QRectF b;
    for (int i : sel) b |= bounds(sheet.strokes[i]);
    return b;
}

void Canvas::begin(QPointF s, float pr, qint64 t, Tool tl) {
    active = tl;
    plusOn = false;
    setFocus();   // Ctrl+Z now undoes ink, not a deletion in the notebook tree
    before = sheet.strokes;
    changed = false;
    QPointF w = toWorld(s);
    Pt pt{float(w.x()), float(w.y()), pr, t};
    if (tl == Lasso || tl == Rect) {
        adding = QGuiApplication::keyboardModifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
        moving = !adding && !sel.isEmpty() && selBounds().contains(w);
        dragFrom = w;
        dragBy = {};
        if (!moving) {
            if (!adding) select({});
            lasso = {w};
        }
        return;
    }
    select({});
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
    } else if (active == Lasso || active == Rect) {
        if (moving) dragBy = w - dragFrom;
        else lasso << w;
        update();
    }
}

void Canvas::end() {
    if (active == Pen || (active == Line && cur.pts[0].pos() != cur.pts[1].pos())) {
        sheet.strokes.append(cur);
        if (autoPages) sheet.pages |= inkCells(sheet, cur, true);   // writing near a page bottom adds the page below
        if (active == Line) paintIntoCache(cur, 0);
        changed = true;
    } else if (active == Lasso || active == Rect) {
        if (moving && !dragBy.isNull()) {
            for (int i : sel)
                for (Pt &p : sheet.strokes[i].pts) { p.x += dragBy.x(); p.y += dragBy.y(); }
            changed = true;
        } else if (!moving) {
            QSet<int> in = strokesInside(sheet.strokes, active == Rect ? QPolygonF(QRectF(lasso.first(), lasso.last()).normalized())
                                                                         : lasso);
            select(adding ? in | sel : in);
        }
        moving = false;
        dragBy = {};
        lasso.clear();
    }
    active = None;
    update();
    if (changed) pushUndo(before);
}

void Canvas::cancel() {
    if (active == None) return;
    sheet.strokes = before;   // drops the live stroke, restores erased strokes and moved selections
    active = None;
    moving = false;
    dragBy = {};
    lasso.clear();
    cacheDirty = true;
    update();
}

void Canvas::pushUndo(const QList<Stroke> &prev) {
    undoStack.append(prev);
    if (undoStack.size() > 100) undoStack.removeFirst();
    redoStack.clear();
    modified();
}

void Canvas::setColor(QColor c) {
    color = c;
    if (sel.isEmpty() || active != None) return;
    QList<Stroke> prev = sheet.strokes;
    for (int i : sel) sheet.strokes[i].color = c;
    pushUndo(prev);
    update();
}

static const char *kStrokesMime = "application/x-techy-draw-strokes";

void Canvas::copySelection() {
    if (sel.isEmpty()) return;
    QList<Stroke> out;
    for (int i : sel) out << sheet.strokes[i];
    QByteArray bytes;
    QDataStream(&bytes, QIODevice::WriteOnly) << qint32(1) << out;   // clipboard format version 1
    auto md = new QMimeData;
    md->setData(kStrokesMime, bytes);
    QGuiApplication::clipboard()->setMimeData(md);
}

void Canvas::deleteSelection() {
    if (sel.isEmpty() || active != None) return;
    QList<Stroke> prev = sheet.strokes;
    QList<int> idx = sel.values();
    std::sort(idx.rbegin(), idx.rend());
    for (int i : idx) sheet.strokes.removeAt(i);
    pushUndo(prev);
    select({});
}

void Canvas::paste() {
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md || active != None) return;
    QDataStream d(md->data(kStrokesMime));
    qint32 version = 0;
    QList<Stroke> in;
    d >> version >> in;
    if (d.status() != QDataStream::Ok || version != 1 || in.isEmpty()) return;
    QRectF b;
    for (const Stroke &s : in) b |= bounds(s);
    QPointF by = toWorld(rect().center()) - b.center();   // paste centered in view
    QList<Stroke> prev = sheet.strokes;
    QSet<int> added;
    for (Stroke &s : in) {
        for (Pt &p : s.pts) { p.x += by.x(); p.y += by.y(); }
        added.insert(sheet.strokes.size());
        sheet.strokes << s;
    }
    pushUndo(prev);
    select(added);
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
    modified();
    sel.clear();
    cacheDirty = true;
    update();
}

void Canvas::redo() {
    if (redoStack.isEmpty() || active != None) return;
    undoStack.append(sheet.strokes);
    sheet.strokes = redoStack.takeLast();
    modified();
    sel.clear();
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
    if (e->button() == Qt::RightButton && active == None) pageMenu(e->position());
    if (e->button() == Qt::LeftButton && active == None && !pressPlus(e->position())) begin(e->position(), 1, e->timestamp(), tool);
}

void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (fromTouch(e)) return;
    if (e->buttons() & Qt::MiddleButton) {
        offset += e->position() - lastMouse;
        viewChanged();
    }
    lastMouse = e->position();
    if (active != None) move(e->position(), 1, e->timestamp());
    else if (!e->buttons()) hover(e->position());
}

void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (!fromTouch(e) && e->button() == Qt::LeftButton && active != None) end();
}

void Canvas::tabletEvent(QTabletEvent *e) {
    lastPenMs = QDateTime::currentMSecsSinceEpoch();
    // Right-click button: one menu per hold, like the notebook tree; it cancels a stroke in progress.
    // Held-button events stay ours, else Qt turns them into mouse events that close the menu again.
    if (e->buttons() & menuButtons) {
        if (!menuHeld && (e->type() == QEvent::TabletPress || active != None)) {
            menuHeld = true;
            cancel();
            pageMenu(e->position());
        }
        return e->accept();   // hovering with the button held opens nothing; touching down then does
    }
    menuHeld = false;
    if (QApplication::activePopupWidget()) return e->ignore();   // a menu is open: Qt turns the pen into a mouse click on it
    e->accept();   // accepting stops synthesized mouse events
    float pr = qBound(0.f, float((e->pressure() - pressLow) / (pressHigh - pressLow)), 1.f);
    if (e->type() == QEvent::TabletPress && active == None && !pressPlus(e->position())) {
        bool eraser = e->pointerType() == QPointingDevice::PointerType::Eraser || (e->buttons() & eraserButtons);
        begin(e->position(), pr, e->timestamp(), eraser ? Eraser : tool);
    } else if (e->type() == QEvent::TabletMove && active == None && !e->buttons()) {
        hover(e->position());
    } else if (e->type() == QEvent::TabletMove && active != None) {
        move(e->position(), pr, e->timestamp());
    } else if (e->type() == QEvent::TabletRelease && active != None) {
        end();
    }
}

// ponytail: "+" needs hover (pen or mouse); fingers have none, add a long-press on an edge if touch users need it
void Canvas::hover(QPointF s) {
    if (plusOn && QLineF(s, plusAt * scale + offset).length() <= 16) return;   // keep it while over it
    QPointF w = toWorld(s);
    QPoint c = sheet.cellAt(w);
    QRectF r = sheet.pageRect(c);
    std::pair<double, QPoint> edges[] = {{w.x() - r.left(), {-1, 0}}, {r.right() - w.x(), {1, 0}},
                                         {w.y() - r.top(), {0, -1}}, {r.bottom() - w.y(), {0, 1}}};
    auto e = *std::min_element(std::begin(edges), std::end(edges), [](auto &a, auto &b) { return a.first < b.first; });
    bool on = sheet.pages.contains(c) && e.first * scale < 40 && !sheet.pages.contains(c + e.second);
    QPointF at = r.center() + QPointF(e.second.x() * r.width(), e.second.y() * r.height()) / 2;
    if (on == plusOn && (!on || at == plusAt)) return;
    plusOn = on;
    plusCell = c + e.second;
    plusAt = at;
    update();
}

bool Canvas::pressPlus(QPointF s) {
    if (!plusOn || QLineF(s, plusAt * scale + offset).length() > 16) return false;
    sheet.pages.insert(plusCell);
    plusOn = false;
    modified();
    update();
    return true;
}

bool Canvas::deletePage(QPoint c) {
    if (active != None || !canDeletePage(sheet, c)) return false;
    sheet.pages.remove(c);
    modified();
    update();
    return true;
}

void Canvas::clearPage(QPoint c) {
    if (active != None) return;
    QList<Stroke> prev = sheet.strokes;
    // ponytail: removes whole strokes, also their parts on other pages; split strokes at the page edge if that bites
    if (!sheet.strokes.removeIf([&](const Stroke &s) { return inkCells(sheet, s).contains(c); })) return;
    sel.clear();
    cacheDirty = true;
    pushUndo(prev);
    update();
}

void Canvas::pageMenu(QPointF s) {
    QPoint c = sheet.cellAt(toWorld(s));
    if (menuOpen || (sinceMenu.isValid() && sinceMenu.elapsed() < 500) || !sheet.pages.contains(c)) return;
    menuOpen = true;
    plusOn = false;
    QMenu m;
    m.addAction("Clear page", [&] { clearPage(c); })->setEnabled(sheet.exportQueue.contains(c));   // queue = pages with ink
    m.addAction("Delete page", [&] { deletePage(c); })->setEnabled(canDeletePage(sheet, c));
    m.exec(mapToGlobal(s.toPoint()));
    menuOpen = false;
    sinceMenu.start();
}

bool Canvas::setPageSize(QSizeF mm) {
    if (!sheet.strokes.isEmpty() || active != None) return false;
    sheet.page = mm;
    cacheDirty = true;
    modified();
    update();
    return true;
}

void Canvas::setExportQueue(const QList<QPoint> &q) {
    sheet.exportQueue = q;
    modified();
}
