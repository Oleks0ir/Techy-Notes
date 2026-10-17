#pragma once
#include "Sheet.h"
#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>
#include <QTransform>
#include <QWidget>
#include <functional>

class Canvas : public QWidget {
public:
    enum Tool { Pen, Highlighter, Line, Lasso, Rect, Snip, Eraser, Trim, Crop, None } tool = Pen;   // Snip: copies a rectangle as an image; Trim, Crop: picture menu only
    static constexpr const char *kToolNames[] = {"Pen", "Highlighter", "Line", "Lasso", "Rect", "Snip", "Eraser"};   // Tool order; toolbar + pen ring
    static constexpr const char *kColors[] = {"#000000", "#1565c0", "#c62828", "#2e7d32", "#fdd835"};   // colour presets
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
    bool leftHanded = false;             // Settings: mirrors the pen ring
    static inline bool penBarrel = false; // pen side button as Windows reports it, also while hovering
    std::function<void()> onPick;        // after a ring pick: the toolbar shows the new tool/colour
    std::function<void()> onMath;        // pen ring > Math: show/hide the math dock
    std::function<void(const QString &)> onEditMath;   // object menu > Edit: that math object's text, for the dock
    // selection > Recognize math: its ink becomes (or joins) an analysed equation, the strokes' shared link
    std::function<void(quint32, const QList<int> &)> onRecognize;   // the link, the lassoed strokes
    // a finger tap on analysed ink, or the pen ring's Open math over it: that equation's link; 0: a tap elsewhere
    std::function<void(quint32)> onOpenEquation;
    std::function<void()> onLeave;       // before another sheet opens: the dock keeps what it shows of this one

    explicit Canvas(QWidget *parent = nullptr);
    void undo();
    void redo();
    void setColor(QColor c);     // also recolors the selection
    void copySelection();
    void deleteSelection();
    void paste(QPointF screen = {});   // at that point of the canvas; default: view centre
    bool open(const QString &file);   // saves the current sheet first; false: kept current sheet or load failed
    void save();                 // writes pending changes now (autosave does it 1 s after the last change)
    bool autoPages = true;       // Settings: ink creates pages (outside pages, page below, ghost page)
    bool deletePage(QPoint c);   // false: not empty, last one, or would split the layout
    void clearPage(QPoint c);    // removes the strokes touching page c (undoable)
    bool setPageSize(QSizeF mm); // false once the sheet has ink
    void setExportQueue(const QList<QPoint> &q);
    // Selected, one undo step. ink 0: at the view centre (plots 80×60 mm, equations 6 mm text); 1: right of the
    // recognized ink, 2: in its place (the ink is removed); equations as tall as that ink. 1/2 need inkValid().
    void insertMath(const QString &math, int ink = 0);
    bool inkValid() const;                  // the ink of useInk is still there, unchanged
    bool updateMath(const QString &math);   // the object last opened with Edit (one undo step); false: it is gone
    QList<int> equationInk(quint32 link) const;   // the plain ink strokes of that equation, ascending
    int mathOf(quint32 link) const;               // its inserted math object; -1: none
    void useInk(quint32 link);                    // the ink Insert places the math by (kept ink stays linked); 0: none
    void editMath(int i);                         // Update changes math object i
    void relink(quint32 from, quint32 to);        // the ink of equation from joins equation to (no undo step)
    // the dock's saved state of an equation (no undo step; empty removes it)
    void setEquation(quint32 link, const QByteArray &state);

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void tabletEvent(QTabletEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void zoomAt(QPointF anchor, double factor);
    void viewChanged();
    void begin(QPointF screen, float pressure, qint64 t, Tool t0);
    void move(QPointF screen, float pressure, qint64 t);
    void end();
    bool cropPicture(int i, QRectF r);   // r world mm, clipped to the picture; false: unchanged
    void cancel();               // drops the gesture in progress without an undo step
    void erase(QPointF world);
    void addImage(const QImage &img, QPointF center);   // world mm; shrunk to fit a page, selected
    void pushUndo(const QList<Stroke> &prev);
    void step(QList<QList<Stroke>> &from, QList<QList<Stroke>> &to);   // undo/redo: from's last state back, current onto to
    void modified();             // after any sheet edit: pages/queue follow the ink, autosave
    void hover(QPointF screen);  // shows a "+" on a free page edge near the pointer
    struct MenuItem { QChar icon; QString label; std::function<void()> run; QColor color = {}; };   // color: a swatch
    QList<MenuItem> objectActions(QPointF world);   // the selection / picture there: its actions; empty: none
    void pageMenu(QPointF screen);   // right-click: object actions, then clear / delete page
    void openRing(QPointF screen);
    bool ringEvent(QTabletEvent *e);
    std::pair<double, double> ringArc(int i) const;
    int ringItemAt(QPointF screen) const;   // -1: none
    void paintRing(QPainter &p);
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
    bool moving = false;       // select gesture moves or resizes the selection
    bool resizing = false;     // ... by its corner handle
    int trimIdx = -1;          // picture to crop with the next drag (picture menu > Trim)
    int cropIdx = -1;          // picture with a crop frame (picture menu > Crop)
    QRectF cropRect, cropFrom; // the frame, world mm; and at the start of a drag
    int cropGrab = 0;          // drag: 1 top-left handle, 2 bottom-right handle, 3 moves the frame
    bool adding = false;       // select gesture adds to the selection (Shift/Ctrl held)
    QPointF dragFrom;          // world mm
    QTransform selXf;          // selection move/resize in progress, world mm
    int mathIdx = -1;          // math object opened with Edit, valid while its text is still mathWas
    QString mathWas;
    QList<int> inkIdx;         // strokes of useInk (ascending) and their content then
    QList<Stroke> inkWas;
    quint32 inkLink = 0;
    quint32 newLink() const;   // an unused Stroke::link id
    QHash<quint32, QRectF> equations() const;   // analysed ink (with a saved state or an inserted object): its bounds
    quint32 equationAt(QPointF world) const;    // the smallest of them around that point; 0: none
    QPointF tapFrom;           // one finger down here, and when (a quick tap on analysed ink opens it)
    qint64 tapMs = 0;
    bool tapMoved = false;

    QPixmap cache;             // pages + committed ink, rendered at cacheScale/cacheOffset
    double cacheScale = 0;
    QPointF cacheOffset;
    bool cacheDirty = true;
    qsizetype cachePages = 0;  // page count the cache shows; re-render when pages are added or deleted
    bool plusOn = false;       // "+" shown at plusAt (world mm, edge midpoint); adds plusCell
    QPoint plusCell;
    QPointF plusAt;
    bool menuOpen = false;     // one page menu at a time, none within 500 ms after the last (pens repeat presses)
    QElapsedTimer sinceMenu;
    QTimer hold;               // finger long-press -> page menu
    QPointF holdAt;
    bool holdFired = false;    // held long enough: the lift opens the menu
    bool menuHeld = false;     // pen right-click button still held since its menu opened
    static constexpr double kRing[3] = {55, 150, 195};   // px: dead zone, inner band (actions), outer band (colours)
    QList<MenuItem> ring;      // pen ring: actions (ringInner of them), then colour swatches
    int ringInner = 0, ringHot = -1;
    QPointF ringAt;
    bool ringOpen = false;
    bool penBlocked = false;   // the tap that closed the ring: no ink until the pen lifts
    bool needBarrelOff = false; // the button state Windows still reports was used; wait until it clears
    QTimer autosave;
    bool dirty = false;        // changes not yet on disk
    QTimer settle;             // running while a pan/zoom gesture is in progress
};
