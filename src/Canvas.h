#pragma once
#include "Sheet.h"
#include <QWidget>

class Canvas : public QWidget {
public:
    enum Tool { Pen, Line, Select, Eraser } tool = Pen;
    Sheet sheet;

    explicit Canvas(QWidget *parent = nullptr);

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void tabletEvent(QTabletEvent *e) override;

private:
    void zoomAt(QPointF anchor, double factor);

    double scale = 3;          // px per mm
    QPointF offset{40, 40};    // screen position of world origin
    QPointF lastMouse;
    qint64 lastPenMs = 0;
};
