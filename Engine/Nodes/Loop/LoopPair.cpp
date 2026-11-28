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

#include "LoopPair.h"

#include <limits>

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/GeoForEachBegin.h"
#include "Engine/Nodes/Loop/GeoForEachEnd.h"
#include "Engine/Nodes/Loop/GeoSolverBegin.h"
#include "Engine/Nodes/Loop/GeoSolverEnd.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/UndoRecorder.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr LoopPairKind kLoopPairKinds[] = {
    LoopPairKind { "GeoForEach", PLUGINID_NATRON_GEOFOREACHBEGIN, PLUGINID_NATRON_GEOFOREACHEND },
    LoopPairKind { "GeoSolver", PLUGINID_NATRON_GEOSOLVERBEGIN, PLUGINID_NATRON_GEOSOLVEREND },
    LoopPairKind { "Loop", PLUGINID_NATRON_LOOPBEGIN, PLUGINID_NATRON_LOOPEND },
};

// The node graph's gap between a node and the one stacked under it.
constexpr double kStackedNodesGap = 30.;

NodePtr
createPairNode(const AppInstancePtr& app,
               const char* pluginID,
               bool isEnd,
               const std::function<void(CreateNodeArgs& args)>& makeArgs)
{
    CreateNodeArgs args;

    makeArgs(args);
    args.setProperty<std::string>(kCreateNodeArgsPropPluginID, pluginID);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);
    if (isEnd) {
        // The end goes where the begin decides, under its own default name.
        const double unset = std::numeric_limits<int>::min();
        args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, unset, 0);
        args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, unset, 1);
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, std::string());
    }
    NodePtr node = app->createNode(args);
    // A creation without the node graph's own undo command is recorded in the open step instead.
    if (node && !args.getProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand)) {
        recordPythonNodeCreated(node);
    }

    return node;
}

} // namespace

std::span<const LoopPairKind>
loopPairKinds()
{
    return std::span<const LoopPairKind>(kLoopPairKinds);
}

const LoopPairKind*
findLoopPairKind(const std::string& name)
{
    for (const LoopPairKind& kind : kLoopPairKinds) {
        if (name == kind.name) {
            return &kind;
        }
    }

    return nullptr;
}

LoopPairNodes
createLoopPair(const AppInstancePtr& app,
               const LoopPairKind& kind,
               const std::function<void(CreateNodeArgs& args)>& makeArgs)
{
    if (!app) {
        return LoopPairNodes();
    }
    UndoStepGuard step(app, std::string("Create ") + kind.name, true);

    NodePtr begin = createPairNode(app, kind.beginPluginID, false, makeArgs);
    if (!begin) {
        return LoopPairNodes();
    }
    NodePtr end = createPairNode(app, kind.endPluginID, true, makeArgs);
    if (!end) {
        destroyNodeFromPython(begin, false);

        return LoopPairNodes();
    }

    if (end->getInput(0) != begin) {
        {
            PythonInputUndoGuard undo(end, 0);
            end->replaceInput(begin, 0);
        }
        double x, y, beginWidth, beginHeight, endWidth, endHeight;
        begin->getPosition(&x, &y);
        begin->getSize(&beginWidth, &beginHeight);
        end->getSize(&endWidth, &endHeight);
        PythonNodeLayoutUndoGuard undo(end);
        end->setPosition(x + (beginWidth - endWidth) / 2., y + beginHeight + kStackedNodesGap);
    }

    NodesList both;
    both.push_back(begin);
    both.push_back(end);
    app->setNodeGraphSelection(both);

    return LoopPairNodes { begin, end };
}

NATRON_NAMESPACE_EXIT
