#include "Canvas.h"
#include "Ink.h"
#include "Math.h"
#include <QActionGroup>
#include <QInputDialog>
#include <memory>
#include <QPlainTextEdit>
#include <QWheelEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QListWidget>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QTabletEvent>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <functional>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QMessageBox>
#include <QSlider>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QToolBar>
#include <QTreeView>
#include <algorithm>

// Notebook tree: folders = notebooks/chapters, .tsheet files = sheets; each item holds its path.
static const QString kExt = ".tsheet";
static constexpr int PathRole = Qt::UserRole;

static bool isSheet(const QStandardItem *it) { return it->data(PathRole).toString().endsWith(kExt); }

static QStandardItem *makeItem(const QString &path) {
    QFileInfo fi(path);
    bool sheet = path.endsWith(kExt);
    auto it = new QStandardItem(QApplication::style()->standardIcon(sheet ? QStyle::SP_FileIcon : QStyle::SP_DirIcon),
                                sheet ? fi.completeBaseName() : fi.fileName());
    it->setData(path, PathRole);
    if (sheet) it->setFlags(it->flags() & ~Qt::ItemIsDropEnabled);
    return it;
}

// Folder contents: names listed in order.txt first, the rest after (folders, then sheets, by name).
static void fill(QStandardItem *folder, const QString &dir) {
    QDir d(dir);
    QStringList names = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name) + d.entryList({"*" + kExt}, QDir::Files, QDir::Name);
    names.removeIf([](const QString &n) { return n.startsWith('.'); });   // session trash
    QStringList order;
    QFile f(dir + "/order.txt");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) order = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    auto rank = [&](const QString &n) { qsizetype i = order.indexOf(n); return i < 0 ? order.size() : i; };
    std::stable_sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return rank(a) < rank(b); });
    for (const QString &n : names) {
        QStandardItem *it = makeItem(dir + "/" + n);
        if (!isSheet(it)) fill(it, dir + "/" + n);
        folder->appendRow(it);
    }
}

static void repath(QStandardItem *it, const QString &from, const QString &to) {
    it->setData(to + it->data(PathRole).toString().mid(from.size()), PathRole);
    for (int r = 0; r < it->rowCount(); ++r) repath(it->child(r), from, to);
}

// Makes the disk match the tree after a rename or drag: renames/moves files and folders, rewrites order.txt.
// False if something failed (name taken or invalid); the caller then rebuilds the tree from disk.
static bool syncFolder(QStandardItem *folder, const QString &dir, Canvas *canvas) {
    QStringList order;
    for (int r = 0; r < folder->rowCount(); ++r) {
        QStandardItem *it = folder->child(r);
        bool sheet = isSheet(it);
        QString name = it->text().trimmed(), from = it->data(PathRole).toString(), to = dir + "/" + name + (sheet ? kExt : "");
        if (name.isEmpty() || (sheet && it->hasChildren())) return false;
        if (from != to) {
            bool taken = QFileInfo::exists(to) && from.compare(to, Qt::CaseInsensitive) != 0;   // case-only rename is fine
            if (taken || !QDir().rename(from, to)) return false;
            repath(it, from, to);
            if (canvas->path == from || canvas->path.startsWith(from + "/")) canvas->path = to + canvas->path.mid(from.size());
        }
        order << QFileInfo(to).fileName();
        if (!sheet && !syncFolder(it, to, canvas)) return false;
    }
    QFile f(dir + "/order.txt");
    return f.open(QIODevice::WriteOnly | QIODevice::Text) && f.write(order.join('\n').toUtf8()) >= 0;
}

// Windows sends no right-click for the pen (Qt takes the raw pen input), so the tree opens its menu itself:
// side button + tap, or holding the pen still for 0.5 s.
struct Tree : QTreeView {
    Canvas *canvas;
    QTimer hold;
    QPointF pressAt;
    bool menuHeld = false;   // right-click button still held since its menu opened
    explicit Tree(Canvas *c) : canvas(c) {
        hold.setSingleShot(true);
        hold.setInterval(500);
        connect(&hold, &QTimer::timeout, this, [this] { customContextMenuRequested(pressAt.toPoint()); });
    }
    bool viewportEvent(QEvent *e) override {
        auto *t = static_cast<QTabletEvent *>(e);
        bool tablet = e->type() == QEvent::TabletPress || e->type() == QEvent::TabletMove || e->type() == QEvent::TabletRelease;
        if (tablet && (t->buttons() & canvas->menuButtons)) {   // one menu per button hold
            if (!menuHeld && e->type() == QEvent::TabletPress) customContextMenuRequested(t->position().toPoint());
            menuHeld = true;
            t->accept();   // no mouse events made from it
            return true;
        }
        if (tablet) menuHeld = false;
        if (e->type() == QEvent::TabletPress) {
            pressAt = t->position();
            hold.start();
        } else if (e->type() == QEvent::TabletRelease ||
                   (e->type() == QEvent::TabletMove && QLineF(t->position(), pressAt).length() > 8)) {
            hold.stop();
        }
        return QTreeView::viewportEvent(e);   // not accepted: Qt turns it into mouse events (select, drag)
    }
};

// Calibration test area: hands every pen event to onPen.
struct PenPad : QFrame {
    std::function<void(QTabletEvent *)> onPen;
    PenPad() {
        setFrameShape(QFrame::Box);
        setMinimumSize(420, 220);
    }
    void tabletEvent(QTabletEvent *e) override {
        e->accept();
        onPen(e);
    }
};

static void calibratePen(QWidget *parent, Canvas *canvas) {
    QDialog dlg(parent);
    dlg.setWindowTitle("Calibrate pen");
    auto task = new QLabel;
    task->setWordWrap(true);
    auto live = new QLabel(" ");
    auto pad = new PenPad;
    auto box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto next = box->addButton("Next", QDialogButtonBox::ActionRole);
    auto skip = box->addButton("Pen has no such button", QDialogButtonBox::ActionRole);
    auto restart = box->addButton("Restart", QDialogButtonBox::ResetRole);
    auto lay = new QVBoxLayout(&dlg);
    for (QWidget *wd : {(QWidget *)task, (QWidget *)pad, (QWidget *)live, (QWidget *)box}) lay->addWidget(wd);

    const QString tasks[] = {"Step 1/4: hold the side button you use as eraser and tap the box.",
                             "Step 2/4: hold the side button you use for right-click and tap the box.",
                             "Step 3/4: draw a line in the box, as lightly as you write.",
                             "Step 4/4: draw a line in the box, pressing firmly.",
                             "Done. Save to use these settings."};
    int step = 0, eraser = 0, menu = 0, n = 0;
    float sum = 0, light = 0, firm = 1;
    auto say = [&](const QString &note) { task->setText(tasks[step] + "\n\n" + note); };
    auto ok = [&](const QString &note) { say(note + "  Tap again to redo, or Next."); next->setEnabled(true); };
    auto go = [&](int s) {
        step = s;
        say({});
        next->setEnabled(false);
        next->setVisible(s < 4);
        skip->setVisible(s < 2);
        box->button(QDialogButtonBox::Save)->setEnabled(s == 4);
    };
    go(0);
    QObject::connect(next, &QPushButton::clicked, [&] { go(step + 1); });
    QObject::connect(skip, &QPushButton::clicked, [&] { (step == 0 ? eraser : menu) = 0; go(step + 1); });
    QObject::connect(restart, &QPushButton::clicked, [&] { go(0); });
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    pad->onPen = [&](QTabletEvent *e) {
        bool eraserMode = e->pointerType() == QPointingDevice::PointerType::Eraser;
        int b = e->buttons() & ~Qt::LeftButton;
        live->setText(QString("pressure %1    buttons 0x%2    %3")
                          .arg(e->pressure(), 0, 'f', 2).arg(int(e->buttons()), 0, 16).arg(eraserMode ? "eraser mode" : "pen"));
        if (step < 2 && e->type() == QEvent::TabletPress) {
            if (step == 0 && (b || eraserMode)) {
                eraser = b;   // 0: the pen reports eraser mode, which always erases
                ok(b ? QString("Registered button 0x%1.").arg(b, 0, 16) : "Registered: the pen switches to eraser mode.");
            } else if (step == 1 && b && b != eraser) {
                menu = b;
                ok(QString("Registered button 0x%1.").arg(b, 0, 16));
            } else {
                say(step == 1 && b ? "That is the eraser button." : "No side button seen, keep it held while the tip touches.");
            }
        } else if (step == 2 || step == 3) {
            if (e->type() == QEvent::TabletPress) sum = n = 0;
            if (e->type() == QEvent::TabletMove && e->pressure() > 0) { sum += e->pressure(); ++n; }
            if (e->type() != QEvent::TabletRelease || n < 5) return;
            float avg = sum / n;
            if (step == 3 && avg < light + 0.1f) return say("Too close to the light line, press harder.");
            (step == 2 ? light : firm) = avg;
            ok(QString("Registered pressure %1.").arg(avg, 0, 'f', 2));
        }
    };
    if (dlg.exec() != QDialog::Accepted) return;
    canvas->eraserButtons = eraser;
    canvas->menuButtons = menu;
    canvas->pressLow = light;
    canvas->pressHigh = firm;
    QSettings st;
    st.setValue("pen/eraser", eraser);
    st.setValue("pen/menu", menu);
    st.setValue("pen/low", light);
    st.setValue("pen/high", firm);
}

static void pageSetup(QWidget *parent, Canvas *canvas) {
    if (!canvas->sheet.strokes.isEmpty())
        return (void)QMessageBox::information(parent, "Techy-Notes", "The page size can only change while the sheet is empty.");
    QDialog dlg(parent);
    dlg.setWindowTitle("Page setup");
    const char *names[] = {"A4", "A5", "Letter", "Custom"};
    const QSizeF sizes[] = {{210, 297}, {148, 210}, {215.9, 279.4}};
    QSizeF cur = canvas->sheet.page;
    auto preset = new QComboBox;
    for (const char *n : names) preset->addItem(n);
    preset->setCurrentIndex(3);
    for (int i = 0; i < 3; ++i)
        if (sizes[i] == cur || sizes[i].transposed() == cur) preset->setCurrentIndex(i);
    auto landscape = new QCheckBox("Landscape");
    landscape->setChecked(cur.width() > cur.height());
    QDoubleSpinBox *wd = new QDoubleSpinBox, *ht = new QDoubleSpinBox;
    for (auto [sb, v] : {std::pair(wd, cur.width()), std::pair(ht, cur.height())}) {
        sb->setRange(50, 2000);
        sb->setDecimals(1);
        sb->setSuffix(" mm");
        sb->setValue(v);
    }
    QObject::connect(preset, &QComboBox::currentIndexChanged, [&](int i) {
        if (i == 3) return;
        QSizeF s = landscape->isChecked() ? sizes[i].transposed() : sizes[i];
        wd->setValue(s.width());
        ht->setValue(s.height());
    });
    QObject::connect(landscape, &QCheckBox::toggled, [&] {
        double w = wd->value();
        wd->setValue(ht->value());
        ht->setValue(w);
    });
    auto box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto form = new QFormLayout(&dlg);
    form->addRow("Size", preset);
    form->addRow("", landscape);
    form->addRow("Width", wd);
    form->addRow("Height", ht);
    form->addRow(box);
    if (dlg.exec() == QDialog::Accepted) canvas->setPageSize({wd->value(), ht->value()});
}

// Every page as a thumbnail: tap toggles it, drag reorders, the number is its place in the PDF.
// Pages with ink start on, in export-queue order; the new order of those is saved with the sheet.
static void exportDialog(QWidget *parent, Canvas *canvas) {
    const Sheet &s = canvas->sheet;
    QList<QPoint> pages = s.exportQueue, rest = s.pages.values();
    std::sort(rest.begin(), rest.end(), rowMajor);
    for (QPoint c : rest)
        if (!pages.contains(c)) pages << c;
    enum { CellRole = Qt::UserRole, OnRole, ThumbRole, DimRole };
    QDialog dlg(parent);
    dlg.setWindowTitle("Export PDF");
    dlg.resize(900, 650);
    auto list = new QListWidget;
    list->setViewMode(QListView::IconMode);
    list->setMovement(QListView::Static);   // reorder through drag and drop, not free placement
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setResizeMode(QListView::Adjust);
    list->setSpacing(10);
    QFont big = list->font();   // the export number under each page
    big.setPointSize(14);
    big.setBold(true);
    list->setFont(big);
    const double k = 220 / s.page.height();   // thumbnail px per mm
    list->setIconSize((s.page * k).toSize());
    for (QPoint c : pages) {
        QPixmap px((s.page * k).toSize());
        px.fill(Qt::white);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(k, k);
        drawPage(p, s, c);
        p.resetTransform();
        p.setPen(QColor(0x80, 0x80, 0x80));
        p.drawRect(px.rect().adjusted(0, 0, -1, -1));   // frame: white page on a white list
        p.end();
        QPixmap dim = px;
        QPainter(&dim).fillRect(dim.rect(), QColor(90, 90, 90, 170));
        auto it = new QListWidgetItem(list);
        it->setData(CellRole, c);
        it->setData(OnRole, s.exportQueue.contains(c));
        it->setData(ThumbRole, px);
        it->setData(DimRole, dim);
        it->setToolTip(QString("Page %1,%2 (column, row)").arg(c.x()).arg(c.y()));
    }
    auto renumber = [&] {
        int n = 0;
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem *it = list->item(i);
            bool on = it->data(OnRole).toBool();
            it->setText(on ? QString::number(++n) : "skip");
            it->setIcon(it->data(on ? ThumbRole : DimRole).value<QPixmap>());
        }
    };
    renumber();
    QObject::connect(list, &QListWidget::itemClicked, [&](QListWidgetItem *it) {
        it->setData(OnRole, !it->data(OnRole).toBool());
        renumber();
    });
    auto later = [&] { QTimer::singleShot(0, &dlg, renumber); };   // after the drop has finished
    QObject::connect(list->model(), &QAbstractItemModel::rowsMoved, &dlg, later);
    QObject::connect(list->model(), &QAbstractItemModel::rowsInserted, &dlg, later);
    auto box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    box->addButton("Export...", QDialogButtonBox::AcceptRole);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel("Tap a page to include or skip it, drag to change the order."));
    lay->addWidget(list);
    lay->addWidget(box);
    if (dlg.exec() != QDialog::Accepted) return;
    QList<QPoint> chosen, queue;
    for (int i = 0; i < list->count(); ++i) {
        QPoint c = list->item(i)->data(CellRole).toPoint();
        if (list->item(i)->data(OnRole).toBool()) chosen << c;
        if (s.exportQueue.contains(c)) queue << c;
    }
    canvas->setExportQueue(queue);
    if (chosen.isEmpty()) return (void)QMessageBox::information(parent, "Techy-Notes", "No page chosen.");
    QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString file = QFileDialog::getSaveFileName(parent, "Export PDF", docs + "/" + QFileInfo(canvas->path).completeBaseName() + ".pdf",
                                                "PDF (*.pdf)");
    if (!file.isEmpty() && !exportPdf(s, chosen, file)) QMessageBox::warning(parent, "Techy-Notes", "Could not write " + file);
}

// Math dock preview: the lines typeset, then the plot (4:3 like an inserted plot). Drag pans; wheel or ± zooms.
// ponytail: no pinch zoom here (finger drag pans via Qt's synthesized mouse); add a QPinchGesture if ± is not enough
struct MathView : QWidget {
    QStringList lines;
    QRectF view{-10, -7.5, 20, 15};   // math units: x from left, y from top (the bottom of the plot) up
    QPointF last;

    QRectF eqRect() const {
        qsizetype n = std::count_if(lines.begin(), lines.end(), [](const QString &l) { return !l.trimmed().isEmpty(); });
        return {8, 8, width() - 16.0, std::min(height() * 0.35, n * 36.0)};
    }
    QRectF plotRect() const {
        QRectF a(8, eqRect().bottom() + 8, width() - 16.0, height() - eqRect().bottom() - 16);
        QSizeF s(4, 3);
        s.scale(a.size(), Qt::KeepAspectRatio);
        return {a.topLeft(), s};
    }
    void zoom(double f, QPointF at) {   // about the math point under at (widget px)
        QRectF r = plotRect();
        double mx = view.left() + (at.x() - r.left()) / r.width() * view.width();
        double my = view.top() + (r.bottom() - at.y()) / r.height() * view.height();
        view = QRectF(mx - (mx - view.left()) * f, my - (my - view.top()) * f, view.width() * f, view.height() * f);
        update();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        p.setRenderHint(QPainter::Antialiasing);
        drawMath(p, "eq\n" + lines.join('\n'), eqRect());
        drawPlot(p, lines, plotRect(), view, 1);
        p.setPen(Qt::lightGray);
        p.drawRect(plotRect());
    }
    void mousePressEvent(QMouseEvent *e) override { last = e->position(); }
    void mouseMoveEvent(QMouseEvent *e) override {
        QRectF r = plotRect();
        QPointF d = e->position() - last;
        view.translate(-d.x() / r.width() * view.width(), d.y() / r.height() * view.height());
        last = e->position();
        update();
    }
    void wheelEvent(QWheelEvent *e) override { zoom(std::pow(0.999, e->angleDelta().y()), e->position()); }
};

// Writing area of the handwriting calibration; pen and finger come in as Qt's synthesized mouse events.
// marks: per written symbol a frame and, under it, the guess and its certainty.
struct Pad : QWidget {
    struct Mark { QRectF box; QString text; QColor color; };
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
            p.setPen(m.color.darker(130));
            p.drawText(QRectF(m.box.center().x() - 40, m.box.bottom() + 6, 80, 20), Qt::AlignHCenter | Qt::AlignTop, m.text);
        }
    }
};

// Settings > Calibrate handwriting: 12 pages, 3 per group (basics, letters + Greek, functions, calculus).
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
};
static constexpr double kSure = 0.9, kUnsure = 0.5;   // green from, red below
static void calibrateHandwriting(QWidget *parent, QList<Template> &personal, const QString &file,
                                 const QList<Prompt> &prompts = kCalibration, const QString &title = "Calibrate handwriting");
// Settings > Train a symbol (also from the calibration): one page with that symbol alone, its ink kept whole
// (an i's dot and stem stay one symbol however they are placed), repeated until it is green
static void trainSymbol(QWidget *parent, QList<Template> &personal, const QString &file) {
    QString sym = QInputDialog::getText(parent, "Train a symbol", "Which symbol? (e.g. i, x, α, ∫; or a word written joined up, e.g. sin)").remove(' ');
    if (!sym.isEmpty()) calibrateHandwriting(parent, personal, file, {{sym, sym}}, "Train \"" + sym + "\"");
}
static void calibrateHandwriting(QWidget *parent, QList<Template> &personal, const QString &file,
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
    d.exec();
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("Techy-Notes");
    app.setApplicationName("Techy-Notes");
    app.setWindowIcon(QIcon(":/icon.png"));
    QMainWindow w;
    w.setWindowTitle("Techy-Notes");
    auto canvas = new Canvas;
    w.setCentralWidget(canvas);
    QSettings st;
    if (st.allKeys().isEmpty()) {   // settings from before the Techy-Draw → Techy-Notes rename
        QSettings old("Techy-Draw", "Techy-Draw");
        for (const QString &k : old.allKeys()) st.setValue(k, old.value(k));
    }
    canvas->eraserButtons = st.value("pen/eraser", canvas->eraserButtons).toInt();
    canvas->menuButtons = st.value("pen/menu", canvas->menuButtons).toInt();
    canvas->pressLow = st.value("pen/low", canvas->pressLow).toFloat();
    canvas->pressHigh = st.value("pen/high", canvas->pressHigh).toFloat();
    auto sheetMenu = w.menuBar()->addMenu("Sheet");
    sheetMenu->addAction("Export PDF...", [&] { exportDialog(&w, canvas); })->setShortcut(Qt::CTRL | Qt::Key_E);
    sheetMenu->addAction("Page setup...", [&] { pageSetup(&w, canvas); });
    auto settings = w.menuBar()->addMenu("Settings");
    settings->addAction("Calibrate pen...", [&] { calibratePen(&w, canvas); });
    canvas->autoPages = st.value("pages/auto", true).toBool();
    auto autoPages = settings->addAction("Create pages automatically", [&](bool on) {
        canvas->autoPages = on;
        QSettings().setValue("pages/auto", on);
        canvas->update();   // ghost page
    });
    autoPages->setCheckable(true);
    autoPages->setChecked(canvas->autoPages);
    autoPages->setToolTip("Ink outside the pages, near a page bottom or on the ghost page creates pages. Off: only \"+\" adds pages.");
    canvas->leftHanded = st.value("pen/leftHanded", false).toBool();
    auto lefty = settings->addAction("Left-handed (pen ring opens to the right)", [=](bool on) {
        canvas->leftHanded = on;
        QSettings().setValue("pen/leftHanded", on);
    });
    lefty->setCheckable(true);
    lefty->setChecked(canvas->leftHanded);
    // Ctrl+Z restores the last notebook-tree deletion while the tree has focus (drawing takes the focus away)
    std::function<bool()> undoDelete = [] { return false; };

    auto tb = w.addToolBar("Tools");
    tb->setMovable(false);
    // Checkable action in an exclusive group; the first one starts checked.
    auto addChoice = [&](QActionGroup *g, const QString &text, auto onPick, QIcon icon = {}) {
        auto a = tb->addAction(icon, text, onPick);
        a->setCheckable(true);
        a->setChecked(g->actions().isEmpty());
        g->addAction(a);
    };

    auto tools = new QActionGroup(&w);
    for (int i = 0; i < 7; ++i) addChoice(tools, Canvas::kToolNames[i], [=] { canvas->tool = Canvas::Tool(i); });
    tools->actions()[Canvas::Snip]->setToolTip("Drag a rectangle to copy that area as an image");
    auto snap = tb->addAction("15°", [=](bool on) { canvas->snapLines = on; });
    snap->setCheckable(true);
    snap->setToolTip("Snap lines to 15° (or hold Shift)");
    auto finger = tb->addAction("Finger", [=](bool on) { canvas->fingerDraws = on; });
    finger->setCheckable(true);
    finger->setToolTip("Draw with one finger (two fingers still pan/zoom)");

    tb->addSeparator();
    auto colors = new QActionGroup(&w);
    for (const char *hex : Canvas::kColors) {
        QColor c(hex);
        QPixmap px(16, 16);
        px.fill(c);
        addChoice(colors, c.name(), [=] { canvas->setColor(c); }, QIcon(px));
    }

    tb->addSeparator();
    canvas->onPick = [=] {   // the pen ring changed the tool or colour: show it on the toolbar
        tools->actions()[canvas->tool]->setChecked(true);
        for (QAction *a : colors->actions())
            if (a->text() == canvas->color.name()) a->setChecked(true);
    };
    auto slider = new QSlider(Qt::Horizontal);   // tenths of a mm
    slider->setRange(1, 100);
    slider->setMaximumWidth(140);
    auto size = new QDoubleSpinBox;
    size->setRange(0.1, 10);
    size->setDecimals(1);
    size->setSingleStep(0.1);
    size->setSuffix(" mm");
    QObject::connect(slider, &QSlider::valueChanged, [=](int v) { size->setValue(v / 10.0); });
    QObject::connect(size, &QDoubleSpinBox::valueChanged, [=](double mm) {
        slider->setValue(qRound(mm * 10));
        canvas->width = mm;
    });
    size->setValue(canvas->width);
    tb->addWidget(slider);
    tb->addWidget(size);

    tb->addSeparator();
    tb->addAction("Undo", [&] { if (!undoDelete()) canvas->undo(); })->setShortcut(QKeySequence::Undo);
    tb->addAction("Redo", [=] { canvas->redo(); })->setShortcut(QKeySequence::Redo);
    tb->addSeparator();
    tb->addAction("Cut", [=] { canvas->copySelection(); canvas->deleteSelection(); })->setShortcut(QKeySequence::Cut);
    tb->addAction("Copy", [=] { canvas->copySelection(); })->setShortcut(QKeySequence::Copy);
    tb->addAction("Paste", [=] { canvas->paste(); })->setShortcut(QKeySequence::Paste);
    tb->addAction("Delete", [=] { canvas->deleteSelection(); })->setShortcut(QKeySequence::Delete);

    // ponytail: fixed notebook root, a "choose folder" setting when someone needs another place
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString rootDir = docs + "/Techy-Notes";
    // Folder from before the Techy-Draw → Techy-Notes rename: move it; if that fails, keep using it (never orphan notebooks)
    if (!QDir(rootDir).exists() && QDir(docs + "/Techy-Draw").exists() && !QDir().rename(docs + "/Techy-Draw", rootDir))
        rootDir = docs + "/Techy-Draw";
    if (!QDir(rootDir).exists() || QDir(rootDir).isEmpty()) {
        QDir().mkpath(rootDir + "/Notebook 1");
        saveSheet(Sheet{}, rootDir + "/Notebook 1/Sheet 1" + kExt);
    }
    imageDir = rootDir + "/.images";   // hidden from the tree like the trash
    auto model = new QStandardItemModel(&w);
    QStandardItem *root = model->invisibleRootItem();
    fill(root, rootDir);
    auto view = new Tree(canvas);
    view->setModel(model);
    view->setHeaderHidden(true);
    view->setDragDropMode(QAbstractItemView::InternalMove);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->expandAll();

    auto warn = [&](const QString &msg) { QMessageBox::warning(&w, "Techy-Notes", msg); };
    auto openSheet = [&](const QModelIndex &i) {
        QString p = i.data(PathRole).toString();
        if (p.endsWith(kExt) && p != canvas->path && !canvas->open(p)) warn("Could not open " + p);
    };
    QObject::connect(view, &QTreeView::clicked, openSheet);

    // Renames and drag moves change the model; mirror them on disk once the change is complete.
    auto sync = new QTimer(&w);
    sync->setSingleShot(true);
    auto rebuild = [&] {
        model->clear();
        fill(root, rootDir);
        view->expandAll();
    };
    QObject::connect(sync, &QTimer::timeout, [&] {
        canvas->save();
        if (syncFolder(root, rootDir, canvas)) return;
        warn("Name already taken or not allowed.");
        rebuild();
    });

    // Deleting moves the file/folder into a session trash inside the notebook root (same drive: a plain rename);
    // Ctrl+Z moves it back. The trash is emptied when the app closes.
    // ponytail: after a crash the trash stays as a hidden ".trash-*" folder in the root; clean it up at startup if they pile up
    QTemporaryDir trash(rootDir + "/.trash-XXXXXX");
    QList<std::pair<QString, QString>> deleted;   // (in trash, original path)
    auto remove = [&](QStandardItem *it) {
        QString from = it->data(PathRole).toString(), to = trash.filePath(QString::number(deleted.size()));
        auto holdsOpen = [&] { return canvas->path == from || canvas->path.startsWith(from + "/"); };
        if (holdsOpen()) canvas->open({});   // saves, then closes the sheet (unless saving failed)
        if (holdsOpen() || !trash.isValid() || !QDir().rename(from, to))
            return warn("Could not delete " + from);
        deleted.append({to, from});
        view->setFocus();   // Ctrl+Z restores it, even when the menu came from the pen
        (it->parent() ? it->parent() : root)->removeRow(it->row());   // rowsRemoved -> order.txt
    };
    undoDelete = [&] {
        if (!view->hasFocus() || deleted.isEmpty()) return false;
        auto [at, to] = deleted.last();
        if (QFileInfo::exists(to) || !QDir().rename(at, to)) {
            warn("Could not restore " + to);
            return true;
        }
        deleted.removeLast();
        rebuild();
        return true;
    };
    QObject::connect(model, &QStandardItemModel::itemChanged, sync, qOverload<>(&QTimer::start));
    QObject::connect(model, &QAbstractItemModel::rowsRemoved, sync, qOverload<>(&QTimer::start));

    auto add = [&](QStandardItem *folder, bool sheet) {
        QString dir = folder == root ? rootDir : folder->data(PathRole).toString(), path;
        QString base = sheet ? "Sheet " : folder == root ? "Notebook " : "Chapter ";
        for (int n = 1; QFileInfo::exists(path = dir + "/" + base + QString::number(n) + (sheet ? kExt : "")); ++n) {}
        if (sheet ? !saveSheet(Sheet{}, path) : !QDir().mkdir(path)) return warn("Could not create " + path);
        QStandardItem *it = makeItem(path);
        folder->appendRow(it);
        sync->start();   // order.txt
        view->expand(folder->index());
        view->setCurrentIndex(it->index());
        if (sheet) openSheet(it->index());
        view->edit(it->index());
    };
    // Hard limit: one menu at a time (exec() runs a nested event loop that can deliver more requests).
    // Cooldown: none within 500 ms after the last one closed. Calibration knob for pens that repeat presses.
    bool menuOpen = false;
    QElapsedTimer sinceMenu;
    QObject::connect(view, &QWidget::customContextMenuRequested, [&](QPoint pos) {
        if (menuOpen || (sinceMenu.isValid() && sinceMenu.elapsed() < 500)) return;
        menuOpen = true;
        auto done = qScopeGuard([&] { menuOpen = false; sinceMenu.start(); });
        QStandardItem *it = model->itemFromIndex(view->indexAt(pos));
        QStandardItem *folder = !it ? root : !isSheet(it) ? it : it->parent() ? it->parent() : root;
        QMenu m;
        m.addAction("New sheet", [&] { add(folder, true); });
        m.addAction(folder == root ? "New notebook" : "New chapter", [&] { add(folder, false); });
        if (it) m.addAction("Rename", [&] { view->edit(it->index()); });
        if (it) m.addAction("Delete", [&] { remove(it); });
        if (!deleted.isEmpty())
            m.addAction("Undo delete of \"" + QFileInfo(deleted.last().second).completeBaseName() + "\" (Ctrl+Z)", [&] {
                view->setFocus();
                undoDelete();
            });
        m.exec(view->viewport()->mapToGlobal(pos));
    });

    QModelIndexList first = model->match(model->index(0, 0), PathRole, kExt, 1, Qt::MatchEndsWith | Qt::MatchRecursive);
    if (!first.isEmpty()) {
        view->setCurrentIndex(first[0]);
        openSheet(first[0]);
    }

    auto dock = new QDockWidget("Notebooks");
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(view);
    w.addDockWidget(Qt::LeftDockWidgetArea, dock);
    tb->addSeparator();
    auto full = tb->addAction("Full screen", [&](bool on) {
        w.setWindowState(on ? w.windowState() | Qt::WindowFullScreen : w.windowState() & ~Qt::WindowFullScreen);
        dock->setVisible(!on);
    });
    full->setCheckable(true);
    full->setShortcut(Qt::Key_F11);

    // Math dock: one expression per line -> preview; Insert puts it on the canvas, Update changes the edited object
    auto mathDock = new QDockWidget("Math");
    mathDock->setFeatures(QDockWidget::DockWidgetClosable);
    auto mathEdit = new QPlainTextEdit;
    mathEdit->setPlaceholderText("One expression per line, e.g.\ny = x^2 - 2x\nsin(x)/x");
    mathEdit->setMaximumHeight(120);
    auto preview = new MathView;
    preview->setMinimumSize(260, 220);
    auto kind = new QComboBox;
    kind->addItems({"Plot", "Equation"});
    auto zoomOut = new QPushButton("−"), zoomIn = new QPushButton("+"), upd = new QPushButton("Update"), ins = new QPushButton("Insert");
    for (QPushButton *b : {zoomOut, zoomIn}) b->setFixedWidth(32);
    upd->setEnabled(false);
    auto mathBox = new QWidget;
    auto mathLay = new QVBoxLayout(mathBox);
    auto mathRow = new QHBoxLayout;
    for (QWidget *x : std::initializer_list<QWidget *>{kind, zoomOut, zoomIn}) mathRow->addWidget(x);
    mathRow->addStretch();
    mathRow->addWidget(upd);
    mathRow->addWidget(ins);
    // Recognized symbols, in reading order: tap the wrong ones, then Correct; each fix becomes a personal template
    auto strip = new QListWidget;
    strip->setFlow(QListView::LeftToRight);
    strip->setWrapping(false);
    strip->setFixedHeight(48);
    strip->setFont(QFont("Cambria", 16));
    strip->setSelectionMode(QAbstractItemView::MultiSelection);   // a tap toggles: touch and pen have no Ctrl
    strip->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    strip->setToolTip("Recognized symbols: double-tap one to rewrite it, or tap the wrong ones, then Correct (or merge), Split or Delete");
    auto fix = new QPushButton("Correct..."), split = new QPushButton("Split"), drop = new QPushButton("Delete");
    split->setToolTip("A symbol that is really several: one per stroke (then Correct them)");
    auto stripRow = new QWidget;
    auto stripLay = new QHBoxLayout(stripRow);
    stripLay->setContentsMargins(0, 0, 0, 0);
    stripLay->addWidget(strip, 1);
    for (QPushButton *b : {fix, split, drop}) stripLay->addWidget(b), b->setEnabled(false);
    stripRow->hide();
    QObject::connect(strip, &QListWidget::itemSelectionChanged, [=] {
        for (QPushButton *b : {fix, split, drop}) b->setEnabled(!strip->selectedItems().isEmpty());
    });
    mathLay->addWidget(mathEdit);
    mathLay->addWidget(stripRow);
    mathLay->addWidget(preview, 1);
    mathLay->addLayout(mathRow);
    mathDock->setWidget(mathBox);
    w.addDockWidget(Qt::RightDockWidgetArea, mathDock);
    mathDock->hide();
    QObject::connect(mathEdit, &QPlainTextEdit::textChanged, [=] {
        preview->lines = mathEdit->toPlainText().split('\n');
        preview->update();
    });
    QObject::connect(zoomOut, &QPushButton::clicked, [=] { preview->zoom(1.25, preview->plotRect().center()); });
    QObject::connect(zoomIn, &QPushButton::clicked, [=] { preview->zoom(0.8, preview->plotRect().center()); });
    auto objectText = [=] {
        auto n = [](double v) { return QString::number(v, 'g', 10); };
        QRectF v = preview->view;
        QString head = kind->currentIndex() ? "eq" : "plot " + n(v.left()) + " " + n(v.right()) + " " + n(v.top()) + " " + n(v.bottom());
        return head + "\n" + mathEdit->toPlainText();
    };
    // first, count: the lines of the text box that hold the recognized text (corrections rewrite only those)
    struct Recognized { QList<QPolygonF> ink; QList<InkSymbol> syms; QList<Template> personal; int first = 0, count = 0; };
    auto rec = std::make_shared<Recognized>();
    const QString personalFile = rootDir + "/.templates.tpl";   // the user's own handwriting, all notebooks
    rec->personal = loadTemplates(personalFile);
    settings->addAction("Calibrate handwriting...", [=, &w] { calibrateHandwriting(&w, rec->personal, personalFile); });
    settings->addAction("Train a symbol...", [=, &w] { trainSymbol(&w, rec->personal, personalFile); });
    auto fillStrip = [=] {
        QSignalBlocker quiet(strip);   // filling is no edit
        strip->clear();
        stripRow->setVisible(!rec->syms.isEmpty());
        for (QList<int> line : inkLayout(rec->syms).lines) {
            std::sort(line.begin(), line.end(), [&](int a, int b) { return rec->syms[a].box.left() < rec->syms[b].box.left(); });
            for (int i : line) {
                auto it = new QListWidgetItem(rec->syms[i].cands.value(0), strip);
                it->setData(Qt::UserRole, i);
                it->setFlags(it->flags() | Qt::ItemIsEditable);
            }
        }
        for (QPushButton *b : {fix, split, drop}) b->setEnabled(false);
    };
    auto putText = [=] {   // the recognized lines, in place in the text box; the other lines stay
        QString all = mathEdit->toPlainText();
        QStringList lines = all.isEmpty() ? QStringList{} : all.split('\n');
        QString mine = inkLayout(rec->syms).text;
        rec->first = std::min<int>(rec->first, lines.size());
        lines = lines.mid(0, rec->first) + (mine.isEmpty() ? QStringList{} : mine.split('\n')) + lines.mid(rec->first + rec->count);
        rec->count = mine.isEmpty() ? 0 : mine.count('\n') + 1;
        mathEdit->setPlainText(lines.join('\n'));
    };
    auto forget = [=] {   // the dock no longer shows recognized ink
        rec->syms.clear();
        fillStrip();
    };
    auto learn = [=, &w](InkSymbol &s, const QString &sym) {   // s is sym: its first candidate, and a personal template
        s.cands.removeAll(sym);
        s.cands.prepend(sym);
        rec->personal << makeTemplate(sym, inkOf(rec->ink, s.strokes));
        if (!saveTemplates(rec->personal, personalFile)) QMessageBox::warning(&w, "Techy-Notes", "Could not save " + personalFile);
    };
    auto selected = [=] {   // the symbols picked in the strip
        QList<int> picked;
        for (QListWidgetItem *it : strip->selectedItems()) picked << it->data(Qt::UserRole).toInt();
        return picked;
    };
    // One selected: its alternatives. Several: type what they are, one character each ("sin" for three), or a
    // single symbol to merge them (one symbol read as two).
    QObject::connect(fix, &QPushButton::clicked, [=, &w] {
        QList<int> picked = selected();
        std::sort(picked.begin(), picked.end(), [&](int a, int b) { return rec->syms[a].box.left() < rec->syms[b].box.left(); });
        QString typed;
        if (picked.size() == 1) {
            QMenu m;
            for (const QString &c : rec->syms[picked[0]].cands.mid(1)) m.addAction(c)->setData(c);
            m.addSeparator();
            m.addAction("Other...")->setData(QString());
            QAction *a = m.exec(QCursor::pos());
            if (!a) return;
            typed = a->data().toString();
        }
        if (typed.isEmpty())
            typed = QInputDialog::getText(&w, "Correct", picked.size() == 1 ? "What did you write here? (e.g. x, 2, α, ∫)"
                                                                            : "What did you write here? One character per symbol (e.g. sin), or one symbol to merge them")
                        .remove(' ');
        if (typed.isEmpty()) return;
        QStringList syms;   // per character; a surrogate pair stays one
        for (int k = 0; k < typed.size(); ++k) syms << (typed[k].isHighSurrogate() && k + 1 < typed.size() ? typed.mid(k++, 2) : typed.mid(k, 1));
        if (picked.size() == 1 || syms.size() == 1) {   // one symbol (merged if several were picked)
            InkSymbol &s = rec->syms[picked[0]];
            for (int k = picked.size() - 1; k > 0; --k) s.strokes << rec->syms[picked[k]].strokes, s.box |= rec->syms[picked[k]].box;
            std::sort(s.strokes.begin(), s.strokes.end());
            learn(s, typed);
            std::sort(picked.begin() + 1, picked.end(), std::greater<int>());
            for (int k = 1; k < picked.size(); ++k) rec->syms.removeAt(picked[k]);
        } else if (syms.size() == picked.size()) {
            for (int k = 0; k < picked.size(); ++k) learn(rec->syms[picked[k]], syms[k]);
        } else {
            return (void)QMessageBox::information(&w, "Correct", QString("%1 symbols are selected, but \"%2\" has %3. Type one per symbol, or one to merge them.")
                                                                     .arg(picked.size()).arg(typed).arg(syms.size()));
        }
        putText();
        fillStrip();
    });
    QObject::connect(split, &QPushButton::clicked, [=] {   // wrongly joined: one symbol per stroke, classified anew
        QList<int> picked = selected();
        std::sort(picked.begin(), picked.end(), std::greater<int>());
        for (int i : picked) {
            QList<int> strokes = rec->syms[i].strokes;
            if (strokes.size() < 2) continue;
            rec->syms.removeAt(i);
            for (int k = 0; k < strokes.size(); ++k) rec->syms.insert(i + k, symbolOf(rec->ink, {strokes[k]}, rec->personal));
        }
        putText();
        fillStrip();
    });
    QObject::connect(drop, &QPushButton::clicked, [=] {   // stray ink: left out of the text, nothing learned
        QList<int> picked = selected();
        std::sort(picked.begin(), picked.end(), std::greater<int>());
        for (int i : picked) rec->syms.removeAt(i);
        putText();
        fillStrip();
    });
    QObject::connect(strip, &QListWidget::itemChanged, [=](QListWidgetItem *item) {   // double-tap rewrite
        int i = item->data(Qt::UserRole).toInt();
        QString typed = item->text().remove(' ');
        if (typed.isEmpty() || typed == rec->syms[i].cands.value(0)) return;
        learn(rec->syms[i], typed);
        putText();
        QTimer::singleShot(0, strip, fillStrip);   // not while the edited item is still in use
    });
    canvas->onRecognize = [=](const QList<QPolygonF> &ink) {   // new lines under what the text box holds
        rec->ink = ink;
        rec->syms = recognizeSymbols(ink, rec->personal);
        QString all = mathEdit->toPlainText();
        while (all.endsWith('\n')) all.chop(1);
        mathEdit->setPlainText(all);
        rec->first = all.isEmpty() ? 0 : all.count('\n') + 1;
        rec->count = 0;
        putText();
        fillStrip();
        mathDock->show();
    };
    QObject::connect(ins, &QPushButton::clicked, [=, &w] {
        int ink = 0;
        if (!rec->syms.isEmpty() && canvas->inkValid()) {   // from recognized ink: replace it or keep it
            QMessageBox box(QMessageBox::Question, "Insert", "Replace the handwritten ink with the inserted math?", QMessageBox::NoButton, &w);
            QAbstractButton *replace = box.addButton("Replace ink", QMessageBox::AcceptRole);
            QAbstractButton *keep = box.addButton("Keep ink", QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() != replace && box.clickedButton() != keep) return;
            ink = box.clickedButton() == replace ? 2 : 1;
        }
        canvas->insertMath(objectText(), ink);
        forget();
        upd->setEnabled(true);
    });
    QObject::connect(upd, &QPushButton::clicked, [=] { upd->setEnabled(canvas->updateMath(objectText())); });
    canvas->onEditMath = [=](const QString &m) {
        forget();
        QStringList l = m.split('\n'), h = l.takeFirst().split(' ', Qt::SkipEmptyParts);
        kind->setCurrentIndex(h.value(0) == "plot" ? 0 : 1);
        if (h.size() == 5) preview->view = QRectF(QPointF(h[1].toDouble(), h[3].toDouble()), QPointF(h[2].toDouble(), h[4].toDouble()));
        mathEdit->setPlainText(l.join('\n'));
        upd->setEnabled(true);
        mathDock->show();
    };
    canvas->onMath = [=] { mathDock->setVisible(!mathDock->isVisible()); };
    QAction *sigma = mathDock->toggleViewAction();
    sigma->setText("∑");
    sigma->setToolTip("Math: type expressions, see them formatted and plotted, insert them");
    tb->addAction(sigma);

    w.resize(1280, 800);
    w.show();
    int rc = app.exec();
    canvas->save();
    return rc;
}
