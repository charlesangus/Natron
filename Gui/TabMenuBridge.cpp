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

#include <QAction>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QPointF>
#include <QPointer>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/UndoHistory.h"

NATRON_NAMESPACE_ENTER

namespace {
QPointer<NodeGraph> g_target;
QPointF g_scenePos;
// The filtered launch's context, until the next launch.
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

void
openMenu(NodeGraph* graph,
         const std::string& pythonContext)
{
    Gui* gui = graph->getGui();
    GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();
    if (!app) {
        return;
    }

    std::string script = "import tabtabtab_natron\n";
    script += "tabtabtab_natron.launch(" + app->getAppIDString() + pythonContext + ")\n";
    std::string err;
    std::string output;
    if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output)) {
        app->appendToScriptEditor(err);
        std::cerr << "Tab menu: " << err << std::endl;
    } else if (!output.empty()) {
        app->appendToScriptEditor(output);
    }
}

// Triggers action as context says; see TabMenuBridge::launch(NodeGraph*, const LaunchContext&).
void
triggerInContext(QAction* action,
                 const TabMenuBridge::LaunchContext& context,
                 const GuiAppInstancePtr& app,
                 const NodeCollectionPtr& collection)
{
    Gui* gui = g_target->getGui();
    UndoHistory* history = gui ? gui->getUndoHistory() : nullptr;
    QString text = action->text();
    text.remove(QLatin1Char('&'));
    if (history) {
        history->openStep(QCoreApplication::translate("TabMenuBridge", "Create %1").arg(text));
    }

    NodePtr insertAfter = context.insertAfter.lock();
    NodeGuiPtr afterGui = insertAfter ? std::dynamic_pointer_cast<NodeGui>(insertAfter->getNodeGui()) : NodeGuiPtr();
    g_target->clearSelection();
    if (afterGui) {
        g_target->selectNode(afterGui, false);
    }
    const NodesList before = collection->getNodes();
    {
        NodeCreationContextScope scope(app, collection, g_scenePos);
        action->trigger();
    }
    NodePtr created;
    const NodesList after = collection->getNodes();
    for (const NodePtr& node : after) {
        if (std::find(before.begin(), before.end(), node) == before.end()) {
            created = node;
            break;
        }
    }
    if (created && context.created) {
        context.created(created);
    }

    if (history) {
        history->closeStep();
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
triggerAction(QAction* action)
{
    if (!action) {
        return;
    }
    Gui* gui = g_target ? g_target->getGui() : 0;
    GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();
    NodeCollectionPtr collection = g_target ? g_target->getGroup() : NodeCollectionPtr();
    if (!app || !collection) {
        action->trigger();

        return;
    }
    if (g_context) {
        // One node per launch: a second trigger, from a nested action, is a plain one.
        std::shared_ptr<LaunchContext> context;
        context.swap(g_context);
        triggerInContext(action, *context, app, collection);

        return;
    }

    NodeCreationContextScope scope(app, collection, g_scenePos);
    action->trigger();
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
    openMenu(graph, std::string());
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
    std::string filter = ", {'inputKind': " + pythonStringLiteral(dataKindKey(context.inputKind));
    filter += std::string(", 'needsPrimPathKnob': ") + (context.needsPrimPathKnob ? "True" : "False") + "}";
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
    }
}
} // namespace TabMenuBridge

NATRON_NAMESPACE_EXIT
