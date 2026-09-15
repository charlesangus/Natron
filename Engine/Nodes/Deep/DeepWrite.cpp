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

#include "DeepWrite.h"

#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <OpenImageIO/deepdata.h>
#include <OpenImageIO/imageio.h>

#include <QString>

#include "Global/FloatingPointExceptions.h"

#include "Engine/DeepImage.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#define kDeepWriteTileSize 64

NATRON_NAMESPACE_ENTER

namespace {
bool
isDepthChannelName(const std::string& name)
{
    return (name == "Z") || (name == "ZBack") || (name == "Zback");
}

// The index into image's sample table of the pixel at (x, y), or -1 when that pixel is outside
// image's bounds.
std::ptrdiff_t
pixelIndex(const DeepImage& image,
           int x,
           int y)
{
    const RectI& bounds = image.getBounds();

    if (!bounds.contains(x, y)) {
        return -1;
    }

    return ((std::ptrdiff_t)(y - bounds.y1) * (std::ptrdiff_t)bounds.width()) + (std::ptrdiff_t)(x - bounds.x1);
}
} // anonymous namespace

NativePluginDescription
DeepWrite::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPWRITE;
    desc.label = "DeepWrite";
    desc.description = tr("Write deep data to a deep EXR file, as a scanline or a tiled part, "
                          "and pass it through unchanged. Every channel is written under its own "
                          "name, deep AOVs included, and samples keep the order they arrived in.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindDeep));
    desc.outputKind = eDataKindDeep;
    desc.isWriter = true;

    return desc;
}

void
DeepWrite::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobOutputFilePtr filename = createKnob<KnobOutputFile>(tr("File"));

    filename->setName("filename");
    filename->setAsOutputImageFile();
    filename->setHintToolTip(tr("The deep EXR file to write."));
    page->addKnob(filename);
    _filename = filename;

    KnobBoolPtr tiled = createKnob<KnobBool>(tr("Tiled"));
    tiled->setName("tiled");
    tiled->setAnimationEnabled(false);
    tiled->setDefaultValue(false);
    tiled->setHintToolTip(tr("Write a deep tiled EXR part rather than a deep scanline one."));
    page->addKnob(tiled);
    _tiled = tiled;
}

std::string
DeepWrite::getFilenameAtTime(double time) const
{
    KnobOutputFilePtr filename = _filename.lock();

    if (!filename) {
        return std::string();
    }

    return filename->generateFileNameAtTime(std::floor(time + 0.5), ViewIdx(0)).toStdString();
}

StatusEnum
DeepWrite::writeDeepImage(const std::string& filename,
                          const DeepImage& image)
{
    const RectI& bounds = image.getBounds();

    if (bounds.isNull()) {
        setPersistentMessage(eMessageTypeError, tr("Nothing to write: the deep data is empty.").toStdString());

        return eStatusFailed;
    }

    std::vector<std::string> channelNames;
    std::vector<const float*> channelData;
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = image.getChannels().begin(); it != image.getChannels().end(); ++it) {
        channelNames.push_back(it->first);
        channelData.push_back(it->second.data());
    }

    if (channelNames.empty()) {
        setPersistentMessage(eMessageTypeError, tr("Nothing to write: the deep data has no channels.").toStdString());

        return eStatusFailed;
    }

    OIIO::ImageSpec spec(bounds.width(), bounds.height(), (int)channelNames.size(), OIIO::TypeDesc::FLOAT);
    spec.channelnames = channelNames;
    spec.deep = true;
    // EXR's rows run downwards from the top of the display window while Natron's run upwards from
    // its bottom, so a data window that does not start at y == 0 is written as an equally offset
    // window measured from the other end -- which is what makes the bounds survive a round trip.
    spec.x = bounds.x1;
    spec.y = 0;
    spec.full_x = 0;
    spec.full_y = 0;
    spec.full_width = bounds.x2;
    spec.full_height = bounds.y2;

    KnobBoolPtr tiledKnob = _tiled.lock();
    const bool tiled = tiledKnob && tiledKnob->getValue();
    if (tiled) {
        spec.tile_width = kDeepWriteTileSize;
        spec.tile_height = kDeepWriteTileSize;
    }

#ifdef DEBUG
    // OIIO's first EXR open() in the process builds its default colour config, and OCIO's builtin
    // colourspace probing raises FE_INVALID doing so. The render threads trap that in debug builds;
    // OFX plugins are shielded by OfxImageEffectInstance::mainEntry(), a native node has to be here.
    boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
    OIIO::ImageOutput::unique_ptr output = OIIO::ImageOutput::create(filename);
    if (!output) {
        setPersistentMessage(eMessageTypeError, OIIO::geterror());

        return eStatusFailed;
    }
    if (!output->supports("deepdata") || (tiled && !output->supports("tiles"))) {
        setPersistentMessage(eMessageTypeError, tr("%1 cannot hold this kind of deep data.").arg(QString::fromUtf8(filename.c_str())).toStdString());

        return eStatusFailed;
    }

    OIIO::DeepData deepData;
    deepData.init(spec);

    const SampleTable& table = image.getSampleTable();
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        // The file's first row is the top of the display window, which is this image's last.
        const std::ptrdiff_t fileRow = (std::ptrdiff_t)(bounds.y2 - 1 - y) * (std::ptrdiff_t)bounds.width();
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            const std::ptrdiff_t index = pixelIndex(image, x, y);
            const std::ptrdiff_t filePixel = fileRow + (std::ptrdiff_t)(x - bounds.x1);
            const U32 count = table.getCount((std::size_t)index);
            const U64 offset = table.getOffset((std::size_t)index);

            deepData.set_samples((int)filePixel, (int)count);
            for (U32 s = 0; s < count; ++s) {
                for (std::size_t c = 0; c < channelData.size(); ++c) {
                    deepData.set_deep_value((int)filePixel, (int)c, (int)s, channelData[c] ? channelData[c][offset + s] : 0.f);
                }
            }
        }
    }

    if (!output->open(filename, spec)) {
        setPersistentMessage(eMessageTypeError, output->geterror());

        return eStatusFailed;
    }
    if (!output->write_deep_image(deepData)) {
        setPersistentMessage(eMessageTypeError, output->geterror());
        output->close();

        return eStatusFailed;
    }
    output->close();

    return eStatusOK;
} // DeepWrite::writeDeepImage

StatusEnum
DeepWrite::renderDeep(const DeepRenderActionArgs& args)
{
    const DeepImagePtr source = args.getInputDeepImage(0);

    if (!source) {
        setPersistentMessage(eMessageTypeError, tr("No deep data to write.").toStdString());

        return eStatusFailed;
    }

    const std::string filename = getFilenameAtTime(args.time);
    if (filename.empty()) {
        setPersistentMessage(eMessageTypeError, tr("No file to write to.").toStdString());

        return eStatusFailed;
    }

    std::vector<std::string> channelNames;
    std::vector<const DeepChannelBuffer*> channels;
    int alphaChannelIndex = 0;
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = source->getChannels().begin(); it != source->getChannels().end(); ++it) {
        if (isDepthChannelName(it->first)) {
            continue;
        }
        if (it->first == "A") {
            alphaChannelIndex = (int)channelNames.size();
        }
        channelNames.push_back(it->first);
        channels.push_back(&it->second);
    }

    if (channelNames.empty()) {
        setPersistentMessage(eMessageTypeError, tr("Nothing to write: the deep data has no channels besides Z and ZBack.").toStdString());

        return eStatusFailed;
    }

    const DeepChannelBuffer* sourceZ = source->getChannel("Z");
    const DeepChannelBuffer* sourceZBack = source->getChannel("ZBack");

    const StatusEnum passThrough = renderDeepTwoPass(args, channelNames, alphaChannelIndex, [source](int x, int y) -> U32 {
        const std::ptrdiff_t index = pixelIndex(*source, x, y);

        return (index < 0) ? 0 : source->getSampleTable().getCount((std::size_t)index); }, [source, sourceZ, sourceZBack, &channels](int x, int y, const MutableDeepPixelView& out) {
        const std::ptrdiff_t index = pixelIndex(*source, x, y);

        if (index < 0) {
            return;
        }
        const U64 offset = source->getSampleTable().getOffset((std::size_t)index);
        for (int s = 0; s < out.numSamples; ++s) {
            out.z[s] = sourceZ ? sourceZ->data()[offset + s] : 0.f;
            out.zback[s] = sourceZBack ? sourceZBack->data()[offset + s] : out.z[s];
            for (int c = 0; c < out.numChannels; ++c) {
                out.channels[c][s] = channels[c]->data()[offset + s];
            }
        } }, source->isTidy());

    if (passThrough != eStatusOK) {
        return passThrough;
    }

    const StatusEnum written = writeDeepImage(filename, *args.outputDeepImage);
    if (written != eStatusOK) {
        return written;
    }

    clearPersistentMessage(false);

    return eStatusOK;
} // DeepWrite::renderDeep

NATRON_NAMESPACE_EXIT
