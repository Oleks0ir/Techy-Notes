#include "Ink.h"
#include <QFile>
#include <QHash>
#include <QLineF>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
constexpr int N = 32;                  // points per stored template
constexpr int kSeq = 24, kGrid = 12;   // features: points along the ink, side of its picture
constexpr int kFeatures = kSeq * 3 + kGrid * kGrid + 3;
constexpr int kCands = 6;              // candidates kept per symbol
constexpr double kNever = -1e9;        // segmentation: strokes that cannot be one symbol

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
// n points evenly spaced along the ink; the jumps between strokes do not count. strokeOf: each point's stroke
QPolygonF resample(const QList<QPolygonF> &strokes, int n, QList<int> *strokeOf = nullptr) {
    double total = 0;
    for (const QPolygonF &s : strokes) total += length(s);
    QPolygonF out{strokes[0][0]};
    QList<int> ids{0};
    double step = total / (n - 1), acc = 0;
    for (int k = 0; k < strokes.size(); ++k) {
        const QPolygonF &s = strokes[k];
        QPointF prev = s[0];
        for (int i = 1; i < s.size() && step > 0;) {
            double d = QLineF(prev, s[i]).length();
            if (d > 0 && acc + d >= step) {
                prev += (s[i] - prev) * ((step - acc) / d);
                out << prev;
                ids << k;
                acc = 0;
            } else {
                acc += d;
                prev = s[i++];
            }
        }
    }
    while (out.size() < n) out << strokes.last().last(), ids << int(strokes.size()) - 1;
    out.resize(n);
    ids.resize(n);
    if (strokeOf) *strokeOf = ids;
    return out;
}

// The built-in classifier with its last layer retrained on the personal samples, plus kReplay built-in samples of
// every symbol so the others are not forgotten. A personal symbol it does not know (a joined-up "sin") becomes a
// new class. Recomputed only when the personal samples change.
const Mlp &classifier(const QList<Template> &personal) {
    static const Mlp base = loadMlp(":/inkmlp.bin");
    static Mlp adapted;
    static quint64 key = 0;
    static bool have = false;
    quint64 k = quint64(personal.size());
    for (const Template &t : personal) {
        k = k * 1000003 ^ qHash(t.sym);
        for (const QPolygonF &s : t.strokes)
            for (QPointF p : s) k = k * 31 + quint64(std::llround(p.x() * 1e4)) * 7 + quint64(std::llround(p.y() * 1e4));
    }
    if (have && k == key) return adapted;
    have = true;
    key = k;
    adapted = base;
    if (personal.isEmpty() || base.d == 0) return adapted;

    constexpr int kReplay = 5;
    static const QList<std::pair<std::vector<float>, int>> replay = [] {
        QList<std::pair<std::vector<float>, int>> r;
        QHash<QString, int> n;
        for (const Template &t : loadTemplates(":/symbols.tpl"))
            if (int c = int(base.classes.indexOf(t.sym)); c >= 0 && n[t.sym]++ < kReplay) r.append({hiddenOf(base, inkFeatures(t.strokes)), c});
        return r;
    }();
    struct Sample { std::vector<float> h; int c; float w; };
    QList<Sample> data;
    for (const auto &[h, c] : replay) data.append({h, c, 1});
    // the newest kRecent samples of each symbol (handwriting drifts; keeps retraining around a second at most)
    constexpr int kRecent = 30;
    QHash<QString, int> seen;
    QList<const Template *> recent;
    for (qsizetype i = personal.size() - 1; i >= 0; --i)
        if (!personal[i].strokes.isEmpty() && seen[personal[i].sym]++ < kRecent) recent << &personal[i];
    for (const Template *tp : recent) {   // ponytail: calibration knob, a personal sample weighs as 4 built-in ones
        const Template &t = *tp;
        int c = int(adapted.classes.indexOf(t.sym));
        if (c < 0) {
            c = int(adapted.classes.size());
            adapted.classes << t.sym;
            adapted.w2.resize(adapted.w2.size() + adapted.h, 0);
            adapted.b2.push_back(0);
        }
        data.append({hiddenOf(base, inkFeatures(t.strokes)), c, 4});
    }
    // Adam on the softmax layer, full batch
    const int C = int(adapted.classes.size()), H = adapted.h;
    std::vector<float> m1(size_t(C) * H + C), m2(m1.size()), g(m1.size());
    double wsum = 0;
    for (const Sample &s : data) wsum += s.w;
    for (int it = 1; it <= 60; ++it) {
        std::fill(g.begin(), g.end(), 0.f);
        for (const Sample &s : data) {
            std::vector<float> p = outputOf(adapted, s.h);
            for (int c = 0; c < C; ++c) {
                float e = (p[c] - (c == s.c ? 1.f : 0.f)) * s.w;
                if (std::abs(e) < 1e-6f) continue;
                float *gw = &g[size_t(c) * H];
                for (int j = 0; j < H; ++j) gw[j] += e * s.h[j];
                g[size_t(C) * H + c] += e;
            }
        }
        const float lr = 0.01f, b1 = 0.9f, b2 = 0.999f;
        float c1 = 1 - std::pow(b1, it), c2 = 1 - std::pow(b2, it);
        for (size_t i = 0; i < g.size(); ++i) {
            float gi = float(g[i] / wsum);
            m1[i] = b1 * m1[i] + (1 - b1) * gi;
            m2[i] = b2 * m2[i] + (1 - b2) * gi * gi;
            float step = lr * (m1[i] / c1) / (std::sqrt(m2[i] / c2) + 1e-8f);
            (i < size_t(C) * H ? adapted.w2[i] : adapted.b2[i - size_t(C) * H]) -= step;
        }
    }
    return adapted;
}

// Symbols by probability, best first, as (-log p, symbol): lower is better
QList<std::pair<double, QString>> classify(const QList<QPolygonF> &strokes, const Mlp &m) {
    if (m.d == 0) return {};
    std::vector<float> p = mlpProbs(m, inkFeatures(strokes));
    QList<int> idx;
    for (int i = 0; i < int(p.size()); ++i) idx << i;
    std::partial_sort(idx.begin(), idx.begin() + std::min<qsizetype>(kCands, idx.size()), idx.end(), [&](int a, int b) { return p[a] > p[b]; });
    QList<std::pair<double, QString>> out;
    for (int k = 0; k < kCands && k < idx.size(); ++k) out.append({-std::log(std::max(p[idx[k]], 1e-12f)), m.classes[idx[k]]});
    return out;
}
// Calibration: x and × are one shape, either counts for the other (the layout tells them apart by their neighbours)
std::vector<float> probsOf(const Mlp &m, const QList<QPolygonF> &strokes) {
    std::vector<float> p = mlpProbs(m, inkFeatures(strokes));
    if (qsizetype x = m.classes.indexOf("x"), t = m.classes.indexOf("×"); x >= 0 && t >= 0) p[x] = p[t] = p[x] + p[t];
    return p;
}
bool flat(const QRectF &b) { return b.height() < 0.3 * b.width(); }   // "-", fraction bars, "="'s lines
double median(QList<double> v) {
    if (v.isEmpty()) return 1;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// A run of neighbouring letters that could well be a function name becomes it: each of its letters among that
// symbol's top 4 candidates, and the name at least 1% as likely as the first choices together ("s1n" -> "sin")
void readWords(QList<InkSymbol> &syms, double h) {
    static const QStringList kWords{"sinh", "cosh", "tanh", "asin", "acos", "atan", "sqrt", "sin", "cos", "tan",
                                    "exp", "abs", "lim", "log", "ln"};
    QList<int> order;
    for (int i = 0; i < syms.size(); ++i) order << i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return syms[a].box.left() < syms[b].box.left(); });
    auto next = [&](int a, int b) {   // b follows a on the same line
        QRectF x = syms[a].box, y = syms[b].box;
        double overlap = std::min(x.bottom(), y.bottom()) - std::max(x.top(), y.top());
        return y.left() - x.right() < 0.8 * h && overlap > 0.4 * std::min(x.height(), y.height());
    };
    // x and × are one shape: × only between two digits (2×3), the letter otherwise
    auto digit = [&](int k) { return k >= 0 && k < order.size() && syms[order[k]].cands.value(0).front().isDigit(); };
    for (int a = 0; a < order.size(); ++a) {
        InkSymbol &s = syms[order[a]];
        qsizetype x = s.cands.indexOf("x");
        if (s.cands.value(0) == "×" && x > 0 && !(digit(a - 1) && digit(a + 1))) s.cands.move(x, 0), s.probs.move(x, 0);
    }
    for (int a = 0; a < order.size(); ++a)
        for (const QString &w : kWords) {
            if (a + w.size() > order.size()) continue;
            bool ok = true;
            double ratio = 1;
            for (int k = 0; k < w.size() && ok; ++k) {
                const InkSymbol &s = syms[order[a + k]];
                qsizetype j = s.cands.indexOf(QString(w[k]));
                ok = j >= 0 && j < 4 && (k == 0 || next(order[a + k - 1], order[a + k]));
                if (ok) ratio *= s.probs.value(j) / std::max(s.probs.value(0), 1e-12);
            }
            if (!ok || ratio < 0.01) continue;
            for (int k = 0; k < w.size(); ++k) {
                InkSymbol &s = syms[order[a + k]];
                qsizetype j = s.cands.indexOf(QString(w[k]));
                s.cands.move(j, 0);
                s.probs.move(j, 0);
            }
            a += int(w.size()) - 1;
            break;
        }
}
}   // namespace

std::vector<float> inkFeatures(const QList<QPolygonF> &in) {
    QList<QPolygonF> strokes;
    for (const QPolygonF &s : in)
        if (!s.isEmpty()) strokes << s;
    std::vector<float> f;
    f.reserve(kFeatures);
    if (strokes.isEmpty()) return std::vector<float>(kFeatures);
    QRectF b = boxOf(strokes);
    double side = std::max({b.width(), b.height(), 1e-9});
    QPointF off = b.topLeft() - QPointF((side - b.width()) / 2, (side - b.height()) / 2);   // centred in the unit square
    auto unit = [&](QPointF p) { return (p - off) / side; };
    QList<int> ids;   // the path: kSeq points with a pen-up mark where a new stroke starts
    QPolygonF seq = resample(strokes, kSeq, &ids);
    for (int i = 0; i < kSeq; ++i) {
        QPointF p = unit(seq[i]);
        f.push_back(float(p.x()));
        f.push_back(float(p.y()));
        f.push_back(i > 0 && ids[i] != ids[i - 1] ? 1.f : 0.f);
    }
    std::vector<float> grid(kGrid * kGrid);   // the picture: cells the ink passes through
    auto mark = [&](QPointF p) {
        QPointF q = unit(p);
        int x = std::clamp(int(q.x() * kGrid), 0, kGrid - 1), y = std::clamp(int(q.y() * kGrid), 0, kGrid - 1);
        grid[size_t(y) * kGrid + x] = 1;
    };
    double total = 0;
    for (const QPolygonF &s : strokes) {
        mark(s[0]);
        for (int i = 1; i < s.size(); ++i) {
            int n = int(QLineF(s[i - 1], s[i]).length() / side * kGrid * 3) + 1;
            for (int k = 1; k <= n; ++k) mark(s[i - 1] + (s[i] - s[i - 1]) * (double(k) / n));
        }
        total += length(s);
    }
    f.insert(f.end(), grid.begin(), grid.end());
    f.push_back(float(std::clamp(std::log((b.width() + 0.01 * side) / (b.height() + 0.01 * side)) / 3, -1.5, 1.5)));
    f.push_back(float(std::min<qsizetype>(strokes.size(), 6)) / 6);
    f.push_back(float(std::min(total / side / 8, 2.0)));
    return f;
}

QList<QPolygonF> inkOf(const QList<QPolygonF> &ink, const QList<int> &strokes) {
    QList<QPolygonF> s;
    for (int i : strokes) s << ink[i];
    return s;
}

std::vector<float> hiddenOf(const Mlp &m, const std::vector<float> &x) {
    std::vector<float> h(m.h);
    for (int j = 0; j < m.h; ++j) {
        const float *w = &m.w1[qsizetype(j) * m.d];
        float s = m.b1[j];
        for (int i = 0; i < m.d; ++i) s += w[i] * x[i];
        h[j] = s > 0 ? s : 0;
    }
    return h;
}

std::vector<float> outputOf(const Mlp &m, const std::vector<float> &h) {
    int c = int(m.classes.size());
    std::vector<float> z(c);
    float mx = -1e30f, sum = 0;
    for (int k = 0; k < c; ++k) {
        const float *w = &m.w2[qsizetype(k) * m.h];
        float s = m.b2[k];
        for (int j = 0; j < m.h; ++j) s += w[j] * h[j];
        z[k] = s;
        mx = std::max(mx, s);
    }
    for (float &v : z) sum += v = std::exp(v - mx);
    for (float &v : z) v /= sum;
    return z;
}

std::vector<float> mlpProbs(const Mlp &m, const std::vector<float> &features) { return outputOf(m, hiddenOf(m, features)); }

Mlp loadMlp(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QDataStream d(&f);
    d.setVersion(QDataStream::Qt_6_0);
    d.setFloatingPointPrecision(QDataStream::SinglePrecision);
    QByteArray magic;
    qint32 v = 0;
    Mlp m;
    d >> magic >> v >> m.d >> m.h >> m.classes >> m.w1 >> m.b1 >> m.w2 >> m.b2;
    qsizetype c = m.classes.size();
    if (d.status() != QDataStream::Ok || magic != "MLP" || v != 1 || m.d != kFeatures || m.w1.size() != qsizetype(m.h) * m.d ||
        m.b1.size() != m.h || m.w2.size() != c * m.h || m.b2.size() != c) return {};
    return m;
}

bool saveMlp(const Mlp &m, const QString &path) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QDataStream d(&f);
    d.setVersion(QDataStream::Qt_6_0);
    d.setFloatingPointPrecision(QDataStream::SinglePrecision);
    d << QByteArray("MLP") << qint32(1) << qint32(m.d) << qint32(m.h) << m.classes << m.w1 << m.b1 << m.w2 << m.b2;
    return d.status() == QDataStream::Ok && f.commit();
}

Template makeTemplate(const QString &sym, const QList<QPolygonF> &strokes) {
    QRectF box = boxOf(strokes);
    double total = 0, k = 1 / std::max({box.width(), box.height(), 1e-9});
    for (const QPolygonF &s : strokes) total += length(s);
    Template t{sym, {}};
    for (const QPolygonF &s : strokes) {   // each stroke resampled, N points shared out by length
        if (s.isEmpty()) continue;
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

namespace {
// What recognition and calibration share: which strokes can be one symbol, and where dots go.
struct Strokes {
    const QList<QPolygonF> &ink;
    double h = 1;      // typical stroke size
    QList<int> idx;    // the strokes the segmentation splits, in writing order: no dots
    QList<int> dots;

    // dotSize: below it (of the typical stroke size) a stroke is a dot
    explicit Strokes(const QList<QPolygonF> &in, double dotSize = 0.3) : ink(in), dotSize(dotSize) {
        QList<double> sizes;
        for (const QPolygonF &s : ink)
            if (!s.isEmpty()) sizes << std::max(s.boundingRect().width(), s.boundingRect().height());
        h = median(sizes);
        for (int i = 0; i < ink.size(); ++i)
            if (!ink[i].isEmpty()) (dot(ink[i].boundingRect()) ? dots : idx) << i;
    }
    double dotSize;
    bool gapsOk = false;   // calibration, last resort: a gap inside a symbol only makes it 20x less likely
    bool dot(const QRectF &b) const { return std::max(b.width(), b.height()) < dotSize * h; }   // ponytail: calibration knob
    // log bonus for a stroke with box b joining a symbol with box gb; kNever: it cannot
    double link(const QRectF &gb, const QRectF &b) const {
        double m = 0.05 * h, below = flat(b) && flat(gb) ? 0.8 * h : 0.3 * h;   // the bars of "=" are further apart
        bool crossing = b.adjusted(-m, -m, m, m).intersects(gb);
        if (b.top() > gb.bottom() + below || b.bottom() < gb.top() - 0.6 * h) return kNever;
        if (flat(b) != flat(gb) && !crossing) return kNever;   // a bar under or over a symbol: a fraction bar
        double overlap = std::min(b.right(), gb.right()) - std::max(b.left(), gb.left()) + 2 * m;
        if (overlap < 0) return gapsOk ? -std::log(20.0) : kNever;   // a gap between them: side by side ("li")
        // ponytail: calibration knobs. Crossing strokes (x, +, t) and a pair of bars (=) are one symbol unless that
        // is 10x less likely than two; strokes overlapping by 30% of the narrower (5, π, ε) unless 3x less likely;
        // strokes that only touch (an r's arc, an m's second hump; or two cramped letters): the classifier decides
        if (overlap < 0.3 * (std::min(b.width(), gb.width()) + 2 * m)) return 0.0;
        return crossing || (flat(b) && flat(gb)) ? std::log(10.0) : std::log(3.0);
    }
    double bonus(const QList<int> &g) const {   // each stroke joining the ones before it; kNever: not one symbol
        double sum = 0;
        QRectF gb = ink[g[0]].boundingRect();
        for (int t = 1; t < g.size(); ++t) {
            double l = link(gb, ink[g[t]].boundingRect());
            if (l <= kNever) return kNever;
            sum += l;
            gb |= ink[g[t]].boundingRect();
        }
        return sum;
    }
    // A dot belongs to a symbol when it is above it (the i's of "sin", dotted whenever), or inside its box (a stray
    // tick of the pen); nearest by centre, and a symbol with a dot already counts as further (two i's side by
    // side get one each). anywhere: every dot to its nearest symbol. Returns the dots left over.
    QList<int> attachDots(QList<QList<int>> &groups, bool anywhere = false) const {
        QList<QRectF> boxes;
        QList<bool> dotted;
        for (const QList<int> &g : groups) boxes << boxOf(inkOf(ink, g)), dotted << false;
        QList<int> left;
        for (int d : dots) {
            QRectF db = ink[d].boundingRect();
            int best = -1;
            auto far = [&](int k) { return std::abs(db.center().x() - boxes[k].center().x()) + (dotted[k] ? h : 0); };
            for (int g = 0; g < groups.size(); ++g) {
                QRectF gb = boxes[g];
                bool above = db.center().x() > gb.left() - 0.6 * h && db.center().x() < gb.right() + 0.6 * h &&
                             db.bottom() < gb.top() + 0.25 * gb.height() && db.bottom() > gb.top() - 1.5 * h;
                if ((anywhere || above || gb.contains(db.center())) && (best < 0 || far(g) < far(best))) best = g;
            }
            if (best < 0) {
                left << d;
                continue;
            }
            groups[best] << d;
            std::sort(groups[best].begin(), groups[best].end());
            boxes[best] |= db;
            dotted[best] = true;
        }
        return left;
    }
};
}   // namespace

QList<InkSymbol> recognizeSymbols(const QList<QPolygonF> &ink, const QList<Template> &personal) {
    // Segmentation: the strokes, in writing order, are split into symbols of up to 4 consecutive strokes so that
    // the whole row is the most likely reading: the sum of the symbols' log-probabilities, plus each stroke's
    // bonus for joining the ones before it (Strokes::link). Dots stay out of it and are attached afterwards.
    const Mlp &model = classifier(personal);
    Strokes st(ink);
    const int n = int(st.idx.size());
    std::vector<double> best(n + 1, -1e18);
    std::vector<int> from(n + 1, -1);
    best[0] = 0;
    for (int j = 1; j <= n; ++j)
        for (int start = std::max(0, j - 4); start < j; ++start) {   // the last symbol: strokes start..j-1
            QList<int> g = st.idx.mid(start, j - start);
            double bonus = st.bonus(g);
            if (bonus <= kNever || best[start] <= -1e17) continue;
            auto cls = classify(inkOf(ink, g), model);
            double score = best[start] + bonus - (cls.isEmpty() ? 30 : cls[0].first);
            if (score > best[j]) best[j] = score, from[j] = start;
        }
    QList<QList<int>> groups;
    for (int j = n; j > 0; j = from[j]) groups.prepend(st.idx.mid(from[j], j - from[j]));
    QList<int> loose = st.attachDots(groups);   // other dots stay: a decimal point, a product dot
    QList<InkSymbol> out;
    for (const QList<int> &g : groups) out << symbolOf(ink, g, personal);
    for (int d : loose) out.append({{d}, ink[d].boundingRect(), {"."}, {1}});
    std::sort(out.begin(), out.end(), [](const InkSymbol &a, const InkSymbol &b) { return a.strokes[0] < b.strokes[0]; });
    readWords(out, st.h);
    return out;
}

static QList<QList<int>> alignSymbols(const QList<QPolygonF> &ink, const QStringList &expected, const QList<Template> &personal, double dotSize, bool gapsOk) {
    // As recognizeSymbols, but into exactly the expected symbols, in order, each scored by its probability of
    // being the symbol it should be
    const Mlp &m = classifier(personal);
    Strokes st(ink, dotSize);
    st.gapsOk = gapsOk;
    const int n = int(st.idx.size()), N = int(expected.size());
    if (N == 0 || n < N || m.d == 0) return {};
    QList<qsizetype> want;
    for (const QString &e : expected) want << m.classes.indexOf(e);
    auto at = [&](int j, int k) { return size_t(j) * (N + 1) + k; };
    std::vector<double> f(size_t(n + 1) * (N + 1), -1e18);
    std::vector<int> from(f.size(), -1);
    f[at(0, 0)] = 0;
    // log-probability of strokes g being expected symbol k
    auto logp = [&](const QList<int> &g, int k) {
        std::vector<float> p = probsOf(m, inkOf(ink, g));
        // a symbol it does not know yet (a new personal one): neither likely nor ruled out
        return want[k] < 0 ? std::log(0.05) : std::log(std::max(double(p[want[k]]), 1e-6));
    };
    for (int j = 1; j <= n; ++j)
        for (int start = std::max(0, j - 6); start < j; ++start) {   // up to 6 strokes: careful writers add serifs
            QList<int> g = st.idx.mid(start, j - start);
            double bonus = st.bonus(g);
            if (bonus <= kNever) continue;
            for (int k = 1; k <= std::min(N, j); ++k) {
                if (f[at(start, k - 1)] <= -1e17) continue;
                double score = f[at(start, k - 1)] + bonus + logp(g, k - 1);
                if (score > f[at(j, k)]) f[at(j, k)] = score, from[at(j, k)] = start;
            }
        }
    if (f[at(n, N)] <= -1e17) return {};
    QList<QList<int>> groups;
    for (int j = n, k = N; k > 0; --k) {
        int s = from[at(j, k)];
        groups.prepend(st.idx.mid(s, j - s));
        j = s;
    }
    // Dots (the prompts have none of their own): each to the nearby symbol it makes most likely to be what it
    // should be (an i gains a lot from its dot, an o next to it nothing); nearby: as for recognition, else any
    for (int d : st.dots) {
        QRectF db = ink[d].boundingRect();
        int best = -1;
        double gain = -1e18;
        for (int pass = 0; pass < 2 && best < 0; ++pass)
            for (int k = 0; k < N; ++k) {
                QRectF gb = boxOf(inkOf(ink, groups[k]));
                bool near = db.center().x() > gb.left() - 0.6 * st.h && db.center().x() < gb.right() + 0.6 * st.h &&
                            db.bottom() > gb.top() - 1.5 * st.h && db.top() < gb.bottom() + 0.3 * st.h;
                if (pass == 0 && !near) continue;
                QList<int> with = groups[k];
                with << d;
                double gk = logp(with, k) - logp(groups[k], k);
                if (gk > gain) gain = gk, best = k;
            }
        groups[best] << d;
        std::sort(groups[best].begin(), groups[best].end());
    }
    return groups;
}

QList<QList<int>> alignSymbols(const QList<QPolygonF> &ink, const QStringList &expected, const QList<Template> &personal) {
    // dots bigger than recognition's go wherever they help most; if that leaves too few strokes (a small first
    // stroke of an r taken for a dot): recognition's dots; if a symbol still cannot be one (an r whose arc has a
    // gap to its stem): gaps allowed, at a price
    for (auto [dotSize, gapsOk] : {std::pair{0.45, false}, {0.3, false}, {0.3, true}})
        if (QList<QList<int>> g = alignSymbols(ink, expected, personal, dotSize, gapsOk); !g.isEmpty()) return g;
    return {};
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
        // this run's baseline and symbol height (bars and dots left out), for telling powers apart
        QList<double> bottoms, heights;
        static const QString kTails = "gjpqyβγμρφχψζη";   // their baseline is at about 60% of their height
        for (int i : ids)
            if (!flat(box(i)) && label(i) != ".")
                bottoms << (label(i).size() == 1 && kTails.contains(label(i)) ? box(i).top() + 0.6 * box(i).height() : box(i).bottom()),
                    heights << box(i).height();
        double base = median(bottoms), hh = heights.isEmpty() ? h : median(heights);
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
            // power: smaller symbols right after it, clearly above the baseline: their centre higher than 0.6 of a
            // symbol height (an "=" or "-" sits at about 0.45; a y's tail does not matter)
            QList<int> sup;
            double right = sb.right();
            for (int i : rest()) {
                QRectF b = box(i);
                if (bottoms.isEmpty() || b.left() > right + 0.6 * h || b.center().y() > base - 0.6 * hh || b.height() > 0.9 * height(s)) break;
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

InkSymbol symbolOf(const QList<QPolygonF> &ink, const QList<int> &strokes, const QList<Template> &personal) {
    QList<QPolygonF> s = inkOf(ink, strokes);
    InkSymbol out{strokes, boxOf(s), {}, {}};
    for (const auto &[cost, sym] : classify(s, classifier(personal))) out.cands << sym, out.probs << std::exp(-cost);
    if (out.cands.isEmpty()) out.cands << "?", out.probs << 0;
    return out;
}

Guess guessSymbol(const QList<QPolygonF> &strokes, const QString &expected, const QList<Template> &personal) {
    const Mlp &m = classifier(personal);
    if (m.d == 0) return {"?", 0};
    std::vector<float> p = probsOf(m, strokes);
    int best = int(std::max_element(p.begin(), p.end()) - p.begin());
    qsizetype e = m.classes.indexOf(expected);
    QString sym = m.classes[best];
    if ((sym == "×" && expected == "x") || (sym == "x" && expected == "×")) sym = expected;
    return {sym, e < 0 ? 0.0 : double(p[e])};
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
