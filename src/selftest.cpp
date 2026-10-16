#undef NDEBUG   // asserts must run in Release builds too
#include "Ink.h"
#include "Math.h"
#include "Sheet.h"
#include <QElapsedTimer>
#include <QHash>
#include <random>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QIODevice>
#include <QLineF>
#include <QTemporaryDir>
#include <cassert>
#include <cstdio>

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);   // fonts for typesetting

    // math: parsing (implicit products, precedence, functions), errors, ExprTk form
    auto ex = [](const char *s) { QString err; MathNode n = parseMath(s, &err); assert(err.isEmpty()); return exprtkOf(n); };
    assert(ex("x^2 - 2x") == "((x^2)-(2*x))");
    assert(ex("2sin(x)/x") == "((2*sin(x))/x)");
    assert(ex("-x^2") == "(-(x^2))" && ex("2^3^2") == "(2^(3^2))" && ex("x^-1") == "(x^(-1))");
    assert(ex("|x-1|") == "abs((x-1))" && typeset(nullptr, parseMath("sin(x)"), 0, 0, 10).w > typeset(nullptr, parseMath("(x)"), 0, 0, 10).w + 10);
    assert(ex("sqrt(x+1)") == "sqrt((x+1))" && ex("ln(x) + log(x)") == "(log(x)+log10(x))" && ex("sin x") == "sin(x)");
    assert(ex("pi x") == "(pi*x)" && ex("xy") == "(x*y)" && ex("(x+1)(x-1)") == "((x+1)*(x-1))" && ex("2e") == "(2*2.718281828459045)");
    assert(ex("x² − 2·π") == "((x^2)-(2*pi))" && ex("√x ÷ 2") == "(sqrt(x)/2)");   // touch keyboard / recognizer symbols
    assert(compileCurve("(x^2)")(3) == 9 && !compileCurve("(a*x)"));
    // notation: Greek by name, ∫ with limits and dx, ∑, lim with an arrow, d/dx; none of them is plotted
    MathNode ig = parseMath("int_0^(2pi) sin(x)^2 + 1 dx");
    assert(ig.k == 'I' && ig.s == "x" && ig.a[0].k == '+' && ig.a[1].s == "0" && ig.a[2].k == '(' && exprtkOf(ig) == "?");
    assert(parseMath("∫x dx").k == 'I' && parseMath("int_-1^1 x dx").a[1].k == '~');
    MathNode sm = parseMath("sum_(n=1)^inf 1/n^2");
    assert(sm.k == 'S' && sm.a[1].a[0].k == '=' && sm.a[2].s == "inf" && sm.a[0].k == '/');
    MathNode lm = parseMath("lim_(x->0) sin(x)/x");
    assert(lm.k == 'L' && lm.a[1].a[0].k == '>' && lm.a[0].k == '/');
    assert(parseMath("x → ∞").k == '>' && parseMath("d/dx x^2").k == 'D' && parseMath("d/dx x^2").s == "x");
    assert(ex("theta + alpha") == "(θ+α)" && ex("Theta beta eta") == "((Θ*β)*η)" && ex("epsilon psi") == "(ε*ψ)");
    assert(ex("d(x)") == "(d*(x))" && ex("x - 1") == "(x-1)");   // plain d and minus still work
    // functions: g(x) = 2x plots 2x; calls of names defined in any line expand, other letter( stays a product
    QStringList fl{"f(x) = x^2", "g(t) = 2t", "y = f(x-1) + g(x)", "a(x+1)", "y = h(x)"};
    assert(exprtkOf(curveOf(fl, 0)) == "(x^2)" && exprtkOf(curveOf(fl, 1)) == "(2*x)");
    assert(exprtkOf(curveOf(fl, 2)) == "(((x-1)^2)+(2*x))" && curveOf(fl, 3).k == 'c' && curveOf(fl, 4).k == 'c');
    assert(curveOf({"f(x) = f(x)"}, 0).k == 0 && curveOf({"x = 3"}, 0).k == 0 && curveOf(fl, 9).k == 0);
    for (const char *t : {"int_0^1 x^2 dx", "sum_(k=0)^n k", "lim_(h->0) (f(x+h) - f(x))/h", "d/dt t^2"})
        assert(typeset(nullptr, parseMath(t), 0, 0, 10).up > 8.5);   // taller than a digit (0.8 s)
    MathNode eq = parseMath("y = x/2");
    assert(eq.k == '=' && eq.a[0].k == 'v' && eq.a[1].k == '/');
    for (const char *bad : {"", "x+", "(x", "x)", "2*/3", "x#"}) {
        QString err;
        MathNode n = parseMath(bad, &err);
        assert(!err.isEmpty() && n.k == 'n' && n.s == bad);   // shown as plain text
    }
    // typesetting: a fraction is taller than a plain number, an exponent widens less than a full-size digit
    MathBox one = typeset(nullptr, parseMath("1"), 0, 0, 10), half = typeset(nullptr, parseMath("1/2"), 0, 0, 10);
    assert(half.up > one.up && half.down > one.down && half.up + half.down > 1.5 * (one.up + one.down));
    assert(typeset(nullptr, parseMath("x^2"), 0, 0, 10).w < typeset(nullptr, parseMath("x2"), 0, 0, 10).w);
    // plot: y = x in [-10,10]² crosses (5,5) and not (5,-5); unknown variables and bad lines are skipped
    QImage plot(200, 200, QImage::Format_RGB32);
    plot.fill(Qt::white);
    {
        QPainter pp(&plot);
        pp.setRenderHint(QPainter::Antialiasing);
        drawMath(pp, "plot -10 10 -10 10\ny = x\ny = a x\nx+", plot.rect());
        drawMath(pp, "eq\ny = x^2/2\nsqrt(x)", QRectF(0, 0, 40, 20));   // smoke: typeset objects draw
    }
    auto curveAt = [&](int x, int y) {
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (QColor c = plot.pixelColor(x + dx, y + dy); c.red() > c.green() + 60) return true;
        return false;
    };
    assert(curveAt(150, 50) && !curveAt(150, 150));

    // ink recognition with the built-in classifier: HWRT samples placed like handwriting (grouping and layout)
    QList<Template> seed = loadTemplates(":/symbols.tpl");
    assert(seed.size() > 1000);
    auto write = [&](QList<QPolygonF> &ink, const QString &sym, double x, double y, double size) {
        auto t = std::find_if(seed.begin(), seed.end(), [&](const Template &t) { return t.sym == sym; });
        assert(t != seed.end());
        for (QPolygonF st : t->strokes) {
            for (QPointF &p : st) p = QPointF(x, y) + p * size;
            ink << st;
        }
    };
    auto read = [&](const QList<QPolygonF> &ink) { return inkLayout(recognizeSymbols(ink, {})).text; };
    QElapsedTimer took;
    took.start();
    QList<QPolygonF> ink;
    write(ink, "x", 0, 10, 8), write(ink, "2", 9, 4, 5), write(ink, "+", 16, 11, 6), write(ink, "1", 25, 10, 8);
    QString got = read(ink);
    std::printf("ink: %s (%lld ms)\n", qPrintable(got), took.elapsed());
    assert(got == "x^2+1");
    ink = {};
    write(ink, "1", 1, 0, 6), write(ink, "-", -1, 8, 9), write(ink, "2", 1, 11, 6);
    std::printf("ink: %s\n", qPrintable(got = read(ink)));
    assert(got == "(1)/(2)");
    ink = {};
    write(ink, "∫", 0, 0, 14), write(ink, "1", 7, -2, 4), write(ink, "0", 5, 12, 4);
    write(ink, "x", 11, 6, 5), write(ink, "d", 17, 4, 7), write(ink, "x", 23, 6, 5);
    MathNode integral = parseMath(got = read(ink));
    std::printf("ink: %s\n", qPrintable(got));
    assert(integral.k == 'I' && integral.s == "x" && integral.a[1].a[0].s == "0");   // limits come in parentheses
    ink = {};
    write(ink, "1", 0, 0, 6), write(ink, "2", 0, 20, 6);
    assert(read(ink) == "1\n2" && inkLayout(recognizeSymbols(ink, {})).lines.size() == 2);
    {   // regression: every built-in x alone is "x", and "y=1" with each built-in y (not y^(=1), x^...)
        int xs = 0, ys = 0, nx = 0, ny = 0;
        for (const Template &t : seed) {
            if (t.sym != "x" && t.sym != "y") continue;
            QList<QPolygonF> in;
            for (QPolygonF st : t.strokes) { for (QPointF &p : st) p = p * 8 + QPointF(0, 10); in << st; }
            if (t.sym == "x") {
                QString r = read(in);
                ++nx, xs += r == "x";
                if (r != "x") std::printf("  x -> %s (%d strokes)\n", qPrintable(r), int(t.strokes.size()));
            } else {
                QRectF yb = QRectF();
                for (const QPolygonF &st : in) yb |= st.boundingRect();
                // y's baseline at 60% of its height (the tail below): "=" at mid x-height, "1" standing on the baseline
                double baseline = yb.top() + 0.6 * yb.height();
                write(in, "=", yb.right() + 2, yb.top() + 0.1 * yb.height(), 5);
                write(in, "1", yb.right() + 9, baseline - 0.9 * yb.height(), 0.9 * yb.height());
                QString r = read(in);
                ++ny, ys += r == "y=1";
                if (r != "y=1") std::printf("  y=1 -> %s\n", qPrintable(r));
            }
        }
        std::printf("x: %d/%d, y=1: %d/%d\n", xs, nx, ys, ny);
        assert(xs >= 15 && ys >= 18);   // a few HWRT samples are odd (6 strokes, a single-stroke x)
    }
    {   // segmentation: 300 random 3-letter rows of built-in samples written close together (0.2 heights apart)
        std::mt19937 rng(5);
        QHash<QString, QList<const Template *>> of;
        for (const Template &t : seed)
            if (t.sym.size() == 1 && t.sym[0] >= 'a' && t.sym[0] <= 'z') of[t.sym] << &t;
        QStringList letters = of.keys();
        letters.sort();   // QHash order changes per run
        int right = 0, symbolsRight = 0, aligned = 0;
        for (int n = 0; n < 300; ++n) {
            QList<QPolygonF> in;
            QString word;
            QList<QList<int>> truth;   // each letter's strokes
            double x = 0;
            for (int k = 0; k < 3; ++k) {
                QString c = letters[rng() % letters.size()];
                const Template *t = of[c][rng() % of[c].size()];
                QRectF b;
                for (const QPolygonF &st : t->strokes) b |= st.boundingRect();
                double sc = 8 / std::max(b.height(), 0.3);   // 8 mm high (a wide flat letter: at most 3x)
                truth.append(QList<int>{});
                for (QPolygonF st : t->strokes) {
                    for (QPointF &p : st) p = (p - b.topLeft()) * sc + QPointF(x, 0);
                    truth.last() << int(in.size());
                    in << st;
                }
                x += b.width() * sc + 1.6;
                word += c;
            }
            QList<InkSymbol> got = recognizeSymbols(in, {});
            right += inkLayout(got).text == word;
            symbolsRight += got.size() == 3;
            QList<QList<int>> al = alignSymbols(in, word.split("", Qt::SkipEmptyParts), {});   // calibration knows the word
            aligned += al == truth;
            if (al != truth && qEnvironmentVariableIsSet("SEGDEBUG")) {
                auto str = [](const QList<QList<int>> &g) { QString s; for (const auto &l : g) { s += "["; for (int i : l) s += QString::number(i) + " "; s += "]"; } return s; };
                std::printf("  align %s: %s, want %s\n", qPrintable(word), qPrintable(str(al)), qPrintable(str(truth)));
            }
            if (got.size() != 3 && qEnvironmentVariableIsSet("SEGDEBUG")) {
                QString g;
                for (const InkSymbol &s : got) g += "[" + s.cands[0] + ":" + QString::number(s.strokes.size()) + "]";
                std::printf("  %s (%d strokes) -> %s\n", qPrintable(word), int(in.size()), qPrintable(g));
            }
        }
        std::printf("segmentation: %d/300 words read right, %d/300 split into 3 symbols, calibration: %d/300 aligned\n",
                    right, symbolsRight, aligned);
        assert(symbolsRight >= 285 && right >= 235 && aligned >= 285);
    }
    // an i dotted at the end of the word, the dot a short dash: still one symbol with its stem
    ink = {QPolygonF{{0, 4}, {0.2, 10}}};
    write(ink, "n", 3, 4, 6);
    qsizetype dotAt = ink.size();
    ink << QPolygonF{{-0.3, 2}, {0.5, 2.1}};
    QList<InkSymbol> word = recognizeSymbols(ink, {});
    assert(word.size() == 2 && word[0].strokes == QList<int>({0, int(dotAt)}));
    // calibration score: a sample of x is x for sure; expected as "o" it loses (below 0.5) and the guess says x
    QList<QPolygonF> xs;
    write(xs, "x", 0, 0, 10);
    Guess gx = guessSymbol(xs, "x", {}), go = guessSymbol(xs, "o", {});
    std::printf("x: %s %.2f, as o: %s %.2f\n", qPrintable(gx.sym), gx.certainty, qPrintable(go.sym), go.certainty);
    assert(gx.sym == "x" && gx.certainty > 0.9 && go.sym == "x" && go.certainty < 0.5);
    // learning: a shape it has never seen (a spiral) is a new symbol after three personal samples, sure of it,
    // and the built-in symbols are not forgotten
    auto spiral = [](double k) {
        QPolygonF s;
        for (int i = 0; i < 40; ++i) s << QPointF(std::cos(i * 0.4), std::sin(i * 0.4)) * (i * 0.4 * k);
        return QList<QPolygonF>{s};
    };
    QList<Template> mine;
    for (double k : {1.0, 1.05, 0.95}) mine << makeTemplate("@", spiral(k));
    Guess before = guessSymbol(spiral(1.02), "@", {}), after = guessSymbol(spiral(1.02), "@", mine);
    std::printf("learned: %s %.2f\n", qPrintable(after.sym), after.certainty);
    assert(before.certainty == 0 && after.sym == "@" && after.certainty > 0.9 && guessSymbol(xs, "x", mine).sym == "x");
    took.restart();   // retraining cost with a well-used personal set (400 samples)
    guessSymbol(xs, "x", seed.mid(0, 400));
    std::printf("retrained on 400 samples: %lld ms\n", took.elapsed());
    // calibration alignment: "sin" with the i dotted last goes to s / i (stem + dot) / n; "x=1" keeps x's and ='s
    // strokes together; ink that cannot be the page's symbols (3 strokes, 4 symbols) is reported as such
    ink = {};
    write(ink, "s", 0, 4, 6);
    qsizetype stem = ink.size();
    ink << QPolygonF{{7, 5}, {7.2, 10}};
    write(ink, "n", 9, 4, 6);
    ink << QPolygonF{{6.8, 2.5}, {7.4, 2.6}};
    QList<QList<int>> al = alignSymbols(ink, {"s", "i", "n"}, {});
    assert(al.size() == 3 && al[1] == QList<int>({int(stem), int(ink.size() - 1)}));
    ink = {};
    write(ink, "x", 0, 4, 6), write(ink, "=", 8, 6, 5), write(ink, "1", 15, 3, 7);
    al = alignSymbols(ink, {"x", "=", "1"}, {});
    assert(al.size() == 3 && al[0].size() >= 2 && al[1].size() == 2);
    assert(alignSymbols({QPolygonF{{0, 0}, {0, 8}}, QPolygonF{{5, 0}, {5, 8}}, QPolygonF{{10, 0}, {10, 8}}}, {"1", "1", "1", "1"}, {}).isEmpty());
    QTemporaryDir tdir;   // personal templates round-trip
    assert(saveTemplates({makeTemplate("x", ink)}, tdir.filePath("p.tpl")) && loadTemplates(tdir.filePath("p.tpl")).size() == 1);

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

    // auto page creation off: ink outside pages makes no page and is not queued
    g = Sheet{};
    g.strokes << line(10, 10, 20, 10) << line(250, 10, 260, 10);
    refresh(g, false);
    assert(g.pages == QSet<QPoint>({{0, 0}}) && g.exportQueue == QList<QPoint>({{0, 0}}));

    // PDF: one page per chosen page (empty ones too), page size from the sheet (landscape kept)
    g = Sheet{};
    g.page = {297, 210};
    g.strokes << line(10, 10, 600, 10) << line(10, 300, 20, 300);   // crosses three pages; one on row 1
    refresh(g);
    assert(g.exportQueue.size() == 4);
    auto pdfOf = [&](const QList<QPoint> &pages) {
        QString path = tmp.filePath("a.pdf");
        assert(exportPdf(g, pages, path));
        QFile pf(path);
        assert(pf.open(QIODevice::ReadOnly));
        return pf.readAll();
    };
    QByteArray pdf = pdfOf(g.exportQueue);
    assert(pdf.startsWith("%PDF") && pdf.contains("/Count 4"));
    assert(pdf.contains("/MediaBox [0 0 842") && !pdf.contains("/MediaBox [0 0 595"));   // 297 x 210 mm in points
    assert(pdfOf({{1, 0}, {7, 7}}).contains("/Count 2"));                                 // subset + a blank page
    assert(!exportPdf(g, {}, tmp.filePath("b.pdf")));                                      // nothing to export

    // images: stored once by content, every page under the rectangle is inked, locked ones are not selectable
    imageDir = tmp.filePath("images");
    QImage red(40, 20, QImage::Format_RGB32);
    red.fill(Qt::red);
    QString id = storeImage(red);
    assert(!id.isEmpty() && storeImage(red) == id && imageOf(id).size() == QSize(40, 20) && imageOf("nope").isNull());
    g = Sheet{};
    Stroke pic{{{200, 10, 1, 0}, {230, 300, 1, 0}}, Qt::black, 0, id};
    assert(inkCells(g, pic) == QSet<QPoint>({{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
    assert(!hits(pic, {210, 100}, 1));                                         // the eraser skips images
    Stroke bg{{{0, 0, 1, 0}, {210, 297, 1, 0}}, Qt::black, 0, id, true};
    assert(inkCells(g, bg) == QSet<QPoint>({{0, 0}}));                          // exactly one page
    assert(strokesInside({bg, pic}, QPolygonF(QRectF(-1, -1, 400, 400))) == QSet<int>({1}));
    g.strokes = {bg};
    assert(saveSheet(g, tmp.filePath("i.tsheet")) && loadSheet(ld, tmp.filePath("i.tsheet")) && ld.strokes[0].img == id && ld.strokes[0].locked);
    assert(snapshot(g.strokes, QRectF(0, 0, 10, 5)).pixelColor(30, 15) == QColor(Qt::red));
    // the highlighter goes under the ink even when drawn later
    QList<Stroke> marked{line(0, 5, 20, 5), {{{10, 0, 1, 0}, {10, 10, 1, 0}}, QColor(255, 255, 0, 100), 5}};
    assert(snapshot(marked, QRectF(0, 0, 20, 10)).pixelColor(60, 30) == QColor(Qt::black));

    // version 1 sheets (no images) still load
    {
        QFile v1(tmp.filePath("v1.tsheet"));
        assert(v1.open(QIODevice::WriteOnly));
        QDataStream d(&v1);
        d.setVersion(QDataStream::Qt_6_0);
        d << qint32(1) << quint32(1) << st.pts << st.color << st.width << sh.pages << sh.page << sh.exportQueue;
    }
    assert(loadSheet(ld, tmp.filePath("v1.tsheet")) && ld.strokes.size() == 1 && ld.strokes[0].pts[1].x == 10 &&
           ld.strokes[0].img.isEmpty() && ld.pages == sh.pages && ld.exportQueue == sh.exportQueue);
    // version 2 sheets (no math) still load
    {
        QFile v2(tmp.filePath("v2.tsheet"));
        assert(v2.open(QIODevice::WriteOnly));
        QDataStream d(&v2);
        d.setVersion(QDataStream::Qt_6_0);
        d << qint32(2) << quint32(1) << bg.pts << bg.color << bg.width << bg.img << bg.locked << sh.pages << sh.page << sh.exportQueue;
    }
    assert(loadSheet(ld, tmp.filePath("v2.tsheet")) && ld.strokes.size() == 1 && ld.strokes[0].img == id &&
           ld.strokes[0].locked && ld.strokes[0].math.isEmpty() && ld.pages == sh.pages);

    // math objects: a box like a picture (pages, never erased), saved, drawn in their own color
    Stroke eqObj{{{10, 10, 1, 0}, {50, 30, 1, 0}}, Qt::blue, 0, {}, false, "eq\ny = x^2", 5};
    assert(eqObj.boxed() && !hits(eqObj, {30, 20}, 1) && inkCells(g, eqObj) == QSet<QPoint>({{0, 0}}));
    g.strokes = {eqObj, {{{0, 40, 1, 0}, {100, 100, 1, 0}}, Qt::black, 0, {}, false, "plot -10 10 -10 10\ny = x"}};
    assert(saveSheet(g, tmp.filePath("m.tsheet")) && loadSheet(ld, tmp.filePath("m.tsheet")) && ld.strokes[0].math == eqObj.math && ld.strokes[0].link == 5 &&
           ld.strokes[1].math.startsWith("plot"));
    QImage ms = snapshot(g.strokes, QRectF(0, 0, 100, 100));
    bool blue = false;
    for (int y = 10 * 6; y < 30 * 6; ++y)
        for (int x = 10 * 6; x < 50 * 6; ++x) blue |= ms.pixelColor(x, y).blue() > ms.pixelColor(x, y).red() + 100;
    assert(blue);
    std::puts("selftest ok");
}
