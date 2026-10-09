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

#include "NativeRead.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <OpenImageIO/imageio.h>

#include <QString>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
// Bands are rounded to this many rows so the blocks of a compressed file are not split.
const int kMinBandRows = 32;

// Upper bound on the decoded bytes one band holds at a time.
const std::size_t kStagingBytes = std::size_t(16) << 20;

double
pixelAspectOf(const OIIO::ImageSpec& spec)
{
    const float par = spec.get_float_attribute("PixelAspectRatio", 1.f);

    return par > 0.f ? (double)par : 1.;
}

// Where the R, G, B and A of a file sit among its channels, -1 when it has none.
struct ColourChannels {
    int index[4];

    ColourChannels()
        : index { -1, -1, -1, -1 }
    {
    }

    static ColourChannels of(const OIIO::ImageSpec& spec)
    {
        ColourChannels result;

        for (int i = 0; i < spec.nchannels && i < (int)spec.channelnames.size(); ++i) {
            std::string name = spec.channelnames[(std::size_t)i];
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            if (name.compare(0, 5, "rgba.") == 0) {
                name.erase(0, 5);
            }
            if (name.size() != 1) {
                continue;
            }
            const std::size_t slot = std::string("rgba").find(name[0]);
            if (slot != std::string::npos && result.index[slot] < 0) {
                result.index[slot] = i;
            }
        }

        return result;
    }

    // A file with no R, G, B or A stays a black RGBA plane rather than being shuffled in.
    int nComps() const
    {
        if (index[0] >= 0 || index[1] >= 0 || index[2] >= 0) {
            return index[3] >= 0 ? 4 : 3;
        }

        return index[3] >= 0 ? 1 : 4;
    }

    int fileChannelForComponent(int nComps,
                                int component) const
    {
        if (nComps == 1) {
            return index[3];
        }
        if ((nComps == 3 || nComps == 4) && component < nComps) {
            return index[component];
        }

        return -1;
    }
};
} // anonymous namespace

NativeRead::NativeRead(NodePtr node)
    : NativeEffectBase(node)
    , _filename()
{
}

NativePluginDescription
NativeRead::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_READ;
    desc.label = "Read";
    desc.description = tr("Read an image or image sequence.").toStdString();
    desc.grouping = PLUGIN_GROUP_IMAGE;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_READ;
    desc.minorVersion = 0;
    desc.outputKind = eDataKindImage;

    return desc;
}

void
NativeRead::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("File"));
    KnobFilePtr filename = createKnob<KnobFile>(tr("File"));

    filename->setName(kOfxImageEffectFileParamName);
    filename->setAsInputImage();
    filename->setHintToolTip(tr("The image or image sequence to read."));
    // NativeEffectBase has no counterpart of an OpenFX clip-preferences slave param, so without
    // this the preferred metadata is only consulted once, at creation, before a file is chosen.
    filename->setIsMetadataSlave(true);
    page->addKnob(filename);
    _filename = filename;
}

std::string
NativeRead::pathAtTime(double time) const
{
    KnobFilePtr filename = _filename.lock();

    if (!filename) {
        return std::string();
    }

    return filename->getFileName((int)std::floor(time + 0.5), ViewIdx(0));
}

StatusEnum
NativeRead::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setBitDepth(-1, eImageBitDepthFloat);
    metadata.setComponentsType(-1, kNatronColorLayerID);
    metadata.setNComps(-1, 4);

    const std::string path = pathAtTime(getCurrentTime());
    if (path.empty()) {
        return eStatusOK;
    }
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    if (!header || header->subimages.empty()) {
        return eStatusOK;
    }
    const OIIO::ImageSpec& spec = header->subimages[0];

    // OpenFX formats must start at (0, 0), so a positive display-window origin only widens the
    // format. Mirroring the display window within itself top to bottom always lands it at
    // [0, full_height), which is why full_y drops out.
    metadata.setOutputFormat(RectI(0, 0, spec.full_x + spec.full_width, spec.full_height));
    metadata.setPixelAspectRatio(-1, pixelAspectOf(spec));
    metadata.setNComps(-1, ColourChannels::of(spec).nComps());

    return eStatusOK;
}

StatusEnum
NativeRead::getRegionOfDefinition(U64 /*hash*/,
                                  double time,
                                  const RenderScale& /*scale*/,
                                  ViewIdx /*view*/,
                                  RectD* rod)
{
    const std::string path = pathAtTime(time);

    if (path.empty()) {
        // The base implementation leaves the RoD null for a node without inputs, and a null RoD
        // makes renderRoI return no planes at all.
        Format format;
        getApp()->getProject()->getProjectDefaultFormat(&format);
        *rod = format.toCanonicalFormat();
        clearPersistentMessage(false);

        return eStatusOK;
    }

    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    if (!header || header->subimages.empty()) {
        setPersistentMessage(eMessageTypeError, error.empty() ? tr("Could not read %1").arg(QString::fromStdString(path)).toStdString() : error);

        return eStatusFailed;
    }
    const OIIO::ImageSpec& spec = header->subimages[0];
    *rod = OiioReadSupport::dataWindowOf(spec).toCanonical_noClipping(0, pixelAspectOf(spec));
    clearPersistentMessage(false);

    return eStatusOK;
}

namespace {
struct ReadPlane {
    Image* target;
    RectI rect;
    std::string path;
    ColourChannels channels;
};

// Decodes the rows [y1, y2) of plane.rect into plane.target, through a staging buffer holding
// the file channels from the first to the last one the plane uses.
bool
decodeBand(const ReadPlane& plane,
           Image::WriteAccess& access,
           int nComps,
           int y1,
           int y2,
           std::string* error)
{
    const ColourChannels& map = plane.channels;
    int lo = -1;
    int hi = -1;
    for (int c = 0; c < nComps; ++c) {
        const int idx = map.fileChannelForComponent(nComps, c);
        if (idx < 0) {
            continue;
        }
        lo = lo < 0 ? idx : std::min(lo, idx);
        hi = std::max(hi, idx);
    }
    const int width = plane.rect.width();
    if (lo < 0) {
        for (int y = y1; y < y2; ++y) {
            std::memset(access.pixelAt(plane.rect.x1, y), 0, (std::size_t)width * nComps * sizeof(float));
        }

        return true;
    }

    const int span = hi - lo + 1;
    const RectI window(plane.rect.x1, y1, plane.rect.x2, y2);
    std::vector<float> staging((std::size_t)width * (y2 - y1) * span);
    if (!OiioReadSupport::decode(plane.path, 0, lo, hi + 1, window, staging.data(), (std::size_t)width * span, error)) {
        return false;
    }
    for (int y = y1; y < y2; ++y) {
        const float* src = staging.data() + (std::size_t)(y - y1) * width * span;
        float* dst = (float*)access.pixelAt(plane.rect.x1, y);
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < nComps; ++c) {
                const int idx = map.fileChannelForComponent(nComps, c);
                dst[c] = idx < 0 ? 0.f : src[idx - lo];
            }
            src += span;
            dst += nComps;
        }
    }

    return true;
}

void
zeroRows(Image::WriteAccess& access,
         const RectI& area,
         int nComps)
{
    for (int y = area.y1; y < area.y2; ++y) {
        std::memset(access.pixelAt(area.x1, y), 0, (std::size_t)area.width() * nComps * sizeof(float));
    }
}
} // anonymous namespace

StatusEnum
NativeRead::render(const RenderActionArgs& args)
{
    const std::string path = pathAtTime(args.time);

    std::string error;
    std::shared_ptr<const OiioReadSupport::Header> header;
    if (!path.empty()) {
        header = OiioReadSupport::readHeader(path, &error);
        if (!header || header->subimages.empty()) {
            setPersistentMessage(eMessageTypeError, error.empty() ? tr("Could not read %1").arg(QString::fromStdString(path)).toStdString() : error);

            return eStatusFailed;
        }
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(this);
    std::atomic<bool> failed(false);
    std::mutex errorMutex;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }

        const int nComps = (int)image->getComponentsCount();
        const RectI roi = args.roi;
        const unsigned int level = args.mappedScale.toMipmapLevel();

        RectI dataWindow;
        ColourChannels channels;
        if (header && it->first.isColorLayer()) {
            dataWindow = OiioReadSupport::dataWindowOf(header->subimages[0]);
            channels = ColourChannels::of(header->subimages[0]);
        }

        // At a coarser level the rows behind the window are decoded at full size, then reduced.
        ImagePtr full;
        const RectI fullRect = roi.upscalePowerOfTwo(level).intersect(dataWindow);
        Image* target = image.get();
        if (level > 0 && !fullRect.isNull()) {
            full = std::make_shared<Image>(image->getComponents(), image->getRoD(), fullRect, 0, image->getPixelAspectRatio(), eImageBitDepthFloat, image->getFieldingOrder(), false);
            target = full.get();
        }

        {
            Image::WriteAccess outAccess(image.get());
            if (level > 0 || fullRect != roi) {
                zeroRows(outAccess, roi, nComps);
            }
        }

        if (fullRect.isNull()) {
            continue;
        }

        ReadPlane plane;
        plane.target = target;
        plane.rect = fullRect;
        plane.path = path;
        plane.channels = channels;

        const int width = fullRect.width();
        const int height = fullRect.height();
        const int span = std::max(1, nComps);
        const int memoryRows = std::max(1, (int)(kStagingBytes / ((std::size_t)width * span * sizeof(float))));
        int rowsPerBand = std::max(kMinBandRows, ((height + nThreads - 1) / std::max(1, nThreads) + kMinBandRows - 1) / kMinBandRows * kMinBandRows);
        rowsPerBand = std::min(rowsPerBand, memoryRows);
        const int nBands = (height + rowsPerBand - 1) / rowsPerBand;

        {
            Image::WriteAccess access(target);
            const std::function<void(int)> renderBand = [&](int band) {
                if (cancel.check() || failed.load()) {
                    return;
                }
                const int y1 = fullRect.y1 + band * rowsPerBand;
                const int y2 = std::min(y1 + rowsPerBand, fullRect.y2);
                std::string bandError;
                if (!decodeBand(plane, access, nComps, y1, y2, &bandError)) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!failed.exchange(true)) {
                        error = bandError;
                    }
                }
            };
            parallelForCancellable(nBands, nThreads, cancel, renderBand);
        }

        if (failed.load()) {
            setPersistentMessage(eMessageTypeError, error);

            return eStatusFailed;
        }

        if (full && !cancel.check()) {
            full->downscaleMipmap(image->getRoD(), fullRect, 0, level, false, image.get());
        }
    }

    clearPersistentMessage(false);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
