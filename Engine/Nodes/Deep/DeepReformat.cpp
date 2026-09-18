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

#include "DeepReformat.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/DeepImage.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

NativePluginDescription
DeepReformat::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPREFORMAT;
    desc.label = "DeepReformat";
    desc.description = tr("Reposition the deep input in a new format, by a whole number of pixels "
                          "and with no filtering: every sample is copied across exactly as it came "
                          "in. Centre puts the input's format in the middle of the new one; with it "
                          "off the input stays at the origin and only the format changes.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindDeep));
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepReformat::initializeKnobs()
{
    Format projectFormat;

    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);

    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    // These three names are what Node::findPluginFormatKnobs() looks for: it populates the choice
    // from the project's format list, keeps it in step as that list changes, and writes the size
    // and par below from whatever entry is picked. Neither choices nor visibility are set here.
    KnobChoicePtr format = createKnob<KnobChoice>(tr("Format"));
    format->setName(kNatronParamFormatChoice);
    format->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> entries;
        int projectFormatIndex = 0;
        getApp()->getProject()->getProjectFormatEntries(&entries, &projectFormatIndex);
        // That host-side refresh moves the default with setDefaultValueWithoutApplying(), leaving
        // the value alone, so without seeding it here a new node would sit on index 0 -- the first
        // format in the list -- rather than on the project's own.
        format->setDefaultValue(projectFormatIndex);
    }
    format->setHintToolTip(tr("The format to reposition the input in."));
    format->setIsMetadataSlave(true);
    page->addKnob(format);

    KnobIntPtr formatSize = createKnob<KnobInt>(tr("Format Size"), 2);
    formatSize->setName(kNatronParamFormatSize);
    formatSize->setAnimationEnabled(false);
    formatSize->setDefaultValue(projectFormat.width(), 0);
    formatSize->setDefaultValue(projectFormat.height(), 1);
    page->addKnob(formatSize);
    _formatSize = formatSize;

    KnobDoublePtr formatPar = createKnob<KnobDouble>(tr("Format Pixel Aspect Ratio"));
    formatPar->setName(kNatronParamFormatPar);
    formatPar->setAnimationEnabled(false);
    formatPar->setDefaultValue(projectFormat.getPixelAspectRatio());
    page->addKnob(formatPar);

    KnobBoolPtr useCustomSize = createKnob<KnobBool>(tr("Use Custom Size"));
    useCustomSize->setName("useCustomSize");
    useCustomSize->setDefaultValue(false);
    useCustomSize->setAnimationEnabled(false);
    useCustomSize->setHintToolTip(tr("Reposition the input in Custom Size rather than in Format."));
    useCustomSize->setIsMetadataSlave(true);
    page->addKnob(useCustomSize);
    _useCustomSize = useCustomSize;

    KnobIntPtr customSize = createKnob<KnobInt>(tr("Custom Size"), 2);
    customSize->setName("customSize");
    customSize->setAnimationEnabled(false);
    customSize->setDefaultValue(projectFormat.width(), 0);
    customSize->setDefaultValue(projectFormat.height(), 1);
    customSize->setHintToolTip(tr("The width and height, in pixels, of the output format Use Custom Size selects."));
    customSize->setIsMetadataSlave(true);
    page->addKnob(customSize);
    _customSize = customSize;

    KnobBoolPtr centre = createKnob<KnobBool>(tr("Centre"));
    centre->setName("centre");
    centre->setDefaultValue(true);
    centre->setAnimationEnabled(false);
    centre->setHintToolTip(tr("Centre the input's format in the new format rather than anchoring it at the origin."));
    centre->setIsMetadataSlave(true);
    page->addKnob(centre);
    _centre = centre;
} // DeepReformat::initializeKnobs

bool
DeepReformat::getTargetSize(int* width,
                            int* height) const
{
    KnobBoolPtr useCustomSize = _useCustomSize.lock();
    KnobIntPtr size = (useCustomSize && useCustomSize->getValue()) ? _customSize.lock() : _formatSize.lock();

    if (!size) {
        return false;
    }
    *width = size->getValue(0);
    *height = size->getValue(1);

    return (*width > 0) && (*height > 0);
}

void
DeepReformat::getTranslation(int* dx,
                             int* dy) const
{
    *dx = 0;
    *dy = 0;

    KnobBoolPtr centre = _centre.lock();
    if (!centre || !centre->getValue()) {
        return;
    }

    EffectInstancePtr input = getInput(0);
    int width = 0;
    int height = 0;
    if (!input || !getTargetSize(&width, &height)) {
        return;
    }

    const RectI inputFormat = input->getOutputFormat();
    // Truncating division on purpose: an odd size difference lands the input a whole pixel off
    // centre rather than on a half pixel no sample could be written to.
    *dx = (width - inputFormat.width()) / 2;
    *dy = (height - inputFormat.height()) / 2;
}

StatusEnum
DeepReformat::getRegionOfDefinition(U64 hash,
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

    int dx = 0;
    int dy = 0;
    getTranslation(&dx, &dy);
    if ((dx != 0) || (dy != 0)) {
        const double par = getAspectRatio(-1);
        // RectD::translate() takes its offset in pixels despite the rectangle being canonical, so
        // the shift goes through pixel space where a whole-pixel offset means what it says.
        RectI pixelRod = rod->toPixelEnclosing(0u /*mipmapLevel*/, par);
        pixelRod.translate(dx, dy);
        *rod = pixelRod.toCanonical_noClipping(0u /*mipmapLevel*/, par);
    }

    return eStatusOK;
}

void
DeepReformat::getRegionsOfInterest(double time,
                                   const RenderScale& scale,
                                   const RectD& outputRoD,
                                   const RectD& renderWindow,
                                   ViewIdx view,
                                   RoIMap* ret)
{
    EffectInstance::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    int dx = 0;
    int dy = 0;
    getTranslation(&dx, &dy);
    if ((dx == 0) && (dy == 0)) {
        return;
    }

    // What lands in a window of the output came from that window shifted back by the translation;
    // asking the input for the window itself would drop the samples along the leading edges.
    const double par = getAspectRatio(-1);
    const double canonicalDx = dx * par;
    for (RoIMap::iterator it = ret->begin(); it != ret->end(); ++it) {
        it->second.x1 -= canonicalDx;
        it->second.x2 -= canonicalDx;
        it->second.y1 -= dy;
        it->second.y2 -= dy;
    }
}

bool
DeepReformat::isIdentity(double time,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx view,
                         double* inputTime,
                         ViewIdx* inputView,
                         int* inputNb)
{
    int dx = 0;
    int dy = 0;

    getTranslation(&dx, &dy);
    if ((dx != 0) || (dy != 0)) {
        return false;
    }

    *inputTime = time;
    *inputNb = 0;
    *inputView = view;

    return true;
}

StatusEnum
DeepReformat::getPreferredMetadata(NodeMetadata& metadata)
{
    int width = 0;
    int height = 0;

    if (getTargetSize(&width, &height)) {
        // Formats start at (0, 0) -- see DeepRead::getPreferredMetadata() -- so a format carries a
        // size and nothing else, and the reposition lives entirely in the region of definition.
        metadata.setOutputFormat(RectI(0, 0, width, height));
    }

    return eStatusOK;
}

StatusEnum
DeepReformat::renderDeep(const DeepRenderActionArgs& args)
{
    const DeepImagePtr input = getInput(0) ? args.getInputDeepImage(0) : DeepImagePtr();

    if (!input) {
        setPersistentMessage(eMessageTypeError, tr("No deep data to reformat: nothing is connected to Source.").toStdString());

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

    int dx = 0;
    int dy = 0;
    getTranslation(&dx, &dy);

    const RectI inputBounds = input->getBounds();
    const SampleTable& table = input->getSampleTable();

    const auto pixelIndex = [inputBounds, dx, dy](int x, int y) -> std::ptrdiff_t {
        const int sourceX = x - dx;
        const int sourceY = y - dy;

        if (!inputBounds.contains(sourceX, sourceY)) {
            return -1;
        }

        return ((std::ptrdiff_t)(sourceY - inputBounds.y1) * (std::ptrdiff_t)inputBounds.width()) + (std::ptrdiff_t)(sourceX - inputBounds.x1);
    };

    clearPersistentMessage(false);

    return renderDeepTwoPass(args, channelNames, alphaChannelIndex, [pixelIndex, &table](int x, int y) -> U32 {
        const std::ptrdiff_t index = pixelIndex(x, y);

        return (index < 0) ? 0 : table.getCount( (std::size_t)index ); }, [pixelIndex, &table, z, zback, &channels](int x, int y, const MutableDeepPixelView& out) {
        const std::ptrdiff_t index = pixelIndex(x, y);

        if (index < 0) {
            return;
        }
        const U64 offset = table.getOffset( (std::size_t)index );
        for (int s = 0; s < out.numSamples; ++s) {
            out.z[s] = z ? z[offset + s] : 0.f;
            out.zback[s] = zback ? zback[offset + s] : out.z[s];
            for (int c = 0; c < out.numChannels; ++c) {
                out.channels[c][s] = channels[c] ? channels[c][offset + s] : 0.f;
            }
        } }, input->isTidy());
} // DeepReformat::renderDeep

NATRON_NAMESPACE_EXIT
