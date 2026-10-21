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
#include <QRandomGenerator>
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
    // structures: boxes, ∏, ∮/∬, nth root, log base, subscripts, prime, ∂, brackets, matrices
    MathNode bx = parseMath("int_(□)^(□) □ dx");
    assert(bx.k == 'I' && bx.a[1].a[0].k == 'p' && bx.a[1].a[0].at == 5 && bx.a[2].a[0].at == 9 && bx.a[0].at == 12 && ex("2□") == "(2*?)");
    assert(parseMath("∏_(k=1)^n k").s == "∏" && parseMath("sum_k k").s == "∑" && parseMath("∮ f ds").k == 'O');
    MathNode dbl = parseMath("∬ f dx dy");
    assert(dbl.k == 'J' && dbl.s == "xy" && parseMath("iint_D f dA").a[1].s == "D");
    assert(ex("sqrt[3](x)") == "((x)^(1/(3)))" && ex("log_2(x)") == "(log(x)/log(2))" && ex("log(x)") == "log10(x)");
    MathNode sub = parseMath("x_1^2 + a_(n+1)");
    assert(sub.a[0].k == '^' && sub.a[0].a[0].k == '_' && sub.a[1].k == '_' && ex("x_1") == "?");
    assert(parseMath("f'(x)").a[0].k == '\'' && parseMath("f′").k == '\'' && ex("f''") == "?");
    MathNode pd = parseMath("∂/∂t u = ∇u");
    assert(pd.a[0].k == 'D' && pd.a[0].s == "∂t" && pd.a[1].a[0].s == "∇" && parseMath("(∂f)/(∂x)").k == '/');
    assert(ex("[x+1]^2") == "((x+1)^2)" && parseMath("{x}").s == "{}");
    MathNode mx = parseMath("[1, 2; 3, 4]");
    assert(mx.k == 'B' && mx.a[0].k == 'G' && mx.a[0].a.size() == 2 && mx.a[0].a[1].a.size() == 2 && ex("(1, 2)") == "?");
    for (const char *bad : {"[x)", "(1,", "sqrt[3(x)"}) { QString err; parseMath(bad, &err); assert(!err.isEmpty()); }
    for (const auto &[name, ts] : kStructures)   // every button template reads, and has a box to fill
        for (const QString &t : ts) { QString err; parseMath(t, &err); assert(err.isEmpty() && t.contains(kBox)); }
    MathBox m22 = typeset(nullptr, parseMath("(a, b; c, d)"), 0, 0, 10), row = typeset(nullptr, parseMath("(a, b)"), 0, 0, 10);
    assert(m22.up + m22.down > 1.8 * (row.up + row.down) && typeset(nullptr, parseMath("x_1"), 0, 0, 10).down > 3);
    assert(typeset(nullptr, parseMath("sqrt[3](x)"), 0, 0, 10).w > typeset(nullptr, parseMath("sqrt(x)"), 0, 0, 10).w);
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
    {   // each character's glyph by its position in the text: □ boxes, "sum" on ∑, lim letter by letter, numbers digit
        // by digit, a typed ∫ and α; nothing for what typesetting adds ("_"); a line that does not parse character by character
        QImage img(400, 300, QImage::Format_RGB32);
        QPainter pp(&img);
        QString body = "sum_(□)^(□) □\n\nlim_(x→0) 12x\n∫ alpha dx\n□+";
        QHash<int, QRectF> g;
        drawMath(pp, "eq\n" + body, img.rect(), &g);
        QList<int> box;
        for (int i = 0; i < body.size(); ++i)
            if (body[i] == kBox) box << i;
        assert(box.size() == 4 && g.contains(box[0]) && g.contains(box[1]) && g.contains(box[2]) && g.contains(box[3]) && g.contains(box[3] + 1) && g[box[3] + 1].left() > g[box[3]].left());
        assert(g[box[1]].bottom() < g[box[0]].top() && g[box[2]].left() > g[box[0]].right());   // lower limit, upper, body
        assert(g.contains(0) && g.contains(2) && g[0] == g[2] && !g.contains(3));
        int l = int(body.indexOf("lim")), ar = int(body.indexOf("→")), n = int(body.indexOf("12"));
        assert(g[l].right() <= g[l + 1].left() + 0.01 && g[l + 1].right() <= g[l + 2].left() + 0.01 && g[ar].top() > g[l].center().y());
        assert(g[n].right() <= g[n + 1].left() + 0.01 && g.contains(n + 2) && !g.contains(n - 1));
        int in = int(body.indexOf("∫")), al = int(body.indexOf("alpha"));
        assert(g.contains(in) && g[in].height() > g[al].height() && g[al].left() > g[in].right() - 1 && !g.contains(al + 1));
        int dx = int(body.indexOf("dx"));
        assert(g.contains(dx) && g.contains(dx + 1) && g[dx + 1].left() >= g[dx].right() - 0.01 && g[dx].left() > g[al].right());
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
    assert(got == "1/2");   // one symbol over one: no parentheses
    ink = {};
    write(ink, "∫", 0, 0, 14), write(ink, "1", 7, -2, 4), write(ink, "0", 5, 12, 4);
    write(ink, "x", 11, 6, 5), write(ink, "d", 17, 4, 7), write(ink, "x", 23, 6, 5);
    MathNode integral = parseMath(got = read(ink));
    std::printf("ink: %s\n", qPrintable(got));
    assert(integral.k == 'I' && integral.s == "x" && integral.a[1].a[0].s == "0");   // limits come in parentheses
    {   // layout from symbol boxes (classification aside): structures own the space around them
        auto sym = [](const char *c, double l, double t, double r, double b, QList<int> strokes = {}) {
            return InkSymbol{strokes, QRectF(QPointF(l, t), QPointF(r, b)), QString::fromUtf8(c).split(' '), {}};
        };
        // lim with n→∞ under it (was a second line), then 1/x
        QList<InkSymbol> s{sym("l 1 |", 0, 0, 1.3, 8), sym("i j", 4, 2, 5.8, 8), sym("m n", 7, 4, 13, 8.5), sym("n h", 0.5, 11, 3, 14),
                           sym("→", 4, 11.5, 9, 13.5), sym("∞ 8", 10, 11.5, 15, 13.5), sym("1", 19, -3, 20.5, 3), sym("-", 17, 4.5, 24, 4.8),
                           sym("x", 18.5, 6.5, 22, 11)};
        InkText t = inkLayout(s);
        std::printf("layout: %s\n", qPrintable(t.text));
        assert(t.text == "lim_(n→∞) 1/x" && t.lines.size() == 1);
        // written with a clear gap under lim and small (they pull the typical height down), the fraction small too
        s = {sym("l", 0, 0, 1.3, 9), sym("i", 4, 3, 5.8, 9), sym("m", 7, 5, 13, 9.5), sym("n", 1, 13.5, 3.5, 16.5),
             sym("→", 4.5, 13.8, 9.5, 16.2), sym("∞", 10.5, 13.5, 15, 16), sym("1", 19, 0.5, 20.2, 4.5), sym("-", 17, 5.5, 23, 5.8),
             sym("x", 18.5, 7, 21.5, 10.5)};
        t = inkLayout(s);
        std::printf("layout: %s\n", qPrintable(t.text.replace('\n', " | ")));
        assert(t.text == "lim_(n→∞) 1/x");
        // its limit starting left of lim and reaching past it
        s = {sym("l", 0, 0, 1.3, 8), sym("i", 4, 2, 5.8, 8), sym("m", 7, 4, 13, 8.5), sym("x", -2, 11, 1, 14), sym("→", 2, 12, 8, 13.5),
             sym("0", 9, 11, 11, 14), sym("1", 15, 1, 16.5, 8), sym("+", 17, 4.5, 20, 7.5), sym("x", 21, 4, 24, 8)};
        assert(inkLayout(s).text == "lim_(x→0) 1+x");
        // ∫'s lower bound written left of it (was put on the line first), upper bound at its top right
        s = {sym("∫", 0, 0, 4, 12), sym("1", 5, -2, 6.2, 2), sym("0", -2, 10, 1, 13.5), sym("x", 8, 5, 11, 8), sym("d", 13, 2, 16, 8), sym("x", 17, 5, 20, 8)};
        assert(inkLayout(s).text == "int_(0)^(1) xdx");
        // ∑ with a wide lower limit and an upper one; a line under it stays a line of its own
        s = {sym("∑", 0, 0, 6, 7), sym("N", 2, -4, 4.5, -1), sym("n", -1, 9, 1, 11), sym("=", 1.5, 9.5, 3.5, 10.5), sym("1", 4, 8.5, 5, 11.5),
             sym("n", 8, 4, 10, 7), sym("y", 0, 20, 3, 25), sym("=", 4, 22, 7, 23), sym("2", 8, 20, 10, 24)};
        t = inkLayout(s);
        std::printf("layout: %s\n", qPrintable(t.text.replace('\n', " | ")));
        assert(t.text == "sum_(n=1)^(N) n | y=2");
        // root index over the hook (written first), subscripts below the baseline, both scripts, log's base, a prime tick,
        // ∫∫ as ∬, ∮; a y's tail is no subscript
        s = {sym("3", -0.5, 1, 1.5, 4), sym("√", 0, 0, 10, 9), sym("x", 4, 4, 7, 8)};
        assert(inkLayout(s).text == "sqrt[3](x)");
        s = {sym("x", 0, 4, 3, 8), sym("1", 3.3, 6.5, 4.3, 9.5), sym("+", 5, 4.5, 8, 7.5), sym("a", 9, 4, 12, 8), sym("n", 12.3, 6.8, 14.3, 9.3)};
        assert(inkLayout(s).text == "x_1+a_n");
        s = {sym("x", 0, 4, 3, 8), sym("1", 3.3, 6.5, 4.3, 9.5), sym("2", 3.3, 1, 5, 4)};
        assert(inkLayout(s).text == "x_1^2");
        s = {sym("l", 0, 0, 1, 8), sym("o", 2, 4, 5, 8), sym("g", 6, 4, 9, 11), sym("2", 9.5, 9, 11, 12), sym("(", 12, 2, 13.5, 10),
             sym("x", 14, 4, 17, 8), sym(")", 17.5, 2, 19, 10)};
        assert(parseMath(inkLayout(s).text).k == 'f' && parseMath(inkLayout(s).text).a.size() == 2);
        s = {sym("f", 0, 0, 3, 10), sym("1 |", 3.3, 0, 3.8, 2.5), sym("(", 4.5, 2, 6, 10), sym("x", 6.5, 4, 9.5, 8), sym(")", 10, 2, 11.5, 10)};
        assert(inkLayout(s).text == "f'(x)");
        s = {sym("∫", 0, 0, 3, 12), sym("∫", 3.5, 0, 6.5, 12), sym("f", 8, 3, 10, 9), sym("d", 11, 3, 13, 8), sym("A", 14, 3, 17, 8)};
        assert(parseMath(inkLayout(s).text).k == 'J');
        s = {sym("∮", 0, 0, 4, 12), sym("f", 6, 3, 8, 9), sym("d", 9, 3, 11, 8), sym("s", 12, 5, 14, 8)};
        assert(parseMath(inkLayout(s).text).k == 'O');
        s = {sym("x", 0, 4, 3, 8), sym("y", 3.5, 4, 6.5, 11), sym("+", 7, 4.5, 10, 7.5), sym("1", 11, 1, 12, 8)};
        assert(inkLayout(s).text == "xy+1");
        // roles set by the user (re-read as a structure) win over the geometry: a bound far off, a power on the line
        s = {sym("∑", 0, 0, 6, 7), sym("n", 8, 4, 10, 7), sym("0", 0, 16, 2, 19)};
        assert(inkLayout(s).text.contains('\n'));
        s[2].role = 'l', s[2].owner = s[0].strokes = {0};
        s[1].strokes = {1}, s[2].strokes = {2};
        assert(inkLayout(s).text == "sum_(0) n");
        s = {sym("x", 0, 4, 3, 8, {0}), sym("2", 3.5, 5, 5, 8, {1}), sym("+", 6, 4.5, 9, 7.5, {2}), sym("1", 10, 1, 11, 8, {3})};
        assert(inkLayout(s).text == "x2+1");
        s[1].role = '^', s[1].owner = {0};
        assert(inkLayout(s).text == "x^2+1");
        // requirements: a tall f with "dx" after it is ∫, an E with symbols over and under it is ∑
        s = {sym("f ∫ S", 0, 0, 4, 14), sym("x", 6, 5, 9, 8), sym("d", 10, 2, 13, 8), sym("x", 14, 5, 17, 8)};
        readStructures(s);
        assert(s[0].cands[0] == "∫" && parseMath(inkLayout(s).text).k == 'I');
        s = {sym("E ∑", 0, 0, 6, 7), sym("k", 2, 9, 4, 12), sym("n", 2, -4, 4, -1), sym("k", 8, 3, 10, 7)};
        readStructures(s);
        assert(s[0].cands[0] == "∑" && inkLayout(s).text == "sum_(k)^(n) k");
        s = {sym("f ∫", 0, 0, 2, 8), sym("x", 3, 3, 5, 8)};   // f(x)-sized, nothing around it: stays f
        readStructures(s);
        assert(s[0].cands[0] == "f");
        // lims from the user's math log: written in one go (one "lim" symbol); the m read as n and far from the i,
        // the arrow read as 1; the arrow as "-" and "l"
        s = {sym("lim", 54.2, 103.6, 64.5, 122.1), sym("n", 53.4, 126.2, 57.3, 133), sym("→", 59.7, 128.2, 68.2, 131),
             sym("∞", 69, 128, 78.4, 133.3), sym("1", 89, 108, 91, 112), sym("-", 87.2, 113.2, 93.1, 114), sym("x", 88, 115, 92, 118.7)};
        assert(inkLayout(s).text == "lim_(n→∞) 1/x");
        s = {sym("l", 52.9, 41.2, 60.4, 56), sym("i", 60.9, 45.9, 70.5, 55.1), sym("n", 52.8, 60.4, 57.2, 66.1), sym("1", 58.9, 61.3, 65.4, 65.2),
             sym("∞", 65.4, 62.7, 73, 67.3), sym("n m", 80.8, 46.7, 85.6, 52.6)};
        readStructures(s);
        assert(s[5].cands[0] == "m" && s[3].cands[0] == "→" && inkLayout(s).text.trimmed() == "lim_(n→∞)");
        s = {sym("l", 311.5, 221.5, 333, 277.5, {0}), sym("i", 341, 245, 344, 271.5, {1}), sym("m", 352.5, 253.5, 383.5, 275, {2}),
             sym("n", 312.5, 300.5, 328.5, 323, {3}), sym("-", 335.5, 309, 351.5, 311, {4}), sym("l", 351, 302.5, 367, 316.5, {5}),
             sym("∞", 373, 301.5, 406.5, 317, {6}), sym("n", 430, 240, 450, 273, {7})};
        readStructures(s);
        assert(s.size() == 7 && s[4].strokes == QList<int>({4, 5}) && inkLayout(s).text == "lim_(n→∞) n");
        assert(symbolText("/", "-") == "-");   // the bar of 1/x

        // each character of the text knows the symbol it shows: lim letter by letter, ∫ as "int", the bar as "/"
        s = {sym("l", 0, 0, 1.3, 8), sym("i", 4, 2, 5.8, 8), sym("m", 7, 4, 13, 8.5), sym("n", 0.5, 11, 3, 14), sym("1", 19, -3, 20.5, 3),
             sym("-", 17, 4.5, 24, 4.8), sym("x", 18.5, 6.5, 22, 11)};
        t = inkLayout(s);
        assert(t.text == "lim_(n) 1/x" && t.src == QList<int>({0, 1, 2, -1, -1, 3, -1, -1, 4, 5, 6}));
        // typed edits carried over: kept characters keep their symbol, one character swapped for one takes the old one's,
        // a retyped run takes none (strict) or the old ones by position (lenient)
        assert(carryLinks("2x+1", {0, 1, 2, 3}, "2×+1", false) == QList<int>({0, 1, 2, 3}));
        assert(carryLinks("2x+1", {0, 1, 2, 3}, "a2x+1", false) == QList<int>({-1, 0, 1, 2, 3}));
        assert(carryLinks("lim_(n) 1/x", t.src, "int_(n) 1/x", false).mid(0, 3) == QList<int>({-1, -1, -1}));
        assert(carryLinks("lim_(n) 1/x", t.src, "int_(n) 1/x", true).mid(0, 4) == QList<int>({0, 1, 2, -1}));
        assert(carryLinks("ab", {0, 1}, "", false).isEmpty() && carryLinks("", {}, "ab", true) == QList<int>({-1, -1}));
        assert(symbolText("int", "f") == "∫" && symbolText("*", "×") == "×" && symbolText("*", "x") == "*" && symbolText("ab", "a").isEmpty());
        QRandomGenerator rnd(7);   // random test formulas all read, with an operator in each
        for (int n = 0; n < 300; ++n) {
            QString f = randomFormula(rnd), err;
            MathNode m = parseMath(f, &err);
            assert(err.isEmpty() && QByteArray("ILS+").contains(m.k));
        }
        assert(sameMath("(1)/(x)", "1/x") && sameMath("int_(0)^(1) x dx", "∫_0^1 x dx") && !sameMath("x2", "x^2") && sameMath("a\n\nb", "a\nb"));

        // layout learning: powers written low (centre at 0.45 of the height) are read as powers once the log shows it
        QList<MathLogEntry> log;
        for (int n = 0; n < 3; ++n)
            log.append({{}, {sym("x", 0, 4, 4, 8), sym("2", 4.5, 4.2, 6, 7.2), sym("+", 8, 5, 11, 8), sym("z", 12, 4, 15, 8)}, "x2+z", {}, "x^2+z"});
        log.append({{}, {sym("a", 0, 4, 4, 8), sym("b", 5, 4, 9, 8)}, "ab", {}, "ab"});   // and the rest stays as it was
        assert(inkLayout(log[0].symbols).text == "x2+z");
        LayoutKnobs fit = fitLayout(log);
        assert(fit.power < 0.6 && inkLayout(log[0].symbols, fit).text == "x^2+z" && inkLayout(log[3].symbols, fit).text == "ab");
        assert(fitLayout({}) == LayoutKnobs{});
        QList<MathLogEntry> full;   // a full log (300 inserts of 12 symbols) is fitted at startup and after each Insert
        for (int n = 0; n < 75; ++n) full += log;
        for (MathLogEntry &e : full) e.symbols += e.symbols + e.symbols;
        QElapsedTimer fitTime;
        fitTime.start();
        fitLayout(full);
        std::printf("layout fitted on %lld inserts: %lld ms\n", full.size(), fitTime.elapsed());
        QTemporaryDir dir;
        assert(saveMathLog(log, dir.filePath("log")) && loadMathLog(dir.filePath("log")).size() == 4);
        assert(loadMathLog(dir.filePath("log"))[0].symbols[1].box == log[0].symbols[1].box && loadMathLog(dir.filePath("none")).isEmpty());
    }
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
    sh.equations[7] = "state";
    assert(saveSheet(sh, tmp.filePath("a.tsheet")));
    Sheet ld;
    assert(loadSheet(ld, tmp.filePath("a.tsheet")));
    assert(ld.strokes.size() == 3 && ld.strokes[2].pts[1].x == 30 && ld.pages == sh.pages && ld.page == sh.page &&
           ld.exportQueue == sh.exportQueue && ld.equations == sh.equations);
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
    objectKinds["eq"] = {[](QPainter &p, const QString &m, const QRectF &r) { drawMath(p, m, r); }, mathSize};   // as the Math graph plug-in does
    QImage ms = snapshot(g.strokes, QRectF(0, 0, 100, 100));
    bool blue = false;
    for (int y = 10 * 6; y < 30 * 6; ++y)
        for (int x = 10 * 6; x < 50 * 6; ++x) blue |= ms.pixelColor(x, y).blue() > ms.pixelColor(x, y).red() + 100;
    assert(blue);
    // a kind no plug-in registered (it is off or missing): a grey box, the object kept
    g.strokes = {{{{10, 10, 1, 0}, {50, 30, 1, 0}}, Qt::blue, 0, {}, false, "nope\n1"}};
    assert(snapshot(g.strokes, QRectF(0, 0, 100, 100)).pixelColor(12 * 6, 12 * 6) == QColor(0xe0, 0xe0, 0xe0));
    std::puts("selftest ok");
}
