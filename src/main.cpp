#include "Canvas.h"
#include <QActionGroup>
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
#include <QVBoxLayout>
#include <functional>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QMenu>
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

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("Techy-Draw");
    app.setApplicationName("Techy-Draw");
    QMainWindow w;
    w.setWindowTitle("Techy-Draw");
    auto canvas = new Canvas;
    w.setCentralWidget(canvas);
    QSettings st;
    st.remove("pen/buttons");   // keys of the first calibration version
    st.remove("pen/erases");
    canvas->eraserButtons = st.value("pen/eraser", canvas->eraserButtons).toInt();
    canvas->menuButtons = st.value("pen/menu", canvas->menuButtons).toInt();
    canvas->pressLow = st.value("pen/low", canvas->pressLow).toFloat();
    canvas->pressHigh = st.value("pen/high", canvas->pressHigh).toFloat();
    w.menuBar()->addMenu("Settings")->addAction("Calibrate pen...", [&] { calibratePen(&w, canvas); });

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
    const char *names[] = {"Pen", "Line", "Lasso", "Rect", "Eraser"};   // Canvas::Tool order
    for (int i = 0; i < 5; ++i) addChoice(tools, names[i], [=] { canvas->tool = Canvas::Tool(i); });
    auto snap = tb->addAction("15°", [=](bool on) { canvas->snapLines = on; });
    snap->setCheckable(true);
    snap->setToolTip("Snap lines to 15° (or hold Shift)");
    auto finger = tb->addAction("Finger", [=](bool on) { canvas->fingerDraws = on; });
    finger->setCheckable(true);
    finger->setToolTip("Draw with one finger (two fingers still pan/zoom)");

    tb->addSeparator();
    auto colors = new QActionGroup(&w);
    for (const char *hex : {"#000000", "#1565c0", "#c62828", "#2e7d32"}) {
        QColor c(hex);
        QPixmap px(16, 16);
        px.fill(c);
        addChoice(colors, c.name(), [=] { canvas->setColor(c); }, QIcon(px));
    }

    tb->addSeparator();
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
    tb->addAction("Undo", [=] { canvas->undo(); })->setShortcut(QKeySequence::Undo);
    tb->addAction("Redo", [=] { canvas->redo(); })->setShortcut(QKeySequence::Redo);
    tb->addSeparator();
    tb->addAction("Cut", [=] { canvas->copySelection(); canvas->deleteSelection(); })->setShortcut(QKeySequence::Cut);
    tb->addAction("Copy", [=] { canvas->copySelection(); })->setShortcut(QKeySequence::Copy);
    tb->addAction("Paste", [=] { canvas->paste(); })->setShortcut(QKeySequence::Paste);
    tb->addAction("Delete", [=] { canvas->deleteSelection(); })->setShortcut(QKeySequence::Delete);

    // ponytail: fixed notebook root, a "choose folder" setting when someone needs another place
    const QString rootDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/Techy-Draw";
    if (!QDir(rootDir).exists() || QDir(rootDir).isEmpty()) {
        QDir().mkpath(rootDir + "/Notebook 1");
        saveSheet(Sheet{}, rootDir + "/Notebook 1/Sheet 1" + kExt);
    }
    auto model = new QStandardItemModel(&w);
    QStandardItem *root = model->invisibleRootItem();
    fill(root, rootDir);
    auto view = new Tree(canvas);
    view->setModel(model);
    view->setHeaderHidden(true);
    view->setDragDropMode(QAbstractItemView::InternalMove);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->expandAll();

    auto warn = [&](const QString &msg) { QMessageBox::warning(&w, "Techy-Draw", msg); };
    auto openSheet = [&](const QModelIndex &i) {
        QString p = i.data(PathRole).toString();
        if (p.endsWith(kExt) && p != canvas->path && !canvas->open(p)) warn("Could not open " + p);
    };
    QObject::connect(view, &QTreeView::clicked, openSheet);

    // Renames and drag moves change the model; mirror them on disk once the change is complete.
    auto sync = new QTimer(&w);
    sync->setSingleShot(true);
    QObject::connect(sync, &QTimer::timeout, [&] {
        canvas->save();
        if (syncFolder(root, rootDir, canvas)) return;
        warn("Name already taken or not allowed.");
        model->clear();
        fill(root, rootDir);
        view->expandAll();
    });
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

    w.resize(1280, 800);
    w.show();
    int rc = app.exec();
    canvas->save();
    return rc;
}
