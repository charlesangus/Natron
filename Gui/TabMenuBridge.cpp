/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "TabMenuBridge.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>

#include <QAction>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QPointF>
#include <QPointer>

#include "Global/PythonUtils.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Settings.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/UndoHistory.h"

NATRON_NAMESPACE_ENTER

namespace {
QPointer<NodeGraph> g_target;
QPointF g_scenePos;
// The filtered launch's context, until the menu's choice is triggered, the menu closes without
// one, or the next launch.
std::shared_ptr<TabMenuBridge::LaunchContext> g_context;

// Puts back the context that was set before, however the action returns, so that an action
// triggered from inside another one hands the outer action its context back; an app the action
// closed is left alone.
class NodeCreationContextScope {
public:
    NodeCreationContextScope(const AppInstancePtr& app,
                             const NodeCollectionPtr& collection,
                             const QPointF& scenePos)
        : _app(app)
        , _previous(app->getNodeCreationContext())
    {
        app->setNodeCreationContext(collection, scenePos.x(), scenePos.y());
    }

    ~NodeCreationContextScope()
    {
        if (AppInstancePtr app = _app.lock()) {
            app->restoreNodeCreationContext(_previous);
        }
    }

    NodeCreationContextScope(const NodeCreationContextScope&) = delete;
    NodeCreationContextScope& operator=(const NodeCreationContextScope&) = delete;

private:
    AppInstanceWPtr _app;
    AppInstance::NodeCreationContext _previous;
};

std::string
pythonStringLiteral(const QString& s)
{
    std::string escaped;

    for (char c : s.toStdString()) {
        if ((c == '\\') || (c == '\'')) {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }

    return "'" + escaped + "'";
}

// The adapter module bootstrap() imported, held for the life of the process.
PyObject* g_adapter = nullptr;

// Reads and empties one of the stream catchers the GUI session puts on sys.stdout and sys.stderr.
std::string
takeCaught(const char* catcher)
{
    std::string text;
    PyObject* stream = PyObject_GetAttrString(NATRON_PYTHON_NAMESPACE::getMainModule(), catcher);

    if (!stream) {
        PyErr_Clear();

        return text;
    }
    PyObject* value = PyObject_GetAttrString(stream, "value");
    if (value) {
        text = NATRON_PYTHON_NAMESPACE::PyStringToStdString(value);
        Py_DECREF(value);
        PyObject* empty = PyUnicode_FromString("");
        PyObject_SetAttrString(stream, "value", empty);
        Py_DECREF(empty);
    } else {
        PyErr_Clear();
    }
    Py_DECREF(stream);

    return text;
}

// Hands what a call into the adapter printed, and its exception if it raised one, to the Script
// Editor of app when there is one, and the exception to stderr. The GIL must be held.
void
reportAdapterCall(const char* function,
                  bool failed,
                  const GuiAppInstancePtr& app)
{
    if (failed) {
        PyErr_Print();
    }
    const std::string err = takeCaught("catchErr");
    const std::string output = takeCaught("catchOut");
    if (app && !output.empty()) {
        app->appendToScriptEditor(output);
    }
    if (failed || !err.empty()) {
        const std::string message = std::string("tabtabtab_natron.") + function + "() failed:\n" + err;
        if (app) {
            app->appendToScriptEditor(message);
        }
        std::cerr << "Tab menu: " << message << std::endl;
    }
}

// Calls the adapter's function with args, a new reference or null for none, which it releases. The
// function is looked up per call so that a script may replace it. The GIL must be held.
void
callAdapter(const char* function,
            PyObject* args,
            const GuiAppInstancePtr& app)
{
    if (!g_adapter) {
        Py_XDECREF(args);

        return;
    }
    PyObject* callable = PyObject_GetAttrString(g_adapter, function);
    PyObject* result = callable ? PyObject_CallObject(callable, args) : nullptr;
    Py_XDECREF(callable);
    Py_XDECREF(args);
    Py_XDECREF(result);
    reportAdapterCall(function, !result, app);
}

// The menu's filter, as the adapter's launch() takes it; see tabtabtab_natron.launch().
struct MenuFilter {
    // "selectedKind" or "inputKind"; null for no filter.
    const char* kindKey = nullptr;
    DataKindEnum kind = eDataKindImage;
    // Whether the filter says needsPrimPathKnob, true or false.
    bool withPrimPathKnob = false;
    bool needsPrimPathKnob = false;
};

void
openMenu(NodeGraph* graph,
         const MenuFilter& filter)
{
    Gui* gui = graph->getGui();
    GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();
    if (!app) {
        return;
    }

    PythonGILLocker pgl;
    PyObject* appObject = PyObject_GetAttrString(NATRON_PYTHON_NAMESPACE::getMainModule(), app->getAppIDString().c_str());
    if (!appObject) {
        reportAdapterCall("launch", true, app);

        return;
    }
    PyObject* args;
    if (filter.kindKey) {
        PyObject* context = PyDict_New();
        PyObject* kind = PyUnicode_FromString(TabMenuBridge::dataKindKey(filter.kind).toStdString().c_str());
        PyDict_SetItemString(context, filter.kindKey, kind);
        Py_DECREF(kind);
        if (filter.withPrimPathKnob) {
            PyDict_SetItemString(context, "needsPrimPathKnob", filter.needsPrimPathKnob ? Py_True : Py_False);
        }
        args = PyTuple_Pack(2, appObject, context);
        Py_DECREF(context);
    } else {
        args = PyTuple_Pack(1, appObject);
    }
    Py_DECREF(appObject);
    callAdapter("launch", args, app);
}

// The kind every selected node outputs, false when the selection is empty, mixed, or holds a node
// whose output kind is polymorphic or not yet resolved.
bool
selectedOutputKind(NodeGraph* graph,
                   DataKindEnum* kind)
{
    bool found = false;

    for (const NodeGuiPtr& nodeGui : graph->getSelectedNodes()) {
        NodePtr node = nodeGui ? nodeGui->getNode() : NodePtr();
        if (!node) {
            continue;
        }
        bool ambiguous = false;
        const DataKindEnum nodeKind = node->getEffectiveOutputDataKind(&ambiguous);
        if (ambiguous || (nodeKind == eDataKindPolymorphic) || (found && (nodeKind != *kind))) {
            return false;
        }
        *kind = nodeKind;
        found = true;
    }

    return found;
}

// Puts back the nodes menu's previous args decorator however the action returns.
class NodesMenuArgsDecoratorScope {
public:
    explicit NodesMenuArgsDecoratorScope(const GuiApplicationManager::NodesMenuArgsDecorator& decorator)
        : _previous(appPTR->swapNodesMenuArgsDecorator(decorator))
    {
    }

    ~NodesMenuArgsDecoratorScope()
    {
        appPTR->swapNodesMenuArgsDecorator(_previous);
    }

    NodesMenuArgsDecoratorScope(const NodesMenuArgsDecoratorScope&) = delete;
    NodesMenuArgsDecoratorScope& operator=(const NodesMenuArgsDecoratorScope&) = delete;

private:
    GuiApplicationManager::NodesMenuArgsDecorator _previous;
};

// Closes the undo step it opened however the action returns.
class UndoStepScope {
public:
    UndoStepScope(UndoHistory* history,
                  const QString& name)
        : _history(history)
    {
        if (_history) {
            _history->openStep(name);
        }
    }

    ~UndoStepScope()
    {
        if (_history) {
            _history->closeStep();
        }
    }

    UndoStepScope(const UndoStepScope&) = delete;
    UndoStepScope& operator=(const UndoStepScope&) = delete;

private:
    UndoHistory* _history;
};

// The first node of after that is not in before, where after is a later copy of the same
// collection's list. A collection only appends, so the new nodes follow before's last one unless
// the action removed it.
NodePtr
firstAddedNode(const NodesList& before,
               const NodesList& after)
{
    if (before.empty()) {
        return after.empty() ? NodePtr() : after.front();
    }
    const NodePtr& last = before.back();
    for (NodesList::const_reverse_iterator it = after.rbegin(); it != after.rend(); ++it) {
        if (*it == last) {
            NodesList::const_iterator next = it.base();

            return (next == after.end()) ? NodePtr() : *next;
        }
    }
    std::unordered_set<const Node*> existing;
    existing.reserve(before.size());
    for (const NodePtr& node : before) {
        existing.insert(node.get());
    }
    for (const NodePtr& node : after) {
        if (existing.find(node.get()) == existing.end()) {
            return node;
        }
    }

    return NodePtr();
}

// Triggers action on graph as context says; see TabMenuBridge::launch(NodeGraph*, const LaunchContext&).
// Without a graph, or once its app or group is gone, the action is triggered as is.
void
triggerInContext(QAction* action,
                 const TabMenuBridge::LaunchContext& context,
                 NodeGraph* graph,
                 const QPointF& scenePos)
{
    Gui* gui = graph ? graph->getGui() : nullptr;
    GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();
    NodeCollectionPtr collection = graph ? graph->getGroup() : NodeCollectionPtr();
    if (!app || !collection) {
        action->trigger();

        return;
    }

    QString text = action->text();
    text.remove(QLatin1Char('&'));
    UndoStepScope step(gui->getUndoHistory(), QCoreApplication::translate("TabMenuBridge", "Create %1").arg(text));

    graph->clearSelection();
    const NodesList before = collection->getNodes();
    {
        NodeCreationContextScope scope(app, collection, scenePos);
        NodesMenuArgsDecoratorScope decorator(context.decorateArgs);
        action->trigger();
    }
    const NodePtr created = firstAddedNode(before, collection->getNodes());
    if (created && context.created) {
        context.created(created);
    }
}
} // anonymous namespace

namespace TabMenuBridge {
QString
dataKindKey(DataKindEnum kind)
{
    switch (kind) {
    case eDataKindImage:
        return QString::fromUtf8("image");
    case eDataKindDeep:
        return QString::fromUtf8("deep");
    case eDataKindScene:
        return QString::fromUtf8("scene");
    case eDataKindPolymorphic:
        return QString::fromUtf8("polymorphic");
    case eDataKindRaw:
        return QString::fromUtf8("raw");
    }

    return QString();
}

void
triggerActionInGraph(QAction* action,
                     NodeGraph* graph,
                     const QPointF& scenePos)
{
    if (!action) {
        return;
    }
    Gui* gui = graph ? graph->getGui() : 0;
    GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();
    NodeCollectionPtr collection = graph ? graph->getGroup() : NodeCollectionPtr();
    if (!app || !collection) {
        action->trigger();

        return;
    }
    NodeCreationContextScope scope(app, collection, scenePos);
    action->trigger();
}

void
triggerAction(QAction* action)
{
    // One choice per launch, and a nested trigger from inside the action is a plain one.
    std::shared_ptr<LaunchContext> context;
    context.swap(g_context);
    if (!action) {
        return;
    }
    if (context) {
        triggerInContext(action, *context, g_target.data(), g_scenePos);

        return;
    }
    triggerActionInGraph(action, g_target.data(), g_scenePos);
}

void
launch(NodeGraph* graph)
{
    if (!graph) {
        return;
    }
    Gui* gui = graph->getGui();
    if (!gui) {
        return;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return;
    }

    g_target = graph;
    g_scenePos = graph->mapToScene(graph->mapFromGlobal(QCursor::pos()));
    g_context.reset();
    MenuFilter filter;
    if (selectedOutputKind(graph, &filter.kind)) {
        filter.kindKey = "selectedKind";
    }
    openMenu(graph, filter);
}

void
launch(NodeGraph* graph,
       const LaunchContext& context)
{
    if (!graph || !graph->getGui() || !graph->getGui()->getApp()) {
        return;
    }

    g_target = graph;
    g_scenePos = graph->mapToScene(graph->mapFromGlobal(QCursor::pos()));
    g_context = std::make_shared<LaunchContext>(context);
    MenuFilter filter;
    filter.kindKey = "inputKind";
    filter.kind = context.inputKind;
    filter.withPrimPathKnob = true;
    filter.needsPrimPathKnob = context.needsPrimPathKnob;
    openMenu(graph, filter);
}

void
bootstrap()
{
    const QString wanted = QCoreApplication::applicationDirPath() + QString::fromUtf8("/../Resources/tabtabtab");
    const QString dir = QDir(wanted).canonicalPath();

    if (dir.isEmpty()) {
        std::cerr << "Tab menu: the tabtabtab folder is missing: " << QDir::cleanPath(wanted).toStdString() << std::endl;

        return;
    }

    const std::string path = pythonStringLiteral(dir);
    std::string script = "import sys\n";
    script += "if " + path + " not in sys.path:\n";
    script += "    sys.path.insert(0, " + path + ")\n";
    script += "import tabtabtab_natron\n";
    std::string err;
    if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0)) {
        std::cerr << "Tab menu: could not import tabtabtab_natron from " << dir.toStdString() << ":\n"
                  << err << std::endl;

        return;
    }
    {
        PythonGILLocker pgl;
        if (g_adapter) {
            return;
        }
        g_adapter = PyImport_ImportModule("tabtabtab_natron");
        if (!g_adapter) {
            reportAdapterCall("import", true, GuiAppInstancePtr());

            return;
        }
        callAdapter("schedule_warm_up", nullptr, GuiAppInstancePtr());
    }

    QObject::connect(appPTR->getCurrentSettings().get(), &Settings::settingChanged, appPTR, [](KnobI* knob) {
        if (!knob) {
            return;
        }
        const std::string& name = knob->getName();
        if ((name == "tabMenuSearchModes") || (name == "tabMenuScroll")) {
            PythonGILLocker pgl;
            callAdapter("invalidate_preferences", nullptr, GuiAppInstancePtr());
        }
    });
    QObject::connect(appPTR, &GuiApplicationManager::nodesMenuRebuilt, appPTR, []() {
        invalidateItems();
    });
}

void
invalidateItems()
{
    if (!g_adapter) {
        return;
    }
    PythonGILLocker pgl;
    callAdapter("invalidate_items", nullptr, GuiAppInstancePtr());
}
} // namespace TabMenuBridge

NATRON_NAMESPACE_EXIT
