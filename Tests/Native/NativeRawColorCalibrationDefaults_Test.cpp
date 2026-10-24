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

#include "RawReferenceSupport.h"

#include <array>
#include <cmath>
#include <string>

#include <QString>

#include "NativeMetadataTestEffect.h"

#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Metadata/CopyMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawColorCalibrationMath.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
namespace Cal = RawColorCalibrationMath;

// The linear DNG's crop (see the fixtures README).
RawReference::CropSite
dngSite()
{
    return { &linearFixture(), 3960, 2640, 2100, 1180, false };
}

// The README's "[commit color calibration]" lines of the scene-referred default jobs.
struct LoggedDefaults {
    double temperature;
    double x;
    double y;
};

ImageMetadata
outputMetadata(const NodePtr& node)
{
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

    return effect ? effect->getOutputMetadata(1., ViewIdx(0)) : ImageMetadata();
}

template <typename T>
std::shared_ptr<T>
knob(const NodePtr& node,
     const char* name)
{
    std::shared_ptr<T> k = node->getEffectInstance()->getKnobByNameAndType<T>(name);
    EXPECT_TRUE(bool(k)) << name;

    return k;
}

std::string
illuminantId(const NodePtr& node)
{
    return knob<KnobChoice>(node, kRawColorCalibrationParamIlluminant)->getActiveEntry().id;
}

std::string
adaptationId(const NodePtr& node)
{
    return knob<KnobChoice>(node, kRawColorCalibrationParamAdaptation)->getActiveEntry().id;
}

QString
warningOf(const NodePtr& node)
{
    QString message;
    int type = 0;

    if (node->hasPersistentMessage()) {
        node->getPersistentMessage(&message, &type, false);
    }

    return message;
}
} // namespace

/**
 * reload_defaults() on the camera files, and the as shot in camera illuminant against darktable's
 * colour calibration jobs (make-reference.py colour_calibration_jobs()).
 **/
class NativeRawColorCalibrationDefaultsTest
    : public RawReferenceFixture {
protected:
    void SetUp() OVERRIDE
    {
        RawReferenceFixture::SetUp();
        setLinearRec709WorkingSpace();
    }

    static const char* fileName(const RawReference::CropSite& site)
    {
        return site.fixture == &linearFixture() ? "DNG" : (site.xtrans ? "RAF" : "CR2");
    }

    // The develop chain with RawTemperature in mode and a RawColorCalibration after RawInputColor.
    NodePtr createCalibrated(const RawReference::CropSite& site,
                             RawTemperatureModeEnum mode,
                             RawDevelop::Chain* chain)
    {
        *chain = createChain(site);
        EXPECT_TRUE(chain->complete()) << fileName(site);
        if (!chain->complete()) {
            return NodePtr();
        }
        knob<KnobChoice>(chain->temperature, kRawTemperatureParamMode)->setValue((int)mode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
        EXPECT_TRUE(bool(node));
        if (node) {
            connectNodes(chain->inputColor, node, 0, true);
        }

        return node;
    }

    // The scene-referred default jobs: darktable's default white balance, as shot to reference,
    // and color calibration at its defaults.
    void expectDefaults(const RawReference::CropSite& site,
                        const LoggedDefaults& logged)
    {
        const std::string name = std::string(fileName(site)) + " defaults";
        RawDevelop::Chain chain;
        NodePtr node = createCalibrated(site, eRawTemperatureModeAsShotToReference, &chain);
        ASSERT_TRUE(bool(node)) << name;

        const RawColorCalibrationSourceDefaults d = rawColorCalibrationSourceDefaults(outputMetadata(chain.inputColor));
        EXPECT_EQ(Cal::Illuminant::Custom, d.illuminant) << name;
        EXPECT_EQ(Cal::Adaptation::Cat16, d.adaptation) << name;
        EXPECT_NEAR(logged.temperature, d.temperature, 1.) << name;
        EXPECT_NEAR(logged.x, d.x, 1e-4) << name;
        EXPECT_NEAR(logged.y, d.y, 1e-4) << name;

        // Shown on the knobs until overridden.
        EXPECT_FALSE(knob<KnobBool>(node, kRawColorCalibrationParamCatOverride)->getValue()) << name;
        EXPECT_EQ("custom", illuminantId(node)) << name;
        EXPECT_EQ("cat16", adaptationId(node)) << name;
        EXPECT_NEAR(logged.temperature, knob<KnobDouble>(node, kRawColorCalibrationParamTemperature)->getValue(), 1.) << name;
        EXPECT_NEAR(logged.x, knob<KnobDouble>(node, kRawColorCalibrationParamX)->getValue(), 1e-4) << name;
        EXPECT_NEAR(logged.y, knob<KnobDouble>(node, kRawColorCalibrationParamY)->getValue(), 1e-4) << name;
        EXPECT_FALSE(knob<KnobChoice>(node, kRawColorCalibrationParamIlluminant)->isEnabled(0)) << name;

        // White balance to the reference: nothing to warn about, and the adaptation is published.
        EXPECT_TRUE(warningOf(node).isEmpty()) << name << ": " << warningOf(node).toStdString();
        EXPECT_TRUE(RawMetadata::getCatApplied(outputMetadata(node))) << name;
    }

    // colour_calibration_jobs() "camera": CAT16 with the illuminant as shot in camera. With
    // rounded, the white balance darktable's job packed: preset 4 with its logged as-shot
    // coefficients, which no longer equal the file's, so the CAT keeps their ratio to D65
    // (_get_white_balance_coeff()). Natron's As Shot to Reference takes the file's own, so a
    // CopyMetadata after RawInputColor puts the rounded ones in raw/wb_applied.
    void compareCamera(const RawReference::CropSite& site,
                       const char* reference,
                       RawTemperatureModeEnum mode,
                       const std::array<double, 3>* rounded)
    {
        const std::string name = std::string(fileName(site)) + " colorcal " + reference;
        RawDevelop::Chain chain;
        NodePtr node = createCalibrated(site, mode, &chain);
        ASSERT_TRUE(bool(node)) << name;
        if (rounded) {
            NodePtr copy = createNode(QString::fromUtf8(PLUGINID_NATRON_COPYMETADATA));
            NodePtr wb = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
            ASSERT_TRUE(copy && wb) << name;
            MetadataSourceTestEffect* wbEffect = dynamic_cast<MetadataSourceTestEffect*>(wb->getEffectInstance().get());
            ASSERT_TRUE(wbEffect != nullptr) << name;
            ImageMetadata extra;
            RawMetadata::setWbApplied(&extra, *rounded);
            wbEffect->setExtraMetadata(extra);
            insertAbove(node, copy);
            connectNodes(wb, copy, 1, true);
            knob<KnobChoice>(copy, "mode")->setValue((int)CopyMetadata::eModeMergeBWins, ViewSpec::all(), 0);
            knob<KnobString>(copy, "keys")->setValue(std::string(kRawMetadataWbApplied), ViewSpec::all(), 0);
        }

        knob<KnobBool>(node, kRawColorCalibrationParamCatOverride)->setValue(true);
        knob<KnobDouble>(node, kRawColorCalibrationParamTemperature)->setValue(4500.);
        knob<KnobChoice>(node, kRawColorCalibrationParamIlluminant)->setValueFromID("camera", 0);
        ASSERT_EQ("camera", illuminantId(node)) << name;
        // gui_update(): the camera illuminant adapts by CAT16 and greys out the adaptation.
        EXPECT_EQ("cat16", adaptationId(node)) << name;
        EXPECT_FALSE(knob<KnobChoice>(node, kRawColorCalibrationParamAdaptation)->isEnabled(0)) << name;

        const std::string file = site.fixture == &linearFixture() ? "canon-eos-5d-mark-iii-linear-rgb-" : (site.xtrans ? "fujifilm-x-pro2-" : "canon-eos-5d-mark-iii-");
        compareCrop(name.c_str(), site, node, (file + reference + ".tif").c_str(), site.xtrans ? RawReference::kMarkesteijnClass : RawReference::kRCDClass);
    }

    // make-reference.py sets colorin's working profile to linear Rec709, and the clips act on
    // working RGB.
    void setLinearRec709WorkingSpace()
    {
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID("Linear Rec.709 (sRGB)", 0);
        ASSERT_EQ(std::string("Linear Rec.709 (sRGB)"), getApp()->getProject()->getWorkingColorSpace());
    }
};

TEST_F(NativeRawColorCalibrationDefaultsTest, Cr2DefaultsMatchDarktable)
{
    expectDefaults(RawReference::cr2Site(), { 3222., 0.4251, 0.4102 });
}

TEST_F(NativeRawColorCalibrationDefaultsTest, RafDefaultsMatchDarktable)
{
    expectDefaults(RawReference::rafSite(), { 3678., 0.3989, 0.3969 });
}

TEST_F(NativeRawColorCalibrationDefaultsTest, DngDefaultsMatchDarktable)
{
    expectDefaults(dngSite(), { 3182., 0.4280, 0.4123 });
}

TEST_F(NativeRawColorCalibrationDefaultsTest, Cr2CameraMatchesDarktable)
{
    compareCamera(RawReference::cr2Site(), "colorcal-camera", eRawTemperatureModeAsShot, nullptr);
    const std::array<double, 3> rounded = { 1.649, 1., 2.165 };
    compareCamera(RawReference::cr2Site(), "colorcal-camera-preset-4", eRawTemperatureModeAsShotToReference, &rounded);
}

TEST_F(NativeRawColorCalibrationDefaultsTest, RafCameraMatchesDarktable)
{
    compareCamera(RawReference::rafSite(), "colorcal-camera", eRawTemperatureModeAsShot, nullptr);
    const std::array<double, 3> rounded = { 1.791, 1., 2.202 };
    compareCamera(RawReference::rafSite(), "colorcal-camera-preset-4", eRawTemperatureModeAsShotToReference, &rounded);
}

TEST_F(NativeRawColorCalibrationDefaultsTest, DngCameraMatchesDarktable)
{
    compareCamera(dngSite(), "colorcal-camera", eRawTemperatureModeAsShot, nullptr);
}

// The camera illuminant is offered only for raw input.
TEST_F(NativeRawColorCalibrationDefaultsTest, CameraIlluminantOnlyForRaw)
{
    RawDevelop::Chain chain;
    NodePtr node = createCalibrated(RawReference::cr2Site(), eRawTemperatureModeAsShotToReference, &chain);
    ASSERT_TRUE(bool(node));
    KnobChoicePtr illuminant = knob<KnobChoice>(node, kRawColorCalibrationParamIlluminant);
    EXPECT_EQ(rawColorCalibrationIlluminantIndex(Cal::Illuminant::Camera) + 1, illuminant->getNumEntries());

    NodePtr plain = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
    ASSERT_TRUE(plain && other);
    connectNodes(plain, other, 0, true);
    EXPECT_EQ(rawColorCalibrationIlluminantIndex(Cal::Illuminant::Camera), knob<KnobChoice>(other, kRawColorCalibrationParamIlluminant)->getNumEntries());
    // Not raw: darktable's defaults for a non-raw image, D 5003 K CAT16.
    EXPECT_EQ("d", illuminantId(other));
    EXPECT_EQ("cat16", adaptationId(other));
}

// _set_trouble_messages(): a white balance other than the reference under a CAT warns.
TEST_F(NativeRawColorCalibrationDefaultsTest, WhiteBalanceAppliedTwiceWarns)
{
    RawDevelop::Chain chain;
    NodePtr node = createCalibrated(RawReference::cr2Site(), eRawTemperatureModeAsShot, &chain);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ("cat16", adaptationId(node));
    EXPECT_TRUE(warningOf(node).contains(QString::fromUtf8("white balance applied twice"))) << warningOf(node).toStdString();

    knob<KnobBool>(node, kRawColorCalibrationParamCatOverride)->setValue(true);
    knob<KnobChoice>(node, kRawColorCalibrationParamAdaptation)->setValueFromID("none", 0);
    EXPECT_TRUE(warningOf(node).isEmpty()) << warningOf(node).toStdString();
}

// reload_defaults(): a second instance after one that adapts defaults to the plain channel mixer
// (illuminant pipe, adaptation none), and _set_trouble_messages() warns once it adapts as well.
TEST_F(NativeRawColorCalibrationDefaultsTest, SecondInstanceDefaultsToPipe)
{
    RawDevelop::Chain chain;
    NodePtr first = createCalibrated(RawReference::cr2Site(), eRawTemperatureModeAsShotToReference, &chain);
    ASSERT_TRUE(bool(first));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
    ASSERT_TRUE(bool(second));
    connectNodes(first, second, 0, true);

    EXPECT_TRUE(RawMetadata::getCatApplied(outputMetadata(first)));
    EXPECT_EQ("pipe", illuminantId(second));
    EXPECT_EQ("none", adaptationId(second));
    // darktable warns only when the second instance adapts too.
    EXPECT_TRUE(warningOf(second).isEmpty()) << warningOf(second).toStdString();

    knob<KnobBool>(second, kRawColorCalibrationParamCatOverride)->setValue(true);
    knob<KnobChoice>(second, kRawColorCalibrationParamAdaptation)->setValueFromID("cat16", 0);
    knob<KnobChoice>(second, kRawColorCalibrationParamIlluminant)->setValueFromID("d", 0);
    EXPECT_TRUE(warningOf(second).contains(QString::fromUtf8("double CAT applied"))) << warningOf(second).toStdString();
    EXPECT_TRUE(warningOf(first).isEmpty()) << warningOf(first).toStdString();
}
