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

#include "DeepRead.h"

#include <cmath>
#include <string>
#include <vector>

#include <OpenImageIO/deepdata.h>
#include <OpenImageIO/imageio.h>

#include <QString>

#include "Engine/DeepImage.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {
// OpenEXR's deep documentation names the back of a volumetric sample "ZBack", which is what
// DeepImage calls it too, but files written by other tools spell it "Zback".
bool
isZBackChannelName(const std::string& name)
{
    return (name == "ZBack") || (name == "Zback");
}
} // anonymous namespace

NativePluginDescription
DeepRead::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPREAD;
    desc.label = "DeepRead";
    desc.description = tr("Read a deep EXR file, scanline or tiled, as deep data: every pixel's "
                          "sample list and every channel the file carries, including deep AOVs, "
                          "in the order the file stores them.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepRead::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobFilePtr filename = createKnob<KnobFile>(tr("File"));

    filename->setName("filename");
    filename->setAsInputImage();
    filename->setHintToolTip(tr("The deep EXR file to read."));
    page->addKnob(filename);
    _filename = filename;
}

std::string
DeepRead::getFilenameAtTime(double time) const
{
    KnobFilePtr filename = _filename.lock();

    if (!filename) {
        return std::string();
    }

    return filename->getFileName((int)std::floor(time + 0.5), ViewIdx(0));
}

StatusEnum
DeepRead::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& /*scale*/,
                                ViewIdx /*view*/,
                                RectD* rod)
{
    const std::string filename = getFilenameAtTime(time);

    if (filename.empty()) {
        setPersistentMessage(eMessageTypeError, tr("No deep file to read.").toStdString());

        return eStatusFailed;
    }

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(filename);
    if (!input) {
        setPersistentMessage(eMessageTypeError, OIIO::geterror());

        return eStatusFailed;
    }

    const OIIO::ImageSpec& spec = input->spec();
    if (!spec.deep) {
        setPersistentMessage(eMessageTypeError, tr("%1 does not hold deep data.").arg(QString::fromUtf8(filename.c_str())).toStdString());

        return eStatusFailed;
    }

    // EXR's rows run downwards from the top of the display window while Natron's run upwards
    // from its bottom, so the data window is mirrored within the display window.
    rod->x1 = spec.x;
    rod->x2 = spec.x + spec.width;
    rod->y1 = spec.full_y + spec.full_height - (spec.y + spec.height);
    rod->y2 = spec.full_y + spec.full_height - spec.y;

    clearPersistentMessage(false);

    return eStatusOK;
}

StatusEnum
DeepRead::renderDeep(const DeepRenderActionArgs& args)
{
    const std::string filename = getFilenameAtTime(args.time);

    if (filename.empty()) {
        setPersistentMessage(eMessageTypeError, tr("No deep file to read.").toStdString());

        return eStatusFailed;
    }

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(filename);
    if (!input) {
        setPersistentMessage(eMessageTypeError, OIIO::geterror());

        return eStatusFailed;
    }

    const OIIO::ImageSpec& spec = input->spec();
    if (!spec.deep) {
        setPersistentMessage(eMessageTypeError, tr("%1 does not hold deep data.").arg(QString::fromUtf8(filename.c_str())).toStdString());

        return eStatusFailed;
    }

    const int dataX = spec.x;
    const int dataY = spec.y;
    const int dataWidth = spec.width;
    const int dataHeight = spec.height;
    const int displayTop = spec.full_y + spec.full_height;

    OIIO::DeepData deepData;
    if (!input->read_native_deep_image(0, 0, deepData)) {
        setPersistentMessage(eMessageTypeError, input->geterror());

        return eStatusFailed;
    }
    input->close();

    std::vector<std::string> channelNames;
    std::vector<int> fileChannels;
    int zChannel = -1;
    int zBackChannel = -1;
    for (int c = 0; c < deepData.channels(); ++c) {
        const std::string name(deepData.channelname(c));

        if (name == "Z") {
            zChannel = c;
        } else if (isZBackChannelName(name)) {
            zBackChannel = c;
        } else {
            channelNames.push_back(name);
            fileChannels.push_back(c);
        }
    }

    if (channelNames.empty()) {
        setPersistentMessage(eMessageTypeError, tr("%1 has no channels besides Z and ZBack.").arg(QString::fromUtf8(filename.c_str())).toStdString());

        return eStatusFailed;
    }

    // Only the views handed to the fill pass carry this, and nothing this node does with them
    // reads it, so a file with no alpha at all still has a well-formed index to report.
    int alphaChannelIndex = 0;
    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        if (channelNames[c] == "A") {
            alphaChannelIndex = (int)c;
            break;
        }
    }

    // The pixel of deepData holding Natron's (x, y), or -1 when that pixel is outside the file's
    // data window -- which the output's bounds can reach, since they follow the render window.
    // (x, y) is at the requested mipmap level; the file is only ever full resolution, and a
    // reduced-resolution pixel carries the sample list of the first full-resolution pixel of the
    // block it covers. Averaging deep samples across pixels has no single right answer, and a
    // proxy is a preview.
    const unsigned int mipmapLevel = args.mipmapLevel;
    const auto filePixelIndex = [dataX, dataY, dataWidth, dataHeight, displayTop, mipmapLevel](int x, int y) -> int {
        const int fileX = (x << mipmapLevel) - dataX;
        const int fileY = displayTop - 1 - (y << mipmapLevel) - dataY;

        if ((fileX < 0) || (fileX >= dataWidth) || (fileY < 0) || (fileY >= dataHeight)) {
            return -1;
        }

        return (fileY * dataWidth) + fileX;
    };

    clearPersistentMessage(false);

    // Samples are transported in the order the file stores them, so tidiness is whatever the
    // file's author left behind: not this node's to claim.
    return renderDeepTwoPass(args, channelNames, alphaChannelIndex, [&deepData, &filePixelIndex](int x, int y) -> U32 {
        const int pixel = filePixelIndex(x, y);

        return (pixel < 0) ? 0 : (U32)deepData.samples(pixel); }, [&deepData, &filePixelIndex, &fileChannels, zChannel, zBackChannel](int x, int y, const MutableDeepPixelView& out) {
        const int pixel = filePixelIndex(x, y);

        if (pixel < 0) {
            return;
        }
        for (int s = 0; s < out.numSamples; ++s) {
            out.z[s] = (zChannel < 0) ? 0.f : deepData.deep_value(pixel, zChannel, s);
            // The deep spec reads a sample with no ZBack as a point sample, i.e. one whose back
            // is its front; a zero left here would instead read as a sample extending backwards.
            out.zback[s] = (zBackChannel < 0) ? out.z[s] : deepData.deep_value(pixel, zBackChannel, s);
            for (int c = 0; c < out.numChannels; ++c) {
                out.channels[c][s] = deepData.deep_value(pixel, fileChannels[c], s);
            }
        } });
} // DeepRead::renderDeep

NATRON_NAMESPACE_EXIT
