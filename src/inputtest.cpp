// Canvas input checks with simulated mouse, pen and touch events. Run: ./build/inputtest -platform offscreen
#undef NDEBUG
#include "Canvas.h"
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QMimeData>
#include <QMouseEvent>
#include <QMenu>
#include <QTimer>
#include <QEventLoop>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QWindow>   // qeventpoint_p.h needs it complete
#include <private/qeventpoint_p.h>
#include <QTemporaryDir>
#include <cassert>
#include <cstdio>
#include <QtMath>

static Canvas *cv;
static QPointF scr(QPointF mm) { return mm * 3 + QPointF(40, 40); }   // default view: scale 3, offset 40,40
static void mouse(QEvent::Type t, QPointF mm, Qt::MouseButtons b) {
    QMouseEvent e(t, scr(mm), cv->mapToGlobal(scr(mm)), t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, b, Qt::NoModifier);
    QApplication::sendEvent(cv, &e);
}
static void drag(QPointF a, QPointF b) {
    mouse(QEvent::MouseButtonPress, a, Qt::LeftButton);
    for (int i = 1; i <= 10; ++i) mouse(QEvent::MouseMove, a + (b - a) * i / 10, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, b, Qt::NoButton);
}
static void hoverAt(QPointF mm) { mouse(QEvent::MouseMove, mm, Qt::NoButton); }
static void click(QPointF mm) {
    mouse(QEvent::MouseButtonPress, mm, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, mm, Qt::NoButton);
}
static bool penPx(QEvent::Type t, QPointF px, Qt::MouseButtons b) {   // true: the canvas accepted it
    QTabletEvent e(t, QPointingDevice::primaryPointingDevice(), px, cv->mapToGlobal(px), 0.8, 0, 0, 0, 0, 0, Qt::NoModifier,
                   t == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton, b);
    QApplication::sendEvent(cv, &e);
    return e.isAccepted();
}
static void idle(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
static bool saw = false;   // a menu was open when closeMenuIn's closer ran
static void closeMenuIn(int ms) {
    saw = false;
    QTimer::singleShot(ms, [] { if (auto m = QApplication::activePopupWidget()) { saw = true; m->close(); } });
}
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } std::printf("ok: %s\n", #x); } while (0)

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    QString file = tmp.filePath("t.tsheet");
    CHECK(saveSheet(Sheet{}, file));
    Canvas c;
    cv = &c;
    c.resize(1000, 800);
    c.show();
    CHECK(c.open(file));
    const Sheet &s = c.sheet;

    drag({50, 100}, {150, 100});                       // upper part of (0,0)
    CHECK(s.pages == QSet<QPoint>({{0, 0}}) && s.exportQueue == QList<QPoint>({{0, 0}}));
    drag({50, 250}, {150, 250});                       // bottom third -> page below
    CHECK(s.pages == QSet<QPoint>({{0, 0}, {0, 1}}) && s.exportQueue == QList<QPoint>({{0, 0}}));
    drag({250, 50}, {260, 60});                        // on the ghost -> real, queued
    CHECK(s.pages.contains(QPoint(1, 0)) && s.exportQueue == QList<QPoint>({{0, 0}, {1, 0}}));
    c.tool = Canvas::Line;
    drag({100, 120}, {100, 330});                      // line into (0,1): only resampled middle/end points
    CHECK(s.exportQueue == QList<QPoint>({{0, 0}, {1, 0}, {0, 1}}));
    c.tool = Canvas::Pen;

    hoverAt({100, 3});                                 // near top edge of (0,0): "+" for (0,-1)
    click({105, 0});                                   // press on the "+"
    CHECK(s.pages.contains(QPoint(0, -1)) && s.strokes.size() == 4);   // added a page, drew nothing
    hoverAt({100, 150});                               // page middle: no "+"
    click({100, 0});                                   // (0,-1) exists now: a normal stroke (dot)
    CHECK(s.strokes.size() == 5);
    c.undo();
    CHECK(s.strokes.size() == 4 && s.pages.contains(QPoint(0, -1)));

    hoverAt({3, 100});                                 // left edge of (0,0): "+" for (-1,0)
    hoverAt({1, 148.5});                               // move onto the "+" button
    click({0, 148.5});
    CHECK(s.pages.contains(QPoint(-1, 0)));

    c.tool = Canvas::Eraser;
    drag({255, 55}, {255, 56});                        // erase the stroke on (1,0)
    CHECK(s.strokes.size() == 3 && s.exportQueue == QList<QPoint>({{0, 0}, {0, 1}}) && s.pages.contains(QPoint(1, 0)));
    c.undo();
    CHECK(s.exportQueue == QList<QPoint>({{0, 0}, {0, 1}, {1, 0}}));   // back, at the end of the queue
    c.redo();
    CHECK(s.exportQueue == QList<QPoint>({{0, 0}, {0, 1}}));
    c.tool = Canvas::Pen;

    CHECK(!c.deletePage({0, 0}) && !c.deletePage({0, 1}));   // have ink
    CHECK(!c.setPageSize({148, 210}));                 // sheet has ink
    // clear page: strokes touching (1,0) go (only the diagonal there; the redo removed it earlier, so draw one)
    drag({250, 100}, {300, 150});
    drag({100, 100}, {250, 120});                      // crosses (0,0) -> (1,0)
    qsizetype before = s.strokes.size();
    c.clearPage({1, 0});
    CHECK(s.strokes.size() == before - 2 && !s.exportQueue.contains(QPoint(1, 0)) && s.exportQueue.contains(QPoint(0, 0)));
    c.undo();
    CHECK(s.strokes.size() == before && s.exportQueue.contains(QPoint(1, 0)));
    c.clearPage({5, 5});                               // no ink there: no undo step, redo stack kept
    c.redo();
    CHECK(s.strokes.size() == before - 2);
    // auto pages off: ink outside makes no page, nothing queued for it, no page below
    c.autoPages = false;
    QSet<QPoint> pagesBefore = s.pages;
    drag({50, 280}, {60, 280});                        // bottom third of (0,0)
    drag({-300, 50}, {-290, 50});                      // outside every page
    CHECK(s.pages == pagesBefore && !s.exportQueue.contains(QPoint(-2, 0)));
    c.undo();
    c.undo();
    c.autoPages = true;

    c.setExportQueue({{0, 1}, {0, 0}});
    c.save();
    Sheet back;
    CHECK(loadSheet(back, file) && back.exportQueue == QList<QPoint>({{0, 1}, {0, 0}}) && back.pages == s.pages &&
          back.strokes.size() == s.strokes.size());
    CHECK(exportPdf(s, s.exportQueue, tmp.filePath("out.pdf")));
    {   // finger: hold 0.5 s arms (no menu yet), lift opens the menu and it stays; a moving finger opens nothing
        QPointingDevice touch("touch", 99, QInputDevice::DeviceType::TouchScreen, QPointingDevice::PointerType::Finger,
                              QInputDevice::Capability::Position, 10, 0);
        QPointF lastMm{100, 100};
        auto finger = [&](QEvent::Type t, QEventPoint::State st, QPointF mm) {
            QEventPoint pt(1, st, scr(mm), c.mapToGlobal(scr(mm)));
            QMutableEventPoint::setPosition(pt, scr(mm));   // delivery sets it for real touches
            QMutableEventPoint::setGlobalLastPosition(pt, c.mapToGlobal(scr(lastMm)));
            lastMm = mm;
            QTouchEvent e(t, &touch, Qt::NoModifier, {pt});
            static_cast<QObject &>(c).event(&e);   // sendEvent drops touch from an unregistered device
        };
        finger(QEvent::TouchBegin, QEventPoint::State::Pressed, {100, 100});
        idle(700);
        CHECK(!QApplication::activePopupWidget());          // held: armed only
        closeMenuIn(300);
        finger(QEvent::TouchEnd, QEventPoint::State::Released, {100, 100});
        idle(500);
        CHECK(saw);                                          // lifted: menu open (until something else happens)
        idle(600);                                           // menu cooldown
        finger(QEvent::TouchBegin, QEventPoint::State::Pressed, {100, 100});
        finger(QEvent::TouchUpdate, QEventPoint::State::Updated, {120, 100});   // 60 px: a pan
        idle(700);
        closeMenuIn(300);
        finger(QEvent::TouchUpdate, QEventPoint::State::Updated, {100, 100});   // pan back
        finger(QEvent::TouchEnd, QEventPoint::State::Released, {100, 100});
        idle(500);
        CHECK(!saw);
        c.fingerDraws = true;                                // finger draws: the long-press dot is dropped
        qsizetype n1 = s.strokes.size();
        finger(QEvent::TouchBegin, QEventPoint::State::Pressed, {100, 100});
        idle(700);
        closeMenuIn(300);
        finger(QEvent::TouchEnd, QEventPoint::State::Released, {100, 100});
        idle(500);
        CHECK(saw && s.strokes.size() == n1);
        c.fingerDraws = false;
        idle(600);
    }
    {   // pen
        auto pen = [](QEvent::Type t, QPointF mm, Qt::MouseButtons b) { return penPx(t, scr(mm), b); };
        qsizetype n0 = s.strokes.size();
        // right-click button + tap: menu (exec blocks until the closer runs), no ink
        closeMenuIn(300);
        pen(QEvent::TabletPress, {100, 150}, Qt::LeftButton | Qt::RightButton);
        CHECK(saw);
        pen(QEvent::TabletMove, {101, 150}, Qt::LeftButton | Qt::RightButton);   // same hold: no second menu
        pen(QEvent::TabletRelease, {101, 150}, Qt::RightButton);
        CHECK(s.strokes.size() == n0);
        pen(QEvent::TabletMove, {101, 150}, Qt::NoButton);                       // button let go
        idle(600);
        // button held while hovering, then touching down: menu
        pen(QEvent::TabletMove, {100, 150}, Qt::RightButton);
        closeMenuIn(300);
        pen(QEvent::TabletPress, {100, 150}, Qt::LeftButton | Qt::RightButton);
        CHECK(saw && s.strokes.size() == n0);
        pen(QEvent::TabletRelease, {100, 150}, Qt::NoButton);
        idle(600);
        // button pressed mid-stroke: stroke cancelled, menu
        pen(QEvent::TabletPress, {100, 150}, Qt::LeftButton);
        pen(QEvent::TabletMove, {120, 150}, Qt::LeftButton);
        closeMenuIn(300);
        pen(QEvent::TabletMove, {130, 150}, Qt::LeftButton | Qt::RightButton);
        CHECK(saw);
        pen(QEvent::TabletRelease, {130, 150}, Qt::NoButton);
        CHECK(s.strokes.size() == n0);
        idle(600);
        // any menu open: pen passed on (a click on the menu), never draws
        QMenu menu;
        menu.addAction("x");
        menu.popup(c.mapToGlobal(QPoint(10, 10)));
        CHECK(QApplication::activePopupWidget() && !pen(QEvent::TabletPress, {100, 150}, Qt::LeftButton));
        pen(QEvent::TabletMove, {120, 150}, Qt::LeftButton);
        pen(QEvent::TabletRelease, {120, 150}, Qt::NoButton);
        CHECK(s.strokes.size() == n0);
        menu.close();
        CHECK(pen(QEvent::TabletPress, {100, 150}, Qt::LeftButton));
        pen(QEvent::TabletMove, {120, 150}, Qt::LeftButton);
        pen(QEvent::TabletRelease, {120, 150}, Qt::NoButton);
        CHECK(s.strokes.size() == n0 + 1);
        c.undo();
    }

    // fresh sheet: page setup allowed, delete rules via the centre page
    QString f2 = tmp.filePath("u.tsheet");
    CHECK(saveSheet(Sheet{}, f2) && c.open(f2));
    CHECK(c.setPageSize({297, 210}) && s.page == QSizeF(297, 210));
    CHECK(!c.deletePage({0, 0}));                      // the last page
    hoverAt({100, 207});                               // bottom edge of landscape (0,0)
    click({148.5, 211});
    CHECK(s.pages == QSet<QPoint>({{0, 0}, {0, 1}}) && s.strokes.isEmpty());
    CHECK(c.deletePage({0, 0}) && s.pages == QSet<QPoint>({{0, 1}}));

    // images: paste a screenshot (centered, selected), resize by the handle, move, the eraser skips it
    imageDir = tmp.filePath("images");
    QString f3 = tmp.filePath("v.tsheet");
    CHECK(saveSheet(Sheet{}, f3) && c.open(f3));
    QImage shotImg(192, 96, QImage::Format_RGB32);   // 50.8 x 25.4 mm at 96 dpi
    shotImg.fill(Qt::red);
    QGuiApplication::clipboard()->setImage(shotImg);
    c.paste();
    CHECK(s.strokes.size() == 1 && !s.strokes[0].img.isEmpty() && QFile::exists(imageDir + "/" + s.strokes[0].img + ".png"));
    auto box = [&] { return QRectF(s.strokes[0].pts[0].pos(), s.strokes[0].pts[1].pos()); };
    CHECK(qAbs(box().width() - 50.8) < 0.01 && qAbs(box().center().x() - 459 / 3.0) < 0.01);   // view centre (499-40)/3
    QPointF br = box().bottomRight();
    drag(br, br + QPointF(50.8, 25.4));                // pen tool on the handle: resize to twice the size
    CHECK(qAbs(box().width() - 101.6) < 0.1 && qAbs(box().height() - 50.8) < 0.1);
    QPointF tl = box().topLeft();
    drag(box().center(), box().center() + QPointF(10, 0));   // inside the selection: move
    CHECK(QLineF(box().topLeft(), tl + QPointF(10, 0)).length() < 0.01);
    c.tool = Canvas::Eraser;
    drag(box().center(), box().center() + QPointF(5, 5));
    CHECK(s.strokes.size() == 1);
    c.copySelection();                                 // strokes for us, a PNG for other apps
    CHECK(QGuiApplication::clipboard()->mimeData()->hasImage());
    c.undo();
    c.undo();
    CHECK(qAbs(box().width() - 50.8) < 0.01);          // undo: move, then resize

    // highlighter: translucent, fixed width; Snip copies the area as an image
    c.tool = Canvas::Highlighter;
    drag({20, 200}, {80, 200});
    CHECK(s.strokes.size() == 2 && s.strokes[1].color.alpha() < 255 && s.strokes[1].img.isEmpty());
    c.tool = Canvas::Snip;
    drag({10, 10}, {60, 40});
    CHECK(s.strokes.size() == 2 && QGuiApplication::clipboard()->image().size() == QSize(300, 180));   // 6 px/mm
    c.save();
    Sheet img;
    CHECK(loadSheet(img, f3) && img.strokes[0].img == s.strokes[0].img && img.strokes[1].color == s.strokes[1].color);

    // picture menu (right-click on it): Move selects it, Trim crops it to the next drag, Delete removes it
    auto menuPick = [&](QPointF mm, const QString &item) {
        QTimer::singleShot(100, [=] {
            if (auto m = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                for (QAction *a : m->actions())
                    if (a->text() == item) a->trigger();
                m->close();
            }
        });
        QMouseEvent e(QEvent::MouseButtonPress, scr(mm), c.mapToGlobal(scr(mm)), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&c, &e);   // returns once the menu closed
        idle(600);                          // menu cooldown
    };
    c.tool = Canvas::Pen;
    QRectF pic = box();
    menuPick(pic.center(), "Move / resize picture");
    drag(pic.center(), pic.center() + QPointF(20, 0));   // the pen moves it, draws nothing
    CHECK(s.strokes.size() == 2 && QLineF(box().topLeft(), pic.topLeft() + QPointF(20, 0)).length() < 0.01);
    pic = box();
    QString oldId = s.strokes[0].img;
    menuPick(pic.center(), "Trim picture");
    drag(pic.topLeft() - QPointF(5, 5), pic.center());   // top-left quarter; the part outside the picture is ignored
    QSize px = imageOf(s.strokes[0].img).size();
    CHECK(s.strokes.size() == 2 && s.strokes[0].img != oldId && QLineF(box().topLeft(), pic.topLeft()).length() < 0.01 &&
          qAbs(box().width() - pic.width() / 2) < 0.01 && px.width() >= 96 && px.width() <= 97 && px.height() >= 48 && px.height() <= 49);
    c.undo();
    CHECK(s.strokes[0].img == oldId);
    // Crop: handles size the frame, inside moves it (nothing changes yet), a tap outside applies
    pic = box();
    menuPick(pic.center(), "Crop picture");
    drag(pic.bottomRight(), pic.bottomRight() - QPointF(pic.width() / 2, 0));   // keep the left half
    drag(pic.topLeft(), pic.topLeft() + QPointF(0, pic.height() / 2));          // ... and its bottom half
    QPointF mid(pic.left() + pic.width() / 4, pic.top() + pic.height() * 3 / 4);
    drag(mid, mid + QPointF(10, -10));                                          // move the frame
    CHECK(s.strokes[0].img == oldId && s.strokes.size() == 2);
    click(pic.bottomRight() + QPointF(30, 30));
    QRectF want(pic.left() + 10, pic.center().y() - 10, pic.width() / 2, pic.height() / 2);
    CHECK(s.strokes[0].img != oldId && s.strokes.size() == 2 && QLineF(box().topLeft(), want.topLeft()).length() < 0.01 &&
          QLineF(box().bottomRight(), want.bottomRight()).length() < 0.01);
    c.undo();
    CHECK(s.strokes[0].img == oldId);
    menuPick(box().center(), "Delete picture");
    CHECK(s.strokes.size() == 1 && s.strokes[0].img.isEmpty());

    // pen ring: side button while hovering (Windows' flag, faked here) opens it at the tip; a tap on an item picks it,
    // a tap elsewhere closes it; that tap never draws. Windows reports the button until that tap.
    auto onRing = [](QPointF at, double deg, double r) {
        return at + QPointF(qCos(qDegreesToRadians(deg)), -qSin(qDegreesToRadians(deg))) * r;
    };
    auto openRingAt = [&](QPointF px) {
        Canvas::penBarrel = true;   // side button pressed while hovering
        penPx(QEvent::TabletMove, px, Qt::NoButton);
    };
    auto tapAt = [&](QPointF px) {
        penPx(QEvent::TabletPress, px, Qt::LeftButton | (Canvas::penBarrel ? Qt::RightButton : Qt::NoButton));
        Canvas::penBarrel = false;                                       // the tap ends it
        penPx(QEvent::TabletMove, px + QPointF(30, 0), Qt::LeftButton);   // draws unless blocked
        penPx(QEvent::TabletRelease, px + QPointF(30, 0), Qt::NoButton);
        penPx(QEvent::TabletMove, px + QPointF(30, 0), Qt::NoButton);    // hovers again
    };
    QPointF at(500, 400);
    qsizetype n = s.strokes.size();
    openRingAt(at);
    QPointF eraserItem = onRing(at, 45 + 6.5 * 18, 100);                 // 7th of 10 tool items, 18° each
    penPx(QEvent::TabletMove, eraserItem, Qt::NoButton);
    tapAt(eraserItem);
    CHECK(c.tool == Canvas::Eraser && s.strokes.size() == n);
    c.tool = Canvas::Pen;
    penPx(QEvent::TabletMove, at, Qt::NoButton);
    tapAt(at);                                                           // ring closed: an ordinary stroke
    CHECK(s.strokes.size() == n + 1);
    c.undo();
    openRingAt(at);
    tapAt(onRing(at, 45 + 2.5 * 36, 172));                               // third of 5 colours
    CHECK(c.color == QColor("#c62828") && s.strokes.size() == n);
    openRingAt(at);
    tapAt(at);                                                           // the centre: closes, changes nothing
    CHECK(c.color == QColor("#c62828") && c.tool == Canvas::Pen && s.strokes.size() == n);
    std::puts("inputtest ok");
    return 0;
}
