#pragma once
#include "Sheet.h"
#include <QPixmap>
#include <QTimer>
#include <QWidget>

class Canvas : public QWidget {
public:
    enum Tool { Pen, Line, Lasso, Rect, Eraser, None } tool = Pen;
    Sheet sheet;
    QColor color = Qt::black;
    float width = 0.5;           // mm
    bool snapLines = false;      // toolbar toggle; Shift snaps too
    bool fingerDraws = false;    // toolbar toggle; off: one finger pans
    bool barrelErases = true;    // ponytail: hardcoded pen button mapping, settings UI later

    explicit Canvas(QWidget *parent = nullptr);
    void undo();
    void redo();
    void setColor(QColor c);     // also recolors the selection
    void copySelection();
    void deleteSelection();
    void paste();

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void tabletEvent(QTabletEvent *e) override;

private:
    void zoomAt(QPointF anchor, double factor);
    void viewChanged();
    void begin(QPointF screen, float pressure, qint64 t, Tool t0);
    void move(QPointF screen, float pressure, qint64 t);
    void end();
    void erase(QPointF world);
    void pushUndo(const QList<Stroke> &prev);
    void select(const QSet<int> &s);
    QRectF selBounds() const;
    void renderCache();
    void paintIntoCache(const Stroke &s, int fromPt);
    QPointF toWorld(QPointF s) const { return (s - offset) / scale; }

    double scale = 3;          // px per mm
    QPointF offset{40, 40};    // screen position of world origin
    QPointF lastMouse;
    qint64 lastPenMs = 0;

    Tool active = None;        // tool of the gesture in progress
    bool touchDrawing = false; // that gesture comes from a finger
    QList<Stroke> before;      // strokes at gesture start, pushed to undo if the gesture changed them
    bool changed = false;
    Stroke cur;                // live freehand stroke / line preview
    QList<QList<Stroke>> undoStack, redoStack;

    QSet<int> sel;             // selected stroke indices; drawn on top, not in the cache
    QPolygonF lasso;           // select gesture outline (Rect: first + last point), world mm
    bool moving = false;       // select gesture drags the selection
    QPointF dragFrom, dragBy;  // world mm

    QPixmap cache;             // pages + committed ink, rendered at cacheScale/cacheOffset
    double cacheScale = 0;
    QPointF cacheOffset;
    bool cacheDirty = true;
    QTimer settle;             // running while a pan/zoom gesture is in progress
};
