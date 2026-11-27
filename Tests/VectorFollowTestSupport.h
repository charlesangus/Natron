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

#include <cmath>
#include <list>
#include <mutex>
#include <string>

#include "Tests/Flow/FlowTestPlate.h"
#include "Tests/MultiTimeTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDFollowTexture "test.natron.built-in.VectorFollowTexture"
#define kTestPluginIDFollowZoomTexture "test.natron.built-in.VectorFollowZoomTexture"
#define kTestPluginIDFollowVectors "test.natron.built-in.VectorFollowVectors"
#define kTestPluginIDFollowZoomVectors "test.natron.built-in.VectorFollowZoomVectors"
#define kFollowVectorsParamBackward "backward"

NATRON_NAMESPACE_ENTER

namespace VectorFollowTest {

constexpr int kWidth = 256;
constexpr int kHeight = 192;
constexpr double kShiftX = 1.5;
constexpr double kShiftY = -1.;
constexpr int kFirstFrame = 1;
constexpr int kLastFrame = 40;

constexpr double kZoomPerFrame = 1.01;

// The texture's picture, rendered through texturePoint(), which maps a pixel centre at a time to
// where the texture is sampled.
class FollowTextureBase
    : public NativeEffectBase {
public:
    explicit FollowTextureBase(NodePtr n)
        : NativeEffectBase(n)
    {
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
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    double cx = 0.;
                    double cy = 0.;
                    texturePoint(args.time, x * step + step / 2, y * step + step / 2, &cx, &cy);
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

protected:
    virtual void texturePoint(double time, double x, double y, double* tx, double* ty) const = 0;

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }
};

// A texture moving by (kShiftX, kShiftY) pixels a frame. Slow enough that a shape stays well
// inside the picture across the whole range.
class FollowTextureEffect
    : public FollowTextureBase {
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
        : FollowTextureBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL
    {
        return staticNodeSpec<kNodeSpec>();
    }

private:
    virtual void texturePoint(double time, double x, double y, double* tx, double* ty) const OVERRIDE FINAL
    {
        *tx = x - time * kShiftX;
        *ty = y - time * kShiftY;
    }
};

// A texture growing by kZoomPerFrame a frame about the picture's centre, so each region of the
// picture moves its own way.
class FollowZoomTextureEffect
    : public FollowTextureBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDFollowZoomTexture,
        .label = "Test Follow Zoom Texture",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
    };

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new FollowZoomTextureEffect(n);
    }

    explicit FollowZoomTextureEffect(NodePtr n)
        : FollowTextureBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL
    {
        return staticNodeSpec<kNodeSpec>();
    }

    // Where the texture point shown at (x, y) at time 0 is shown at time.
    static void zoomed(double time, double x, double y, double* zx, double* zy)
    {
        const double s = std::pow(kZoomPerFrame, time);
        *zx = kWidth / 2. + (x - kWidth / 2.) * s;
        *zy = kHeight / 2. + (y - kHeight / 2.) * s;
    }

private:
    virtual void texturePoint(double time, double x, double y, double* tx, double* ty) const OVERRIDE FINAL
    {
        const double s = std::pow(kZoomPerFrame, time);
        *tx = kWidth / 2. + (x - kWidth / 2.) / s;
        *ty = kHeight / 2. + (y - kHeight / 2.) / s;
    }
};

/**
 * @brief The exact motion vectors of one of the textures above, as a vector source: the Forward
 * layer, and the Backward layer unless backward is off. Each pixel holds the vector of its centre,
 * and the motion is affine, so the bilinear read of a follow is exact everywhere inside. Solving
 * the texture's flow takes far longer and only adds solver error to what a follow test checks.
 **/
class FollowVectorsBase
    : public NativeEffectBase {
public:
    explicit FollowVectorsBase(NodePtr n)
        : NativeEffectBase(n)
    {
        setSupportsRenderScaleMaybe(eSupportsNo);
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
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
        *rod = RectD(0., 0., kWidth, kHeight);

        return eStatusOK;
    }

    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL
    {
        metadata.setNComps(-1, 4);
        metadata.setBitDepth(-1, eImageBitDepthFloat);
        metadata.setOutputFormat(RectI(0, 0, kWidth, kHeight));

        return eStatusOK;
    }

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL
    {
        std::list<ImageLayerDesc>& produced = (*comps)[-1];
        KnobBoolPtr backward = _backward.lock();

        produced.clear();
        produced.push_back(ImageLayerDesc::getRGBAComponents());
        produced.push_back(ImageLayerDesc::getForwardMotionComponents());
        if (!backward || backward->getValue()) {
            produced.push_back(ImageLayerDesc::getBackwardMotionComponents());
        }
        *passThroughTime = time;
        *passThroughView = view;
        *passThroughInputNb = -1;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const std::string forwardID = ImageLayerDesc::getForwardMotionComponents().getLayerID();
        const std::string backwardID = ImageLayerDesc::getBackwardMotionComponents().getLayerID();

        for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
            if (!output.second) {
                continue;
            }
            const std::string& layerID = output.first.getLayerID();
            const bool isForward = layerID == forwardID;
            const bool isMotion = isForward || (layerID == backwardID);
            const int nComps = (int)output.second->getComponentsCount();
            Image::WriteAccess access(output.second.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                float* dst = reinterpret_cast<float*>(access.pixelAt(args.roi.x1, y));
                if (!dst) {
                    return eStatusFailed;
                }
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    double dx = 0.;
                    double dy = 0.;
                    if (isMotion) {
                        motion(x + 0.5, y + 0.5, isForward, &dx, &dy);
                    }
                    for (int c = 0; c < nComps; ++c) {
                        if (isMotion) {
                            dst[c] = (c == 0) ? (float)dx : (c == 1) ? (float)dy
                                                                     : 0.f;
                        } else {
                            dst[c] = (c == 3) ? 1.f : 0.f;
                        }
                    }
                    dst += nComps;
                }
            }
        }

        return eStatusOK;
    }

protected:
    // Where the texture shown at (x, y) moves by over the next frame, or over the previous one.
    virtual void motion(double x, double y, bool forward, double* dx, double* dy) const = 0;

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobBoolPtr backward = createKnob<KnobBool>(std::string("Backward"));
        backward->setName(kFollowVectorsParamBackward);
        backward->setAnimationEnabled(false);
        backward->setDefaultValue(true);
        page->addKnob(backward);
        _backward = backward;
    }

    KnobBoolWPtr _backward;
};

// The vectors of FollowTextureEffect.
class FollowVectorsEffect
    : public FollowVectorsBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDFollowVectors,
        .label = "Test Follow Vectors",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
    };

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new FollowVectorsEffect(n);
    }

    explicit FollowVectorsEffect(NodePtr n)
        : FollowVectorsBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL
    {
        return staticNodeSpec<kNodeSpec>();
    }

private:
    virtual void motion(double /*x*/, double /*y*/, bool forward, double* dx, double* dy) const OVERRIDE FINAL
    {
        *dx = forward ? kShiftX : -kShiftX;
        *dy = forward ? kShiftY : -kShiftY;
    }
};

// The vectors of FollowZoomTextureEffect.
class FollowZoomVectorsEffect
    : public FollowVectorsBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDFollowZoomVectors,
        .label = "Test Follow Zoom Vectors",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
    };

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new FollowZoomVectorsEffect(n);
    }

    explicit FollowZoomVectorsEffect(NodePtr n)
        : FollowVectorsBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL
    {
        return staticNodeSpec<kNodeSpec>();
    }

private:
    virtual void motion(double x, double y, bool forward, double* dx, double* dy) const OVERRIDE FINAL
    {
        const double growth = (forward ? kZoomPerFrame : 1. / kZoomPerFrame) - 1.;
        *dx = (x - kWidth / 2.) * growth;
        *dy = (y - kHeight / 2.) * growth;
    }
};

inline void
registerFollowTextureOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        MultiTimeTest::registerPlugin<FollowTextureEffect>();
        MultiTimeTest::registerPlugin<FollowZoomTextureEffect>();
        MultiTimeTest::registerPlugin<FollowVectorsEffect>();
        MultiTimeTest::registerPlugin<FollowZoomVectorsEffect>();
    });
}

} // namespace VectorFollowTest

NATRON_NAMESPACE_EXIT

#endif // Tests_VectorFollowTestSupport_h
