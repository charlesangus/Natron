/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef Tests_VectorFollowTestSupport_h
#define Tests_VectorFollowTestSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <mutex>

#include "Tests/Flow/FlowTestPlate.h"
#include "Tests/MultiTimeTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDFollowTexture "test.natron.built-in.VectorFollowTexture"

NATRON_NAMESPACE_ENTER

namespace VectorFollowTest {

constexpr int kWidth = 256;
constexpr int kHeight = 192;
constexpr double kShiftX = 1.5;
constexpr double kShiftY = -1.;
constexpr int kFirstFrame = 1;
constexpr int kLastFrame = 40;

// A texture moving by (kShiftX, kShiftY) pixels a frame. Slow enough that a shape stays well
// inside the picture across the whole range.
class FollowTextureEffect
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDFollowTexture,
        .label = "Test Follow Texture",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
    };

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new FollowTextureEffect(n);
    }

    explicit FollowTextureEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual void addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE FINAL
    {
        comps->push_back(ImageLayerDesc::getRGBAComponents());
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL
    {
        *first = kFirstFrame;
        *last = kLastFrame;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        rod->x1 = 0.;
        rod->y1 = 0.;
        rod->x2 = kWidth;
        rod->y2 = kHeight;

        return eStatusOK;
    }

    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL
    {
        metadata.setNComps(-1, 4);
        metadata.setBitDepth(-1, eImageBitDepthFloat);
        metadata.setIsFrameVarying(true);
        metadata.setOutputFormat(RectI(0, 0, kWidth, kHeight));

        return eStatusOK;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        static const FlowTest::NoiseTexture texture(1234);
        const int step = 1 << args.mappedScale.toMipmapLevel();

        for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
            if (!output.second) {
                continue;
            }
            const int nComps = (int)output.second->getComponentsCount();
            Image::WriteAccess access(output.second.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                float* dst = reinterpret_cast<float*>(access.pixelAt(args.roi.x1, y));
                if (!dst) {
                    continue;
                }
                const double cy = y * step + step / 2 - args.time * kShiftY;
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    const double cx = x * step + step / 2 - args.time * kShiftX;
                    const float value = (float)texture(cx, cy);
                    for (int c = 0; c < nComps; ++c) {
                        dst[c] = (c == 3) ? 1.f : value;
                    }
                    dst += nComps;
                }
            }
        }

        return eStatusOK;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }
};

inline void
registerFollowTextureOnce()
{
    static std::once_flag once;

    std::call_once(once, []() { MultiTimeTest::registerPlugin<FollowTextureEffect>(); });
}

} // namespace VectorFollowTest

NATRON_NAMESPACE_EXIT

#endif // Tests_VectorFollowTestSupport_h
