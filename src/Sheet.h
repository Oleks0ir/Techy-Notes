#pragma once
#include <QColor>
#include <QDataStream>
#include <QList>
#include <QPoint>
#include <QPolygonF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QtMath>

class QPainter;

struct Pt {                                                    // mm, pressure 0..1, ms timestamp
    float x, y, p; qint64 t;
    QPointF pos() const { return {x, y}; }
};
struct Stroke { QList<Pt> pts; QColor color; float width; };   // a line is a 2-point stroke

struct Sheet {
    QList<Stroke> strokes;
    QSet<QPoint> pages{QPoint(0, 0)};                           // (col,row) cells, may be negative
    QSizeF page{210, 297};                                      // mm, A4 portrait
    QList<QPoint> exportQueue;

    QPoint cellAt(QPointF p) const { return {qFloor(p.x() / page.width()), qFloor(p.y() / page.height())}; }
    QRectF pageRect(QPoint c) const {
        return {c.x() * page.width(), c.y() * page.height(), page.width(), page.height()};
    }
};

bool hits(const Stroke &s, QPointF p, float radius);   // p within radius (mm) of the stroke's ink
QPointF snap15(QPointF a, QPointF b);                  // b rotated around a to the nearest 15° step
QRectF bounds(const Stroke &s);                        // ink bounds incl. width
QSet<int> strokesInside(const QList<Stroke> &strokes, const QPolygonF &area);   // all points inside
void drawSegment(QPainter &p, const Stroke &s, int i); // segment ending at point i (a dot for i == 0)
void drawStroke(QPainter &p, const Stroke &s);         // painter in mm coordinates
bool saveSheet(const Sheet &s, const QString &path);   // atomic (QSaveFile)
bool loadSheet(Sheet &s, const QString &path);         // false: unreadable or other version, s untouched
// Cells holding the stroke's points, segments resampled every 2 mm (a diagonal's bbox corners do not count).
// below: also the cell under each point that lies in the bottom third of its page (page growth while writing).
QSet<QPoint> inkCells(const Sheet &s, const Stroke &st, bool below = false);
void refresh(Sheet &s);                                // pages += cells with ink; queue drops emptied, appends newly inked
bool canDeletePage(const Sheet &s, QPoint c);          // an empty page, not the last, that leaves no new island
bool exportPdf(const Sheet &s, const QString &path);   // exportQueue pages in order, ink clipped per page

QDataStream &operator<<(QDataStream &d, const Pt &p);
QDataStream &operator>>(QDataStream &d, Pt &p);
QDataStream &operator<<(QDataStream &d, const Stroke &s);
QDataStream &operator>>(QDataStream &d, Stroke &s);
