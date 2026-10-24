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
#include <optional>
#include <string>

#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawCACorrect.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawDenoise.h"
#include "Engine/Nodes/Raw/RawHotPixels.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
using RawReference::CropSite;
using RawReference::Tolerance;

// darktable's white balance coefficients for the camera files, as its log rounds them to 1e-3
// (Tests/fixtures/raw/README.md, Recorded values), which the temperature jobs packed.
const std::array<double, 3> kCr2D65 = { 2.251, 1.000, 1.418 };
const std::array<double, 3> kCr2AsShot = { 1.649, 1.000, 2.165 };
const std::array<double, 3> kRafD65 = { 2.293, 1.000, 1.612 };
const std::array<double, 3> kRafAsShot = { 1.791, 1.000, 2.202 };
// make-reference.py's USER_COEFFS.
const std::array<double, 3> kUserCoeffs = { 2.2, 1.0, 1.6 };
// Half the rounding step of darktable's log, and a little for the float multipliers.
const double kRoundedCoeffTolerance = 5e-4 + 1e-6;

// rawdenoise_jobs()'s shaped all-channels curve, at x = 0, 0.25, 0.5, 0.75 and 1.
const double kShapedCurve[RawKernels::kRawDenoiseBands] = { 0.9, 0.7, 0.5, 0.3, 0.1 };

// The class of a module job that runs on the file's default demosaic.
Tolerance
baseClass(const CropSite& site)
{
    return site.xtrans ? RawReference::kMarkesteijnClass : RawReference::kRCDClass;
}

std::string
referenceName(const CropSite& site,
              const char* suffix)
{
    return std::string(site.xtrans ? "fujifilm-x-pro2-" : "canon-eos-5d-mark-iii-") + suffix + ".tif";
}

std::string
caseName(const CropSite& site,
         const char* what)
{
    return std::string(site.xtrans ? "RAF " : "CR2 ") + what;
}
} // namespace

/**
 * The darktable jobs of make-reference.py for the modes RawReference_Test does not cover: each
 * one sets on the Develop RAW chain, or on a node put where darktable's pipe order (v3.0:
 * highlights, cacorrect, hotpixels, rawdenoise, demosaic) has it, the knobs the job's packed
 * params map to, and leaves the rest at the defaults darktable's history left alone.
 **/
class RawReferenceModulesTest
    : public RawReferenceFixture {
protected:
    // A node of pluginID between RawHighlights and RawDemosaic.
    NodePtr insertBeforeDemosaic(const RawDevelop::Chain& chain,
                                 const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node)) << pluginID;
        if (node) {
            insertAbove(chain.demosaic, node);
        }

        return node;
    }

    static ImageMetadata outputMetadata(const NodePtr& node)
    {
        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

        return effect ? effect->getOutputMetadata(1., ViewIdx(0)) : ImageMetadata();
    }

    static void expectRoundsTo(const char* what,
                               const std::optional<std::array<double, 3>>& natron,
                               const std::array<double, 3>& darktable)
    {
        ASSERT_TRUE(natron.has_value()) << what;
        std::printf("%s: Natron %.5f %.5f %.5f, darktable's log %.3f %.3f %.3f\n", what, (*natron)[0], (*natron)[1], (*natron)[2], darktable[0], darktable[1], darktable[2]);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(darktable[c], (*natron)[c], kRoundedCoeffTolerance) << what << " channel " << c;
        }
    }

    static void setCustomCoefficients(const NodePtr& temperature,
                                      const std::array<double, 3>& coeffs)
    {
        EffectInstancePtr effect = temperature->getEffectInstance();
        const char* const names[3] = { kRawTemperatureParamRed, kRawTemperatureParamGreen, kRawTemperatureParamBlue };

        for (int c = 0; c < 3; ++c) {
            effect->getKnobByNameAndType<KnobDouble>(names[c])->setValue(coeffs[c]);
        }
        effect->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->setValue((int)eRawTemperatureModeCustom);
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(coeffs[c], effect->getKnobByNameAndType<KnobDouble>(names[c])->getValue()) << names[c];
        }
    }

    // demosaic_jobs(): one Bayer method on the CR2 with the job's demosaic options.
    void compareBayerDemosaic(const char* what,
                              RawDemosaicBayerMethodEnum method,
                              int greenEq,
                              double edgeThreshold,
                              int colorSmoothing,
                              const char* suffix,
                              const Tolerance& tolerance)
    {
        const CropSite site = RawReference::cr2Site();
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << what;
        EffectInstancePtr demosaic = chain.demosaic->getEffectInstance();
        demosaic->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod)->setValue((int)method);
        demosaic->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamGreenEq)->setValue(greenEq);
        demosaic->getKnobByNameAndType<KnobDouble>(kRawDemosaicParamEdgeThreshold)->setValue(edgeThreshold);
        demosaic->getKnobByNameAndType<KnobInt>(kRawDemosaicParamColorSmoothing)->setValue(colorSmoothing);
        compareCrop(caseName(site, what).c_str(), site, chain.inputColor, referenceName(site, suffix).c_str(), tolerance);
    }

    // temperature_jobs(), as shot: no temperature history entry, so darktable's own as-shot
    // multipliers, unrounded.
    void compareTemperatureAsShot(const CropSite& site,
                                  const std::array<double, 3>& loggedAsShot)
    {
        const std::string name = caseName(site, "temperature as shot");
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->setValue((int)eRawTemperatureModeAsShot);
        expectRoundsTo(name.c_str(), RawMetadata::getWbApplied(outputMetadata(chain.temperature)), loggedAsShot);
        compareCrop(name.c_str(), site, chain.inputColor, referenceName(site, "temperature-as-shot").c_str(), baseClass(site));
    }

    // temperature_jobs(), camera reference: preset 3 with the logged D65 coefficients, which the
    // README has the test set as they were packed. Natron's Camera Reference multipliers must
    // round to them.
    void compareTemperatureCameraReference(const CropSite& site,
                                           const std::array<double, 3>& loggedD65)
    {
        const std::string name = caseName(site, "temperature camera reference");
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->setValue((int)eRawTemperatureModeCameraReference);
        expectRoundsTo(name.c_str(), RawMetadata::getWbApplied(outputMetadata(chain.temperature)), loggedD65);
        setCustomCoefficients(chain.temperature, loggedD65);
        compareCrop(name.c_str(), site, chain.inputColor, referenceName(site, "temperature-camera-reference").c_str(), baseClass(site));
    }

    // temperature_jobs(), as shot to reference: preset 4 with the logged as-shot coefficients.
    // The mode takes the file's as-shot multipliers, which must round to them; RawInputColor
    // then applies the late correction to the reference multipliers.
    void compareTemperatureAsShotToReference(const CropSite& site,
                                             const std::array<double, 3>& loggedAsShot)
    {
        const std::string name = caseName(site, "temperature as shot to reference");
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->setValue((int)eRawTemperatureModeAsShotToReference);
        const ImageMetadata metadata = outputMetadata(chain.temperature);
        expectRoundsTo(name.c_str(), RawMetadata::getWbApplied(metadata), loggedAsShot);
        EXPECT_TRUE(RawMetadata::getWbLateCorrection(metadata).has_value()) << name;
        compareCrop(name.c_str(), site, chain.inputColor, referenceName(site, "temperature-as-shot-to-reference").c_str(), baseClass(site));
    }

    // temperature_jobs(), user: preset 2 with make-reference.py's USER_COEFFS.
    void compareTemperatureUser(const CropSite& site)
    {
        const std::string name = caseName(site, "temperature user coefficients");
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        setCustomCoefficients(chain.temperature, kUserCoeffs);
        compareCrop(name.c_str(), site, chain.inputColor, referenceName(site, "temperature-user").c_str(), baseClass(site));
    }

    // hotpixels_jobs(): hotpixels_params(strength, threshold, permissive), the photosites not
    // marked.
    void compareHotPixels(const CropSite& site,
                          double strength,
                          double threshold,
                          bool permissive,
                          const char* variant)
    {
        const std::string name = caseName(site, (std::string("hot pixels ") + variant).c_str());
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        NodePtr hotPixels = insertBeforeDemosaic(chain, PLUGINID_NATRON_RAWHOTPIXELS);
        ASSERT_TRUE(bool(hotPixels));
        ASSERT_FALSE(HasFatalFailure());
        EffectInstancePtr effect = hotPixels->getEffectInstance();
        effect->getKnobByNameAndType<KnobDouble>(kRawHotPixelsParamStrength)->setValue(strength);
        effect->getKnobByNameAndType<KnobDouble>(kRawHotPixelsParamThreshold)->setValue(threshold);
        effect->getKnobByNameAndType<KnobBool>(kRawHotPixelsParamPermissive)->setValue(permissive);
        compareCrop(name.c_str(), site, chain.inputColor, referenceName(site, (std::string("hotpixels-") + variant).c_str()).c_str(), baseClass(site));
    }

    // cacorrect_jobs(): cacorrect_params(iterations, avoidshift) on the CR2 with RCD.
    void compareCACorrect(int iterations,
                          bool avoidShift,
                          const char* suffix)
    {
        const CropSite site = RawReference::cr2Site();
        const std::string name = caseName(site, (std::string("raw CA correction ") + suffix).c_str());
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        NodePtr caCorrect = insertBeforeDemosaic(chain, PLUGINID_NATRON_RAWCACORRECT);
        ASSERT_TRUE(bool(caCorrect));
        ASSERT_FALSE(HasFatalFailure());
        EffectInstancePtr effect = caCorrect->getEffectInstance();
        // The choice's entries are 1 to 5 iterations.
        effect->getKnobByNameAndType<KnobChoice>(kRawCACorrectParamIterations)->setValue(iterations - 1);
        effect->getKnobByNameAndType<KnobBool>(kRawCACorrectParamAvoidShift)->setValue(avoidShift);
        compareCrop(name.c_str(), site, chain.inputColor, referenceName(site, (std::string("cacorrect-") + suffix).c_str()).c_str(), RawReference::kAMaZEClass);
    }

    // rawdenoise_jobs(): threshold 0.05, the all-channels curve allCurve at darktable's evenly
    // spaced positions and the red, green and blue curves flat at 0.5.
    void compareRawDenoise(const CropSite& site,
                           const double allCurve[RawKernels::kRawDenoiseBands],
                           const char* variant)
    {
        const std::string name = caseName(site, (std::string("raw denoise 0.05 ") + variant).c_str());
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        NodePtr denoise = insertBeforeDemosaic(chain, PLUGINID_NATRON_RAWDENOISE);
        ASSERT_TRUE(bool(denoise));
        ASSERT_FALSE(HasFatalFailure());
        EffectInstancePtr effect = denoise->getEffectInstance();
        effect->getKnobByNameAndType<KnobDouble>(kRawDenoiseParamThreshold)->setValue(0.05);
        for (int channel = 0; channel < 4; ++channel) {
            for (int band = 0; band < RawKernels::kRawDenoiseBands; ++band) {
                effect->getKnobByNameAndType<KnobDouble>(RawDenoise::strengthKnobName(channel, band))->setValue((channel == 0) ? allCurve[band] : 0.5);
            }
        }
        compareCrop(name.c_str(), site, chain.inputColor, referenceName(site, (std::string("rawdenoise-0.05-") + variant).c_str()).c_str(), baseClass(site));
    }
};

TEST_F(RawReferenceModulesTest, BayerVNG4MatchesDarktable)
{
    compareBayerDemosaic("demosaic VNG4", eRawDemosaicBayerVNG4, (int)eRawDemosaicGreenEqNone, 0., 0, "vng4", RawReference::kAMaZEClass);
}

TEST_F(RawReferenceModulesTest, XTransVNGMatchesDarktable)
{
    const CropSite site = RawReference::rafSite();
    const RawDevelop::Chain chain = createChain(site);
    ASSERT_TRUE(chain.complete());
    chain.demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamXTransMethod)->setValue((int)eRawDemosaicXTransVNG);
    compareCrop("RAF demosaic X-Trans VNG", site, chain.inputColor, referenceName(site, "vng").c_str(), RawReference::kMarkesteijnClass);
}

TEST_F(RawReferenceModulesTest, BayerPPGMedianThresholdMatchesDarktable)
{
    compareBayerDemosaic("demosaic PPG, edge threshold 0.3", eRawDemosaicBayerPPG, (int)eRawDemosaicGreenEqNone, 0.3, 0, "ppg-median-0.3", RawReference::kRCDClass);
}

TEST_F(RawReferenceModulesTest, BayerGreenEquilibrationLocalMatchesDarktable)
{
    compareBayerDemosaic("demosaic RCD, match greens local", eRawDemosaicBayerRCD, (int)eRawDemosaicGreenEqLocal, 0., 0, "rcd-greeneq-local", RawReference::kRCDClass);
}

TEST_F(RawReferenceModulesTest, BayerGreenEquilibrationFullMatchesDarktable)
{
    compareBayerDemosaic("demosaic RCD, match greens full", eRawDemosaicBayerRCD, (int)eRawDemosaicGreenEqFull, 0., 0, "rcd-greeneq-full", RawReference::kRCDClass);
}

TEST_F(RawReferenceModulesTest, BayerGreenEquilibrationBothMatchesDarktable)
{
    compareBayerDemosaic("demosaic RCD, match greens full and local", eRawDemosaicBayerRCD, (int)eRawDemosaicGreenEqBoth, 0., 0, "rcd-greeneq-both", RawReference::kRCDClass);
}

TEST_F(RawReferenceModulesTest, BayerColorSmoothingOnceMatchesDarktable)
{
    compareBayerDemosaic("demosaic RCD, colour smoothing 1", eRawDemosaicBayerRCD, (int)eRawDemosaicGreenEqNone, 0., 1, "rcd-smoothing-1", RawReference::kRCDClass);
}

TEST_F(RawReferenceModulesTest, BayerColorSmoothingFiveTimesMatchesDarktable)
{
    compareBayerDemosaic("demosaic RCD, colour smoothing 5", eRawDemosaicBayerRCD, (int)eRawDemosaicGreenEqNone, 0., 5, "rcd-smoothing-5", RawReference::kRCDClass);
}

TEST_F(RawReferenceModulesTest, BayerTemperatureAsShotMatchesDarktable)
{
    compareTemperatureAsShot(RawReference::cr2Site(), kCr2AsShot);
}

TEST_F(RawReferenceModulesTest, XTransTemperatureAsShotMatchesDarktable)
{
    compareTemperatureAsShot(RawReference::rafSite(), kRafAsShot);
}

TEST_F(RawReferenceModulesTest, BayerTemperatureCameraReferenceMatchesDarktable)
{
    compareTemperatureCameraReference(RawReference::cr2Site(), kCr2D65);
}

TEST_F(RawReferenceModulesTest, XTransTemperatureCameraReferenceMatchesDarktable)
{
    compareTemperatureCameraReference(RawReference::rafSite(), kRafD65);
}

TEST_F(RawReferenceModulesTest, BayerTemperatureAsShotToReferenceMatchesDarktable)
{
    compareTemperatureAsShotToReference(RawReference::cr2Site(), kCr2AsShot);
}

TEST_F(RawReferenceModulesTest, XTransTemperatureAsShotToReferenceMatchesDarktable)
{
    compareTemperatureAsShotToReference(RawReference::rafSite(), kRafAsShot);
}

TEST_F(RawReferenceModulesTest, BayerTemperatureUserMatchesDarktable)
{
    compareTemperatureUser(RawReference::cr2Site());
}

TEST_F(RawReferenceModulesTest, XTransTemperatureUserMatchesDarktable)
{
    compareTemperatureUser(RawReference::rafSite());
}

TEST_F(RawReferenceModulesTest, BayerHotPixelsMatchDarktable)
{
    compareHotPixels(RawReference::cr2Site(), 0.25, 0.05, false, "default");
}

TEST_F(RawReferenceModulesTest, BayerHotPixelsPermissiveMatchDarktable)
{
    compareHotPixels(RawReference::cr2Site(), 0.25, 0.05, true, "permissive");
}

TEST_F(RawReferenceModulesTest, BayerHotPixelsStrength1MatchDarktable)
{
    compareHotPixels(RawReference::cr2Site(), 1., 0.05, false, "strength-1");
}

TEST_F(RawReferenceModulesTest, XTransHotPixelsMatchDarktable)
{
    compareHotPixels(RawReference::rafSite(), 0.25, 0.05, false, "default");
}

TEST_F(RawReferenceModulesTest, XTransHotPixelsPermissiveMatchDarktable)
{
    compareHotPixels(RawReference::rafSite(), 0.25, 0.05, true, "permissive");
}

TEST_F(RawReferenceModulesTest, XTransHotPixelsStrength1MatchDarktable)
{
    compareHotPixels(RawReference::rafSite(), 1., 0.05, false, "strength-1");
}

TEST_F(RawReferenceModulesTest, CACorrectOneIterationMatchesDarktable)
{
    compareCACorrect(1, false, "iterations-1");
}

TEST_F(RawReferenceModulesTest, CACorrectTwoIterationsMatchesDarktable)
{
    compareCACorrect(2, false, "iterations-2");
}

TEST_F(RawReferenceModulesTest, CACorrectFiveIterationsMatchesDarktable)
{
    compareCACorrect(5, false, "iterations-5");
}

TEST_F(RawReferenceModulesTest, CACorrectAvoidShiftMatchesDarktable)
{
    compareCACorrect(2, true, "avoidshift");
}

TEST_F(RawReferenceModulesTest, BayerRawDenoiseFlatMatchesDarktable)
{
    const double flat[RawKernels::kRawDenoiseBands] = { 0.5, 0.5, 0.5, 0.5, 0.5 };
    compareRawDenoise(RawReference::cr2Site(), flat, "flat");
}

TEST_F(RawReferenceModulesTest, BayerRawDenoiseShapedMatchesDarktable)
{
    compareRawDenoise(RawReference::cr2Site(), kShapedCurve, "shaped");
}

TEST_F(RawReferenceModulesTest, XTransRawDenoiseFlatMatchesDarktable)
{
    const double flat[RawKernels::kRawDenoiseBands] = { 0.5, 0.5, 0.5, 0.5, 0.5 };
    compareRawDenoise(RawReference::rafSite(), flat, "flat");
}

TEST_F(RawReferenceModulesTest, XTransRawDenoiseShapedMatchesDarktable)
{
    compareRawDenoise(RawReference::rafSite(), kShapedCurve, "shaped");
}
