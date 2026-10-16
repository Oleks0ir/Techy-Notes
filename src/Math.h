#pragma once
#include <QList>
#include <QRectF>
#include <QString>
#include <functional>

class QPainter;

// Parsed expression, Desmos-style input ("y = x^2 - 2x", "sin(x)/x", "sqrt(2)pi").
// k: 'n' number, 'v' variable/constant, '+' '-' '*' '/' '^' '=' (a[0] op a[1]), '~' negation,
// '(' parentheses typed by the user, 'f' function s(a[0]).
struct MathNode { char k; QString s; QList<MathNode> a; };

MathNode parseMath(const QString &text, QString *err = nullptr);   // on error: {'n', text} and *err set
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
void drawMath(QPainter &p, const QString &math, const QRectF &r);
QSizeF mathSize(const QString &math);   // "eq": the size at font size 1 (aspect for fitting); empty: nothing to show
