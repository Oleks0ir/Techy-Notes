#pragma once
#include "Math.h"
#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>

// Shared by the Math graph plug-in (its dock) and the Math recognizer (its corrections and formula pages).
// Math dock preview, in two panes: the lines typeset (plot false), and the plot (4:3 like an inserted plot; drag
// pans, wheel or ± zooms). The formula is where recognized ink is corrected: a tap on a character reports it (its
// position in the text), framed characters are the selected ink symbols, the shaded one the □ being filled.
// ponytail: no pinch zoom here (finger drag pans via Qt's synthesized mouse); add a QPinchGesture if ± is not enough
struct MathView : QWidget {
    bool plot = false;
    QStringList lines;
    QRectF view{-10, -7.5, 20, 15};   // math units: x from left, y from top (the bottom of the plot) up
    QPointF last;
    QHash<int, QRectF> glyphs;   // where each character of the lines was drawn, by its position in the text
    QSet<int> framed;            // characters of the selected ink symbols
    int filled = -1;             // the □ selected in the text box
    bool large = false;          // recognized ink is being corrected: the formula gets more room
    std::function<void(int)> tapped, doubleTapped;

    QRectF eqRect() const {
        qsizetype n = std::count_if(lines.begin(), lines.end(), [](const QString &l) { return !l.trimmed().isEmpty(); });
        return plot ? QRectF() : QRectF(8, 8, width() - 16.0, std::min(height() - 16.0, n * (large ? 64.0 : 40.0)));
    }
    QRectF plotRect() const {
        if (!plot) return {};
        QRectF a = QRectF(rect()).adjusted(8, 8, -8, -8);
        QSizeF s(4, 3);
        s.scale(a.size(), Qt::KeepAspectRatio);
        return {a.topLeft(), s};
    }
    int at(QPointF p) const {   // the smallest character under p, a finger's width around each; -1: none
        int best = -1;
        for (auto it = glyphs.begin(); it != glyphs.end(); ++it)
            if (it->adjusted(-6, -6, 6, 6).contains(p) && (best < 0 || it->width() * it->height() < glyphs[best].width() * glyphs[best].height()))
                best = it.key();
        return best;
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
        p.setPen(Qt::black);   // the painter starts with the palette's text colour: white in Windows dark mode
        if (plot) {
            drawPlot(p, lines, plotRect(), view, 1);
            p.setPen(Qt::lightGray);
            return p.drawRect(plotRect());
        }
        drawMath(p, "eq\n" + lines.join('\n'), eqRect(), &glyphs);
        if (glyphs.contains(filled)) p.fillRect(glyphs[filled], QColor(0, 120, 215, 60));
        p.setPen(QPen(QColor(0, 120, 215), 1.5));
        p.setBrush(Qt::NoBrush);
        for (int c : framed)
            if (glyphs.contains(c)) p.drawRoundedRect(glyphs[c].adjusted(-2, -2, 2, 2), 3, 3);
    }
    bool panning = false;   // the press was on the plot: a drag pans it (one on the formula never does)
    void mousePressEvent(QMouseEvent *e) override {
        last = e->position();
        panning = plotRect().contains(last);
        if (int c = plot ? -1 : at(last); c >= 0 && tapped) tapped(c);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        if (int c = at(e->position()); c >= 0 && doubleTapped) doubleTapped(c);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (!panning) return;
        QRectF r = plotRect();
        QPointF d = e->position() - last;
        view.translate(-d.x() / r.width() * view.width(), d.y() / r.height() * view.height());
        last = e->position();
        update();
    }
    void wheelEvent(QWheelEvent *e) override {
        if (plot) zoom(std::pow(0.999, e->angleDelta().y()), e->position());
    }
};

// Math dock text box: Tab selects the next □ (from the top after the last), to type into it.
struct MathEdit : QPlainTextEdit {
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() != Qt::Key_Tab) return QPlainTextEdit::keyPressEvent(e);
        QTextCursor c = document()->find(QString(kBox), textCursor().selectionEnd());
        if (c.isNull()) c = document()->find(QString(kBox), 0);
        if (!c.isNull()) setTextCursor(c);
    }
};

// The Math graph's dock as other plug-ins get it: its output "editor" (type "math-editor"). They add their own rows
// and buttons to it and hook into it.
struct MathEditor {
    QDockWidget *dock;
    MathEdit *edit;            // one expression per line
    MathView *preview, *plot;
    QComboBox *kind;           // 0: Plot, 1: Equation
    QPushButton *update;       // on while the object last inserted or edited can be updated
    QVBoxLayout *lay;          // structure buttons, text box, the two panes, then the button row
    QHBoxLayout *row;          // the buttons: kind, zoom, ..., Update, Insert
    std::function<void(int)> selectAt;   // the character at that position selected (a □: to type into it)
    std::function<int()> selectedBox;    // the □ selected in the text box: its position; -1: none
    // Hooks, empty when no plug-in takes them
    std::function<bool(const QString &)> reread;   // a structure button: true if it was taken (else inserted as text)
    std::function<int()> askInk;         // Insert: 0 at the view centre, 1 next to its ink, 2 in place of it; -1 cancels
    std::function<void(int)> inserted;   // after Insert, with askInk's answer
    std::function<void()> editing;       // Edit math: before the object's text is loaded
};
Q_DECLARE_METATYPE(MathEditor *)