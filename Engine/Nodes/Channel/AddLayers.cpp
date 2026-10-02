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

#include "AddLayers.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {

const ImageLayerDesc*
findColorStorage(const std::list<ImageLayerDesc>& layers)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer() && (it->getNumComponents() > 0)) {
            return &(*it);
        }
    }

    return NULL;
}

// Mirrors ResolvedLayer::channelBit: a single-channel colour plane is alpha, on bit 3.
int
colorBitOfIndex(int nComps,
                int index)
{
    return (nComps == 1) ? 3 : index;
}

int
colorChannelOnBit(const ImageLayerDesc& layout,
                  int nAvailable,
                  int bit)
{
    const int nComps = layout.getNumComponents();

    for (int i = 0; (i < nComps) && (i < nAvailable) && (i < 4); ++i) {
        if (colorBitOfIndex(nComps, i) == bit) {
            return i;
        }
    }

    return -1;
}

template <typename SRCPIX, typename DSTPIX>
void
copyRow(const Image::ReadAccess* src,
        Image::WriteAccess* dst,
        int y,
        int x1,
        int x2,
        int dstComps,
        const std::vector<int>& srcChannels)
{
    for (int x = x1; x < x2; ++x) {
        DSTPIX* out = (DSTPIX*)dst->pixelAt(x, y);
        if (!out) {
            continue;
        }
        const SRCPIX* in = src ? (const SRCPIX*)src->pixelAt(x, y) : NULL;
        for (int c = 0; c < dstComps; ++c) {
            const int s = (c < (int)srcChannels.size()) ? srcChannels[c] : -1;
            out[c] = (in && (s >= 0)) ? Image::convertPixelDepth<SRCPIX, DSTPIX>(in[s]) : DSTPIX(0);
        }
    }
}

typedef void (*CopyRowFunc)(const Image::ReadAccess*, Image::WriteAccess*, int, int, int, int, const std::vector<int>&);

template <typename DSTPIX>
CopyRowFunc
selectCopyRowForSource(ImageBitDepthEnum srcDepth)
{
    switch (srcDepth) {
    case eImageBitDepthByte:
        return &copyRow<unsigned char, DSTPIX>;
    case eImageBitDepthShort:
        return &copyRow<unsigned short, DSTPIX>;
    case eImageBitDepthFloat:
        return &copyRow<float, DSTPIX>;
    default:
        return NULL;
    }
}

// The input is fetched without mapping to this node's clip preferences, so its depth can
// differ from the output's.
CopyRowFunc
selectCopyRow(ImageBitDepthEnum srcDepth,
              ImageBitDepthEnum dstDepth)
{
    switch (dstDepth) {
    case eImageBitDepthByte:
        return selectCopyRowForSource<unsigned char>(srcDepth);
    case eImageBitDepthShort:
        return selectCopyRowForSource<unsigned short>(srcDepth);
    case eImageBitDepthFloat:
        return selectCopyRowForSource<float>(srcDepth);
    default:
        return NULL;
    }
}
} // namespace

AddLayers::AddLayers(NodePtr node)
    : NativeEffectBase(node)
    , _layers()
    , _subLabel()
{
}

AddLayers::~AddLayers()
{
}

NativePluginDescription
AddLayers::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_ADDLAYERS;
    desc.label = "AddLayers";
    desc.description = tr("Add the chosen project layers to the stream, filled with zeros, wherever the input does not "
                          "have them. A layer the input already has passes through unchanged. The colour views rgba, "
                          "rgb, alpha and xy share one colour plane: adding alpha to an rgb input gives rgba with alpha "
                          "at zero, and the input's own colour channels are kept.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_CHANNEL;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
AddLayers::addAcceptedComponents(int inputNb,
                                 std::list<ImageLayerDesc>* comps)
{
    NativeEffectBase::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
AddLayers::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChannelSetPtr layers = createKnob<KnobChannelSet>(tr("Layers"));
    layers->setName(kAddLayersParamLayers);
    layers->setWithChannelButtons(false);
    layers->setAnimationEnabled(false);
    layers->setIsMetadataSlave(true);
    {
        std::vector<ChannelSetRow> none(1);
        none[0].mode = ChannelSetRow::eModeNone;
        layers->setDefaultValue(layers->encodeRows(none));
    }
    layers->setHintToolTip(tr("The project layers to add. A layer the input already has is left untouched. rgba, rgb, "
                              "alpha and xy name parts of the one colour plane, which is widened to hold them. A regex "
                              "row matches project layer names, e.g. spec.* for every layer starting with spec."));
    page->addKnob(layers);
    _layers = layers;

    // Follows PrecompNode's kNatronOfxParamStringSublabelName precedent: Node.cpp wraps
    // this knob's value in parentheses and shows it next to the node's label on its own.
    KnobStringPtr sublabel = createKnob<KnobString>(tr("SubLabel"));
    sublabel->setName(kNatronOfxParamStringSublabelName);
    sublabel->setSecretByDefault(true);
    sublabel->setAnimationEnabled(false);
    page->addKnob(sublabel);
    _subLabel = sublabel;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(layers, 0, LayerKnobSpec::eRoleTarget);
    }

    refreshSubLabel();
} // AddLayers::initializeKnobs

void
AddLayers::listRegistryPlanes(double time,
                              ViewIdx view,
                              std::list<ImageLayerDesc>* planes) const
{
    NodePtr node = getNode();
    KnobChannelSetPtr layers = _layers.lock();

    if (node && layers) {
        node->listLayersForKnob(layers, time, view, planes);
    }
}

AddLayers::Outcome
AddLayers::computeOutcome(double time,
                          ViewIdx view)
{
    Outcome outcome;
    KnobChannelSetPtr layers = _layers.lock();

    if (!layers) {
        return outcome;
    }

    std::list<ImageLayerDesc> registryPlanes;
    listRegistryPlanes(time, view, &registryPlanes);
    const std::vector<ResolvedLayer> resolved = layers->resolve(registryPlanes);

    std::list<ImageLayerDesc> present;
    if (getInput(0)) {
        getPresentLayers(time, view, 0, &present);
    }

    const ImageLayerDesc* storage = findColorStorage(present);
    if (storage) {
        outcome.inputStorage = ImageLayerDesc::mapNCompsToColorLayer(storage->getNumComponents());
        outcome.inputBits = ImageLayerDesc::colorStorageBits(outcome.inputStorage);
    }

    std::set<std::string> presentIDs;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (!it->isColorLayer()) {
            presentIDs.insert(it->getLayerID());
        }
    }

    std::bitset<4> addedBits;
    std::set<std::string> addedIDs;
    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (it->desc.isColorLayer()) {
            addedBits |= it->channels | it->zeroChannels;
        } else if ((presentIDs.find(it->desc.getLayerID()) == presentIDs.end()) && addedIDs.insert(it->desc.getLayerID()).second) {
            outcome.addedLayers.push_back(it->desc);
        }
    }

    if ((addedBits & ~outcome.inputBits).any()) {
        outcome.widensColor = true;
        outcome.outputStorage = ImageLayerDesc::narrowestColorStorageCovering(outcome.inputBits | addedBits);
    }

    return outcome;
} // AddLayers::computeOutcome

void
AddLayers::getComponentsNeededAndProduced(double time,
                                          ViewIdx view,
                                          EffectInstance::ComponentsNeededMap* comps,
                                          double* passThroughTime,
                                          int* passThroughView,
                                          int* passThroughInputNb)
{
    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInputNb = 0;

    std::list<ImageLayerDesc>& produced = (*comps)[-1];
    std::list<ImageLayerDesc>& needed = (*comps)[0];
    produced.clear();
    needed.clear();

    const Outcome outcome = computeOutcome(time, view);
    if (outcome.widensColor) {
        produced.push_back(outcome.outputStorage);
        if (outcome.inputStorage.getNumComponents() > 0) {
            needed.push_back(outcome.inputStorage);
        }
    }
    produced.insert(produced.end(), outcome.addedLayers.begin(), outcome.addedLayers.end());
}

StatusEnum
AddLayers::getPreferredMetadata(NodeMetadata& metadata)
{
    AppInstancePtr app = getApp();
    const double time = app ? app->getTimeLine()->currentFrame() : 0.;
    const Outcome outcome = computeOutcome(time, ViewIdx(0));

    if (outcome.widensColor) {
        metadata.setNComps(-1, outcome.outputStorage.getNumComponents());
    }

    return eStatusOK;
}

StatusEnum
AddLayers::getRegionOfDefinition(U64 hash,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    if (!getInput(0)) {
        calcDefaultRegionOfDefinition(hash, time, scale, view, rod);

        return eStatusOK;
    }

    return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
}

bool
AddLayers::isIdentity(double time,
                      const RenderScale& /*scale*/,
                      const RectI& /*roi*/,
                      ViewIdx view,
                      double* inputTime,
                      ViewIdx* inputView,
                      int* inputNb)
{
    if (!computeOutcome(time, view).producesNothing()) {
        return false;
    }

    *inputNb = 0;
    *inputTime = time;
    *inputView = view;

    return true;
}

StatusEnum
AddLayers::render(const RenderActionArgs& args)
{
    const Outcome outcome = computeOutcome(args.time, args.view);

    // Fetched before any image is locked: fetching renders upstream, which may write into a
    // cached image this render would otherwise already hold a read lock on.
    ImagePtr colorSource;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (it->first.isColorLayer() && (outcome.inputStorage.getNumComponents() > 0)) {
            RectI inputRoI;
            colorSource = getImage(0, args.time, args.mappedScale, args.view, NULL, &outcome.inputStorage, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            break;
        }
    }

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImageLayerDesc& plane = it->first;
        const ImagePtr& outImage = it->second;
        if (!outImage) {
            continue;
        }

        if (!plane.isColorLayer() || !colorSource) {
            outImage->fillZero(args.roi);
            continue;
        }

        const ImageLayerDesc& sourceLayout = colorSource->getComponents();
        const int nAvailable = (int)colorSource->getComponentsCount();
        const int nPlaneComps = plane.getNumComponents();
        std::vector<int> srcChannels((std::size_t)nPlaneComps, -1);
        for (int c = 0; (c < nPlaneComps) && (c < 4); ++c) {
            const int bit = colorBitOfIndex(nPlaneComps, c);
            if (outcome.inputBits[bit]) {
                srcChannels[c] = colorChannelOnBit(sourceLayout, nAvailable, bit);
            }
        }

        const CopyRowFunc copyRowFunc = selectCopyRow(colorSource->getBitDepth(), outImage->getBitDepth());
        if (!copyRowFunc) {
            return eStatusFailed;
        }

        // The image can be wider than the plane (a plane with no same-sized supported layout);
        // its extra channels are written as 0.
        const int dstComps = (int)outImage->getComponentsCount();
        Image::ReadAccess src(colorSource.get());
        Image::WriteAccess dst(outImage.get());
        for (int y = args.roi.y1; y < args.roi.y2; ++y) {
            if (aborted()) {
                return eStatusOK;
            }
            copyRowFunc(&src, &dst, y, args.roi.x1, args.roi.x2, dstComps, srcChannels);
        }
    }

    return eStatusOK;
} // AddLayers::render

std::string
AddLayers::buildSubLabel()
{
    KnobChannelSetPtr layers = _layers.lock();

    if (!layers) {
        return std::string();
    }

    const std::vector<ChannelSetRow> rows = layers->getRows();
    if (rows.empty() || (rows[0].mode == ChannelSetRow::eModeNone)) {
        return std::string();
    }

    AppInstancePtr app = getApp();
    const double time = app ? app->getTimeLine()->currentFrame() : 0.;
    std::list<ImageLayerDesc> registryPlanes;
    listRegistryPlanes(time, ViewIdx(0), &registryPlanes);

    return layers->getSummary(registryPlanes);
}

void
AddLayers::refreshSubLabel()
{
    KnobStringPtr sublabel = _subLabel.lock();

    if (sublabel) {
        sublabel->setValue(buildSubLabel());
    }
}

bool
AddLayers::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    KnobChannelSetPtr layers = _layers.lock();

    if (layers && (k == layers.get())) {
        refreshSubLabel();

        return true;
    }

    return false;
}

void
AddLayers::onKnobsLoaded()
{
    refreshSubLabel();
}

void
AddLayers::onChannelsSelectorRefreshed()
{
    refreshSubLabel();
}

NATRON_NAMESPACE_EXIT
