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

#ifndef Tests_DeepRenderTestEffect_h
#define Tests_DeepRenderTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Engine/DeepImage.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

#define kTestPluginIDDeepRenderSource "test.natron.built-in.DeepRenderSource"
#define kTestPluginIDDeepRenderGain "test.natron.built-in.DeepRenderGain"

#define kDeepRenderTestWidth 19
#define kDeepRenderTestHeight 13
#define kDeepRenderTestGain 3.f

NATRON_NAMESPACE_ENTER

// The per-pixel formulas the source stub evaluates. They live here rather than inside the node so
// that the serial reference the two-pass test compares against can drive exactly the same
// arithmetic, leaving the parallel chunked driver as the only difference between the two.
inline U32
deepRenderTestSampleCount(int x,
                          int y)
{
    return (U32)(((x * 3) + (y * 5)) % 4);
}

inline float
deepRenderTestZ(int x,
                int y,
                int sample)
{
    return (float)x + (0.25f * (float)y) + (0.03125f * (float)sample);
}

inline float
deepRenderTestChannel(int x,
                      int y,
                      int sample,
                      int channel)
{
    return ((float)((x * 7) + (y * 11) + (sample * 13) + (channel * 17))) / 32.f;
}

inline bool
deepRenderTestPixelIndex(const DeepImage& image,
                         int x,
                         int y,
                         std::size_t* index)
{
    const RectI& bounds = image.getBounds();

    if (!bounds.contains(x, y)) {
        return false;
    }
    *index = ((std::size_t)(y - bounds.y1) * (std::size_t)bounds.width()) + (std::size_t)(x - bounds.x1);

    return true;
}

inline std::vector<std::string>
deepRenderTestChannelNames()
{
    std::vector<std::string> names;

    names.push_back("R");
    names.push_back("G");
    names.push_back("B");
    names.push_back("A");

    return names;
}

// How many times each stub's renderDeep() actually ran, so a test can tell a cache hit from a
// second render.
inline std::atomic<int>&
deepRenderTestSourceRenderCount()
{
    static std::atomic<int> count(0);

    return count;
}

inline std::atomic<int>&
deepRenderTestGainRenderCount()
{
    static std::atomic<int> count(0);

    return count;
}

// Called at the top of the source stub's render action, so a test can make something happen
// (aborting the render, typically) at a point the caller cannot otherwise reach.
inline std::function<void()>&
deepRenderTestSourceHook()
{
    static std::function<void()> hook;

    return hook;
}

/**
 * @brief A deep generator: no inputs, a fixed region of definition, and a sample count that
 * varies per pixel (including pixels with no samples at all) so that the prefix-sum offsets the
 * two-pass helper builds are not uniform.
 **/
class DeepRenderTestSource
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DeepRenderTestSource(n);
    }

    explicit DeepRenderTestSource(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    // Real Tier-1 deep nodes are spatially local, so deep RoI propagation mirrors the image
    // path's: these stubs need to say so too, or every RoI narrows to the RoD instead of the
    // caller's actual render window.
    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        rod->x1 = 0.;
        rod->y1 = 0.;
        rod->x2 = kDeepRenderTestWidth;
        rod->y2 = kDeepRenderTestHeight;

        return eStatusOK;
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDDeepRenderSource;
        desc.label = "Test Deep Render Source";
        desc.description = "";
        desc.outputKind = eDataKindDeep;

        return desc;
    }

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++deepRenderTestSourceRenderCount();
        if (deepRenderTestSourceHook()) {
            deepRenderTestSourceHook()();
        }

        return renderDeepTwoPass(args, deepRenderTestChannelNames(), 3, &deepRenderTestSampleCount, [](int x, int y, const MutableDeepPixelView& out) {
            for (int s = 0; s < out.numSamples; ++s) {
                out.z[s] = deepRenderTestZ(x, y, s);
                out.zback[s] = out.z[s];
                for (int c = 0; c < out.numChannels; ++c) {
                    out.channels[c][s] = deepRenderTestChannel(x, y, s, c);
                }
            } }, true);
    }
};

/**
 * @brief A deep colour op: one deep input, same sample structure as that input, every channel
 * scaled. Second link of the chain the pipeline test renders.
 **/
class DeepRenderTestGain
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DeepRenderTestGain(n);
    }

    explicit DeepRenderTestGain(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    // Real Tier-1 deep nodes are spatially local, so deep RoI propagation mirrors the image
    // path's: these stubs need to say so too, or every RoI narrows to the RoD instead of the
    // caller's actual render window.
    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDDeepRenderGain;
        desc.label = "Test Deep Render Gain";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindDeep));
        desc.outputKind = eDataKindDeep;

        return desc;
    }

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++deepRenderTestGainRenderCount();

        const DeepImagePtr source = args.getInputDeepImage(0);
        if (!source) {
            return eStatusFailed;
        }

        const std::vector<std::string> channelNames = deepRenderTestChannelNames();
        const DeepChannelBuffer* srcZ = source->getChannel("Z");
        const DeepChannelBuffer* srcZBack = source->getChannel("ZBack");
        std::vector<const DeepChannelBuffer*> srcChannels(channelNames.size());
        for (std::size_t c = 0; c < channelNames.size(); ++c) {
            srcChannels[c] = source->getChannel(channelNames[c]);
            if (!srcChannels[c]) {
                return eStatusFailed;
            }
        }
        if (!srcZ) {
            return eStatusFailed;
        }

        return renderDeepTwoPass(args, channelNames, 3, [source](int x, int y) -> U32 {
            std::size_t index;

            if ( !deepRenderTestPixelIndex(*source, x, y, &index) ) {
                return 0;
            }

            return source->getSampleTable().getCount(index); }, [source, srcZ, srcZBack, &srcChannels](int x, int y, const MutableDeepPixelView& out) {
            std::size_t index;

            if ( !deepRenderTestPixelIndex(*source, x, y, &index) ) {
                return;
            }
            const U64 offset = source->getSampleTable().getOffset(index);
            for (int s = 0; s < out.numSamples; ++s) {
                out.z[s] = srcZ->data()[offset + s];
                out.zback[s] = srcZBack ? srcZBack->data()[offset + s] : out.z[s];
                for (int c = 0; c < out.numChannels; ++c) {
                    out.channels[c][s] = srcChannels[c]->data()[offset + s] * kDeepRenderTestGain;
                }
            } }, source->isTidy());
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_DeepRenderTestEffect_h
