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

#include "RawDevelop.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QTimer>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGraphI.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/IO/RawExtensions.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

namespace RawDevelop {
namespace {
    // Node graph units between two nodes of the chain, and around them inside the backdrop.
    const double kNodeGap = 30.;
    const double kBackdropMargin = 30.;
    const double kBackdropHeader = 40.;

    NodePtr
    createChainNode(const AppInstancePtr& app,
                    const NodeCollectionPtr& group,
                    const char* pluginID,
                    double x,
                    double y)
    {
        CreateNodeArgs args(pluginID, group);

        args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
        args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, x, 0);
        args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, y, 1);

        return app->createNode(args);
    }

    std::string
    readFilename(const NodePtr& read)
    {
        KnobIPtr knob = read->getKnobByName(kOfxImageEffectFileParamName);
        KnobStringBase* file = dynamic_cast<KnobStringBase*>(knob.get());

        return file ? file->getValue() : std::string();
    }
} // namespace

bool
Chain::complete() const
{
    return prepare && temperature && highlights && demosaic && inputColor && colorCalibration && backdrop;
}

ChainLayout
layoutChain(double readX,
            double readY,
            double readWidth,
            double readHeight,
            const std::vector<NodeSize>& sizes)
{
    ChainLayout layout;
    double y = readY + readHeight + kNodeGap + kBackdropHeader;
    double maxWidth = readWidth;

    for (const NodeSize& size : sizes) {
        // A node shorter or narrower than the Read is centred under it.
        layout.nodes.push_back({ readX + (readWidth - size.width) / 2., y, size.width, size.height });
        maxWidth = std::max(maxWidth, size.width);
        y += size.height + kNodeGap;
    }
    const double top = readY + readHeight + kNodeGap;
    layout.backdrop.width = maxWidth + 2. * kBackdropMargin;
    layout.backdrop.height = (y - kNodeGap + kBackdropMargin) - top;
    layout.backdrop.x = readX + readWidth / 2. - layout.backdrop.width / 2.;
    layout.backdrop.y = top;

    return layout;
}

Chain
buildChain(const NodePtr& read)
{
    Chain chain;

    if (!read) {
        return chain;
    }
    AppInstancePtr app = read->getApp();
    NodeCollectionPtr group = read->getGroup();
    if (!app || !group) {
        return chain;
    }

    double readX = 0., readY = 0., readW = 0., readH = 0.;
    read->getPosition(&readX, &readY);
    read->getSize(&readW, &readH);

    const char* const ids[] = { PLUGINID_NATRON_RAWPREPARE, PLUGINID_NATRON_RAWTEMPERATURE, PLUGINID_NATRON_RAWHIGHLIGHTS, PLUGINID_NATRON_RAWDEMOSAIC, PLUGINID_NATRON_RAWINPUTCOLOR, PLUGINID_NATRON_RAWCOLORCALIBRATION };
    NodePtr* const slots[] = { &chain.prepare, &chain.temperature, &chain.highlights, &chain.demosaic, &chain.inputColor, &chain.colorCalibration };
    static_assert(sizeof(ids) / sizeof(ids[0]) == sizeof(slots) / sizeof(slots[0]), "one slot per node");
    const int count = (int)(sizeof(ids) / sizeof(ids[0]));
    // Each node's size is known once it exists, so the chain is created, then laid out.
    std::vector<NodeSize> sizes;
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        NodePtr node = createChainNode(app, group, ids[i], readX, readY + readH + kNodeGap + kBackdropHeader);
        if (!node) {
            ok = false;
            break;
        }
        *slots[i] = node;
        NodeSize size;
        node->getSize(&size.width, &size.height);
        sizes.push_back(size);
    }

    if (ok) {
        const ChainLayout layout = layoutChain(readX, readY, readW, readH, sizes);
        NodePtr previous = read;
        for (int i = 0; (i < count) && ok; ++i) {
            NodePtr node = *slots[i];
            node->setPosition(layout.nodes[(std::size_t)i].x, layout.nodes[(std::size_t)i].y);
            ok = app->getProject()->connectNodes(0, previous, node);
            previous = node;
        }
        if (ok) {
            chain.backdrop = createChainNode(app, group, PLUGINID_NATRON_BACKDROP, layout.backdrop.x, layout.backdrop.y);
            if (chain.backdrop) {
                chain.backdrop->setLabel(kRawDevelopBackdropTitle);
                chain.backdrop->setPosition(layout.backdrop.x, layout.backdrop.y);
                chain.backdrop->setSize(layout.backdrop.width, layout.backdrop.height);
            }
        }
    }

    if (!chain.complete()) {
        for (NodePtr* slot : { &chain.prepare, &chain.temperature, &chain.highlights, &chain.demosaic, &chain.inputColor, &chain.colorCalibration, &chain.backdrop }) {
            if (*slot) {
                (*slot)->destroyNode(false, false);
            }
        }

        return Chain();
    }
    if (NodeGraphI* graph = group->getNodeGraph()) {
        graph->addNodesToCreationOf(read, { chain.prepare, chain.temperature, chain.highlights, chain.demosaic, chain.inputColor, chain.colorCalibration, chain.backdrop });
    }

    return chain;
} // buildChain

bool
isInteractiveArgs(const CreateNodeArgs& args)
{
    return !args.getProperty<bool>(kCreateNodeArgsPropSilent) && !args.getProperty<bool>(kCreateNodeArgsPropOutOfProject) && !args.getProperty<bool>(kCreateNodeArgsPropNoNodeGUI) && !args.getProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization);
}

bool
isInteractiveCreation(const AppInstancePtr& app,
                      const CreateNodeArgs& args)
{
    if (!app || app->isBackground() || app->isCreatingPythonGroup() || app->isCreatingNodeTree()) {
        return false;
    }
    ProjectPtr project = app->getProject();
    if (!project || project->isLoadingProject()) {
        return false;
    }

    return isInteractiveArgs(args);
}

void
onReadCreated(const NodePtr& read,
              const CreateNodeArgs& args)
{
    if (!read) {
        return;
    }
    AppInstancePtr app = read->getApp();
    if (!isInteractiveCreation(app, args) || !appPTR->getCurrentSettings()->isDevelopRawOnReadEnabled() || !RawExtensions::isRawPath(readFilename(read))) {
        return;
    }
    // The interface places a new node after it is created, and the chain goes under that place.
    const NodeWPtr weak = read;
    QTimer::singleShot(0, QCoreApplication::instance(), [weak]() {
        NodePtr node = weak.lock();
        if (node && node->isActivated()) {
            buildChain(node);
        }
    });
}
} // namespace RawDevelop

NATRON_NAMESPACE_EXIT
