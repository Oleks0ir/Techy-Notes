// One-off: HWRT symbol data (Martin Thoma, zenodo.org/records/50022, ODbL 1.0) -> the built-in recognizer data.
// Usage: hwrt2tpl <dir with symbols.csv, train-data.csv, test-data.csv> <out dir>
// Writes <out>/symbols.tpl: kPerSymbol samples of each symbol we recognize, one per writer (the classifier's
// replay set when it learns the user's samples), and <out>/inkmlp.bin: the symbol classifier, trained on up to
// kTrainCap samples per symbol, randomly distorted each pass, and checked on HWRT's test split.
// Symbols HWRT lacks (= ( ) t) come from drawn, jittered shapes. Rerun only to change the symbol set or the
// classifier; the outputs are checked in.
#include "../src/Ink.h"
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>
#include <QTransform>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

static constexpr int kPerSymbol = 20, kTrainCap = 1500, kTrainMin = 400;   // templates; training samples per symbol
static constexpr int kHidden = 256, kEpochs = 16, kBatch = 64;

// HWRT's LaTeX name -> dock text. ponytail: no upper-case Latin (same shapes as lower case; needs size from layout)
static const QHash<QString, QString> kWanted = [] {
    QHash<QString, QString> m;
    for (char c = '0'; c <= '9'; ++c) m[QString(c)] = QString(c);
    for (char c = 'a'; c <= 'z'; ++c) m[QString(c)] = QString(c);
    for (const char *c : {"+", "-", "/", "|", "[", "]", "<", ">"}) m[c] = c;
    const char *named[][2] = {{"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"varepsilon", "ε"},
        {"epsilon", "ε"}, {"zeta", "ζ"}, {"eta", "η"}, {"theta", "θ"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"},
        {"nu", "ν"}, {"pi", "π"}, {"rho", "ρ"}, {"sigma", "σ"}, {"tau", "τ"}, {"varphi", "φ"}, {"phi", "φ"},
        {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"}, {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"},
        {"Lambda", "Λ"}, {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"}, {"cdot", "·"}, {"times", "×"},
        {"int", "∫"}, {"sum", "∑"}, {"sqrt{}", "√"}, {"infty", "∞"}, {"rightarrow", "→"}};
    for (auto &[l, s] : named) m["\\" + QString(l)] = QString::fromUtf8(s);
    return m;
}();

struct Sample { QString sym; QList<QPolygonF> strokes; QString writer; };

// Drawn shapes for what HWRT lacks, n jittered variants each
static QList<Sample> drawn(int n, quint32 seed) {
    QList<Sample> out;
    QRandomGenerator rnd(seed);
    auto j = [&](double a = 0.08) { return (rnd.generateDouble() - 0.5) * a; };
    auto arc = [&](bool open) {   // "(" bulges left, ")" right
        QPolygonF a;
        double bulge = 0.25 + j(0.2);
        for (int i = 0; i <= 16; ++i) {
            double t = M_PI * i / 16;
            a << QPointF((open ? bulge - bulge * qSin(t) : bulge * qSin(t)) + j(0.02), (1 - qCos(t)) / 2);
        }
        return a;
    };
    for (int v = 0; v < n; ++v) {
        out.append({"=", {QPolygonF{{j(), j()}, {1 + j(), j()}}, QPolygonF{{j(), 0.4 + j(0.2)}, {1 + j(), 0.4 + j(0.2)}}}});
        out.append({"(", {arc(true)}});
        out.append({")", {arc(false)}});
        double x = 0.45 + j(0.1);
        QPolygonF stem{{x + j(), 0}, {x, 0.85}, {x + 0.15, 1}, {x + 0.35 + j(), 0.92 + j()}}, bar{{0.1 + j(), 0.3 + j()}, {0.85 + j(), 0.3 + j()}};
        if (v % 3 == 2) stem = QPolygonF{{x + j(), 0}, {x + j(0.04), 1}};   // a plain cross
        out.append({"t", v % 2 ? QList<QPolygonF>{stem, bar} : QList<QPolygonF>{bar, stem}});
    }
    return out;
}

static QList<Sample> readCsv(const QString &path, const QHash<QString, QString> &symOf) {
    QList<Sample> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    for (QByteArray line; !(line = f.readLine()).isEmpty();) {
        int a = line.indexOf(';'), b = line.indexOf(';', a + 1), c = line.indexOf(';', b + 1);
        if (a < 0 || b < 0 || c < 0) continue;
        QString sym = symOf.value(line.left(a));
        if (sym.isEmpty()) continue;
        Sample s{sym, {}, QString::fromLatin1(line.mid(a + 1, b - a - 1))};
        for (const QJsonValue &st : QJsonDocument::fromJson(line.mid(b + 1, c - b - 1)).array()) {
            QPolygonF poly;
            for (const QJsonValue &p : st.toArray()) poly << QPointF(p["x"].toDouble(), p["y"].toDouble());
            if (!poly.isEmpty()) s.strokes << poly;
        }
        if (!s.strokes.isEmpty()) out << s;
    }
    return out;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3) return std::puts("usage: hwrt2tpl <hwrt dir> <out dir>"), 2;
    QString dir = argv[1], outDir = argv[2];
    QHash<QString, QString> symOf;   // symbol_id -> dock text
    QFile sf(dir + "/symbols.csv");
    if (!sf.open(QIODevice::ReadOnly)) return std::puts("no symbols.csv"), 1;
    for (QByteArray line; !(line = sf.readLine()).isEmpty();) {
        QList<QByteArray> f = line.trimmed().split(';');
        if (f.size() >= 2 && kWanted.contains(QString::fromUtf8(f[1]))) symOf[f[0]] = kWanted[QString::fromUtf8(f[1])];
    }
    QList<Sample> train = readCsv(dir + "/train-data.csv", symOf), test = readCsv(dir + "/test-data.csv", symOf);
    if (train.isEmpty() || test.isEmpty()) return std::puts("no train-data.csv / test-data.csv"), 1;
    std::printf("%d training, %d test samples\n", int(train.size()), int(test.size()));

    // templates: one per writer, kPerSymbol per symbol; training: up to kTrainCap per symbol
    QList<Template> tpl;
    QHash<QString, QSet<QString>> writers;
    QHash<QString, QList<int>> bySym;
    for (int i = 0; i < train.size(); ++i) {
        const Sample &s = train[i];
        if (bySym[s.sym].size() < kTrainCap) bySym[s.sym] << i;
        if (writers[s.sym].size() < kPerSymbol && !writers[s.sym].contains(s.writer)) writers[s.sym].insert(s.writer), tpl << makeTemplate(s.sym, s.strokes);
    }
    for (const Sample &s : drawn(6, 42)) tpl << makeTemplate(s.sym, s.strokes);
    for (const Sample &s : drawn(kTrainMin, 7)) bySym[s.sym] << int(train.size()), train << s;
    test << drawn(30, 99);
    if (!saveTemplates(tpl, outDir + "/symbols.tpl")) return std::puts("cannot write symbols.tpl"), 1;

    Mlp m;
    m.classes = bySym.keys();
    std::sort(m.classes.begin(), m.classes.end());
    m.d = int(inkFeatures({QPolygonF{{0, 0}, {1, 1}}}).size());
    m.h = kHidden;
    const int D = m.d, H = m.h, C = int(m.classes.size());
    std::mt19937 rng(1);
    std::normal_distribution<float> gauss(0, 1);
    m.w1.resize(qsizetype(H) * D);
    m.b1.assign(H, 0);
    m.w2.resize(qsizetype(C) * H);
    m.b2.assign(C, 0);
    for (float &w : m.w1) w = gauss(rng) * std::sqrt(2.f / D);   // He initialisation
    for (float &w : m.w2) w = gauss(rng) * std::sqrt(1.f / H);
    QHash<QString, int> classOf;
    for (int c = 0; c < C; ++c) classOf[m.classes[c]] = c;

    // random affine distortion: rotation ±8°, scale 0.85-1.15 per axis, shear ±0.15
    std::uniform_real_distribution<double> uni(-1, 1);
    auto distort = [&](const QList<QPolygonF> &strokes) {
        QTransform t;
        t.rotate(8 * uni(rng));
        t.scale(1 + 0.15 * uni(rng), 1 + 0.15 * uni(rng));
        t.shear(0.15 * uni(rng), 0);
        QList<QPolygonF> out;
        for (const QPolygonF &s : strokes) out << t.map(s);
        return out;
    };
    auto evaluate = [&] {
        int top1 = 0, top3 = 0;
        for (const Sample &s : test) {
            std::vector<float> p = mlpProbs(m, inkFeatures(s.strokes));
            int c = classOf.value(s.sym, -1), better = 0;
            for (float q : p) better += q > p[c];
            top1 += better == 0;
            top3 += better < 3;
        }
        std::printf("  test: top-1 %.1f%%, top-3 %.1f%%\n", 100.0 * top1 / test.size(), 100.0 * top3 / test.size());
        std::fflush(stdout);
    };

    // Adam, minibatches, every symbol at least kTrainMin times a pass (small ones repeated, distorted anew)
    std::vector<float *> params{m.w1.data(), m.b1.data(), m.w2.data(), m.b2.data()};
    std::vector<qsizetype> sizes{m.w1.size(), m.b1.size(), m.w2.size(), m.b2.size()};
    std::vector<std::vector<float>> g(4), m1(4), m2(4);
    for (int k = 0; k < 4; ++k) g[k].resize(sizes[k]), m1[k].resize(sizes[k]), m2[k].resize(sizes[k]);
    int step = 0;
    for (int epoch = 1; epoch <= kEpochs; ++epoch) {
        QList<int> order;
        for (const QList<int> &idx : bySym)
            for (int k = 0; k < std::max<qsizetype>(idx.size(), kTrainMin); ++k) order << idx[k % idx.size()];
        std::shuffle(order.begin(), order.end(), rng);
        float lr = 1e-3f * std::pow(0.85f, epoch - 1);
        double loss = 0;
        for (int at = 0; at < order.size(); at += kBatch) {
            for (auto &v : g) std::fill(v.begin(), v.end(), 0.f);
            int n = std::min<int>(kBatch, int(order.size()) - at);
            for (int b = 0; b < n; ++b) {
                const Sample &s = train[order[at + b]];
                std::vector<float> x = inkFeatures(distort(s.strokes)), h = hiddenOf(m, x), p = outputOf(m, h), dh(H, 0);
                int c = classOf[s.sym];
                loss -= std::log(std::max(p[c], 1e-12f));
                for (int k = 0; k < C; ++k) {
                    float e = p[k] - (k == c);
                    float *gw = &g[2][size_t(k) * H];
                    const float *w = &m.w2[size_t(k) * H];
                    for (int j = 0; j < H; ++j) gw[j] += e * h[j], dh[j] += e * w[j];
                    g[3][k] += e;
                }
                for (int j = 0; j < H; ++j) {
                    if (h[j] <= 0) continue;
                    float *gw = &g[0][size_t(j) * D];
                    for (int i = 0; i < D; ++i) gw[i] += dh[j] * x[i];
                    g[1][j] += dh[j];
                }
            }
            ++step;
            float c1 = 1 - std::pow(0.9f, step), c2 = 1 - std::pow(0.999f, step);
            for (int k = 0; k < 4; ++k)
                for (qsizetype i = 0; i < sizes[k]; ++i) {
                    float gi = g[k][i] / n;
                    m1[k][i] = 0.9f * m1[k][i] + 0.1f * gi;
                    m2[k][i] = 0.999f * m2[k][i] + 0.001f * gi * gi;
                    params[k][i] -= lr * (m1[k][i] / c1) / (std::sqrt(m2[k][i] / c2) + 1e-8f);
                }
        }
        std::printf("pass %d/%d: loss %.3f\n", epoch, kEpochs, loss / order.size());
        evaluate();
    }
    if (!saveMlp(m, outDir + "/inkmlp.bin")) return std::puts("cannot write inkmlp.bin"), 1;
    std::printf("%d classes, %d templates written\n", C, int(tpl.size()));
}
