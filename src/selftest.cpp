#undef NDEBUG   // asserts must run in Release builds too
#include "Sheet.h"
#include <QFile>
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

    // activation is point-based: a diagonal line crossing (0,0) -> (1,0) -> (1,1) never touches (0,1)
    auto line = [](double x1, double y1, double x2, double y2) { return Stroke{{{float(x1), float(y1), 1, 0}, {float(x2), float(y2), 1, 0}}, Qt::black, 1}; };
    Sheet g;
    assert(inkCells(g, line(205, 280, 215, 300)) == QSet<QPoint>({{0, 0}, {1, 0}, {1, 1}}));
    assert(inkCells(g, line(10, 10, 10, 900)) == QSet<QPoint>({{0, 0}, {0, 1}, {0, 2}, {0, 3}}));   // resampled middle
    assert(inkCells(g, line(-5, -5, -4, -4)) == QSet<QPoint>({{-1, -1}}));                          // negative cells
    // down growth: ink in the bottom third adds the page below, ink above it does not
    assert(inkCells(g, line(10, 150, 20, 150), true) == QSet<QPoint>({{0, 0}}));
    assert(inkCells(g, line(10, 250, 20, 250), true) == QSet<QPoint>({{0, 0}, {0, 1}}));

    // queue: inked pages appended in order, kept order on re-ink, emptied pages dropped; ink outside makes pages
    g.strokes << line(10, 10, 20, 10);
    refresh(g);
    assert(g.exportQueue == QList<QPoint>({{0, 0}}));
    g.strokes << line(220, 10, 230, 10) << line(10, -10, 20, -10);   // right of (0,0) and above it
    refresh(g);
    assert(g.pages == QSet<QPoint>({{0, 0}, {1, 0}, {0, -1}}));
    assert(g.exportQueue == QList<QPoint>({{0, 0}, {0, -1}, {1, 0}}));   // new ones appended in row, column order
    g.exportQueue = {{1, 0}, {0, -1}, {0, 0}};                           // user reorder
    g.strokes << line(30, 30, 40, 40);
    refresh(g);
    assert(g.exportQueue == QList<QPoint>({{1, 0}, {0, -1}, {0, 0}}));
    g.strokes.removeAt(1);                                               // erase the ink on (1,0)
    refresh(g);
    assert(g.exportQueue == QList<QPoint>({{0, -1}, {0, 0}}) && g.pages.contains(QPoint(1, 0)));   // empty page stays

    // delete: only empty pages, never the last, no new island
    assert(canDeletePage(g, {1, 0}) && !canDeletePage(g, {0, 0}) && !canDeletePage(g, {5, 5}));
    g.strokes = {};
    g.pages = {{0, 0}, {1, 0}, {2, 0}, {1, 1}};
    assert(!canDeletePage(g, {1, 0}));                                   // would split (0,0) from (2,0)
    assert(canDeletePage(g, {2, 0}) && canDeletePage(g, {1, 1}) && canDeletePage(g, {0, 0}));
    g.pages = {{0, 0}, {3, 3}};                                          // islands from ink outside pages
    assert(canDeletePage(g, {3, 3}));
    g.pages = {{0, 0}};
    assert(!canDeletePage(g, {0, 0}));

    // PDF: one page per queue entry, page size from the sheet (landscape kept)
    g = Sheet{};
    g.page = {297, 210};
    g.strokes << line(10, 10, 600, 10) << line(10, 300, 20, 300);   // crosses three pages; one on row 1
    refresh(g);
    assert(g.exportQueue.size() == 4);
    QString pdfPath = tmp.filePath("a.pdf");
    assert(exportPdf(g, pdfPath));
    QFile pf(pdfPath);
    assert(pf.open(QIODevice::ReadOnly));
    QByteArray pdf = pf.readAll();
    assert(pdf.startsWith("%PDF") && pdf.contains("/Count 4"));
    assert(pdf.contains("/MediaBox [0 0 842") && !pdf.contains("/MediaBox [0 0 595"));   // 297 x 210 mm in points
    assert(!exportPdf(Sheet{}, tmp.filePath("b.pdf")));                                    // nothing to export

    std::puts("selftest ok");
}
