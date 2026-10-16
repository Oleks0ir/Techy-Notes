#include "Canvas.h"
#include <QActionGroup>
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

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("Techy-Notes");
    app.setApplicationName("Techy-Notes");
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

    w.resize(1280, 800);
    w.show();
    int rc = app.exec();
    canvas->save();
    return rc;
}
