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

#ifndef Tests_Scene_ShaderManifestTestUtils_h
#define Tests_Scene_ShaderManifestTestUtils_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Network/NetworkNode.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

// The manifest of renderer ("cycles", "moonray") as the build embeds it.
inline QJsonObject
shaderManifest(const std::string& renderer)
{
    QFile file(QString::fromUtf8(":/Shaders/") + QString::fromUtf8(renderer.c_str()) + QString::fromUtf8(".json"));

    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }

    return QJsonDocument::fromJson(file.readAll()).object();
}

inline QJsonObject
shaderLayoutFile(const std::string& renderer)
{
    QFile file(QString::fromUtf8(":/Shaders/") + QString::fromUtf8(renderer.c_str()) + QString::fromUtf8("_layout.json"));

    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }

    return QJsonDocument::fromJson(file.readAll()).object();
}

inline QJsonObject
shaderManifestNode(const std::string& renderer,
                   const std::string& id)
{
    for (const QJsonValue& node : shaderManifest(renderer).value(QLatin1String("nodes")).toArray()) {
        if (node.toObject().value(QLatin1String("id")).toString().toStdString() == id) {
            return node.toObject();
        }
    }

    return QJsonObject();
}

inline std::vector<std::string>
shaderManifestIds(const std::string& renderer)
{
    std::vector<std::string> ret;

    for (const QJsonValue& node : shaderManifest(renderer).value(QLatin1String("nodes")).toArray()) {
        ret.push_back(node.toObject().value(QLatin1String("id")).toString().toStdString());
    }

    return ret;
}

// Creates a shader node as a script does, by ID: shader plug-ins are kept out of the menus.
inline NodePtr
createShaderNode(const AppInstancePtr& app,
                 const std::string& pluginId,
                 const NodeCollectionPtr& collection)
{
    CreateNodeArgs args(pluginId, collection);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);

    return app->createNode(args);
}

inline NetworkNode*
shaderEffect(const NodePtr& node)
{
    return node ? dynamic_cast<NetworkNode*>(node->getEffectInstance().get()) : nullptr;
}

NATRON_NAMESPACE_EXIT

#endif // Tests_Scene_ShaderManifestTestUtils_h
