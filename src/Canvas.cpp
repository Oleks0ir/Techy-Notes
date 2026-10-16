#include "Canvas.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDropEvent>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QTouchEvent>
#include <QtMath>
#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <QAbstractNativeEventFilter>
#include <windows.h>
// Qt reports the pen's side button only while the tip touches; Windows has it while hovering too (WM_POINTER).
// Windows keeps it set after the button is let go, until the next tap.
// ponytail: Windows only; Android/iOS need their own hover-button source (or another ring trigger)
struct BarrelFilter : QAbstractNativeEventFilter {
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
        auto *m = static_cast<MSG *>(message);
        if (m->message != WM_POINTERUPDATE && m->message != WM_POINTERDOWN && m->message != WM_POINTERUP) return false;
        POINTER_INPUT_TYPE t;
        POINTER_PEN_INFO pi;
        UINT32 id = GET_POINTERID_WPARAM(m->wParam);
        if (GetPointerType(id, &t) && t == PT_PEN && GetPointerPenInfo(id, &pi)) Canvas::penBarrel = pi.penFlags & PEN_FLAG_BARREL;
        return false;   // Qt handles it as usual
    }
};
#endif

static const QColor kBackground(0x5a, 0x5a, 0x5a);
static constexpr float kHighlightWidth = 5;    // mm. Calibration knob, like the alpha below
static constexpr int kHighlightAlpha = 100;

Canvas::Canvas(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setTabletTracking(true);   // pen hover: palm rejection, "+" buttons
    setMouseTracking(true);    // mouse hover: "+" buttons
    setFocusPolicy(Qt::ClickFocus);
    setAcceptDrops(true);      // image files and images
#ifdef Q_OS_WIN
    static BarrelFilter barrel;
    qApp->installNativeEventFilter(&barrel);   // installing twice is harmless
#endif
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
    trimIdx = cropIdx = -1;   // any edit (undo too) may shift the indices
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
    trimIdx = cropIdx = -1;
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
    // ponytail: checks every stroke, tile cache / spatial index when big sheets stutter
    QList<Stroke> shown;
    for (int i = 0; i < sheet.strokes.size(); ++i)
        if (!sel.contains(i)) shown << sheet.strokes[i];
    drawStrokes(p, shown, QRectF(toWorld({0, 0}), toWorld(QPointF(size().width(), size().height()))));
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
    if (active == Line || active == Highlighter) drawStroke(p, cur);
    if (!sel.isEmpty()) {
        QList<Stroke> picked;
        for (int i : sel) picked << sheet.strokes[i];
        p.save();
        p.setTransform(selXf, true);
        drawStrokes(p, picked, {});
        p.restore();
        p.setPen(QPen(QColor(0x1565c0), 0, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(selXf.mapRect(selBounds()));
    }
    if (trimIdx >= 0) {   // armed: the picture to trim
        const Stroke &st = sheet.strokes[trimIdx];
        p.setPen(QPen(QColor(0x1565c0), 3 / scale));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(st.pts[0].pos(), st.pts[1].pos()));
    }
    if (cropIdx >= 0) {   // crop frame; the part cut away is dimmed
        const Stroke &st = sheet.strokes[cropIdx];
        QPainterPath away, keep;
        away.addRect(QRectF(st.pts[0].pos(), st.pts[1].pos()).normalized());
        keep.addRect(cropRect);
        p.fillPath(away.subtracted(keep), QColor(0, 0, 0, 110));
        p.setPen(QPen(QColor(0x1565c0), 3 / scale));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cropRect);
    }
    if ((active == Lasso || active == Rect || active == Snip || active == Trim) && !moving && !lasso.isEmpty()) {
        p.setPen(QPen(Qt::darkGray, 0, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        if (active == Lasso) p.drawPolyline(lasso);
        else p.drawRect(QRectF(lasso.first(), lasso.last()).normalized());
    }
    p.resetTransform();
    auto handle = [&](QPointF world) {   // fixed screen size
        p.setPen(QPen(QColor(0x1565c0), 2));
        p.setBrush(Qt::white);
        p.drawRect(QRectF(world * scale + offset - QPointF(7, 7), QSizeF(14, 14)));
    };
    if (!sel.isEmpty()) handle(selXf.map(selBounds().bottomRight()));   // resize
    if (cropIdx >= 0) {
        handle(cropRect.topLeft());
        handle(cropRect.bottomRight());
    }
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
    if (ringOpen) paintRing(p);
}

void Canvas::paintRing(QPainter &p) {
    QFont iconFont("Segoe Fluent Icons");   // Windows 11; Windows 10 has the same codepoints in Segoe MDL2 Assets
    if (!QFontMetrics(iconFont).inFont(QChar(0xE76D))) iconFont = QFont("Segoe MDL2 Assets");
    iconFont.setPixelSize(22);
    QFont small = font();
    small.setPixelSize(11);
    QPainterPath lit;
    for (int i = 0; i < ring.size(); ++i) {
        auto [a, span] = ringArc(i);
        bool inner = i < ringInner, hot = i == ringHot;
        double ri = kRing[inner ? 0 : 1], ro = kRing[inner ? 1 : 2];
        QRectF outer(ringAt - QPointF(ro, ro), QSizeF(2 * ro, 2 * ro)), in(ringAt - QPointF(ri, ri), QSizeF(2 * ri, 2 * ri));
        QPainterPath seg;
        seg.arcMoveTo(outer, a);
        seg.arcTo(outer, a, span);
        seg.arcTo(in, a + span, -span);
        seg.closeSubpath();
        const MenuItem &it = ring[i];
        bool swatch = it.color.isValid();
        if (hot) lit = seg;
        p.setPen(QPen(QColor(0x90, 0x90, 0x90), 1));
        p.setBrush(swatch ? it.color : hot ? QColor(0x1565c0) : QColor(250, 250, 250, 235));
        p.drawPath(seg);
        if (swatch) continue;   // lit: thick outline, drawn last
        double mid = qDegreesToRadians(a + span / 2), rm = (ri + ro) / 2;
        QPointF c = ringAt + QPointF(qCos(mid), -qSin(mid)) * rm;
        p.setPen(hot ? Qt::white : Qt::black);
        bool glyph = !it.icon.isNull() && QFontMetrics(iconFont).inFont(it.icon);
        p.setFont(glyph ? iconFont : small);   // icon only; the lit item's name shows in the centre
        p.drawText(QRectF(c - QPointF(40, 15), QSizeF(80, 30)), Qt::AlignCenter, glyph ? QString(it.icon) : it.label);
    }
    if (ringHot >= ringInner) {
        p.setPen(QPen(QColor(0x1565c0), 5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(lit);
    }
    if (ringHot >= 0 && ringHot < ringInner) {
        QRectF r(ringAt + QPointF(-kRing[0], 10), QSizeF(2 * kRing[0], 18));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));   // readable on white pages and the grey desk
        p.drawRoundedRect(r, 4, 4);
        p.setFont(small);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, ring[ringHot].label);
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
    QPointF w = toWorld(s);
    bool onHandle = !sel.isEmpty() && QLineF(s, selBounds().bottomRight() * scale + offset).length() < 20;
    bool grab = onHandle || (!sel.isEmpty() && selBounds().contains(w));
    if (grab && (tl == Pen || tl == Highlighter || tl == Line)) tl = Lasso;   // drawing tools move/resize a selection too
    if (trimIdx >= 0) tl = Trim;   // picture menu > Trim: the next drag, whatever the tool
    if (cropIdx >= 0) tl = Crop;   // picture menu > Crop: every drag, until a tap outside the frame
    active = tl;
    plusOn = false;
    setFocus();   // Ctrl+Z now undoes ink, not a deletion in the notebook tree
    before = sheet.strokes;
    changed = false;
    Pt pt{float(w.x()), float(w.y()), pr, t};
    if (tl == Crop) {   // corner handles size the frame, inside moves it, outside applies the crop
        auto atHandle = [&](QPointF c) { return QLineF(s, c * scale + offset).length() < 20; };
        cropGrab = atHandle(cropRect.topLeft()) ? 1 : atHandle(cropRect.bottomRight()) ? 2 : cropRect.contains(w) ? 3 : 0;
        dragFrom = w;
        cropFrom = cropRect;
        if (!cropGrab) {
            changed = cropPicture(cropIdx, cropRect);
            cropIdx = -1;
            update();
        }
        return;
    }
    if (tl == Snip || tl == Trim) {
        lasso = {w};
        return;
    }
    if (tl == Lasso || tl == Rect) {
        adding = QGuiApplication::keyboardModifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
        moving = !adding && grab;
        resizing = moving && onHandle;
        dragFrom = w;
        selXf = {};
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
    } else if (tl == Highlighter) {   // drawn live on top, under the ink once done
        QColor h = color;
        h.setAlpha(kHighlightAlpha);
        pt.p = 1;
        cur = {{pt}, h, kHighlightWidth};
        update();
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
    } else if (active == Highlighter) {
        cur.pts.append({float(w.x()), float(w.y()), 1, t});
        update();
    } else if (active == Line) {
        if (snapLines || QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) w = snap15(cur.pts[0].pos(), w);
        cur.pts[1] = {float(w.x()), float(w.y()), 1, t};
        update();
    } else if (active == Eraser) {
        erase(w);
    } else if (active == Crop && cropGrab) {
        QRectF r = cropFrom;
        QPointF d = w - dragFrom;
        const Stroke &st = sheet.strokes[cropIdx];
        QRectF box = QRectF(st.pts[0].pos(), st.pts[1].pos()).normalized();
        if (cropGrab == 1) r.setTopLeft(QPointF(qMin(r.left() + d.x(), r.right() - 1), qMin(r.top() + d.y(), r.bottom() - 1)));
        else if (cropGrab == 2) r.setBottomRight(QPointF(qMax(r.right() + d.x(), r.left() + 1), qMax(r.bottom() + d.y(), r.top() + 1)));
        else r.moveTo(qBound(box.left(), r.left() + d.x(), box.right() - r.width()), qBound(box.top(), r.top() + d.y(), box.bottom() - r.height()));
        cropRect = r & box;
        update();
    } else if (active == Lasso || active == Rect || active == Snip || active == Trim) {
        if (resizing) {   // uniform scale about the top-left corner, following the handle along the diagonal
            QPointF tl = selBounds().topLeft(), a = dragFrom - tl;
            double f = qMax(0.05, QPointF::dotProduct(w - tl, a) / qMax(QPointF::dotProduct(a, a), 1e-6));
            selXf = QTransform().translate(tl.x(), tl.y()).scale(f, f).translate(-tl.x(), -tl.y());
        } else if (moving) {
            selXf = QTransform::fromTranslate(w.x() - dragFrom.x(), w.y() - dragFrom.y());
        } else {
            lasso << w;
        }
        update();
    }
}

void Canvas::end() {
    if (active == Pen || active == Highlighter || (active == Line && cur.pts[0].pos() != cur.pts[1].pos())) {
        sheet.strokes.append(cur);
        if (autoPages) sheet.pages |= inkCells(sheet, cur, true);   // writing near a page bottom adds the page below
        if (active == Line) paintIntoCache(cur, 0);
        if (active == Highlighter) cacheDirty = true;   // goes under the ink
        changed = true;
    } else if (active == Snip) {
        QRectF r = QRectF(lasso.first(), lasso.last()).normalized();
        if (!r.isEmpty()) QGuiApplication::clipboard()->setImage(snapshot(sheet.strokes, r));
        lasso.clear();
    } else if (active == Trim) {   // crop the picture to the dragged rectangle; a tap or a miss leaves it
        changed = cropPicture(trimIdx, QRectF(lasso.first(), lasso.last()).normalized());
        trimIdx = -1;
        lasso.clear();
    } else if (active == Lasso || active == Rect) {
        if (moving && !selXf.isIdentity()) {
            for (int i : sel) {
                Stroke &st = sheet.strokes[i];
                for (Pt &p : st.pts) {
                    QPointF q = selXf.map(p.pos());
                    p.x = q.x();
                    p.y = q.y();
                }
                st.width *= selXf.m11();   // resize scales the ink width too
            }
            changed = true;
        } else if (!moving) {
            QSet<int> in = strokesInside(sheet.strokes, active == Rect ? QPolygonF(QRectF(lasso.first(), lasso.last()).normalized())
                                                                         : lasso);
            select(adding ? in | sel : in);
        }
        moving = resizing = false;
        selXf = {};
        lasso.clear();
    }
    active = None;
    update();
    if (changed) pushUndo(before);
}

bool Canvas::cropPicture(int i, QRectF r) {
    Stroke &st = sheet.strokes[i];
    QRectF box = QRectF(st.pts[0].pos(), st.pts[1].pos()).normalized();
    r &= box;
    QImage img = imageOf(st.img);
    if (r.isEmpty() || r == box || img.isNull()) return false;
    double sx = img.width() / box.width(), sy = img.height() / box.height();
    QRect px = QRectF((r.left() - box.left()) * sx, (r.top() - box.top()) * sy, r.width() * sx, r.height() * sy).toAlignedRect();
    QString id = storeImage(img.copy(px & img.rect()));
    if (id.isEmpty()) return false;
    st = {{{float(r.left()), float(r.top()), 1, 0}, {float(r.right()), float(r.bottom()), 1, 0}}, st.color, 0, id};
    cacheDirty = true;
    return true;
}

void Canvas::cancel() {
    if (active == None) return;
    sheet.strokes = before;   // drops the live stroke, restores erased strokes and moved selections
    active = None;
    moving = resizing = false;
    selXf = {};
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
    for (int i : sel) {
        QColor k = c;
        k.setAlpha(sheet.strokes[i].color.alpha());   // a highlight stays one
        sheet.strokes[i].color = k;
    }
    pushUndo(prev);
    update();
}

static const char *kStrokesMime = "application/x-techy-draw-strokes";

void Canvas::copySelection() {
    if (sel.isEmpty()) return;
    QList<Stroke> out;
    for (int i : sel) out << sheet.strokes[i];
    QByteArray bytes;
    QDataStream(&bytes, QIODevice::WriteOnly) << kSheetVersion << out;   // clipboard format version = sheet's stroke format
    auto md = new QMimeData;
    md->setData(kStrokesMime, bytes);
    md->setImageData(snapshot(out, selBounds()));   // for other apps
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

void Canvas::paste(QPointF at) {
    if (at.isNull()) at = rect().center();
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md || active != None) return;
    if (!md->hasFormat(kStrokesMime) && md->hasImage())   // a screenshot
        return addImage(qvariant_cast<QImage>(md->imageData()), toWorld(at));
    QDataStream d(md->data(kStrokesMime));
    qint32 version = 0;
    QList<Stroke> in;
    d >> version >> in;
    if (d.status() != QDataStream::Ok || version != kSheetVersion || in.isEmpty()) return;
    QRectF b;
    for (const Stroke &s : in) b |= bounds(s);
    QPointF by = toWorld(at) - b.center();
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

void Canvas::addImage(const QImage &img, QPointF center) {
    if (img.isNull() || active != None) return;
    QString id = storeImage(img);
    if (id.isEmpty()) return (void)QMessageBox::warning(this, "Techy-Draw", "Could not store the image in " + imageDir);
    QSizeF mm = QSizeF(img.size()) * 25.4 / 96 / devicePixelRatioF();   // about its size on screen
    QSizeF fit = sheet.page * 0.9;
    if (mm.width() > fit.width() || mm.height() > fit.height()) mm.scale(fit, Qt::KeepAspectRatio);
    QPointF a = center - QPointF(mm.width(), mm.height()) / 2, b = a + QPointF(mm.width(), mm.height());
    QList<Stroke> prev = sheet.strokes;
    sheet.strokes << Stroke{{{float(a.x()), float(a.y()), 1, 0}, {float(b.x()), float(b.y()), 1, 0}}, Qt::black, 0, id};
    pushUndo(prev);
    select({int(sheet.strokes.size() - 1)});
}

void Canvas::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasImage() || e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void Canvas::dropEvent(QDropEvent *e) {
    QPointF at = toWorld(e->position());
    if (e->mimeData()->hasImage()) return addImage(qvariant_cast<QImage>(e->mimeData()->imageData()), at);
    for (const QUrl &u : e->mimeData()->urls()) {   // image files; anything else is skipped
        QImage img(u.toLocalFile());
        if (img.isNull()) continue;
        addImage(img, at);
        at += QPointF(10, 10);
    }
}

void Canvas::erase(QPointF w) {
    // ponytail: point hit-test per event, fast swipes can skip thin strokes; test the swept segment if that bites
    if (sheet.strokes.removeIf([&](const Stroke &s) { return hits(s, w, 4 / scale); })) {
        changed = cacheDirty = true;
        update();
    }
}

void Canvas::step(QList<QList<Stroke>> &from, QList<QList<Stroke>> &to) {
    if (from.isEmpty() || active != None) return;
    to.append(sheet.strokes);
    sheet.strokes = from.takeLast();
    modified();
    sel.clear();
    cacheDirty = true;
    update();
}

void Canvas::undo() { step(undoStack, redoStack); }
void Canvas::redo() { step(redoStack, undoStack); }

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

void Canvas::openRing(QPointF s) {
    QPointF w = toWorld(s);
    ring = objectActions(w);
    bool colors = ring.isEmpty() || !sel.isEmpty();   // tools ring and selection ring get the colour band
    if (ring.isEmpty()) {
        const ushort icons[] = {0xE76D, 0xE7E6, 0xE738, 0, 0xE739, 0xE7A8, 0xE75C};   // kToolNames order; 0: no glyph, label
        for (int t = 0; t < 7; ++t) ring << MenuItem{QChar(icons[t]), kToolNames[t], [this, t] { tool = Tool(t); }};
        ring << MenuItem{QChar(0xE77F), "Paste", [this, s] { paste(s); }}
             << MenuItem{QChar(0xE7A7), "Undo", [this] { undo(); }} << MenuItem{QChar(0xE7A6), "Redo", [this] { redo(); }};
    }
    ringInner = ring.size();
    if (colors)
        for (const char *hex : kColors)
            ring << MenuItem{QChar(), hex, [this, c = QColor(hex)] { setColor(c); }, QColor(hex)};
    ringAt = s;
    ringHot = -1;
    ringOpen = true;
    plusOn = false;
    update();
}

// Arc of ring item i: start angle and span, degrees counter-clockwise from 3 o'clock.
// Right-handed: 7:30 over 10:30 to 1:30, away from the hand; left-handed mirrored.
std::pair<double, double> Canvas::ringArc(int i) const {
    bool inner = i < ringInner;
    int n = inner ? ringInner : ring.size() - ringInner;
    double span = 180.0 / n, a = 45 + (inner ? i : i - ringInner) * span;
    return {leftHanded ? 180 - a - span : a, span};
}

int Canvas::ringItemAt(QPointF s) const {
    QPointF v = s - ringAt;
    double r = QLineF(s, ringAt).length(), ang = qRadiansToDegrees(qAtan2(-v.y(), v.x()));
    if (r < kRing[0] || r >= (ringInner < ring.size() ? kRing[2] : kRing[1])) return -1;
    for (int i = 0; i < ring.size(); ++i) {
        auto [a, span] = ringArc(i);
        if ((i < ringInner) == (r < kRing[1]) && std::fmod(ang - a + 720, 360.0) < span) return i;
    }
    return -1;
}

// Radial menu (pen only): opens while hovering with the side button; hover lights an item, a tap picks it, a tap
// elsewhere closes it. Windows reports the button only until that tap, so the tap is part of the gesture. The tap
// never draws: the pen stays blocked until it is lifted. True: the event was the ring's.
bool Canvas::ringEvent(QTabletEvent *e) {
    if (ringOpen) {
        e->accept();
        if (e->type() == QEvent::TabletPress) {
            int i = ringItemAt(e->position());
            ringOpen = false;
            penBlocked = needBarrelOff = true;
            update();
            if (i >= 0) ring[i].run();
            if (onPick) onPick();
        } else if (e->type() == QEvent::TabletMove && ringItemAt(e->position()) != ringHot) {
            ringHot = ringItemAt(e->position());
            update();
        }
        return true;
    }
    if (penBlocked) {
        e->accept();
        if (e->type() == QEvent::TabletRelease) penBlocked = false;
        return true;
    }
    if (!penBarrel) needBarrelOff = false;
    if (penBarrel && !needBarrelOff && e->type() == QEvent::TabletMove && !e->buttons() && active == None &&
        !QApplication::activePopupWidget()) {
        e->accept();
        openRing(e->position());
        return true;
    }
    return false;
}


void Canvas::tabletEvent(QTabletEvent *e) {
    lastPenMs = QDateTime::currentMSecsSinceEpoch();
    if (ringEvent(e)) return;
    // Right-click button: one menu per hold, like the notebook tree; it cancels a stroke in progress.
    // Held-button events stay ours, else Qt turns them into mouse events that close the menu again.
    if (e->buttons() & menuButtons) {
        if (!menuHeld && (e->type() == QEvent::TabletPress || active != None)) {
            menuHeld = needBarrelOff = true;   // no ring from the button state Windows keeps after this
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

// The object under w (world mm) and what can be done with it: general actions first, then its own.
// Objects: the selection, then the topmost unlocked picture. Empty: plain canvas.
// ponytail: two kinds as plain branches; an object type when equations/graphs add more
QList<Canvas::MenuItem> Canvas::objectActions(QPointF w) {
    int img = -1;
    for (int i = sheet.strokes.size() - 1; i >= 0 && img < 0; --i) {
        const Stroke &st = sheet.strokes[i];
        if (!st.img.isEmpty() && !st.locked && QRectF(st.pts[0].pos(), st.pts[1].pos()).normalized().contains(w)) img = i;
    }
    if (!sel.isEmpty() && sel != QSet<int>{img} && selBounds().contains(w))   // a selected picture alone stays a picture
        return {{QChar(0xE8C6), "Cut", [this] { copySelection(); deleteSelection(); }},
                {QChar(0xE8C8), "Copy", [this] { copySelection(); }},
                {QChar(0xE74D), "Delete", [this] { deleteSelection(); }}};
    if (img < 0) return {};
    return {{QChar(0xE7C2), "Move / resize picture", [this, img] { select({img}); }},   // selected: any tool drags it
            {QChar(0xE74D), "Delete picture", [this, img] { select({img}); deleteSelection(); }},
            {QChar(0xE7A8), "Crop picture", [this, img] {
                 select({});
                 trimIdx = -1;
                 cropIdx = img;
                 const Stroke &st = sheet.strokes[img];
                 cropRect = QRectF(st.pts[0].pos(), st.pts[1].pos()).normalized();
                 update();
             }},
            {QChar(0xE8C6), "Trim picture", [this, img] {
                 select({});
                 cropIdx = -1;
                 trimIdx = img;
                 update();
             }}};
}

void Canvas::pageMenu(QPointF s) {
    QPointF w = toWorld(s);
    QPoint c = sheet.cellAt(w);
    QList<MenuItem> acts = objectActions(w);
    if (menuOpen || (sinceMenu.isValid() && sinceMenu.elapsed() < 500) || (acts.isEmpty() && !sheet.pages.contains(c))) return;
    menuOpen = true;
    plusOn = false;
    QMenu m;
    for (const MenuItem &a : acts) m.addAction(a.label, a.run);
    if (!acts.isEmpty()) m.addSeparator();
    if (sheet.pages.contains(c)) {
        m.addAction("Clear page", [&] { clearPage(c); })->setEnabled(sheet.exportQueue.contains(c));   // queue = pages with ink
        m.addAction("Delete page", [&] { deletePage(c); })->setEnabled(canDeletePage(sheet, c));
    }
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
