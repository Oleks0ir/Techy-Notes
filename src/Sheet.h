#pragma once
#include <QColor>
#include <QDataStream>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QPolygonF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QtMath>

#include <functional>
#include <utility>

class QPainter;

// Data of the core DLL that its users (the app, plug-ins) share: MSVC imports it explicitly (functions are exported
// by WINDOWS_EXPORT_ALL_SYMBOLS, data has to be imported); MinGW imports it by itself.
#if defined(_MSC_VER) && !defined(TECHY_CORE)
#define TECHY_DATA __declspec(dllimport)
#else
#define TECHY_DATA
#endif

inline constexpr qint32 kSheetVersion = 5;   // bump on any format change. 2: images, highlighter, locked. 3: math. 4: link. 5: equations
inline bool rowMajor(QPoint a, QPoint b) { return std::pair(a.y(), a.x()) < std::pair(b.y(), b.x()); }   // top row first

struct Pt {                                                    // mm, pressure 0..1, ms timestamp
    float x, y, p; qint64 t;
    QPointF pos() const { return {x, y}; }
    bool operator==(const Pt &) const = default;
};
// A line is a 2-point stroke. color alpha < 255: highlighter (drawn under the ink, no pressure).
// img or math set: a box object filling the rectangle pts[0]..pts[1] (an image from the store, or a plug-in's
// object drawn from its text, see objectKinds); selected, moved, undone like ink, never erased.
struct Stroke {
    QList<Pt> pts; QColor color; float width;
    QString img;          // image id (imageOf), empty for ink
    bool locked = false;  // not selectable (imported PDF page backgrounds)
    QString math;         // math object text (Math.h), empty otherwise
    quint32 link = 0;     // nonzero: handwriting and the math object it was recognized as (Keep ink) share it
    bool boxed() const { return !img.isEmpty() || !math.isEmpty(); }
};

// Notebook-wide image store, <notebook root>/.images/<id>.png; set at startup.
// ponytail: images are never deleted from the store; collect unreferenced ones if the folder grows too big
extern TECHY_DATA QString imageDir;
QString storeImage(const QImage &img);   // writes it once (id = content hash); empty: failed
QImage imageOf(const QString &id);       // cached decode; null if missing

// Box objects of plug-ins, by the first word of Stroke::math ("plot", "eq"): how one is drawn into its rectangle
// (painter in mm, pen = its colour) and, for kinds that size to their content, that size at font size 1 (empty:
// nothing to show; no size: 80×60 mm). A kind no plug-in registered draws as a grey box.
// ponytail: "math" names kept for every kind, to spare a format change
struct ObjectKind {
    std::function<void(QPainter &, const QString &, const QRectF &)> draw;
    std::function<QSizeF(const QString &)> size;
};
extern TECHY_DATA QHash<QString, ObjectKind> objectKinds;
inline QString kindOf(const QString &math) { return math.section('\n', 0, 0).section(' ', 0, 0); }

struct Sheet {
    QList<Stroke> strokes;
    QSet<QPoint> pages{QPoint(0, 0)};                           // (col,row) cells, may be negative
    QSizeF page{210, 297};                                      // mm, A4 portrait
    QList<QPoint> exportQueue;
    QHash<quint32, QByteArray> equations;   // per Stroke::link of analysed ink: the math dock's state of that equation (main.cpp)

    QPoint cellAt(QPointF p) const { return {qFloor(p.x() / page.width()), qFloor(p.y() / page.height())}; }
    QRectF pageRect(QPoint c) const {
        return {c.x() * page.width(), c.y() * page.height(), page.width(), page.height()};
    }
};

bool hits(const Stroke &s, QPointF p, float radius);   // p within radius (mm) of the stroke's ink
QPointF snap15(QPointF a, QPointF b);                  // b rotated around a to the nearest 15° step
QRectF bounds(const Stroke &s);                        // ink bounds incl. width; a box object's rectangle (width 0)
QSet<int> strokesInside(const QList<Stroke> &strokes, const QPolygonF &area);   // all points inside
void drawSegment(QPainter &p, const Stroke &s, int i); // segment ending at point i (a dot for i == 0)
void drawStroke(QPainter &p, const Stroke &s);         // painter in mm coordinates
void drawStrokes(QPainter &p, const QList<Stroke> &all, const QRectF &area);   // images, highlighter, ink on top; area null: all
QImage snapshot(const QList<Stroke> &all, const QRectF &area);                 // area on white, ~150 dpi
bool saveSheet(const Sheet &s, const QString &path);   // atomic (QSaveFile)
bool loadSheet(Sheet &s, const QString &path);         // false: unreadable or other version, s untouched
// Cells holding the stroke's points, segments resampled every 2 mm (a diagonal's bbox corners do not count).
// below: also the cell under each point that lies in the bottom third of its page (page growth while writing).
QSet<QPoint> inkCells(const Sheet &s, const Stroke &st, bool below = false);
void refresh(Sheet &s, bool grow = true);             // grow: pages += cells with ink. Queue = inked pages: drops emptied, appends newly inked
void drawPage(QPainter &p, const Sheet &s, QPoint c);  // painter in mm with the page's top-left at the origin; ink clipped
bool canDeletePage(const Sheet &s, QPoint c);          // an empty page, not the last, that leaves no new island
bool exportPdf(const Sheet &s, const QList<QPoint> &pages, const QString &path);   // one PDF page each, in order

QDataStream &operator<<(QDataStream &d, const Pt &p);
QDataStream &operator>>(QDataStream &d, Pt &p);
QDataStream &operator<<(QDataStream &d, const Stroke &s);
QDataStream &operator>>(QDataStream &d, Stroke &s);
