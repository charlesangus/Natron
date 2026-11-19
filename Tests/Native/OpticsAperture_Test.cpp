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

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include "Tests/BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Optics/Aperture.h"
#include "Engine/Optics/ApertureKnobs.h"

NATRON_NAMESPACE_USING
using namespace Optics;

namespace {

const double kPi = 3.14159265358979323846;
const int kThreads = 8;

std::vector<float>
rasterise(const ApertureParams& params,
          int width,
          int height,
          const ApertureImage& image = ApertureImage(),
          int threads = kThreads,
          double lightX = 0.,
          double lightY = 0.)
{
    ApertureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.lightX = lightX;
    frame.lightY = lightY;
    std::vector<float> out((std::size_t)width * height, -1.f);
    EXPECT_TRUE(rasteriseAperture(params, frame, image, out.data(), threads));

    return out;
}

double
mean(const std::vector<float>& mask)
{
    double sum = 0.;
    for (float v : mask) {
        sum += v;
    }

    return sum / mask.size();
}

// The mask spans [-1, 1] on both axes, an area of 4.
double
area(const std::vector<float>& mask)
{
    return mean(mask) * 4.;
}

// A regular polygon of the given inradius.
double
polygonArea(int sides,
            double inradius)
{
    return sides * std::tan(kPi / sides) * inradius * inradius;
}

// Contact sheets for review, written when NATRON_TEST_APERTURE_PNG_DIR names a directory.
void
writeContact(const std::string& name,
             const std::vector<float>& mask,
             int width,
             int height)
{
    const char* dir = std::getenv("NATRON_TEST_APERTURE_PNG_DIR");
    if (!dir || !*dir) {
        return;
    }
    std::vector<float> topDown(mask.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            topDown[(std::size_t)y * width + x] = mask[(std::size_t)(height - 1 - y) * width + x];
        }
    }
    const std::string path = std::string(dir) + "/aperture_" + name + ".png";
    OIIO::ImageSpec spec(width, height, 1, OIIO::TypeDesc::UINT8);
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    const bool written = out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, topDown.data()) && out->close();
    EXPECT_TRUE(written) << path;
}

} // namespace

TEST(OpticsAperture, HexagonHasTheAnalyticArea)
{
    const ApertureParams params;
    const std::vector<float> mask = rasterise(params, 512, 512);
    const double expected = polygonArea(6, 0.75);
    EXPECT_NEAR(area(mask), expected, expected * 0.005);
    writeContact("default", mask, 512, 512);
}

TEST(OpticsAperture, RotationKeepsTheArea)
{
    ApertureParams params;
    params.rotation = 17.;
    const double expected = polygonArea(6, 0.75);
    EXPECT_NEAR(area(rasterise(params, 512, 512)), expected, expected * 0.005);
}

TEST(OpticsAperture, ManyBladesApproachADisc)
{
    ApertureParams params;
    params.blades = 128;
    const double disc = kPi * 0.75 * 0.75;
    EXPECT_NEAR(area(rasterise(params, 512, 512)), disc, disc * 0.005);

    params.blades = 5;
    const double pentagon = area(rasterise(params, 512, 512));
    EXPECT_GT(std::fabs(pentagon - disc), disc * 0.05);
}

TEST(OpticsAperture, SizeScalesPerAxis)
{
    ApertureParams params;
    params.blades = 128;
    params.sizeX = 0.5;
    params.sizeY = 0.25;
    const double ellipse = kPi * 0.5 * 0.25;
    EXPECT_NEAR(area(rasterise(params, 512, 512)), ellipse, ellipse * 0.005);
}

TEST(OpticsAperture, SoftnessKeepsTheMaskInRange)
{
    ApertureParams params;
    params.softness = 1.;
    const std::vector<float> mask = rasterise(params, 128, 128);
    bool partial = false;
    for (float v : mask) {
        EXPECT_GE(v, 0.f);
        EXPECT_LE(v, 1.f);
        partial = partial || (v > 0.05f && v < 0.95f);
    }
    EXPECT_TRUE(partial);
}

TEST(OpticsAperture, RoundnessChangesTheShape)
{
    ApertureParams params;
    const double sharp = area(rasterise(params, 256, 256));
    params.roundness = 0.5;
    const double round = area(rasterise(params, 256, 256));
    EXPECT_GT(std::fabs(round - sharp), sharp * 0.01);
}

TEST(OpticsAperture, EachLayerLowersTheMean)
{
    const ApertureParams plain;
    const double base = mean(rasterise(plain, 256, 256));

    ApertureParams dust;
    dust.dustStrength = 1.;
    const std::vector<float> dustMask = rasterise(dust, 512, 512);
    EXPECT_LT(mean(dustMask), base - 0.005);
    writeContact("dust", dustMask, 512, 512);

    ApertureParams scratches;
    scratches.scratchesStrength = 1.;
    scratches.scratchesRotationVariation = 0.3;
    const std::vector<float> scratchesMask = rasterise(scratches, 512, 512);
    EXPECT_LT(mean(scratchesMask), base - 0.005);
    writeContact("scratches", scratchesMask, 512, 512);

    ApertureParams grating;
    grating.gratingStrength = 1.;
    grating.gratingLength = 1.;
    const std::vector<float> gratingMask = rasterise(grating, 512, 512);
    EXPECT_LT(mean(gratingMask), base - 0.005);
    writeContact("grating", gratingMask, 512, 512);
}

TEST(OpticsAperture, ZeroStrengthsLeaveThePolygon)
{
    ApertureParams params;
    params.dustDensity = 1.;
    params.scratchesDensity = 1.;
    params.gratingDensity = 1.;
    EXPECT_EQ(rasterise(ApertureParams(), 128, 128), rasterise(params, 128, 128));
}

TEST(OpticsAperture, ParallaxMovesTheDust)
{
    ApertureParams params;
    params.dustStrength = 1.;
    const std::vector<float> still = rasterise(params, 128, 128);
    EXPECT_NE(still, rasterise(params, 128, 128, ApertureImage(), kThreads, 0.3, -0.2));

    params.dustParallaxX = 0.;
    params.dustParallaxY = 0.;
    EXPECT_EQ(still, rasterise(params, 128, 128, ApertureImage(), kThreads, 0.3, -0.2));
}

TEST(OpticsAperture, ImageLayerMultipliesTheMask)
{
    const int size = 16;
    std::vector<float> pixels((std::size_t)size * size, 1.f);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size / 2; ++x) {
            pixels[(std::size_t)y * size + x] = 0.f;
        }
    }
    ApertureImage image;
    image.pixels = pixels.data();
    image.width = size;
    image.height = size;

    ApertureParams params;
    params.blades = 128;
    params.imageStrength = 1.;
    params.imageSizeX = 1.;
    params.imageSizeY = 1.;
    const std::vector<float> mask = rasterise(params, 128, 128, image);
    const std::vector<float> plain = rasterise(ApertureParams(), 128, 128);
    const double half = mean(mask);
    EXPECT_GT(half, 0.);
    EXPECT_LT(half, mean(rasterise([] {
                        ApertureParams disc;
                        disc.blades = 128;
                        return disc;
                    }(),
                                   128, 128))
                  * 0.6);
    // Left of centre the image is black, so the mask is.
    EXPECT_EQ(0.f, mask[(std::size_t)64 * 128 + 40]);
    EXPECT_GT(mask[(std::size_t)64 * 128 + 90], 0.99f);
    EXPECT_GT(plain[(std::size_t)64 * 128 + 40], 0.99f);
}

TEST(OpticsAperture, SameParametersGiveTheSameMask)
{
    ApertureParams params;
    params.softness = 0.3;
    params.dustStrength = 0.7;
    params.scratchesStrength = 0.5;
    params.scratchesRotationVariation = 1.;
    params.gratingStrength = 0.4;
    const std::vector<float> first = rasterise(params, 256, 192, ApertureImage(), kThreads, 0.1, 0.2);
    EXPECT_EQ(first, rasterise(params, 256, 192, ApertureImage(), kThreads, 0.1, 0.2));
    EXPECT_EQ(first, rasterise(params, 256, 192, ApertureImage(), 1, 0.1, 0.2));
}

TEST(OpticsAperture, AbortStopsTheMask)
{
    ApertureFrame frame;
    frame.width = 64;
    frame.height = 64;
    std::vector<float> out((std::size_t)64 * 64, 0.f);
    EXPECT_FALSE(rasteriseAperture(ApertureParams(), frame, ApertureImage(), out.data(), kThreads, [] { return true; }));
}

namespace {

struct DoubleDefault {
    const char* name;
    const char* label;
    double value;
    int dimensions;
};

const DoubleDefault kDoubleDefaults[] = {
    { "Size", "Size", 0.75, 2 },
    { "Roundness", "Roundness", 0., 1 },
    { "Rotation", "Rotation", 0., 1 },
    { "Softness", "Softness", 0., 1 },
    { "GratingStrength", "Grating Strength", 0., 1 },
    { "GratingDensity", "Grating Density", 0.5, 1 },
    { "GratingLength", "Grating Length", 0.5, 1 },
    { "GratingWidth", "Grating Width", 0.25, 1 },
    { "GratingSoftness", "Grating Softness", 0., 1 },
    { "ScratchesStrength", "Scratches Strength", 0., 1 },
    { "ScratchesDensity", "Scratches Density", 0.5, 1 },
    { "ScratchesLength", "Scratches Length", 0.5, 1 },
    { "ScratchesWidth", "Scratches Width", 0.25, 1 },
    { "ScratchesRotation", "Scratches Rotation", 0., 1 },
    { "ScratchesRotationVariation", "Scratches Rotation Variation", 0., 1 },
    { "ScratchesSoftness", "Scratches Softness", 0., 1 },
    { "ScratchesParallax", "Scratches Parallax", 1., 2 },
    { "DustStrength", "Dust Strength", 0., 1 },
    { "DustDensity", "Dust Density", 0.5, 1 },
    { "DustRadius", "Dust Radius", 0.5, 1 },
    { "DustSoftness", "Dust Softness", 0., 1 },
    { "DustParallax", "Dust Parallax", 1., 2 },
    { "ImageStrength", "Image Strength", 0., 1 },
    { "ImageSize", "Image Size", 0.75, 2 },
    { "ImageBlack", "Image Black", 0., 1 },
    { "ImageWhite", "Image White", 1., 1 },
};

const char* const kGroups[] = { "Shape", "Grating", "Scratches", "Dust", "Image" };

KnobPagePtr
addPage(EffectInstance* effect)
{
    return AppManager::createKnob<KnobPage>(effect, std::string("Aperture"));
}

} // namespace

class OpticsApertureKnobsTest
    : public BaseTest {
};

TEST_F(OpticsApertureKnobsTest, EveryKnobHasItsDefault)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    EffectInstance* effect = node->getEffectInstance().get();
    ApertureKnobs::create(effect, addPage(effect), "aperture", "Aperture");

    for (const char* group : kGroups) {
        KnobIPtr knob = effect->getKnobByName(std::string("aperture") + group);
        ASSERT_TRUE(bool(std::dynamic_pointer_cast<KnobGroup>(knob))) << group;
        EXPECT_EQ(std::string("Aperture ") + group, knob->getLabel());
    }
    EXPECT_TRUE(std::dynamic_pointer_cast<KnobGroup>(effect->getKnobByName("apertureShape"))->getDefaultValue(0));
    EXPECT_FALSE(std::dynamic_pointer_cast<KnobGroup>(effect->getKnobByName("apertureDust"))->getDefaultValue(0));

    for (const DoubleDefault& d : kDoubleDefaults) {
        const std::string name = std::string("aperture") + d.name;
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(effect->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_EQ(std::string("Aperture ") + d.label, knob->getLabel()) << name;
        ASSERT_EQ(d.dimensions, knob->getDimension()) << name;
        for (int dim = 0; dim < d.dimensions; ++dim) {
            EXPECT_EQ(d.value, knob->getDefaultValue(dim)) << name << " " << dim;
        }
        EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobGroup>(knob->getParentKnob()))) << name;
    }

    KnobIntPtr blades = std::dynamic_pointer_cast<KnobInt>(effect->getKnobByName("apertureBlades"));
    ASSERT_TRUE(bool(blades));
    EXPECT_EQ(6, blades->getDefaultValue(0));
    EXPECT_EQ(2, blades->getMinimum(0));
    EXPECT_EQ(std::string("Aperture Blades"), blades->getLabel());
    EXPECT_EQ(effect->getKnobByName("apertureShape"), blades->getParentKnob());
}

TEST_F(OpticsApertureKnobsTest, ParamsReflectEdits)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    EffectInstance* effect = node->getEffectInstance().get();
    const ApertureKnobs knobs = ApertureKnobs::create(effect, addPage(effect), "aperture", "Aperture");

    const ApertureParams defaults = knobs.params(0.);
    const ApertureParams expected;
    EXPECT_EQ(expected.sizeX, defaults.sizeX);
    EXPECT_EQ(expected.blades, defaults.blades);
    EXPECT_EQ(expected.dustParallaxY, defaults.dustParallaxY);
    EXPECT_EQ(expected.imageWhite, defaults.imageWhite);

    auto setDouble = [&](const char* name, double value, int dim) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(effect->getKnobByName(std::string("aperture") + name));
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value, ViewSpec::all(), dim);
    };
    setDouble("Size", 0.5, 0);
    setDouble("Size", 0.25, 1);
    setDouble("Roundness", 0.2, 0);
    setDouble("Rotation", 30., 0);
    setDouble("Softness", 0.1, 0);
    setDouble("GratingStrength", 0.3, 0);
    setDouble("GratingDensity", 0.4, 0);
    setDouble("GratingLength", 0.6, 0);
    setDouble("GratingWidth", 0.7, 0);
    setDouble("GratingSoftness", 0.8, 0);
    setDouble("ScratchesStrength", 0.15, 0);
    setDouble("ScratchesDensity", 0.25, 0);
    setDouble("ScratchesLength", 0.35, 0);
    setDouble("ScratchesWidth", 0.45, 0);
    setDouble("ScratchesRotation", 45., 0);
    setDouble("ScratchesRotationVariation", 0.55, 0);
    setDouble("ScratchesSoftness", 0.65, 0);
    setDouble("ScratchesParallax", 0.2, 0);
    setDouble("ScratchesParallax", 0.3, 1);
    setDouble("DustStrength", 0.9, 0);
    setDouble("DustDensity", 0.11, 0);
    setDouble("DustRadius", 0.22, 0);
    setDouble("DustSoftness", 0.33, 0);
    setDouble("DustParallax", 0.4, 0);
    setDouble("DustParallax", 0.6, 1);
    setDouble("ImageStrength", 0.5, 0);
    setDouble("ImageSize", 0.9, 0);
    setDouble("ImageSize", 0.8, 1);
    setDouble("ImageBlack", 0.1, 0);
    setDouble("ImageWhite", 0.9, 0);
    KnobIntPtr blades = std::dynamic_pointer_cast<KnobInt>(effect->getKnobByName("apertureBlades"));
    ASSERT_TRUE(bool(blades));
    blades->setValue(9, ViewSpec::all(), 0);

    const ApertureParams p = knobs.params(0.);
    EXPECT_EQ(0.5, p.sizeX);
    EXPECT_EQ(0.25, p.sizeY);
    EXPECT_EQ(9, p.blades);
    EXPECT_EQ(0.2, p.roundness);
    EXPECT_EQ(30., p.rotation);
    EXPECT_EQ(0.1, p.softness);
    EXPECT_EQ(0.3, p.gratingStrength);
    EXPECT_EQ(0.4, p.gratingDensity);
    EXPECT_EQ(0.6, p.gratingLength);
    EXPECT_EQ(0.7, p.gratingWidth);
    EXPECT_EQ(0.8, p.gratingSoftness);
    EXPECT_EQ(0.15, p.scratchesStrength);
    EXPECT_EQ(0.25, p.scratchesDensity);
    EXPECT_EQ(0.35, p.scratchesLength);
    EXPECT_EQ(0.45, p.scratchesWidth);
    EXPECT_EQ(45., p.scratchesRotation);
    EXPECT_EQ(0.55, p.scratchesRotationVariation);
    EXPECT_EQ(0.65, p.scratchesSoftness);
    EXPECT_EQ(0.2, p.scratchesParallaxX);
    EXPECT_EQ(0.3, p.scratchesParallaxY);
    EXPECT_EQ(0.9, p.dustStrength);
    EXPECT_EQ(0.11, p.dustDensity);
    EXPECT_EQ(0.22, p.dustRadius);
    EXPECT_EQ(0.33, p.dustSoftness);
    EXPECT_EQ(0.4, p.dustParallaxX);
    EXPECT_EQ(0.6, p.dustParallaxY);
    EXPECT_EQ(0.5, p.imageStrength);
    EXPECT_EQ(0.9, p.imageSizeX);
    EXPECT_EQ(0.8, p.imageSizeY);
    EXPECT_EQ(0.1, p.imageBlack);
    EXPECT_EQ(0.9, p.imageWhite);
}

TEST_F(OpticsApertureKnobsTest, OmittedKnobsAreLeftToTheCaller)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    EffectInstance* effect = node->getEffectInstance().get();
    ApertureKnobOptions options;
    options.blades = false;
    options.rotation = false;
    const ApertureKnobs knobs = ApertureKnobs::create(effect, addPage(effect), "iris", "Iris", options);

    EXPECT_FALSE(bool(effect->getKnobByName("irisBlades")));
    EXPECT_FALSE(bool(effect->getKnobByName("irisRotation")));
    EXPECT_TRUE(bool(effect->getKnobByName("irisSize")));
    EXPECT_TRUE(bool(effect->getKnobByName("irisRoundness")));

    const ApertureParams p = knobs.params(0.);
    EXPECT_EQ(ApertureParams().blades, p.blades);
    EXPECT_EQ(ApertureParams().rotation, p.rotation);
}
