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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <lensfun.h>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/FilterKnobRenderChecks.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Metadata/MetadataModify.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/LensCorrect.h"
#include "Engine/Nodes/Transform/LensCorrectProfile.h"
#include "Engine/Nodes/Transform/LensfunRuntime.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const char* const kCanon5D3 = "Canon EOS 5D Mark III";
const char* const kCanon70200 = "Canon EF 70-200mm f/2.8L IS II USM";

std::shared_ptr<lfLens>
findLens(const char* maker,
         const char* model,
         const char* lensName,
         float* crop)
{
    const lfDatabase* db = LensfunRuntime::database();

    if (!db) {
        return std::shared_ptr<lfLens>();
    }
    std::lock_guard<std::mutex> lock(LensfunRuntime::queryMutex());
    const lfCamera** cameras = db->FindCamerasExt(maker, model);
    if (!cameras) {
        return std::shared_ptr<lfLens>();
    }
    *crop = cameras[0]->CropFactor;
    std::shared_ptr<lfLens> lens;
    const lfLens** lenses = db->FindLenses(cameras[0], nullptr, lensName);
    if (lenses) {
        lens = LensfunRuntime::copyLens(*lenses[0]);
        lf_free(lenses);
    }
    lf_free(cameras);

    return lens;
}

// A rectilinear lens with one ACM distortion calibration: no database lens uses the model.
std::shared_ptr<lfLens>
acmLens()
{
    std::shared_ptr<lfLens> lens = std::make_shared<lfLens>();

    lens->SetMaker("Test");
    lens->SetModel("ACM 50mm f/2");
    lens->AddMount("Canon EF");
    lens->Type = LF_RECTILINEAR;
    lens->MinFocal = 50.f;
    lens->MaxFocal = 50.f;
    lens->MinAperture = 2.f;
    lens->MaxAperture = 22.f;
    lfLensCalibDistortion lcd = {};
    lcd.Model = LF_DIST_MODEL_ACM;
    lcd.Focal = 50.f;
    lcd.RealFocal = 50.f;
    lcd.RealFocalMeasured = false;
    lcd.Terms[0] = -0.02f;
    lcd.Terms[1] = 0.004f;
    lcd.Terms[2] = -0.0005f;
    lcd.Terms[3] = 0.0003f;
    lcd.Terms[4] = -0.0002f;
    lcd.CalibAttr.CropFactor = 1.f;
    lcd.CalibAttr.AspectRatio = 1.5f;
    lens->AddCalibDistortion(&lcd);

    return lens;
}

LensCorrectProfile::Request
request(float focal,
        float crop,
        bool reverse)
{
    LensCorrectProfile::Request r;

    r.width = 6000;
    r.height = 4000;
    r.focal = focal;
    r.crop = crop;
    r.aperture = 2.8f;
    r.distance = 1000.f;
    r.reverse = reverse;

    return r;
}

// lfModifier's auto scale with every requested correction enabled, as darktable asks for it, where
// EnableScaling(0) would see only those enabled before it.
float
lensfunAutoScale(const lfLens& lens,
                 const LensCorrectProfile::Request& r)
{
    lfModifier modifier(&lens, r.focal, r.crop, r.width, r.height, LF_PF_F32, r.reverse);

    if (r.distortion) {
        modifier.EnableDistortionCorrection();
    }
    modifier.EnableProjectionTransform((lfLensType)r.targetGeometry);
    if (r.tca) {
        modifier.EnableTCACorrection();
    }
    if (r.vignetting) {
        modifier.EnableVignettingCorrection(r.aperture, r.distance);
    }

    return modifier.GetAutoScale(r.reverse);
}

std::unique_ptr<lfModifier>
oracle(const lfLens& lens,
       const LensCorrectProfile::Request& r)
{
    std::unique_ptr<lfModifier> modifier(new lfModifier(&lens, r.focal, r.crop, r.width, r.height, LF_PF_F32, r.reverse));

    if (r.distortion) {
        modifier->EnableDistortionCorrection();
    }
    modifier->EnableProjectionTransform((lfLensType)r.targetGeometry);
    modifier->EnableScaling((r.scale == 0.f) ? lensfunAutoScale(lens, r) : r.scale);
    if (r.tca) {
        modifier->EnableTCACorrection();
    }
    if (r.vignetting) {
        modifier->EnableVignettingCorrection(r.aperture, r.distance);
    }

    return modifier;
}

bool
nearlyEqual(float expected,
            float actual,
            float tolerance)
{
    if (std::isnan(expected) || std::isnan(actual)) {
        return std::isnan(expected) && std::isnan(actual);
    }

    return std::fabs(expected - actual) <= tolerance * std::max(1.f, std::fabs(expected));
}

void
expectProfileMatches(const lfLens& lens,
                     const LensCorrectProfile::Request& r,
                     const std::string& what)
{
    const LensCorrectProfile::Profile profile = LensCorrectProfile::build(lens, r);
    std::unique_ptr<lfModifier> modifier = oracle(lens, r);
    std::size_t moved = 0;

    for (int j = 0; j <= 8; ++j) {
        for (int i = 0; i <= 12; ++i) {
            const float x = i * (r.width - 1) / 12.f;
            const float y = j * (r.height - 1) / 8.f;
            float want[6] = { x, y, x, y, x, y };
            modifier->ApplySubpixelGeometryDistortion(x, y, 1, 1, want);
            float got[6];
            profile.map(x, y, got);
            for (int k = 0; k < 6; ++k) {
                EXPECT_TRUE(nearlyEqual(want[k], got[k], 1e-5f)) << what << " at (" << x << ", " << y << ") value " << k << ": lensfun "
                                                                 << want[k] << ", profile " << got[k];
            }
            if ((std::fabs(got[2] - x) > 0.01f) || (std::fabs(got[3] - y) > 0.01f)) {
                ++moved;
            }
            if (r.vignetting) {
                float pixel[3] = { 1.f, 1.f, 1.f };
                modifier->ApplyColorModification(pixel, x, y, 1, 1, LF_CR_3(RED, GREEN, BLUE), 3 * sizeof(float));
                EXPECT_TRUE(nearlyEqual(pixel[1], profile.vignettingGain(x, y), 1e-5f)) << what << " gain at (" << x << ", " << y << ")";
            }
        }
    }
    if (r.distortion || (r.targetGeometry != 0) || r.tca) {
        EXPECT_GT(moved, 0u) << what << ": nothing moved";
    }
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane,
           unsigned mipmapLevel = 0)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
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
    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * 4];
}

std::size_t
nonFinitePixels(const RenderedPlane& plane)
{
    return (std::size_t)std::count_if(plane.pixels.begin(), plane.pixels.end(), [](float v) {
        return !std::isfinite(v);
    });
}

RectD
regionOf(const NodePtr& node,
         unsigned mipmapLevel = 0)
{
    const EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;

    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod, &isProjectFormat));

    return rod;
}

bool
hasColourIn(const RenderedPlane& plane,
            const RectI& area)
{
    for (int y = std::max(area.y1, plane.window.y1); y < std::min(area.y2, plane.window.y2); ++y) {
        for (int x = std::max(area.x1, plane.window.x1); x < std::min(area.x2, plane.window.x2); ++x) {
            const float* p = pixelAt(plane, x, y);
            if ((p[0] != 0.f) || (p[1] != 0.f) || (p[2] != 0.f)) {
                return true;
            }
        }
    }

    return false;
}

std::string
persistentMessage(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return message.toStdString();
}

LensCorrect*
lensCorrectOf(const NodePtr& node)
{
    return node ? dynamic_cast<LensCorrect*>(node->getEffectInstance().get()) : nullptr;
}

template <typename KnobType>
KnobType*
knobOf(const NodePtr& node,
       const std::string& name)
{
    return dynamic_cast<KnobType*>(node->getKnobByName(name).get());
}

void
setMetadata(const NodePtr& modify,
            int slot,
            const std::string& key,
            const std::string& value)
{
    const std::string suffix = std::to_string(slot);
    KnobChoice* op = knobOf<KnobChoice>(modify, "operation" + suffix);
    KnobString* keyKnob = knobOf<KnobString>(modify, "key" + suffix);
    KnobChoice* type = knobOf<KnobChoice>(modify, "type" + suffix);
    KnobString* valueKnob = knobOf<KnobString>(modify, "stringValue" + suffix);

    ASSERT_TRUE(op && keyKnob && type && valueKnob);
    op->setValue((int)MetadataModify::eOperationSet, ViewSpec::all(), 0);
    keyKnob->setValue(key, ViewSpec::all(), 0);
    type->setValue((int)MetadataModify::eValueTypeString, ViewSpec::all(), 0);
    valueKnob->setValue(value, ViewSpec::all(), 0);
}

void
setMetadata(const NodePtr& modify,
            int slot,
            const std::string& key,
            double value)
{
    const std::string suffix = std::to_string(slot);
    KnobChoice* op = knobOf<KnobChoice>(modify, "operation" + suffix);
    KnobString* keyKnob = knobOf<KnobString>(modify, "key" + suffix);
    KnobChoice* type = knobOf<KnobChoice>(modify, "type" + suffix);
    KnobDouble* valueKnob = knobOf<KnobDouble>(modify, "doubleValue" + suffix);

    ASSERT_TRUE(op && keyKnob && type && valueKnob);
    op->setValue((int)MetadataModify::eOperationSet, ViewSpec::all(), 0);
    keyKnob->setValue(key, ViewSpec::all(), 0);
    type->setValue((int)MetadataModify::eValueTypeDouble, ViewSpec::all(), 0);
    valueKnob->setValue(value, ViewSpec::all(), 0);
}

struct LensCase {
    const char* what;
    const char* camera;
    const char* lens;
    double focal;
    const char* mode;
    const char* geometry;
    // 0 for the automatic scale.
    double scale;
};

// Every distortion model and projection lensfun has, both directions, zoomed in and out, and a fisheye
// target scaled down until its corners lie past 90 degrees, where lensfun has no answer.
const LensCase kLensCases[] = {
    { "ptlens correct", kCanon5D3, kCanon70200, 100., kLensCorrectModeCorrect, kLensCorrectGeometryLens, 0. },
    { "ptlens distort", kCanon5D3, kCanon70200, 100., kLensCorrectModeDistort, kLensCorrectGeometryLens, 0. },
    { "ptlens correct scale 1.5", kCanon5D3, kCanon70200, 100., kLensCorrectModeCorrect, kLensCorrectGeometryLens, 1.5 },
    { "ptlens distort scale 0.7", kCanon5D3, kCanon70200, 100., kLensCorrectModeDistort, kLensCorrectGeometryLens, 0.7 },
    { "poly3", "X-Pro2", "XF50mmF2 R WR", 50., kLensCorrectModeCorrect, kLensCorrectGeometryLens, 0. },
    { "poly5 fixed lens", "Canon PowerShot G12", "", 0., kLensCorrectModeCorrect, kLensCorrectGeometryLens, 0. },
    { "fisheye to rectilinear", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeCorrect, kLensCorrectGeometryRectilinear, 0. },
    { "fisheye to equisolid distort", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeDistort, kLensCorrectGeometryEquisolid, 0. },
    { "fisheye to thoby", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeCorrect, kLensCorrectGeometryThoby, 0. },
    { "ptlens to fisheye scale 0.1", kCanon5D3, kCanon70200, 100., kLensCorrectModeCorrect, kLensCorrectGeometryFisheye, 0.1 },
};

std::uint32_t
bitsOf(float v)
{
    std::uint32_t bits = 0;

    std::memcpy(&bits, &v, sizeof(bits));

    return bits;
}

// Renders window whole, then as uneven tiles, each asking the Source for its own region of
// interest, and expects every tile's values to be the whole render's, bit for bit.
void
expectTilesMatchTheWhole(const NodePtr& node,
                         const RectI& window,
                         const std::string& what,
                         unsigned mipmapLevel = 0)
{
    RenderedPlane whole;
    ASSERT_TRUE(renderRGBA(node, window, &whole, mipmapLevel)) << what;
    const double cutsX[] = { 0., 0.21, 0.47, 0.74, 1. };
    const double cutsY[] = { 0., 0.23, 0.58, 1. };
    for (int j = 0; j + 1 < 4; ++j) {
        for (int i = 0; i + 1 < 5; ++i) {
            const RectI tile(window.x1 + (int)(cutsX[i] * window.width()), window.y1 + (int)(cutsY[j] * window.height()),
                             window.x1 + (int)(cutsX[i + 1] * window.width()), window.y1 + (int)(cutsY[j + 1] * window.height()));
            if (tile.isNull()) {
                continue;
            }
            RenderedPlane part;
            ASSERT_TRUE(renderRGBA(node, tile, &part, mipmapLevel)) << what;
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        const float want = pixelAt(whole, x, y)[c];
                        const float got = pixelAt(part, x, y)[c];
                        ASSERT_EQ(bitsOf(want), bitsOf(got)) << what << " mip " << mipmapLevel << " tile [" << tile.x1 << "," << tile.y1 << "," << tile.x2 << ","
                                                             << tile.y2 << ") at " << x << ", " << y << " channel " << c << ": whole " << want << ", tile " << got;
                    }
                }
            }
        }
    }
}

// The region node asks of source for window, in pixels at mipmapLevel.
RectI
sourceRoIOf(const NodePtr& node,
            const NodePtr& source,
            const RectI& window,
            unsigned mipmapLevel = 0)
{
    const EffectInstancePtr effect = node->getEffectInstance();
    RoIMap rois;

    effect->getRegionsOfInterest_public(kTime, RenderScale::fromMipmapLevel(mipmapLevel), regionOf(node, mipmapLevel), window.toCanonical_noClipping(mipmapLevel, 1.), ViewIdx(0), &rois);
    const RoIMap::const_iterator found = rois.find(source->getEffectInstance());
    EXPECT_TRUE(found != rois.end());

    return (found == rois.end()) ? RectI() : found->second.toPixelEnclosing(mipmapLevel, 1.);
}
} // namespace

class NativeLensCorrectTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeLensCorrect64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeLensCorrect(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSCORRECT), PLUGIN_MAJOR_NATRON_LENSCORRECT);
        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));
        if (node && input) {
            connectNodes(input, node, kLensCorrectInputSource, true);
        }

        return node;
    }

    NodePtr makeCanon(const NodePtr& input)
    {
        NodePtr node = makeLensCorrect(input);
        if (node) {
            EXPECT_TRUE(setKnobValue(node, kLensCorrectParamCamera, std::string(kCanon5D3)));
            EXPECT_TRUE(setKnobValue(node, kLensCorrectParamLens, std::string(kCanon70200)));
            EXPECT_TRUE(setKnobValues(node, kLensCorrectParamFocalLength, { 100. }));
            EXPECT_TRUE(setKnobValues(node, kLensCorrectParamAperture, { 2.8 }));
        }

        return node;
    }

    // A Source reaching 16 pixels past a 32 x 24 format on the left and right, 12 below and above.
    NodePtr makeOverscanSource()
    {
        getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth / 2, kParitySourceHeight / 2, "nativeLensCorrect32x24", 1.));
        NodePtr source = makeSource();
        if (source) {
            setParitySourceOrigin(source, -kParitySourceWidth / 4, -kParitySourceHeight / 4);
        }

        return source;
    }

    void setManualScale(const NodePtr& node,
                        const char* mode,
                        double scale)
    {
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamMode, std::string(mode)));
        EXPECT_TRUE(setKnobValues(node, kLensCorrectParamAutoScale, { 0. }));
        EXPECT_TRUE(setKnobValues(node, kLensCorrectParamScale, { scale }));
    }

    void configure(const NodePtr& node,
                   const LensCase& c)
    {
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamCamera, std::string(c.camera)));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamLens, std::string(c.lens)));
        EXPECT_TRUE(setKnobValues(node, kLensCorrectParamFocalLength, { c.focal }));
        EXPECT_TRUE(setKnobValues(node, kLensCorrectParamAperture, { 2.8 }));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamMode, std::string(c.mode)));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamTargetGeometry, std::string(c.geometry)));
        if (c.scale > 0.) {
            setManualScale(node, c.mode, c.scale);
        }
    }

    // A flat Source the size of a 3000 x 2000 format, so that a window is small beside it.
    NodePtr makeLargeSource()
    {
        getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, kLargeWidth, kLargeHeight, "nativeLensCorrect3000x2000", 1.));
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, { 0.5, 0.5, 0.5, 1. }));
            const RectD rod = regionOf(constant);
            EXPECT_EQ(RectD(0, 0, kLargeWidth, kLargeHeight), rod) << rod.x1 << "," << rod.y1 << "," << rod.x2 << "," << rod.y2;
        }

        return constant;
    }

    static constexpr int kLargeWidth = 3000;
    static constexpr int kLargeHeight = 2000;
};

TEST_F(NativeLensCorrectTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_LENSCORRECT, PLUGIN_MAJOR_NATRON_LENSCORRECT, "LensCorrect");
}

TEST_F(NativeLensCorrectTest, EveryControlHasAKnob)
{
    resetProject();
    NodePtr node = makeLensCorrect(NodePtr());
    ASSERT_TRUE(bool(node));
    const char* const names[] = {
        kLensCorrectParamMethod,
        kLensCorrectParamMode,
        kLensCorrectParamCorrectDistortion,
        kLensCorrectParamCorrectTCA,
        kLensCorrectParamCorrectVignetting,
        kLensCorrectParamCamera,
        kLensCorrectParamLens,
        kLensCorrectParamCropFactor,
        kLensCorrectParamFocalLength,
        kLensCorrectParamAperture,
        kLensCorrectParamDistance,
        kLensCorrectParamTargetGeometry,
        kLensCorrectParamAutoScale,
        kLensCorrectParamScale,
        kLensCorrectParamTCAOverride,
        kLensCorrectParamTCARed,
        kLensCorrectParamTCABlue,
        kLensCorrectParamVignetteStrength,
        kLensCorrectParamVignetteRadius,
        kLensCorrectParamVignetteSteepness,
        kLensCorrectParamShowVignetteMask,
        kLensCorrectParamInfo,
        kResamplerParamFilterType,
        kResamplerParamLobes,
        kResamplerParamBicubicB,
        kResamplerParamBicubicC,
        kResamplerParamFilterClamp,
        kResamplerParamFilterBlackOutside,
    };
    for (const char* name : names) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }
    KnobChoice* geometry = knobOf<KnobChoice>(node, kLensCorrectParamTargetGeometry);
    ASSERT_TRUE(geometry != nullptr);
    // The option index is the lfLensType.
    const std::vector<ChoiceOption> options = geometry->getEntries_mt_safe();
    ASSERT_EQ(9u, options.size());
    EXPECT_EQ(std::string(kLensCorrectGeometryRectilinear), options[LF_RECTILINEAR].id);
    EXPECT_EQ(std::string(kLensCorrectGeometryEquirectangular), options[LF_EQUIRECTANGULAR].id);
    EXPECT_EQ(std::string(kLensCorrectGeometryThoby), options[LF_FISHEYE_THOBY].id);
}

TEST_F(NativeLensCorrectTest, PTLensPoly3TcaAndVignettingMatchLensfun)
{
    float crop = 0.f;
    std::shared_ptr<lfLens> lens = findLens("Canon", kCanon5D3, kCanon70200, &crop);
    ASSERT_TRUE(bool(lens));
    for (bool reverse : { false, true }) {
        LensCorrectProfile::Request r = request(100.f, crop, reverse);
        r.scale = 1.f;
        expectProfileMatches(*lens, r, reverse ? "70-200 distort" : "70-200 correct");
    }
}

TEST_F(NativeLensCorrectTest, Poly3MatchesLensfun)
{
    float crop = 0.f;
    std::shared_ptr<lfLens> lens = findLens("Fujifilm", "X-Pro2", "XF50mmF2 R WR", &crop);
    ASSERT_TRUE(bool(lens));
    for (bool reverse : { false, true }) {
        LensCorrectProfile::Request r = request(50.f, crop, reverse);
        r.scale = 1.f;
        expectProfileMatches(*lens, r, reverse ? "XF50 distort" : "XF50 correct");
    }
}

TEST_F(NativeLensCorrectTest, Poly5MatchesLensfun)
{
    float crop = 0.f;
    std::shared_ptr<lfLens> lens = findLens("Canon", "Canon PowerShot G12", "", &crop);
    ASSERT_TRUE(bool(lens));
    for (bool reverse : { false, true }) {
        LensCorrectProfile::Request r = request(lens->MinFocal, crop, reverse);
        r.scale = 1.f;
        expectProfileMatches(*lens, r, reverse ? "G12 distort" : "G12 correct");
    }
}

TEST_F(NativeLensCorrectTest, AcmMatchesLensfun)
{
    std::shared_ptr<lfLens> lens = acmLens();
    // Lensfun has no reverse ACM.
    LensCorrectProfile::Request r = request(50.f, 1.f, false);
    r.scale = 1.f;
    expectProfileMatches(*lens, r, "ACM correct");
}

TEST_F(NativeLensCorrectTest, ProjectionsMatchLensfun)
{
    float crop = 0.f;
    std::shared_ptr<lfLens> lens = findLens("Canon", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", &crop);
    ASSERT_TRUE(bool(lens));
    ASSERT_EQ(LF_FISHEYE, lens->Type);
    for (int geometry = LF_RECTILINEAR; geometry <= LF_FISHEYE_THOBY; ++geometry) {
        for (bool reverse : { false, true }) {
            LensCorrectProfile::Request r = request(16.f, crop, reverse);
            r.targetGeometry = geometry;
            r.tca = false;
            r.vignetting = false;
            r.scale = 1.f;
            expectProfileMatches(*lens, r, "Zenitar to " + std::to_string(geometry) + (reverse ? " distort" : " correct"));
        }
    }
}

// The auto scale lensfun picks, applied as darktable applies it, leaves no empty border: every
// edge pixel of the corrected image maps inside the source.
TEST_F(NativeLensCorrectTest, AutoScaleKeepsTheEdgesInside)
{
    float crop = 0.f;
    std::shared_ptr<lfLens> lens = findLens("Canon", kCanon5D3, kCanon70200, &crop);
    ASSERT_TRUE(bool(lens));
    LensCorrectProfile::Request r = request(70.f, crop, false);
    r.scale = 0.f;
    // Lensfun keeps a margin of 0.2% of the radius, which is under a pixel at this size.
    r.width = 600;
    r.height = 400;
    EXPECT_FLOAT_EQ(lensfunAutoScale(*lens, r), LensCorrectProfile::autoScale(*lens, r));
    expectProfileMatches(*lens, r, "70-200 auto scale");
    const LensCorrectProfile::Profile profile = LensCorrectProfile::build(*lens, r);
    const float slack = 1.f;
    float closest = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 2 * (r.width + r.height); ++i) {
        float x, y;
        if (i < r.width) {
            x = (float)i;
            y = 0.f;
        } else if (i < 2 * r.width) {
            x = (float)(i - r.width);
            y = (float)(r.height - 1);
        } else if (i < 2 * r.width + r.height) {
            x = 0.f;
            y = (float)(i - 2 * r.width);
        } else {
            x = (float)(r.width - 1);
            y = (float)(i - 2 * r.width - r.height);
        }
        float out[6];
        profile.map(x, y, out);
        for (int c = 0; c < 3; ++c) {
            EXPECT_GE(out[2 * c], -slack) << x << ", " << y;
            EXPECT_LE(out[2 * c], r.width - 1 + slack) << x << ", " << y;
            EXPECT_GE(out[2 * c + 1], -slack) << x << ", " << y;
            EXPECT_LE(out[2 * c + 1], r.height - 1 + slack) << x << ", " << y;
        }
        closest = std::min({ closest, out[2], r.width - 1 - out[2], out[3], r.height - 1 - out[3] });
    }
    // Not scaled further than it takes: an edge still reaches the boundary.
    EXPECT_LE(closest, 1.5f);
}

TEST_F(NativeLensCorrectTest, VignettingGainSurvivesLeavingTheCalibration)
{
    LensCorrectProfile::Profile profile;
    const float huge = 1.6e16f;

    profile.vignetting = LensCorrectProfile::VignettingOp::Devignette;
    profile.vignettingTerms[0] = -1.f;
    EXPECT_FLOAT_EQ(1.f / 0.75f, profile.vignettingGain(0.5f, 0.f));
    // The falloff at zero, then negative.
    EXPECT_FLOAT_EQ(1.f, profile.vignettingGain(1.f, 0.f));
    EXPECT_FLOAT_EQ(1.f, profile.vignettingGain(2.f, 0.f));
    profile.vignetting = LensCorrectProfile::VignettingOp::Vignette;
    EXPECT_FLOAT_EQ(0.75f, profile.vignettingGain(0.5f, 0.f));
    EXPECT_FLOAT_EQ(0.f, profile.vignettingGain(2.f, 0.f));

    // r^4 and r^6 overflow with opposite signs.
    profile.vignettingTerms[0] = 1.f;
    profile.vignettingTerms[1] = -1.f;
    profile.vignettingTerms[2] = 1.f;
    for (LensCorrectProfile::VignettingOp op : { LensCorrectProfile::VignettingOp::Devignette, LensCorrectProfile::VignettingOp::Vignette }) {
        profile.vignetting = op;
        EXPECT_FLOAT_EQ(1.f, profile.vignettingGain(huge, huge));
        EXPECT_FLOAT_EQ(1.f, profile.vignettingGain(std::numeric_limits<float>::quiet_NaN(), 0.f));
    }
}

// Scaled far down, the corners of a fisheye target lie past 90 degrees, where lensfun answers kHuge.
TEST_F(NativeLensCorrectTest, HugePositionsRenderFinite)
{
    float crop = 0.f;
    std::shared_ptr<lfLens> lens = findLens("Canon", kCanon5D3, kCanon70200, &crop);
    ASSERT_TRUE(bool(lens));
    LensCorrectProfile::Request r = request(100.f, crop, false);
    r.width = kParitySourceWidth;
    r.height = kParitySourceHeight;
    r.targetGeometry = LF_FISHEYE;
    r.scale = 0.1f;
    const LensCorrectProfile::Profile profile = LensCorrectProfile::build(*lens, r);
    float corner[6];
    profile.map(0.f, 0.f, corner);
    // kHuge, or past float's range once it has been through the distortion polynomial.
    ASSERT_FALSE(std::isnan(corner[2]));
    ASSERT_GE(std::fabs(corner[2]), 1e15f);
    EXPECT_TRUE(std::isfinite(profile.vignettingGain(corner[2], corner[3])));

    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamTargetGeometry, std::string(kLensCorrectGeometryFisheye)));
    setManualScale(node, kLensCorrectModeCorrect, 0.1);
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    for (double blackOutside : { 1., 0. }) {
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { blackOutside }));
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(node, window, &out));
        EXPECT_EQ(0u, nonFinitePixels(out)) << "black outside " << blackOutside;
        const RectD rod = regionOf(node);
        EXPECT_FALSE(rod.isNull());
        for (double edge : { rod.x1, rod.y1, rod.x2, rod.y2 }) {
            EXPECT_LT(std::fabs(edge), 1e10) << "black outside " << blackOutside;
        }
    }
}

// Vignetting is evaluated at Source positions well past the frame's corner.
TEST_F(NativeLensCorrectTest, OverscanSourceRendersFinite)
{
    resetProject();
    NodePtr source = makeOverscanSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));

    const RectD sourceRoD = regionOf(source);
    const RectD rod = regionOf(node);
    EXPECT_LE(rod.x1, sourceRoD.x1);
    EXPECT_LE(rod.y1, sourceRoD.y1);
    EXPECT_GE(rod.x2, sourceRoD.x2);
    EXPECT_GE(rod.y2, sourceRoD.y2);

    const RectI window = rod.toPixelEnclosing(0u, 1.);
    RenderedPlane out;
    ASSERT_TRUE(renderRGBA(node, window, &out));
    EXPECT_EQ(0u, nonFinitePixels(out));
    EXPECT_TRUE(hasColourIn(out, RectI(window.x1, window.y1, 0, window.y2)));
}

// Output pixels of a mipmap level average the scale 1 pixels they cover, a half pixel of error at the
// level being several times the tolerance on the Source's ramps. The Source's own level is taken out:
// it evaluates each level's pixel off its centre.
TEST_F(NativeLensCorrectTest, ScaledRendersMatchScaleOne)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    setParitySourceNoise(source, false);
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));
    const RectI fullWindow = paritySourceWindow(source, kTime, 0);
    RenderedPlane sourceFull;
    ASSERT_TRUE(renderRGBA(source, fullWindow, &sourceFull));

    const auto boxAverage = [](const RenderedPlane& plane, const RectI& box, int channel) {
        float sum = 0.f;
        for (int y = box.y1; y < box.y2; ++y) {
            for (int x = box.x1; x < box.x2; ++x) {
                sum += pixelAt(plane, x, y)[channel];
            }
        }

        return sum / (float)box.area();
    };
    for (const char* mode : { kLensCorrectModeCorrect, kLensCorrectModeDistort }) {
        ASSERT_TRUE(setKnobValue(node, kLensCorrectParamMode, std::string(mode)));
        RenderedPlane full;
        ASSERT_TRUE(renderRGBA(node, fullWindow, &full));
        EXPECT_EQ(0u, nonFinitePixels(full)) << mode;
        for (unsigned mip : { 1u, 2u }) {
            const int step = 1 << mip;
            const float tolerance = 0.01f * step / 2;
            const int border = 4 * step;
            const RectI window = paritySourceWindow(source, kTime, mip);
            RenderedPlane scaled, sourceScaled;
            ASSERT_TRUE(renderRGBA(node, window, &scaled, mip));
            ASSERT_TRUE(renderRGBA(source, window, &sourceScaled, mip));
            EXPECT_EQ(0u, nonFinitePixels(scaled)) << mode << " mip " << mip;
            int compared = 0;
            for (int y = window.y1; y < window.y2; ++y) {
                for (int x = window.x1; x < window.x2; ++x) {
                    const RectI covered(x * step, y * step, (x + 1) * step, (y + 1) * step);
                    if ((covered.x1 < fullWindow.x1 + border) || (covered.y1 < fullWindow.y1 + border) || (covered.x2 > fullWindow.x2 - border) || (covered.y2 > fullWindow.y2 - border)) {
                        continue;
                    }
                    ++compared;
                    for (int c = 0; c < 3; ++c) {
                        const float sourceOffset = pixelAt(sourceScaled, x, y)[c] - boxAverage(sourceFull, covered, c);
                        ASSERT_NEAR(boxAverage(full, covered, c) + sourceOffset, pixelAt(scaled, x, y)[c], tolerance) << mode << " mip " << mip << " at " << x << ", " << y << " channel " << c;
                    }
                }
            }
            EXPECT_GT(compared, 0) << mode << " mip " << mip;
        }
    }
}

// Outside the box the map lands outside the Source, so nothing the model can show is cut off.
TEST_F(NativeLensCorrectTest, OutputBoxBoundsTheImageOfTheSource)
{
    struct Case {
        const char* what;
        const char* maker;
        const char* camera;
        const char* lens;
        float focal;
        int geometry;
    };
    const Case cases[] = {
        { "ptlens", "Canon", kCanon5D3, kCanon70200, 100.f, LF_UNKNOWN },
        { "poly3", "Fujifilm", "X-Pro2", "XF50mmF2 R WR", 50.f, LF_UNKNOWN },
        { "fisheye to rectilinear", "Canon", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16.f, LF_RECTILINEAR },
        { "fisheye to equisolid", "Canon", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16.f, LF_FISHEYE_EQUISOLID },
    };
    const auto check = [](const lfLens& lens, const LensCorrectProfile::Request& r, const std::string& what) {
        const float w = (float)r.width;
        const float h = (float)r.height;
        const LensCorrectProfile::MappedBox box = LensCorrectProfile::outputBox(lens, r, -0.5, -0.5, w - 0.5, h - 0.5);
        ASSERT_TRUE(box.any) << what;
        if (!box.complete) {
            return;
        }
        const LensCorrectProfile::Profile profile = LensCorrectProfile::build(lens, r);
        const float margin = 2.f;
        const int steps = 64;
        for (int i = 0; i <= steps; ++i) {
            const float across = (float)box.x1 - margin + (float)(box.x2 - box.x1 + 2 * margin) * i / steps;
            const float down = (float)box.y1 - margin + (float)(box.y2 - box.y1 + 2 * margin) * i / steps;
            const float points[4][2] = {
                { across, (float)box.y1 - margin },
                { across, (float)box.y2 + margin },
                { (float)box.x1 - margin, down },
                { (float)box.x2 + margin, down },
            };
            for (const auto& point : points) {
                float out[6];
                profile.map(point[0], point[1], out);
                const bool outside = !std::isfinite(out[2]) || !std::isfinite(out[3]) || (out[2] < -0.5f) || (out[2] > w - 0.5f) || (out[3] < -0.5f) || (out[3] > h - 0.5f);
                EXPECT_TRUE(outside) << what << " at " << point[0] << ", " << point[1] << " maps to " << out[2] << ", " << out[3];
            }
        }
    };
    for (const Case& c : cases) {
        float crop = 0.f;
        std::shared_ptr<lfLens> lens = findLens(c.maker, c.camera, c.lens, &crop);
        ASSERT_TRUE(bool(lens)) << c.what;
        for (bool reverse : { false, true }) {
            for (float scale : { 0.f, 0.7f, 1.f, 1.5f }) {
                LensCorrectProfile::Request r = request(c.focal, crop, reverse);
                r.targetGeometry = c.geometry;
                r.scale = scale;
                check(*lens, r, std::string(c.what) + (reverse ? " distort" : " correct") + " scale " + std::to_string(scale));
            }
        }
    }

    // Correcting through ACM has no opposite step, distorting no ACM step at all.
    std::shared_ptr<lfLens> acm = acmLens();
    LensCorrectProfile::Request r = request(50.f, 1.f, false);
    r.scale = 1.f;
    check(*acm, r, "ACM correct");
    const LensCorrectProfile::MappedBox corrected = LensCorrectProfile::outputBox(*acm, r, -0.5, -0.5, r.width - 0.5, r.height - 0.5);
    EXPECT_GT(std::fabs(corrected.x1 + 0.5), 1.);
    r.reverse = true;
    const LensCorrectProfile::MappedBox distorted = LensCorrectProfile::outputBox(*acm, r, -0.5, -0.5, r.width - 0.5, r.height - 0.5);
    EXPECT_NEAR(-0.5, distorted.x1, 1e-2);
    EXPECT_NEAR(r.width - 0.5, distorted.x2, 1e-2);
}

// Scale 1.5 correcting and 0.7 distorting both push pixels out, past a Source the size of the format.
TEST_F(NativeLensCorrectTest, RegionOfDefinitionFollowsThePixels)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));
    const RectD sourceRoD = regionOf(source);

    struct Case {
        const char* mode;
        double scale;
        double pastLeft;
    };
    const Case cases[] = { { kLensCorrectModeCorrect, 1.5, 16. }, { kLensCorrectModeDistort, 0.7, 13.7 } };
    for (const Case& c : cases) {
        setManualScale(node, c.mode, c.scale);
        const RectD rod = regionOf(node);
        const double pastTop = c.pastLeft * kParitySourceHeight / kParitySourceWidth;
        const double tolerance = 6.;
        EXPECT_NEAR(sourceRoD.x1 - c.pastLeft, rod.x1, tolerance) << c.mode;
        EXPECT_NEAR(sourceRoD.x2 + c.pastLeft, rod.x2, tolerance) << c.mode;
        EXPECT_NEAR(sourceRoD.y1 - pastTop, rod.y1, tolerance) << c.mode;
        EXPECT_NEAR(sourceRoD.y2 + pastTop, rod.y2, tolerance) << c.mode;

        for (unsigned mip : { 0u, 1u, 2u }) {
            const RectD scaledRoD = regionOf(node, mip);
            EXPECT_DOUBLE_EQ(rod.x1, scaledRoD.x1) << c.mode << " mip " << mip;
            EXPECT_DOUBLE_EQ(rod.y1, scaledRoD.y1) << c.mode << " mip " << mip;
            EXPECT_DOUBLE_EQ(rod.x2, scaledRoD.x2) << c.mode << " mip " << mip;
            EXPECT_DOUBLE_EQ(rod.y2, scaledRoD.y2) << c.mode << " mip " << mip;

            const RectI window = scaledRoD.toPixelEnclosing(mip, 1.);
            RenderedPlane out;
            ASSERT_TRUE(renderRGBA(node, window, &out, mip)) << c.mode << " mip " << mip;
            EXPECT_EQ(0u, nonFinitePixels(out)) << c.mode << " mip " << mip;
            EXPECT_TRUE(hasColourIn(out, RectI(window.x1, window.y1, 0, window.y2))) << c.mode << " mip " << mip;
            EXPECT_TRUE(hasColourIn(out, RectI((kParitySourceWidth >> mip), window.y1, window.x2, window.y2))) << c.mode << " mip " << mip;
            if (mip == 0) {
                // The region holds all of it: its margin leaves the outermost row past the Source's fade.
                EXPECT_FALSE(hasColourIn(out, RectI(window.x1, window.y1, window.x2, window.y1 + 1))) << c.mode;
            }
        }
    }

    // Scaled in, the image and its region shrink; clamped edges or a partial mix keep the Source's.
    setManualScale(node, kLensCorrectModeCorrect, 0.5);
    const RectD inside = regionOf(node);
    EXPECT_GT(inside.x1, sourceRoD.x1 + 8.);
    EXPECT_LT(inside.x2, sourceRoD.x2 - 8.);
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { 0. }));
    EXPECT_DOUBLE_EQ(sourceRoD.x1, regionOf(node).x1);
    EXPECT_DOUBLE_EQ(sourceRoD.x2, regionOf(node).x2);
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    EXPECT_DOUBLE_EQ(sourceRoD.x1, regionOf(node).x1);
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 1. }));

    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamMethod, std::string(kLensCorrectMethodVignette)));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteStrength, { 0.5 }));
    EXPECT_DOUBLE_EQ(sourceRoD.x1, regionOf(node).x1);
    EXPECT_DOUBLE_EQ(sourceRoD.x2, regionOf(node).x2);
}

TEST_F(NativeLensCorrectTest, ClearedWarningReturnsOnTheNextRender)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamCamera, std::string(kCanon5D3)));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamLens, std::string("No Such Lens 13mm f/9")));
    // The manual vignette keeps the node rendering: a pass-through can be answered from the action caches.
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteStrength, { 0.5 }));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane out;
    ASSERT_TRUE(renderRGBA(node, window, &out));
    const std::string warning = persistentMessage(node);
    EXPECT_NE(std::string::npos, warning.find("not in the lensfun database")) << warning;

    node->clearPersistentMessage(false);
    EXPECT_EQ(std::string(), persistentMessage(node));
    ASSERT_TRUE(renderRGBA(node, window, &out));
    EXPECT_EQ(warning, persistentMessage(node));

    // Another message that replaced the warning stays.
    node->setPersistentMessage(eMessageTypeWarning, "another message");
    ASSERT_TRUE(renderRGBA(node, window, &out));
    EXPECT_EQ(std::string("another message"), persistentMessage(node));

    node->clearPersistentMessage(false);
    ASSERT_TRUE(renderRGBA(node, window, &out));
    EXPECT_EQ(warning, persistentMessage(node));
    node->disconnectInput(kLensCorrectInputSource);
    EXPECT_EQ(std::string(), persistentMessage(node));
}

// The crop factor takes every calibration of the lens out of reach.
TEST_F(NativeLensCorrectTest, NoApplicableCalibrationWarnsAndPassesThrough)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane before, after;
    ASSERT_TRUE(renderRGBA(source, window, &before));

    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamCropFactor, { 0.5 }));
    ASSERT_TRUE(renderRGBA(node, window, &after));
    ASSERT_EQ(before.pixels.size(), after.pixels.size());
    for (std::size_t i = 0; i < before.pixels.size(); ++i) {
        ASSERT_EQ(before.pixels[i], after.pixels[i]) << "float " << i;
    }
    std::string warning = persistentMessage(node);
    EXPECT_NE(std::string::npos, warning.find("no distortion, TCA, vignetting calibration")) << warning;

    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamCorrectTCA, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamCorrectVignetting, { 0. }));
    ASSERT_TRUE(renderRGBA(node, window, &after));
    warning = persistentMessage(node);
    EXPECT_NE(std::string::npos, warning.find("no distortion calibration")) << warning;
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamCorrectDistortion, { 0. }));
    ASSERT_TRUE(renderRGBA(node, window, &after));
    EXPECT_EQ(std::string(), persistentMessage(node));
}

// A custom TCA needs no calibration, so distortion and vignetting are the only ones missing.
TEST_F(NativeLensCorrectTest, PartlyCalibratedLensNamesTheSkippedInInfo)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane out;

    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamTCAOverride, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamTCARed, { 1.001 }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamCropFactor, { 0.5 }));
    ASSERT_TRUE(renderRGBA(node, window, &out));
    EXPECT_EQ(std::string(), persistentMessage(node));
    KnobString* info = knobOf<KnobString>(node, kLensCorrectParamInfo);
    ASSERT_TRUE(info != nullptr);
    const std::string text = info->getValue();
    EXPECT_NE(std::string::npos, text.find("not corrected: distortion, vignetting")) << text;
    EXPECT_EQ(std::string::npos, text.find("TCA,")) << text;
}

TEST_F(NativeLensCorrectTest, UnknownLensPassesThroughWithAMessage)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamCamera, std::string(kCanon5D3)));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamLens, std::string("No Such Lens 13mm f/9")));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane before, after;
    ASSERT_TRUE(renderRGBA(source, window, &before));
    ASSERT_TRUE(renderRGBA(node, window, &after));
    ASSERT_EQ(before.pixels.size(), after.pixels.size());
    EXPECT_TRUE(before.pixels == after.pixels);
    EXPECT_NE(std::string::npos, persistentMessage(node).find("not in the lensfun database")) << persistentMessage(node);

    // A lens that is found takes the message back.
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamLens, std::string(kCanon70200)));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamFocalLength, { 100. }));
    ASSERT_TRUE(renderRGBA(node, window, &after));
    EXPECT_EQ(std::string(), persistentMessage(node));
    EXPECT_FALSE(before.pixels == after.pixels);
}

TEST_F(NativeLensCorrectTest, HonoursTheNewFiltersAndTheirParameters)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    EXPECT_TRUE(filterChoiceChangesTheRender(node, window, kTime, Resampler::eFilterCubic, Resampler::eFilterLanczos3));
    EXPECT_TRUE(sincLobesChangeTheRender(node, window, kTime));
    EXPECT_TRUE(bicubicParametersChangeTheRender(node, window, kTime));
}

TEST_F(NativeLensCorrectTest, TilesMatchTheWholeFrame)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteStrength, { 0.5 }));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane whole;
    ASSERT_TRUE(renderRGBA(node, window, &whole));
    const int midX = (window.x1 + window.x2) / 2 + 3;
    const int midY = (window.y1 + window.y2) / 2 - 5;
    const RectI tiles[] = {
        RectI(window.x1, window.y1, midX, midY),
        RectI(midX, window.y1, window.x2, midY),
        RectI(window.x1, midY, midX, window.y2),
        RectI(midX, midY, window.x2, window.y2),
    };
    for (const RectI& tile : tiles) {
        RenderedPlane part;
        ASSERT_TRUE(renderRGBA(node, tile, &part));
        for (int y = tile.y1; y < tile.y2; ++y) {
            for (int x = tile.x1; x < tile.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    ASSERT_FLOAT_EQ(pixelAt(whole, x, y)[c], pixelAt(part, x, y)[c]) << x << ", " << y << " channel " << c;
                }
            }
        }
    }
}

// Each tile asks the Source for only the region its pixels map from, and still renders exactly what
// the whole frame does: every filter tap, edge clamp and outside test lands as it would on the whole
// Source.
TEST_F(NativeLensCorrectTest, TilesAreBitIdenticalToTheWholeFrame)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    for (const LensCase& c : kLensCases) {
        NodePtr node = makeLensCorrect(source);
        ASSERT_TRUE(bool(node)) << c.what;
        configure(node, c);
        // A partial mix renders the whole region, past the Source where it has grown.
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.75 }));
        expectTilesMatchTheWhole(node, regionOf(node).toPixelEnclosing(0u, 1.), c.what);
    }

    NodePtr node = makeLensCorrect(source);
    ASSERT_TRUE(bool(node));
    configure(node, kLensCases[6]);
    const RectI window = regionOf(node).toPixelEnclosing(0u, 1.);
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(Resampler::filterOptions()[(std::size_t)Resampler::eFilterLanczos3].id)));
    expectTilesMatchTheWhole(node, window, "fisheye to rectilinear, lanczos3");
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { 0. }));
    expectTilesMatchTheWhole(node, window, "fisheye to rectilinear, lanczos3, edges clamped");
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(Resampler::filterOptions()[(std::size_t)Resampler::eFilterImpulse].id)));
    expectTilesMatchTheWhole(node, window, "fisheye to rectilinear, impulse, edges clamped");

    NodePtr scaled = makeLensCorrect(source);
    ASSERT_TRUE(bool(scaled));
    configure(scaled, kLensCases[3]);
    ASSERT_TRUE(setKnobValues(scaled, kResamplerParamFilterBlackOutside, { 0. }));
    expectTilesMatchTheWhole(scaled, regionOf(scaled, 1).toPixelEnclosing(1u, 1.), "ptlens distort scale 0.7, edges clamped", 1);
}

// Every Source pixel a cubic filter can read for a window, mapped pixel by pixel through lensfun's
// model, lies in the region the node asks for: the footprint around each red, green and blue position,
// widened by the kernel scale the Jacobian gives there, and moved onto the Source's edge when the
// position lies past it, as the Resampler clamps it.
TEST_F(NativeLensCorrectTest, RegionOfInterestHoldsEveryRead)
{
    resetProject();
    NodePtr source = makeLargeSource();
    ASSERT_TRUE(bool(source));
    for (const LensCase& c : kLensCases) {
        NodePtr node = makeLensCorrect(source);
        ASSERT_TRUE(bool(node)) << c.what;
        configure(node, c);
        LensCorrect* effect = lensCorrectOf(node);
        ASSERT_TRUE(effect != nullptr);
        const LensCorrect::Lookup found = effect->lookup(kTime, ViewIdx(0));
        ASSERT_TRUE(bool(found.lens)) << c.what << ": " << found.warning;
        KnobChoice* geometry = knobOf<KnobChoice>(node, kLensCorrectParamTargetGeometry);
        ASSERT_TRUE(geometry != nullptr);

        LensCorrectProfile::Request r;
        r.width = kLargeWidth;
        r.height = kLargeHeight;
        r.focal = found.focal;
        r.crop = found.crop;
        r.aperture = found.aperture;
        r.distance = found.distance;
        r.reverse = (std::string(c.mode) == kLensCorrectModeDistort);
        r.tca = !found.monochrome;
        r.targetGeometry = geometry->getValueAtTime(kTime, 0, ViewIdx(0));
        r.scale = (float)c.scale;
        const LensCorrectProfile::Profile profile = LensCorrectProfile::build(*found.lens, r);

        for (unsigned mip : { 0u, 3u }) {
            const double scale = 1. / (1 << mip);
            const int width = (int)(kLargeWidth * scale);
            const int height = (int)(kLargeHeight * scale);
            const RectI rod(0, 0, width, height);
            const auto map = [&](int px, int py, float s[6]) {
                const float xu = (float)((px + 0.5) / scale - 0.5);
                const float yu = (float)(kLargeHeight - 0.5 - (py + 0.5) / scale);
                float lf[6];
                profile.map(xu, yu, lf);
                for (int k = 0; k < 3; ++k) {
                    s[2 * k] = (float)((lf[2 * k] + 0.5) * scale);
                    s[2 * k + 1] = (float)((kLargeHeight - 0.5 - lf[2 * k + 1]) * scale);
                }
            };
            std::vector<RectI> windows = {
                RectI(0, 0, 160, 120),
                RectI(width - 160, height - 120, width, height),
                RectI(width / 2 - 80, height / 2 - 60, width / 2 + 80, height / 2 + 60),
                RectI(0, height / 2 - 60, 160, height / 2 + 60),
                RectI(0, height / 3, width, height / 3 + 3),
                RectI(2 * width / 3, 0, 2 * width / 3 + 3, height),
            };
            if (mip != 0) {
                windows.push_back(rod);
            }
            for (const RectI& window : windows) {
                const RectI roi = sourceRoIOf(node, source, window, mip).intersect(rod);
                std::size_t misses = 0;
                std::string first;
                for (int y = window.y1; y < window.y2; ++y) {
                    for (int x = window.x1; x < window.x2; ++x) {
                        float s[6], left[6], right[6], below[6], above[6];
                        map(x, y, s);
                        map(x - 1, y, left);
                        map(x + 1, y, right);
                        map(x, y - 1, below);
                        map(x, y + 1, above);
                        const double kernelX = std::max(1., std::hypot(0.5 * (right[2] - left[2]), 0.5 * (above[2] - below[2])));
                        const double kernelY = std::max(1., std::hypot(0.5 * (right[3] - left[3]), 0.5 * (above[3] - below[3])));
                        for (int k = 0; k < 3; ++k) {
                            const double sx = s[2 * k];
                            const double sy = s[2 * k + 1];
                            if (!std::isfinite(sx) || !std::isfinite(sy)) {
                                continue;
                            }
                            // A Jacobian with no answer may scale the kernel to anything.
                            const double reachX = std::isfinite(kernelX) ? 2. * kernelX + 1.5 : 1e12;
                            const double reachY = std::isfinite(kernelY) ? 2. * kernelY + 1.5 : 1e12;
                            const auto onRoD = [](double v, int lo, int hi) {
                                return (int)std::min<double>(std::max<double>(std::floor(v), lo), hi - 1);
                            };
                            const int x1 = onRoD(sx - reachX, rod.x1, rod.x2);
                            const int x2 = onRoD(sx + reachX, rod.x1, rod.x2);
                            const int y1 = onRoD(sy - reachY, rod.y1, rod.y2);
                            const int y2 = onRoD(sy + reachY, rod.y1, rod.y2);
                            if ((x1 < roi.x1) || (x2 >= roi.x2) || (y1 < roi.y1) || (y2 >= roi.y2)) {
                                if (misses++ == 0) {
                                    std::ostringstream text;
                                    text << "pixel " << x << ", " << y << " channel " << k << " reads [" << x1 << "," << y1 << " - " << x2 << "," << y2 << "] outside ["
                                         << roi.x1 << "," << roi.y1 << "," << roi.x2 << "," << roi.y2 << ")";
                                    first = text.str();
                                }
                            }
                        }
                    }
                }
                EXPECT_EQ(0u, misses) << c.what << " mip " << mip << " window [" << window.x1 << "," << window.y1 << "," << window.x2 << "," << window.y2 << "): " << first;
            }
        }
    }
}

// A window a fraction of a large Source asks for a fraction of it, at full and reduced scale, even
// through a fisheye: only a window reaching where lensfun has no answer needs the whole Source.
TEST_F(NativeLensCorrectTest, SmallWindowAsksForLittleOfTheSource)
{
    resetProject();
    NodePtr source = makeLargeSource();
    ASSERT_TRUE(bool(source));
    for (int index : { 0, 1, 3, 4, 5, 6, 7 }) {
        const LensCase& c = kLensCases[index];
        NodePtr node = makeLensCorrect(source);
        ASSERT_TRUE(bool(node)) << c.what;
        configure(node, c);
        for (unsigned mip : { 0u, 2u }) {
            const int width = kLargeWidth >> mip;
            const int height = kLargeHeight >> mip;
            const RectI rod(0, 0, width, height);
            for (const RectI& window : { RectI(width / 2 - 32, height / 2 - 32, width / 2 + 32, height / 2 + 32),
                                         RectI(width / 3, height / 4, width / 3 + 64, height / 4 + 64) }) {
                const RectI roi = sourceRoIOf(node, source, window, mip).intersect(rod);
                EXPECT_TRUE(roi.contains(window)) << c.what << " mip " << mip;
                EXPECT_LT(roi.area(), rod.area() / 20) << c.what << " mip " << mip << ": [" << roi.x1 << "," << roi.y1 << "," << roi.x2 << "," << roi.y2 << ")";
            }
        }
    }

    // Past 90 degrees off the axis a fisheye has no answer, and a window there reads the whole Source.
    NodePtr node = makeLensCorrect(source);
    ASSERT_TRUE(bool(node));
    configure(node, kLensCases[9]);
    const RectI corner(0, 0, 64, 64);
    EXPECT_EQ(RectI(0, 0, kLargeWidth, kLargeHeight), sourceRoIOf(node, source, corner).intersect(RectI(0, 0, kLargeWidth, kLargeHeight)));
}

TEST_F(NativeLensCorrectTest, MetadataFillsTheLookup)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_METADATAMODIFY));
    ASSERT_TRUE(bool(modify));
    connectNodes(source, modify, 0, true);
    ASSERT_NO_FATAL_FAILURE(setMetadata(modify, 1, kExifMetadataMake, std::string("Canon")));
    ASSERT_NO_FATAL_FAILURE(setMetadata(modify, 2, kExifMetadataModel, std::string(kCanon5D3)));
    ASSERT_NO_FATAL_FAILURE(setMetadata(modify, 3, kExifMetadataLensModel, std::string(kCanon70200) + " (stabilised)"));
    ASSERT_NO_FATAL_FAILURE(setMetadata(modify, 4, kExifMetadataFocalLength, 135.));
    ASSERT_NO_FATAL_FAILURE(setMetadata(modify, 5, kExifMetadataFNumber, 4.));
    NodePtr node = makeLensCorrect(modify);
    ASSERT_TRUE(bool(node));
    LensCorrect* effect = lensCorrectOf(node);
    ASSERT_TRUE(effect != nullptr);

    const LensCorrect::Lookup found = effect->lookup(kTime, ViewIdx(0));
    EXPECT_EQ(std::string(kCanon70200), found.lensQuery);
    EXPECT_NE(std::string::npos, found.lensFound.find("70-200")) << found.lensFound;
    EXPECT_NE(std::string::npos, found.cameraFound.find("5D Mark III")) << found.cameraFound;
    EXPECT_FLOAT_EQ(1.f, found.crop);
    EXPECT_EQ(std::string("camera"), found.cropFrom);
    EXPECT_FLOAT_EQ(135.f, found.focal);
    EXPECT_EQ(std::string("metadata"), found.focalFrom);
    EXPECT_FLOAT_EQ(4.f, found.aperture);
    EXPECT_EQ(std::string("metadata"), found.apertureFrom);
    EXPECT_FLOAT_EQ(1000.f, found.distance);
    EXPECT_EQ(std::string("default"), found.distanceFrom);
    EXPECT_TRUE(found.warning.empty()) << found.warning;

    // A knob overrides its key.
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamFocalLength, { 70. }));
    const LensCorrect::Lookup typed = effect->lookup(kTime, ViewIdx(0));
    EXPECT_FLOAT_EQ(70.f, typed.focal);
    EXPECT_EQ(std::string("knob"), typed.focalFrom);
}

TEST_F(NativeLensCorrectTest, FixedLensCameraNeedsNoLensName)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamCamera, std::string("Canon PowerShot G12")));
    LensCorrect* effect = lensCorrectOf(node);
    ASSERT_TRUE(effect != nullptr);
    const LensCorrect::Lookup found = effect->lookup(kTime, ViewIdx(0));
    // lensfun's English name for every compact's lens is "fixed lens"; its untranslated model names
    // the camera.
    ASSERT_TRUE(bool(found.lens)) << found.lensFound;
    EXPECT_NE(std::string::npos, std::string(found.lens->Model).find("G12")) << found.lensFound;
    EXPECT_EQ(std::string("lens"), found.focalFrom);
    EXPECT_TRUE(found.warning.empty()) << found.warning;
}

// Only a metadata lens name falls back to the fixed-lens camera's lens; a typed one is not replaced.
TEST_F(NativeLensCorrectTest, FixedLensFallbackIsForMetadataNames)
{
    const std::string unknown = "No Such Lens 13mm f/9";

    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_METADATAMODIFY));
    ASSERT_TRUE(bool(modify));
    connectNodes(source, modify, 0, true);
    ASSERT_NO_FATAL_FAILURE(setMetadata(modify, 1, kExifMetadataLensModel, unknown));
    NodePtr node = makeLensCorrect(modify);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamCamera, std::string("Canon PowerShot G12")));
    LensCorrect* effect = lensCorrectOf(node);
    ASSERT_TRUE(effect != nullptr);

    const LensCorrect::Lookup fromMetadata = effect->lookup(kTime, ViewIdx(0));
    ASSERT_TRUE(bool(fromMetadata.lens)) << fromMetadata.warning;
    EXPECT_NE(std::string::npos, std::string(fromMetadata.lens->Model).find("G12")) << fromMetadata.lensFound;
    EXPECT_TRUE(fromMetadata.warning.empty()) << fromMetadata.warning;

    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamLens, unknown));
    const LensCorrect::Lookup typed = effect->lookup(kTime, ViewIdx(0));
    EXPECT_FALSE(bool(typed.lens)) << typed.lensFound;
    EXPECT_NE(std::string::npos, typed.warning.find("not in the lensfun database")) << typed.warning;
}

// A flat field takes lensfun's vignetting gain at each pixel: brighter corners when correcting,
// darker when distorting.
TEST_F(NativeLensCorrectTest, FlatFieldTakesTheVignettingGain)
{
    resetProject();
    float crop = 0.f;
    std::shared_ptr<lfLens> lens = findLens("Canon", kCanon5D3, kCanon70200, &crop);
    ASSERT_TRUE(bool(lens));
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    ASSERT_TRUE(bool(constant));
    const float level = 0.5f;
    ASSERT_TRUE(setKnobValues(constant, kConstantParamColor, { level, level, level, 1. }));
    NodePtr node = makeCanon(constant);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamCorrectDistortion, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamCorrectTCA, { 0. }));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    for (bool reverse : { false, true }) {
        ASSERT_TRUE(setKnobValue(node, kLensCorrectParamMode, std::string(reverse ? kLensCorrectModeDistort : kLensCorrectModeCorrect)));
        lfModifier modifier(lens.get(), 100.f, crop, kParitySourceWidth, kParitySourceHeight, LF_PF_F32, reverse);
        modifier.EnableVignettingCorrection(2.8f, 1000.f);
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(node, window, &out));
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                // Lensfun's y runs down from the top row.
                float gain[3] = { 1.f, 1.f, 1.f };
                modifier.ApplyColorModification(gain, (float)x, (float)(kParitySourceHeight - 1 - y), 1, 1, LF_CR_3(RED, GREEN, BLUE), 3 * sizeof(float));
                const float* got = pixelAt(out, x, y);
                for (int c = 0; c < 3; ++c) {
                    ASSERT_NEAR(level * gain[c], got[c], 1e-5f * level * gain[c]) << (reverse ? "distort " : "correct ") << x << ", " << y;
                }
                ASSERT_FLOAT_EQ(1.f, got[3]);
            }
        }
        const float centre = pixelAt(out, kParitySourceWidth / 2, kParitySourceHeight / 2)[1];
        const float corner = pixelAt(out, 0, 0)[1];
        if (reverse) {
            EXPECT_LT(corner, centre);
        } else {
            EXPECT_GT(corner, centre);
        }
    }
}

// darktable's manual vignette, on the input before lensfun's corrections: nothing at the centre,
// 1 + 2 strength at the corners at full steepness.
TEST_F(NativeLensCorrectTest, ManualVignetteOnly)
{
    resetProject();
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(setKnobValues(constant, kConstantParamColor, { 0.5, 0.5, 0.5, 1. }));
    NodePtr node = makeLensCorrect(constant);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamMethod, std::string(kLensCorrectMethodVignette)));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteStrength, { 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteSteepness, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamShowVignetteMask, { 1. }));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane out;
    ASSERT_TRUE(renderRGBA(node, window, &out));
    const float w2 = 0.5f * kParitySourceWidth;
    const float h2 = 0.5f * kParitySourceHeight;
    for (int y = 0; y < kParitySourceHeight; ++y) {
        for (int x = 0; x < kParitySourceWidth; ++x) {
            const float lx = (float)x;
            const float ly = (float)(kParitySourceHeight - 1 - y);
            const float r = std::min(1.f, std::hypot(lx - w2, ly - h2) / std::hypot(w2, h2));
            const double b = 1.0 + 0.5 * 10.0;
            const double v = 1.0;
            const float val = std::max(0.f, (float)(2.0 * 0.5 * (v + (-v / std::tanh(b)) * std::tanh(b * (1.0 - r)))));
            const float* got = pixelAt(out, x, y);
            ASSERT_NEAR(0.5f * (1.f + val), got[0], 1e-5f) << x << ", " << y;
            ASSERT_NEAR(val, got[3], 1e-5f) << x << ", " << y;
        }
    }
}
