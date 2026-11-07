#ifndef NATRON_TESTS_NATIVEVECTORGENERATORTEST_H
#define NATRON_TESTS_NATIVEVECTORGENERATORTEST_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <string>
#include <utility>

#include "Tests/Flow/FlowTestPlate.h"
#include "Tests/MultiTimeTestEffect.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDMovingTexture "test.natron.built-in.MovingTexture"
#define kMovingTextureParamMoving "moving"
#define kMovingTextureParamFormatChangeFrame "formatChangeFrame"
#define kMovingTextureParamOriginChangeFrame "originChangeFrame"
#define kMovingTextureParamBlackFrame "blackFrame"
#define kMovingTextureParamAwayFrame "awayFrame"
#define kMovingTextureParamFailFrame "failFrame"

NATRON_NAMESPACE_ENTER

// A named namespace and inline functions, so the plug-in registers once for every test file of the binary.
namespace NativeVectorGeneratorPlate {
// The plate of the flow library's own tests, whose thresholds apply here.
constexpr int kWidth = 256;
constexpr int kHeight = 192;
constexpr double kShiftX = 3.5;
constexpr double kShiftY = -2.25;
constexpr int kMargin = 8;
constexpr int kFirstFrame = 1;
constexpr int kLastFrame = 5;
constexpr double kDisEpe = 0.05;
constexpr double kTvL1Epe = 0.015;
constexpr double kIdenticalEpe = 1e-3;
constexpr double kConfident = 0.9;

// A texture moving by (kShiftX, kShiftY) canonical pixels a frame over the frames [kFirstFrame,
// kLastFrame], sampled at each pixel's canonical centre so a mipmap level shows the same picture.
// Alpha splits the moving picture in two. From formatChangeFrame on, the format's height differs
// while the region of definition stays; from originChangeFrame on, the format moves without changing
// size. At blackFrame the picture is black, and at awayFrame the region of definition is far from
// that of every other frame. At failFrame the render fails.
class MovingTextureEffect
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDMovingTexture,
        .label = "Test Moving Texture",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
    };

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new MovingTextureEffect(n);
    }

    explicit MovingTextureEffect(NodePtr n)
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
                                             double time,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const double offset = (frameKnobValue(_awayFrame) == time) ? 4. * kWidth : 0.;
        rod->x1 = offset;
        rod->y1 = 0.;
        rod->x2 = offset + kWidth;
        rod->y2 = kHeight;

        return eStatusOK;
    }

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL
    {
        metadata.setNComps(-1, 4);
        metadata.setBitDepth(-1, eImageBitDepthFloat);
        metadata.setIsFrameVarying(true);
        const int change = frameKnobValue(_formatChangeFrame);
        const bool changed = (change != 0) && (time >= change);
        const int originChange = frameKnobValue(_originChangeFrame);
        const int x1 = ((originChange != 0) && (time >= originChange)) ? 16 : 0;
        metadata.setOutputFormat(RectI(x1, 0, x1 + kWidth, changed ? kHeight / 2 : kHeight));

        return eStatusOK;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        static const FlowTest::NoiseTexture texture(1234);
        const int step = 1 << args.mappedScale.toMipmapLevel();
        KnobBoolPtr movingKnob = _moving.lock();
        const double travel = (!movingKnob || movingKnob->getValue()) ? args.time : 0.;
        const bool black = frameKnobValue(_blackFrame) == args.time;
        if (frameKnobValue(_failFrame) == args.time) {
            return eStatusFailed;
        }

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
                const double cy = y * step + step / 2 - travel * kShiftY;
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    const double cx = x * step + step / 2 - travel * kShiftX;
                    const float value = black ? 0.f : (float)texture(cx, cy);
                    for (int c = 0; c < nComps; ++c) {
                        dst[c] = ((c == 3) && !black) ? ((cx < kWidth / 2) ? 1.f : 0.f) : value;
                    }
                    dst += nComps;
                }
            }
        }

        return eStatusOK;
    }

private:
    static int frameKnobValue(const KnobIntWPtr& knob)
    {
        const KnobIntPtr locked = knob.lock();

        return locked ? locked->getValue() : 0;
    }

    KnobIntPtr createFrameKnob(const KnobPagePtr& page,
                               const std::string& label,
                               const std::string& name)
    {
        KnobIntPtr knob = createKnob<KnobInt>(label);
        knob->setName(name);
        knob->setDefaultValue(0);
        knob->setIsMetadataSlave(true);
        page->addKnob(knob);

        return knob;
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));

        KnobBoolPtr moving = createKnob<KnobBool>(std::string("Moving"));
        moving->setName(kMovingTextureParamMoving);
        moving->setDefaultValue(true);
        page->addKnob(moving);
        _moving = moving;

        _formatChangeFrame = createFrameKnob(page, "Format Change Frame", kMovingTextureParamFormatChangeFrame);
        _originChangeFrame = createFrameKnob(page, "Origin Change Frame", kMovingTextureParamOriginChangeFrame);
        _blackFrame = createFrameKnob(page, "Black Frame", kMovingTextureParamBlackFrame);
        _awayFrame = createFrameKnob(page, "Away Frame", kMovingTextureParamAwayFrame);
        _failFrame = createFrameKnob(page, "Fail Frame", kMovingTextureParamFailFrame);
    }

    KnobBoolWPtr _moving;
    KnobIntWPtr _formatChangeFrame;
    KnobIntWPtr _originChangeFrame;
    KnobIntWPtr _blackFrame;
    KnobIntWPtr _awayFrame;
    KnobIntWPtr _failFrame;
};

inline void
registerMovingTextureOnce()
{
    static std::once_flag once;

    std::call_once(once, []() { MultiTimeTest::registerPlugin<MovingTextureEffect>(); });
}

inline const ImageLayerDesc&
forwardLayer()
{
    return ImageLayerDesc::getForwardMotionComponents();
}

inline const ImageLayerDesc&
backwardLayer()
{
    return ImageLayerDesc::getBackwardMotionComponents();
}

inline RectI
windowAt(unsigned mipmapLevel)
{
    return RectI(0, 0, kWidth >> mipmapLevel, kHeight >> mipmapLevel);
}

// Mean over the pixels at least `margin` from the plane's edge of the distance from (a, b) in the
// plane's first two channels.
inline double
meanDistance(const RenderedPlane& plane,
             double a,
             double b,
             int margin)
{
    const int width = plane.window.width();
    const int height = plane.window.height();
    const std::size_t nComps = plane.channels.size();
    double sum = 0.;
    long count = 0;
    for (int y = margin; y < height - margin; ++y) {
        for (int x = margin; x < width - margin; ++x) {
            const float* pixel = &plane.pixels[((std::size_t)y * width + x) * nComps];
            sum += std::hypot(pixel[0] - a, pixel[1] - b);
            ++count;
        }
    }

    return count ? sum / count : 0.;
}

inline double
meanOfChannel(const RenderedPlane& plane,
              int channel,
              int margin)
{
    const int width = plane.window.width();
    const int height = plane.window.height();
    const std::size_t nComps = plane.channels.size();
    double sum = 0.;
    long count = 0;
    for (int y = margin; y < height - margin; ++y) {
        for (int x = margin; x < width - margin; ++x) {
            sum += plane.pixels[((std::size_t)y * width + x) * nComps + channel];
            ++count;
        }
    }

    return count ? sum / count : 0.;
}

// The largest magnitude in one channel, or in all of them when channel is negative.
inline float
largestMagnitude(const RenderedPlane& plane,
                 int channel = -1)
{
    const std::size_t nComps = plane.channels.size();
    float largest = 0.f;
    for (std::size_t i = 0; i < plane.pixels.size(); ++i) {
        if ((channel < 0) || ((i % nComps) == (std::size_t)channel)) {
            largest = std::max(largest, std::fabs(plane.pixels[i]));
        }
    }

    return largest;
}
} // namespace NativeVectorGeneratorPlate

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVEVECTORGENERATORTEST_H
