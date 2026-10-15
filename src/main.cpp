#include "Canvas.h"
#include <QActionGroup>
#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
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
    auto tools = new QActionGroup(&w);
    const char *names[] = {"Pen", "Line", "Select", "Eraser"};
    for (int i = 0; i < 4; ++i) {
        auto a = tb->addAction(names[i], [=] { canvas->tool = Canvas::Tool(i); });
        a->setCheckable(true);
        a->setChecked(i == 0);
        tools->addAction(a);
    }

    auto dock = new QDockWidget("Notebooks");
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(new QTreeView);   // filled in milestone 4
    w.addDockWidget(Qt::LeftDockWidgetArea, dock);

    w.resize(1280, 800);
    w.show();
    return app.exec();
}
