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
#include <list>
#include <string>

#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QPointF>
#include <QPointer>

#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Node.h"
#include "Engine/Settings.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGraph.h"

NATRON_NAMESPACE_ENTER

namespace {
QPointer<NodeGraph> g_target;
QPointF g_scenePos;

QString
createShortcutText(const QString& shortcutGroup,
                   const QString& pluginID)
{
    std::list<QKeySequence> sequences = getKeybind(shortcutGroup, pluginID);

    if (sequences.empty()) {
        return QString();
    }

    return sequences.front().toString(QKeySequence::NativeText);
}

QString
categoryColorName(const Plugin& plugin)
{
    float r, g, b;

    appPTR->getCurrentSettings()->getNodeCategoryColor(Node::categoryForPlugin(plugin), &r, &g, &b);

    return QColor::fromRgbF(std::clamp(r, 0.f, 1.f),
                            std::clamp(g, 0.f, 1.f),
                            std::clamp(b, 0.f, 1.f))
        .name();
}

QJsonObject
makeNodeItem(const Plugin& plugin,
             const QString& label,
             const TabMenuBridge::ShortcutLookup& shortcutFor)
{
    const QStringList& grouping = plugin.getGrouping();
    QString shortcutGroup = QString::fromUtf8(kShortcutGroupNodes);
    QJsonArray groupingArray;

    for (const QString& g : grouping) {
        groupingArray.append(g);
        shortcutGroup += QLatin1Char('/') + g;
    }

    const QString& iconPath = plugin.getIconFilePath();
    QJsonObject item;
    item[QString::fromUtf8("id")] = plugin.getPluginID();
    item[QString::fromUtf8("major")] = plugin.getMajorVersion();
    item[QString::fromUtf8("label")] = label;
    item[QString::fromUtf8("grouping")] = groupingArray;
    item[QString::fromUtf8("icon")] = (!iconPath.isEmpty() && QFile::exists(iconPath)) ? iconPath : QString();
    item[QString::fromUtf8("color")] = categoryColorName(plugin);
    item[QString::fromUtf8("shortcut")] = shortcutFor ? shortcutFor(shortcutGroup, plugin.getPluginID()) : QString();

    return item;
}

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
} // anonymous namespace

namespace TabMenuBridge {
QString
nodeItemsJson()
{
    return nodeItemsJson(appPTR->getPluginsList(), createShortcutText);
}

QString
nodeItemsJson(const PluginsMap& plugins,
              const ShortcutLookup& shortcutFor)
{
    QJsonArray items;

    for (PluginsMap::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        for (PluginVersionsOrdered::const_reverse_iterator it2 = it->second.rbegin(); it2 != it->second.rend(); ++it2) {
            const Plugin* plugin = *it2;
            if (!plugin || !plugin->getIsUserCreatable()) {
                continue;
            }
            // Only the family's newest version, user-creatable or not, keeps the bare label, so an
            // older version never shows up looking like the current one.
            const QString label = (it2 == it->second.rbegin()) ? plugin->getLabelWithoutSuffix() : plugin->getLabelVersionMajorEncoded();
            items.append(makeNodeItem(*plugin, label, shortcutFor));
        }
    }

    return QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact));
}

bool
createNode(const QString& pluginID,
           int majorVersion)
{
    if (!g_target) {
        return false;
    }
    Gui* gui = g_target->getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }

    const PluginsMap& allPlugins = appPTR->getPluginsList();
    if (allPlugins.find(pluginID.toStdString()) == allPlugins.end()) {
        return false;
    }

    CreateNodeArgs args(pluginID.toStdString(), g_target->getGroup());
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, g_scenePos.x(), 0);
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, g_scenePos.y(), 1);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, majorVersion, 0);

    return (bool)app->createNode(args);
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

    std::string script = "import tabtabtab_natron\n";
    script += "tabtabtab_natron.launch(" + app->getAppIDString() + ")\n";
    std::string err;
    std::string output;
    if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output)) {
        app->appendToScriptEditor(err);
        std::cerr << "Tab menu: " << err << std::endl;
    } else if (!output.empty()) {
        app->appendToScriptEditor(output);
    }
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
