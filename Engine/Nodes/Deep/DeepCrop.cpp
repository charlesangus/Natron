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

#include "DeepCrop.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/DeepImage.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

NativePluginDescription
DeepCrop::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPCROP;
    desc.label = "DeepCrop";
    desc.description = tr("Crop the deep input to a rectangle in X and Y and/or to a range in Z. "
                          "A sample is kept only if its Z is at or past Near and its ZBack (or Z, "
                          "on a point sample) is at or before Far. Reformat sets the output format "
                          "to the crop rectangle instead of keeping the input's.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindDeep));
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepCrop::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobDoublePtr bbox = createKnob<KnobDouble>(tr("Bbox"), 4);
    bbox->setName("bbox");
    bbox->setAsRectangle();
    bbox->setAnimationEnabled(false);
    {
        Format projectFormat;
        getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
        bbox->setDefaultValue((double)projectFormat.x1, 0);
        bbox->setDefaultValue((double)projectFormat.y1, 1);
        bbox->setDefaultValue((double)projectFormat.width(), 2);
        bbox->setDefaultValue((double)projectFormat.height(), 3);
    }
    bbox->setHintToolTip(tr("The rectangle, in canonical coordinates and as (X, Y, Width, Height), that Use Bbox crops the input to."));
    // See DeepRead::initializeKnobs()'s _filename for why NativeEffectBase needs this spelled out:
    // Reformat's output format (below) is computed from Bbox.
    bbox->setIsMetadataSlave(true);
    page->addKnob(bbox);
    _bbox = bbox;

    KnobBoolPtr useBBox = createKnob<KnobBool>(tr("Use Bbox"));
    useBBox->setName("useBBox");
    useBBox->setDefaultValue(true);
    useBBox->setHintToolTip(tr("Crop the input's samples to Bbox in X and Y."));
    page->addKnob(useBBox);
    _useBBox = useBBox;

    KnobDoublePtr zRange = createKnob<KnobDouble>(tr("Z Range"), 2);
    zRange->setName("zRange");
    zRange->setAnimationEnabled(false);
    zRange->setDefaultValue(0., 0);
    zRange->setDefaultValue(1e30, 1);
    zRange->setHintToolTip(tr("The [Near, Far] depth range Use Z Range crops the input's samples to."));
    page->addKnob(zRange);
    _zRange = zRange;

    KnobBoolPtr useZRange = createKnob<KnobBool>(tr("Use Z Range"));
    useZRange->setName("useZRange");
    useZRange->setDefaultValue(false);
    useZRange->setHintToolTip(tr("Crop the input's samples to Z Range in depth."));
    page->addKnob(useZRange);
    _useZRange = useZRange;

    KnobBoolPtr reformat = createKnob<KnobBool>(tr("Reformat"));
    reformat->setName("reformat");
    reformat->setDefaultValue(false);
    reformat->setHintToolTip(tr("Set the output format to Bbox rather than keeping the input's."));
    reformat->setIsMetadataSlave(true);
    page->addKnob(reformat);
    _reformat = reformat;
} // DeepCrop::initializeKnobs

RectD
DeepCrop::getBBoxCanonical(double time) const
{
    KnobDoublePtr bbox = _bbox.lock();
    RectD rect;

    if (!bbox) {
        return rect;
    }
    const double x = bbox->getValueAtTime(time, 0);
    const double y = bbox->getValueAtTime(time, 1);
    const double w = bbox->getValueAtTime(time, 2);
    const double h = bbox->getValueAtTime(time, 3);

    rect.x1 = x;
    rect.y1 = y;
    rect.x2 = x + w;
    rect.y2 = y + h;

    return rect;
}

StatusEnum
DeepCrop::getRegionOfDefinition(U64 hash,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return eStatusReplyDefault;
    }
    bool isProjectFormat = false;
    StatusEnum stat = input->getRegionOfDefinition_public(hash, time, scale, view, rod, &isProjectFormat);

    if (stat == eStatusFailed) {
        return stat;
    }

    KnobBoolPtr useBBoxKnob = _useBBox.lock();
    if (useBBoxKnob && useBBoxKnob->getValueAtTime(time)) {
        *rod = rod->intersect(getBBoxCanonical(time));
    }

    return eStatusOK;
}

bool
DeepCrop::isIdentity(double time,
                     const RenderScale& scale,
                     const RectI& /*roi*/,
                     ViewIdx view,
                     double* inputTime,
                     ViewIdx* inputView,
                     int* inputNb)
{
    KnobBoolPtr useZRangeKnob = _useZRange.lock();

    if (useZRangeKnob && useZRangeKnob->getValueAtTime(time)) {
        return false;
    }

    KnobBoolPtr useBBoxKnob = _useBBox.lock();
    if (useBBoxKnob && useBBoxKnob->getValueAtTime(time)) {
        EffectInstancePtr input = getInput(0);
        if (!input) {
            return false;
        }
        RectD inputRod;
        bool isProjectFormat = false;
        if (input->getRegionOfDefinition_public(input->getHash(), time, scale, view, &inputRod, &isProjectFormat) == eStatusFailed) {
            return false;
        }
        if (!getBBoxCanonical(time).contains(inputRod)) {
            return false;
        }
    }

    *inputTime = time;
    *inputNb = 0;
    *inputView = view;

    return true;
} // DeepCrop::isIdentity

StatusEnum
DeepCrop::getPreferredMetadata(NodeMetadata& metadata)
{
    KnobBoolPtr reformatKnob = _reformat.lock();

    if (reformatKnob && reformatKnob->getValue()) {
        const double par = metadata.getPixelAspectRatio(-1);
        const RectI format = getBBoxCanonical(getCurrentTime()).toPixelEnclosing(0u /*mipmapLevel*/, par);
        metadata.setOutputFormat(format);
    }

    return eStatusOK;
}

StatusEnum
DeepCrop::renderDeep(const DeepRenderActionArgs& args)
{
    const DeepImagePtr input = getInput(0) ? args.getInputDeepImage(0) : DeepImagePtr();

    if (!input) {
        setPersistentMessage(eMessageTypeError, tr("No deep data to crop: nothing is connected to Source.").toStdString());

        return eStatusFailed;
    }

    std::vector<std::string> channelNames;
    int alphaChannelIndex = 0;
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = input->getChannels().begin(); it != input->getChannels().end(); ++it) {
        if ((it->first == "Z") || (it->first == "ZBack")) {
            continue;
        }
        if (it->first == "A") {
            alphaChannelIndex = (int)channelNames.size();
        }
        channelNames.push_back(it->first);
    }

    const DeepChannelBuffer* zBuffer = input->getChannel("Z");
    const DeepChannelBuffer* zBackBuffer = input->getChannel("ZBack");
    const float* const z = zBuffer ? zBuffer->data() : nullptr;
    const float* const zback = zBackBuffer ? zBackBuffer->data() : nullptr;
    std::vector<const float*> channels(channelNames.size());
    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        const DeepChannelBuffer* buffer = input->getChannel(channelNames[c]);
        channels[c] = buffer ? buffer->data() : nullptr;
    }

    KnobBoolPtr useZRangeKnob = _useZRange.lock();
    const bool useZRange = useZRangeKnob && useZRangeKnob->getValueAtTime(args.time);
    float zmin = 0.f;
    float zmax = 0.f;
    if (useZRange) {
        KnobDoublePtr zRangeKnob = _zRange.lock();
        zmin = zRangeKnob ? (float)zRangeKnob->getValueAtTime(args.time, 0) : 0.f;
        zmax = zRangeKnob ? (float)zRangeKnob->getValueAtTime(args.time, 1) : 0.f;
    }

    const RectI inputBounds = input->getBounds();
    const SampleTable& table = input->getSampleTable();

    const auto pixelIndex = [inputBounds](int x, int y) -> std::ptrdiff_t {
        if (!inputBounds.contains(x, y)) {
            return -1;
        }

        return ((std::ptrdiff_t)(y - inputBounds.y1) * (std::ptrdiff_t)inputBounds.width()) + (std::ptrdiff_t)(x - inputBounds.x1);
    };

    // A point sample (zback <= z) uses its own z as both bounds, matching DeepPixelView::zbackAt().
    const auto sampleKept = [z, zback, useZRange, zmin, zmax](U64 offset, U32 s) -> bool {
        if (!useZRange) {
            return true;
        }
        const float sz = z ? z[offset + s] : 0.f;
        const float szback = zback ? zback[offset + s] : sz;

        return (sz >= zmin) && (szback <= zmax);
    };

    clearPersistentMessage(false);

    return renderDeepTwoPass(args, channelNames, alphaChannelIndex, [pixelIndex, &table, sampleKept, useZRange](int x, int y) -> U32 {
        const std::ptrdiff_t index = pixelIndex(x, y);

        if (index < 0) {
            return 0;
        }
        const U32 count = table.getCount((std::size_t)index);
        if (!useZRange) {
            return count;
        }
        const U64 offset = table.getOffset((std::size_t)index);
        U32 kept = 0;
        for (U32 s = 0; s < count; ++s) {
            if (sampleKept(offset, s)) {
                ++kept;
            }
        }

        return kept; }, [pixelIndex, &table, sampleKept, z, zback, &channels](int x, int y, const MutableDeepPixelView& out) {
        const std::ptrdiff_t index = pixelIndex(x, y);

        if (index < 0) {
            return;
        }
        const U32 count = table.getCount((std::size_t)index);
        const U64 offset = table.getOffset((std::size_t)index);
        int slot = 0;
        for (U32 s = 0; s < count; ++s) {
            if (!sampleKept(offset, s)) {
                continue;
            }
            out.z[slot] = z ? z[offset + s] : 0.f;
            out.zback[slot] = zback ? zback[offset + s] : out.z[slot];
            for (int c = 0; c < out.numChannels; ++c) {
                out.channels[c][slot] = channels[c] ? channels[c][offset + s] : 0.f;
            }
            ++slot;
        } }, input->isTidy());
} // DeepCrop::renderDeep

NATRON_NAMESPACE_EXIT
