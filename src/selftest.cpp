#undef NDEBUG   // asserts must run in Release builds too
#include "Sheet.h"
#include <QIODevice>
#include <QLineF>
#include <QTemporaryDir>
#include <cassert>
#include <cstdio>

int main() {
    Sheet s;
    assert(s.pages.contains(QPoint(0, 0)));
    assert(s.pageRect(QPoint(-1, 2)) == QRectF(-210, 594, 210, 297));

    Stroke st{{{0, 0, 1, 0}, {10, 0, 1, 0}}, Qt::black, 1};
    assert(hits(st, {5, 0.4}, 0.1));      // within half width
    assert(hits(st, {5, 1.5}, 1));        // within radius + half width
    assert(!hits(st, {5, 2}, 1));
    assert(!hits(st, {12, 0}, 1));        // past the end
    assert(hits({{{3, 3, 1, 0}}, Qt::black, 1}, {3.5, 3}, 0.1));   // single-point dot

    QPointF b = snap15({0, 0}, {10, 1});  // ~5.7° -> 0°
    assert(qAbs(b.y()) < 1e-9 && qAbs(b.x() - QLineF(0, 0, 10, 1).length()) < 1e-9);
    b = snap15({0, 0}, {10, -9});         // ~42° up -> 45°
    assert(qAbs(b.x() + b.y()) < 1e-9);

    // selection: only strokes with every point inside
    QList<Stroke> strokes{st, {{{3, 3, 1, 0}}, Qt::red, 1}, {{{3, 3, 1, 0}, {30, 3, 1, 0}}, Qt::red, 1}};
    assert(strokesInside(strokes, QPolygonF(QRectF(-1, -1, 15, 15))) == QSet<int>({0, 1}));
    assert(bounds(st) == QRectF(-0.5, -0.5, 11, 1));

    // stroke serialization round-trip (clipboard now, sheet files later)
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << strokes;
    QList<Stroke> back;
    QDataStream in(bytes);
    in >> back;
    assert(back.size() == 3 && back[2].pts[1].x == 30 && back[1].color == Qt::red && back[0].width == 1);

    // sheet file round-trip
    QTemporaryDir tmp;
    Sheet sh;
    sh.strokes = strokes;
    sh.pages << QPoint(-1, 2);
    sh.page = {148, 210};
    sh.exportQueue = {{0, 0}, {-1, 2}};
    assert(saveSheet(sh, tmp.filePath("a.tsheet")));
    Sheet ld;
    assert(loadSheet(ld, tmp.filePath("a.tsheet")));
    assert(ld.strokes.size() == 3 && ld.strokes[2].pts[1].x == 30 && ld.pages == sh.pages && ld.page == sh.page &&
           ld.exportQueue == sh.exportQueue);
    assert(!loadSheet(ld, tmp.filePath("missing.tsheet")) && ld.pages == sh.pages);   // failed load leaves it untouched

    std::puts("selftest ok");
}
