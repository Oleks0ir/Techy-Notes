#pragma once
#include "Canvas.h"
#include <QHash>
#include <QStringList>
#include <QVariant>
#include <QtPlugin>
#include <memory>

class QMainWindow;
class QMenu;
class QToolBar;
struct Host;

// A plug-in's input or output. A wire joins an output to an input of the same type ("ink", "math-editor").
// label, icon: an input wired to the canvas's selection is an entry of the selection's menu and ring.
struct Port { QString name, type, label; QChar icon = {}; };

// A plug-in: a library in the plugins folder (a Qt plugin, IID kPluginIid). start() adds its GUI: docks, menus,
// toolbar actions (Host), Canvas::ringItems / objectMenus, objectKinds. Other plug-ins reach it only through ports.
// ponytail: a C++ interface, so plug-ins are built with the app's compiler and Qt; a C ABI if outside ones appear
struct Plugin {
    virtual ~Plugin() = default;
    virtual QString name() const = 0;
    virtual QList<Port> inputs() const { return {}; }
    virtual QList<Port> outputs() const { return {}; }
    virtual void start(Host &host) = 0;
    virtual void receive(const QString & /*input*/, const QVariant & /*v*/) {}   // from a wired output; invalid v: unwired
};
#define kPluginIid "io.github.oleks0ir.techynotes.Plugin/1"
Q_DECLARE_INTERFACE(Plugin, kPluginIid)

// The app as plug-ins see it, and the wiring between them (Settings > Plug-ins shows and changes it).
struct Host {
    QMainWindow *window = nullptr;
    Canvas *canvas = nullptr;
    QMenu *settings = nullptr;
    QToolBar *tools = nullptr;
    QString rootDir;   // the notebook root: plug-ins keep notebook-wide files there
    struct Wire {
        Plugin *from; QString out; Plugin *to; QString in;
        bool operator==(const Wire &) const = default;
    };
    QList<Plugin *> nodes;   // nodes[0]: the canvas, output "selection" (type "ink")
    QList<Wire> wires;
    QStringList errors;      // plug-in files that did not load, and why

    Host();
    ~Host();
    void load(const QString &dir);   // the plug-ins there (file names with "techyplugin"), in name order
    void add(Plugin *p);
    // Starts the plug-ins not switched off, then wires them: as saved, else each input to the first output of its type
    void start();
    bool on(Plugin *p) const;
    void send(Plugin *from, const QString &out, const QVariant &v);   // to its wired inputs; kept for wires made later
    bool link(const Wire &w);   // false: no such ports, other types, a node that is off, or wired already
    void unlink(const Wire &w);
    void save() const;          // the wiring and the switched-off plug-ins, for the next start
    void showGraph(QWidget *parent);   // the node view

private:
    std::unique_ptr<Plugin> canvasNode;
    QStringList off;   // names
    QHash<std::pair<Plugin *, QString>, QVariant> last;   // each output's last value
    void selectionMenu();
};
