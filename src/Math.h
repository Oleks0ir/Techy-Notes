#pragma once
#include <QHash>
#include <QList>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <functional>

class QPainter;

// Parsed expression, Desmos-style input ("y = x^2 - 2x", "sin(x)/x", "sqrt(2)pi").
// k: 'n' number, 'v' variable/constant, '+' '-' '*' '/' '^' '=' '_' (a[0] op a[1]), '~' negation,
// '(' parentheses typed by the user, 'B' [ ] or { } (s: the pair), 'G' matrix of 'r' rows, 'f' function s(a[0])
// (a[1]: sqrt's index, log's base), ''' prime, 'p' empty box □.
struct MathNode { char k; QString s; QList<MathNode> a; int at = -1; };   // at: where it was written in the text (parser spelling)

MathNode parseMath(const QString &text, QString *err = nullptr);   // on error: {'n', text} and *err set
bool sameMath(const QString &a, const QString &b);   // the same formulas, line by line (grouping parentheses aside)
QString exprtkOf(const MathNode &n);                                // explicit form for ExprTk ("(2*x)")
std::function<double(double)> compileCurve(const QString &exprtk);  // f(x); empty if it does not compile (Eval.cpp)

struct MathBox { double w, up, down; };   // width, ascent and descent from the baseline
// Lays out n with its baseline starting at (x, y), font size s (painter units); p null: only measures.
MathBox typeset(QPainter *p, const MathNode &n, double x, double y, double s);
// What line i plots: "y = …", "… = y", "g(x) = …" or a bare expression, with calls of functions defined in any
// line ("f(x) = x^2", then "y = f(x-1)") expanded. k == 0: nothing to plot.
MathNode curveOf(const QStringList &lines, int i);
// y = f(x) curves (lines that are not are skipped) in r; view: x range left..right, y range top..bottom (y up).
// u: line width unit in painter units (text is 9u high).
void drawPlot(QPainter &p, const QStringList &lines, const QRectF &r, const QRectF &view, double u);

// A math object's text (Stroke::math): a header line, then one expression per line.
// "plot <x0> <x1> <y0> <y1>": the curves in that view; "eq": the expressions typeset, scaled to fit r.
// glyphs: where each character of the text after the header line was drawn (device coordinates), by its position
// (lines joined with '\n'); characters the typesetting adds or leaves out ("(", "^", spaces) have none.
void drawMath(QPainter &p, const QString &math, const QRectF &r, QHash<int, QRectF> *glyphs = nullptr);
QSizeF mathSize(const QString &math);   // "eq": the size at font size 1 (aspect for fitting); empty: nothing to show

inline constexpr QChar kBox(u'□');   // an empty box of a structure, to be filled in
// The dock's structure buttons, Word-style: per group its name and templates (the first is the button's picture).
inline const QList<std::pair<QString, QStringList>> kStructures = {
    {"Fraction", {"(□)/(□)"}},
    {"Script", {"□^(□)", "□_(□)", "□_(□)^(□)"}},
    {"Radical", {"sqrt(□)", "sqrt[□](□)"}},
    {"Integral", {"int_(□)^(□) □ dx", "int □ dx", "iint □ dx dy", "oint □ ds"}},
    {"Large operator", {"sum_(□)^(□) □", "sum_(□) □", "prod_(□)^(□) □"}},
    {"Bracket", {"(□)", "[□]", "{□}", "|□|"}},
    {"Function", {"sin(□)", "cos(□)", "tan(□)", "ln(□)", "log_(□)(□)", "e^(□)"}},
    {"Limit", {"lim_(□->□) □", "lim_(x->0) □", "lim_(x->inf) □"}},
    {"Derivative", {"d/dx □", "(d□)/(d□)", "∂/∂x □", "(∂□)/(∂□)", "□'"}},
    {"Matrix", {"[□, □; □, □]", "[□, □, □; □, □, □; □, □, □]", "(□; □)", "(□; □; □)"}},
};
