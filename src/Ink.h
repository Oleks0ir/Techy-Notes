#pragma once
#include <QDataStream>
#include <QList>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>

// Handwritten math -> dock text. Strokes are grouped into symbols, each symbol is matched against written
// samples ($P point-cloud matching), and the layout (powers, fractions, roots, ∫/∑/lim limits) comes from geometry.

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
};
// ink: strokes in writing order (any units, y down). Dots (tiny strokes) become "." for the layout to place.
QList<InkSymbol> recognizeSymbols(const QList<QPolygonF> &ink, const QList<Template> &templates);
struct InkText {
    QString text;               // one expression per line, in the dock's syntax
    QList<QList<int>> lines;    // symbol indices of each line (for marking a line's ink later)
};
InkText inkLayout(const QList<InkSymbol> &symbols);
// Calibration: the best match for one written symbol, and how clearly `expected` beats every other symbol:
// d_rival² / (d_expected² + d_rival²) of their $P distances. 0.5: a toss-up; below: another symbol wins;
// 0.9: expected is 3x closer than any rival.
struct Guess { QString sym; double certainty; };
Guess guessSymbol(const QList<QPolygonF> &strokes, const QString &expected, const QList<Template> &templates);
// Calibration: strokes whose horizontal extents overlap form one symbol, left to right (writing in a row,
// symbols apart). Independent of the templates, so it works before any are good.
QList<QList<int>> splitColumns(const QList<QPolygonF> &ink);
