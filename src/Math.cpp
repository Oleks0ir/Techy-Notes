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
                         "exp", "abs", "ln", "log", "pi", "inf", "int", "iint", "oint", "sum", "prod", "lim"};
// drawMath's glyph map: typeset puts the rect of the character at position at of the parsed (spelled) line at
// (*glyphSink)[glyphBase + (*glyphOrig)[at]]: its position in the text drawMath was given. ponytail: global, GUI thread only
QHash<int, QRectF> *glyphSink = nullptr;
const QList<int> *glyphOrig = nullptr;
int glyphBase = 0;
void mark(QPainter *p, int at, const QRectF &r) {
    if (p && glyphSink && at >= 0 && at < glyphOrig->size()) glyphSink->insert(glyphBase + (*glyphOrig)[at], p->transform().mapRect(r));
}
// Greek letters typed by name become the letters (one-letter variables); longer names first where one contains
// another (Theta, theta, zeta, beta before eta; epsilon before psi). ponytail: no "xi" (reads as x·i), no omicron/upsilon
const std::pair<const char *, const char16_t *> kGreek[] = {
    {"Gamma", u"Γ"}, {"Delta", u"Δ"}, {"Theta", u"Θ"}, {"Lambda", u"Λ"}, {"Sigma", u"Σ"}, {"Phi", u"Φ"},
    {"Psi", u"Ψ"}, {"Omega", u"Ω"}, {"alpha", u"α"}, {"theta", u"θ"}, {"zeta", u"ζ"}, {"beta", u"β"},
    {"eta", u"η"}, {"gamma", u"γ"}, {"delta", u"δ"}, {"epsilon", u"ε"}, {"iota", u"ι"}, {"kappa", u"κ"},
    {"lambda", u"λ"}, {"mu", u"μ"}, {"nu", u"ν"}, {"rho", u"ρ"}, {"sigma", u"σ"}, {"tau", u"τ"}, {"phi", u"φ"},
    {"chi", u"χ"}, {"psi", u"ψ"}, {"omega", u"ω"}};

// Recursive descent: equation = expr [('=' | '->') expr]; expr = term {±term}; term = unary {(*|/|implicit) unary};
// unary = '-' unary | power; power = atom ['_' sub] {'} ['^' unary]; atom = number | □ | name['(' equation ')']
// | bracket | int[_lo][^hi] expr d<var> | sum[_lo][^hi] term | lim_lo term | d/d<var> term | ∂/∂<var> term;
// bracket = ( [ or { cells ) ] or }, cells separated by ',' (columns) and ';' (rows): more than one is a matrix.
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
    static MathNode bin(char k, MathNode a, MathNode b, int pos = -1) { return {k, {}, {std::move(a), std::move(b)}, pos}; }
    MathNode equation() {
        MathNode a = expr();
        if (int o = (peek(), i); eat('=')) return bin('=', a, expr(), o);
        if (int o = (peek(), i); at("->")) return i += 2, bin('>', a, expr(), o);
        return a;
    }
    MathNode expr() {
        for (MathNode a = term();;) {
            int o = (peek(), i);
            if (eat('+')) a = bin('+', a, term(), o);
            else if (!at("->") && eat('-')) a = bin('-', a, term(), o);
            else return a;
        }
    }
    MathNode term() {
        for (MathNode a = unary();;) {
            QChar c = peek();
            int o = i;
            if (eat('*')) a = bin('*', a, unary(), o);
            else if (eat('/')) a = bin('/', a, unary(), o);
            else if (inInt && atDiff()) return a;
            else if (c.isLetterOrNumber() || QStringView(u"([{.□∂∇").contains(c)) a = bin('*', a, power());   // 2x, x(x+1)
            else return a;
        }
    }
    MathNode unary() {
        int o = (peek(), i);
        return eat('-') ? MathNode{'~', {}, {unary()}, o} : power();
    }
    MathNode power() {
        MathNode b = atom();
        if (eat('_')) b = bin('_', b, limit());   // x_1, a_(n+1)
        while (peek() == u'\'') b = {'\'', {}, {b}, i++};   // f'
        return eat('^') ? bin('^', b, unary()) : b;
    }
    MathNode inner() { MathNode e = equation(); if (!eat(')')) bad = true; return e; }
    MathNode limit() { int o = (peek(), i); return eat('-') ? MathNode{'~', {}, {atom()}, o} : atom(); }   // int_0^1: 0 and 1, not 0^1
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
            return {'n', s.mid(j, i - j), {}, j};
        }
        if (c == kBox) return MathNode{'p', {}, {}, i++};
        for (auto [o, e] : {std::pair{'(', ')'}, {'[', ']'}, {'{', '}'}})
            if (eat(o)) {   // '(' or 'B' ("[]", "{}") around one expression, or around a 'G' of 'r' rows
                QList<MathNode> rows{{'r', {}, {equation()}}};
                while (true) {
                    if (eat(',')) rows.last().a << equation();
                    else if (eat(';')) rows << MathNode{'r', {}, {equation()}};
                    else break;
                }
                if (!eat(e)) bad = true;
                MathNode in = rows.size() == 1 && rows[0].a.size() == 1 ? rows[0].a[0] : MathNode{'G', {}, rows};
                return o == '(' ? MathNode{'(', {}, {in}} : MathNode{'B', QString(o) + e, {in}};
            }
        if (eat('|')) {   // |x| = abs(x); ponytail: no nested bars without parentheses (||x|-1| fails)
            MathNode e = expr();
            if (!eat('|')) bad = true;
            return {'f', "abs", {e}};
        }
        bool partial = QStringView(s).mid(i).startsWith(u"∂/∂");
        if ((partial || at("d/d")) && i + 3 < s.size() && s[i + 3].isLetter()) {   // d/dx f; s: "∂x" for ∂/∂x
            QString v = (partial ? "∂" : "") + s.mid(i + 3, 1);
            i += 4;
            return {'D', v, {term()}};
        }
        if (c == u'∂' || c == u'∇') return MathNode{'v', c, {}, i++};
        if (!c.isLetter()) return bad = true, MathNode{'n', "?"};
        for (const QString &f : kNames)
            if (QStringView(s).mid(i).startsWith(f)) {
                int f0 = i;
                i += f.size();
                if (f == "pi" || f == "inf") return {'v', f, {}, f0};
                MathNode lo{'n', ""}, hi{'n', ""};
                if (f.endsWith("int")) {   // 'I' ∫, 'O' ∮, 'J' ∬; a: body, lower, upper; s: the variables of "dx" ("xy" for ∬ … dx dy)
                    limits(lo, hi);
                    ++inInt;
                    MathNode body = expr();
                    --inInt;
                    QString v;
                    QList<MathNode> ds;   // a[3], a[4]: each "dx", at: where its d was written
                    while (atDiff() && v.size() < (f == "iint" ? 2 : 1)) ds.append({'v', s.mid(i + 1, 1), {}, i}), v += s[i + 1], i += 2;
                    return {f == "int" ? 'I' : f == "oint" ? 'O' : 'J', v, QList<MathNode>{body, lo, hi} + ds, f0};
                }
                if (f == "sum" || f == "prod" || f == "lim") {   // s: the sign
                    limits(lo, hi);
                    return {f == "lim" ? 'L' : 'S', f == "sum" ? "∑" : f == "prod" ? "∏" : "lim", {term(), lo, hi}, f0};
                }
                if (f == "sqrt" && eat('[')) {   // sqrt[3](x): a: radicand, index
                    MathNode n = equation();
                    if (!eat(']')) bad = true;
                    return {'f', f, {eat('(') ? inner() : power(), n}, f0};
                }
                if (f == "log" && eat('_')) {   // log_2(x): a: argument, base
                    MathNode b = limit();
                    return {'f', f, {eat('(') ? inner() : power(), b}, f0};
                }
                return {'f', f, {eat('(') ? inner() : power()}, f0};   // sin(x) or sin x
            }
        int v0 = i;
        QString v(s[i++]);
        if (i < s.size() && s[i] == '(') return ++i, MathNode{'c', v, {inner()}, v0};   // g(x): a call if g is defined, else g·(x)
        return {'v', v, {}, v0};
    }
};

const MathNode &bare(const MathNode &n) { return n.k == '(' && n.a[0].k != 'G' ? n.a[0] : n; }   // fraction parts, exponents (a matrix keeps its brackets)
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

// text in the parser's spelling: what a touch keyboard or a recognizer types, and Greek by name; orig: per character
// of the result, the position in text it came from
static MathNode parseLine(const QString &text, QString *err, QList<int> &orig) {
    static const QList<std::pair<QString, QString>> kSpell = [] {
        QList<std::pair<QString, QString>> l;
        for (auto [from, to] : {std::pair{u"−", "-"}, {u"–", "-"}, {u"×", "*"}, {u"·", "*"}, {u"⋅", "*"},
                                {u"÷", "/"}, {u"π", "pi"}, {u"√", "sqrt"}, {u"²", "^2"}, {u"³", "^3"},
                                {u"∫", "int"}, {u"∮", "oint"}, {u"∬", "iint"}, {u"∑", "sum"}, {u"∏", "prod"}, {u"→", "->"},
                                {u"∞", "inf"}, {u"′", "'"}})
            l.append({QString::fromUtf16(from), to});
        for (auto [name, letter] : kGreek) l.append({name, QString::fromUtf16(letter)});
        return l;
    }();
    QString t;
    orig.clear();
    for (int i = 0; i < text.size();) {
        auto r = std::find_if(kSpell.begin(), kSpell.end(), [&](const auto &p) { return QStringView(text).mid(i).startsWith(p.first); });
        QString piece = r == kSpell.end() ? text.mid(i, 1) : r->second;
        t += piece;
        for (qsizetype k = 0; k < piece.size(); ++k) orig << i;
        i += r == kSpell.end() ? 1 : int(r->first.size());
    }
    orig << int(text.size());   // the end, for an error there
    Parser ps{t};
    MathNode n = ps.equation();
    if (!ps.bad && ps.peek().isNull()) return n;
    if (err) *err = QString("cannot read from position %1").arg(orig.value(ps.i, int(text.size())) + 1);
    orig.clear();   // shown as it was written, each character where it is (a misreading can still be tapped)
    for (int i = 0; i <= text.size(); ++i) orig << i;
    return {'n', text, {}, 0};
}

MathNode parseMath(const QString &text, QString *err) {
    QList<int> orig;
    return parseLine(text, err, orig);
}

bool sameMath(const QString &a, const QString &b) {
    QStringList la = a.split('\n', Qt::SkipEmptyParts), lb = b.split('\n', Qt::SkipEmptyParts);
    if (la.size() != lb.size()) return false;
    std::function<bool(const MathNode &, const MathNode &)> same = [&](const MathNode &m, const MathNode &n) {
        if (m.k == '(' || n.k == '(') return same(m.k == '(' ? m.a[0] : m, n.k == '(' ? n.a[0] : n);   // grouping only
        if (m.k != n.k || m.s != n.s || m.a.size() != n.a.size()) return false;
        for (int k = 0; k < m.a.size(); ++k)
            if (!same(m.a[k], n.a[k])) return false;
        return true;
    };
    for (int i = 0; i < la.size(); ++i) {
        QString ea, eb;
        MathNode x = parseMath(la[i], &ea), y = parseMath(lb[i], &eb);
        if (!ea.isEmpty() || !eb.isEmpty() ? QString(la[i]).remove(' ') != QString(lb[i]).remove(' ') : !same(x, y)) return false;
    }
    return true;
}

QString exprtkOf(const MathNode &n) {
    auto a = [&](int j) { return exprtkOf(n.a[j]); };
    switch (n.k) {
    case 'n': return n.s;
    case 'v': return n.s == "e" ? "2.718281828459045" : n.s;
    case '~': return "(-" + a(0) + ")";
    case '(': case 'B': return a(0);
    case 'f':
        if (n.a.size() > 1) return n.s == "log" ? "(log(" + a(0) + ")/log(" + a(1) + "))" : "((" + a(0) + ")^(1/(" + a(1) + ")))";   // log_b, nth root
        return (n.s == "ln" ? "log" : n.s == "log" ? "log10" : n.s) + "(" + a(0) + ")";
    case 'c': return "(" + exprtkOf({'v', n.s}) + "*(" + a(0) + "))";   // not expanded by curveOf: a product
    case 'I': case 'O': case 'J': case 'S': case 'L': case 'D': return "?";   // ponytail: not evaluated (no plots of integrals/sums/limits/derivatives)
    case '_': case '\'': case 'p': case 'G': return "?";   // x_1, f', an empty box, a matrix: nothing to plot
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
    // the glyph map: characters at..at+t.size()-1 drawn as t at gx (each: one rect per character, else t's rect for all)
    auto marks = [&](int at, const QString &t, double gx, bool each, bool italic = false) {
        if (!p || !glyphSink || at < 0) return;
        for (int k = 0; k < t.size(); ++k) {
            double x0 = each ? text(nullptr, t.left(k), 0, 0, s, italic).w : 0, w = text(nullptr, each ? t.mid(k, 1) : t, 0, 0, s, italic).w;
            mark(p, at + k, QRectF(gx + x0, y - 0.8 * s, std::max(w, 0.3 * s), 1.05 * s));
        }
    };
    switch (n.k) {
    case '+': case '-': case '=': case '>': case '*': {
        QString op = n.k == '+' ? " + " : n.k == '-' ? " − " : n.k == '=' ? " = " : n.k == '>' ? " → "
                   : leadsWithNumber(n.a[1]) ? "·" : n.a[1].k == 'f' ? " " : "";   // 2·3, 2x, x sin(x)
        MathBox a = typeset(p, n.a[0], x, y, s), o = text(p, op, x + a.w, y, s), b = typeset(p, n.a[1], x + a.w + o.w, y, s);
        for (int k = 0; k < (n.k == '>' ? 2 : 1) && n.at >= 0; ++k)   // the operator typed or written ("->" is two characters)
            mark(p, n.at + k, QRectF(x + a.w, y - 0.8 * s, std::max(o.w, 0.3 * s), 1.05 * s));
        return {a.w + o.w + b.w, std::max({a.up, o.up, b.up}), std::max({a.down, o.down, b.down})};
    }
    case '~': {
        MathBox m = text(p, "−", x, y, s), c = typeset(p, n.a[0], x + m.w, y, s);
        marks(n.at, "−", x, false);
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
            mark(p, n.at, QRectF(x, ax - 0.15 * s, w, 0.3 * s));
        }
        return {w, 0.27 * s + g + mt.down + mt.up, g + md.up + md.down - 0.27 * s};
    }
    case '^': case '_': {   // power, subscript, or both stacked (x_1^2)
        bool both = n.k == '^' && n.a[0].k == '_';
        const MathNode *sup = n.k == '^' ? &bare(n.a[1]) : nullptr, *sub = both ? &bare(n.a[0].a[1]) : n.k == '_' ? &bare(n.a[1]) : nullptr;
        MathBox b = typeset(p, both ? n.a[0].a[0] : n.a[0], x, y, s), e{0, 0, 0}, d{0, 0, 0};
        double up = b.up, down = b.down, sx = x + b.w + 0.03 * s;
        if (sup) {
            e = typeset(nullptr, *sup, 0, 0, 0.7 * s);
            double raise = std::max(0.45 * s, b.up - 0.55 * e.up);
            typeset(p, *sup, sx, y - raise, 0.7 * s);
            up = std::max(up, raise + e.up), down = std::max(down, e.down - raise);
        }
        if (sub) {
            d = typeset(nullptr, *sub, 0, 0, 0.7 * s);
            double drop = std::max(0.25 * s, d.up - 0.45 * s);
            typeset(p, *sub, sx, y + drop, 0.7 * s);
            up = std::max(up, d.up - drop), down = std::max(down, drop + d.down);
        }
        return {b.w + 0.03 * s + std::max(e.w, d.w), up, down};
    }
    case '\'': {
        MathBox b = typeset(p, n.a[0], x, y, s), t = text(p, "′", x + b.w, y, s);
        marks(n.at, "′", x + b.w, false);
        return {b.w + t.w, std::max(b.up, t.up), b.down};
    }
    case '(': return fence("(", ")", n.a[0], 0);
    case 'B': return fence(n.s.left(1), n.s.mid(1), n.a[0], 0);
    case 'G': {   // matrix: cells centred in their columns, rows stacked around the math axis; one row: "a, b"
        if (n.a.size() == 1) {
            double w = 0, up = 0, down = 0;
            for (int k = 0; k < n.a[0].a.size(); ++k) {
                if (k) w += text(p, ", ", x + w, y, s).w;
                MathBox c = typeset(p, n.a[0].a[k], x + w, y, s);
                w += c.w, up = std::max(up, c.up), down = std::max(down, c.down);
            }
            return {w, up, down};
        }
        QList<double> cw, ru, rd;
        for (const MathNode &r : n.a) {
            ru << 0, rd << 0;
            for (int k = 0; k < r.a.size(); ++k) {
                MathBox c = typeset(nullptr, r.a[k], 0, 0, s);
                if (k >= cw.size()) cw << 0;
                cw[k] = std::max(cw[k], c.w), ru.last() = std::max(ru.last(), c.up), rd.last() = std::max(rd.last(), c.down);
            }
        }
        double gx = 0.8 * s, gy = 0.25 * s, h = gy * (n.a.size() - 1), w = gx * (cw.size() - 1);
        for (int r = 0; r < n.a.size(); ++r) h += ru[r] + rd[r];
        for (double c : cw) w += c;
        double top = y - 0.27 * s - h / 2, ry = top;
        for (int r = 0; r < n.a.size(); ++r) {
            double cx = x + 0.15 * s;   // a little air inside the brackets
            for (int k = 0; k < cw.size(); ++k) {
                if (k < n.a[r].a.size()) typeset(p, n.a[r].a[k], cx + (cw[k] - typeset(nullptr, n.a[r].a[k], 0, 0, s).w) / 2, ry + ru[r], s);
                cx += cw[k] + gx;
            }
            ry += ru[r] + rd[r] + gy;
        }
        return {w + 0.3 * s, y - top + 0.1 * s, top + h - y + 0.1 * s};
    }
    case 'p': {   // empty box: dotted, digit-sized; reported to the box sink
        QRectF r(x + 0.06 * s, y - 0.7 * s, 0.5 * s, 0.78 * s);
        if (p) {
            p->save();
            p->setPen(QPen(QColor(140, 140, 140), 0.04 * s, Qt::DotLine));
            p->setBrush(Qt::NoBrush);
            p->drawRect(r);
            p->restore();
            mark(p, n.at, r);
        }
        return {0.62 * s, 0.75 * s, 0.1 * s};
    }
    case 'c': {
        MathBox f = typeset(p, {'v', n.s, {}, n.at}, x, y, s), c = fence("(", ")", n.a[0], f.w);
        return {f.w + c.w, std::max(f.up, c.up), std::max(f.down, c.down)};
    }
    case 'f': {
        if (n.s == "abs") return fence("|", "|", n.a[0], 0);
        if (n.s == "sqrt") {   // radical sign + bar over the argument; an index small over the hook
            MathBox c = typeset(nullptr, n.a[0], 0, 0, s), ix{0, 0, 0};
            if (n.a.size() > 1) ix = typeset(nullptr, bare(n.a[1]), 0, 0, 0.55 * s);
            double top = y - c.up - 0.1 * s, bot = y + std::max(c.down, 0.1 * s), rw = 0.5 * s, ox = std::max(0.0, ix.w - 0.2 * s);
            if (p) {
                p->save();
                p->setPen(QPen(p->pen().color(), 0.06 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->drawPolyline(QPolygonF{{x + ox + 0.03 * s, y - 0.3 * s}, {x + ox + 0.14 * s, y - 0.36 * s}, {x + ox + 0.28 * s, bot},
                                          {x + ox + rw, top}, {x + ox + rw + c.w + 0.1 * s, top}});
                p->restore();
                if (n.a.size() > 1) typeset(p, bare(n.a[1]), x, y - 0.42 * s - ix.down, 0.55 * s);
                typeset(p, n.a[0], x + ox + rw + 0.05 * s, y, s);
                for (int k = 0; k < 4; ++k) mark(p, n.at + k, QRectF(x + ox, top, rw, bot - top));   // the radical: "sqrt" or "√"
            }
            return {ox + rw + c.w + 0.15 * s, std::max(c.up + 0.13 * s, 0.42 * s + ix.down + ix.up), bot - y};
        }
        MathBox f = text(p, n.s, x, y, s), b{0, 0, 0};
        marks(n.at, n.s, x, true);
        if (n.a.size() > 1) b = typeset(p, bare(n.a[1]), x + f.w, y + 0.25 * s, 0.7 * s);   // log's base
        MathBox c = fence("(", ")", n.a[0], f.w + b.w);
        return {f.w + b.w + c.w, std::max(f.up, c.up), std::max({f.down, c.down, 0.25 * s + b.down})};
    }
    case 'I': case 'O': case 'J': {   // ∫ (∮, ∬) stretched around the body, limits small at its top and bottom right, then "dx"
        const MathNode &lo = bare(n.a[1]), &hi = bare(n.a[2]);
        MathBox b = typeset(nullptr, n.a[0], 0, 0, s), ml = typeset(nullptr, lo, 0, 0, 0.6 * s), mh = typeset(nullptr, hi, 0, 0, 0.6 * s);
        double ky = std::max(2.0 * s, b.up + b.down + 0.3 * s) / (1.05 * s), yb = y - 0.27 * s + 0.275 * s * ky;
        MathBox g = text(p, n.k == 'I' ? "∫" : n.k == 'O' ? "∮" : "∬", x, yb, s, false, ky);
        for (int k = 0; k < (n.k == 'I' ? 3 : 4); ++k) mark(p, n.at + k, QRectF(x, yb - g.up, g.w, g.up + g.down));   // "int" or "∫"
        double top = yb - g.up, bot = yb + g.down, lw = std::max(mh.w, ml.w - 0.2 * s);
        typeset(p, hi, x + g.w, top + mh.up, 0.6 * s);
        typeset(p, lo, x + g.w - 0.2 * s, bot - ml.down, 0.6 * s);
        double bx = x + g.w + lw + 0.1 * s;
        typeset(p, n.a[0], bx, y, s);
        double w = bx - x + b.w;
        for (int k = 0; k < n.s.size(); ++k) {   // " dx": the d and the variable where they were written (a[3 + k])
            double dw = text(nullptr, "d", 0, 0, s).w;
            w += text(p, " d", x + w, y, s).w;
            MathBox vb = text(p, n.s.mid(k, 1), x + w, y, s, true);
            if (int at = n.a.value(3 + k).at; at >= 0)
                mark(p, at, QRectF(x + w - dw, y - 0.8 * s, dw, 1.05 * s)), mark(p, at + 1, QRectF(x + w, y - 0.8 * s, vb.w, 1.05 * s));
            w += vb.w;
        }
        return {w, std::max(y - top, b.up), std::max(bot - y, b.down)};
    }
    case 'S': case 'L': {   // ∑, ∏ or lim (the sign in s), limits centred under (and over) it, then the body
        const MathNode &lo = bare(n.a[1]), &hi = bare(n.a[2]);
        double gs = n.k == 'S' ? 1.4 * s : s, yb = n.k == 'S' ? y + 0.2 * s : y;
        const QString &sign = n.s;
        MathBox g = text(nullptr, sign, 0, 0, gs), ml = typeset(nullptr, lo, 0, 0, 0.6 * s), mh = typeset(nullptr, hi, 0, 0, 0.6 * s);
        double w = std::max({g.w, ml.w, mh.w}), hiY = yb - 0.75 * gs - 0.05 * s - mh.down, loY = yb + 0.25 * gs + ml.up;
        if (n.k == 'L') loY = y + 0.3 * s + ml.up;
        text(p, sign, x + (w - g.w) / 2, yb, gs);
        for (int k = 0; k < (n.s == "∏" ? 4 : 3); ++k)   // "sum", "prod", "∑", "∏" as one; lim letter by letter
            mark(p, n.at + k, n.k == 'L' ? QRectF(x + (w - g.w) / 2 + text(nullptr, sign.left(k), 0, 0, gs).w, yb - 0.8 * gs, text(nullptr, sign.mid(k, 1), 0, 0, gs).w, 1.05 * gs)
                                         : QRectF(x + (w - g.w) / 2, yb - g.up, g.w, g.up + g.down));
        typeset(p, hi, x + (w - mh.w) / 2, hiY, 0.6 * s);
        typeset(p, lo, x + (w - ml.w) / 2, loY, 0.6 * s);
        MathBox b = typeset(p, n.a[0], x + w + 0.15 * s, y, s);
        double up = mh.w > 0 ? y - hiY + mh.up : y - yb + 0.75 * gs;
        return {w + 0.15 * s + b.w, std::max(up, b.up), std::max(ml.w > 0 ? loY + ml.down - y : yb - y + 0.2 * s, b.down)};
    }
    case 'D': {   // d/dx (∂/∂x) as a fraction, then the body
        QString d = n.s.size() > 1 ? "∂" : "d";
        MathNode fr{'/', {}, {{'n', d}, {'*', {}, {{'n', d}, {'v', n.s.right(1)}}}}};
        MathBox f = typeset(p, fr, x, y, s), b = typeset(p, n.a[0], x + f.w + 0.05 * s, y, s);
        return {f.w + 0.05 * s + b.w, std::max(f.up, b.up), std::max(f.down, b.down)};
    }
    case 'v': {   // pi and ∞ upright, Latin and lower-case Greek italic, upper-case Greek, ∂ and ∇ upright
        bool upright = n.s == "pi" || n.s == "inf" || !n.s[0].isLetter() || (n.s[0].isUpper() && n.s[0].unicode() >= 0x391);
        QString g = n.s == "pi" ? "π" : n.s == "inf" ? "∞" : n.s;
        MathBox b = text(p, g, x, y, s, !upright);
        for (int k = 0; k < n.s.size() && n.at >= 0; ++k) mark(p, n.at + k, QRectF(x, y - 0.8 * s, std::max(b.w, 0.3 * s), 1.05 * s));
        return b;
    }
    default: marks(n.at, n.s, x, true); return text(p, n.s, x, y, s);   // numbers digit by digit
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

// "eq" objects: the non-empty lines stacked, kGap apart, left-aligned; size at font size 1.
// start: where the line starts in the lines joined; orig: the spelling map of its parse (parseLine)
static constexpr double kGap = 0.3;
struct EqRow { MathNode n; MathBox b; int start; QList<int> orig; };
static QList<EqRow> eqRows(const QStringList &lines, QSizeF &size) {
    QList<EqRow> rows;
    double w = 0, h = 0;
    int start = 0;
    for (const QString &l : lines) {
        if (!l.trimmed().isEmpty()) {
            QList<int> orig;
            MathNode n = parseLine(l, nullptr, orig);
            MathBox b = typeset(nullptr, n, 0, 0, 1);
            w = std::max(w, b.w);
            h += b.up + b.down + (rows.isEmpty() ? 0 : kGap);
            rows.append({n, b, start, orig});
        }
        start += int(l.size()) + 1;
    }
    size = QSizeF(w, h);
    return rows;
}

void drawMath(QPainter &p, const QString &math, const QRectF &r, QHash<int, QRectF> *glyphs) {
    QStringList lines = math.split('\n');
    QStringList head = lines.takeFirst().split(' ', Qt::SkipEmptyParts);
    if (glyphs) glyphs->clear();
    if (head.value(0) == "plot" && head.size() == 5) {
        QRectF v(QPointF(head[1].toDouble(), head[3].toDouble()), QPointF(head[2].toDouble(), head[4].toDouble()));
        drawPlot(p, lines, r, v, std::min(r.width(), r.height()) / 240);
        return;
    }
    QSizeF size;
    auto rows = eqRows(lines, size);
    if (size.isEmpty()) return;
    double k = std::min(r.width() / size.width(), r.height() / size.height()), y = r.top();
    glyphSink = glyphs;
    for (const EqRow &row : rows) {
        glyphBase = row.start, glyphOrig = &row.orig;
        typeset(&p, row.n, r.left(), y + row.b.up * k, k);
        y += (row.b.up + row.b.down + kGap) * k;
    }
    glyphSink = nullptr;
}

QSizeF mathSize(const QString &math) {
    QSizeF size;
    eqRows(math.split('\n').mid(1), size);
    return size;
}
