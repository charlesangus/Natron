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
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <lensfun.h>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
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
#include "Engine/Nodes/Metadata/ModifyMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/LensCorrect.h"
#include "Engine/Nodes/Transform/LensCorrectProfile.h"
#include "Engine/Nodes/Transform/LensfunRuntime.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const char* const kCanon5D3 = "Canon EOS 5D Mark III";
const char* const kCanon70200 = "Canon EF 70-200mm f/2.8L IS II USM";

// A copy of the database's lens, and the camera's crop factor.
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
        lens = std::make_shared<lfLens>(*lenses[0]);
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

// The lfModifier darktable's enables give, scaled by the profile's own scale (darktable's autoscale
// runs with every requested correction, where EnableScaling(0) would see only those before it).
std::unique_ptr<lfModifier>
oracle(const lfLens& lens,
       const LensCorrectProfile::Request& r)
{
    std::unique_ptr<lfModifier> modifier(new lfModifier(&lens, r.focal, r.crop, r.width, r.height, LF_PF_F32, r.reverse));

    if (r.distortion) {
        modifier->EnableDistortionCorrection();
    }
    modifier->EnableProjectionTransform((lfLensType)r.targetGeometry);
    modifier->EnableScaling((r.scale == 0.f) ? LensCorrectProfile::autoScale(lens, r) : r.scale);
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

// The profile maps a 13 x 9 grid over the image, edges included, as lfModifier does, and gives
// the same vignetting gain.
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
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
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
    op->setValue((int)ModifyMetadata::eOperationSet, ViewSpec::all(), 0);
    keyKnob->setValue(key, ViewSpec::all(), 0);
    type->setValue((int)ModifyMetadata::eValueTypeString, ViewSpec::all(), 0);
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
    op->setValue((int)ModifyMetadata::eOperationSet, ViewSpec::all(), 0);
    keyKnob->setValue(key, ViewSpec::all(), 0);
    type->setValue((int)ModifyMetadata::eValueTypeDouble, ViewSpec::all(), 0);
    valueKnob->setValue(value, ViewSpec::all(), 0);
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

    // A LensCorrect with the 70-200 at 100 mm, f/2.8, typed in.
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
};

TEST_F(NativeLensCorrectTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_LENSCORRECT, PLUGIN_MAJOR_NATRON_LENSCORRECT, "LensCorrect");
}

// darktable's lens correction controls, each with a knob.
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
    expectProfileMatches(*lens, r, "70-200 auto scale");
    const LensCorrectProfile::Profile profile = LensCorrectProfile::build(*lens, r);
    const float slack = 1.f;
    for (int i = 0; i < 2 * (r.width + r.height); i += 7) {
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
    }
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

    const RectI window = paritySourceWindow(source, kTime, 0);
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

TEST_F(NativeLensCorrectTest, TilesMatchTheWholeFrame)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeCanon(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteStrength, { 0.5 }));

    const RectI window = paritySourceWindow(source, kTime, 0);
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

// The camera, lens and shooting settings come from the Source's metadata when the knobs are empty.
TEST_F(NativeLensCorrectTest, MetadataFillsTheLookup)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
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

// A fixed-lens camera finds its lens without a lens name.
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
    EXPECT_NE(std::string::npos, found.lensFound.find("G12")) << found.lensFound;
    EXPECT_EQ(std::string("lens"), found.focalFrom);
    EXPECT_TRUE(found.warning.empty()) << found.warning;
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
