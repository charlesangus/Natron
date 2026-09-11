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

#include "DeepMerge.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/KnobTypes.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isDepthChannelName(const std::string& name)
{
    return (name == "Z") || (name == "ZBack");
}

bool
hasSamples(const DeepImagePtr& image)
{
    return image && (image->getSampleTable().getTotalSampleCount() > 0);
}

// One input's buffers resolved against the output's channel list once per render, so that the
// per-pixel passes only ever index.
struct DeepInputView {
    DeepImagePtr image;
    const float* z;
    const float* zback;
    std::vector<const float*> channels;

    DeepInputView()
        : image()
        , z(nullptr)
        , zback(nullptr)
        , channels()
    {
    }

    void init(const DeepImagePtr& source,
              const std::vector<std::string>& channelNames)
    {
        image = source;
        channels.assign(channelNames.size(), nullptr);
        if (!image) {
            return;
        }
        const DeepChannelBuffer* zBuffer = image->getChannel("Z");
        const DeepChannelBuffer* zBackBuffer = image->getChannel("ZBack");
        z = zBuffer ? zBuffer->data() : nullptr;
        zback = zBackBuffer ? zBackBuffer->data() : nullptr;
        for (std::size_t c = 0; c < channelNames.size(); ++c) {
            const DeepChannelBuffer* buffer = image->getChannel(channelNames[c]);
            channels[c] = buffer ? buffer->data() : nullptr;
        }
    }

    // The index into the sample table of the pixel at (x, y), or -1 when this input has nothing
    // there: no image, no depths at all, or (x, y) outside its bounds.
    std::ptrdiff_t pixelIndex(int x,
                              int y) const
    {
        if (!image || !z) {
            return -1;
        }
        const RectI& bounds = image->getBounds();
        if (!bounds.contains(x, y)) {
            return -1;
        }

        return ((std::ptrdiff_t)(y - bounds.y1) * (std::ptrdiff_t)bounds.width()) + (std::ptrdiff_t)(x - bounds.x1);
    }

    U32 sampleCount(std::ptrdiff_t index) const
    {
        return (index < 0) ? 0 : image->getSampleTable().getCount((std::size_t)index);
    }

    U64 sampleOffset(std::ptrdiff_t index) const
    {
        return image->getSampleTable().getOffset((std::size_t)index);
    }
};

void
copySample(const DeepPixelView& src,
           int srcIndex,
           const MutableDeepPixelView& dst,
           int dstIndex)
{
    dst.z[dstIndex] = src.z[srcIndex];
    dst.zback[dstIndex] = src.zbackAt(srcIndex);
    for (int c = 0; c < src.numChannels; ++c) {
        dst.channels[c][dstIndex] = src.channels[c][srcIndex];
    }
}

// Writes input's samples at pixel index into out starting at slot *next, advancing *next past
// them. A channel the input lacks is zero on its samples.
void
appendSamples(const DeepInputView& input,
              std::ptrdiff_t index,
              const MutableDeepPixelView& out,
              int* next)
{
    const U32 count = input.sampleCount(index);

    if (count == 0) {
        return;
    }
    const U64 offset = input.sampleOffset(index);
    for (U32 s = 0; s < count; ++s) {
        const int slot = *next + (int)s;
        out.z[slot] = input.z[offset + s];
        out.zback[slot] = input.zback ? input.zback[offset + s] : out.z[slot];
        for (int c = 0; c < out.numChannels; ++c) {
            out.channels[c][slot] = input.channels[c] ? input.channels[c][offset + s] : 0.f;
        }
    }
    *next += (int)count;
}

struct CombineWorkspace {
    std::vector<int> order;
    DeepPixelScratch copy;
};

CombineWorkspace&
combineWorkspace()
{
    static thread_local CombineWorkspace workspace;

    return workspace;
}

void
sortSamplesInPlace(const MutableDeepPixelView& out)
{
    CombineWorkspace& ws = combineWorkspace();

    DeepPixelOps::sortSamplesByDepth(out, ws.order);

    bool alreadySorted = true;
    for (int s = 0; s < out.numSamples; ++s) {
        if (ws.order[s] != s) {
            alreadySorted = false;
            break;
        }
    }
    if (alreadySorted) {
        return;
    }

    ws.copy.reset(out.numChannels, out.alphaChannelIndex);
    ws.copy.setSampleCount(out.numSamples);
    const MutableDeepPixelView copy = ws.copy.view();
    for (int s = 0; s < out.numSamples; ++s) {
        copySample(out, s, copy, s);
    }
    for (int s = 0; s < out.numSamples; ++s) {
        copySample(copy, ws.order[s], out, s);
    }
}

struct HoldoutWorkspace {
    DeepPixelScratch matte;
    DeepTidyWorkspace tidyWork;
    std::vector<float> boundaries;
    DeepPixelScratch mattePieces;
    std::vector<const float*> aChannels;
};

HoldoutWorkspace&
holdoutWorkspace()
{
    static thread_local HoldoutWorkspace workspace;

    return workspace;
}

// Tidies B's alpha at one pixel into ws->matte and lists in ws->boundaries every depth at which
// one of its fragments begins or ends.
void
prepareMatte(const DeepPixelView& bPixel,
             HoldoutWorkspace* ws)
{
    DeepPixelOps::tidySamples(bPixel, &ws->matte, &ws->tidyWork);

    const DeepPixelView matte = ws->matte.view();
    ws->boundaries.clear();
    for (int s = 0; s < matte.numSamples; ++s) {
        ws->boundaries.push_back(matte.z[s]);
        if (!matte.isPointSample(s)) {
            ws->boundaries.push_back(matte.zbackAt(s));
        }
    }
    std::sort(ws->boundaries.begin(), ws->boundaries.end());
    ws->boundaries.erase(std::unique(ws->boundaries.begin(), ws->boundaries.end()), ws->boundaries.end());
}

std::vector<float>::const_iterator
firstInteriorBoundary(const std::vector<float>& boundaries,
                      float z)
{
    return std::upper_bound(boundaries.begin(), boundaries.end(), z);
}

std::vector<float>::const_iterator
endInteriorBoundary(const std::vector<float>& boundaries,
                    float zback)
{
    return std::lower_bound(boundaries.begin(), boundaries.end(), zback);
}

U32
heldOutSampleCount(const DeepPixelView& aPixel,
                   const std::vector<float>& boundaries)
{
    U32 count = 0;

    for (int s = 0; s < aPixel.numSamples; ++s) {
        count += 1;
        if (!aPixel.isPointSample(s)) {
            const std::vector<float>::const_iterator first = firstInteriorBoundary(boundaries, aPixel.z[s]);
            const std::vector<float>::const_iterator end = endInteriorBoundary(boundaries, aPixel.zbackAt(s));
            if (end > first) {
                count += (U32)(end - first);
            }
        }
    }

    return count;
}

// The transmittance the tidy matte accumulates in front of one piece [z, zback] of an A sample,
// under the ordering and the split and merge rules tidySamples() applies: a matte fragment
// wholly in front attenuates the piece in full, one coincident with it by half (Hillman's
// interspersed merge), and one straddling the piece's front by the alpha its front part holds.
// The piece's interior holds none of the matte's boundaries, so a fragment overlapping it
// covers it entirely.
float
matteTransmittance(const DeepPixelView& matte,
                   float z,
                   float zback,
                   HoldoutWorkspace* ws)
{
    const bool pointPiece = zback <= z;
    const float back = pointPiece ? z : zback;
    float transmittance = 1.f;

    for (int g = 0; g < matte.numSamples; ++g) {
        const float zg = matte.z[g];
        const float zbg = matte.zbackAt(g);
        const float alpha = matte.channels[0][g];

        if (zbg <= zg) {
            // A point fragment sorts before a volumetric piece starting at its depth, and merges
            // with a point piece at its depth.
            if ((zg < z) || ((zg == z) && !pointPiece)) {
                transmittance *= 1.f - alpha;
            } else if (zg == z) {
                transmittance *= 1.f - 0.5f * alpha;
            }
            continue;
        }
        if (zbg <= z) {
            transmittance *= 1.f - alpha;
            continue;
        }
        if (zg >= back) {
            continue;
        }

        ws->mattePieces.reset(1, 0);
        ws->mattePieces.setSampleCount(3);
        const MutableDeepPixelView pieces = ws->mattePieces.view();
        pieces.z[0] = zg;
        pieces.zback[0] = zbg;
        pieces.channels[0][0] = alpha;
        int coincident = 0;
        if (zg < z) {
            DeepPixelOps::splitVolumeSample(pieces, 0, z, pieces, 0, 1);
            transmittance *= 1.f - pieces.channels[0][0];
            coincident = 1;
        }
        if (pointPiece) {
            continue;
        }
        if (pieces.zback[coincident] > zback) {
            DeepPixelOps::splitVolumeSample(pieces, coincident, zback, pieces, coincident, 2);
        }
        transmittance *= 1.f - 0.5f * pieces.channels[0][coincident];
    }

    return transmittance;
}

// Writes A's samples at this pixel into out, each cut at the matte's boundaries inside it and
// each resulting piece attenuated by the matte in front of it. out holds exactly
// heldOutSampleCount() slots.
void
writeHeldOutSamples(const DeepPixelView& aPixel,
                    const MutableDeepPixelView& out,
                    HoldoutWorkspace* ws)
{
    const DeepPixelView matte = ws->matte.view();
    int slot = 0;

    for (int s = 0; s < aPixel.numSamples; ++s) {
        const int firstPiece = slot;

        copySample(aPixel, s, out, slot);
        if (!aPixel.isPointSample(s)) {
            const std::vector<float>::const_iterator end = endInteriorBoundary(ws->boundaries, aPixel.zbackAt(s));
            for (std::vector<float>::const_iterator it = firstInteriorBoundary(ws->boundaries, aPixel.z[s]); it != end; ++it) {
                DeepPixelOps::splitVolumeSample(out, slot, *it, out, slot, slot + 1);
                ++slot;
            }
        }
        for (int piece = firstPiece; piece <= slot; ++piece) {
            const float transmittance = matteTransmittance(matte, out.z[piece], out.zback[piece], ws);
            if (transmittance != 1.f) {
                for (int c = 0; c < out.numChannels; ++c) {
                    out.channels[c][piece] *= transmittance;
                }
            }
        }
        ++slot;
    }
}
} // anonymous namespace

NativePluginDescription
DeepMerge::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPMERGE;
    desc.label = "DeepMerge";
    desc.description = tr("Merge two deep inputs. Combine puts both inputs' samples into one "
                          "deep pixel, sorted front-to-back and left untidied; Holdout keeps A's "
                          "samples, each attenuated by the transparency B accumulates in front of "
                          "it, and drops B's colour.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("A", true, eDataKindDeep));
    desc.inputs.push_back(NativeInputDescription("B", true, eDataKindDeep));
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepMerge::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobChoicePtr operation = createKnob<KnobChoice>(tr("Operation"));

    operation->setName("operation");
    operation->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("combine", "Combine", tr("Both inputs' samples, sorted by depth into one pixel.").toStdString()));
        choices.push_back(ChoiceOption("holdout", "Holdout", tr("A's samples, attenuated by whatever of B lies in front of them.").toStdString()));
        operation->populateChoices(choices);
    }
    operation->setDefaultValue((int)eOperationCombine);
    page->addKnob(operation);
    _operation = operation;
}

DeepMerge::OperationEnum
DeepMerge::getOperation() const
{
    KnobChoicePtr operation = _operation.lock();

    return (operation && (operation->getValue() == (int)eOperationHoldout)) ? eOperationHoldout : eOperationCombine;
}

StatusEnum
DeepMerge::getRegionOfDefinition(U64 hash,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    if (getOperation() == eOperationCombine) {
        return NativeEffectBase::getRegionOfDefinition(hash, time, scale, view, rod);
    }

    EffectInstancePtr a = getInput(0);
    if (!a) {
        return eStatusReplyDefault;
    }
    bool isProjectFormat = false;

    return a->getRegionOfDefinition_public(hash, time, scale, view, rod, &isProjectFormat);
}

StatusEnum
DeepMerge::renderDeep(const DeepRenderActionArgs& args)
{
    const DeepImagePtr a = args.getInputDeepImage(0);
    const DeepImagePtr b = args.getInputDeepImage(1);

    if (!a && !b) {
        setPersistentMessage(eMessageTypeError, tr("Nothing to merge: neither input carries deep data.").toStdString());

        return eStatusFailed;
    }

    if (getOperation() == eOperationHoldout) {
        return renderHoldout(args, a, b);
    }

    return renderCombine(args, a, b);
}

StatusEnum
DeepMerge::renderCombine(const DeepRenderActionArgs& args,
                         const DeepImagePtr& a,
                         const DeepImagePtr& b)
{
    std::set<std::string> names;
    for (int input = 0; input < 2; ++input) {
        const DeepImagePtr& image = (input == 0) ? a : b;
        if (!image) {
            continue;
        }
        for (std::map<std::string, DeepChannelBuffer>::const_iterator it = image->getChannels().begin(); it != image->getChannels().end(); ++it) {
            if (!isDepthChannelName(it->first)) {
                names.insert(it->first);
            }
        }
    }

    const std::vector<std::string> channelNames(names.begin(), names.end());
    if (channelNames.empty()) {
        setPersistentMessage(eMessageTypeError, tr("Nothing to merge: the deep data has no channels besides Z and ZBack.").toStdString());

        return eStatusFailed;
    }

    // Only the views handed to the fill pass carry this, and nothing this node does with them
    // reads it, so inputs with no alpha at all still have a well-formed index to report.
    int alphaChannelIndex = 0;
    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        if (channelNames[c] == "A") {
            alphaChannelIndex = (int)c;
            break;
        }
    }

    DeepInputView inputA;
    DeepInputView inputB;
    inputA.init(a, channelNames);
    inputB.init(b, channelNames);

    clearPersistentMessage(false);

    // Two inputs' samples can overlap however sorted they each were; only one input on its own
    // stays as tidy as it came.
    const bool resultIsTidy = !(hasSamples(a) && hasSamples(b)) && (!hasSamples(a) || a->isTidy()) && (!hasSamples(b) || b->isTidy());

    return renderDeepTwoPass(args, channelNames, alphaChannelIndex, [&inputA, &inputB](int x, int y) -> U32 { return inputA.sampleCount(inputA.pixelIndex(x, y)) + inputB.sampleCount(inputB.pixelIndex(x, y)); }, [&inputA, &inputB](int x, int y, const MutableDeepPixelView& out) {
        int next = 0;

        appendSamples(inputA, inputA.pixelIndex(x, y), out, &next);
        appendSamples(inputB, inputB.pixelIndex(x, y), out, &next);
        if (out.numSamples > 1) {
            sortSamplesInPlace(out);
        } }, resultIsTidy);
} // DeepMerge::renderCombine

StatusEnum
DeepMerge::renderHoldout(const DeepRenderActionArgs& args,
                         const DeepImagePtr& a,
                         const DeepImagePtr& b)
{
    std::vector<std::string> channelNames;
    int alphaChannelIndex = -1;
    if (a) {
        for (std::map<std::string, DeepChannelBuffer>::const_iterator it = a->getChannels().begin(); it != a->getChannels().end(); ++it) {
            if (isDepthChannelName(it->first)) {
                continue;
            }
            if (it->first == "A") {
                alphaChannelIndex = (int)channelNames.size();
            }
            channelNames.push_back(it->first);
        }
    }

    if (channelNames.empty() || (alphaChannelIndex < 0)) {
        if (!hasSamples(a)) {
            // Nothing to hold out, so nothing depends on A's channel set: keep the output well
            // formed with alpha alone.
            channelNames.assign(1, "A");
            alphaChannelIndex = 0;
        } else {
            setPersistentMessage(eMessageTypeError, tr("Holdout needs an alpha channel on A.").toStdString());

            return eStatusFailed;
        }
    }

    const DeepChannelBuffer* bAlphaBuffer = hasSamples(b) ? b->getChannel("A") : nullptr;
    if (hasSamples(b) && (!bAlphaBuffer || !bAlphaBuffer->data())) {
        setPersistentMessage(eMessageTypeError, tr("Holdout needs an alpha channel on B.").toStdString());

        return eStatusFailed;
    }
    const float* const bAlpha = bAlphaBuffer ? bAlphaBuffer->data() : nullptr;

    DeepInputView inputA;
    DeepInputView inputB;
    inputA.init(a, channelNames);
    inputB.init(bAlpha ? b : DeepImagePtr(), std::vector<std::string>());

    const auto aPixelView = [&inputA, alphaChannelIndex](std::ptrdiff_t index, HoldoutWorkspace* ws) -> DeepPixelView {
        const U64 offset = inputA.sampleOffset(index);
        DeepPixelView pixel;

        ws->aChannels.resize(inputA.channels.size());
        for (std::size_t c = 0; c < inputA.channels.size(); ++c) {
            ws->aChannels[c] = inputA.channels[c] + offset;
        }
        pixel.z = inputA.z + offset;
        pixel.zback = inputA.zback ? (inputA.zback + offset) : nullptr;
        pixel.channels = ws->aChannels.data();
        pixel.numChannels = (int)inputA.channels.size();
        pixel.alphaChannelIndex = alphaChannelIndex;
        pixel.numSamples = (int)inputA.sampleCount(index);

        return pixel;
    };

    // The matte only ever needs B's alpha: a single-channel view is all tidySamples() sees.
    const auto bPixelView = [&inputB, bAlpha](std::ptrdiff_t index, const float** alphaSlot) -> DeepPixelView {
        const U64 offset = inputB.sampleOffset(index);
        DeepPixelView pixel;

        *alphaSlot = bAlpha + offset;
        pixel.z = inputB.z + offset;
        pixel.zback = inputB.zback ? (inputB.zback + offset) : nullptr;
        pixel.channels = alphaSlot;
        pixel.numChannels = 1;
        pixel.alphaChannelIndex = 0;
        pixel.numSamples = (int)inputB.sampleCount(index);

        return pixel;
    };

    clearPersistentMessage(false);

    // Cutting A's samples at the matte's boundaries and scaling the pieces preserves their order
    // and keeps them from overlapping, so A's tidiness is the output's.
    const bool resultIsTidy = !hasSamples(a) || a->isTidy();

    return renderDeepTwoPass(args, channelNames, alphaChannelIndex, [&inputA, &inputB, &aPixelView, &bPixelView](int x, int y) -> U32 {
        const std::ptrdiff_t indexA = inputA.pixelIndex(x, y);
        const U32 countA = inputA.sampleCount(indexA);

        if (countA == 0) {
            return 0;
        }
        const std::ptrdiff_t indexB = inputB.pixelIndex(x, y);
        if (inputB.sampleCount(indexB) == 0) {
            return countA;
        }
        HoldoutWorkspace& ws = holdoutWorkspace();
        const float* alphaSlot = nullptr;
        prepareMatte(bPixelView(indexB, &alphaSlot), &ws);

        return heldOutSampleCount(aPixelView(indexA, &ws), ws.boundaries); }, [&inputA, &inputB, &aPixelView, &bPixelView](int x, int y, const MutableDeepPixelView& out) {
        const std::ptrdiff_t indexA = inputA.pixelIndex(x, y);

        if (inputA.sampleCount(indexA) == 0) {
            return;
        }
        HoldoutWorkspace& ws = holdoutWorkspace();
        const DeepPixelView aPixel = aPixelView(indexA, &ws);
        const std::ptrdiff_t indexB = inputB.pixelIndex(x, y);
        if (inputB.sampleCount(indexB) == 0) {
            for (int s = 0; s < aPixel.numSamples; ++s) {
                copySample(aPixel, s, out, s);
            }

            return;
        }
        const float* alphaSlot = nullptr;
        prepareMatte(bPixelView(indexB, &alphaSlot), &ws);
        writeHeldOutSamples(aPixel, out, &ws); }, resultIsTidy);
} // DeepMerge::renderHoldout

NATRON_NAMESPACE_EXIT
