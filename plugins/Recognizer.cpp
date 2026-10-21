// Math recognizer plug-in: handwriting -> math text. Its input "ink" (wired to the canvas's selection: Recognize
// math) reads lassoed ink into the Math graph's dock (input "editor"), where the reading is corrected and learned;
// Settings: handwriting calibration, training, random formula tests, layout learning.
#include "Ink.h"
#include "MathView.h"
#include "Plugin.h"
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <memory>
// Writing area of the handwriting calibration; pen and finger come in as Qt's synthesized mouse events.
// marks: per written symbol a frame and, under it, the guess and its certainty.
struct Pad : QWidget {
    struct Mark { QRectF box; QString text; QColor color; bool picked = false; };   // picked: selected to correct
    QList<QPolygonF> ink;   // px
    QList<Mark> marks;
    std::function<void()> lifted;   // after each stroke
    void mousePressEvent(QMouseEvent *e) override {
        ink.append(QPolygonF{e->position()});
        marks.clear();
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (ink.isEmpty() || !e->buttons()) return;
        ink.last() << e->position();
        update();
    }
    void mouseReleaseEvent(QMouseEvent *) override { if (lifted) lifted(); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(210, 210, 210), 1, Qt::DashLine));   // baseline guide
        p.drawLine(QPointF(0, height() * 0.7), QPointF(width(), height() * 0.7));
        p.setPen(QPen(Qt::black, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const QPolygonF &s : ink) s.size() == 1 ? p.drawPoint(s[0]) : p.drawPolyline(s);
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        p.setFont(small);
        for (const Mark &m : marks) {
            QColor fill = m.color;
            fill.setAlpha(40);
            p.setPen(QPen(m.color, 2));
            p.setBrush(fill);
            p.drawRoundedRect(m.box.adjusted(-4, -4, 4, 4), 4, 4);
            if (m.picked) {
                p.setPen(QPen(QColor(0, 120, 215), 3));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(m.box.adjusted(-8, -8, 8, 8), 6, 6);
            }
            p.setPen(m.color.darker(130));
            p.drawText(QRectF(m.box.center().x() - 40, m.box.bottom() + 6, 80, 20), Qt::AlignHCenter | Qt::AlignTop, m.text);
        }
    }
};

// Settings > Calibrate handwriting: 13 pages, 3 per group (basics, letters + Greek, functions, calculus) and the
// newer signs, then the formula pages (formulaTest).
// After each stroke the page is guessed with what was learned so far: every symbol framed red (wrong, or
// below 50%), yellow (50-90%) or green (90%+), the guess and certainty under it. Train stores all symbols of
// the page as personal templates and asks for it again; once all are green, Next stores and goes on.
// Skip goes on without storing, Stop testing ends; what was stored stays.
struct Prompt { QString show, glyphs; };   // glyphs: the written symbols, left to right, space-separated
static const QList<Prompt> kCalibration = {
    {"0 1 2 3 4 5 6 7 8 9", "0 1 2 3 4 5 6 7 8 9"},
    {"x² + 3x − 1 = 0", "x 2 + 3 x - 1 = 0"},
    {"y = (x + 1)(x − 4)", "y = ( x + 1 ) ( x - 4 )"},
    {"a b c d e f g h i j k l m", "a b c d e f g h i j k l m"},
    {"n o p q r s t u v w x y z", "n o p q r s t u v w x y z"},
    {"α β γ δ ε θ λ μ π σ φ ω", "α β γ δ ε θ λ μ π σ φ ω"},
    {"sin", "s i n"},
    {"sin()", "s i n ( )"},
    {"cos(x) + ln(x)", "c o s ( x ) + l n ( x )"},
    {"∫ 2x + sin(x) dx", "∫ 2 x + s i n ( x ) d x"},
    {"∑ n² = ∞", "∑ n 2 = ∞"},
    {"√    →    lim", "√ → l i m"},
    {"∂   ∏   ∮   {   }   ∇", "∂ ∏ ∮ { } ∇"},
};
static constexpr double kSure = 0.9, kUnsure = 0.5;   // green from, red below
static bool calibrateHandwriting(QWidget *parent, QList<Template> &personal, const QString &file,
                                 const QList<Prompt> &prompts = kCalibration, const QString &title = "Calibrate handwriting");
// Settings > Train a symbol (also from the calibration): one page with that symbol alone, its ink kept whole
// (an i's dot and stem stay one symbol however they are placed), repeated until it is green
static void trainSymbol(QWidget *parent, QList<Template> &personal, const QString &file) {
    QString sym = QInputDialog::getText(parent, "Train a symbol", "Which symbol? (e.g. i, x, α, ∫; or a word written joined up, e.g. sin)").remove(' ');
    if (!sym.isEmpty()) calibrateHandwriting(parent, personal, file, {{sym, sym}}, "Train \"" + sym + "\"");
}
static bool calibrateHandwriting(QWidget *parent, QList<Template> &personal, const QString &file,
                                 const QList<Prompt> &prompts, const QString &title) {
    const int count = prompts.size();
    QDialog d(parent);
    d.setWindowTitle(title);
    auto step = new QLabel, show = new QLabel, status = new QLabel;
    show->setFont(QFont("Cambria", 28));
    status->setWordWrap(true);
    auto pad = new Pad;
    pad->setMinimumSize(760, 260);
    auto clear = new QPushButton("Clear"), skip = new QPushButton("Skip"), next = new QPushButton("Train"),
         stop = new QPushButton("Stop testing");
    auto lay = new QVBoxLayout(&d);
    lay->addWidget(step);
    lay->addWidget(new QLabel("Please write:"));
    lay->addWidget(show);
    lay->addWidget(pad, 1);
    lay->addWidget(status);
    auto row = new QHBoxLayout;
    row->addWidget(clear);
    row->addStretch();
    if (&prompts == &kCalibration) {   // a symbol that keeps going wrong: train it on its own
        auto one = new QPushButton("Train one symbol...");
        row->insertWidget(1, one);
        QObject::connect(one, &QPushButton::clicked, [&] { trainSymbol(&d, personal, file); });
    }
    for (QPushButton *b : {stop, skip, next}) row->addWidget(b);
    lay->addLayout(row);
    next->setDefault(true);
    int at = 0, round = 1;
    bool green = false;
    QList<QList<int>> cols;   // the page's symbols as last scored
    QTimer settle;            // score once the pen rests
    settle.setSingleShot(true);
    settle.setInterval(600);
    pad->lifted = [&] { settle.start(); };
    auto reset = [&] {
        settle.stop();
        pad->ink.clear();
        pad->marks.clear();
        pad->update();
        cols.clear();
        green = false;
        next->setText("Train");
        next->setEnabled(false);
    };
    auto load = [&] {
        bool whole = !prompts[at].glyphs.contains(' ');
        step->setText(QString("Page %1 of %2: ").arg(at + 1).arg(count) +
                      (whole ? "write it once." : "write it in one row."));
        show->setText(prompts[at].show);
        round = 1;
        status->clear();
        reset();
    };
    QObject::connect(&settle, &QTimer::timeout, [&] {   // guess the page, compare with the prompt
        QStringList want = prompts[at].glyphs.split(' ');
        cols = alignSymbols(pad->ink, want, personal);   // the ink split into exactly the prompt's symbols
        if (want.size() == 1) {   // one symbol: all of the ink, however it falls apart
            cols = {{}};
            for (int i = 0; i < pad->ink.size(); ++i) cols[0] << i;
        }
        pad->marks.clear();
        green = cols.size() == want.size();
        if (!green)   // it cannot be split into those symbols: show how it reads instead, in grey
            for (const InkSymbol &s : recognizeSymbols(pad->ink, personal)) pad->marks.append({s.box, s.cands.value(0), Qt::gray});
        for (int k = 0; k < cols.size() && cols.size() == want.size(); ++k) {
            QList<QPolygonF> st;
            QRectF box;
            for (int i : cols[k]) st << pad->ink[i], box |= pad->ink[i].boundingRect();
            Guess g = guessSymbol(st, want[k], personal);
            double c = g.sym == want[k] ? g.certainty : std::min(g.certainty, kUnsure - 0.01);   // wrong is red
            green = green && c >= kSure;
            QColor col = c >= kSure ? QColor(46, 160, 67) : c >= kUnsure ? QColor(212, 160, 23) : QColor(207, 34, 46);
            pad->marks.append({box, QString("%1 %2%").arg(g.sym).arg(qRound(c * 100)), col});
        }
        pad->update();
        if (cols.size() != want.size()) {
            status->setText(QString("This ink cannot be split into the %1 symbols of the page (grey: how it reads). Maybe a "
                                    "symbol is missing, or one has a gap inside it. Clear and write it again, or Skip.")
                                .arg(want.size()));
            next->setText("Train");
            next->setEnabled(false);
            return;
        }
        status->setText(green ? "All symbols recognized with high certainty."
                              : QString("Round %1. Red: wrong or unsure, yellow: fairly sure, green: sure. Train learns this page, "
                                        "then write it again.").arg(round));
        next->setText(!green ? "Train" : at == count - 1 ? "Finish" : "Next");
        next->setEnabled(true);
    });
    QObject::connect(clear, &QPushButton::clicked, reset);
    QObject::connect(skip, &QPushButton::clicked, [&] { ++at == count ? d.accept() : load(); });
    QObject::connect(stop, &QPushButton::clicked, &d, &QDialog::reject);
    QObject::connect(next, &QPushButton::clicked, [&] {   // Train or Next: either way the page is learned
        QStringList want = prompts[at].glyphs.split(' ');
        if (cols.size() != want.size()) return;
        for (int k = 0; k < cols.size(); ++k) personal << makeTemplate(want[k], inkOf(pad->ink, cols[k]));
        if (!saveTemplates(personal, file)) QMessageBox::warning(&d, "Techy-Notes", "Could not save " + file);
        if (green) return (void)(++at == count ? d.accept() : load());
        ++round;
        status->setText(QString("Learned. Round %1: write the page again.").arg(round));
        reset();
    });
    load();
    return d.exec() == QDialog::Accepted;
}

static QPixmap mathPixmap(const QString &text, QSize size) {   // text typeset black on white, centred, at most 48 px a line
    QPixmap pm(size);
    pm.fill(Qt::white);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::black);
    if (QSizeF s = mathSize("eq\n" + text); !s.isEmpty()) {
        QSizeF z = s * std::min({size.width() / s.width(), size.height() / s.height(), 48.0});
        drawMath(p, "eq\n" + text, QRectF(QPointF((size.width() - z.width()) / 2, (size.height() - z.height()) / 2), z));
    }
    return pm;
}

// Corrections of recognized symbols, shared by the math dock and the formula pages.
// What the picked symbols are: one picked, its alternatives (or typed); several, one character each ("sin" for three)
// or one symbol to merge them; merge: always one symbol. Empty: cancelled, or a count that fits neither (said so).
static QStringList askSymbols(QWidget *parent, const QList<InkSymbol> &syms, QList<int> picked, bool merge = false) {
    QString typed;
    if (picked.size() == 1 && !merge) {
        QMenu m;
        for (const QString &c : syms[picked[0]].cands.mid(1)) m.addAction(c)->setData(c);
        m.addSeparator();
        m.addAction("Other...")->setData(QString());
        QAction *a = m.exec(QCursor::pos());
        if (!a) return {};
        typed = a->data().toString();
    }
    if (typed.isEmpty())
        typed = QInputDialog::getText(parent, merge ? "Merge" : "Correct",
                                      merge ? "These are one symbol. Which? (e.g. x, ∫, lim)"
                                      : picked.size() == 1 ? "What did you write here? (e.g. x, 2, α, ∫)"
                                                           : "What did you write here? One character per symbol (e.g. sin), or one symbol to merge them")
                    .remove(' ');
    if (typed.isEmpty()) return {};
    if (merge) return {typed};
    QStringList out;   // per character; a surrogate pair stays one
    for (int k = 0; k < typed.size(); ++k) out << (typed[k].isHighSurrogate() && k + 1 < typed.size() ? typed.mid(k++, 2) : typed.mid(k, 1));
    if (picked.size() > 1 && out.size() > 1 && out.size() != picked.size()) {
        QMessageBox::information(parent, "Correct", QString("%1 symbols are selected, but \"%2\" has %3. Type one per symbol, or one to merge them.")
                                                        .arg(picked.size()).arg(typed).arg(out.size()));
        return {};
    }
    return picked.size() == 1 || out.size() == 1 ? QStringList{typed} : out;
}
static void sortByX(const QList<InkSymbol> &syms, QList<int> &picked) {
    std::sort(picked.begin(), picked.end(), [&](int a, int b) { return syms[a].box.left() < syms[b].box.left(); });
}
// The picked symbols become typed (from askSymbols): one each, or one symbol merged from all (into the leftmost).
// Returns the symbols that changed, by their index afterwards, each with its new reading first: to be learned.
static QList<int> setSymbols(QList<InkSymbol> &syms, QList<int> picked, const QStringList &typed) {
    sortByX(syms, picked);
    auto be = [](InkSymbol &s, const QString &t) { s.cands.removeAll(t), s.cands.prepend(t); };
    if (typed.size() > 1) {
        for (int k = 0; k < picked.size(); ++k) be(syms[picked[k]], typed[k]);
        return picked;
    }
    InkSymbol &s = syms[picked[0]];
    for (int k = 1; k < picked.size(); ++k) s.strokes << syms[picked[k]].strokes, s.box |= syms[picked[k]].box;
    std::sort(s.strokes.begin(), s.strokes.end());
    be(s, typed[0]);
    QList<int> gone = picked.mid(1);
    std::sort(gone.begin(), gone.end(), std::greater<int>());
    for (int i : gone) syms.removeAt(i);
    return {picked[0] - int(std::count_if(gone.begin(), gone.end(), [&](int i) { return i < picked[0]; }))};
}
// A symbol that is really several: one per stroke, classified anew. Returns the new symbols' indices.
static QList<int> splitSymbols(QList<InkSymbol> &syms, QList<int> picked, const QList<QPolygonF> &ink, const QList<Template> &personal) {
    std::sort(picked.begin(), picked.end(), std::greater<int>());
    QList<int> made;
    for (int i : picked) {
        QList<int> strokes = syms[i].strokes;
        if (strokes.size() < 2) continue;
        syms.removeAt(i);
        for (int &m : made) m += int(strokes.size()) - 1;   // those after i moved on
        for (int k = 0; k < strokes.size(); ++k) syms.insert(i + k, symbolOf(ink, {strokes[k]}, personal)), made << i + k;
    }
    return made;
}

// The calibration's formula pages, after its symbol pages: operators with their limits (recognizer spelling)
static const QStringList kFormulaPages = {"lim_(n→∞) 1/n", "lim_(x→0) sin(x)/x", "int_(0)^(1) x^2 dx", "int_(a)^(b) f(x) dx",
                                          "sum_(n=1)^(∞) 1/n^2", "sum_(k=0)^(n) k", "prod_(k=1)^(n) k"};

// Formula pages (calibration, Settings > Test with random formulas): write the formula shown; once the pen rests it
// is read with the personal layout and compared. Each symbol is framed as on the symbol pages: its reading and the
// certainty of what it should be (lined up with the formula), green / yellow / red; grey: not a symbol of the formula.
// Tap symbols in the reading under the pad to correct them (Correct, Split, Merge: learned at once); Train learns
// every symbol as what it should be, then the formula is written again. Next keeps the page in the math log for
// layout learning (its symbols named from the formula where the reading lines up with it), Skip does not.
// count < 0: endless, scored.
static void formulaTest(QWidget *parent, const QString &title, const std::function<QString(int)> &formula, int count,
                        QList<Template> &personal, const QString &file, const LayoutKnobs &knobs, QList<MathLogEntry> &log) {
    QDialog d(parent);
    d.setWindowTitle(title);
    auto step = new QLabel, show = new QLabel, status = new QLabel;
    status->setWordWrap(true);
    auto pad = new Pad;
    pad->setMinimumSize(760, 320);
    auto read = new MathView;   // how it was read; tap symbols there to correct them
    read->large = true;
    read->setMinimumHeight(110);
    auto clear = new QPushButton("Clear"), fix = new QPushButton("Correct..."), split = new QPushButton("Split"),
         merge = new QPushButton("Merge"), train = new QPushButton("Train"), skip = new QPushButton("Skip"), next = new QPushButton("Next"),
         stop = new QPushButton("Stop");
    split->setToolTip("A symbol that is really several: one per stroke");
    merge->setToolTip("Several symbols that are really one");
    train->setToolTip("Learn every symbol as what the formula says it is, then write it again");
    auto lay = new QVBoxLayout(&d);
    lay->addWidget(step);
    lay->addWidget(show);
    lay->addWidget(pad, 1);
    lay->addWidget(read);
    lay->addWidget(status);
    auto row = new QHBoxLayout;
    for (QPushButton *b : {clear, fix, split, merge}) row->addWidget(b);
    row->addStretch();
    for (QPushButton *b : {train, stop, skip, next}) row->addWidget(b);
    lay->addLayout(row);
    next->setDefault(true);
    int at = 0, right = 0, done = 0, round = 1;
    bool ok = false;
    QString want;
    QList<InkSymbol> syms;
    QList<int> picked;      // symbols selected in the reading
    QStringList should;     // per symbol: what the formula says it is; empty: not one of its symbols
    InkText got;
    QTimer settle;   // read once the pen rests
    settle.setSingleShot(true);
    settle.setInterval(900);
    pad->lifted = [&] { settle.start(); };
    auto learn = [&](int i, const QString &sym) { personal << makeTemplate(sym, inkOf(pad->ink, syms[i].strokes)); };
    auto keep = [&] {
        if (!saveTemplates(personal, file)) QMessageBox::warning(&d, "Techy-Notes", "Could not save " + file);
    };
    auto score = [&] {   // the layout, the comparison, the frames
        got = inkLayout(syms, knobs);
        ok = sameMath(got.text, want);
        QList<int> links = carryLinks(got.text, got.src, want, false);   // per character of want: the symbol it shows
        should.clear();
        pad->marks.clear();
        for (int i = 0; i < syms.size(); ++i) {
            QString chars, r = syms[i].cands.value(0);
            for (int p = 0; p < links.size(); ++p)
                if (links[p] == i) chars += want[p];
            QString e = symbolText(chars, r);
            should << e;
            if (e.isEmpty()) {
                pad->marks.append({syms[i].box, r, Qt::gray, picked.contains(i)});
                continue;
            }
            double c = guessSymbol(inkOf(pad->ink, syms[i].strokes), e, personal).certainty;
            if (r != e) c = std::min(c, kUnsure - 0.01);   // wrong is red
            QColor col = c >= kSure ? QColor(46, 160, 67) : c >= kUnsure ? QColor(212, 160, 23) : QColor(207, 34, 46);
            pad->marks.append({syms[i].box, QString("%1 %2%").arg(r).arg(qRound(c * 100)), col, picked.contains(i)});
        }
        pad->update();
        read->lines = got.text.split('\n');
        read->framed.clear();
        for (int p = 0; p < got.src.size(); ++p)
            if (picked.contains(got.src[p])) read->framed.insert(p);
        read->update();
        fix->setEnabled(!picked.isEmpty());
        split->setEnabled(!picked.isEmpty());
        merge->setEnabled(picked.size() > 1);
        train->setEnabled(!syms.isEmpty());
        next->setEnabled(!syms.isEmpty());
        status->setText(ok ? "Read right." : "Read as shown under the pad. Tap symbols there to correct them, or Train (learns every symbol as "
                                             "what it should be). Next keeps it, so the layout can learn how you write this.");
    };
    auto reset = [&] {
        settle.stop();
        pad->ink.clear();
        pad->marks.clear();
        pad->update();
        syms.clear();
        picked.clear();
        read->lines.clear();
        read->framed.clear();
        read->update();
        for (QPushButton *b : {fix, split, merge, train, next}) b->setEnabled(false);
        status->setText(round > 1 ? QString("Learned. Round %1: write it again.").arg(round) : "Write it below, as on paper.");
    };
    auto load = [&] {
        want = formula(at);
        round = 1;
        step->setText(count < 0 ? QString("Formula %1. Read right so far: %2 of %3.").arg(at + 1).arg(right).arg(done)
                                : QString("Formula %1 of %2. Please write:").arg(at + 1).arg(count));
        show->setPixmap(mathPixmap(want, {760, 120}));
        reset();
    };
    QObject::connect(&settle, &QTimer::timeout, [&] {
        syms = recognizeSymbols(pad->ink, personal);
        picked.clear();
        score();
    });
    read->tapped = [&](int pos) {
        int s = got.src.value(pos, -1);
        if (s < 0) return;
        if (!picked.removeAll(s)) picked << s;
        score();
    };
    auto correct = [&](bool one) {   // Correct, Merge
        QStringList typed = askSymbols(&d, syms, picked, one);
        if (typed.isEmpty()) return;
        for (int i : setSymbols(syms, picked, typed)) learn(i, syms[i].cands.value(0));
        keep();
        picked.clear();
        score();
    };
    QObject::connect(fix, &QPushButton::clicked, [&] { correct(false); });
    QObject::connect(merge, &QPushButton::clicked, [&] { correct(true); });
    QObject::connect(split, &QPushButton::clicked, [&] {
        splitSymbols(syms, picked, pad->ink, personal);
        picked.clear();
        score();
    });
    QObject::connect(train, &QPushButton::clicked, [&] {
        settle.stop();
        for (int i = 0; i < syms.size(); ++i)
            if (!should.value(i).isEmpty()) learn(i, should[i]);
        keep();
        ++round;
        reset();
    });
    auto advance = [&] { ++at == count ? d.accept() : load(); };
    QObject::connect(clear, &QPushButton::clicked, reset);
    QObject::connect(skip, &QPushButton::clicked, advance);
    QObject::connect(stop, &QPushButton::clicked, &d, &QDialog::reject);
    QObject::connect(next, &QPushButton::clicked, [&] {
        settle.stop();
        if (syms.isEmpty()) return;
        ++done, right += ok;
        QList<int> links = carryLinks(got.text, got.src, want, false);   // per character of want: the symbol it shows
        MathLogEntry e{pad->ink, syms, got.text, {}, want};
        for (int i = 0; i < e.symbols.size(); ++i) {
            QString chars;
            for (int p = 0; p < links.size(); ++p)
                if (links[p] == i) chars += want[p];
            QString f = symbolText(chars, e.symbols[i].cands.value(0));
            e.symbols[i].cands = {f.isEmpty() ? e.symbols[i].cands.value(0) : f};
            e.symbols[i].probs.clear();
        }
        log << e;
        advance();
    });
    load();
    d.exec();
}

// The ink being recognized and how its text was arrived at
struct Recognized {
    QList<QPolygonF> ink;
    QList<InkSymbol> syms;
    QList<Template> personal;
    QTextCursor region;              // the recognized text in the text box (follows edits around it)
    bool box = false;                // it fills a □
    QString doc;                     // the text box as last seen
    QList<int> links;                // per character of doc: the symbol it shows, -1: none
    bool writing = false;            // the layout is writing the text: not a typed edit
    bool fromPad = false;            // the ink is the dock pad's, not the page's
    int padAt = -1;                  // the pad's target: a □'s position, -1: new lines
    bool padStarted = false;         // its ink is being read into the text
    QList<int> picked;               // symbols selected in the preview
    QHash<QList<int>, QString> was;  // per symbol (its strokes): what the classifier read or was taught
    QString first;                   // the first reading
    QStringList chain;               // the readings after each change
    QList<MathLogEntry> log;
    LayoutKnobs knobs;               // fitted on the log
    quint32 link = 0;                // the analysed equation shown (its ink's Stroke::link); 0: none
    QList<std::pair<qint64, int>> keys;   // per stroke of ink: the page stroke it is (first timestamp, points); empty: the pad's
    QList<Template> taught;          // learned from its corrections since its last Insert (Insert lists them)
    QString scratch;                 // typed text of no equation: back when the equation closes
    bool loading = false;            // an equation is being opened: nothing stored meanwhile
};

class Recognizer : public QObject, public Plugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "io.github.oleks0ir.techynotes.Plugin/1")
    Q_INTERFACES(Plugin)

public:
    QString name() const override { return "Math recognizer"; }
    QList<Port> inputs() const override { return {{"ink", "ink", "Recognize math", QChar(0xE8EF)}, {"editor", "math-editor"}}; }
    void start(Host &host) override;
    void receive(const QString &in, const QVariant &v) override;

private:
    Host *host = nullptr;
    // the dock it works in, set up once. ponytail: one editor; a second math-editor plug-in needs attach() per editor
    MathEditor *ed = nullptr;
    bool wired = false;   // ed is wired to it now
    std::shared_ptr<Recognized> rec = std::make_shared<Recognized>();
    QString personalFile, logFile;
    QAction *learnOn = nullptr, *lenient = nullptr;
    std::function<void()> keepLog;
    // made by attach(): its hooks into the editor, and what the canvas and the ink input call
    std::function<bool(const QString &)> reread;
    std::function<int()> askInk;
    std::function<void(int)> inserted, tapped, doubleTapped;
    std::function<void()> editing;
    std::function<void(quint32, bool)> openEq;
    std::function<void(quint32, const QList<int> &)> recognizeInk;
    QWidget *write = nullptr, *padBox = nullptr;
    void attach();
    void wire(bool on);
};

void Recognizer::start(Host &h) {
    host = &h;
    QMainWindow *w = h.window;
    QSettings st;
    auto rec = this->rec;
    personalFile = h.rootDir + "/.templates.tpl", logFile = h.rootDir + "/.mathlog";   // all notebooks
    const QString personalFile = this->personalFile, logFile = this->logFile;
    rec->personal = loadTemplates(personalFile);
    rec->log = loadMathLog(logFile);
    if (QList<QVariant> k = st.value("ink/layout").toList(); k.size() == 4)   // fitted after the last Insert (fitting takes up to ~1 s)
        rec->knobs = {k[0].toDouble(), k[1].toDouble(), k[2].toDouble(), k[3].toDouble()};
    auto refit = [=] {
        rec->knobs = fitLayout(rec->log);
        QSettings().setValue("ink/layout", QList<QVariant>{rec->knobs.power, rec->knobs.lineGap, rec->knobs.reach, rec->knobs.span});
    };
    keepLog = [=] {   // the log saved and the layout refitted on it
        while (rec->log.size() > 300) rec->log.removeFirst();   // ponytail: the last 300 pages; enough to fit 4 knobs
        if (!saveMathLog(rec->log, logFile)) QMessageBox::warning(w, "Techy-Notes", "Could not save " + logFile);
        refit();
    };
    auto formulas = [=, keepLog = keepLog](const QString &title, const std::function<QString(int)> &formula, int count) {
        qsizetype before = rec->log.size();
        formulaTest(w, title, formula, count, rec->personal, personalFile, rec->knobs, rec->log);
        if (rec->log.size() != before) keepLog();
    };
    QMenu *settings = h.settings;
    settings->addAction("Calibrate handwriting...", [=] {   // symbols, then formulas
        if (calibrateHandwriting(w, rec->personal, personalFile))
            formulas("Calibrate handwriting: formulas", [](int i) { return kFormulaPages[i]; }, int(kFormulaPages.size()));
    });
    settings->addAction("Test with random formulas...", [=] {
        auto r = std::make_shared<QRandomGenerator>(QRandomGenerator::global()->generate());
        formulas("Test with random formulas", [r](int) { return randomFormula(*r); }, -1);
    });
    settings->addAction("Train a symbol...", [=] { trainSymbol(w, rec->personal, personalFile); });
    learnOn = settings->addAction("Learn from inserted math", [](bool on) { QSettings().setValue("ink/learnOnInsert", on); });
    learnOn->setCheckable(true);
    learnOn->setChecked(st.value("ink/learnOnInsert", true).toBool());
    learnOn->setToolTip("After Insert: offer to learn the handwritten symbols whose text you changed");
    lenient = settings->addAction("Lenient matching of edited math", [](bool on) { QSettings().setValue("ink/lenient", on); });
    lenient->setCheckable(true);
    lenient->setChecked(st.value("ink/lenient", false).toBool());
    lenient->setToolTip("Off: a retyped part (lim made into ∫) keeps no link to its ink and is not learned. On: matched to the ink by position.");
    settings->addAction("Reset layout learning...", [=] {
        if (QMessageBox::question(w, "Techy-Notes", "Forget how your handwritten formulas were laid out (powers, limits, lines) "
                                                    "and go back to the built-in layout?") != QMessageBox::Yes)
            return;
        QFile::remove(logFile);
        rec->log.clear();
        refit();
    });
    // analysed handwriting (inserted or not): a finger tap, or the menu / pen ring's Open math, opens its equation
    h.canvas->objectMenus << [this](QPointF at, int i) -> QList<Canvas::MenuItem> {
        quint32 l = i < 0 && wired ? host->canvas->equationAt(at) : 0;
        if (!l) return {};
        return {{QChar(0xE8EF), "Open math", [this, l] { openEq(l, false); }}};
    };
    h.canvas->onOpenEquation = [this](quint32 l) { if (wired) openEq(l, false); };
    h.canvas->onLeave = [this] { if (wired) openEq(0, false); };
}

void Recognizer::receive(const QString &in, const QVariant &v) {
    if (in == "ink") {   // the selection: its plain ink; with analysed ink in it, that equation grown
        QList<int> idx;
        for (int i : v.value<QList<int>>())
            if (const Stroke &s = host->canvas->sheet.strokes[i]; !s.boxed() && s.color.alpha() == 255) idx << i;
        if (wired && !idx.isEmpty()) recognizeInk(host->canvas->linkInk(idx), idx);
    } else if (in == "editor") {
        auto e = v.value<MathEditor *>();
        if (e && !ed) ed = e, attach();
        wire(e && e == ed);
    }
}

// Unwired: what it shows of an equation goes (the typed text comes back), its buttons and hooks leave the dock
void Recognizer::wire(bool on) {
    if (!ed || on == wired) return;
    if (!on) editing();
    wired = on;
    write->setVisible(on);
    if (!on) padBox->hide();
    if (on) {
        ed->reread = reread, ed->askInk = askInk, ed->inserted = inserted, ed->editing = editing;
        ed->preview->tapped = tapped, ed->preview->doubleTapped = doubleTapped;
    } else {
        ed->reread = {}, ed->askInk = {}, ed->inserted = {}, ed->editing = {};
        ed->preview->tapped = ed->selectAt, ed->preview->doubleTapped = {};
    }
}

void Recognizer::attach() {
    Canvas *canvas = host->canvas;
    QMainWindow *w = host->window;
    auto rec = this->rec;
    const QString personalFile = this->personalFile;
    auto lenient = this->lenient, learnOn = this->learnOn;
    auto keepLog = this->keepLog;
    auto mathDock = ed->dock;
    auto mathEdit = ed->edit;
    auto preview = ed->preview, plotView = ed->plot;
    auto kind = ed->kind;
    auto upd = ed->update;
    auto mathLay = ed->lay;
    auto mathRow = ed->row;
    auto selectAt = ed->selectAt;
    auto selectedBox = ed->selectedBox;
    auto write = new QPushButton("Write");
    write->setToolTip("A writing pad under the preview: what you write fills the selected □ (or new lines), read as you go");
    mathRow->insertWidget(mathRow->indexOf(upd), write);
    this->write = write;
    // Recognized ink is corrected in the preview: tap symbols (framed), then Correct, Split or Delete; each fix
    // becomes a personal template
    auto fix = new QPushButton("Correct..."), split = new QPushButton("Split"), drop = new QPushButton("Delete");
    split->setToolTip("A symbol that is really several: one per stroke (then Correct them)");
    auto stripRow = new QWidget;
    auto stripLay = new QHBoxLayout(stripRow);
    stripLay->setContentsMargins(0, 0, 0, 0);
    auto hint = new QLabel("Tap symbols below to correct them, double-tap to rewrite one. With symbols selected, ∫ ∑ ∏ lim and script buttons read them as that structure.");
    hint->setWordWrap(true);
    stripLay->addWidget(hint, 1);
    for (QPushButton *b : {fix, split, drop}) stripLay->addWidget(b), b->setEnabled(false);
    stripRow->hide();
    mathLay->insertWidget(mathLay->indexOf(mathEdit) + 1, stripRow);
    auto pad = new Pad;   // writing pad: Write opens it, Done closes it
    pad->setMinimumHeight(160);
    auto padClear = new QPushButton("Clear"), padDone = new QPushButton("Done");
    auto padBox = new QWidget;
    auto padLay = new QHBoxLayout(padBox);
    padLay->setContentsMargins(0, 0, 0, 0);
    padLay->addWidget(pad, 1);
    auto padButtons = new QVBoxLayout;
    padButtons->addWidget(padClear);
    padButtons->addWidget(padDone);
    padButtons->addStretch();
    padLay->addLayout(padButtons);
    padBox->hide();
    mathLay->insertWidget(mathLay->count() - 1, padBox);   // over the button row
    this->padBox = padBox;
    auto regionText = [=] { return rec->region.selectedText().replace(QChar::ParagraphSeparator, '\n'); };
    using Key = std::pair<qint64, int>;
    auto keyOf = [](const Stroke &s) { return Key(s.pts.value(0).t, int(s.pts.size())); };   // survives moving and resizing
    auto pageInk = [=](const QList<int> &strokes, QList<Key> &keys) {   // those page strokes as ink, and their keys
        QList<QPolygonF> ink;
        keys.clear();
        for (int i : strokes) {
            const Stroke &s = canvas->sheet.strokes[i];
            QPolygonF poly;
            for (const Pt &p : s.pts) poly << p.pos();
            ink << poly;
            keys << keyOf(s);
        }
        return ink;
    };
    // The open equation's state, saved with its sheet: the text box, the plot view, its ink (the page strokes it was
    // read from, and all of its strokes then), symbols and their links to the text, readings, what was learned.
    // Pad ink has no page strokes: then only the text is kept.
    auto storeEq = [=] {
        if (!rec->link || rec->loading) return;
        bool pad = rec->keys.size() != rec->ink.size();
        QList<Key> all;
        pageInk(canvas->equationInk(rec->link), all);
        QByteArray b;
        QDataStream d(&b, QIODevice::WriteOnly);
        d.setVersion(QDataStream::Qt_6_0);
        d << qint32(1) << mathEdit->toPlainText() << qint32(rec->region.selectionStart()) << qint32(rec->region.selectionEnd()) << rec->box
          << qint32(kind->currentIndex()) << plotView->view << all << (pad ? QList<Key>() : rec->keys) << qint32(pad ? 0 : rec->syms.size());
        for (const InkSymbol &s : pad ? QList<InkSymbol>() : rec->syms)
            d << s.strokes << s.box << s.cands << s.probs << qint8(s.role) << s.owner << rec->was.value(s.strokes);
        d << (pad ? QList<int>() : rec->links) << rec->first << rec->chain << rec->taught;
        canvas->setEquation(rec->link, b);
    };
    auto showMarks = [=] {   // the preview's frames and shading, the correction buttons
        preview->framed.clear();
        for (int p = 0; p < rec->links.size(); ++p)
            if (rec->picked.contains(rec->links[p])) preview->framed.insert(p);
        preview->filled = selectedBox();
        preview->large = !rec->syms.isEmpty();
        stripRow->setVisible(!rec->syms.isEmpty());
        for (QPushButton *b : {fix, split, drop}) b->setEnabled(!rec->picked.isEmpty());
        preview->update();
        storeEq();
    };
    QObject::connect(mathEdit, &QPlainTextEdit::textChanged, [=] {   // typed edits: the links to the ink follow them
        QString now = mathEdit->toPlainText();
        if (!rec->syms.isEmpty() && !rec->writing && now != rec->doc) {
            rec->links = carryLinks(rec->doc, rec->links, now, lenient->isChecked());
            rec->doc = now;
            if (rec->chain.isEmpty() || !rec->chain.last().startsWith("typed: ")) rec->chain << QString();
            rec->chain.last() = "typed: " + regionText();
        }
        showMarks();
    });
    QObject::connect(mathEdit, &QPlainTextEdit::selectionChanged, showMarks);
    // What the text now says symbol i is: its linked characters ("int" is ∫, "*" a × or ·); empty: not one symbol
    auto finalLabel = [=](int i) {
        QString t;
        for (int p = 0; p < rec->links.size(); ++p)
            if (rec->links[p] == i) t += rec->doc[p];
        return symbolText(t, rec->syms[i].cands.value(0));
    };
    // Typed relabels (one character for one) become the symbols' readings, so a new layout keeps them.
    // ponytail: other typed changes inside the recognized text (added "^", retyped parts) are lost when a correction
    // rewrites it; patch the text instead of rewriting it if that bites
    auto absorb = [=] {
        for (int i = 0; i < rec->syms.size(); ++i)
            if (QString f = finalLabel(i); !f.isEmpty() && f != rec->syms[i].cands.value(0)) {
                rec->syms[i].cands.removeAll(f);
                rec->syms[i].cands.prepend(f);
            }
    };
    auto putText = [=](const QString &why) {   // the layout of the symbols in place of the recognized text (or the □)
        InkText lt = inkLayout(rec->syms, rec->knobs);
        QString mine = lt.text;
        QList<int> src = lt.src;
        if (rec->box && mine.isEmpty()) mine = kBox, src = {-1};   // nothing left: the box again
        else if (rec->box) mine.replace('\n', ' ');
        int start = rec->region.selectionStart(), len = rec->region.selectionEnd() - start;
        rec->writing = true;
        rec->region.insertText(mine);
        rec->writing = false;
        rec->region.setPosition(start);
        rec->region.setPosition(start + int(mine.size()), QTextCursor::KeepAnchor);
        rec->links = rec->links.mid(0, start) + src + rec->links.mid(start + len);
        rec->doc = mathEdit->toPlainText();
        if (!why.isEmpty()) rec->chain << why + ": " + mine;
        QTextCursor c = mathEdit->textCursor();   // after it, nothing selected
        c.setPosition(rec->region.selectionEnd());
        mathEdit->setTextCursor(c);
        showMarks();
    };
    auto forget = [=] {   // the dock no longer corrects recognized ink
        rec->syms.clear();
        rec->picked.clear();
        rec->links.clear();
        rec->chain.clear();
        pad->ink.clear();   // and the pad's session ends
        pad->update();
        rec->padStarted = false;
        showMarks();
    };
    auto learn = [=](InkSymbol &s, const QString &sym) {   // s is sym: its first candidate, and a personal template
        s.cands.removeAll(sym);
        s.cands.prepend(sym);
        rec->was[s.strokes] = sym;
        rec->personal << makeTemplate(sym, inkOf(rec->ink, s.strokes));
        rec->taught << rec->personal.last();
        if (!saveTemplates(rec->personal, personalFile)) QMessageBox::warning(w, "Techy-Notes", "Could not save " + personalFile);
    };
    tapped = [=](int pos) {   // a □: fill it; recognized ink: (un)select its symbol; typed text: edit it there
        QString t = mathEdit->toPlainText();
        int s = rec->links.value(pos, -1);
        if (pos < t.size() && t[pos] == kBox && padBox->isVisible()) {   // the pad writes into this box now
            pad->ink.clear();
            pad->update();
            rec->padAt = pos;
            rec->padStarted = false;
        }
        if (s < 0 || (pos < t.size() && t[pos] == kBox)) return selectAt(pos);
        if (!rec->picked.removeAll(s)) rec->picked << s;
        showMarks();
    };
    doubleTapped = [=](int pos) {   // rewrite one symbol
        int s = rec->links.value(pos, -1);
        if (s < 0) return;
        QString typed = QInputDialog::getText(w, "Rewrite", "What did you write here? (e.g. x, 2, α, ∫)", QLineEdit::Normal,
                                              rec->syms[s].cands.value(0)).remove(' ');
        if (typed.isEmpty() || typed == rec->syms[s].cands.value(0)) return;
        absorb();
        learn(rec->syms[s], typed);
        rec->picked.clear();
        putText("rewrite");
    };
    // One selected: its alternatives. Several: type what they are, one character each ("sin" for three), or a
    // single symbol to merge them (one symbol read as two).
    QObject::connect(fix, &QPushButton::clicked, [=] {
        QStringList typed = askSymbols(w, rec->syms, rec->picked);
        if (typed.isEmpty()) return;
        absorb();
        for (int i : setSymbols(rec->syms, rec->picked, typed)) learn(rec->syms[i], rec->syms[i].cands.value(0));
        rec->picked.clear();
        putText("correct");
    });
    QObject::connect(split, &QPushButton::clicked, [=] {   // wrongly joined: one symbol per stroke, classified anew
        absorb();
        for (int i : splitSymbols(rec->syms, rec->picked, rec->ink, rec->personal)) rec->was[rec->syms[i].strokes] = rec->syms[i].cands.value(0);
        rec->picked.clear();
        putText("split");
    });
    QObject::connect(drop, &QPushButton::clicked, [=] {   // stray ink: left out of the text, nothing learned
        QList<int> picked = rec->picked;
        std::sort(picked.begin(), picked.end(), std::greater<int>());
        absorb();
        for (int i : picked) rec->syms.removeAt(i);
        rec->picked.clear();
        putText("delete");
    });
    // into the □ at at, else (-1) new lines under the text; keys: the page strokes the ink is (none: the pad's)
    auto recognize = [=](const QList<QPolygonF> &ink, const QList<Key> &keys, int at) {
        rec->keys = keys;
        rec->box = at >= 0;
        rec->writing = true;
        if (!rec->box) {
            QString all = mathEdit->toPlainText();
            while (all.endsWith('\n')) all.chop(1);
            mathEdit->setPlainText(all.isEmpty() ? all : all + "\n");
            at = int(mathEdit->toPlainText().size());
        }
        rec->writing = false;
        rec->ink = ink;
        rec->syms = recognizeSymbols(ink, rec->personal);
        rec->was.clear();
        for (const InkSymbol &s : rec->syms) rec->was[s.strokes] = s.cands.value(0);
        rec->picked.clear();
        rec->chain.clear();
        rec->doc = mathEdit->toPlainText();
        rec->links = QList<int>(rec->doc.size(), -1);
        rec->region = QTextCursor(mathEdit->document());
        rec->region.setPosition(at);
        rec->region.setPosition(at + (rec->box ? 1 : 0), QTextCursor::KeepAnchor);
        putText({});
        rec->first = regionText();
        mathDock->show();
    };
    auto closeEq = new QPushButton("Close");
    closeEq->setToolTip("Close this handwritten equation (it stays underlined; tap it to open it again). The typed text comes back.");
    closeEq->hide();
    mathRow->insertWidget(mathRow->indexOf(write), closeEq);
    // The equation as it was left: its symbols found again in the ink now (moved is fine; erased strokes drop their
    // symbols; strokes lassoed in since are read and added). False: no state (or unreadable).
    auto restoreEq = [=](const QByteArray &b) {
        QDataStream d(b);
        d.setVersion(QDataStream::Qt_6_0);
        qint32 v = 0, start = 0, end = 0, k = 0, n = 0;
        QString text;
        bool box = false;
        QRectF view;
        QList<Key> all, keys;
        d >> v;
        if (v != 1) return false;
        d >> text >> start >> end >> box >> k >> view >> all >> keys >> n;
        QList<InkSymbol> syms;
        QStringList was;
        for (int i = 0; i < n && d.status() == QDataStream::Ok; ++i) {
            InkSymbol s;
            qint8 role = 0;
            QString wv;
            d >> s.strokes >> s.box >> s.cands >> s.probs >> role >> s.owner >> wv;
            s.role = char(role);
            syms << s, was << wv;
        }
        QList<int> links;
        d >> links >> rec->first >> rec->chain >> rec->taught;
        if (d.status() != QDataStream::Ok) return false;
        QList<int> strokes = canvas->equationInk(rec->link);
        QList<Key> now;
        QList<QPolygonF> cur = pageInk(strokes, now);
        QList<int> to(keys.size(), -1);   // the state's ink index -> rec->ink index
        rec->ink.clear(), rec->keys.clear();
        for (int j = 0; j < keys.size(); ++j)
            if (qsizetype c = now.indexOf(keys[j]); c >= 0) to[j] = int(rec->ink.size()), rec->ink << cur[c], rec->keys << keys[j];
        auto remap = [&](QList<int> &l) {
            for (int &x : l)
                if ((x = to.value(x, -1)) < 0) return false;
            return true;
        };
        bool changed = false;
        rec->syms.clear();
        rec->was.clear();
        for (int i = 0; i < syms.size(); ++i) {
            InkSymbol s = syms[i];
            if (!remap(s.strokes)) {
                changed = true;
                continue;
            }
            if (!remap(s.owner)) s.owner.clear(), s.role = 0;
            s.box = {};
            for (const QPolygonF &p : inkOf(rec->ink, s.strokes)) s.box |= p.boundingRect();
            rec->was[s.strokes] = was[i];
            rec->syms << s;
        }
        QList<QPolygonF> fresh;   // lassoed in since
        QList<Key> freshKeys;
        for (int c = 0; c < now.size(); ++c)
            if (!all.contains(now[c]) && !keys.contains(now[c])) fresh << cur[c], freshKeys << now[c];
        if (!fresh.isEmpty()) {
            int off = int(rec->ink.size());
            for (InkSymbol s : recognizeSymbols(fresh, rec->personal)) {
                for (int &x : s.strokes) x += off;
                rec->was[s.strokes] = s.cands.value(0);
                rec->syms << s;
            }
            rec->ink += fresh, rec->keys += freshKeys;
            changed = true;
        }
        rec->box = box;
        rec->writing = true;
        mathEdit->setPlainText(text);
        rec->writing = false;
        kind->setCurrentIndex(k);
        plotView->view = view;
        rec->doc = text;
        rec->links = links.size() == text.size() ? links : QList<int>(text.size(), -1);
        rec->region = QTextCursor(mathEdit->document());
        rec->region.setPosition(std::clamp<int>(start, 0, int(text.size())));
        rec->region.setPosition(std::clamp<int>(end, 0, int(text.size())), QTextCursor::KeepAnchor);
        if (changed) putText("ink changed");
        return true;
    };
    // That equation in the dock, alone; 0: none (the typed scratch text back). again: read its ink again even if it
    // is the one shown (lassoed again with more strokes).
    auto openEq = [=](quint32 link, bool again) {
        if (link == rec->link && !again) {
            if (link) mathDock->show();
            return;
        }
        if (rec->link && rec->link != link) storeEq();   // (the one shown is stored on every change: its strokes known
        else if (!rec->link && link) rec->scratch = mathEdit->toPlainText();   // then, so lassoed-in ones read as new)
        rec->loading = true;
        forget();
        rec->link = link;
        rec->ink.clear(), rec->keys.clear(), rec->was.clear(), rec->taught.clear(), rec->first.clear();
        rec->box = rec->fromPad = false;
        rec->region = QTextCursor();
        canvas->useInk(link);
        int obj = canvas->mathOf(link);
        if (link) canvas->editMath(obj);   // (none: an object opened with Edit math stays the one Update changes)
        upd->setEnabled(obj >= 0);
        closeEq->setVisible(link);
        bool ok = link && restoreEq(canvas->sheet.equations.value(link));
        if (!link || (!ok && obj >= 0)) {   // the scratch text, or kept ink from before equations were saved: its object's text
            QStringList l = link ? canvas->sheet.strokes[obj].math.split('\n') : rec->scratch.split('\n');
            if (link) {
                QStringList h = l.takeFirst().split(' ', Qt::SkipEmptyParts);
                kind->setCurrentIndex(h.value(0) == "plot" ? 0 : 1);
                if (h.size() == 5) plotView->view = QRectF(QPointF(h[1].toDouble(), h[3].toDouble()), QPointF(h[2].toDouble(), h[4].toDouble()));
            }
            rec->writing = true;
            mathEdit->setPlainText(l.join('\n'));
            rec->writing = false;
        } else if (!ok) {   // read for the first time
            rec->writing = true;
            mathEdit->clear();
            rec->writing = false;
            QList<Key> keys;
            QList<QPolygonF> ink = pageInk(canvas->equationInk(link), keys);
            recognize(ink, keys, -1);
        }
        rec->loading = false;
        showMarks();   // and stored
        if (link) mathDock->show();
    };
    this->openEq = openEq;
    // The lasso on the page: a □ selected (new ink): it fills the □, joining the equation shown; else its equation
    // opens, read for the first time or with the new strokes added
    recognizeInk = [=](quint32 link, const QList<int> &strokes) {
        int box = selectedBox();
        if (box >= 0 && !canvas->sheet.equations.contains(link)) {
            if (rec->link && rec->link != link) canvas->relink(link, rec->link);
            rec->loading = true;
            if (!rec->link) rec->link = link, rec->scratch.clear(), closeEq->show();
            canvas->useInk(rec->link);
            QList<Key> keys;
            QList<QPolygonF> ink = pageInk(strokes, keys);
            recognize(ink, keys, box);
            rec->loading = false;
            showMarks();
        } else {
            openEq(link, true);
        }
        rec->fromPad = false;
        pad->ink.clear();   // the page's ink takes over from the pad
        pad->update();
        rec->padStarted = false;
        rec->padAt = -1;
    };
    QObject::connect(closeEq, &QPushButton::clicked, [=] { openEq(0, false); });
    // The pad: each time the pen rests, all its ink is read again in place of what it wrote before
    auto padSettle = new QTimer(pad);
    padSettle->setSingleShot(true);
    padSettle->setInterval(400);
    pad->lifted = [=] { padSettle->start(); };
    auto startPad = [=] {   // a new pad session: into the selected □, else new lines
        pad->ink.clear();
        pad->update();
        rec->padAt = selectedBox();
        rec->padStarted = false;
        padBox->show();
    };
    QObject::connect(padSettle, &QTimer::timeout, [=] {
        if (pad->ink.isEmpty()) return;
        if (!rec->padStarted || !rec->fromPad) {
            recognize(pad->ink, {}, rec->padAt);
            rec->fromPad = rec->padStarted = true;
            return;
        }
        rec->ink = pad->ink;
        rec->keys.clear();
        rec->syms = recognizeSymbols(pad->ink, rec->personal);
        rec->was.clear();
        for (const InkSymbol &s : rec->syms) rec->was[s.strokes] = s.cands.value(0);
        rec->picked.clear();
        putText({});
        rec->first = regionText();
    });
    QObject::connect(write, &QPushButton::clicked, startPad);
    QObject::connect(padDone, &QPushButton::clicked, padBox, &QWidget::hide);
    QObject::connect(padClear, &QPushButton::clicked, [=] {   // what the pad wrote goes (a box comes back), the pad starts over
        if (rec->padStarted && rec->fromPad) {
            rec->syms.clear();
            putText({});
            rec->padAt = rec->box ? rec->region.selectionStart() : -1;
            forget();
        }
        pad->ink.clear();
        pad->update();
        rec->padStarted = false;
    });
    // Symbols selected in the preview + a structure button: read them as that structure. ∫ ∑ ∏: the selected symbol
    // that can be the sign (else the tallest) becomes it, the others its limits by height; lim: the tallest and the
    // two after it on its line are l, i, m (fewer: one symbol "lim"), the rest its limit; xⁿ / xₙ: the leftmost is the base
    reread = [=](const QString &t) {
        QList<int> p = rec->picked;
        char kind = t.startsWith("int_") ? 'I' : t.startsWith("sum") ? 'S' : t.startsWith("prod") ? 'P' : t.startsWith("lim")      ? 'L'
                  : t.startsWith("□^") ? '^' : t.startsWith("□_(□)") && !t.contains('^') ? '_' : 0;
        if (p.size() < 2 || !kind) return false;
        absorb();
        QList<InkSymbol> &sy = rec->syms;
        std::sort(p.begin(), p.end(), [&](int a, int b) { return sy[a].box.left() < sy[b].box.left(); });
        int head = p[0];
        QList<int> part{head};   // the operator's own symbols
        QString sign = kind == 'I' ? "∫" : kind == 'S' ? "∑" : "∏";
        auto tallest = [&](auto score) { return *std::max_element(p.begin(), p.end(), [&](int a, int b) { return score(a) < score(b); }); };
        if (kind == 'I' || kind == 'S' || kind == 'P') {
            head = tallest([&](int i) { return (sy[i].cands.mid(0, 4).contains(sign) ? 1e9 : 0) + sy[i].box.height(); });
            part = {head};
            learn(sy[head], sign);
        } else if (kind == 'L') {
            head = tallest([&](int i) { return sy[i].box.height(); });
            part.clear();
            for (int i : p)   // left to right on the l's line, from the l
                if (sy[i].box.left() >= sy[head].box.left() - 0.01 && sy[i].box.center().y() > sy[head].box.top() &&
                    sy[i].box.center().y() < sy[head].box.bottom() && part.size() < 3)
                    part << i;
            if (part.size() == 3) {
                for (int k = 0; k < 3; ++k)
                    if (sy[part[k]].cands.value(0) != QString("lim")[k]) learn(sy[part[k]], QString("lim")[k]);
            } else {   // written in one go (or read as two): one symbol "lim"
                QList<int> merged;
                QRectF box;
                for (int i : part) merged << sy[i].strokes, box |= sy[i].box;
                std::sort(merged.begin(), merged.end());
                QList<QList<int>> kept;   // the others by their strokes, to find them again after the merge
                for (int i : p)
                    if (!part.contains(i)) kept << sy[i].strokes;
                sy[part[0]].strokes = merged, sy[part[0]].box = box;
                learn(sy[part[0]], "lim");
                QList<int> gone = part.mid(1);
                std::sort(gone.begin(), gone.end(), std::greater<int>());
                for (int i : gone) sy.removeAt(i);
                auto find = [&](const QList<int> &s) { return int(std::find_if(sy.begin(), sy.end(), [&](const InkSymbol &x) { return x.strokes == s; }) - sy.begin()); };
                head = find(merged);
                part = p = {head};
                for (const QList<int> &s : kept) p << find(s);
            }
        }
        for (int i : p)
            if (!part.contains(i)) {
                sy[i].owner = sy[head].strokes;
                sy[i].role = kind == '^' || kind == '_' ? kind : kind == 'L' || sy[i].box.center().y() > sy[head].box.center().y() ? 'l' : 'u';
            }
        rec->picked.clear();
        putText("re-read");
        return true;
    };
    auto inkIcon = [](const QList<QPolygonF> &ink) {   // a symbol's ink, fitted into a 48 px square
        QPixmap pm(48, 48);
        pm.fill(Qt::white);
        QRectF b;
        for (const QPolygonF &s : ink) b |= s.boundingRect();
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        double k = 40 / std::max({b.width(), b.height(), 1e-9});
        p.translate(24 - b.center().x() * k, 24 - b.center().y() * k);
        p.scale(k, k);
        p.setPen(QPen(Qt::black, 2.5 / k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const QPolygonF &s : ink) s.size() == 1 ? p.drawPoint(s[0]) : p.drawPolyline(s);
        return QIcon(pm);
    };
    // After Insert: what the corrections of this equation taught (learned already: untick to forget it), and the
    // symbols whose text you changed by typing (tick to learn them); then the log entry, the layout refitted on the log
    auto learnFromInsert = [=] {
        QList<int> todo;
        for (int i = 0; i < rec->syms.size(); ++i)
            if (QString f = finalLabel(i); !f.isEmpty() && f != rec->was.value(rec->syms[i].strokes)) todo << i;
        QList<Template> taught = rec->taught;
        if (todo.isEmpty() && taught.isEmpty()) return;
        QDialog d(w);
        d.setWindowTitle("Learn your handwriting");
        auto lay = new QVBoxLayout(&d);
        lay->addWidget(new QLabel("Learned from your corrections (untick to forget one), and changed by typing (ticked: learn it):"));
        auto list = new QListWidget;
        list->setIconSize({48, 48});
        for (const Template &t : taught)
            (new QListWidgetItem(inkIcon(t.strokes), QString("%1   (learned from a correction)").arg(t.sym), list))->setCheckState(Qt::Checked);
        for (int i : todo) {
            auto it = new QListWidgetItem(inkIcon(inkOf(rec->ink, rec->syms[i].strokes)),
                                          QString("%1   (typed; was read as %2)").arg(finalLabel(i), rec->was.value(rec->syms[i].strokes)), list);
            it->setCheckState(Qt::Checked);
        }
        lay->addWidget(list);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("OK");
        QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        lay->addWidget(buttons);
        if (d.exec() != QDialog::Accepted) return;
        bool forgot = false;
        for (int k = 0; k < taught.size(); ++k)
            if (list->item(k)->checkState() != Qt::Checked)
                forgot |= rec->personal.removeIf([&](const Template &t) { return t.sym == taught[k].sym && t.strokes == taught[k].strokes; }) > 0;
        if (forgot && !saveTemplates(rec->personal, personalFile)) QMessageBox::warning(w, "Techy-Notes", "Could not save " + personalFile);
        for (int k = 0; k < todo.size(); ++k)
            if (list->item(int(taught.size()) + k)->checkState() == Qt::Checked) learn(rec->syms[todo[k]], finalLabel(todo[k]));
    };
    auto logInsert = [=] {
        MathLogEntry e{rec->ink, rec->syms, rec->first, rec->chain, regionText()};
        for (int i = 0; i < e.symbols.size(); ++i) {
            QString f = finalLabel(i);
            e.symbols[i].cands = {f.isEmpty() ? e.symbols[i].cands.value(0) : f};
            e.symbols[i].probs.clear();
        }
        rec->log << e;
        keepLog();
    };
    askInk = [=] {   // an analysed equation: replace its ink or keep it
        if (!rec->link || !canvas->inkValid()) return 0;
        QMessageBox box(QMessageBox::Question, "Insert", "Replace the handwritten ink with the inserted math?", QMessageBox::NoButton, w);
        QAbstractButton *replace = box.addButton("Replace ink", QMessageBox::AcceptRole);
        QAbstractButton *keep = box.addButton("Keep ink", QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        return box.clickedButton() == replace ? 2 : box.clickedButton() == keep ? 1 : -1;
    };
    inserted = [=](int ink) {
        if (!rec->syms.isEmpty() || !rec->taught.isEmpty()) {
            if (learnOn->isChecked()) learnFromInsert();
            if (!rec->syms.isEmpty()) logInsert();
        }
        rec->taught.clear();
        if (ink == 2) return openEq(0, false);   // its ink is gone
        if (!rec->link) return forget();
        canvas->useInk(rec->link);   // kept ink: still there, underlined blue now; Update changes the new object
        storeEq();
    };
    editing = [=] {   // Edit math: the object's text in place of the scratch text
        openEq(0, false);
        forget();
    };
}

#include "Recognizer.moc"