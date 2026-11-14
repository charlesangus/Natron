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

#include "RotoPaint.h"

#include <sstream> // stringstream
#include <cassert>
#include <stdexcept>

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/MergingEnum.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeMetadata.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoObjectSelect.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

#include "Global/GLIncludes.h"
#include "Global/GlobalDefines.h"


#define ROTOPAINT_MASK_INPUT_INDEX 10

NATRON_NAMESPACE_ENTER

struct RotoPaintPrivate {
    bool isPaintByDefault;
    RotoPaintInteractPtr ui;

    RotoPaintPrivate(RotoPaint* publicInterface,
                     bool isPaintByDefault)
        : isPaintByDefault(isPaintByDefault)
        , ui(RotoPaintInteract::create(publicInterface, isPaintByDefault))
    {
    }
};

std::string
RotoPaint::getPluginDescription() const
{
    return "RotoPaint is a vector based free-hand drawing node that helps for tasks such as rotoscoping, matting, etc...";
}

RotoPaint::RotoPaint(NodePtr node,
                     bool isPaintByDefault)
    : EffectInstance(node)
    , _imp( new RotoPaintPrivate(this, isPaintByDefault) )
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

RotoPaint::~RotoPaint()
{
}

bool
RotoPaint::isDefaultBehaviourPaintContext() const
{
    return _imp->isPaintByDefault;
}

std::string
RotoPaint::getPluginID() const
{
    return PLUGINID_NATRON_ROTOPAINT;
}

std::string
RotoPaint::getPluginLabel() const
{
    return "RotoPaint";
}

std::string
RotoNode::getPluginID() const
{
    return PLUGINID_NATRON_ROTO;
}

std::string
RotoNode::getPluginLabel() const
{
    return "Roto";
}

std::string
RotoNode::getPluginDescription() const
{
    return "Create masks and shapes";
}

bool
RotoPaint::isHostChannelSelectorSupported(bool* defaultR,
                                          bool* defaultG,
                                          bool* defaultB,
                                          bool* defaultA) const
{
    *defaultR = true;
    *defaultG = true;
    *defaultB = true;
    *defaultA = true;

    return true;
}

bool
RotoNode::isHostChannelSelectorSupported(bool* defaultR,
                                         bool* defaultG,
                                         bool* defaultB,
                                         bool* defaultA) const
{
    *defaultR = false;
    *defaultG = false;
    *defaultB = false;
    *defaultA = true;

    return true;
}

std::string
RotoPaint::getInputLabel (int inputNb) const
{
    if (inputNb == ROTOPAINT_MASK_INPUT_INDEX) {
        return "Mask";
    } else if (inputNb == 0) {
        return "Bg";
    } else {
        std::stringstream ss;
        ss << "Bg" << inputNb + 1;

        return ss.str();
    }
}

bool
RotoPaint::isInputMask(int inputNb) const
{
    return inputNb == ROTOPAINT_MASK_INPUT_INDEX;
}

void
RotoPaint::addAcceptedComponents(int inputNb,
                                 std::list<ImageLayerDesc>* comps)
{
    if (inputNb != ROTOPAINT_MASK_INPUT_INDEX) {
        comps->push_back(ImageLayerDesc::getRGBAComponents());
        comps->push_back(ImageLayerDesc::getRGBComponents());
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
    comps->push_back(ImageLayerDesc::getAlphaComponents());
}

void
RotoPaint::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
RotoPaint::initializeKnobs()
{
    //This page is created in the RotoContext, before initializeKnobs() is called.
    KnobPagePtr generalPage = std::dynamic_pointer_cast<KnobPage>( getKnobByName("General") );

    assert(generalPage);

    RotoContextPtr context = getNode()->getRotoContext();
    assert(context);
    QObject::connect( context.get(), SIGNAL(breakMultiStroke()), this, SLOT(onBreakMultiStrokeTriggered()) );
    _imp->ui->connectToCurveContext();

    // The curve viewer buttons come first: the viewer shows its knobs in creation order.
    _imp->ui->createViewerButtonKnobs(generalPage);

    // RotoPaint

    _imp->ui->createSettingKnobs(this, generalPage);

    KnobButtonPtr resetCloneOffset = _imp->ui->createResetCloneOffsetKnob(generalPage);

    _imp->ui->addSettingKnobsToViewerUI(this, resetCloneOffset);

    KnobPagePtr toolbar = _imp->ui->createToolbarKnobs(eRotoCurveToolSetAll | eRotoCurveToolSetObjectSelect);
    _imp->ui->createObjectSelectKnobs(generalPage);

    _imp->ui->createToolKnobs(this, toolbar, _imp->isPaintByDefault);

    _imp->ui->createRightClickMenuKnobs(generalPage);

    KnobButtonPtr defaultAction;
    KnobGroupPtr defaultRole;
    if (_imp->isPaintByDefault) {
        defaultAction = _imp->ui->brushAction.lock();
        defaultRole = _imp->ui->paintBrushToolGroup.lock();
    } else {
        defaultAction = _imp->ui->drawBezierAction.lock();
        defaultRole = _imp->ui->bezierEditionToolGroup.lock();
    }
    _imp->ui->setCurrentTool(defaultAction);
    _imp->ui->onRoleChangedInternal(defaultRole);
    _imp->ui->onToolChangedInternal(defaultAction);
} // RotoPaint::initializeKnobs

void
RotoPaint::getPluginShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    RotoCurveInteract::getViewerButtonShortcuts(shortcuts);

    RotoPaintBrushKnobs::getSettingShortcuts(shortcuts);

    RotoCurveInteract::getToolbarShortcuts(eRotoCurveToolSetAll | eRotoCurveToolSetObjectSelect, shortcuts);
    RotoPaintBrushKnobs::getToolShortcuts(shortcuts);

    RotoCurveInteract::getRightClickMenuShortcuts(shortcuts);
}

bool
RotoPaint::shouldPreferPluginOverlayOverHostOverlay() const
{
    // Ctrl-drags on the points' transform box (pivot, corners) go to the roto overlay before the
    // shape's transform jack.
    return !_imp->ui->ctrlDown || _imp->ui->showCpsBbox;
}

bool
RotoPaint::shouldDrawHostOverlay() const
{
    KnobButtonPtr b = _imp->ui->showTransformHandle.lock();
    if (!b) {
        return true;
    }
    return b->getValue();
}

void
RotoPaint::onEffectCreated(bool /*mayCreateFileDialog*/,
                           const CreateNodeArgs& args)
{
    _imp->ui->onHostEffectCreated(args);
}

void
RotoPaint::onKnobsLoaded()
{
    _imp->ui->selectedItems = getNode()->getRotoContext()->getSelectedCurves();

    // When loading a roto node, always switch to selection mode by default
    KnobButtonPtr defaultAction = _imp->ui->selectAllAction.lock();
    KnobGroupPtr defaultRole = _imp->ui->selectToolGroup.lock();;

    _imp->ui->setCurrentTool(defaultAction);
    _imp->ui->onRoleChangedInternal(defaultRole);
    _imp->ui->onToolChangedInternal(defaultAction);
}

bool
RotoPaint::knobChanged(KnobI* k,
                       ValueChangedReasonEnum reason,
                       ViewSpec view,
                       double time,
                       bool originatedFromMainThread)
{
    if (!k) {
        return false;
    }


    RotoContextPtr ctx = getNode()->getRotoContext();

    if (!ctx) {
        return false;
    }
    bool ret = false;
    if (_imp->ui->onKnobChanged(k, time, &ret)) {
        return ret;
    }

    return ctx->knobChanged(k, reason, view, time, originatedFromMainThread);
} // RotoPaint::knobChanged

void
RotoPaint::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                             double time)
{
    EffectInstance::refreshExtraStateAfterTimeChanged(isPlayback, time);
    _imp->ui->refreshAfterTimeChanged(time);
}

StatusEnum
RotoPaint::getPreferredMetadataAt(double /*time*/,
                                  ViewIdx /*view*/,
                                  NodeMetadata& metadata)
{
    metadata.setNComps( -1, 4 );
    metadata.setComponentsType(-1, kNatronColorLayerID);

    return eStatusOK;
}

void
RotoPaint::onInputChanged(int inputNb)
{
    NodePtr inputNode = getNode()->getInput(0);

    getNode()->getRotoContext()->onRotoPaintInputChanged(inputNode);
    _imp->ui->onHostInputChanged();
    EffectInstance::onInputChanged(inputNb);
}

StatusEnum
RotoPaint::getRegionOfDefinition(U64 hash,
                                 double time,
                                 const RenderScale & scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    StatusEnum st = EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);

    if (st != eStatusOK) {
        rod->x1 = rod->y1 = rod->x2 = rod->y2 = 0.;
    }
    RectD maskRod;
    getNode()->getRotoContext()->getMaskRegionOfDefinition(time, view, &maskRod);
    if ( rod->isNull() ) {
        *rod = maskRod;
    } else {
        rod->merge(maskRod);
    }

    return eStatusOK;
}

FramesNeededMap
RotoPaint::getFramesNeeded(double time,
                           ViewIdx view)
{
    FramesNeededMap ret;
    FrameRangesMap views;
    OfxRangeD range;

    range.min = range.max = time;
    views[view].push_back(range);
    ret.insert( std::make_pair(0, views) );

    return ret;
}

void
RotoPaint::getRegionsOfInterest(double time,
                                const RenderScale & scale,
                                const RectD & outputRoD, //!< the RoD of the effect, in canonical coordinates
                                const RectD & renderWindow, //!< the region to be rendered in the output image, in Canonical Coordinates
                                ViewIdx view,
                                RoIMap* ret)
{
    RotoContextPtr roto = getNode()->getRotoContext();
    NodePtr bottomMerge = roto->getRotoPaintBottomMergeNode();

    if (bottomMerge) {
        ret->insert( std::make_pair(bottomMerge->getEffectInstance(), renderWindow) );
    }
    EffectInstance::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);
}

bool
RotoPaint::isIdentity(double time,
                      const RenderScale & scale,
                      const RectI & roi,
                      ViewIdx view,
                      double* inputTime,
                      ViewIdx* inputView,
                      int* inputNb)
{
    *inputView = view;
    NodePtr node = getNode();
    EffectInstancePtr maskInput = getInput(ROTOPAINT_MASK_INPUT_INDEX);
    if (maskInput) {
        RectD maskRod;
        bool isProjectFormat;
        StatusEnum s = maskInput->getRegionOfDefinition_public(maskInput->getRenderHash(), time, scale, view, &maskRod, &isProjectFormat);
        Q_UNUSED(s);
        const RectI maskPixelRod = maskRod.toPixelEnclosing(scale, getRenderMetadata(time, view)->getPixelAspectRatio(ROTOPAINT_MASK_INPUT_INDEX));
        if ( !maskPixelRod.intersects(roi) ) {
            *inputTime = time;
            *inputNb = 0;

            return true;
        }
    }

    std::list<RotoDrawableItemPtr> items = node->getRotoContext()->getCurvesByRenderOrder();
    if ( items.empty() ) {
        *inputNb = 0;
        *inputTime = time;

        return true;
    }

    return false;
}

StatusEnum
RotoPaint::render(const RenderActionArgs& args)
{
    RotoContextPtr roto = getNode()->getRotoContext();
    std::list<RotoDrawableItemPtr> items = roto->getCurvesByRenderOrder(false /*onlyActiveItems*/);
    ImageBitDepthEnum bgDepth = getRenderMetadata(args.time, args.view)->getBitDepth(0);
    std::list<ImageLayerDesc> neededComps;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator layer = args.outputLayers.begin();
         layer != args.outputLayers.end(); ++layer) {
        neededComps.push_back(layer->first);
    }

    if (items.empty()) {
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator layer = args.outputLayers.begin();
             layer != args.outputLayers.end(); ++layer) {
            RectI bgImgRoI;
            ImagePtr bgImg = getImage(0, args.time, args.mappedScale, args.view, 0, &layer->first, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM /*returnOpenGLtexture*/, 0 /*textureDepth*/, &bgImgRoI);
            if (bgImg) {
                if (bgImg->getComponents() != layer->second->getComponents()) {
                    bgImg->convertToFormat(args.roi, 3, false, layer->second.get());
                } else {
                    layer->second->pasteFrom(*bgImg, args.roi, false);
                }
            } else {
                layer->second->fillZero(args.roi);
            }
        }
    } else {
        NodesList rotoPaintNodes;
        {
            bool ok = getThreadLocalRotoPaintTreeNodes(&rotoPaintNodes);
            if (!ok) {
                throw std::logic_error("RotoPaint::render(): getThreadLocalRotoPaintTreeNodes() failed");
            }
        }
        NodePtr bottomMerge = roto->getRotoPaintBottomMergeNode();
        RenderingFlagSetter flagIsRendering(bottomMerge);

        unsigned int mipmapLevel = args.mappedScale.toMipmapLevel();
        RenderRoIArgs rotoPaintArgs(args.time,
                                    args.mappedScale,
                                    mipmapLevel,
                                    args.view,
                                    args.byPassCache,
                                    args.roi,
                                    RectD(),
                                    neededComps,
                                    bgDepth,
                                    false,
                                    this,
                                    eStorageModeRAM /*returnOpenGLtex*/,
                                    args.time);
        std::map<ImageLayerDesc, ImagePtr> rotoPaintImages;
        RenderRoIRetCode code = renderInputOrTakeFromStore(bottomMerge->getEffectInstance(), -1, &rotoPaintArgs, &rotoPaintImages, this);
        if (code == eRenderRoIRetCodeFailed) {
            return eStatusFailed;
        } else if (code == eRenderRoIRetCodeAborted) {
            return eStatusOK;
        } else if ( rotoPaintImages.empty() ) {
            for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator layer = args.outputLayers.begin();
                 layer != args.outputLayers.end(); ++layer) {
                layer->second->fillZero(args.roi);
            }

            return eStatusOK;
        }
        assert(rotoPaintImages.size() == args.outputLayers.size());

        const U64 hash = getRenderHash();

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator layer = args.outputLayers.begin();
             layer != args.outputLayers.end(); ++layer) {
            std::map<ImageLayerDesc, ImagePtr>::iterator rotoImagesIt = rotoPaintImages.find(layer->first);
            assert( rotoImagesIt != rotoPaintImages.end() );
            if ( rotoImagesIt == rotoPaintImages.end() ) {
                continue;
            }
            RectI bgImgRoI;
            ImagePtr bgImg = getImage(0, args.time, args.mappedScale, args.view, 0, &layer->first, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM /*returnOpenGLtexture*/, 0 /*textureDepth*/, &bgImgRoI);
            std::bitset<4> copyChannels = getProcessChannelsForPlane(hash, args.time, args.view, layer->first);
            // The bits follow the plane's own layout, where a one-channel plane's channel is bit 3,
            // but the host reads a one-channel non-Color plane back from channel 0 of the wider
            // image it hands this render (see channelForAlphaForPlane), so that is the channel the
            // bit must guard; the others are discarded by that conversion.
            if ((layer->first.getNumComponents() == 1) && !layer->first.isColorLayer() && (layer->second->getComponentsCount() > 1)) {
                const bool processed = copyChannels[3];
                copyChannels.set();
                copyChannels[0] = processed;
            }
            if ( !rotoImagesIt->second->getBounds().contains(args.roi) ) {
                // We first fill with the bg image because the bounds of the image produced by the last merge of the rotopaint tree
                // might not be equal to the bounds of the image produced by the rotopaint. This is because the RoD of the rotopaint is the
                // union of all the mask strokes bounds, whereas all nodes inside the rotopaint tree don't take the mask RoD into account.
                if (bgImg) {
                    RectI bgBounds = bgImg->getBounds();

                    // The bg bounds might not be inside the roi, but yet we need to fill the whole roi, so just fill borders
                    // with black and transparent, e.g:
                    /*
                        AAAAAAAAA
                        DDXXXXXBB
                        DDXXXXXBB
                        DDXXXXXBB
                        CCCCCCCCC
                     */
                    RectI merge = bgBounds;
                    merge.merge(args.roi);
                    RectI aRect;
                    aRect.x1 = merge.x1;
                    aRect.y1 = bgBounds.y2;
                    aRect.y2 = merge.y2;
                    aRect.x2 = merge.x2;

                    RectI bRect;
                    bRect.x1 = bgBounds.x2;
                    bRect.y1 = bgBounds.y1;
                    bRect.x2 = merge.x2;
                    bRect.y2 = bgBounds.y2;

                    RectI cRect;
                    cRect.x1 = merge.x1;
                    cRect.y1 = merge.y1;
                    cRect.x2 = merge.x2;
                    cRect.y2 = bgBounds.y1;

                    RectI dRect;
                    dRect.x1 = merge.x1;
                    dRect.y1 = bgBounds.y1;
                    dRect.x2 = bgBounds.x1;
                    dRect.y2 = bgBounds.y2;

                    layer->second->fillZero(aRect);
                    layer->second->fillZero(bRect);
                    layer->second->fillZero(cRect);
                    layer->second->fillZero(dRect);

                    if (bgImg->getComponents() != layer->second->getComponents()) {
                        const RectI intersection = args.roi.intersect(bgImg->getBounds());
                        if (!intersection.isNull()) {
                            bgImg->convertToFormat(intersection, 3, false, layer->second.get());
                        }
                    } else {
                        layer->second->pasteFrom(*bgImg, args.roi, false);
                    }
                } else {
                    layer->second->fillZero(args.roi);
                }
            }

            if (rotoImagesIt->second->getComponents() != layer->second->getComponents()) {
                rotoImagesIt->second->convertToFormat(args.roi, 3, false, layer->second.get());
            } else {
                layer->second->pasteFrom(*(rotoImagesIt->second), args.roi, false);
            }
            layer->second->copyUnProcessedChannels(args.roi, copyChannels, bgImg);
        }
    } // RenderingFlagSetter

    return eStatusOK;
} // RotoPaint::render

void
RotoPaint::clearLastRenderedImage()
{
    EffectInstance::clearLastRenderedImage();
    NodesList rotoPaintNodes;
    NodePtr node = getNode();

    if (node) {
        RotoContextPtr roto = node->getRotoContext();
        assert(roto);
        roto->getRotoPaintTreeNodes(&rotoPaintNodes);
        for (NodesList::iterator it = rotoPaintNodes.begin(); it != rotoPaintNodes.end(); ++it) {
            (*it)->clearLastRenderedImage();
        }
    }
}

void
RotoPaint::drawOverlay(double time,
                       const RenderScale & /*renderScale*/,
                       ViewIdx /*view*/)
{
    _imp->ui->drawOverlay(time);
}

void
RotoPaint::onInteractViewportSelectionCleared()
{
    _imp->ui->onViewportSelectionCleared();
}

void
RotoPaint::onInteractViewportSelectionUpdated(const RectD& rectangle,
                                              bool onRelease)
{
    _imp->ui->onViewportSelectionUpdated(rectangle, onRelease);
}

bool
RotoPaint::onOverlayPenDoubleClicked(double /*time*/,
                                     const RenderScale& /*renderScale*/,
                                     ViewIdx /*view*/,
                                     const QPointF& /*viewportPos*/,
                                     const QPointF& pos)
{
    return _imp->ui->onOverlayPenDoubleClicked(pos);
}

bool
RotoPaint::onOverlayPenDown(double time,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            const QPointF& /*viewportPos*/,
                            const QPointF& pos,
                            double pressure,
                            double timestamp,
                            PenType pen)
{
    return _imp->ui->onOverlayPenDown(time, pos, pressure, timestamp, pen);
}

bool
RotoPaint::onOverlayClaimsAltPenDown(double time,
                                     const QPointF& pos)
{
    return _imp->ui->claimsAltPenDown(time, pos);
}

void
RotoPaint::onOverlayPenDownModifiers(KeyboardModifiers modifiers)
{
    _imp->ui->syncModifiers(modifiers);
}

bool
RotoPaint::onOverlayPenMotion(double time,
                              const RenderScale& /*renderScale*/,
                              ViewIdx /*view*/,
                              const QPointF& /*viewportPos*/,
                              const QPointF& pos,
                              double pressure,
                              double timestamp)
{
    return _imp->ui->onOverlayPenMotion(time, pos, pressure, timestamp);
}

bool
RotoPaint::onOverlayPenUp(double /*time*/,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          const QPointF& /*viewportPos*/,
                          const QPointF& /*pos*/,
                          double /*pressure*/,
                          double /*timestamp*/)
{
    return _imp->ui->onOverlayPenUp();
}

bool
RotoPaint::onOverlayKeyDown(double /*time*/,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            Key key,
                            KeyboardModifiers /*modifiers*/)
{
    return _imp->ui->onOverlayKeyDown(key);
}

bool
RotoPaint::onOverlayKeyUp(double /*time*/,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          Key key,
                          KeyboardModifiers /*modifiers*/)
{
    return _imp->ui->onOverlayKeyUp(key);
}

bool
RotoPaint::onOverlayKeyRepeat(double /*time*/,
                              const RenderScale& /*renderScale*/,
                              ViewIdx /*view*/,
                              Key /*key*/,
                              KeyboardModifiers /*modifiers*/)
{
    return false;
} // onOverlayKeyRepeat

bool
RotoPaint::onOverlayFocusGained(double /*time*/,
                                const RenderScale& /*renderScale*/,
                                ViewIdx /*view*/)
{
    return false;
} // onOverlayFocusGained

bool
RotoPaint::onOverlayFocusLost(double /*time*/,
                              const RenderScale& /*renderScale*/,
                              ViewIdx /*view*/)
{
    return _imp->ui->onOverlayFocusLost();
}

RotoPaintInteractPtr
RotoPaint::getInteract() const
{
    return _imp->ui;
}

void
RotoPaint::abortObjectSelectEncoding(bool wait)
{
    if (_imp->ui && _imp->ui->objectSelect) {
        _imp->ui->objectSelect->abortEncoding(wait);
    }
}

bool
RotoPaint::isObjectSelectEncoding() const
{
    return _imp->ui && _imp->ui->objectSelect && _imp->ui->objectSelect->isEncoding();
}

void
RotoPaint::onBreakMultiStrokeTriggered()
{
    _imp->ui->onBreakMultiStrokeTriggered();
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_RotoPaint.cpp"

