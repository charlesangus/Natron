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

#include "ViewerTabPrivate.h"

#include <cassert>
#include <stdexcept>

#include <QDebug>

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeMetadata.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/ChannelsComboBox.h"
#include "Gui/ClickableLabel.h"
#include "Gui/Gui.h"
#include "Gui/NodeGui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/ViewerTab.h"


NATRON_NAMESPACE_ENTER

ViewerTabPrivate::ViewerTabPrivate(ViewerTab* publicInterface,
                                   ViewerInstance* node)
    : publicInterface(publicInterface)
    , viewer(NULL)
    , viewerContainer(NULL)
    , viewerLayout(NULL)
    , viewerSubContainer(NULL)
    , viewerSubContainerLayout(NULL)
    , sceneViewport(NULL)
    , sceneBreadcrumbBar(NULL)
    , mainLayout(NULL)
    , firstSettingsRow(NULL)
    , secondSettingsRow(NULL)
    , firstRowLayout(NULL)
    , secondRowLayout(NULL)
    , sceneRenderRow(NULL)
    , sceneRenderRowLayout(NULL)
    , maxSamplesBox(NULL)
    , refinementPausedButton(NULL)
    , renderEverythingButton(NULL)
    , refineSamplesLabel(NULL)
    , geoDisplayRow(NULL)
    , layerChoice(NULL)
    , alphaChannelChoice(NULL)
    , layerMenuTime(0.)
    , layerMenuView(0)
    , viewerChannels(NULL)
    , viewerChannelsAutoswitchedToAlpha(false)
    , zoomCombobox(NULL)
    , syncViewerButton(NULL)
    , centerViewerButton(NULL)
    , clipToProjectFormatButton(NULL)
    , fullFrameProcessingButton(NULL)
    , enableViewerRoI(NULL)
    , refreshButton(NULL)
    , pauseButton(NULL)
    , iconRefreshOff()
    , iconRefreshOn()
    , activateRenderScale(NULL)
    , renderScaleCombo(NULL)
    , autoProxyButton(NULL)
    , firstInputLabel(NULL)
    , firstInputImage(NULL)
    , compositingOperatorLabel(NULL)
    , compositingOperator(NULL)
    , secondInputLabel(NULL)
    , secondInputImage(NULL)
    , toggleGainButton(NULL)
    , gainBox(NULL)
    , gainSlider(NULL)
    , applyCameraExposureButton(NULL)
    , cameraExposureLabel(NULL)
    , gammaBox(NULL)
    , toggleGammaButton(NULL)
    , gammaSlider(NULL)
    , viewerDisplay(NULL)
    , viewerView(NULL)
    , viewerLook(NULL)
    , populatingDisplayTransform(false)
    , colorManagement()
    , configChangedCallbackId(-1)
    , checkerboardButton(NULL)
    , mapSiblingOverlaysButton(NULL)
    , pickerButton(NULL)
    , viewsComboBox(NULL)
    , infoWidget()
    , playerButtonsContainer(0)
    , playerLayout(NULL)
    , currentFrameBox(NULL)
    , firstFrame_Button(NULL)
    , previousKeyFrame_Button(NULL)
    , play_Backward_Button(NULL)
    , previousFrame_Button(NULL)
    , nextFrame_Button(NULL)
    , play_Forward_Button(NULL)
    , nextKeyFrame_Button(NULL)
    , lastFrame_Button(NULL)
    , previousIncrement_Button(NULL)
    , incrementSpinBox(NULL)
    , nextIncrement_Button(NULL)
    , playbackMode_Button(NULL)
    , playBackInputButton(NULL)
    , playBackInputSpinbox(NULL)
    , playBackOutputButton(NULL)
    , playBackOutputSpinbox(NULL)
    , playbackModeMutex()
    , playbackMode(ePlaybackModeLoop)
    , tripleSyncButton(0)
    , canEditFpsBox(NULL)
    , canEditFpsLabel(NULL)
    , fpsBox(NULL)
    , turboButton(NULL)
    , timeLineGui(NULL)
    , timelineBoundsGesture()
    , nodesContext()
    , currentNodeContext()
    , inputNamesMap()
    , compOperatorPrevious(eViewerCompositingOperatorWipeUnder)
    , viewerNode(node)
    , visibleToolbarsMutex()
    , infobarVisible(true)
    , playerVisible(true)
    , timelineVisible(true)
    , leftToolbarVisible(true)
    , rightToolbarVisible(true)
    , topToolbarVisible(true)
    , isFileDialogViewer(false)
    , fpsMutex()
    , fps(24.)
    , hasPenDown(false)
    , hasCaughtPenMotionWhileDragging(false)
{
    infoWidget[0] = infoWidget[1] = NULL;
}

OverlayPathResult
ViewerTabPrivate::resolveOverlay(double time,
                                 ViewIdx view,
                                 const NodePtr& node) const
{
    return overlayPathCache.resolve(viewerNode, node, time, view, viewerValue<bool>(kViewerParamMapSiblingOverlays, true));
}

void
ViewerTabPrivate::getComponentsAvailabel(double time,
                                         ViewIdx view,
                                         std::set<ImageLayerDesc>* comps) const
{
    int activeInputIdx[2];

    viewerNode->getActiveInputs(activeInputIdx[0], activeInputIdx[1]);
    EffectInstancePtr activeInput[2] = { EffectInstancePtr(), EffectInstancePtr() };
    bool anyInput = false;
    bool hasColor = false;
    for (int i = 0; i < 2; ++i) {
        activeInput[i] = viewerNode->getInput(activeInputIdx[i]);
        if (activeInput[i]) {
            anyInput = true;
            std::list<ImageLayerDesc> compsAvailable;
            activeInput[i]->getPresentLayers(time, view, -1, &compsAvailable);
            for (std::list<ImageLayerDesc>::iterator it = compsAvailable.begin(); it != compsAvailable.end(); ++it) {
                hasColor = hasColor || it->isColorLayer();
                comps->insert(*it);
            }
        }
    }

    // A stream whose colour plane was removed upstream still lists the colour views, which read
    // zero there, as every other layer menu does.
    if (anyInput && !hasColor) {
        comps->insert(ImageLayerDesc::getRGBAComponents());
    }
}

void
ViewerTabPrivate::getLayerViewsAvailabelAtTime(double time,
                                               ViewIdx view,
                                               std::list<ImageLayerDesc>* layerViews,
                                               std::list<ImageLayerDesc>* channelViews,
                                               ImageLayerDesc* storageColorLayer) const
{
    std::set<ImageLayerDesc> storage;
    getComponentsAvailabel(time, view, &storage);

    if (storageColorLayer) {
        *storageColorLayer = ImageLayerDesc::getNoneComponents();
        for (std::set<ImageLayerDesc>::const_iterator it = storage.begin(); it != storage.end(); ++it) {
            if (it->isColorLayer()) {
                *storageColorLayer = *it;
                break;
            }
        }
    }

    if (layerViews) {
        layerViews->assign(storage.begin(), storage.end());
        ImageLayerDesc::expandColorViews(layerViews);
    }
    if (channelViews) {
        channelViews->assign(storage.begin(), storage.end());
        ImageLayerDesc::collapseColorToLayoutView(channelViews);
    }
}

static bool
isCurveHost(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();

    return effect && (effect->getCurveContextKind() != EffectInstance::eCurveContextKindNone);
}

std::string
ViewerTabPrivate::getViewerSlot(const NodePtr& node)
{
    // Curve hosts (Roto, RotoPaint, and any node editing curves) are different plug-ins but we
    // don't want two of them at the same time in the viewer. The key can't clash with a plug-in ID.
    if (isCurveHost(node)) {
        return std::string("<curve host>");
    }

    return node->getPluginID();
}

std::list<ViewerTabPrivate::PluginViewerContext>::iterator
ViewerTabPrivate::findActiveNodeContextForSlot(const std::string& slot)
{
    for (std::list<PluginViewerContext>::iterator it = currentNodeContext.begin(); it != currentNodeContext.end(); ++it) {
        if (it->slot == slot) {
            return it;
        }
    }

    return currentNodeContext.end();
}

bool
ViewerTabPrivate::hasInactiveNodeViewerContext(const NodePtr& node)
{
    std::list<ViewerTabPrivate::PluginViewerContext>::iterator found = findActiveNodeContextForSlot(getViewerSlot(node));

    if ( found == currentNodeContext.end() ) {
        return false;
    }
    NodeGuiPtr n = found->currentNode.lock();
    if (!n) {
        return false;
    }

    return n->getNode() != node;
}

bool
ViewerTabPrivate::isOverlayHiddenByViewerSlot(const NodePtr& node)
{
    return !isCurveHost(node) && hasInactiveNodeViewerContext(node);
}

NATRON_NAMESPACE_EXIT
