#include "Sheet.h"
#include <QFile>
#include <QLineF>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
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

static constexpr qint32 kSheetVersion = 1;   // bump on any format change

bool saveSheet(const Sheet &s, const QString &path) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QDataStream d(&f);
    d.setVersion(QDataStream::Qt_6_0);
    d << kSheetVersion << s.strokes << s.pages << s.page << s.exportQueue;
    return d.status() == QDataStream::Ok && f.commit();
}

bool loadSheet(Sheet &s, const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream d(&f);
    d.setVersion(QDataStream::Qt_6_0);
    qint32 version = 0;
    d >> version;
    if (version != kSheetVersion) return false;
    Sheet in;
    d >> in.strokes >> in.pages >> in.page >> in.exportQueue;
    if (d.status() != QDataStream::Ok) return false;
    s = in;
    return true;
}

QSet<QPoint> inkCells(const Sheet &s, const Stroke &st, bool below) {
    QSet<QPoint> out;
    for (int i = 0; i < st.pts.size(); ++i) {
        QPointF a = st.pts[qMax(i - 1, 0)].pos(), b = st.pts[i].pos();
        int n = qCeil(QLineF(a, b).length() / 2);
        for (int k = 0; k <= n; ++k) {
            QPointF p = n ? a + (b - a) * k / n : b;
            QPoint c = s.cellAt(p);
            out.insert(c);
            if (below && p.y() - s.pageRect(c).top() > s.page.height() * 2 / 3) out.insert(c + QPoint(0, 1));
        }
    }
    return out;
}

void refresh(Sheet &s, bool grow) {
    // ponytail: rescans all ink on every edit, keep per-stroke cells when big sheets lag
    QSet<QPoint> inked;
    for (const Stroke &st : s.strokes) inked |= inkCells(s, st);
    if (grow) s.pages |= inked;
    else inked &= s.pages;   // ink off the pages is not exported
    s.exportQueue.removeIf([&](QPoint c) { return !inked.contains(c); });
    QList<QPoint> fresh;
    for (QPoint c : inked)
        if (!s.exportQueue.contains(c)) fresh << c;
    std::sort(fresh.begin(), fresh.end(), [](QPoint a, QPoint b) { return std::pair(a.y(), a.x()) < std::pair(b.y(), b.x()); });
    s.exportQueue += fresh;
}

static int islands(QSet<QPoint> left) {
    int n = 0;
    while (!left.isEmpty()) {
        ++n;
        QList<QPoint> todo{*left.begin()};
        left.remove(todo[0]);
        while (!todo.isEmpty()) {
            QPoint c = todo.takeLast();
            for (QPoint d : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)})
                if (left.remove(c + d)) todo << c + d;
        }
    }
    return n;
}

bool canDeletePage(const Sheet &s, QPoint c) {
    if (!s.pages.contains(c) || s.pages.size() == 1) return false;
    for (const Stroke &st : s.strokes)
        if (inkCells(s, st).contains(c)) return false;
    QSet<QPoint> rest = s.pages;
    rest.remove(c);
    return islands(rest) <= islands(s.pages);
}

void drawPage(QPainter &p, const Sheet &s, QPoint c) {
    QRectF r = s.pageRect(c);
    p.save();
    p.translate(-r.topLeft());
    p.setClipRect(r);
    for (const Stroke &st : s.strokes)
        if (bounds(st).intersects(r)) drawStroke(p, st);
    p.restore();
}

bool exportPdf(const Sheet &s, const QList<QPoint> &pages, const QString &path) {
    if (pages.isEmpty()) return false;
    QPdfWriter pdf(path);
    pdf.setCreator("Techy-Draw");
    pdf.setPageSize(QPageSize(s.page, QPageSize::Millimeter, {}, QPageSize::ExactMatch));
    pdf.setPageMargins({});
    QPainter p;
    if (!p.begin(&pdf)) return false;
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(pdf.resolution() / 25.4, pdf.resolution() / 25.4);   // mm
    for (int i = 0; i < pages.size(); ++i) {
        if (i) pdf.newPage();
        drawPage(p, s, pages[i]);
    }
    return p.end();
}
