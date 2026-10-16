#include "Ink.h"
#include <QFile>
#include <QHash>
#include <QLineF>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr int N = 32;   // points per $P cloud
using Cloud = std::array<QPointF, N>;

double length(const QPolygonF &s) {
    double l = 0;
    for (int i = 1; i < s.size(); ++i) l += QLineF(s[i - 1], s[i]).length();
    return l;
}
QRectF boxOf(const QList<QPolygonF> &strokes) {
    QRectF b;
    for (const QPolygonF &s : strokes) b |= s.boundingRect();
    return b;
}
// n points evenly spaced along the ink; the jumps between strokes do not count
QPolygonF resample(const QList<QPolygonF> &strokes, int n) {
    double total = 0;
    for (const QPolygonF &s : strokes) total += length(s);
    QPolygonF out{strokes[0][0]};
    double step = total / (n - 1), acc = 0;
    for (const QPolygonF &s : strokes) {
        QPointF prev = s[0];
        for (int i = 1; i < s.size() && step > 0;) {
            double d = QLineF(prev, s[i]).length();
            if (d > 0 && acc + d >= step) {
                prev += (s[i] - prev) * ((step - acc) / d);
                out << prev;
                acc = 0;
            } else {
                acc += d;
                prev = s[i++];
            }
        }
    }
    while (out.size() < n) out << strokes.last().last();
    out.resize(n);
    return out;
}
// $P: resampled, scaled by the larger side (aspect kept: "-" is not "|"), centroid at the origin
Cloud cloudOf(const QList<QPolygonF> &strokes) {
    QPolygonF p = resample(strokes, N);
    QRectF b = p.boundingRect();
    double k = 1 / std::max({b.width(), b.height(), 1e-9});
    QPointF c;
    for (QPointF q : p) c += q / N;
    Cloud out;
    for (int i = 0; i < N; ++i) out[i] = (p[i] - c) * k;
    return out;
}
// Greedy matching from point `start`, earlier matches weigh more; stops once past `best`
double cloudDist(const Cloud &a, const Cloud &b, int start, double best) {
    std::array<bool, N> used{};
    double sum = 0;
    for (int k = 0; k < N; ++k) {
        const QPointF &p = a[(start + k) % N];
        double mn = 1e18;
        int at = 0;
        for (int j = 0; j < N; ++j)
            if (!used[j]) {
                double dx = p.x() - b[j].x(), dy = p.y() - b[j].y(), d = dx * dx + dy * dy;
                if (d < mn) mn = d, at = j;
            }
        used[at] = true;
        sum += (1 - double(k) / N) * std::sqrt(mn);
        if (sum >= best) return sum;
    }
    return sum;
}
double match(const Cloud &a, const Cloud &b, double best) {
    for (int i = 0; i < N; i += 5)   // step ≈ √N
        best = std::min({best, cloudDist(a, b, i, best), cloudDist(b, a, i, best)});
    return best;
}

struct Prepared { QString sym; Cloud cloud; double aspect; };   // aspect: log(width / height)
double aspectOf(const QList<QPolygonF> &strokes) {
    QRectF b = boxOf(strokes);
    return std::log((b.width() + 1e-3 * (b.width() + b.height()) + 1e-9) / (b.height() + 1e-3 * (b.width() + b.height()) + 1e-9));
}
constexpr int kCands = 6;   // candidates kept per symbol
// Symbols by their nearest sample, best first. Samples of a very different shape (a "-" for an "1") are skipped,
// and matching stops early once a sample cannot make the top kCands.
QList<std::pair<double, QString>> classify(const QList<QPolygonF> &strokes, const QList<Prepared> &tpl) {
    Cloud c = cloudOf(strokes);
    double asp = aspectOf(strokes), bound = 1e18;
    QHash<QString, double> best;
    QList<double> tops;   // the kCands smallest per-symbol bests
    QList<std::pair<double, int>> rough;   // one-direction match from point 0: keep the closest 200 for the full match
    for (int i = 0; i < tpl.size(); ++i)
        if (std::abs(tpl[i].aspect - asp) <= 1.5) rough.append({cloudDist(c, tpl[i].cloud, 0, 1e18), i});   // ~4.5x wider or narrower: skipped
    std::sort(rough.begin(), rough.end());
    rough.resize(std::min<qsizetype>(rough.size(), 200));   // ponytail: calibration knob (speed vs. accuracy)
    for (auto [r, i] : rough) {
        const Prepared &t = tpl[i];
        auto it = best.find(t.sym);
        double b = it == best.end() ? 1e18 : *it, d = match(c, t.cloud, std::min(b, bound));
        if (d >= b) continue;
        best[t.sym] = d;
        tops.clear();
        for (double v : best) tops << v;
        std::sort(tops.begin(), tops.end());
        if (tops.size() >= kCands) bound = tops[kCands - 1];
    }
    QList<std::pair<double, QString>> out;
    for (auto it = best.begin(); it != best.end(); ++it) out.append({it.value(), it.key()});
    std::sort(out.begin(), out.end());
    return out;
}
bool flat(const QRectF &b) { return b.height() < 0.3 * b.width(); }   // "-", fraction bars, "="'s lines
double median(QList<double> v) {
    if (v.isEmpty()) return 1;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}
}   // namespace

Template makeTemplate(const QString &sym, const QList<QPolygonF> &strokes) {
    QRectF box = boxOf(strokes);
    double total = 0, k = 1 / std::max({box.width(), box.height(), 1e-9});
    for (const QPolygonF &s : strokes) total += length(s);
    Template t{sym, {}};
    for (const QPolygonF &s : strokes) {   // each stroke resampled, N points shared out by length
        QPolygonF r = resample({s}, std::max(2, int(std::lround(N * length(s) / std::max(total, 1e-9)))));
        for (QPointF &p : r) p = (p - box.topLeft()) * k;
        t.strokes << r;
    }
    return t;
}

QList<Template> loadTemplates(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QDataStream d(&f);
    d.setVersion(QDataStream::Qt_6_0);
    QByteArray magic;
    qint32 v = 0;
    QList<Template> t;
    d >> magic >> v;
    if (magic != "TPL" || v != kTemplateVersion) return {};
    d >> t;
    return d.status() == QDataStream::Ok ? t : QList<Template>{};
}

bool saveTemplates(const QList<Template> &t, const QString &path) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QDataStream d(&f);
    d.setVersion(QDataStream::Qt_6_0);
    d << QByteArray("TPL") << kTemplateVersion << t;
    return d.status() == QDataStream::Ok && f.commit();
}

QList<InkSymbol> recognizeSymbols(const QList<QPolygonF> &ink, const QList<Template> &templates) {
    QList<Prepared> tpl;
    for (const Template &t : templates)
        if (!t.strokes.isEmpty() && !t.strokes[0].isEmpty()) tpl.append({t.sym, cloudOf(t.strokes), aspectOf(t.strokes)});
    QList<double> sizes;
    for (const QPolygonF &s : ink) sizes << std::max(s.boundingRect().width(), s.boundingRect().height());
    double h = median(sizes);   // typical symbol size
    auto dot = [&](const QRectF &b) { return std::max(b.width(), b.height()) < 0.3 * h; };   // ponytail: calibration knob

    // Grouping: a stroke joins the symbol written just before it when their boxes are close and the joined ink
    // matches a sample at least as well as the parts on average (x, +, =, π, i's dot). ponytail: consecutive
    // strokes only, so an i dotted or a t crossed at the end of the line becomes its own symbol
    struct Group { QList<int> strokes; QRectF box; QList<std::pair<double, QString>> cls; };
    QList<Group> groups;
    auto inkOf = [&](const QList<int> &idx) { QList<QPolygonF> s; for (int i : idx) s << ink[i]; return s; };
    auto score = [&](const QList<int> &idx) { return dot(boxOf(inkOf(idx))) ? QList<std::pair<double, QString>>{{0, "."}} : classify(inkOf(idx), tpl); };
    for (int i = 0; i < ink.size(); ++i) {
        if (ink[i].isEmpty()) continue;
        QRectF b = ink[i].boundingRect();
        Group one{{i}, b, score({i})};
        if (!groups.isEmpty() && groups.last().strokes.size() < 4) {
            Group &g = groups.last();
            QRectF gb = g.box;
            bool near = b.left() < gb.right() + 0.15 * h && b.right() > gb.left() - 0.15 * h &&
                        b.top() < gb.bottom() + 0.3 * h && b.bottom() > gb.top() - 0.6 * h;
            bool crossing = b.adjusted(-0.05 * h, -0.05 * h, 0.05 * h, 0.05 * h).intersects(gb);
            // a bar under or over a symbol is a fraction bar, not part of it ("=" is two bars: allowed); a dot
            // written as a short dash is no bar
            if (near && !dot(b) && (flat(b) != flat(gb)) && !crossing) near = false;
            if (near && dot(b) && b.bottom() < gb.top() + 0.3 * gb.height()) {   // i, j
                g.strokes << i;
                g.box |= b;
                g.cls = classify(inkOf(g.strokes), tpl);
                continue;
            }
            if (near && !dot(b) && !dot(gb) && !g.cls.isEmpty() && !one.cls.isEmpty()) {
                QList<int> joined = g.strokes;
                joined << i;
                auto cls = classify(inkOf(joined), tpl);
                double parts = (g.cls[0].first * g.strokes.size() + one.cls[0].first) / joined.size();
                if (!cls.isEmpty() && cls[0].first <= parts) {
                    g = {joined, gb | b, cls};
                    continue;
                }
            }
        }
        groups << one;
    }
    // A dot on its own above a symbol belongs to it, whenever it was written (the i's of "sin" dotted at the end)
    for (int d = groups.size() - 1; d >= 0; --d) {
        if (groups[d].strokes.size() != 1 || !dot(groups[d].box)) continue;
        QRectF db = groups[d].box;
        int best = -1;
        for (int g = 0; g < groups.size(); ++g) {
            QRectF gb = groups[g].box;
            if (g == d || dot(gb) || db.center().x() < gb.left() - 0.3 * h || db.center().x() > gb.right() + 0.3 * h ||
                db.bottom() > gb.top() + 0.25 * gb.height() || db.bottom() < gb.top() - h) continue;
            if (best < 0 || std::abs(db.center().x() - gb.center().x()) < std::abs(db.center().x() - groups[best].box.center().x())) best = g;
        }
        if (best < 0) continue;
        Group &g = groups[best];
        g.strokes << groups[d].strokes;
        std::sort(g.strokes.begin(), g.strokes.end());
        g.box |= db;
        g.cls = classify(inkOf(g.strokes), tpl);
        groups.removeAt(d);
    }
    QList<InkSymbol> out;
    for (const Group &g : groups) {
        InkSymbol s{g.strokes, g.box, {}};
        for (int k = 0; k < g.cls.size() && k < kCands; ++k) s.cands << g.cls[k].second;
        if (s.cands.isEmpty()) s.cands << "?";
        out << s;
    }
    return out;
}

namespace {
// Recursive layout of a set of symbols: left to right, a fraction bar / √ / ∫ / ∑ / lim takes the symbols
// around it, a smaller symbol raised after another is its power.
struct Layout {
    const QList<InkSymbol> &sy;
    double h;   // typical symbol height

    QString label(int i) const {
        QString t = sy[i].cands.value(0);
        return t == "·" || t == "×" ? "*" : t;
    }
    QRectF box(int i) const { return sy[i].box; }
    double height(int i) const { return std::max(box(i).height(), 0.6 * h); }   // flat symbols count as normal size
    bool can(int i, const char *t) const { return sy[i].cands.mid(0, 3).contains(QString::fromUtf8(t)); }

    QString run(QList<int> ids) const {
        std::sort(ids.begin(), ids.end(), [&](int a, int b) { return box(a).left() < box(b).left(); });
        QSet<int> done;
        QString out;
        int prev = -1;   // last symbol on the baseline (decides "." vs "·")
        auto rest = [&] { QList<int> r; for (int i : ids) if (!done.contains(i)) r << i; return r; };
        auto take = [&](auto pred) { QList<int> r; for (int i : rest()) if (pred(i)) r << i, done.insert(i); return r; };
        auto above = [&](int bar, int i) { return box(i).center().y() < box(bar).center().y(); };
        auto spans = [&](int bar, int i) {   // i sits over or under the bar
            double x = box(i).center().x();
            return i != bar && x > box(bar).left() && x < box(bar).right() &&
                   std::abs(box(i).center().y() - box(bar).center().y()) < 2.5 * h;
        };
        auto isBar = [&](int b) {
            if (!flat(box(b)) || !can(b, "-")) return false;
            bool up = false, down = false;
            for (int i : rest()) if (spans(b, i)) (above(b, i) ? up : down) = true;
            return up && down;
        };
        while (true) {
            QList<int> r = rest();
            if (r.isEmpty()) break;
            int s = r[0], bar = -1;
            for (int b : r)   // the widest fraction bar over the next symbol goes first (a numerator may start left of it)
                if ((b == s || spans(b, s)) && isBar(b) && (bar < 0 || box(b).width() > box(bar).width())) bar = b;
            if (bar >= 0) s = bar;
            QRectF sb = box(s);
            QString t = label(s);
            done.insert(s);
            if (s == bar) {
                QList<int> num = take([&](int i) { return spans(s, i) && above(s, i); });
                QList<int> den = take([&](int i) { return spans(s, i); });
                out += "(" + run(num) + ")/(" + run(den) + ")";
                prev = s;
                continue;
            }
            if (t == "√") {
                out += "sqrt(" + run(take([&](int i) { return sb.contains(box(i).center()) && box(i).center().x() > sb.left() + 0.25 * sb.width(); })) + ")";
                prev = s;
                continue;
            }
            bool lim = can(s, "l") && r.size() >= 3 && can(r[1], "i") && can(r[2], "m") && r[0] == s;
            if (t == "∫" || t == "∑" || lim) {
                if (lim) done.insert(r[1]), done.insert(r[2]), sb |= box(r[1]) | box(r[2]);
                double reach = t == "∫" ? 0.8 * sb.width() + 0.5 * h : 0.2 * sb.width();
                auto near = [&](int i) {
                    QRectF b = box(i);
                    return b.center().x() > sb.left() - 0.3 * sb.width() && b.center().x() < sb.right() + reach && b.height() < 0.8 * sb.height() &&
                           b.top() > sb.top() - 1.5 * h && b.bottom() < sb.bottom() + 1.5 * h;
                };
                QList<int> hi = lim ? QList<int>{} : take([&](int i) { return near(i) && box(i).center().y() < sb.top() + 0.3 * sb.height(); });
                QList<int> lo = take([&](int i) { return near(i) && box(i).center().y() > sb.bottom() - (lim ? 0.0 : 0.3 * sb.height()); });
                out += lim ? QString("lim") : t == "∫" ? QString("int") : QString("sum");
                if (!lo.isEmpty()) out += "_(" + run(lo) + ")";
                if (!hi.isEmpty()) out += "^(" + run(hi) + ")";
                out += " ";
                prev = s;
                continue;
            }
            if (t == ".") {   // at mid height: a product dot; on the baseline: decimal point
                bool mid = prev >= 0 && sb.center().y() < box(prev).bottom() - 0.3 * height(prev);
                out += mid ? "*" : ".";
                continue;
            }
            out += t;
            prev = s;
            // power: smaller symbols right after it whose bottom is above its middle
            double mid = sb.top() + 0.5 * height(s);
            QList<int> sup;
            double right = sb.right();
            for (int i : rest()) {
                QRectF b = box(i);
                if (b.left() > right + 0.6 * h || b.bottom() > mid || b.height() > 0.9 * height(s)) break;
                sup << i;
                right = std::max(right, b.right());
            }
            if (!sup.isEmpty()) {
                for (int i : sup) done.insert(i);
                QString e = run(sup);
                out += sup.size() == 1 ? "^" + e : "^(" + e + ")";
            }
        }
        return out;
    }
};
}   // namespace

Guess guessSymbol(const QList<QPolygonF> &strokes, const QString &expected, const QList<Template> &templates) {
    QList<Prepared> tpl;
    for (const Template &t : templates)
        if (!t.strokes.isEmpty() && !t.strokes[0].isEmpty()) tpl.append({t.sym, cloudOf(t.strokes), aspectOf(t.strokes)});
    auto cls = classify(strokes, tpl);
    Cloud c = cloudOf(strokes);
    double de = 1e18, dr = 1e18;   // expected: every one of its samples, also those the rough pass dropped
    for (const Prepared &t : tpl)
        if (t.sym == expected) de = match(c, t.cloud, de);
    for (const auto &[d, s] : cls)
        if (s != expected) { dr = d; break; }
    if (de >= 1e18) return {cls.isEmpty() ? "?" : cls[0].second, 0};   // no sample of it at all
    QString best = de <= dr ? expected : cls[0].second;
    if (dr >= 1e18) return {best, 1};
    return {best, dr * dr / (de * de + dr * dr + 1e-18)};
}

QList<QList<int>> splitColumns(const QList<QPolygonF> &ink) {
    QList<int> order;
    for (int i = 0; i < ink.size(); ++i)
        if (!ink[i].isEmpty()) order << i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return ink[a].boundingRect().left() < ink[b].boundingRect().left(); });
    QList<QList<int>> out;
    double right = -1e18, left = 0;
    for (int i : order) {
        QRectF b = ink[i].boundingRect();
        double m = 0.02 * std::max(b.width(), b.height());   // a straight "|" has no width
        b.adjust(-m, 0, m, 0);
        // joins the column when it overlaps it by 30% of the narrower one (neighbours touching do not join)
        double overlap = right - b.left(), narrow = std::min(b.width(), right - left);
        if (out.isEmpty() || overlap < 0.3 * narrow || overlap <= 0) out.append(QList<int>{}), left = b.left(), right = b.right();
        out.last() << i;
        right = std::max(right, b.right());
    }
    // dots (an i's dot beside its stem rather than over it) join the nearest column
    QList<double> sizes;
    for (int i : order) sizes << std::max(ink[i].boundingRect().width(), ink[i].boundingRect().height());
    double h = median(sizes);
    auto colBox = [&](const QList<int> &c) { QRectF b; for (int i : c) b |= ink[i].boundingRect(); return b; };
    for (int c = out.size() - 1; c >= 0 && out.size() > 1; --c) {
        QRectF cb = colBox(out[c]);
        if (std::max(cb.width(), cb.height()) >= 0.3 * h) continue;
        int best = -1;
        for (int k = 0; k < out.size(); ++k)
            if (k != c && (best < 0 || std::abs(colBox(out[k]).center().x() - cb.center().x()) < std::abs(colBox(out[best]).center().x() - cb.center().x()))) best = k;
        out[best] << out[c];
        out.removeAt(c);
    }
    for (QList<int> &c : out) std::sort(c.begin(), c.end());   // writing order inside a symbol
    return out;
}

InkText inkLayout(const QList<InkSymbol> &symbols) {
    QList<double> hs;
    for (const InkSymbol &s : symbols)
        if (!flat(s.box) && s.cands.value(0) != ".") hs << s.box.height();
    Layout l{symbols, median(hs)};
    // lines: a gap of more than 0.4 symbol heights starts a new one; bars reach one height up and down,
    // so a fraction stays one line
    auto ext = [&](int i) { QRectF b = symbols[i].box; return flat(b) ? b.adjusted(0, -l.h, 0, l.h) : b; };
    QList<int> order;
    for (int i = 0; i < symbols.size(); ++i) order << i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return ext(a).top() < ext(b).top(); });
    InkText out;
    double bottom = -1e18;
    for (int i : order) {
        if (out.lines.isEmpty() || ext(i).top() > bottom + 0.4 * l.h) out.lines.append(QList<int>{});
        out.lines.last() << i;
        bottom = out.lines.last().size() == 1 ? ext(i).bottom() : std::max(bottom, ext(i).bottom());
    }
    QStringList text;
    for (const QList<int> &line : out.lines) text << l.run(line);
    out.text = text.join('\n');
    return out;
}
