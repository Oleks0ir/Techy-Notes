#include "Sheet.h"
#include "Math.h"
#include <QBuffer>
#include <QCache>
#include <QCryptographicHash>
#include <QDir>
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
    if (s.boxed()) return false;   // the eraser leaves images and math alone
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
        if (!strokes[i].locked && std::all_of(strokes[i].pts.begin(), strokes[i].pts.end(),
                        [&](const Pt &p) { return area.containsPoint(p.pos(), Qt::OddEvenFill); }))
            in.insert(i);
    return in;
}

QDataStream &operator<<(QDataStream &d, const Pt &p) { return d << p.x << p.y << p.p << p.t; }
QDataStream &operator>>(QDataStream &d, Pt &p) { return d >> p.x >> p.y >> p.p >> p.t; }
QDataStream &operator<<(QDataStream &d, const Stroke &s) { return d << s.pts << s.color << s.width << s.img << s.locked << s.math << s.link; }
QDataStream &operator>>(QDataStream &d, Stroke &s) { return d >> s.pts >> s.color >> s.width >> s.img >> s.locked >> s.math >> s.link; }

QString storeImage(const QImage &img) {
    QByteArray png;
    QBuffer buf(&png);
    if (img.isNull() || !buf.open(QIODevice::WriteOnly) || !img.save(&buf, "PNG")) return {};
    QString id = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha1).toHex());
    QString file = imageDir + "/" + id + ".png";
    if (QFile::exists(file)) return id;   // same picture stored before
    QSaveFile f(file);
    if (!QDir().mkpath(imageDir) || !f.open(QIODevice::WriteOnly) || f.write(png) != png.size() || !f.commit()) return {};
    return id;
}

QImage imageOf(const QString &id) {
    static QCache<QString, QImage> cache(512);   // MB of decoded images kept in memory
    if (QImage *i = cache.object(id)) return *i;
    QImage img(imageDir + "/" + id + ".png");
    if (!img.isNull()) cache.insert(id, new QImage(img), qMax<qsizetype>(1, img.sizeInBytes() >> 20));
    return img;
}

void drawSegment(QPainter &p, const Stroke &s, int i) {
    const Pt &b = s.pts[i], &a = s.pts[qMax(i - 1, 0)];
    p.setPen(QPen(s.color, s.width * (kMinPressureWidth + (1 - kMinPressureWidth) * b.p),
                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (i == 0) p.drawPoint(b.pos());
    else p.drawLine(a.pos(), b.pos());
}

void drawStroke(QPainter &p, const Stroke &s) {
    if (!s.img.isEmpty()) {
        QRectF r = bounds(s);
        QImage img = imageOf(s.img);
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (img.isNull()) p.fillRect(r, QColor(0xe0, 0xe0, 0xe0));   // file missing from the store
        else p.drawImage(r, img);
        p.restore();
    } else if (!s.math.isEmpty()) {
        p.save();
        p.setPen(s.color);
        drawMath(p, s.math, bounds(s));
        p.restore();
    } else if (s.color.alpha() < 255) {   // highlighter: one path, so overlapping segments don't darken
        QPolygonF poly;
        for (const Pt &pt : s.pts) poly << pt.pos();
        p.setPen(QPen(s.color, s.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        poly.size() == 1 ? p.drawPoint(poly[0]) : p.drawPolyline(poly);
    } else {
        for (int i = 0; i < s.pts.size(); ++i) drawSegment(p, s, i);
    }
}

void drawStrokes(QPainter &p, const QList<Stroke> &all, const QRectF &area) {
    auto layer = [](const Stroke &s) { return s.boxed() ? 0 : s.color.alpha() < 255 ? 1 : 2; };
    for (int l : {0, 1, 2})
        for (const Stroke &s : all)
            if (layer(s) == l && (area.isNull() || bounds(s).intersects(area))) drawStroke(p, s);
}

QImage snapshot(const QList<Stroke> &all, const QRectF &area) {
    double k = qMin(6.0, 4000 / qMax(area.width(), area.height()));   // px per mm, at most 4000 px wide
    QImage img((area.size() * k).toSize().expandedTo({1, 1}), QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(k, k);
    p.translate(-area.topLeft());
    drawStrokes(p, all, area);
    return img;
}

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
    if (version < 1 || version > kSheetVersion) return false;
    Sheet in;
    quint32 n = 0;   // the stroke list as QDataStream writes it, minus the fields older versions lack
    for (d >> n; n && d.status() == QDataStream::Ok; --n) {
        Stroke st;
        d >> st.pts >> st.color >> st.width;
        if (version >= 2) d >> st.img >> st.locked;
        if (version >= 3) d >> st.math;
        if (version >= 4) d >> st.link;
        in.strokes << st;
    }
    d >> in.pages >> in.page >> in.exportQueue;
    if (d.status() != QDataStream::Ok) return false;
    s = in;
    return true;
}

QSet<QPoint> inkCells(const Sheet &s, const Stroke &st, bool below) {
    QSet<QPoint> out;
    if (st.boxed()) {   // image/math: every cell its rectangle covers (edges touching a cell don't count)
        QRectF r = bounds(st).adjusted(0.01, 0.01, -0.01, -0.01);
        QPoint a = s.cellAt(r.topLeft()), b = s.cellAt(r.bottomRight());
        for (int y = a.y(); y <= b.y(); ++y)
            for (int x = a.x(); x <= b.x(); ++x) out.insert({x, y});
        return out;
    }
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
    std::sort(fresh.begin(), fresh.end(), rowMajor);
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
    drawStrokes(p, s.strokes, r);
    p.restore();
}

bool exportPdf(const Sheet &s, const QList<QPoint> &pages, const QString &path) {
    if (pages.isEmpty()) return false;
    QPdfWriter pdf(path);
    pdf.setCreator("Techy-Notes");
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
