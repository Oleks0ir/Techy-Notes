#include "Sheet.h"
#include <QLineF>
#include <QPainter>
#include <algorithm>

// Calibration knob: share of the preset width drawn at zero pressure.
static constexpr float kMinPressureWidth = 0.2f;

static double segDist(QPointF p, QPointF a, QPointF b) {
    QPointF ab = b - a;
    double l2 = QPointF::dotProduct(ab, ab);
    double t = l2 > 0 ? qBound(0.0, QPointF::dotProduct(p - a, ab) / l2, 1.0) : 0;
    return QLineF(p, a + t * ab).length();
}

bool hits(const Stroke &s, QPointF p, float radius) {
    radius += s.width / 2;
    for (int i = 0; i < s.pts.size(); ++i)
        if (segDist(p, s.pts[i].pos(), s.pts[qMin(i + 1, s.pts.size() - 1)].pos()) <= radius) return true;
    return false;
}

QPointF snap15(QPointF a, QPointF b) {
    QLineF l(a, b);
    l.setAngle(qRound(l.angle() / 15) * 15.0);
    return l.p2();
}

QRectF bounds(const Stroke &s) {
    QPolygonF poly;
    for (const Pt &p : s.pts) poly << p.pos();
    double m = s.width / 2;
    return poly.boundingRect().adjusted(-m, -m, m, m);
}

QSet<int> strokesInside(const QList<Stroke> &strokes, const QPolygonF &area) {
    QSet<int> in;
    for (int i = 0; i < strokes.size(); ++i)
        if (std::all_of(strokes[i].pts.begin(), strokes[i].pts.end(),
                        [&](const Pt &p) { return area.containsPoint(p.pos(), Qt::OddEvenFill); }))
            in.insert(i);
    return in;
}

QDataStream &operator<<(QDataStream &d, const Pt &p) { return d << p.x << p.y << p.p << p.t; }
QDataStream &operator>>(QDataStream &d, Pt &p) { return d >> p.x >> p.y >> p.p >> p.t; }
QDataStream &operator<<(QDataStream &d, const Stroke &s) { return d << s.pts << s.color << s.width; }
QDataStream &operator>>(QDataStream &d, Stroke &s) { return d >> s.pts >> s.color >> s.width; }

void drawSegment(QPainter &p, const Stroke &s, int i) {
    const Pt &b = s.pts[i], &a = s.pts[qMax(i - 1, 0)];
    p.setPen(QPen(s.color, s.width * (kMinPressureWidth + (1 - kMinPressureWidth) * b.p),
                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (i == 0) p.drawPoint(b.pos());
    else p.drawLine(a.pos(), b.pos());
}

void drawStroke(QPainter &p, const Stroke &s) {
    for (int i = 0; i < s.pts.size(); ++i) drawSegment(p, s, i);
}
