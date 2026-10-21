#include "Plugin.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QLabel>
#include <QLibrary>
#include <QMouseEvent>
#include <QPluginLoader>
#include <QSettings>
#include <QVBoxLayout>

namespace {
struct CanvasNode : Plugin {
    QString name() const override { return "Canvas"; }
    QList<Port> outputs() const override { return {{"selection", "ink", "Selection"}}; }
    void start(Host &) override {}
};

Port portOf(const QList<Port> &ports, const QString &name) {   // type empty: none
    for (const Port &p : ports)
        if (p.name == name) return p;
    return {};
}

// Settings > Plug-ins: a box per plug-in (the canvas first), its inputs on the left and outputs on the right,
// coloured by type. Drag an output onto an input to wire them; tap a wire to remove it.
struct NodeView : QGraphicsView {
    Host &h;
    QGraphicsScene sc;
    QGraphicsLineItem *drag = nullptr;   // the wire being drawn, from the output `from`
    int fromNode = -1;
    QString fromPort;

    explicit NodeView(Host &host) : h(host) {
        setScene(&sc);
        setRenderHint(QPainter::Antialiasing);
        build();
    }
    static QColor colorOf(const QString &type) { return QColor::fromHsv(int(qHash(type) % 360), 150, 210); }
    void build() {
        sc.clear();
        drag = nullptr;
        QHash<std::pair<Plugin *, QString>, QPointF> outAt, inAt;
        for (int i = 0; i < h.nodes.size(); ++i) {
            Plugin *p = h.nodes[i];
            const QList<Port> ins = p->inputs(), outs = p->outputs();
            const double x = i * 260, w = 180;
            sc.addRect(x, 0, w, 40 + 26 * std::max(ins.size(), outs.size()), QPen(Qt::gray), h.on(p) ? QColor(0xf4f4f4) : QColor(0xd0d0d0));
            sc.addSimpleText(p->name() + (h.on(p) ? "" : " (off)"))->setPos(x + 8, 8);
            auto port = [&](const Port &pt, int k, bool in) {
                QPointF c(x + (in ? 0 : w), 48 + 26 * k);
                auto dot = sc.addEllipse(QRectF(c - QPointF(8, 8), QSizeF(16, 16)), QPen(Qt::darkGray), colorOf(pt.type));
                dot->setData(0, i), dot->setData(1, pt.name), dot->setData(2, in);
                dot->setZValue(2);
                dot->setToolTip(pt.type);
                auto t = sc.addSimpleText(pt.name);
                t->setPos(in ? c.x() + 12 : c.x() - 12 - t->boundingRect().width(), c.y() - t->boundingRect().height() / 2);
                (in ? inAt : outAt)[{p, pt.name}] = c;
            };
            for (int k = 0; k < ins.size(); ++k) port(ins[k], k, true);
            for (int k = 0; k < outs.size(); ++k) port(outs[k], k, false);
        }
        for (int k = 0; k < h.wires.size(); ++k) {
            const Host::Wire &wr = h.wires[k];
            auto l = sc.addLine(QLineF(outAt.value({wr.from, wr.out}), inAt.value({wr.to, wr.in})), QPen(QColor(0x1565c0), 6, Qt::SolidLine, Qt::RoundCap));
            l->setZValue(1);
            l->setData(3, k);
            l->setToolTip("Tap to remove");
        }
    }
    QGraphicsItem *portAt(QPoint pos) {
        for (QGraphicsItem *it : items(pos))
            if (it->data(1).isValid()) return it;
        return nullptr;
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (QGraphicsItem *it = portAt(e->pos()); it && !it->data(2).toBool()) {   // an output: draw a wire from it
            fromNode = it->data(0).toInt();
            fromPort = it->data(1).toString();
            QPointF c = it->sceneBoundingRect().center();
            drag = sc.addLine(QLineF(c, c), QPen(Qt::darkGray, 3, Qt::DashLine));
            return;
        }
        for (QGraphicsItem *it : items(e->pos()))
            if (it->data(3).isValid()) {
                h.unlink(h.wires[it->data(3).toInt()]);
                return build();
            }
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (!drag) return;
        QLineF l = drag->line();
        l.setP2(mapToScene(e->pos()));
        drag->setLine(l);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (!drag) return;
        if (QGraphicsItem *to = portAt(e->pos()); to && to->data(2).toBool())
            h.link({h.nodes[fromNode], fromPort, h.nodes[to->data(0).toInt()], to->data(1).toString()});
        build();
    }
};
}

Host::Host() : canvasNode(new CanvasNode) {
    nodes << canvasNode.get();
    off = QSettings().value("plugins/off").toStringList();
}

Host::~Host() = default;

void Host::load(const QString &dir) {
    for (const QFileInfo &f : QDir(dir).entryInfoList(QDir::Files, QDir::Name)) {
        if (!f.fileName().contains("techyplugin") || !QLibrary::isLibrary(f.fileName())) continue;
        QPluginLoader l(f.absoluteFilePath());
        if (auto p = qobject_cast<Plugin *>(l.instance())) add(p);
        else errors << f.fileName() + ": " + l.errorString();
    }
}

void Host::add(Plugin *p) { nodes << p; }

bool Host::on(Plugin *p) const { return p == canvasNode.get() || !off.contains(p->name()); }

void Host::start() {
    for (Plugin *p : nodes)
        if (on(p)) p->start(*this);
    QSettings st;
    QSet<QString> known;   // plug-ins the saved wiring knows; new ones are wired by type
    for (const QString &s : st.value("plugins/links").toStringList()) {
        QStringList f = s.split('\t');
        if (f.size() != 4) continue;
        known << f[0] << f[2];
        auto byName = [&](const QString &n) -> Plugin * {
            for (Plugin *p : nodes)
                if (p->name() == n) return p;
            return nullptr;
        };
        link({byName(f[0]), f[1], byName(f[2]), f[3]});
    }
    for (Plugin *to : nodes)
        for (const Port &in : to->inputs())
            for (bool done = known.contains(to->name()); Plugin *from : nodes)
                for (const Port &out : from->outputs())
                    if (!done && from != to && out.type == in.type) done = link({from, out.name, to, in.name});
}

void Host::send(Plugin *from, const QString &out, const QVariant &v) {
    last[{from, out}] = v;
    for (const Wire &w : QList<Wire>(wires))
        if (w.from == from && w.out == out) w.to->receive(w.in, v);
}

bool Host::link(const Wire &w) {
    if (!w.from || !w.to || !on(w.from) || !on(w.to) || wires.contains(w)) return false;
    QString type = portOf(w.from->outputs(), w.out).type;
    if (type.isEmpty() || type != portOf(w.to->inputs(), w.in).type) return false;
    wires << w;
    if (auto v = last.constFind({w.from, w.out}); v != last.constEnd()) w.to->receive(w.in, *v);
    selectionMenu();
    return true;
}

void Host::unlink(const Wire &w) {
    if (!wires.removeAll(w)) return;
    w.to->receive(w.in, {});
    selectionMenu();
}

void Host::save() const {
    QSettings st;
    auto running = [&](const QString &n) {
        return std::any_of(nodes.begin(), nodes.end(), [&](Plugin *p) { return p->name() == n && on(p); });
    };
    QStringList l;
    for (const QString &s : st.value("plugins/links").toStringList())   // wires of plug-ins off or missing now: kept for later
        if (QStringList f = s.split('\t'); f.size() == 4 && !(running(f[0]) && running(f[2]))) l << s;
    for (const Wire &w : wires) l << QStringList{w.from->name(), w.out, w.to->name(), w.in}.join('\t');
    st.setValue("plugins/links", l);
}

void Host::selectionMenu() {
    if (!canvas) return;
    canvas->selectionItems.clear();
    for (const Wire &w : wires)
        if (w.from == canvasNode.get()) {
            Port in = portOf(w.to->inputs(), w.in);
            canvas->selectionItems << Canvas::MenuItem{in.icon, in.label.isEmpty() ? w.to->name() : in.label,
                                                       [this, w] { w.to->receive(w.in, QVariant::fromValue(canvas->selected())); }};
        }
}

void Host::showGraph(QWidget *parent) {
    QDialog d(parent);
    d.setWindowTitle("Plug-ins");
    d.resize(900, 420);
    auto lay = new QVBoxLayout(&d);
    auto hint = new QLabel("Drag from an output (right side of a box) to an input (left side) of the same colour to wire them; tap a "
                           "wire to remove it. Switching a plug-in on or off takes effect at the next start.");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    lay->addWidget(new NodeView(*this), 1);
    auto row = new QHBoxLayout;
    QStringList offNext = off;
    for (Plugin *p : nodes)
        if (p != canvasNode.get()) {
            auto b = new QCheckBox(p->name());
            b->setChecked(!off.contains(p->name()));
            QObject::connect(b, &QCheckBox::toggled, [&offNext, n = p->name()](bool on) {
                offNext.removeAll(n);
                if (!on) offNext << n;
            });
            row->addWidget(b);
        }
    row->addStretch();
    lay->addLayout(row);
    if (!errors.isEmpty()) {
        auto e = new QLabel("Not loaded:\n" + errors.join('\n'));
        e->setWordWrap(true);
        lay->addWidget(e);
    }
    auto box = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(box, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    lay->addWidget(box);
    d.exec();
    save();
    QSettings().setValue("plugins/off", offNext);
}
