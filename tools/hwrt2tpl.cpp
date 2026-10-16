// One-off: HWRT symbol data (Martin Thoma, zenodo.org/records/50022, ODbL 1.0) -> data/symbols.tpl.
// Usage: hwrt2tpl <dir with symbols.csv and train-data.csv> <out.tpl>
// Takes up to kPerSymbol samples of each symbol we recognize, one per writer, and adds drawn shapes for the
// symbols HWRT lacks (= ( ) t). Rerun only to change the symbol set; the output is checked in.
#include "../src/Ink.h"
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QtMath>
#include <cstdio>

static constexpr int kPerSymbol = 20;   // samples per symbol

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

// Drawn shapes for what HWRT lacks, a few jittered variants each
static QList<Template> drawn() {
    QList<Template> out;
    QRandomGenerator rnd(42);
    auto j = [&] { return (rnd.generateDouble() - 0.5) * 0.08; };
    auto arc = [&](bool open) {   // "(" bulges left, ")" right
        QPolygonF a;
        for (int i = 0; i <= 16; ++i) {
            double t = M_PI * i / 16;
            a << QPointF((open ? 0.35 - 0.35 * qSin(t) : 0.35 * qSin(t)) + j() / 4, (1 - qCos(t)) / 2);
        }
        return a;
    };
    for (int v = 0; v < 6; ++v) {
        out << makeTemplate("=", {QPolygonF{{0, j()}, {1, j()}}, QPolygonF{{j(), 0.4 + j()}, {1 + j(), 0.4 + j()}}});
        out << makeTemplate("(", {arc(true)}) << makeTemplate(")", {arc(false)});
        QPolygonF stem{{0.45 + j(), 0}, {0.45, 0.85}, {0.6, 1}, {0.8 + j(), 0.92}}, bar{{0.1 + j(), 0.3 + j()}, {0.85 + j(), 0.3 + j()}};
        out << makeTemplate("t", v % 2 ? QList<QPolygonF>{stem, bar} : QList<QPolygonF>{bar, stem});
    }
    return out;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3) return std::puts("usage: hwrt2tpl <hwrt dir> <out.tpl>"), 2;
    QString dir = argv[1];
    QHash<QString, QString> symOf;   // symbol_id -> dock text
    QFile sf(dir + "/symbols.csv");
    if (!sf.open(QIODevice::ReadOnly)) return std::puts("no symbols.csv"), 1;
    for (QByteArray line; !(line = sf.readLine()).isEmpty();) {
        QList<QByteArray> f = line.trimmed().split(';');
        if (f.size() >= 2 && kWanted.contains(QString::fromUtf8(f[1]))) symOf[f[0]] = kWanted[QString::fromUtf8(f[1])];
    }
    QHash<QString, int> count;
    QHash<QString, QSet<QByteArray>> writers;   // one sample per writer and symbol: more different hands
    QList<Template> out;
    QFile tf(dir + "/train-data.csv");
    if (!tf.open(QIODevice::ReadOnly)) return std::puts("no train-data.csv"), 1;
    for (QByteArray line; !(line = tf.readLine()).isEmpty();) {
        int a = line.indexOf(';'), b = line.indexOf(';', a + 1), c = line.indexOf(';', b + 1);
        if (a < 0 || b < 0 || c < 0) continue;
        QString sym = symOf.value(line.left(a));
        QByteArray user = line.mid(a + 1, b - a - 1);
        if (sym.isEmpty() || count[sym] >= kPerSymbol || writers[sym].contains(user)) continue;
        QList<QPolygonF> strokes;
        for (const QJsonValue &st : QJsonDocument::fromJson(line.mid(b + 1, c - b - 1)).array()) {
            QPolygonF poly;
            for (const QJsonValue &p : st.toArray()) poly << QPointF(p["x"].toDouble(), p["y"].toDouble());
            if (!poly.isEmpty()) strokes << poly;
        }
        if (strokes.isEmpty()) continue;
        out << makeTemplate(sym, strokes);
        ++count[sym];
        writers[sym].insert(user);
    }
    out << drawn();
    if (!saveTemplates(out, argv[2])) return std::puts("cannot write output"), 1;
    for (auto it = count.begin(); it != count.end(); ++it) std::printf("%s:%d ", qPrintable(it.key()), it.value());
    std::printf("\n%d templates\n", int(out.size()));
}
