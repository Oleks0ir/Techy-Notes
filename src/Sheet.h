#pragma once
#include <QColor>
#include <QList>
#include <QPoint>
#include <QRectF>
#include <QSet>

struct Pt { float x, y, p; qint64 t; };                       // mm, pressure 0..1, ms timestamp
struct Stroke { QList<Pt> pts; QColor color; float width; };   // a line is a 2-point stroke

struct Sheet {
    QList<Stroke> strokes;
    QSet<QPoint> pages{QPoint(0, 0)};                           // (col,row) cells, may be negative
    QSizeF page{210, 297};                                      // mm, A4 portrait
    QList<QPoint> exportQueue;

    QRectF pageRect(QPoint c) const {
        return {c.x() * page.width(), c.y() * page.height(), page.width(), page.height()};
    }
};
