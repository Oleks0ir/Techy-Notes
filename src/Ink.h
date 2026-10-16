#pragma once
#include <QDataStream>
#include <QList>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <vector>

// Handwritten math -> dock text. Strokes are grouped into symbols, each symbol is classified by a small neural
// network (trained on HWRT by tools/hwrt2tpl, its last layer retrained on the user's own samples), and the
// layout (powers, fractions, roots, ∫/∑/lim limits) comes from geometry.

// Symbol classifier: features -> ReLU hidden layer -> softmax over classes. Built in as :/inkmlp.bin.
struct Mlp {
    int d = 0, h = 0;
    QStringList classes;
    QList<float> w1, b1, w2, b2;   // w1: h x d, w2: classes x h, row-major
};
std::vector<float> inkFeatures(const QList<QPolygonF> &strokes);   // any units; Mlp::d values
QList<QPolygonF> inkOf(const QList<QPolygonF> &ink, const QList<int> &strokes);   // those strokes of ink
std::vector<float> hiddenOf(const Mlp &m, const std::vector<float> &features);   // the ReLU layer
std::vector<float> outputOf(const Mlp &m, const std::vector<float> &hidden);     // softmax over the classes
std::vector<float> mlpProbs(const Mlp &m, const std::vector<float> &features);   // outputOf(hiddenOf(...))
Mlp loadMlp(const QString &path);   // d == 0: missing or unreadable
bool saveMlp(const Mlp &m, const QString &path);

// One written sample of a symbol: its strokes, scaled into the unit square (aspect kept, top-left at 0,0).
// sym is what the dock text gets ("x", "2", "α", "∫", "√", "(", ...).
struct Template { QString sym; QList<QPolygonF> strokes; };
inline QDataStream &operator<<(QDataStream &d, const Template &t) { return d << t.sym << t.strokes; }
inline QDataStream &operator>>(QDataStream &d, Template &t) { return d >> t.sym >> t.strokes; }
Template makeTemplate(const QString &sym, const QList<QPolygonF> &strokes);   // any units

// Template files: "TPL" + version + QList<Template>. Seed set: :/symbols.tpl (HWRT, ODbL); personal: notebook root.
inline constexpr qint32 kTemplateVersion = 1;
QList<Template> loadTemplates(const QString &path);                  // empty: missing or unreadable
bool saveTemplates(const QList<Template> &t, const QString &path);   // atomic

struct InkSymbol {
    QList<int> strokes;   // indices into the ink passed to recognizeSymbols
    QRectF box;           // ink units
    QStringList cands;    // best match first; cands[0] is used for the text
    QList<double> probs;  // the classifier's probability of each candidate
};
// ink: strokes in writing order (any units, y down). Dots (tiny strokes) become "." for the layout to place.
// personal: the user's own samples; the classifier's last layer is retrained on them (cached until they change).
// Runs of letters that are likely enough as a function name (sin, lim, ...) are read as that name.
QList<InkSymbol> recognizeSymbols(const QList<QPolygonF> &ink, const QList<Template> &personal);
// The given strokes of ink classified as one symbol (the dock's Split: each stroke on its own)
InkSymbol symbolOf(const QList<QPolygonF> &ink, const QList<int> &strokes, const QList<Template> &personal);
struct InkText {
    QString text;               // one expression per line, in the dock's syntax
    QList<QList<int>> lines;    // symbol indices of each line (for marking a line's ink later)
};
InkText inkLayout(const QList<InkSymbol> &symbols);
// Calibration: the classifier's best guess for one written symbol, and its probability for `expected`
// (0 if it does not know that symbol yet).
struct Guess { QString sym; double certainty; };
Guess guessSymbol(const QList<QPolygonF> &strokes, const QString &expected, const QList<Template> &personal);
// Calibration: the ink split into exactly the expected symbols, in writing order (the same rules as recognition,
// each part scored by its probability of being the symbol it should be); dots go to their nearest symbol.
// Stroke indices per expected symbol; empty: it cannot be split that way (too few strokes, gaps inside a symbol).
QList<QList<int>> alignSymbols(const QList<QPolygonF> &ink, const QStringList &expected, const QList<Template> &personal);
