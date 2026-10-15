#pragma once
#include "Sheet.h"
#include <QPixmap>
#include <QTimer>
#include <QWidget>

class Canvas : public QWidget {
public:
    enum Tool { Pen, Line, Lasso, Rect, Eraser, None } tool = Pen;
    Sheet sheet;
    QString path;                // open sheet file; empty: none, canvas disabled
    QColor color = Qt::black;
    float width = 0.5;           // mm
    bool snapLines = false;      // toolbar toggle; Shift snaps too
    bool fingerDraws = false;    // toolbar toggle; off: one finger pans
    // Calibration knobs, set in Settings > Calibrate pen: side buttons and the pressure range of light..firm writing.
    // Defaults = Surface Pro 7+ pen: eraser button switches the pen to eraser mode, right-click button = RightButton.
    int eraserButtons = 0;                  // buttons that erase, besides the pen's own eraser mode
    int menuButtons = Qt::RightButton;      // right-click: opens menus, does nothing on the canvas yet
    float pressLow = 0, pressHigh = 1;

    explicit Canvas(QWidget *parent = nullptr);
    void undo();
    void redo();
    void setColor(QColor c);     // also recolors the selection
    void copySelection();
    void deleteSelection();
    void paste();
    bool open(const QString &file);   // saves the current sheet first; false: kept current sheet or load failed
    void save();                 // writes pending changes now (autosave does it 1 s after the last change)
    bool deletePage();           // the focused page (under the screen centre); false: not empty, last one, or would split
    bool setPageSize(QSizeF mm); // false once the sheet has ink
    void setExportQueue(const QList<QPoint> &q);

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
    void modified();             // after any sheet edit: pages/queue follow the ink, autosave
    void hover(QPointF screen);  // shows a "+" on a free page edge near the pointer
    bool pressPlus(QPointF screen);
    QPoint focused() const { return sheet.cellAt(toWorld(rect().center())); }
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
    bool adding = false;       // select gesture adds to the selection (Shift/Ctrl held)
    QPointF dragFrom, dragBy;  // world mm

    QPixmap cache;             // pages + committed ink, rendered at cacheScale/cacheOffset
    double cacheScale = 0;
    QPointF cacheOffset;
    bool cacheDirty = true;
    qsizetype cachePages = 0;  // page count the cache shows; re-render when pages are added or deleted
    bool plusOn = false;       // "+" shown at plusAt (world mm, edge midpoint); adds plusCell
    QPoint plusCell;
    QPointF plusAt;
    QTimer autosave;
    bool dirty = false;        // changes not yet on disk
    QTimer settle;             // running while a pan/zoom gesture is in progress
};
