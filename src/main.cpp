#include "Canvas.h"
#include <QActionGroup>
#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QMainWindow>
#include <QSlider>
#include <QToolBar>
#include <QTreeView>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QMainWindow w;
    w.setWindowTitle("Techy-Draw");
    auto canvas = new Canvas;
    w.setCentralWidget(canvas);

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
    const char *names[] = {"Pen", "Line", "Select", "Eraser"};
    for (int i = 0; i < 4; ++i) addChoice(tools, names[i], [=] { canvas->tool = Canvas::Tool(i); });
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
        addChoice(colors, c.name(), [=] { canvas->color = c; }, QIcon(px));
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

    auto dock = new QDockWidget("Notebooks");
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(new QTreeView);   // filled in milestone 4
    w.addDockWidget(Qt::LeftDockWidgetArea, dock);

    w.resize(1280, 800);
    w.show();
    return app.exec();
}
