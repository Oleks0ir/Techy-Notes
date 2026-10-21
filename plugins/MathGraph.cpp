// Math graph plug-in: the Math dock (typed expressions, structure buttons, the formula typeset and plotted), math
// objects on the canvas (plots and equations) and their Insert / Update / Edit. Other plug-ins work in its dock
// through its output "editor".
#include "MathView.h"
#include "Plugin.h"
#include <QAction>
#include <QMainWindow>
#include <QMenu>
#include <QProxyStyle>
#include <QSplitter>
#include <QToolBar>
#include <QToolButton>

class MathGraph : public QObject, public Plugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "io.github.oleks0ir.techynotes.Plugin/1")
    Q_INTERFACES(Plugin)

public:
    QString name() const override { return "Math graph"; }
    QList<Port> outputs() const override { return {{"editor", "math-editor"}}; }
    void start(Host &host) override;
};

void MathGraph::start(Host &host) {
    QMainWindow *w = host.window;
    Canvas *canvas = host.canvas;
    auto draw = [](QPainter &p, const QString &m, const QRectF &r) { drawMath(p, m, r); };
    objectKinds["plot"] = {draw, {}};
    objectKinds["eq"] = {draw, mathSize};

    // Math dock: one expression per line -> preview; Insert puts it on the canvas, Update changes the edited object
    auto mathDock = new QDockWidget("Math");
    mathDock->setFeatures(QDockWidget::DockWidgetClosable);
    auto mathEdit = new MathEdit;
    mathEdit->setPlaceholderText("One expression per line, e.g.\ny = x^2 - 2x\nsin(x)/x\nThe buttons above insert structures; Tab goes to the next □");
    mathEdit->setMaximumHeight(120);
    auto preview = new MathView, plotView = new MathView;   // the formula, the plot: two panes, the divider between them
    plotView->plot = true;
    preview->setMinimumSize(260, 60);
    auto panes = new QSplitter(Qt::Vertical);
    panes->addWidget(preview);
    panes->addWidget(plotView);
    panes->setStretchFactor(1, 2);
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
    auto ed = new MathEditor{mathDock, mathEdit, preview, plotView, kind, upd, mathLay, mathRow};   // lives as long as the app
    // Structure buttons (Word's Equation tab): a group's first template, its menu holds them all. Inserted at the
    // cursor, the selected text goes into its first □, and the first □ left gets selected to type into.
    ed->selectAt = [=](int at) {   // the character at position at of the text, selected (a □: to type into it)
        QTextCursor c = mathEdit->textCursor();
        c.setPosition(at);
        c.setPosition(std::min<int>(at + 1, int(mathEdit->toPlainText().size())), QTextCursor::KeepAnchor);
        mathEdit->setTextCursor(c);
        mathEdit->setFocus();
    };
    auto insertStructure = [=](QString t) {
        QTextCursor c = mathEdit->textCursor();
        QString sel = c.selectedText();
        if (!sel.isEmpty() && sel != kBox && !sel.contains(QChar::ParagraphSeparator)) t.replace(t.indexOf(kBox), 1, sel);
        int start = c.selectionStart();
        c.insertText(t);
        QTextCursor box = mathEdit->document()->find(QString(kBox), start);
        if (!box.isNull() && box.selectionEnd() <= start + t.size()) c = box;
        mathEdit->setTextCursor(c);
        mathEdit->setFocus();
    };
    auto apply = [=](const QString &t) {   // with symbols selected in the preview, a plug-in may read them as that structure
        if (!ed->reread || !ed->reread(t)) insertStructure(t);
    };
    auto mathIcon = [w](const QString &t) {   // the template typeset, centred
        double dpr = w->devicePixelRatioF();
        QPixmap pm(QSize(48, 48) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(w->palette().color(QPalette::WindowText));
        QSizeF s = mathSize("eq\n" + t);
        s.scale(44, 44, Qt::KeepAspectRatio);
        drawMath(p, "eq\n" + t, QRectF(QPointF(24 - s.width() / 2, 24 - s.height() / 2), s));
        return QIcon(pm);
    };
    struct BigIcons : QProxyStyle {   // menu icons large enough to read a formula
        int pixelMetric(PixelMetric m, const QStyleOption *o, const QWidget *wd) const override {
            return m == PM_SmallIconSize ? 40 : QProxyStyle::pixelMetric(m, o, wd);
        }
    };
    auto bigIcons = new BigIcons;
    bigIcons->setParent(w);
    auto structures = new QToolBar;   // too narrow: the rest goes behind its » button
    structures->setIconSize({32, 32});
    for (const auto &[name, ts] : kStructures) {
        auto b = new QToolButton;
        b->setIcon(mathIcon(ts[0]));
        b->setToolTip(name);
        if (ts.size() == 1) {
            QObject::connect(b, &QToolButton::clicked, [=, t = ts[0]] { apply(t); });
        } else {
            auto m = new QMenu(b);
            m->setStyle(bigIcons);
            for (const QString &t : ts) m->addAction(mathIcon(t), t, [=] { apply(t); });
            b->setMenu(m);
            b->setPopupMode(QToolButton::InstantPopup);
        }
        structures->addWidget(b);
    }
    mathLay->addWidget(structures);
    mathLay->addWidget(mathEdit);
    mathLay->addWidget(panes, 1);
    mathLay->addLayout(mathRow);
    mathDock->setWidget(mathBox);
    w->addDockWidget(Qt::RightDockWidgetArea, mathDock);
    mathDock->hide();
    ed->selectedBox = [=] {   // the □ selected in the text box: its position, -1: none
        QTextCursor c = mathEdit->textCursor();
        return c.selectedText() == kBox ? c.selectionStart() : -1;
    };
    QObject::connect(zoomOut, &QPushButton::clicked, [=] { plotView->zoom(1.25, plotView->plotRect().center()); });
    QObject::connect(zoomIn, &QPushButton::clicked, [=] { plotView->zoom(0.8, plotView->plotRect().center()); });
    auto objectText = [=] {
        QStringList lines = mathEdit->toPlainText().split('\n');
        bool curve = false;   // nothing to plot (∫, a matrix, x = 3): typeset it rather than insert an empty plot
        for (int i = 0; i < lines.size() && !curve; ++i)
            if (MathNode c = curveOf(lines, i); c.k) curve = bool(compileCurve(exprtkOf(c)));
        if (!curve) kind->setCurrentIndex(1);
        auto n = [](double v) { return QString::number(v, 'g', 10); };
        QRectF v = plotView->view;
        QString head = kind->currentIndex() ? "eq" : "plot " + n(v.left()) + " " + n(v.right()) + " " + n(v.top()) + " " + n(v.bottom());
        return head + "\n" + mathEdit->toPlainText();
    };
    QObject::connect(mathEdit, &QPlainTextEdit::textChanged, [=] {
        preview->lines = plotView->lines = mathEdit->toPlainText().split('\n');
        plotView->update();
        preview->update();
    });
    auto shade = [=] {   // the selected □ shaded in the preview
        preview->filled = ed->selectedBox();
        preview->update();
    };
    QObject::connect(mathEdit, &QPlainTextEdit::textChanged, shade);
    QObject::connect(mathEdit, &QPlainTextEdit::selectionChanged, shade);
    preview->tapped = ed->selectAt;   // a tap on the formula: edit it there

    QObject::connect(ins, &QPushButton::clicked, [=] {
        int ink = ed->askInk ? ed->askInk() : 0;
        if (ink < 0) return;
        canvas->insertMath(objectText(), ink);
        upd->setEnabled(true);
        if (ed->inserted) ed->inserted(ink);
    });
    QObject::connect(upd, &QPushButton::clicked, [=] { upd->setEnabled(canvas->updateMath(objectText())); });
    canvas->objectMenus << [=](QPointF, int i) -> QList<Canvas::MenuItem> {   // a math object: its text into the dock
        if (i < 0 || !QStringList{"plot", "eq"}.contains(kindOf(canvas->sheet.strokes[i].math))) return {};
        return {{QChar(0xE70F), "Edit math", [=] {
                     canvas->editMath(i);   // Update changes it
                     if (ed->editing) ed->editing();
                     QStringList l = canvas->sheet.strokes[i].math.split('\n'), h = l.takeFirst().split(' ', Qt::SkipEmptyParts);
                     kind->setCurrentIndex(h.value(0) == "plot" ? 0 : 1);
                     if (h.size() == 5) plotView->view = QRectF(QPointF(h[1].toDouble(), h[3].toDouble()), QPointF(h[2].toDouble(), h[4].toDouble()));
                     mathEdit->setPlainText(l.join('\n'));
                     upd->setEnabled(true);
                     mathDock->show();
                 }}};
    };
    canvas->ringItems << Canvas::MenuItem{QChar(0xE8EF), "Math", [=] { mathDock->setVisible(!mathDock->isVisible()); }};
    QAction *sigma = mathDock->toggleViewAction();
    sigma->setText("∑");
    sigma->setToolTip("Math: type expressions, see them formatted and plotted, insert them");
    host.tools->addAction(sigma);
    host.send(this, "editor", QVariant::fromValue(ed));
}

#include "MathGraph.moc"
