#include "Math.h"
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

namespace {
// Function names, longer ones before their prefixes; any other letter is a one-letter variable (xy = x·y).
const QStringList kNames{"sqrt", "asin", "acos", "atan", "sinh", "cosh", "tanh", "sin", "cos", "tan",
                         "exp", "abs", "ln", "log", "pi", "inf", "int", "sum", "lim"};
// Greek letters typed by name become the letters (one-letter variables); longer names first where one contains
// another (Theta, theta, zeta, beta before eta; epsilon before psi). ponytail: no "xi" (reads as x·i), no omicron/upsilon
const std::pair<const char *, const char16_t *> kGreek[] = {
    {"Gamma", u"Γ"}, {"Delta", u"Δ"}, {"Theta", u"Θ"}, {"Lambda", u"Λ"}, {"Sigma", u"Σ"}, {"Phi", u"Φ"},
    {"Psi", u"Ψ"}, {"Omega", u"Ω"}, {"alpha", u"α"}, {"theta", u"θ"}, {"zeta", u"ζ"}, {"beta", u"β"},
    {"eta", u"η"}, {"gamma", u"γ"}, {"delta", u"δ"}, {"epsilon", u"ε"}, {"iota", u"ι"}, {"kappa", u"κ"},
    {"lambda", u"λ"}, {"mu", u"μ"}, {"nu", u"ν"}, {"rho", u"ρ"}, {"sigma", u"σ"}, {"tau", u"τ"}, {"phi", u"φ"},
    {"chi", u"χ"}, {"psi", u"ψ"}, {"omega", u"ω"}};

// Recursive descent: equation = expr [('=' | '->') expr]; expr = term {±term}; term = unary {(*|/|implicit) unary};
// unary = '-' unary | power; power = atom ['^' unary]; atom = number | name['(' equation ')'] | '(' equation ')'
// | int[_lo][^hi] expr d<var> | sum[_lo][^hi] term | lim_lo term | d/d<var> term.
struct Parser {
    QString s;
    int i = 0;
    bool bad = false;
    int inInt = 0;   // inside an integrand: "dx" ends it instead of multiplying

    QChar peek() {
        while (i < s.size() && s[i].isSpace()) ++i;
        return i < s.size() ? s[i] : QChar();
    }
    bool at(const char *t) { return peek(), QStringView(s).mid(i).startsWith(QLatin1String(t)); }
    bool eat(char c) { return peek() == QLatin1Char(c) ? (++i, true) : false; }
    bool atDiff() { return at("d") && i + 1 < s.size() && s[i + 1].isLetter(); }   // "dx" in an integral
    static MathNode bin(char k, MathNode a, MathNode b) { return {k, {}, {std::move(a), std::move(b)}}; }
    MathNode equation() {
        MathNode a = expr();
        if (eat('=')) return bin('=', a, expr());
        if (at("->")) return i += 2, bin('>', a, expr());
        return a;
    }
    MathNode expr() {
        for (MathNode a = term();;) {
            if (eat('+')) a = bin('+', a, term());
            else if (!at("->") && eat('-')) a = bin('-', a, term());
            else return a;
        }
    }
    MathNode term() {
        for (MathNode a = unary();;) {
            QChar c = peek();
            if (eat('*')) a = bin('*', a, unary());
            else if (eat('/')) a = bin('/', a, unary());
            else if (inInt && atDiff()) return a;
            else if (c.isLetterOrNumber() || c == '(' || c == '.') a = bin('*', a, power());   // 2x, x(x+1)
            else return a;
        }
    }
    MathNode unary() { return eat('-') ? MathNode{'~', {}, {unary()}} : power(); }
    MathNode power() { MathNode b = atom(); return eat('^') ? bin('^', b, unary()) : b; }
    MathNode inner() { MathNode e = equation(); if (!eat(')')) bad = true; return e; }
    MathNode limit() { return eat('-') ? MathNode{'~', {}, {atom()}} : atom(); }   // int_0^1: 0 and 1, not 0^1
    void limits(MathNode &lo, MathNode &hi) {   // _lo and ^hi, either order
        for (int k = 0; k < 2; ++k) {
            if (eat('_')) lo = limit();
            else if (eat('^')) hi = limit();
        }
    }
    MathNode atom() {
        QChar c = peek();
        if (c.isDigit() || c == '.') {
            int j = i;
            while (i < s.size() && (s[i].isDigit() || s[i] == '.')) ++i;
            return {'n', s.mid(j, i - j)};
        }
        if (eat('(')) return {'(', {}, {inner()}};
        if (eat('|')) {   // |x| = abs(x); ponytail: no nested bars without parentheses (||x|-1| fails)
            MathNode e = expr();
            if (!eat('|')) bad = true;
            return {'f', "abs", {e}};
        }
        if (!c.isLetter()) return bad = true, MathNode{'n', "?"};
        if (at("d/d") && i + 3 < s.size() && s[i + 3].isLetter()) {   // d/dx f
            QString v = s.mid(i + 3, 1);
            i += 4;
            return {'D', v, {term()}};
        }
        for (const QString &f : kNames)
            if (QStringView(s).mid(i).startsWith(f)) {
                i += f.size();
                if (f == "pi" || f == "inf") return {'v', f};
                MathNode lo{'n', ""}, hi{'n', ""};
                if (f == "int") {   // a: body, lower, upper; s: the variable of "dx" (empty: none written)
                    limits(lo, hi);
                    ++inInt;
                    MathNode body = expr();
                    --inInt;
                    QString v;
                    if (atDiff()) v = s.mid(i + 1, 1), i += 2;
                    return {'I', v, {body, lo, hi}};
                }
                if (f == "sum" || f == "lim") {
                    limits(lo, hi);
                    return {f == "sum" ? 'S' : 'L', {}, {term(), lo, hi}};
                }
                return {'f', f, {eat('(') ? inner() : power()}};   // sin(x) or sin x
            }
        QString v(s[i++]);
        if (i < s.size() && s[i] == '(') return ++i, MathNode{'c', v, {inner()}};   // g(x): a call if g is defined, else g·(x)
        return {'v', v};
    }
};

const MathNode &bare(const MathNode &n) { return n.k == '(' ? n.a[0] : n; }   // fraction parts, exponents
bool leadsWithNumber(const MathNode &n) {
    return n.k == 'n' || n.k == '~' || (QByteArray("+-*/^=").contains(n.k) && leadsWithNumber(n.a[0]));
}

constexpr int kPx = 100;   // font pixel size; the painter scales it to s
QFont mathFont(bool italic) {
    QFont f(QStringList{"Cambria", "Times New Roman"});
    f.setStyleHint(QFont::Serif);
    f.setPixelSize(kPx);
    f.setItalic(italic);
    return f;
}
// Fixed ascent/descent (0.8 s / 0.25 s) instead of the font's, so layout is the same with every font.
// ky stretches the glyph vertically (tall parentheses), keeping the baseline at y.
MathBox text(QPainter *p, const QString &t, double x, double y, double s, bool italic = false, double ky = 1) {
    QFont f = mathFont(italic);
    if (p) {
        p->save();
        p->translate(x, y);
        p->scale(s / kPx, ky * s / kPx);
        p->setFont(f);
        p->drawText(QPointF(), t);
        p->restore();
    }
    return {QFontMetricsF(f).horizontalAdvance(t) * s / kPx, 0.8 * s * ky, 0.25 * s * ky};
}

double gridStep(double span) {   // 1, 2 or 5 · 10^k, at most ~8 lines
    double raw = span / 8, m = std::pow(10, std::floor(std::log10(raw)));
    for (double k : {1, 2, 5})
        if (k * m >= raw) return k * m;
    return 10 * m;
}
QString num(double v) { return QString::number(v, 'g', 6).replace('-', u'−'); }
bool isY(const MathNode &n) { return n.k == 'v' && n.s == "y"; }
}   // namespace

MathNode parseMath(const QString &text, QString *err) {
    QString t = text;   // what a touch keyboard or a recognizer types
    for (auto [from, to] : {std::pair{u"−", "-"}, {u"–", "-"}, {u"×", "*"}, {u"·", "*"}, {u"⋅", "*"},
                            {u"÷", "/"}, {u"π", "pi"}, {u"√", "sqrt"}, {u"²", "^2"}, {u"³", "^3"},
                            {u"∫", "int"}, {u"∑", "sum"}, {u"→", "->"}, {u"∞", "inf"}})
        t.replace(QString::fromUtf16(from), to);
    for (auto [name, letter] : kGreek) t.replace(name, QString::fromUtf16(letter));
    Parser ps{t};
    MathNode n = ps.equation();
    if (!ps.bad && ps.peek().isNull()) return n;
    if (err) *err = QString("cannot read from position %1").arg(ps.i + 1);
    return {'n', text};
}

QString exprtkOf(const MathNode &n) {
    auto a = [&](int j) { return exprtkOf(n.a[j]); };
    switch (n.k) {
    case 'n': return n.s;
    case 'v': return n.s == "e" ? "2.718281828459045" : n.s;
    case '~': return "(-" + a(0) + ")";
    case '(': return a(0);
    case 'f': return (n.s == "ln" ? "log" : n.s == "log" ? "log10" : n.s) + "(" + a(0) + ")";
    case 'c': return "(" + exprtkOf({'v', n.s}) + "*(" + a(0) + "))";   // not expanded by curveOf: a product
    case 'I': case 'S': case 'L': case 'D': return "?";   // ponytail: not evaluated (no plots of integrals/sums/limits/derivatives)
    default: return "(" + a(0) + QChar(n.k) + a(1) + ")";   // '=' becomes ExprTk's comparison; plots never pass it
    }
}

MathBox typeset(QPainter *p, const MathNode &n, double x, double y, double s) {
    // l c r with l and r stretched to c's height
    auto fence = [&](const QString &l, const QString &r, const MathNode &c, double dx) -> MathBox {
        MathBox m = typeset(nullptr, c, 0, 0, s);
        double ky = std::max(1.0, (m.up + m.down) / (1.05 * s)), yb = y + std::max(m.down, 0.25 * s) - 0.25 * s * ky;
        MathBox a = text(p, l, x + dx, yb, s, false, ky);
        MathBox b = typeset(p, c, x + dx + a.w, y, s);
        MathBox e = text(p, r, x + dx + a.w + b.w, yb, s, false, ky);
        return {a.w + b.w + e.w, std::max(b.up, a.up + y - yb), std::max(b.down, yb - y + a.down)};
    };
    switch (n.k) {
    case '+': case '-': case '=': case '>': case '*': {
        QString op = n.k == '+' ? " + " : n.k == '-' ? " − " : n.k == '=' ? " = " : n.k == '>' ? " → "
                   : leadsWithNumber(n.a[1]) ? "·" : n.a[1].k == 'f' ? " " : "";   // 2·3, 2x, x sin(x)
        MathBox a = typeset(p, n.a[0], x, y, s), o = text(p, op, x + a.w, y, s), b = typeset(p, n.a[1], x + a.w + o.w, y, s);
        return {a.w + o.w + b.w, std::max({a.up, o.up, b.up}), std::max({a.down, o.down, b.down})};
    }
    case '~': {
        MathBox m = text(p, "−", x, y, s), c = typeset(p, n.a[0], x + m.w, y, s);
        return {m.w + c.w, std::max(m.up, c.up), std::max(m.down, c.down)};
    }
    case '/': {
        const MathNode &t = bare(n.a[0]), &d = bare(n.a[1]);
        MathBox mt = typeset(nullptr, t, 0, 0, s), md = typeset(nullptr, d, 0, 0, s);
        double w = std::max(mt.w, md.w) + 0.3 * s, ax = y - 0.27 * s, g = 0.12 * s;   // ax: bar height (math axis)
        if (p) {
            p->fillRect(QRectF(x + 0.08 * s, ax - 0.03 * s, w - 0.16 * s, 0.06 * s), p->pen().color());
            typeset(p, t, x + (w - mt.w) / 2, ax - g - mt.down, s);
            typeset(p, d, x + (w - md.w) / 2, ax + g + md.up, s);
        }
        return {w, 0.27 * s + g + mt.down + mt.up, g + md.up + md.down - 0.27 * s};
    }
    case '^': {
        const MathNode &ex = bare(n.a[1]);
        MathBox b = typeset(p, n.a[0], x, y, s), e = typeset(nullptr, ex, 0, 0, 0.7 * s);
        double raise = std::max(0.45 * s, b.up - 0.55 * e.up);
        typeset(p, ex, x + b.w + 0.03 * s, y - raise, 0.7 * s);
        return {b.w + 0.03 * s + e.w, std::max(b.up, raise + e.up), std::max(b.down, e.down - raise)};
    }
    case '(': return fence("(", ")", n.a[0], 0);
    case 'c': {
        MathBox f = typeset(p, {'v', n.s}, x, y, s), c = fence("(", ")", n.a[0], f.w);
        return {f.w + c.w, std::max(f.up, c.up), std::max(f.down, c.down)};
    }
    case 'f': {
        if (n.s == "abs") return fence("|", "|", n.a[0], 0);
        if (n.s == "sqrt") {   // radical sign + bar over the argument
            MathBox c = typeset(nullptr, n.a[0], 0, 0, s);
            double top = y - c.up - 0.1 * s, bot = y + std::max(c.down, 0.1 * s), rw = 0.5 * s;
            if (p) {
                p->save();
                p->setPen(QPen(p->pen().color(), 0.06 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->drawPolyline(QPolygonF{{x + 0.03 * s, y - 0.3 * s}, {x + 0.14 * s, y - 0.36 * s}, {x + 0.28 * s, bot},
                                          {x + rw, top}, {x + rw + c.w + 0.1 * s, top}});
                p->restore();
                typeset(p, n.a[0], x + rw + 0.05 * s, y, s);
            }
            return {rw + c.w + 0.15 * s, c.up + 0.13 * s, bot - y};
        }
        MathBox f = text(p, n.s, x, y, s), c = fence("(", ")", n.a[0], f.w);
        return {f.w + c.w, std::max(f.up, c.up), std::max(f.down, c.down)};
    }
    case 'I': {   // ∫ stretched around the body, limits small at its top and bottom right, then "dx"
        const MathNode &lo = bare(n.a[1]), &hi = bare(n.a[2]);
        MathBox b = typeset(nullptr, n.a[0], 0, 0, s), ml = typeset(nullptr, lo, 0, 0, 0.6 * s), mh = typeset(nullptr, hi, 0, 0, 0.6 * s);
        double ky = std::max(2.0 * s, b.up + b.down + 0.3 * s) / (1.05 * s), yb = y - 0.27 * s + 0.275 * s * ky;
        MathBox g = text(p, "∫", x, yb, s, false, ky);
        double top = yb - g.up, bot = yb + g.down, lw = std::max(mh.w, ml.w - 0.2 * s);
        typeset(p, hi, x + g.w, top + mh.up, 0.6 * s);
        typeset(p, lo, x + g.w - 0.2 * s, bot - ml.down, 0.6 * s);
        double bx = x + g.w + lw + 0.1 * s;
        typeset(p, n.a[0], bx, y, s);
        double w = bx - x + b.w;
        if (!n.s.isEmpty()) w += text(p, " d", x + w, y, s).w, w += text(p, n.s, x + w, y, s, true).w;
        return {w, std::max(y - top, b.up), std::max(bot - y, b.down)};
    }
    case 'S': case 'L': {   // ∑ or lim, limits centred under (and over) it, then the body
        const MathNode &lo = bare(n.a[1]), &hi = bare(n.a[2]);
        double gs = n.k == 'S' ? 1.4 * s : s, yb = n.k == 'S' ? y + 0.2 * s : y;
        QString sign = n.k == 'S' ? "∑" : "lim";
        MathBox g = text(nullptr, sign, 0, 0, gs), ml = typeset(nullptr, lo, 0, 0, 0.6 * s), mh = typeset(nullptr, hi, 0, 0, 0.6 * s);
        double w = std::max({g.w, ml.w, mh.w}), hiY = yb - 0.75 * gs - 0.05 * s - mh.down, loY = yb + 0.25 * gs + ml.up;
        if (n.k == 'L') loY = y + 0.3 * s + ml.up;
        text(p, sign, x + (w - g.w) / 2, yb, gs);
        typeset(p, hi, x + (w - mh.w) / 2, hiY, 0.6 * s);
        typeset(p, lo, x + (w - ml.w) / 2, loY, 0.6 * s);
        MathBox b = typeset(p, n.a[0], x + w + 0.15 * s, y, s);
        double up = mh.w > 0 ? y - hiY + mh.up : y - yb + 0.75 * gs;
        return {w + 0.15 * s + b.w, std::max(up, b.up), std::max(ml.w > 0 ? loY + ml.down - y : yb - y + 0.2 * s, b.down)};
    }
    case 'D': {   // d/dx as a fraction, then the body
        MathNode fr{'/', {}, {{'n', "d"}, {'*', {}, {{'n', "d"}, {'v', n.s}}}}};
        MathBox f = typeset(p, fr, x, y, s), b = typeset(p, n.a[0], x + f.w + 0.05 * s, y, s);
        return {f.w + 0.05 * s + b.w, std::max(f.up, b.up), std::max(f.down, b.down)};
    }
    case 'v': {   // pi and ∞ upright, Latin and lower-case Greek italic, upper-case Greek upright
        if (n.s == "pi" || n.s == "inf") return text(p, n.s == "pi" ? "π" : "∞", x, y, s);
        return text(p, n.s, x, y, s, !(n.s[0].isUpper() && n.s[0].unicode() >= 0x391));
    }
    default: return text(p, n.s, x, y, s);
    }
}

namespace {
using Defs = QHash<QString, std::pair<QString, MathNode>>;   // f -> (parameter, body) of "f(x) = body"
bool isDef(const MathNode &n) { return n.k == '=' && n.a[0].k == 'c' && n.a[0].a[0].k == 'v'; }
MathNode substitute(const MathNode &n, const QString &var, const MathNode &by) {
    if (n.k == 'v' && n.s == var) return {'(', {}, {by}};
    MathNode out{n.k, n.s, {}};
    for (const MathNode &c : n.a) out.a << substitute(c, var, by);
    return out;
}
MathNode expand(const MathNode &n, const Defs &defs, int depth = 0) {   // calls of defined functions -> their bodies
    MathNode out{n.k, n.s, {}};
    for (const MathNode &c : n.a) out.a << expand(c, defs, depth);
    auto d = defs.find(n.s);
    if (n.k != 'c' || d == defs.end()) return out;
    if (depth > 8) return {0, {}, {}};   // f(x) = f(x): no curve
    return expand(substitute(d->second, d->first, out.a[0]), defs, depth + 1);
}
}   // namespace

MathNode curveOf(const QStringList &lines, int i) {
    Defs defs;
    for (const QString &l : lines)
        if (MathNode n = parseMath(l); isDef(n)) defs[n.a[0].s] = {n.a[0].a[0].s, n.a[1]};
    QString err;
    MathNode n = parseMath(lines.value(i), &err);
    if (!err.isEmpty() || lines.value(i).trimmed().isEmpty()) return {0, {}, {}};
    if (n.k == '=') {
        if (isDef(n)) n = substitute(n.a[1], n.a[0].a[0].s, {'v', "x"});   // g(t) = t^2 is drawn over x too
        else if (isY(n.a[0])) n = n.a[1];
        else if (isY(n.a[1])) n = n.a[0];
        else return {0, {}, {}};
    }
    return expand(n, defs);
}

void drawPlot(QPainter &p, const QStringList &lines, const QRectF &r, const QRectF &v, double u) {
    auto X = [&](double x) { return r.left() + (x - v.left()) / v.width() * r.width(); };
    auto Y = [&](double y) { return r.bottom() - (y - v.top()) / v.height() * r.height(); };
    p.save();
    p.setClipRect(r, Qt::IntersectClip);
    p.setBrush(Qt::NoBrush);
    double sx = gridStep(v.width()), sy = gridStep(v.height()), ts = 9 * u;
    double ax = std::clamp(X(0), r.left(), r.right()), ay = std::clamp(Y(0), r.top(), r.bottom());   // axes stay at the edge
    p.setPen(QPen(QColor(225, 225, 225), 0.5 * u));
    for (double i = std::ceil(v.left() / sx); i * sx <= v.right(); ++i) p.drawLine(QPointF(X(i * sx), r.top()), QPointF(X(i * sx), r.bottom()));
    for (double i = std::ceil(v.top() / sy); i * sy <= v.bottom(); ++i) p.drawLine(QPointF(r.left(), Y(i * sy)), QPointF(r.right(), Y(i * sy)));
    p.setPen(QPen(QColor(90, 90, 90), u));
    p.drawLine(QPointF(ax, r.top()), QPointF(ax, r.bottom()));
    p.drawLine(QPointF(r.left(), ay), QPointF(r.right(), ay));
    for (double i = std::ceil(v.left() / sx); i * sx <= v.right(); ++i) {   // numbers under the x axis (above at the bottom edge)
        if (i == 0) continue;
        QString t = num(i * sx);
        double w = text(nullptr, t, 0, 0, ts).w, tx = X(i * sx) - w / 2;
        if (tx < r.left() || tx + w > r.right()) continue;   // would be cut by the edge
        text(&p, t, tx, ay + 1.1 * ts > r.bottom() ? ay - 0.4 * ts : ay + 1.1 * ts, ts);
    }
    for (double i = std::ceil(v.top() / sy); i * sy <= v.bottom(); ++i) {   // numbers left of the y axis (right at the left edge)
        if (i == 0) continue;
        QString t = num(i * sy);
        double w = text(nullptr, t, 0, 0, ts).w, ty = Y(i * sy) + 0.3 * ts;
        if (ty - 0.8 * ts < r.top() || ty > r.bottom()) continue;
        text(&p, t, ax - w - 0.4 * ts < r.left() ? ax + 0.4 * ts : ax - w - 0.4 * ts, ty, ts);
    }

    static const QColor kColors[] = {"#c74440", "#2d70b3", "#388c46", "#6042a6", "#fa7e19", "#000000"};
    const int N = 1000;   // ponytail: fixed sampling; adaptive if fast oscillations look wrong
    double dt = v.width() / N, lim = 10 * r.height();
    for (int li = 0; li < lines.size(); ++li) {   // colors follow the line number, also for skipped lines
        MathNode n = curveOf(lines, li);
        if (!n.k) continue;
        auto curve = compileCurve(exprtkOf(n));
        if (!curve) continue;   // e.g. a variable other than x
        auto f = [&](double t) { return std::clamp(Y(curve(t)), r.top() - lim, r.bottom() + lim); };
        p.setPen(QPen(kColors[li % 6], 2 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF poly;
        for (int k = 0; k <= N; ++k) {
            double t = v.left() + k * dt, py = f(t);
            bool ok = std::isfinite(py), jump = ok && !poly.isEmpty() && std::abs(py - poly.last().y()) > r.height();
            if (jump) {   // steep but continuous if the midpoint lies between (tan's asymptote: it does not)
                double m = f(t - dt / 2);
                jump = !(m > std::min(py, poly.last().y()) && m < std::max(py, poly.last().y()));
            }
            if (!ok || jump) {
                if (poly.size() > 1) p.drawPolyline(poly);
                poly.clear();
            }
            if (ok) poly << QPointF(X(t), py);
        }
        if (poly.size() > 1) p.drawPolyline(poly);
    }
    p.restore();
}

// "eq" objects: the non-empty lines stacked, kGap apart, left-aligned; size at font size 1
static constexpr double kGap = 0.3;
static QList<std::pair<MathNode, MathBox>> eqRows(const QStringList &lines, QSizeF &size) {
    QList<std::pair<MathNode, MathBox>> rows;
    double w = 0, h = 0;
    for (const QString &l : lines) {
        if (l.trimmed().isEmpty()) continue;
        MathNode n = parseMath(l);
        MathBox b = typeset(nullptr, n, 0, 0, 1);
        w = std::max(w, b.w);
        h += b.up + b.down + (rows.isEmpty() ? 0 : kGap);
        rows.append({n, b});
    }
    size = QSizeF(w, h);
    return rows;
}

void drawMath(QPainter &p, const QString &math, const QRectF &r) {
    QStringList lines = math.split('\n');
    QStringList head = lines.takeFirst().split(' ', Qt::SkipEmptyParts);
    if (head.value(0) == "plot" && head.size() == 5) {
        QRectF v(QPointF(head[1].toDouble(), head[3].toDouble()), QPointF(head[2].toDouble(), head[4].toDouble()));
        drawPlot(p, lines, r, v, std::min(r.width(), r.height()) / 240);
        return;
    }
    QSizeF size;
    auto rows = eqRows(lines, size);
    if (size.isEmpty()) return;
    double k = std::min(r.width() / size.width(), r.height() / size.height()), y = r.top();
    for (const auto &[n, b] : rows) {
        typeset(&p, n, r.left(), y + b.up * k, k);
        y += (b.up + b.down + kGap) * k;
    }
}

QSizeF mathSize(const QString &math) {
    QSizeF size;
    eqRows(math.split('\n').mid(1), size);
    return size;
}
