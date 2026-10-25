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

#include "NativeWrite.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <OpenImageIO/imageio.h>

#include <QObject>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

namespace {
enum FormatTypeEnum {
    eFormatTypeInput = 0,
    eFormatTypeProject,
    eFormatTypeFixed
};

enum FrameRangeEnum {
    eFrameRangeUnion = 0,
    eFrameRangeProject,
    eFrameRangeManual
};

const int kMinBandPixels = 16384;

const double kMaxEvictedFrames = 1 << 16;

const std::size_t kMaxReadBackFiles = 4096;

void
setSecretAndDisabled(const KnobIPtr& knob,
                     bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

std::string
extensionOf(const std::string& path)
{
    return std::filesystem::path(path).extension().string();
}

std::string
viewNameOf(const std::vector<std::string>& names,
           ViewIdx view)
{
    const int index = view.value();

    return (index >= 0 && (std::size_t)index < names.size()) ? names[(std::size_t)index] : std::string();
}

struct ReadBackPlane {
    Image* target;
    RectI rect;
    std::string path;
    int subimage;
    std::vector<int> fileChannels; // one per component, -1 for a component the file lacks
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr toWorking;
};

// Decodes the rows [y1, y2) of plane.rect into plane.target, through a staging buffer holding
// the file channels from the first to the last one the plane uses.
bool
decodeReadBackBand(const ReadBackPlane& plane,
                   Image::WriteAccess& access,
                   int nComps,
                   int y1,
                   int y2,
                   std::string* error)
{
    int lo = -1;
    int hi = -1;
    for (int c = 0; c < nComps; ++c) {
        const int idx = plane.fileChannels[(std::size_t)c];
        if (idx < 0) {
            continue;
        }
        lo = lo < 0 ? idx : std::min(lo, idx);
        hi = std::max(hi, idx);
    }
    const int width = plane.rect.width();
    if (lo < 0) {
        for (int y = y1; y < y2; ++y) {
            std::fill_n((float*)access.pixelAt(plane.rect.x1, y), (std::size_t)width * nComps, 0.f);
        }

        return true;
    }
    const int span = hi - lo + 1;
    const RectI window(plane.rect.x1, y1, plane.rect.x2, y2);
    std::vector<float> staging((std::size_t)width * (y2 - y1) * span);
    if (!OiioReadSupport::decode(plane.path, plane.subimage, lo, hi + 1, window, staging.data(), (std::size_t)width * span, error)) {
        return false;
    }
    for (int y = y1; y < y2; ++y) {
        const float* src = staging.data() + (std::size_t)(y - y1) * width * span;
        float* dst = (float*)access.pixelAt(plane.rect.x1, y);
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < nComps; ++c) {
                const int idx = plane.fileChannels[(std::size_t)c];
                dst[c] = idx < 0 ? 0.f : src[idx - lo];
            }
            src += span;
            dst += nComps;
        }
        if (plane.toWorking && !IoColorSpace::convertRow(*plane.toWorking, (float*)access.pixelAt(plane.rect.x1, y), width, nComps, error)) {
            return false;
        }
    }

    return true;
}

std::vector<ChoiceOption>
compressionOptions(const std::string& formatName)
{
    std::vector<ChoiceOption> options;
    const std::vector<OiioWriteSupport::CompressionChoice>& choices = OiioWriteSupport::compressionChoices(formatName);

    for (std::size_t i = 0; i < choices.size(); ++i) {
        options.push_back(ChoiceOption(choices[i].id, std::string(), choices[i].hint));
    }
    if (options.empty()) {
        options.push_back(ChoiceOption(kNatronWriteCompressionDefault));
    }

    return options;
}

// Every compression any writable format offers, for the menu a file name has not narrowed yet.
std::vector<ChoiceOption>
allCompressionOptions()
{
    std::vector<ChoiceOption> options;
    const std::vector<std::string>& extensions = OiioWriteSupport::writableExtensions();

    for (std::size_t e = 0; e < extensions.size(); ++e) {
        const std::vector<ChoiceOption> formatOptions = compressionOptions(OiioWriteSupport::formatNameForWritableExtension(extensions[e]));
        for (std::size_t i = 0; i < formatOptions.size(); ++i) {
            const bool known = std::any_of(options.begin(), options.end(), [&](const ChoiceOption& o) { return o.id == formatOptions[i].id; });
            if (!known) {
                options.push_back(formatOptions[i]);
            }
        }
    }
    if (options.empty()) {
        options.push_back(ChoiceOption(kNatronWriteCompressionDefault));
    }

    return options;
}

const char*
dataTypeLabel(OiioWriteSupport::DataType type)
{
    switch (type) {
    case OiioWriteSupport::DataType::UInt8:
        return "8 bits integer";
    case OiioWriteSupport::DataType::UInt10:
        return "10 bits integer";
    case OiioWriteSupport::DataType::UInt12:
        return "12 bits integer";
    case OiioWriteSupport::DataType::UInt16:
        return "16 bits integer";
    case OiioWriteSupport::DataType::Half:
        return "16 bits floating point";
    case OiioWriteSupport::DataType::UInt32:
        return "32 bits integer";
    case OiioWriteSupport::DataType::Float:
        return "32 bits floating point";
    case OiioWriteSupport::DataType::UInt64:
        return "64 bits integer";
    case OiioWriteSupport::DataType::Double:
        return "64 bits floating point";
    }

    return "";
}

std::vector<ChoiceOption>
bitDepthOptions(const std::vector<OiioWriteSupport::DataType>& types)
{
    std::vector<ChoiceOption> options;

    options.push_back(ChoiceOption(kNatronWriteBitDepthAuto, std::string(), "Guess from the output format"));
    for (std::size_t i = 0; i < types.size(); ++i) {
        options.push_back(ChoiceOption(OiioWriteSupport::dataTypeId(types[i]), dataTypeLabel(types[i]), std::string()));
    }

    return options;
}

std::vector<OiioWriteSupport::DataType>
allDataTypes()
{
    return {
        OiioWriteSupport::DataType::UInt8,
        OiioWriteSupport::DataType::UInt10,
        OiioWriteSupport::DataType::UInt12,
        OiioWriteSupport::DataType::UInt16,
        OiioWriteSupport::DataType::Half,
        OiioWriteSupport::DataType::UInt32,
        OiioWriteSupport::DataType::Float,
        OiioWriteSupport::DataType::UInt64,
        OiioWriteSupport::DataType::Double
    };
}

bool
sameIDs(const std::vector<ChoiceOption>& a,
        const std::vector<ChoiceOption>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id) {
            return false;
        }
    }

    return true;
}

// Replaces the menu, keeping the chosen entry when the new menu has it and falling back to the
// first otherwise.
void
setMenu(const KnobChoicePtr& knob,
        const std::vector<ChoiceOption>& options)
{
    if (!knob || sameIDs(knob->getEntries_mt_safe(), options)) {
        return;
    }
    knob->populateChoices(options);
    const std::string active = knob->getActiveEntry().id;
    const bool kept = std::any_of(options.begin(), options.end(), [&](const ChoiceOption& o) { return o.id == active; });
    if (!kept) {
        knob->setValue(0);
    }
}

std::string
activeID(const KnobChoicePtr& knob)
{
    return knob ? knob->getActiveEntry().id : std::string();
}

// The sample type a file of `formatName` is written with: the Bit Depth choice when the format
// holds it, else the format's own pick.
OiioWriteSupport::DataType
resolveDataType(const KnobChoicePtr& bitDepth,
                const std::string& formatName,
                const std::string& extension)
{
    OiioWriteSupport::DataType type = OiioWriteSupport::autoDataType(formatName, extension);
    OiioWriteSupport::DataType chosen;

    if (bitDepth && OiioWriteSupport::dataTypeFromId(activeID(bitDepth), &chosen)) {
        const std::vector<OiioWriteSupport::DataType>& valid = OiioWriteSupport::validDataTypes(formatName);
        if (std::find(valid.begin(), valid.end(), chosen) != valid.end()) {
            type = chosen;
        }
    }

    return type;
}

FileColorCategoryEnum
colorCategoryOf(OiioWriteSupport::DataType type,
                const std::string& extension)
{
    switch (type) {
    case OiioWriteSupport::DataType::Half:
    case OiioWriteSupport::DataType::Float:
    case OiioWriteSupport::DataType::Double:
        return eFileColorCategoryFloat;
    case OiioWriteSupport::DataType::UInt10:
    case OiioWriteSupport::DataType::UInt12:
    case OiioWriteSupport::DataType::UInt16: {
        std::string lower = extension;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });

        return (lower == ".dpx" || lower == ".cin") ? eFileColorCategoryLog : eFileColorCategory16Bit;
    }
    case OiioWriteSupport::DataType::UInt32:
    case OiioWriteSupport::DataType::UInt64:
        return eFileColorCategory16Bit;
    default:
        return eFileColorCategory8Bit;
    }
}

struct FrameSource {
    const Image::ReadAccess* access;
    ImageBitDepthEnum depth;
    RectI bounds;
    int nComps;
    std::vector<int> channels;
    RectI window;
    std::size_t outOffset;
    std::size_t outStride;
    bool colour;
};

bool
coveredColumns(const FrameSource& frame,
               int y,
               int* x1,
               int* x2)
{
    const bool inRows = (y >= frame.bounds.y1) && (y < frame.bounds.y2);

    *x1 = inRows ? std::max(frame.window.x1, frame.bounds.x1) : frame.window.x2;
    *x2 = inRows ? std::min(frame.window.x2, frame.bounds.x2) : frame.window.x2;

    return *x2 > *x1;
}

template <typename SRCPIX>
void
convertRow(const FrameSource& frame,
           int y,
           float* out)
{
    const int nOut = (int)frame.channels.size();
    int x1 = 0;
    int x2 = 0;

    if (!coveredColumns(frame, y, &x1, &x2)) {
        return;
    }
    const SRCPIX* in = (const SRCPIX*)frame.access->pixelAt(x1, y);
    float* dst = out + (std::size_t)(x1 - frame.window.x1) * frame.outStride + frame.outOffset;
    for (int x = x1; x < x2; ++x, in += frame.nComps, dst += frame.outStride) {
        for (int c = 0; c < nOut; ++c) {
            const int s = frame.channels[(std::size_t)c];
            if (s >= 0) {
                dst[c] = Image::convertPixelDepth<SRCPIX, float>(in[s]);
            }
        }
    }
}

void
convertFileRow(const FrameSource& frame,
               int fileRow,
               float* out)
{
    // File rows run downwards from the top of the data window, Natron's upwards.
    const int y = frame.window.y2 - 1 - fileRow;

    switch (frame.depth) {
    case eImageBitDepthByte:
        convertRow<unsigned char>(frame, y, out);
        break;
    case eImageBitDepthShort:
        convertRow<unsigned short>(frame, y, out);
        break;
    default:
        convertRow<float>(frame, y, out);
        break;
    }
}

// Runs after convertFileRow(), converting in place the RGB it left in `out`.
bool
convertColourRow(const FrameSource& frame,
                 int fileRow,
                 const OCIO_NAMESPACE::CPUProcessor& processor,
                 std::vector<float>& scratch,
                 float* out,
                 std::string* error)
{
    int x1 = 0;
    int x2 = 0;

    if (frame.channels.size() < 3 || !coveredColumns(frame, frame.window.y2 - 1 - fileRow, &x1, &x2)) {
        return true;
    }
    const int count = x2 - x1;
    scratch.resize((std::size_t)count * 3);
    float* base = out + (std::size_t)(x1 - frame.window.x1) * frame.outStride + frame.outOffset;
    for (int i = 0; i < count; ++i) {
        std::copy(base + (std::size_t)i * frame.outStride, base + (std::size_t)i * frame.outStride + 3, scratch.begin() + (std::ptrdiff_t)i * 3);
    }
    if (!IoColorSpace::convertRow(processor, scratch.data(), count, 3, error)) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        std::copy(scratch.begin() + (std::ptrdiff_t)i * 3, scratch.begin() + (std::ptrdiff_t)i * 3 + 3, base + (std::size_t)i * frame.outStride);
    }

    return true;
}

StatusEnum
copyPlane(const EffectInstance& effect,
          const ImagePtr& source,
          const RectI& sourceBounds,
          const std::vector<int>& srcChannels,
          const ImagePtr& dst,
          const RectI& roi)
{
    const ImageBitDepthEnum dstDepth = dst->getBitDepth();
    const RectI both = source ? roi.intersect(sourceBounds) : RectI();
    const bool hasBoth = source && (both.x2 > both.x1) && (both.y2 > both.y1);

    if (hasBoth) {
        Image::ReadAccess src(source.get());
        const StatusEnum stat = ChannelCopy::copyChannels(&src, source->getBitDepth(), dst.get(), both, srcChannels, effect);
        if (stat != eStatusOK) {
            return stat;
        }
    }

    std::vector<RectI> blank;
    if (!hasBoth) {
        blank.push_back(roi);
    } else {
        blank.push_back(RectI(roi.x1, roi.y1, roi.x2, both.y1));
        blank.push_back(RectI(roi.x1, both.y2, roi.x2, roi.y2));
        blank.push_back(RectI(roi.x1, both.y1, both.x1, both.y2));
        blank.push_back(RectI(both.x2, both.y1, roi.x2, both.y2));
    }
    for (std::size_t i = 0; i < blank.size(); ++i) {
        if ((blank[i].x2 > blank[i].x1) && (blank[i].y2 > blank[i].y1)) {
            const StatusEnum stat = ChannelCopy::copyChannels(NULL, dstDepth, dst.get(), blank[i], srcChannels, effect);
            if (stat != eStatusOK) {
                return stat;
            }
        }
    }

    return eStatusOK;
}

struct PlaneChannels {
    std::vector<int> source;
    std::vector<std::string> names;
};

// The colour row writes its selected channels as RGBA, RGB or alpha alone, whatever else the
// row has enabled, so a partial R/G/B selection still writes a whole RGB triple.
PlaneChannels
colourChannels(const ResolvedLayer* selection,
               int nComps,
               bool formatHasAlpha)
{
    static const char* const kNames[] = { "R", "G", "B", "A" };
    PlaneChannels ret;
    const bool full = (nComps == 3 || nComps == 4);
    bool alphaOnly = (nComps == 1);
    bool wantAlpha = true;
    if (selection && full) {
        alphaOnly = selection->channels.count() == 1 && selection->channels[3] && nComps == 4;
        wantAlpha = selection->channels[3];
    }
    if (alphaOnly) {
        ret.source.push_back(nComps - 1);
        ret.names.push_back("A");

        return ret;
    }
    for (int c = 0; c < nComps; ++c) {
        const bool isAlpha = (nComps == 4 && c == 3);
        if (isAlpha && (!formatHasAlpha || !wantAlpha)) {
            continue;
        }
        ret.source.push_back(c);
        ret.names.push_back(c < 4 ? kNames[c] : "C" + std::to_string(c));
    }

    return ret;
}

// Named layer.channel, the names the native Read regroups into the layer.
PlaneChannels
layerChannels(const ImageLayerDesc& layer,
              const ResolvedLayer* selection)
{
    PlaneChannels ret;
    const std::vector<std::string>& all = layer.getChannels();
    for (std::size_t c = 0; c < all.size(); ++c) {
        if (selection && !selection->isChannelSelected((int)c)) {
            continue;
        }
        ret.source.push_back((int)c);
        ret.names.push_back(layer.getLayerID() + "." + all[c]);
    }

    return ret;
}

const ResolvedLayer*
findSelection(const std::vector<ResolvedLayer>& selected,
              const ImageLayerDesc& layer)
{
    for (std::size_t i = 0; i < selected.size(); ++i) {
        if (layer.isColorLayer() ? selected[i].desc.isColorLayer() : (selected[i].desc.getLayerID() == layer.getLayerID())) {
            return &selected[i];
        }
    }

    return NULL;
}

// A non-colour layer the file gets only some channels of is a layer of those channels alone,
// under its own name, which is what the native Read makes of the file. The output declares it so,
// rather than the input's whole layer with the unwritten channels black.
ImageLayerDesc
writtenLayer(const ImageLayerDesc& layer,
             const PlaneChannels& channels)
{
    if (layer.isColorLayer() || ((int)channels.source.size() == layer.getNumComponents())) {
        return layer;
    }
    const std::vector<std::string>& all = layer.getChannels();
    std::vector<std::string> names;
    for (std::size_t i = 0; i < channels.source.size(); ++i) {
        names.push_back(all[(std::size_t)channels.source[i]]);
    }

    return ImageLayerDesc(layer.getLayerID(), layer.getLayerLabel(), std::string(), names);
}

// `planes` indexes the candidates the file gets, colour first, and `channels` and `outputs` run
// alongside it. The encoder writes this plan and the output declares and shows it, so the two
// cannot disagree.
struct FilePlan {
    std::vector<int> planes;
    std::vector<PlaneChannels> channels;
    std::vector<ImageLayerDesc> outputs;
    std::vector<std::string> dropped;
};

int
plannedPlaneOf(const FilePlan& plan,
               const ImageLayerDesc& layer)
{
    for (std::size_t p = 0; p < plan.outputs.size(); ++p) {
        if (layer.isColorLayer() ? plan.outputs[p].isColorLayer() : (plan.outputs[p] == layer)) {
            return (int)p;
        }
    }

    return -1;
}

FilePlan
planFile(const std::vector<ImageLayerDesc>& candidates,
         const std::vector<ResolvedLayer>& selected,
         bool hasLayerKnob,
         const OiioWriteSupport::FormatCaps& caps)
{
    FilePlan plan;
    bool haveColor = false;

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const ImageLayerDesc& layer = candidates[i];
        if (layer.getNumComponents() <= 0) {
            continue;
        }
        const ResolvedLayer* selection = findSelection(selected, layer);
        // The render is handed a colour plane even when no row selects colour.
        if (hasLayerKnob && !selection) {
            continue;
        }
        if (layer.isColorLayer()) {
            if (haveColor) {
                continue;
            }
            haveColor = true;
            plan.planes.insert(plan.planes.begin(), (int)i);
            plan.channels.insert(plan.channels.begin(), colourChannels(selection, layer.getNumComponents(), caps.alpha));
            plan.outputs.insert(plan.outputs.begin(), layer);
        } else {
            PlaneChannels channels = layerChannels(layer, selection);
            if (channels.source.empty()) {
                continue;
            }
            plan.planes.push_back((int)i);
            plan.outputs.push_back(writtenLayer(layer, channels));
            plan.channels.push_back(channels);
        }
    }
    // A format that cannot hold more than four channels keeps the colour row alone.
    if (!caps.nChannels) {
        while (plan.planes.size() > 1 || (plan.planes.size() == 1 && !haveColor)) {
            plan.dropped.push_back(candidates[(std::size_t)plan.planes.back()].getLayerID());
            plan.planes.pop_back();
            plan.channels.pop_back();
            plan.outputs.pop_back();
        }
    }

    return plan;
}

// The planner drops layers from the back of the plan, so reading `dropped` backwards gives them in
// the order they were listed.
std::string
droppedLayerNames(const FilePlan& plan)
{
    std::string names;

    for (std::size_t i = plan.dropped.size(); i-- > 0;) {
        names += (names.empty() ? "" : ", ") + plan.dropped[i];
    }

    return names;
}

// Without a writable format there is no file to mirror, so only the channel set restricts the output.
OiioWriteSupport::FormatCaps
outputCaps(const std::string& path)
{
    const std::string formatName = path.empty() ? std::string() : OiioWriteSupport::formatNameForWritableExtension(extensionOf(path));
    if (formatName.empty()) {
        OiioWriteSupport::FormatCaps caps;
        caps.alpha = true;
        caps.nChannels = true;

        return caps;
    }

    return OiioWriteSupport::capabilities(formatName);
}

enum PartSplitEnum {
    ePartSplitSingle = 0,
    ePartSplitViews,
    ePartSplitViewsLayers
};

bool
hasViewToken(const std::string& pattern)
{
    return pattern.find("%v") != std::string::npos || pattern.find("%V") != std::string::npos;
}

// OpenEXR names a channel of a non-first view of a multiView part after the view in the component
// before the channel: R becomes Right.R and diffuse.R becomes diffuse.Right.R.
std::string
viewChannelName(const std::string& channel,
                const std::string& view)
{
    const std::size_t dot = channel.rfind('.');

    return dot == std::string::npos ? view + "." + channel : channel.substr(0, dot + 1) + view + "." + channel.substr(dot + 1);
}

struct ViewPlane {
    FrameSource frame;
    std::vector<std::string> names;
    std::string label;
};

struct ViewPlanes {
    std::string view;
    std::vector<ViewPlane> planes;
};

struct PartLayout {
    std::vector<OiioWriteSupport::WritePart> parts;
    std::vector<std::vector<FrameSource>> frames;
};

// `base` is the spec of every part but for its channels.
PartLayout
layoutParts(const OIIO::ImageSpec& base,
            const std::vector<ViewPlanes>& views,
            PartSplitEnum split)
{
    struct Member {
        std::size_t view;
        std::size_t plane;
    };
    PartLayout layout;
    const bool multiView = views.size() > 1;

    const auto addPart = [&](const std::vector<Member>& members, bool prefixViews) {
        OiioWriteSupport::WritePart part;
        std::vector<std::string> names;
        std::vector<FrameSource> frames;
        int alpha = -1;

        for (std::size_t i = 0; i < members.size(); ++i) {
            const ViewPlane& plane = views[members[i].view].planes[members[i].plane];
            FrameSource frame = plane.frame;
            frame.outOffset = names.size();
            for (std::size_t c = 0; c < plane.names.size(); ++c) {
                if (alpha < 0 && plane.names[c] == "A" && !(prefixViews && members[i].view > 0)) {
                    alpha = (int)names.size();
                }
                names.push_back(prefixViews && members[i].view > 0 ? viewChannelName(plane.names[c], views[members[i].view].view) : plane.names[c]);
            }
            frames.push_back(frame);
        }
        for (std::size_t f = 0; f < frames.size(); ++f) {
            frames[f].outStride = names.size();
        }
        part.spec = base;
        part.spec.nchannels = (int)names.size();
        part.spec.channelnames = names;
        part.spec.alpha_channel = alpha;
        layout.parts.push_back(part);
        layout.frames.push_back(frames);
    };

    if (split == ePartSplitSingle) {
        std::vector<Member> members;
        for (std::size_t v = 0; v < views.size(); ++v) {
            for (std::size_t p = 0; p < views[v].planes.size(); ++p) {
                members.push_back(Member { v, p });
            }
        }
        addPart(members, true);
        if (multiView) {
            std::vector<OIIO::ustring> viewNames;
            for (std::size_t v = 0; v < views.size(); ++v) {
                viewNames.push_back(OIIO::ustring(views[v].view));
            }
            layout.parts[0].spec.attribute("multiView", OIIO::TypeDesc(OIIO::TypeDesc::STRING, (int)viewNames.size()), viewNames.data());
        }

        return layout;
    }
    for (std::size_t v = 0; v < views.size(); ++v) {
        if (split == ePartSplitViews) {
            std::vector<Member> members;
            for (std::size_t p = 0; p < views[v].planes.size(); ++p) {
                members.push_back(Member { v, p });
            }
            addPart(members, false);
            if (multiView) {
                layout.parts.back().spec.attribute("view", views[v].view);
            }
            continue;
        }
        for (std::size_t p = 0; p < views[v].planes.size(); ++p) {
            addPart(std::vector<Member>(1, Member { v, p }), false);
            OIIO::ImageSpec& spec = layout.parts.back().spec;
            if (multiView) {
                spec.attribute("view", views[v].view);
            }
            // OpenEXR wants every part of a file named uniquely, and a reader that finds no name
            // takes the synthesised one for the layer of the colour part.
            const std::string& label = views[v].planes[p].label;
            if (!label.empty()) {
                spec.attribute("oiio:subimagename", multiView ? label + "." + views[v].view : label);
            }
        }
    }

    return layout;
}
} // anonymous namespace

NativeWrite::NativeWrite(NodePtr node)
    : NativeEffectBase(node)
{
}

void
NativeWrite::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobOutputFilePtr filename = createKnob<KnobOutputFile>(tr("File"));
    filename->setName(kOfxImageEffectFileParamName);
    filename->setAsOutputImageFile();
    filename->setHintToolTip(tr("The image or image sequence to write."));
    filename->setIsMetadataSlave(true);
    page->addKnob(filename);
    _filename = filename;

    KnobBoolPtr overwrite = createKnob<KnobBool>(tr("Overwrite"));
    overwrite->setName(kNatronWriteParamOverwrite);
    overwrite->setHintToolTip(tr("Overwrite existing files when rendering."));
    overwrite->setDefaultValue(true);
    overwrite->setAnimationEnabled(false);
    page->addKnob(overwrite);
    _overwrite = overwrite;

    KnobChoicePtr formatType = createKnob<KnobChoice>(tr("Format Type"));
    formatType->setName(kNatronWriteParamFormatType);
    formatType->setHintToolTip(tr("Determines which rectangle of pixels will be written in output."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNatronWriteFormatTypeInput, tr("Input Format").toStdString(), tr("Renders the pixels included in the input format").toStdString()));
        options.push_back(ChoiceOption(kNatronWriteFormatTypeProject, tr("Project Format").toStdString(), tr("Renders the pixels included in the project format").toStdString()));
        options.push_back(ChoiceOption(kNatronWriteFormatTypeFixed, tr("Fixed Format").toStdString(), tr("Renders the pixels included in the format indicated by the Format parameter.").toStdString()));
        formatType->populateChoices(options);
    }
    formatType->setDefaultValue(eFormatTypeProject);
    formatType->setAnimationEnabled(false);
    formatType->setAddNewLine(false);
    page->addKnob(formatType);
    _formatType = formatType;

    const ExtentKnobs::FormatKnobSet formatKnobs = ExtentKnobs::createFormatKnobs(this, page);
    _format = formatKnobs.choice;
    _formatSize = formatKnobs.size;
    _formatPar = formatKnobs.par;
    formatKnobs.choice->setSecretByDefault(true);

    KnobBoolPtr clipToRoD = createKnob<KnobBool>(tr("Clip To RoD"));
    clipToRoD->setName(kNatronWriteParamClipToRoD);
    clipToRoD->setHintToolTip(tr("When checked, the portion of the image written will be the region of definition of the image in input and not the format selected by the Format Type parameter.\n"
                                 "For the EXR file format, this will distinguish the data window (size of the image in input) from the display window (the format specified by Format Type)."));
    clipToRoD->setDefaultValue(true);
    clipToRoD->setAnimationEnabled(false);
    page->addKnob(clipToRoD);
    _clipToRoD = clipToRoD;

    KnobChoicePtr frameRange = createKnob<KnobChoice>(tr("Frame Range"));
    frameRange->setName(kNatronWriteParamFrameRange);
    frameRange->setHintToolTip(tr("What frame range should be rendered."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNatronWriteFrameRangeUnion, tr("Union of input ranges").toStdString(), tr("The union of all inputs frame ranges will be rendered.").toStdString()));
        options.push_back(ChoiceOption(kNatronWriteFrameRangeProject, tr("Project frame range").toStdString(), tr("The frame range delimited by the frame range of the project will be rendered.").toStdString()));
        options.push_back(ChoiceOption(kNatronWriteFrameRangeManual, tr("Manual").toStdString(), tr("The frame range will be the one defined by the first frame and last frame parameters.").toStdString()));
        frameRange->populateChoices(options);
    }
    frameRange->setDefaultValue(eFrameRangeProject);
    frameRange->setAnimationEnabled(false);
    page->addKnob(frameRange);
    _frameRange = frameRange;

    KnobIntPtr firstFrame = createKnob<KnobInt>(tr("First Frame"));
    firstFrame->setName(kNatronWriteParamFirstFrame);
    firstFrame->setHintToolTip(tr("The first frame to render, when the frame range is Manual."));
    firstFrame->setDefaultValue(0);
    firstFrame->setAnimationEnabled(false);
    firstFrame->disableSlider();
    firstFrame->setAddNewLine(false);
    page->addKnob(firstFrame);
    _firstFrame = firstFrame;

    KnobIntPtr lastFrame = createKnob<KnobInt>(tr("Last Frame"));
    lastFrame->setName(kNatronWriteParamLastFrame);
    lastFrame->setHintToolTip(tr("The last frame to render, when the frame range is Manual."));
    lastFrame->setDefaultValue(0);
    lastFrame->setAnimationEnabled(false);
    lastFrame->disableSlider();
    page->addKnob(lastFrame);
    _lastFrame = lastFrame;

    // Node.cpp shows this knob's value, in parentheses, next to the node's label; it follows the
    // file name there.
    KnobStringPtr sublabel = createKnob<KnobString>(tr("SubLabel"));
    sublabel->setName(kNatronOfxParamStringSublabelName);
    sublabel->setSecretByDefault(true);
    sublabel->setAnimationEnabled(false);
    sublabel->setEvaluateOnChange(false);
    page->addKnob(sublabel);

    KnobIntPtr frameIncr = createKnob<KnobInt>(tr(kNatronWriteParamFrameStepLabel));
    frameIncr->setName(kNatronWriteParamFrameStep);
    frameIncr->setHintToolTip(tr(kNatronWriteParamFrameStepHint));
    frameIncr->setAnimationEnabled(false);
    frameIncr->setMinimum(1);
    frameIncr->setDefaultValue(1);
    page->addKnob(frameIncr);

    OcioSpaceKnobs::Description outputSpace;
    outputSpace.prefix = kNatronWriteParamOCIOOutputSpace;
    outputSpace.label = tr("Output Colorspace");
    outputSpace.spaceHint = tr("The OpenColorIO colorspace of the file. The image is converted to it from the project's "
                               "working space. It is shown only when the project's config has no colorspace of that name.");
    outputSpace.menuHint = tr("The OpenColorIO colorspace of the file. The image is converted to it from the project's "
                              "working space. A new Write takes the colorspace the config's file rules give the file name, "
                              "else the project's default for files of its kind. The config's default rule does not count. "
                              "Reset to default returns to that colorspace.");
    outputSpace.setLabel = tr("Output Colorspace Set");
    _outputSpace.create(this, page, outputSpace, [this]() { return automaticOutputSpace(); });

    KnobChoicePtr bitDepth = createKnob<KnobChoice>(tr("Bit Depth"));
    bitDepth->setName(kNatronWriteParamBitDepth);
    bitDepth->setHintToolTip(tr("Number of bits per sample in the file. Auto picks the depth from the output format."));
    bitDepth->populateChoices(bitDepthOptions(allDataTypes()));
    bitDepth->setDefaultValue(0);
    bitDepth->setAnimationEnabled(false);
    page->addKnob(bitDepth);
    _bitDepth = bitDepth;

    KnobChoicePtr compression = createKnob<KnobChoice>(tr("Compression"));
    compression->setName(kNatronWriteParamCompression);
    compression->setHintToolTip(tr("The compression of the file, for the formats that have a choice. Default lets the format decide."));
    compression->populateChoices(allCompressionOptions());
    compression->setDefaultValue(0);
    compression->setAnimationEnabled(false);
    page->addKnob(compression);
    _compression = compression;

    KnobIntPtr quality = createKnob<KnobInt>(tr("Quality"));
    quality->setName(kNatronWriteParamQuality);
    quality->setHintToolTip(tr("Indicates the quality of compression to use (0-100), for those plugins and compression methods that allow a variable amount of compression, with higher numbers indicating higher image fidelity. [JPEG, TIFF w/ JPEG comp., WEBP]"));
    quality->setMinimum(0);
    quality->setMaximum(100);
    quality->setDisplayMinimum(0);
    quality->setDisplayMaximum(100);
    quality->setDefaultValue(100);
    quality->setAnimationEnabled(false);
    page->addKnob(quality);
    _quality = quality;

    KnobDoublePtr dwa = createKnob<KnobDouble>(tr("DWA Compression Level"));
    dwa->setName(kNatronWriteParamDWACompressionLevel);
    dwa->setHintToolTip(tr("Amount of compression when using Dreamworks DWAA or DWAB compression options. These lossy formats are variable in quality and can minimize the compression artifacts. Higher values will result in greater compression and likewise smaller file size, but increases the chance for artifacts. Values from 45 to 150 are usually correct for production shots, whereas HDR vacation photos could use up to 500. Values below 45 should give no visible improvement on photographs. [EXR w/ DWAa or DWAb comp.]"));
    dwa->setMinimum(0.);
    dwa->setMaximum(100000.);
    dwa->setDisplayMinimum(0.);
    dwa->setDisplayMaximum(500.);
    dwa->setDefaultValue(45.);
    dwa->setAnimationEnabled(false);
    page->addKnob(dwa);
    _dwaCompressionLevel = dwa;

    KnobIntPtr zip = createKnob<KnobInt>(tr("ZIP Compression Level"));
    zip->setName(kNatronWriteParamZIPCompressionLevel);
    zip->setHintToolTip(tr("Compression level for zip/deflate compression, on a scale from 1 (fastest, minimal compression) to 9 (slowest, maximal compression) [EXR, TIFF or Zfile w/ zip or zips comp.]"));
    zip->setMinimum(1);
    zip->setMaximum(9);
    zip->setDisplayMinimum(1);
    zip->setDisplayMaximum(9);
    zip->setDefaultValue(4);
    zip->setAnimationEnabled(false);
    page->addKnob(zip);
    _zipCompressionLevel = zip;

    KnobChoicePtr tileSize = createKnob<KnobChoice>(tr("Tile Size"));
    tileSize->setName(kNatronWriteParamTileSize);
    tileSize->setHintToolTip(tr("Size of a tile in the output file for formats that support tiles. If scan-line based, the whole image will have a single tile."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("0", tr("Scan-Line Based").toStdString(), std::string()));
        options.push_back(ChoiceOption("64"));
        options.push_back(ChoiceOption("128"));
        options.push_back(ChoiceOption("256"));
        options.push_back(ChoiceOption("512"));
        tileSize->populateChoices(options);
    }
    tileSize->setDefaultValue(0);
    tileSize->setAnimationEnabled(false);
    page->addKnob(tileSize);
    _tileSize = tileSize;

    KnobChoicePtr partSplitting = createKnob<KnobChoice>(tr("Parts"));
    partSplitting->setName(kNatronWriteParamPartSplitting);
    partSplitting->setHintToolTip(tr("Defines whether to separate views and layers in different EXR parts or not. Multi-part files can be opened only with applications supporting OpenEXR 2."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNatronWritePartSplittingSingle, tr("Single Part").toStdString(), tr("All views and layers will be in the same part, ensuring compatibility with OpenEXR 1.x").toStdString()));
        options.push_back(ChoiceOption(kNatronWritePartSplittingViews, tr("Split Views").toStdString(), tr("All views will have their own part, and each part will contain all layers.").toStdString()));
        options.push_back(ChoiceOption(kNatronWritePartSplittingViewsLayers, tr("Split Views,Layers").toStdString(), tr("Each layer of each view will have its own part, which makes for the fastest decoding.").toStdString()));
        partSplitting->populateChoices(options);
    }
    partSplitting->setDefaultValue(0);
    partSplitting->setAnimationEnabled(false);
    page->addKnob(partSplitting);
    _partSplitting = partSplitting;

    // Its script name and its first entry are what OutputEffectInstance and Node look for.
    KnobChoicePtr views = createKnob<KnobChoice>(tr("Views"));
    views->setName(kNatronWriteParamViewsSelector);
    views->setHintToolTip(tr("Select the views to render. When choosing All, make sure the output filename does not have a %v or %V view pattern in which case each view would be written to a separate file."));
    views->populateChoices(std::vector<ChoiceOption>(1, ChoiceOption(kNatronWriteViewsAll)));
    views->setDefaultValue(0);
    views->setAnimationEnabled(false);
    page->addKnob(views);
    _views = views;

    KnobBoolPtr writeMetadata = createKnob<KnobBool>(tr("Write Metadata"));
    writeMetadata->setName(kNatronWriteParamWriteMetadata);
    writeMetadata->setHintToolTip(tr("When checked, the metadata of the input image (EXR attributes, EXIF fields, timecode and so on) is written to the file, as far as the file format can hold it."));
    writeMetadata->setDefaultValue(true);
    writeMetadata->setAnimationEnabled(false);
    page->addKnob(writeMetadata);
    _writeMetadata = writeMetadata;

    KnobBoolPtr readBack = createKnob<KnobBool>(tr(kNatronWriteParamReadBackLabel));
    readBack->setName(kNatronWriteParamReadBack);
    readBack->setHintToolTip(tr(kNatronWriteParamReadBackHint));
    readBack->setDefaultValue(false);
    readBack->setAnimationEnabled(false);
    readBack->setIsMetadataSlave(true);
    readBack->setAddNewLine(false);
    page->addKnob(readBack);
    _readBack = readBack;

    // Left to trigger a render: the evaluation retires the images and action results cached under
    // the old node hash, and the metadata is derived again from the files now on disk.
    KnobButtonPtr reload = createKnob<KnobButton>(tr("Reload"));
    reload->setName(kNatronWriteParamReload);
    reload->setHintToolTip(tr("Reads the written files again: forgets their headers and layers and the images read from "
                              "them, so a file changed on disk is what the node outputs. Only while Read back file is on."));
    reload->setIsMetadataSlave(true);
    reload->setAllDimensionsEnabled(false);
    page->addKnob(reload);
    _reload = reload;

    RenderEnginePtr engine = getRenderEngine();
    if (engine) {
        QObject::connect(engine.get(), &RenderEngine::renderFinished, this, [this](int) { onSequenceRenderFinished(); });
    }
    ProjectPtr project = getApp()->getProject();
    if (project) {
        QObject::connect(project.get(), &Project::projectViewsChanged, this, [this]() {
            refreshViewsMenu();
            _viewToKeep.clear();
        });
    }

    refreshOptions();
}

void
NativeWrite::onKnobsLoaded()
{
    _outputSpace.refreshMenu();
    // The project's views may load after this node's knobs, so a saved view missing from the menu now stays chosen until they arrive.
    KnobChoicePtr views = _views.lock();
    const std::string saved = views ? views->getActiveEntry().id : std::string();
    _viewToKeep = saved == kNatronWriteViewsAll ? std::string() : saved;
    refreshOptions();
}

std::optional<std::string>
NativeWrite::automaticOutputSpace() const
{
    KnobOutputFilePtr filename = _filename.lock();
    const std::string path = filename ? filename->getValue() : std::string();

    // A Write without a file converts nothing.
    if (path.empty()) {
        return std::string();
    }
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    const std::string extension = extensionOf(path);
    const std::string formatName = OiioWriteSupport::formatNameForWritableExtension(extension);
    if (!project || formatName.empty()) {
        return std::nullopt;
    }
    const OiioWriteSupport::DataType type = resolveDataType(_bitDepth.lock(), formatName, extension);
    const std::string space = IoColorSpace::defaultOutputSpace(*project, path, colorCategoryOf(type, extension));
    if (space.empty()) {
        return std::nullopt;
    }

    return space;
}

StatusEnum
NativeWrite::getPreferredMetadataAt(double time,
                                    ViewIdx view,
                                    NodeMetadata& metadata)
{
    KnobOutputFilePtr filename = _filename.lock();
    if (!filename) {
        return eStatusOK;
    }
    // The file's format when this frame and view are read back from disk.
    std::string path;
    const std::shared_ptr<const OiioReadSupport::Header> header = readBackFrame(time, view, &path);
    if (!header) {
        // An RGBA input whose file gets no alpha, because the format has none or the channel set
        // leaves it out, makes an RGB output: the alpha is not in the file, so not in the output.
        if ((metadata.getComponentsType(-1) == kNatronColorLayerID) && (metadata.getNComps(-1) == 4) && getNode()->getLayerKnob()) {
            const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
            ResolvedLayer colour;
            colour.desc = rgba;
            getColorWriteBits(rgba, &colour.channels);
            if (colour.channels.any()) {
                const FilePlan plan = planFile(std::vector<ImageLayerDesc>(1, rgba), std::vector<ResolvedLayer>(1, colour), true, outputCaps(filename->getValue()));
                if ((plan.channels.size() == 1) && (plan.channels[0].names.size() == 3)) {
                    metadata.setNComps(-1, 3);
                }
            }
        }

        return eStatusOK;
    }
    OiioReadSupport::PartChannels colour;
    const bool hasColour = OiioReadSupport::colourPartOf(*header, viewNameOf(getApp()->getProject()->getProjectViewNames(), view), &colour);
    const OIIO::ImageSpec& spec = header->subimages[hasColour ? (std::size_t)colour.subimage : 0];

    // OpenFX formats must start at (0, 0), so a positive display-window origin only widens the
    // format. Mirroring the display window within itself top to bottom always lands it at
    // [0, full_height), which is why full_y drops out.
    metadata.setOutputFormat(RectI(0, 0, spec.full_x + spec.full_width, spec.full_height));
    metadata.setPixelAspectRatio(-1, OiioReadSupport::pixelAspectOf(spec));
    if (hasColour) {
        metadata.setNComps(-1, OiioReadSupport::ColourChannels::of(colour.names, colour.index).nComps());
    }

    return eStatusOK;
}

void
NativeWrite::onCurrentFrameMetadataChanged(const NodeMetadata& metadata)
{
    NativeEffectBase::onCurrentFrameMetadataChanged(metadata);
    refreshViewsMenu();
}

void
NativeWrite::refreshViewsMenu()
{
    std::vector<ChoiceOption> options(1, ChoiceOption(kNatronWriteViewsAll));
    const std::vector<std::string> names = getApp()->getProject()->getProjectViewNames();

    for (std::size_t i = 0; i < names.size(); ++i) {
        options.push_back(ChoiceOption(names[i]));
    }
    if (!_viewToKeep.empty()) {
        if (std::find(names.begin(), names.end(), _viewToKeep) == names.end()) {
            options.push_back(ChoiceOption(_viewToKeep));
        } else {
            _viewToKeep.clear();
        }
    }
    setMenu(_views.lock(), options);
}

std::vector<ViewIdx>
NativeWrite::viewsToWrite(ViewIdx view) const
{
    const std::vector<ViewIdx> own(1, view);
    KnobOutputFilePtr filename = _filename.lock();
    KnobChoicePtr views = _views.lock();

    if (!filename || !views) {
        return own;
    }
    const std::string pattern = filename->getValue();
    const int nViews = getApp()->getProject()->getProjectViewsCount();
    if (hasViewToken(pattern) || nViews < 2 || views->getValue() != 0 || !OiioWriteSupport::paramVisibility(OiioWriteSupport::formatNameForWritableExtension(extensionOf(pattern)), std::string()).views) {
        return own;
    }
    // One render of view 0 stands for the whole file, so the other views' renders have nothing to write.
    if (view.value() != 0) {
        return std::vector<ViewIdx>();
    }
    std::vector<ViewIdx> all;
    for (int v = 0; v < nViews; ++v) {
        all.push_back(ViewIdx(v));
    }

    return all;
}

FramesNeededMap
NativeWrite::getFramesNeeded(double time,
                             ViewIdx view)
{
    std::string path;
    if (readBackFrame(time, view, &path)) {
        return FramesNeededMap();
    }
    FramesNeededMap ret = NativeEffectBase::getFramesNeeded(time, view);
    const std::vector<ViewIdx> views = viewsToWrite(view);
    FramesNeededMap::iterator input = ret.find(0);

    if ((views.size() > 1) && (input != ret.end())) {
        RangeD range;
        range.min = range.max = time;
        for (std::size_t v = 0; v < views.size(); ++v) {
            input->second[views[v]] = std::vector<RangeD>(1, range);
        }
    }

    return ret;
}

void
NativeWrite::getComponentsNeededAndProduced(double time,
                                            ViewIdx view,
                                            EffectInstance::ComponentsNeededMap* comps,
                                            double* passThroughTime,
                                            int* passThroughView,
                                            int* passThroughInputNb)
{
    EffectInstance::getComponentsNeededAndProduced(time, view, comps, passThroughTime, passThroughView, passThroughInputNb);

    std::list<ImageLayerDesc>& produced = (*comps)[-1];
    std::string path;
    const std::shared_ptr<const OiioReadSupport::Header> header = readBackFrame(time, view, &path);
    if (header) {
        for (EffectInstance::ComponentsNeededMap::iterator it = comps->begin(); it != comps->end(); ++it) {
            if (it->first >= 0) {
                it->second.clear();
            }
        }
        produced = readBackLayers(*header, view);

        return;
    }
    const std::vector<ImageLayerDesc> candidates(produced.begin(), produced.end());
    std::vector<ResolvedLayer> selected;
    const bool hasLayerKnob = getNode()->resolveLayerKnob(time, view, &selected);
    const KnobOutputFilePtr filename = _filename.lock();
    const FilePlan plan = planFile(candidates, selected, hasLayerKnob, outputCaps(filename ? filename->getValue() : std::string()));

    produced.assign(plan.outputs.begin(), plan.outputs.end());
}

std::vector<ImageLayerDesc>
NativeWrite::inputPlanes(double time,
                         ViewIdx view)
{
    EffectInstance::ComponentsNeededMap comps;
    double passThroughTime = time;
    int passThroughView = view.value();
    int passThroughInputNb = -1;

    EffectInstance::getComponentsNeededAndProduced(time, view, &comps, &passThroughTime, &passThroughView, &passThroughInputNb);
    const std::list<ImageLayerDesc>& planes = comps[-1];

    return std::vector<ImageLayerDesc>(planes.begin(), planes.end());
}

std::list<ImageLayerDesc>
NativeWrite::readBackLayers(const OiioReadSupport::Header& header,
                            ViewIdx view) const
{
    std::list<ImageLayerDesc> layers;
    ImageLayerDesc colourLayer;
    ImageLayerDesc pairedLayer;
    getMetadataComponents(-1, &colourLayer, &pairedLayer);

    const std::string viewName = viewNameOf(getApp()->getProject()->getProjectViewNames(), view);
    OiioReadSupport::PartChannels colourPart;
    if (OiioReadSupport::colourPartOf(header, viewName, &colourPart)) {
        layers.push_back(colourLayer.isColorLayer() ? colourLayer : ImageLayerDesc::mapNCompsToColorLayer(OiioReadSupport::ColourChannels::of(colourPart.names, colourPart.index).nComps()));
    }
    std::vector<OiioReadSupport::FileLayer> fileLayers;
    OiioReadSupport::fileLayers(header, &fileLayers, viewName);
    for (std::size_t i = 0; i < fileLayers.size(); ++i) {
        layers.push_back(fileLayers[i].desc);
    }

    return layers;
}

StatusEnum
NativeWrite::checkFrameToWrite(double time,
                               ViewIdx view)
{
    KnobOutputFilePtr filename = _filename.lock();
    KnobBoolPtr overwrite = _overwrite.lock();
    const std::string pattern = filename ? filename->getValue() : std::string();
    const std::string formatName = pattern.empty() ? std::string() : OiioWriteSupport::formatNameForWritableExtension(extensionOf(pattern));

    // A missing file name or an unsupported format is reported by the render action.
    if (formatName.empty() || !overwrite) {
        return eStatusOK;
    }
    if (!overwrite->getValue()) {
        std::error_code ec;
        const std::string path = filename->generateFileNameAtTime((SequenceTime)std::floor(time + 0.5), view).toStdString();
        if (std::filesystem::exists(std::filesystem::path(path), ec)) {
            return eStatusOK;
        }
    }
    std::vector<ResolvedLayer> selected;
    const bool hasLayerKnob = getNode()->resolveLayerKnob(time, view, &selected);
    const FilePlan plan = planFile(inputPlanes(time, view), selected, hasLayerKnob, OiioWriteSupport::capabilities(formatName));

    if (plan.planes.empty() && !plan.dropped.empty()) {
        setPersistentMessage(eMessageTypeError, tr("%1 can hold only the colour channels, and there are none to write: %2 cannot be written").arg(QString::fromStdString(formatName), QString::fromStdString(droppedLayerNames(plan))).toStdString());

        return eStatusFailed;
    }

    return eStatusOK;
}

void
NativeWrite::onEffectCreated(bool mayCreateFileDialog,
                             const CreateNodeArgs& /*args*/)
{
    // The host's hint says unselected channels pass through, which they do not here.
    const KnobIPtr channels = getNode()->getLayerKnob();
    if (channels) {
        channels->setHintToolTip(tr("The layers and channels this node writes to the file. The node's output, to the Viewer "
                                    "and to every node downstream, is what the file holds: a partial R/G/B selection writes "
                                    "and shows the whole RGB, a channel or layer the file format cannot hold is neither "
                                    "written nor shown, and no other layer passes through."));
    }

    KnobOutputFilePtr filename = _filename.lock();

    if (filename && !filename->getValue().empty()) {
        rejectUnsupportedFormat(filename->getValue());

        return;
    }
    if (!mayCreateFileDialog || !filename || getApp()->isBackground()
        || !appPTR->getCurrentSettings()->isFileDialogEnabledForNewWriters()) {
        return;
    }
    const std::string path = getApp()->saveImageFileDialog();
    if (path.empty()) {
        throw std::runtime_error("");
    }
    filename->setValue(path);
}

bool
NativeWrite::readBackAvailable() const
{
    const KnobOutputFilePtr filename = _filename.lock();
    const std::string pattern = filename ? filename->getValue() : std::string();

    return !pattern.empty() && OiioWriteSupport::formatNameForWritableExtension(extensionOf(pattern)) == "openexr";
}

bool
NativeWrite::readBackActive() const
{
    const KnobBoolPtr readBack = _readBack.lock();

    return readBack && readBack->getValue() && readBackAvailable() && !isDoingSequentialRender();
}

std::string
NativeWrite::outputHashText(U64 hash)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);

    return std::string(text);
}

std::shared_ptr<const OiioReadSupport::Header>
NativeWrite::readBackFrame(double time,
                           ViewIdx view,
                           std::string* path) const
{
    path->clear();
    if (!readBackActive()) {
        return std::shared_ptr<const OiioReadSupport::Header>();
    }
    const KnobOutputFilePtr filename = _filename.lock();
    const std::string candidate = filename ? filename->generateFileNameAtTime((SequenceTime)std::floor(time + 0.5), view).toStdString() : std::string();
    if (candidate.empty()) {
        return std::shared_ptr<const OiioReadSupport::Header>();
    }
    const ReadBackFile file = readBackFile(candidate);
    if (!file.header || (file.storedHash != outputHashText(getOutputContentHash()))) {
        return std::shared_ptr<const OiioReadSupport::Header>();
    }
    *path = candidate;

    return file.header;
}

NativeWrite::ReadBackFile
NativeWrite::readBackFile(const std::string& path) const
{
    std::uint64_t purges = 0;
    {
        std::lock_guard<std::mutex> lock(_readBackFilesMutex);
        const std::map<std::string, ReadBackFile>::const_iterator it = _readBackFiles.find(path);
        if (it != _readBackFiles.end()) {
            return it->second;
        }
        purges = _readBackFilesPurges;
    }

    ReadBackFile file;
    std::error_code ec;
    if (std::filesystem::is_regular_file(std::filesystem::path(path), ec)) {
        std::string error;
        const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
        if (header && !header->subimages.empty()) {
            file.header = header;
            // Every part of the file carries the attribute, so the first one speaks for the file.
            file.storedHash = header->subimages[0].get_string_attribute(kNatronWriteOutputHashAttribute);
        }
    }

    std::lock_guard<std::mutex> lock(_readBackFilesMutex);
    // A purge during the read may have been for a file rewritten after it was looked at.
    if (purges == _readBackFilesPurges) {
        if ((_readBackFiles.size() >= kMaxReadBackFiles) && (_readBackFiles.find(path) == _readBackFiles.end())) {
            _readBackFiles.clear();
        }
        _readBackFiles[path] = file;
    }

    return file;
}

U64
NativeWrite::getOutputContentHash() const
{
    const KnobBoolPtr readBack = _readBack.lock();
    std::vector<std::pair<std::string, U64>> knobs;
    bool hasExpression = false;
    // Read by the render threads, which write this hash into the file.
    const KnobsVec all = getKnobs_mt_safe();
    for (KnobsVec::const_iterator it = all.begin(); it != all.end(); ++it) {
        const KnobIPtr& knob = *it;
        if (!knob || !knob->isRenderHashed() || (knob == readBack)) {
            continue;
        }
        bool knobHasExpression = false;
        bool readsOutsideKnob = false;
        knob->getContentHashDependencies(&knobHasExpression, &readsOutsideKnob);
        hasExpression = hasExpression || knobHasExpression;
        knobs.push_back(std::make_pair(knob->getName(), knob->getContentHash()));
    }
    std::sort(knobs.begin(), knobs.end());

    ContentHasher h;
    h.appendString("natron:writeOutput");
    h.appendString(getPluginID());
    const Plugin* plugin = getNode()->getPlugin();
    h.appendBool(plugin != NULL);
    if (plugin) {
        h.appendI64(plugin->getMajorVersion());
        h.appendI64(plugin->getMinorVersion());
        h.appendU64(plugin->getBinaryContentStamp());
    }
    h.appendU64(knobs.size());
    for (std::size_t i = 0; i < knobs.size(); ++i) {
        h.appendString(knobs[i].first);
        h.appendU64(knobs[i].second);
    }
    h.appendBool(hasExpression);
    if (hasExpression) {
        h.appendString(getNode()->getFullyQualifiedName());
    }
    const EffectInstancePtr input = getInput(0);
    h.appendU64(input ? input->getRenderHash() : 0);
    ProjectPtr project = getApp()->getProject();
    if (hasExpression || dependsOnProjectFrameRange()) {
        double first = 0., last = 0.;
        project->getFrameRange(&first, &last);
        h.appendDouble(first);
        h.appendDouble(last);
    }
    h.appendU64(project->getContentDigest());

    return h.finalize();
} // NativeWrite::getOutputContentHash

bool
NativeWrite::isInputOptional(int inputNb) const
{
    return readBackActive() || NativeEffectBase::isInputOptional(inputNb);
}

bool
NativeWrite::outputFollowsLayerKnob(double time,
                                    ViewIdx view) const
{
    std::string path;

    return !readBackFrame(time, view, &path);
}

void
NativeWrite::refreshReadBackState()
{
    const KnobBoolPtr readBack = _readBack.lock();
    const KnobButtonPtr reload = _reload.lock();
    const KnobIPtr button = getKnobByName(kNatronWriteParamStartRender);
    const bool available = readBackAvailable();
    const bool on = available && readBack && readBack->getValue();

    setSecretAndDisabled(readBack, !available);
    if (reload) {
        reload->setSecret(!available);
        reload->setAllDimensionsEnabled(on);
    }
    if (button) {
        button->setAllDimensionsEnabled(!on);
    }
}

void
NativeWrite::purgeCaches()
{
    {
        std::lock_guard<std::mutex> lock(_readBackFilesMutex);
        _readBackFiles.clear();
        ++_readBackFilesPurges;
    }
    // The shared header cache parses a file again when its time or size changed; this also
    // catches a file rewritten within one tick of the clock at the same size.
    const KnobOutputFilePtr filename = _filename.lock();
    double first = 0.;
    double last = 0.;
    getFrameRange(&first, &last);
    if (filename && !filename->getValue().empty() && (last - first < kMaxEvictedFrames)) {
        const int nViews = std::max(1, getApp()->getProject()->getProjectViewsCount());
        std::vector<std::string> paths;
        for (double frame = std::floor(first); frame <= last; frame += 1.) {
            for (int v = 0; v < nViews; ++v) {
                paths.push_back(filename->generateFileNameAtTime((SequenceTime)frame, ViewIdx(v)).toStdString());
            }
        }
        OiioReadSupport::evictHeaders(paths);
    }
    invalidateOutputMetadata();
    clearActionsCache();
    // A file read or rewritten under an unchanged hash makes the node show other pixels than the
    // images cached under that hash hold.
    NodePtr node = getNode();
    node->removeAllImagesFromCache(false);

    // Neither do the hashes downstream change, yet what those nodes cached was derived from what
    // this one showed before: the layers they pass through and the images they rendered.
    std::unordered_set<const Node*> visited;
    NodesList toVisit;
    node->getOutputsWithGroupRedirection(toVisit);
    while (!toVisit.empty()) {
        NodePtr output = toVisit.front();
        toVisit.pop_front();
        if (!output || !visited.insert(output.get()).second) {
            continue;
        }
        EffectInstancePtr effect = output->getEffectInstance();
        if (effect) {
            effect->clearActionsCache();
        }
        output->removeAllImagesFromCache(false);
        output->getOutputsWithGroupRedirection(toVisit);
    }
}

void
NativeWrite::onSequenceRenderFinished()
{
    // Also while read back is off: what was cached while it was on no longer matches the files.
    purgeCaches();
}

bool
NativeWrite::rejectUnsupportedFormat(const std::string& path)
{
    if (OiioWriteSupport::isWritablePath(path)) {
        return false;
    }
    setPersistentMessage(eMessageTypeError, tr("Unsupported format: %1").arg(QString::fromStdString(path)).toStdString());

    return true;
}

void
NativeWrite::refreshOptions()
{
    KnobOutputFilePtr filename = _filename.lock();
    KnobChoicePtr compression = _compression.lock();
    KnobChoicePtr bitDepth = _bitDepth.lock();
    KnobChoicePtr formatType = _formatType.lock();

    if (!filename || !compression || !bitDepth || !formatType) {
        return;
    }
    const std::string path = filename->getValue();
    const std::string formatName = path.empty() ? std::string() : OiioWriteSupport::formatNameForWritableExtension(extensionOf(path));
    const OiioWriteSupport::FormatCaps caps = formatName.empty() ? OiioWriteSupport::FormatCaps() : OiioWriteSupport::capabilities(formatName);

    if (formatName.empty()) {
        setMenu(compression, allCompressionOptions());
        setMenu(bitDepth, bitDepthOptions(allDataTypes()));
    } else {
        setMenu(compression, compressionOptions(formatName));
        setMenu(bitDepth, bitDepthOptions(OiioWriteSupport::validDataTypes(formatName)));
    }
    const OiioWriteSupport::ParamVisibility visible = OiioWriteSupport::paramVisibility(formatName, activeID(compression));

    setSecretAndDisabled(compression, !visible.compression);
    setSecretAndDisabled(bitDepth, formatName.empty());
    setSecretAndDisabled(_quality.lock(), !visible.quality);
    setSecretAndDisabled(_dwaCompressionLevel.lock(), !visible.dwaCompressionLevel);
    setSecretAndDisabled(_zipCompressionLevel.lock(), !visible.zipCompressionLevel);
    setSecretAndDisabled(_tileSize.lock(), !visible.tileSize);
    setSecretAndDisabled(_partSplitting.lock(), !visible.views);
    setSecretAndDisabled(_views.lock(), !visible.views || hasViewToken(path));
    setSecretAndDisabled(_clipToRoD.lock(), !caps.displayWindow);
    setSecretAndDisabled(_format.lock(), formatType->getValue() != eFormatTypeFixed);
    setSecretAndDisabled(_formatSize.lock(), true);
    setSecretAndDisabled(_formatPar.lock(), true);

    refreshViewsMenu();

    KnobChoicePtr frameRange = _frameRange.lock();
    const bool manual = frameRange && frameRange->getValue() == eFrameRangeManual;
    setSecretAndDisabled(_firstFrame.lock(), !manual);
    setSecretAndDisabled(_lastFrame.lock(), !manual);
    refreshReadBackState();
}

void
NativeWrite::getFrameRange(double* first,
                           double* last)
{
    KnobChoicePtr frameRange = _frameRange.lock();
    KnobIntPtr firstFrame = _firstFrame.lock();
    KnobIntPtr lastFrame = _lastFrame.lock();
    const int choice = frameRange ? frameRange->getValue() : (int)eFrameRangeProject;

    if ((choice == eFrameRangeManual) && firstFrame && lastFrame) {
        *first = firstFrame->getValue();
        *last = lastFrame->getValue();
    } else if (choice == eFrameRangeUnion) {
        EffectInstance::getFrameRange(first, last);
    } else {
        getApp()->getFrameRange(first, last);
    }
}

bool
NativeWrite::dependsOnProjectFrameRange() const
{
    KnobChoicePtr frameRange = _frameRange.lock();
    const int choice = frameRange ? frameRange->getValue() : (int)eFrameRangeProject;

    if ((choice == eFrameRangeManual) && _firstFrame.lock() && _lastFrame.lock()) {
        return false;
    }

    return choice != eFrameRangeUnion;
}

void
NativeWrite::seedFrame(const KnobIntPtr& knob,
                       bool isFirst,
                       bool force)
{
    if (!knob || (!force && (knob->getValue() != 0))) {
        return;
    }
    double first = 0.;
    double last = 0.;
    getApp()->getFrameRange(&first, &last);
    knob->setValue((int)(isFirst ? first : last));
}

bool
NativeWrite::knobChanged(KnobI* k,
                         ValueChangedReasonEnum reason,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    const KnobBoolPtr readBack = _readBack.lock();
    if (readBack && (k == readBack.get())) {
        refreshReadBackState();
        clearPersistentMessage(false);

        return true;
    }
    const KnobButtonPtr reload = _reload.lock();
    if (reload && (k == reload.get())) {
        purgeCaches();
        clearPersistentMessage(false);

        return true;
    }
    KnobOutputFilePtr filename = _filename.lock();
    KnobChoicePtr compression = _compression.lock();
    KnobChoicePtr formatType = _formatType.lock();
    KnobChoicePtr bitDepth = _bitDepth.lock();

    if (bitDepth && (k == bitDepth.get())) {
        _outputSpace.sourceChanged();

        return true;
    }
    if (_outputSpace.knobChanged(k, reason)) {
        return true;
    }
    if (filename && (k == filename.get())) {
        refreshOptions();
        _outputSpace.sourceChanged();
        const std::string path = filename->getValue();
        if (path.empty() || !rejectUnsupportedFormat(path)) {
            clearPersistentMessage(false);
        }

        return true;
    }
    if ((compression && (k == compression.get())) || (formatType && (k == formatType.get()))) {
        refreshOptions();

        return true;
    }
    KnobChoicePtr frameRange = _frameRange.lock();
    if (frameRange && (k == frameRange.get())) {
        if (frameRange->getValue() == eFrameRangeManual) {
            seedFrame(_firstFrame.lock(), true, false);
            seedFrame(_lastFrame.lock(), false, false);
        }
        refreshOptions();

        return true;
    }
    // Resetting a knob delivers the restore reason twice; seeding from the range sets the same value both times.
    if (reason == eValueChangedReasonRestoreDefault) {
        KnobIntPtr firstFrame = _firstFrame.lock();
        KnobIntPtr lastFrame = _lastFrame.lock();
        if (firstFrame && (k == firstFrame.get())) {
            seedFrame(firstFrame, true, true);

            return true;
        }
        if (lastFrame && (k == lastFrame.get())) {
            seedFrame(lastFrame, false, true);

            return true;
        }
    }

    return false;
}

void
NativeWrite::selectedFormat(double time,
                            ViewIdx view,
                            RectI* display,
                            double* par) const
{
    KnobChoicePtr formatType = _formatType.lock();
    const int type = formatType ? formatType->getValueAtTime(time, 0, view) : (int)eFormatTypeProject;

    if (type == eFormatTypeInput) {
        NodeMetadataConstPtr inputMetadata = getInputMetadataAt(0, time, view);
        if (inputMetadata) {
            const RectI inputFormat = inputMetadata->getOutputFormat();
            if ((inputFormat.x2 > inputFormat.x1) && (inputFormat.y2 > inputFormat.y1)) {
                *display = inputFormat;
                *par = inputMetadata->getPixelAspectRatio(-1);

                return;
            }
        }
    } else if (type == eFormatTypeFixed) {
        KnobIntPtr size = _formatSize.lock();
        KnobDoublePtr formatPar = _formatPar.lock();
        if (size && formatPar) {
            *display = RectI(0, 0, size->getValueAtTime(time, 0, view), size->getValueAtTime(time, 1, view));
            *par = formatPar->getValueAtTime(time, 0, view);

            return;
        }
    }
    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
    *display = projectFormat;
    *par = projectFormat.getPixelAspectRatio();
}

bool
NativeWrite::clipToRoDActive() const
{
    KnobBoolPtr clipToRoD = _clipToRoD.lock();

    return clipToRoD && !clipToRoD->getIsSecret() && clipToRoD->getValue();
}

bool
NativeWrite::resolveWindow(double time,
                           ViewIdx view,
                           Window* window) const
{
    selectedFormat(time, view, &window->display, &window->par);
    window->rod = window->display.toCanonical_noClipping(0, window->par);

    EffectInstancePtr input = getInput(0);
    if (input && clipToRoDActive()) {
        bool isProjectFormat = false;
        if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, &window->rod, &isProjectFormat) == eStatusFailed) {
            return false;
        }
        NodeMetadataConstPtr inputMetadata = input->getMetadata(time, view);
        window->par = inputMetadata ? inputMetadata->getPixelAspectRatio(-1) : 1.;
    }

    return true;
}

StatusEnum
NativeWrite::getRegionOfDefinition(U64 /*hash*/,
                                   double time,
                                   const RenderScale& /*scale*/,
                                   ViewIdx view,
                                   RectD* rod)
{
    std::string path;
    const std::shared_ptr<const OiioReadSupport::Header> header = readBackFrame(time, view, &path);
    if (header) {
        OiioReadSupport::PartChannels colour;
        const bool hasColour = OiioReadSupport::colourPartOf(*header, viewNameOf(getApp()->getProject()->getProjectViewNames(), view), &colour);
        const OIIO::ImageSpec& spec = header->subimages[hasColour ? (std::size_t)colour.subimage : 0];
        *rod = OiioReadSupport::dataWindowOf(spec).toCanonical_noClipping(0, OiioReadSupport::pixelAspectOf(spec));

        return eStatusOK;
    }
    Window window;

    if (!resolveWindow(time, view, &window)) {
        return eStatusFailed;
    }
    *rod = window.rod;

    return eStatusOK;
}

void
NativeWrite::getRegionsOfInterest(double time,
                                  const RenderScale& /*scale*/,
                                  const RectD& outputRoD,
                                  const RectD& /*renderWindow*/,
                                  ViewIdx view,
                                  RoIMap* ret)
{
    std::string path;
    if (readBackFrame(time, view, &path)) {
        return;
    }
    EffectInstancePtr input = getInput(0);

    if (input) {
        ret->insert(std::make_pair(input, outputRoD));
    }
}

StatusEnum
NativeWrite::renderReadBack(const RenderActionArgs& args,
                            const OiioReadSupport::Header& header,
                            const std::string& path)
{
    ProjectPtr project = getApp()->getProject();
    const std::string viewName = viewNameOf(project->getProjectViewNames(), args.view);
    std::vector<OiioReadSupport::FileLayer> fileLayers;
    OiioReadSupport::PartChannels colourPart;
    OiioReadSupport::fileLayers(header, &fileLayers, viewName);
    const bool hasColour = OiioReadSupport::colourPartOf(header, viewName, &colourPart);

    // The file's own tag or a file rule could name another space, but what the file holds is what this node wrote.
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr toWorking;
    {
        std::string conversionError;
        if (!IoColorSpace::processor(*project, _outputSpace.space(), _outputSpace.workingSpace(), _outputSpace.contextVariables(args.time), &toWorking, &conversionError)) {
            setPersistentMessage(eMessageTypeError, conversionError);

            return eStatusFailed;
        }
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(this);
    std::atomic<bool> failed(false);
    std::mutex errorMutex;
    std::string error;
    const unsigned int level = args.mappedScale.toMipmapLevel();

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        const int nComps = (int)image->getComponentsCount();

        RectI dataWindow;
        int subimage = 0;
        std::vector<int> fileChannels((std::size_t)nComps, -1);
        if (it->first.isColorLayer()) {
            if (hasColour) {
                subimage = colourPart.subimage;
                dataWindow = OiioReadSupport::dataWindowOf(header.subimages[(std::size_t)subimage]);
                const OiioReadSupport::ColourChannels colour = OiioReadSupport::ColourChannels::of(colourPart.names, colourPart.index);
                for (int c = 0; c < nComps; ++c) {
                    fileChannels[(std::size_t)c] = colour.fileChannelForComponent(nComps, c);
                }
            }
        } else {
            const std::vector<std::string>& wanted = image->getComponents().getChannels();
            for (std::vector<OiioReadSupport::FileLayer>::const_iterator layer = fileLayers.begin(); layer != fileLayers.end(); ++layer) {
                if (layer->desc.getLayerID() != it->first.getLayerID()) {
                    continue;
                }
                subimage = layer->subimage;
                dataWindow = OiioReadSupport::dataWindowOf(header.subimages[(std::size_t)subimage]);
                const std::vector<std::string>& present = layer->desc.getChannels();
                for (int c = 0; c < nComps && (std::size_t)c < wanted.size(); ++c) {
                    const std::vector<std::string>::const_iterator found = std::find(present.begin(), present.end(), wanted[(std::size_t)c]);
                    if (found != present.end()) {
                        fileChannels[(std::size_t)c] = layer->channels[(std::size_t)(found - present.begin())];
                    }
                }
                break;
            }
        }

        // At a coarser level the rows behind the window are decoded at full size, then reduced.
        ImagePtr full;
        const RectI fullRect = args.roi.upscalePowerOfTwo(level).intersect(dataWindow);
        Image* target = image.get();
        if (level > 0 && !fullRect.isNull()) {
            full = std::make_shared<Image>(image->getComponents(), image->getRoD(), fullRect, 0, image->getPixelAspectRatio(), eImageBitDepthFloat, image->getFieldingOrder(), false);
            target = full.get();
        }
        if (level > 0 || fullRect != args.roi) {
            Image::WriteAccess outAccess(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                std::fill_n((float*)outAccess.pixelAt(args.roi.x1, y), (std::size_t)args.roi.width() * nComps, 0.f);
            }
        }
        if (fullRect.isNull()) {
            continue;
        }

        ReadBackPlane plane;
        plane.target = target;
        plane.rect = fullRect;
        plane.path = path;
        plane.subimage = subimage;
        plane.fileChannels = fileChannels;
        if (it->first.isColorLayer() && (nComps == 3 || nComps == 4)) {
            plane.toWorking = toWorking;
        }
        const int rowsPerBand = std::max(1, kMinBandPixels / fullRect.width());
        const int nBands = (fullRect.height() + rowsPerBand - 1) / rowsPerBand;
        {
            Image::WriteAccess access(target);
            const std::function<void(int)> renderBand = [&](int band) {
                if (cancel.check() || failed.load()) {
                    return;
                }
                const int y1 = fullRect.y1 + band * rowsPerBand;
                const int y2 = std::min(y1 + rowsPerBand, fullRect.y2);
                std::string bandError;
                if (!decodeReadBackBand(plane, access, nComps, y1, y2, &bandError)) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!failed.exchange(true)) {
                        error = bandError;
                    }
                }
            };
            parallelForCancellable(nBands, nThreads, cancel, renderBand);
        }
        if (failed.load()) {
            setPersistentMessage(eMessageTypeError, error.empty() ? tr("Could not read %1").arg(QString::fromStdString(path)).toStdString() : error);

            return eStatusFailed;
        }
        if (full && !cancel.check()) {
            full->downscaleMipmap(image->getRoD(), fullRect, 0, level, false, image.get());
        }
    }
    clearPersistentMessage(false);

    return eStatusOK;
}

StatusEnum
NativeWrite::render(const RenderActionArgs& args)
{
    {
        std::string path;
        const std::shared_ptr<const OiioReadSupport::Header> header = readBackFrame(args.time, args.view, &path);
        if (header) {
            return renderReadBack(args, *header, path);
        }
    }
    // Taken before any knob is read, so that an edit made while the frame is written leaves the
    // file not matching.
    const U64 outputHash = getOutputContentHash();
    std::vector<ResolvedLayer> selected;
    const bool hasLayerKnob = getNode()->resolveLayerKnob(args.time, args.view, &selected);
    KnobOutputFilePtr filename = _filename.lock();
    KnobBoolPtr overwrite = _overwrite.lock();
    if (!filename || !overwrite) {
        return eStatusFailed;
    }
    // Planned from every plane the input offers, not only those asked of this render, so that a
    // layer the format cannot hold is still named in the warning about it.
    const std::vector<ImageLayerDesc> candidates = inputPlanes(args.time, args.view);
    const FilePlan plan = planFile(candidates, selected, hasLayerKnob, outputCaps(filename->getValue()));
    const bool writing = isRenderingOwnSequence();

    std::vector<int> outputPlanes;
    std::vector<bool> fetched(plan.planes.size(), writing);
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const int p = plannedPlaneOf(plan, it->first);
        outputPlanes.push_back(p);
        if (p >= 0) {
            fetched[(std::size_t)p] = true;
        }
    }

    struct PlaneCopy {
        ImagePtr source;
        RectI sourceBounds;
        std::vector<int> srcChannels;
    };

    // Fetched before any image is locked: fetching renders upstream, which may write into a
    // cached image this render would otherwise already hold a read lock on. A plane is fetched as
    // the input's whole layer, which the input produces, even when the file gets only some of it.
    const auto fetchPlanes = [&](ViewIdx view) {
        std::vector<PlaneCopy> planes(plan.planes.size());
        for (std::size_t p = 0; p < plan.planes.size(); ++p) {
            if (!fetched[p]) {
                continue;
            }
            PlaneCopy& copy = planes[p];
            const ImageLayerDesc& plane = candidates[(std::size_t)plan.planes[p]];
            RectI inputRoI;

            copy.source = getImage(0, args.time, args.mappedScale, view, NULL, &plane, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            copy.srcChannels.assign((std::size_t)plane.getNumComponents(), -1);
            if (copy.source) {
                copy.sourceBounds = copy.source->getBounds();
                const int nAvailable = (int)copy.source->getComponentsCount();
                for (int c = 0; (c < plane.getNumComponents()) && (c < nAvailable); ++c) {
                    copy.srcChannels[(std::size_t)c] = c;
                }
            }
        }

        return planes;
    };
    const std::vector<PlaneCopy> copies = fetchPlanes(args.view);

    // The output is what the file of this frame holds, whether or not the frame is written, so the
    // cache holds the same image for a render to disk and for a pull.
    const auto copyOutput = [&]() {
        std::vector<int>::const_iterator planned = outputPlanes.begin();
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it, ++planned) {
            const ImagePtr& outImage = it->second;
            if (!outImage) {
                continue;
            }
            std::vector<int> channels((std::size_t)outImage->getComponentsCount(), -1);
            ImagePtr source;
            RectI sourceBounds;
            if (*planned >= 0) {
                const std::size_t p = (std::size_t)*planned;
                const PlaneCopy& copy = copies[p];
                const std::vector<int>& inFile = plan.channels[p].source;
                source = copy.source;
                sourceBounds = copy.sourceBounds;
                // The colour plane keeps its layout with the unwritten channels black; any other
                // plane is the written channels alone, in file order.
                const bool colour = it->first.isColorLayer();
                for (std::size_t i = 0; i < inFile.size(); ++i) {
                    const std::size_t c = (std::size_t)inFile[i];
                    const std::size_t dst = colour ? c : i;
                    if ((dst < channels.size()) && (c < copy.srcChannels.size())) {
                        channels[dst] = copy.srcChannels[c];
                    }
                }
            }
            const StatusEnum stat = copyPlane(*this, source, sourceBounds, channels, outImage, args.roi);
            if ((stat != eStatusOK) || aborted()) {
                return stat;
            }
        }

        return eStatusOK;
    };

    // Only a render to disk writes the file; a viewer or a node downstream pulling this frame
    // must never touch the disk.
    if (!writing) {
        return copyOutput();
    }

    const std::vector<ViewIdx> written = viewsToWrite(args.view);
    std::vector<std::vector<PlaneCopy>> viewCopies;
    for (std::size_t v = 0; v < written.size(); ++v) {
        viewCopies.push_back(written[v] == args.view ? copies : fetchPlanes(written[v]));
    }

    const std::string path = filename->generateFileNameAtTime((SequenceTime)std::floor(args.time + 0.5), args.view).toStdString();
    if (path.empty()) {
        setPersistentMessage(eMessageTypeError, tr("No output file is set").toStdString());

        return eStatusFailed;
    }
    if (rejectUnsupportedFormat(path)) {
        return eStatusFailed;
    }

    bool exists = false;
    if (!overwrite->getValue()) {
        std::error_code ec;
        exists = std::filesystem::exists(std::filesystem::path(path), ec);
    }

    const std::string ext = extensionOf(path);
    const std::string formatName = OiioWriteSupport::formatNameForWritableExtension(ext);
    const OiioWriteSupport::FormatCaps caps = OiioWriteSupport::capabilities(formatName);

    const bool warnDropped = !plan.dropped.empty();
    const std::string droppedNames = droppedLayerNames(plan);

    RenderCancellation cancel(this);
    bool wrote = false;
    std::string writeError;
    if (!exists && !plan.planes.empty() && !written.empty()) {
        Window window;
        if (!resolveWindow(args.time, args.view, &window)) {
            return eStatusFailed;
        }
        const RectI dataWindow = window.rod.toPixelEnclosing(0u, window.par);
        if ((dataWindow.width() <= 0) || (dataWindow.height() <= 0)) {
            setPersistentMessage(eMessageTypeError, tr("There is nothing to write: the region to write is empty").toStdString());

            return eStatusFailed;
        }

        std::vector<std::string> names;
        for (std::size_t p = 0; p < plan.channels.size(); ++p) {
            names.insert(names.end(), plan.channels[p].names.begin(), plan.channels[p].names.end());
        }

        KnobChoicePtr compression = _compression.lock();
        const OiioWriteSupport::DataType type = resolveDataType(_bitDepth.lock(), formatName, ext);

        OiioWriteSupport::WritePart part;
        OIIO::ImageSpec& spec = part.spec;
        spec = OIIO::ImageSpec(dataWindow.width(), dataWindow.height(), (int)names.size(), OiioWriteSupport::dataTypeOiio(type));
        spec.channelnames = names;
        spec.alpha_channel = -1;
        spec.z_channel = -1;
        spec.attribute("oiio:BitsPerSample", OiioWriteSupport::dataTypeBitsPerSample(type));
        // Without this OIIO divides RGB by alpha on the way out for the formats it treats as
        // unassociated, and the buffer must be written as it is.
        spec.attribute("oiio:UnassociatedAlpha", 1);
        const std::string outputSpace = _outputSpace.space();
        if (!outputSpace.empty()) {
            spec.attribute("oiio:ColorSpace", IoColorSpace::fileTag(*getApp()->getProject(), outputSpace));
        }
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr toOutput;
        {
            std::string conversionError;
            if (!IoColorSpace::processor(*getApp()->getProject(), _outputSpace.workingSpace(), outputSpace, _outputSpace.contextVariables(args.time), &toOutput, &conversionError)) {
                setPersistentMessage(eMessageTypeError, conversionError);

                return eStatusFailed;
            }
        }
        if (window.par != 1.) {
            spec.attribute("PixelAspectRatio", (float)window.par);
        }
        if (caps.displayWindow) {
            spec.x = dataWindow.x1;
            spec.y = window.display.y2 - dataWindow.y2;
            spec.full_x = window.display.x1;
            spec.full_y = window.display.y1;
            spec.full_width = window.display.width();
            spec.full_height = window.display.height();
        }

        const KnobBoolPtr writeMetadata = _writeMetadata.lock();
        if (!writeMetadata || writeMetadata->getValue()) {
            OiioWriteSupport::applyMetadata(spec, getUpstreamMetadata(args.time, args.view), formatName);
        }
        if (formatName == "openexr") {
            spec.attribute(kNatronWriteOutputHashAttribute, outputHashText(outputHash));
        }

        const std::string compressionID = activeID(compression);
        const OiioWriteSupport::ParamVisibility visible = OiioWriteSupport::paramVisibility(formatName, compressionID);
        std::string compressionName = (compressionID == kNatronWriteCompressionDefault) ? std::string() : compressionID;
        if (compressionName.empty() && visible.quality && (formatName == "jpeg" || formatName == "webp" || formatName == "heic" || formatName == "avif")) {
            compressionName = formatName;
        }
        if (visible.quality) {
            KnobIntPtr quality = _quality.lock();
            const int value = quality ? quality->getValue() : 100;
            if (compressionName.empty()) {
                spec.attribute("CompressionQuality", value);
            } else {
                compressionName += ':' + std::to_string(value);
            }
        } else if (visible.dwaCompressionLevel) {
            KnobDoublePtr dwa = _dwaCompressionLevel.lock();
            compressionName += ':' + std::to_string((int)(dwa ? dwa->getValue() : 45.));
        } else if (visible.zipCompressionLevel) {
            KnobIntPtr zip = _zipCompressionLevel.lock();
            compressionName += ':' + std::to_string(zip ? zip->getValue() : 4);
        }
        if (!compressionName.empty()) {
            spec.attribute("compression", compressionName);
        }
        if (visible.tileSize) {
            KnobChoicePtr tileSize = _tileSize.lock();
            const int tile = tileSize ? std::atoi(activeID(tileSize).c_str()) : 0;
            if (tile > 0) {
                spec.tile_width = std::min(tile, spec.width);
                spec.tile_height = std::min(tile, spec.height);
                spec.tile_depth = 1;
            }
        }

        std::vector<std::unique_ptr<Image::ReadAccess>> accesses;
        std::vector<ViewPlanes> viewPlanes;
        const std::vector<std::string> projectViews = getApp()->getProject()->getProjectViewNames();
        for (std::size_t v = 0; v < written.size(); ++v) {
            ViewPlanes planes;
            planes.view = ((std::size_t)written[v].value() < projectViews.size()) ? projectViews[(std::size_t)written[v].value()] : std::string();
            for (std::size_t p = 0; p < plan.planes.size(); ++p) {
                const PlaneCopy& copy = viewCopies[v][p];
                accesses.push_back(std::unique_ptr<Image::ReadAccess>(new Image::ReadAccess(copy.source.get())));
                ViewPlane plane;
                FrameSource& frame = plane.frame;
                frame.access = copy.source ? accesses.back().get() : NULL;
                frame.depth = copy.source ? copy.source->getBitDepth() : eImageBitDepthFloat;
                frame.bounds = copy.source ? copy.sourceBounds : RectI();
                frame.nComps = copy.source ? (int)copy.source->getComponentsCount() : 0;
                frame.window = dataWindow;
                frame.outOffset = 0;
                frame.outStride = 0;
                frame.colour = false;
                for (std::size_t i = 0; i < plan.channels[p].source.size(); ++i) {
                    const std::size_t c = (std::size_t)plan.channels[p].source[i];
                    frame.channels.push_back(c < copy.srcChannels.size() ? copy.srcChannels[c] : -1);
                }
                plane.names = plan.channels[p].names;
                const ImageLayerDesc& layer = plan.outputs[p];
                frame.colour = layer.isColorLayer();
                plane.label = layer.isColorLayer() ? std::string(kNatronColorStorageLabel) : layer.getLayerID();
                planes.planes.push_back(plane);
            }
            viewPlanes.push_back(planes);
        }

        PartSplitEnum split = ePartSplitSingle;
        if (visible.views) {
            const std::string splitID = activeID(_partSplitting.lock());
            split = splitID == kNatronWritePartSplittingViewsLayers ? ePartSplitViewsLayers : (splitID == kNatronWritePartSplittingViews ? ePartSplitViews : ePartSplitSingle);
        }
        const PartLayout layout = layoutParts(spec, viewPlanes, split);

        const int nThreads = appPTR->getNCPUsAvailableForEffect();
        const int rowsPerChunk = std::max(1, kMinBandPixels / std::max(1, spec.width));
        std::atomic<bool> conversionFailed(false);
        std::string rowError;
        std::mutex rowErrorMutex;
        const OiioWriteSupport::RowSource rowSource = [&](int part, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride) {
            const std::vector<FrameSource>& frames = layout.frames[(std::size_t)part];
            const std::size_t rowFloats = (std::size_t)spec.width * (std::size_t)layout.parts[(std::size_t)part].spec.nchannels;
            const int count = rowEnd - rowBegin;
            const int chunks = (count + rowsPerChunk - 1) / rowsPerChunk;
            const std::function<void(int)> body = [&](int chunk) {
                const int first = rowBegin + chunk * rowsPerChunk;
                const int last = std::min(rowEnd, first + rowsPerChunk);
                std::vector<float> scratch;
                for (int r = first; r < last; ++r) {
                    float* out = (float*)((char*)band + (std::ptrdiff_t)(r - rowBegin) * (std::ptrdiff_t)rowStride);
                    std::fill(out, out + rowFloats, 0.f);
                    for (std::size_t f = 0; f < frames.size(); ++f) {
                        if (frames[f].access) {
                            convertFileRow(frames[f], r, out);
                            if (toOutput && frames[f].colour) {
                                std::string error;
                                if (!convertColourRow(frames[f], r, *toOutput, scratch, out, &error)) {
                                    std::lock_guard<std::mutex> lock(rowErrorMutex);
                                    rowError = error;
                                    conversionFailed = true;

                                    return;
                                }
                            }
                        }
                    }
                    if (((r - first) % 8 == 0) && cancel.check()) {
                        return;
                    }
                }
            };
            parallelForCancellable(chunks, nThreads, cancel, body);

            return !cancel.check() && !conversionFailed;
        };

        wrote = OiioWriteSupport::writeFile(path, layout.parts, rowSource, [&]() { return cancel.check(); }, &writeError);
        if (conversionFailed) {
            setPersistentMessage(eMessageTypeError, rowError);

            return eStatusFailed;
        }
        if (!wrote && !cancel.check()) {
            setPersistentMessage(eMessageTypeError, writeError.empty() ? tr("Could not write %1").arg(QString::fromStdString(path)).toStdString() : writeError);

            return eStatusFailed;
        }
    }

    const StatusEnum stat = copyOutput();
    if ((stat != eStatusOK) || aborted()) {
        return stat;
    }
    if (wrote && warnDropped) {
        setPersistentMessage(eMessageTypeWarning, tr("%1 cannot hold more than four channels: only the colour was written, not %2").arg(QString::fromStdString(formatName), QString::fromStdString(droppedNames)).toStdString());
    } else if (wrote || exists) {
        clearPersistentMessage(false);
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
