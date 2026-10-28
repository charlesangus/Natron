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

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"
#include "NativeGolden.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Filter/VectorBlur.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kWidth = 64;
const int kHeight = 48;

#define kTestPluginIDVectorBlurSource "net.natron.test.VectorBlurSource"
#define kVBSourceParamScene "scene"
#define kVBSourceParamU "u"
#define kVBSourceParamV "v"
#define kVBSourceParamMotion "motion"

enum Scene {
    eSceneSmooth = 0, // a smooth colour pattern
    eScenePoint = 1, // one white pixel at (32, 24) on black
    eSceneOcclusion = 2, // a static near green square over far moving red stripes
};

// The near square of the occlusion scene.
const RectI kSquare(24, 16, 40, 32);

ImageLayerDesc
depthLayer()
{
    return ImageLayerDesc("depth", "depth", "Z", std::vector<std::string> { "Z" });
}

// The RGBA of each scene at pixel (x, y), black outside the 64x48 source.
void
sceneColour(int scene,
            int x,
            int y,
            float* rgba)
{
    if ((x < 0) || (x >= kWidth) || (y < 0) || (y >= kHeight)) {
        std::fill(rgba, rgba + 4, 0.f);

        return;
    }
    switch (scene) {
    case eScenePoint: {
        const float v = ((x == 32) && (y == 24)) ? 1.f : 0.f;
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = v;
        break;
    }
    case eSceneOcclusion:
        if (kSquare.contains(x, y)) {
            rgba[0] = 0.f;
            rgba[1] = 1.f;
        } else {
            rgba[0] = ((x / 4) % 2) ? 1.f : 0.f;
            rgba[1] = 0.f;
        }
        rgba[2] = 0.f;
        rgba[3] = 1.f;
        break;
    default:
        rgba[0] = (float)(0.5 + 0.5 * std::sin(x * 0.4));
        rgba[1] = (float)y / kHeight;
        rgba[2] = (float)((x * 3 + y) % 5) / 5.f;
        rgba[3] = 1.f;
        break;
    }
}

/**
 * @brief A 64x48 source carrying RGBA, forward motion vectors and depth, at mipmap 0. The scene
 * knob picks the RGBA (see Scene); the vectors are (u, v) everywhere but on the occlusion
 * scene's square, where they are zero; depth is 1 on that square and 10 elsewhere. With motion
 * off the source carries no motion layer.
 **/
class VectorBlurSourceTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new VectorBlurSourceTestEffect(n);
    }

    explicit VectorBlurSourceTestEffect(NodePtr n)
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

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
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

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDVectorBlurSource,
        .label = "Test VectorBlur Source",
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobIntPtr scene = createKnob<KnobInt>(std::string("Scene"));
        scene->setName(kVBSourceParamScene);
        scene->setAnimationEnabled(false);
        scene->setDefaultValue(eSceneSmooth);
        page->addKnob(scene);
        _scene = scene;

        KnobDoublePtr u = createKnob<KnobDouble>(std::string("U"));
        u->setName(kVBSourceParamU);
        u->setAnimationEnabled(false);
        u->setDefaultValue(0.);
        page->addKnob(u);
        _u = u;

        KnobDoublePtr v = createKnob<KnobDouble>(std::string("V"));
        v->setName(kVBSourceParamV);
        v->setAnimationEnabled(false);
        v->setDefaultValue(0.);
        page->addKnob(v);
        _v = v;

        KnobBoolPtr motion = createKnob<KnobBool>(std::string("Motion"));
        motion->setName(kVBSourceParamMotion);
        motion->setAnimationEnabled(false);
        motion->setDefaultValue(true);
        page->addKnob(motion);
        _motion = motion;
    }

    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        metadata.setNComps(-1, 4);
        metadata.setBitDepth(-1, eImageBitDepthFloat);

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
        KnobBoolPtr motion = _motion.lock();

        produced.clear();
        produced.push_back(ImageLayerDesc::getRGBAComponents());
        if (!motion || motion->getValue()) {
            produced.push_back(ImageLayerDesc::getForwardMotionComponents());
        }
        produced.push_back(depthLayer());
        *passThroughTime = time;
        *passThroughView = view;
        *passThroughInputNb = -1;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        KnobIntPtr sceneKnob = _scene.lock();
        KnobDoublePtr uKnob = _u.lock();
        KnobDoublePtr vKnob = _v.lock();
        const int scene = sceneKnob ? sceneKnob->getValue() : 0;
        const float u = uKnob ? (float)uKnob->getValue() : 0.f;
        const float v = vKnob ? (float)vKnob->getValue() : 0.f;

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image) {
                continue;
            }
            if (image->getBitDepth() != eImageBitDepthFloat) {
                return eStatusFailed;
            }
            const std::string& layerID = it->first.getLayerID();
            const bool isMotion = layerID == ImageLayerDesc::getForwardMotionComponents().getLayerID();
            const bool isDepth = layerID == depthLayer().getLayerID();
            const int nComps = (int)image->getComponentsCount();
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    const bool still = (scene == eSceneOcclusion) && kSquare.contains(x, y);
                    float values[4] = { 0.f, 0.f, 0.f, 0.f };
                    if (isMotion) {
                        values[0] = still ? 0.f : u;
                        values[1] = still ? 0.f : v;
                    } else if (isDepth) {
                        values[0] = still ? 1.f : 10.f;
                    } else {
                        sceneColour(scene, x, y, values);
                    }
                    float* pixel = reinterpret_cast<float*>(access.pixelAt(x, y));
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    if (nComps == 1) {
                        pixel[0] = isDepth ? values[0] : values[3];
                        continue;
                    }
                    for (int c = 0; c < nComps; ++c) {
                        pixel[c] = values[c];
                    }
                }
            }
        }

        return eStatusOK;
    }

    KnobIntWPtr _scene;
    KnobDoubleWPtr _u;
    KnobDoubleWPtr _v;
    KnobBoolWPtr _motion;
};

template <typename NODE>
void
registerNode()
{
    EffectInstancePtr node(NODE::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&NODE::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made
    // from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerTestNodes()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNode<VectorBlurSourceTestEffect>();
    });
}

const RectI kWindow(0, 0, kWidth, kHeight);

bool
renderRGBA(const NodePtr& node,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, kWindow, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

const float*
pixelAt(const RenderedPlane& plane,
        int x,
        int y)
{
    const std::size_t n = plane.channels.size();

    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * n];
}

// Bilinear reconstruction of the scene's RGBA around (gx, gy) in pixel-centre coordinates.
void
bilinear(int scene,
         double gx,
         double gy,
         float* out)
{
    const double fx = std::floor(gx);
    const double fy = std::floor(gy);
    const double ax = gx - fx;
    const double ay = gy - fy;
    const int x0 = (int)fx;
    const int y0 = (int)fy;
    const double w[4] = { (1. - ax) * (1. - ay), ax * (1. - ay), (1. - ax) * ay, ax * ay };
    const int px[4] = { x0, x0 + 1, x0, x0 + 1 };
    const int py[4] = { y0, y0, y0 + 1, y0 + 1 };
    double acc[4] = { 0., 0., 0., 0. };

    for (int k = 0; k < 4; ++k) {
        float c[4];
        sceneColour(scene, px[k], py[k], c);
        for (int i = 0; i < 4; ++i) {
            acc[i] += w[k] * c[i];
        }
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = (float)acc[i];
    }
}

void
messageOf(const NodePtr& node,
          QString* message)
{
    int type = 0;

    // getPersistentMessage appends to its out parameter.
    message->clear();
    node->getPersistentMessage(message, &type, false);
}

GoldenVariant
goldenVariant(const std::string& name,
              bool mask,
              const std::function<bool(const GoldenGraph&)>& apply)
{
    GoldenVariant v;

    v.name = name;
    v.mask = mask;
    v.apply = apply;

    return v;
}

} // namespace

class NativeVectorBlurTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        registerTestNodes();
        resetProject();
    }

    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeVectorBlur64x48", 1.));
    }

    NodePtr makeSource(int scene,
                       double u,
                       double v)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDVectorBlurSource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            EXPECT_TRUE(setKnobValues(source, kVBSourceParamScene, { (double)scene }));
            EXPECT_TRUE(setKnobValues(source, kVBSourceParamU, { u }));
            EXPECT_TRUE(setKnobValues(source, kVBSourceParamV, { v }));
        }

        return source;
    }

    NodePtr makeVectorBlur(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_VECTORBLUR), PLUGIN_MAJOR_NATRON_VECTORBLUR);
        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));
        if (node && source) {
            connectNodes(source, node, kVectorBlurInputSource, true);
        }

        return node;
    }

    // `node` over the whole window matches `expected(x, y)`; returns the mismatch count.
    std::size_t expectRGBA(const NodePtr& node,
                           const std::function<void(int, int, float*)>& expected,
                           const ParityTolerance& tolerance,
                           const std::string& what)
    {
        RenderedPlane actual;
        if (!renderRGBA(node, &actual)) {
            ADD_FAILURE() << what << ": render failed";

            return 1;
        }
        std::size_t wrong = 0;
        for (int y = kWindow.y1; y < kWindow.y2; ++y) {
            for (int x = kWindow.x1; x < kWindow.x2; ++x) {
                float want[4];
                expected(x, y, want);
                const float* got = pixelAt(actual, x, y);
                for (int c = 0; c < 4; ++c) {
                    if (!std::isfinite(got[c]) || !tolerance.accepts(want[c], got[c])) {
                        if (wrong == 0) {
                            ADD_FAILURE() << what << ": first mismatch at (" << x << ", " << y << ") channel " << c << ": " << got[c] << " expected " << want[c];
                        }
                        ++wrong;
                        break;
                    }
                }
            }
        }
        EXPECT_EQ(0u, wrong) << what;

        return wrong;
    }
};

TEST_F(NativeVectorBlurTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_VECTORBLUR, PLUGIN_MAJOR_NATRON_VECTORBLUR, "VectorBlur");
}

TEST_F(NativeVectorBlurTest, KnobsAndInputs)
{
    NodePtr node = makeVectorBlur(NodePtr());
    ASSERT_TRUE(bool(node));
    for (const char* name : { kVectorBlurParamUV, kVectorBlurParamOffset, kVectorBlurParamScale, kVectorBlurParamVectorUnits, kVectorBlurParamSamples,
                              kVectorBlurParamMaxSamples, kVectorBlurParamFilter, kVectorBlurParamMaxLength, kVectorBlurParamOcclusion,
                              kVectorBlurParamDepth, kVectorBlurParamDepthUnits }) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }
    EXPECT_EQ(2, node->getNInputs());
    KnobLayerSelect* uv = dynamic_cast<KnobLayerSelect*>(node->getKnobByName(kVectorBlurParamUV).get());
    ASSERT_TRUE(uv != NULL);
    EXPECT_EQ(std::string(kNatronLayerSelectAll), uv->getLayer());
}

// Zero vectors leave every pixel as it is.
TEST_F(NativeVectorBlurTest, ZeroVectorsAreIdentity)
{
    NodePtr source = makeSource(eSceneSmooth, 0., 0.);
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(bool(source) && bool(node));
    expectRGBA(node, [](int x, int y, float* out) { sceneColour(eSceneSmooth, x, y, out); }, ParityTolerance::exact(), "zero vectors");
}

// Without a motion layer the Source passes through.
TEST_F(NativeVectorBlurTest, NoMotionLayerIsIdentity)
{
    NodePtr source = makeSource(eSceneSmooth, 8., 0.);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(setKnobValues(source, kVBSourceParamMotion, { 0. }));
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(bool(node));
    expectRGBA(node, [](int x, int y, float* out) { sceneColour(eSceneSmooth, x, y, out); }, ParityTolerance::exact(), "no motion layer");
}

// A constant horizontal vector of 8 pixels is a box blur of the bilinear Source over the streak
// [x + 0.5 - 4, x + 0.5 + 4]: 16 samples half a pixel apart sample each linear piece at its
// midpoint, so the integral is exact.
TEST_F(NativeVectorBlurTest, ConstantVectorIsABoxBlur)
{
    NodePtr source = makeSource(eSceneSmooth, 8., 0.);
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(bool(source) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kVectorBlurParamSamples, { 16. }));
    const auto box = [](int x, int y, float* out) {
        const int n = 4096;
        double acc[4] = { 0., 0., 0., 0. };
        for (int i = 0; i < n; ++i) {
            // Pixel-centre coordinate of a point of the streak.
            const double s = x + 0.5 - 4. + 8. * (i + 0.5) / n;
            float c[4];
            bilinear(eSceneSmooth, s - 0.5, y, c);
            for (int k = 0; k < 4; ++k) {
                acc[k] += c[k];
            }
        }
        for (int k = 0; k < 4; ++k) {
            out[k] = (float)(acc[k] / n);
        }
    };
    expectRGBA(node, box, ParityTolerance::make(1e-4, 1e-4), "box streak");

    // The same vector in normalised units: 0.125 of the 64 pixel wide format.
    ASSERT_TRUE(setKnobValues(source, kVBSourceParamU, { 0.125 }));
    ASSERT_TRUE(setKnobValue(node, kVectorBlurParamVectorUnits, std::string(kVectorBlurUnitsNormalised)));
    expectRGBA(node, box, ParityTolerance::make(1e-4, 1e-4), "normalised box streak");
}

// Offset 0 streaks a point back along the vector, offset 1 forward.
TEST_F(NativeVectorBlurTest, OffsetPicksTheSide)
{
    NodePtr source = makeSource(eScenePoint, 8., 0.);
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(bool(source) && bool(node));

    for (int side = 0; side < 2; ++side) {
        ASSERT_TRUE(setKnobValues(node, kVectorBlurParamOffset, { (double)side }));
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(node, &actual));
        float before = 0.f;
        float after = 0.f;
        for (int x = 0; x < kWidth; ++x) {
            const float r = pixelAt(actual, x, 24)[0];
            if (x < 32) {
                before += r;
            } else if (x > 32) {
                after += r;
            }
        }
        if (side == 0) {
            EXPECT_GT(before, 0.5f) << "offset 0";
            EXPECT_EQ(0.f, after) << "offset 0";
        } else {
            EXPECT_EQ(0.f, before) << "offset 1";
            EXPECT_GT(after, 0.5f) << "offset 1";
        }
        // Rows away from the point stay black.
        for (int x = 0; x < kWidth; ++x) {
            EXPECT_EQ(0.f, pixelAt(actual, x, 22)[0]) << x;
        }
    }
}

// Depth-aware occlusion keeps the static near square sharp: the far background moving past it
// does not smear its green outward, but for the bilinear fringe of the samples beside it.
// Without occlusion it does.
TEST_F(NativeVectorBlurTest, DepthAwareKeepsANearSquareSharp)
{
    NodePtr source = makeSource(eSceneOcclusion, 8., 0.);
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(bool(source) && bool(node));

    const auto measure = [&](float* bleed, std::size_t* squareWrong, float* stripeBlur) {
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(node, &actual));
        *bleed = 0.f;
        *squareWrong = 0;
        *stripeBlur = 0.f;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const float* got = pixelAt(actual, x, y);
                float want[4];
                sceneColour(eSceneOcclusion, x, y, want);
                if (kSquare.contains(x, y)) {
                    *squareWrong += std::equal(want, want + 4, got) ? 0 : 1;
                } else {
                    *bleed += got[1];
                    *stripeBlur += std::abs(got[0] - want[0]);
                }
            }
        }
    };

    float offBleed = 0.f;
    std::size_t offSquare = 0;
    float offStripes = 0.f;
    measure(&offBleed, &offSquare, &offStripes);
    EXPECT_EQ(0u, offSquare) << "the square's own vectors are zero";
    EXPECT_GT(offBleed, 16.f) << "without occlusion the square smears into the background";

    ASSERT_TRUE(setKnobValue(node, kVectorBlurParamOcclusion, std::string(kVectorBlurOcclusionDepth)));
    float onBleed = 0.f;
    std::size_t onSquare = 0;
    float onStripes = 0.f;
    measure(&onBleed, &onSquare, &onStripes);
    EXPECT_EQ(0u, onSquare);
    EXPECT_LT(onBleed, 0.25f * offBleed) << "occlusion keeps the square's green in";
    EXPECT_GT(onStripes, 0.5f * offStripes) << "the far background still blurs";
}

// Vectors past Max Length are shortened, with a warning that a change to Max Length takes back.
TEST_F(NativeVectorBlurTest, ClampingWarns)
{
    NodePtr source = makeSource(eSceneSmooth, 4., 0.);
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(bool(source) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kVectorBlurParamMaxLength, { 16. }));

    QString message;
    RenderedPlane actual;
    ASSERT_TRUE(renderRGBA(node, &actual));
    messageOf(node, &message);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();

    // 200 pixels shortened to 16 blur like a 16 pixel vector.
    ASSERT_TRUE(setKnobValues(source, kVBSourceParamU, { 200. }));
    RenderedPlane clamped;
    ASSERT_TRUE(renderRGBA(node, &clamped));
    messageOf(node, &message);
    EXPECT_EQ(QString::fromUtf8(VectorBlur::clampedMessage().c_str()), message);

    ASSERT_TRUE(setKnobValues(node, kVectorBlurParamMaxLength, { 256. }));
    messageOf(node, &message);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();

    ASSERT_TRUE(setKnobValues(source, kVBSourceParamU, { 16. }));
    RenderedPlane exact;
    ASSERT_TRUE(renderRGBA(node, &exact));
    messageOf(node, &message);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
    ASSERT_EQ(clamped.pixels.size(), exact.pixels.size());
    for (std::size_t i = 0; i < exact.pixels.size(); ++i) {
        ASSERT_NEAR(exact.pixels[i], clamped.pixels[i], 1e-5) << i;
    }
}

// Regression fixtures on the noisy parity source, its red and green read as vectors in pixels.
TEST_F(NativeVectorBlurTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "VectorBlur";
    spec.pluginID = PLUGINID_NATRON_VECTORBLUR;
    spec.major = PLUGIN_MAJOR_NATRON_VECTORBLUR;
    spec.maskInput = "Mask";

    const std::function<bool(const GoldenGraph&)> vectors = [](const GoldenGraph& g) {
        KnobLayerSelect* uv = dynamic_cast<KnobLayerSelect*>(g.node->getKnobByName(kVectorBlurParamUV).get());
        if (!uv) {
            ADD_FAILURE() << "no uv layer select";

            return false;
        }
        uv->setLayer(kNatronColorViewRGBA);
        uv->setChannels(std::vector<std::string> { "R", "G" });

        return setKnobValues(g.node, kVectorBlurParamScale, { 12. });
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("box", false, vectors));
    v.push_back(goldenVariant("triangle", false, [vectors](const GoldenGraph& g) {
        return vectors(g) && setKnobValue(g.node, kVectorBlurParamFilter, std::string(kVectorBlurFilterTriangle)) && setKnobValues(g.node, kVectorBlurParamOffset, { 0.25 });
    }));
    v.push_back(goldenVariant("occlusion", false, [vectors](const GoldenGraph& g) {
        return vectors(g) && setKnobValue(g.node, kVectorBlurParamOcclusion, std::string(kVectorBlurOcclusionDepth)) && setGoldenChannelSelect(g.node, kVectorBlurParamDepth, "rgba.B");
    }));
    v.push_back(goldenVariant("maskmix", true, [vectors](const GoldenGraph& g) {
        return vectors(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 });
    }));
    runGoldenCases(getApp(), spec, v);
}
