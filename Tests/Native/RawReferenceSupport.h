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

#ifndef Tests_Native_RawReferenceSupport_h
#define Tests_Native_RawReferenceSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "RawFixtures.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawDevelop.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace RawReference {
// The crop tools/raw/make-reference.py takes from darktable's developed image: kCropSize pixels
// square, its top-left corner (left, top) from the top-left of the developed image.
const int kCropSize = 192;

// A tenth of a per cent of the crop's pixels.
const int kMarkesteijnOutliers = kCropSize * kCropSize / 1000;

/**
 * The bounds the errors of one comparison must stay within, in linear Rec. 709 relative to the
 * reference's mean value over the crop: a row of the first table of Tolerances in
 * Tests/fixtures/raw/README.md.
 **/
struct Tolerance {
    double mean;
    double max;
    // How many pixels may exceed max. Markesteijn picks the directions it averages by comparing
    // integer homogeneity counts with a threshold, so a 1e-5 relative change in the mosaic can
    // drop or add a direction and move an isolated pixel by over 15 %. A different algorithm
    // moves thousands of pixels of the crop that far.
    int outlierBudget;
};

const Tolerance kRCDClass = { 1e-3, 2e-2, 0 };
const Tolerance kAMaZEClass = { 3e-3, 5e-2, 0 };
const Tolerance kMarkesteijnClass = { 3e-3, 5e-2, kMarkesteijnOutliers };

/**
 * Where a camera file's crop sits: the size of darktable's developed image, which RawPrepare
 * must keep, and the crop's top-left corner from its top-left.
 **/
struct CropSite {
    const RawFixture* fixture;
    int developedWidth;
    int developedHeight;
    int left;
    int top;
    bool xtrans;
};

inline CropSite
cr2Site()
{
    return { &bayerFixture(), 5794, 3868, 3072, 1728, false };
}

inline CropSite
rafSite()
{
    return { &xtransFixture(), 6032, 4032, 3264, 3072, true };
}

struct ErrorStats {
    double mean = 0.;
    double max = 0.;
    double level = 0.;
    int maxX = 0;
    int maxY = 0;
    int outliers = 0;
};

inline bool
readReference(const std::string& path,
              std::vector<float>* pixels,
              int* width,
              int* height,
              std::string* error)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);

    if (!input) {
        *error = "cannot open " + path + ": " + OIIO::geterror();

        return false;
    }
    const OIIO::ImageSpec spec = input->spec();
    if (spec.nchannels < 3) {
        *error = path + " has fewer than three channels";

        return false;
    }
    std::vector<float> all((std::size_t)spec.width * spec.height * spec.nchannels);
    const bool ok = input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, all.data());
    input->close();
    if (!ok) {
        *error = "cannot read " + path;

        return false;
    }
    pixels->resize((std::size_t)spec.width * spec.height * 3);
    for (std::size_t i = 0; i < (std::size_t)spec.width * spec.height; ++i) {
        for (int c = 0; c < 3; ++c) {
            (*pixels)[i * 3 + c] = all[i * spec.nchannels + c];
        }
    }
    *width = spec.width;
    *height = spec.height;

    return true;
}

/**
 * Half the spacing of half floats around v: how far a reference stored as half float may sit
 * from the float darktable exported.
 **/
inline double
halfFloatRounding(double v)
{
    const double a = std::fabs(v);

    // Below 2^-14 half floats are subnormal, 2^-24 apart.
    if (a < std::ldexp(1., -14)) {
        return std::ldexp(1., -25);
    }
    int exponent = 0;
    (void)std::frexp(a, &exponent);

    // a is in [2^(exponent - 1), 2^exponent), where half floats are 2^(exponent - 11) apart.
    return std::ldexp(1., exponent - 12);
}

/**
 * The error of got against a sample of a half float reference, beyond the reference's own
 * rounding, so the storage format does not count against a tolerance.
 **/
inline double
halfFloatError(double got,
               double want)
{
    return std::max(0., std::fabs(got - want) - halfFloatRounding(want));
}
} // namespace RawReference

/**
 * Develops a camera fixture through the Develop RAW chain and compares a crop of RawInputColor's
 * output with one of darktable's reference TIFFs in Tests/fixtures/raw/reference/.
 **/
class RawReferenceFixture
    : public BaseTest {
protected:
    NodePtr createRead(const RawFixture& fixture)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ));

        EXPECT_TRUE(bool(read));
        if (read) {
            KnobFilePtr file = read->getEffectInstance()->getKnobByNameAndType<KnobFile>(kOfxImageEffectFileParamName);
            EXPECT_TRUE(bool(file));
            if (file) {
                file->setValue(fixture.path());
            }
        }

        return read;
    }

    // A Read of site's file with the Develop RAW chain under it, at its defaults: darktable's
    // under its default scene-referred workflow, which the scene-referred default jobs render.
    RawDevelop::Chain createDefaultChain(const RawReference::CropSite& site)
    {
        NodePtr read = createRead(*site.fixture);

        if (!read) {
            return RawDevelop::Chain();
        }

        return RawDevelop::buildChain(read);
    }

    // The Develop RAW chain with RawTemperature on As Shot, darktable's default under the
    // workflow "none" every other job runs with. Those jobs end at colorin, so the chain's
    // RawInputColor is what they compare.
    RawDevelop::Chain createChain(const RawReference::CropSite& site)
    {
        RawDevelop::Chain chain = createDefaultChain(site);

        if (chain.temperature) {
            KnobChoicePtr mode = chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode);
            EXPECT_TRUE(bool(mode));
            if (mode) {
                mode->setValue((int)eRawTemperatureModeAsShot);
            }
        }

        return chain;
    }

    // Puts node between the Develop chain's node below and its input, as darktable's pipe order
    // puts a module between two of the chain's.
    void insertAbove(const NodePtr& below,
                     const NodePtr& node)
    {
        ASSERT_TRUE(below && node);
        NodePtr above = below->getInput(0);
        ASSERT_TRUE(bool(above));
        disconnectNodes(above, below, true);
        connectNodes(above, node, 0, true);
        connectNodes(node, below, 0, true);
    }

    static RectD regionOfDefinition(const NodePtr& node)
    {
        RectD rod;
        bool isProjectFormat = false;
        EffectInstancePtr effect = node->getEffectInstance();
        EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));

        return rod;
    }

    // The project working space to linear Rec. 709, which RawInputColor's output is converted by
    // to compare with darktable's export.
    RawColorMath::Mat3 workingToRec709() const
    {
        const RawColorMath::Mat3 xyzToRec709 = *RawColorMath::invert(RawColorMath::srgbToXyz());
        ProjectColorManagementPtr cm = getApp()->getProject()->getColorManagement();
        double m[9];

        if (!cm || !cm->getWorkingToXYZ(m)) {
            return RawColorMath::multiply(xyzToRec709, RawColorMath::srgbToXyz());
        }
        RawColorMath::Mat3 workingToXyz;
        for (int i = 0; i < 9; ++i) {
            workingToXyz[i] = m[i];
        }

        return RawColorMath::multiply(xyzToRec709, workingToXyz);
    }

    /**
     * Renders site's crop of output, converts it to linear Rec. 709 and compares it with
     * reference, printing the mean and largest errors relative to the reference's mean level.
     **/
    void compareCrop(const char* name,
                     const RawReference::CropSite& site,
                     const NodePtr& output,
                     const char* reference,
                     const RawReference::Tolerance& tolerance)
    {
        using namespace RawReference;

        std::vector<float> expectedPixels;
        int refWidth = 0;
        int refHeight = 0;
        std::string error;
        ASSERT_TRUE(readReference(std::string(NATRON_TESTS_FIXTURES_DIR "/raw/reference/") + reference, &expectedPixels, &refWidth, &refHeight, &error)) << error;
        ASSERT_EQ(kCropSize, refWidth) << name;
        ASSERT_EQ(kCropSize, refHeight) << name;
        ASSERT_TRUE(bool(output)) << name;

        // darktable's image is the sensor crop RawPrepare keeps; a different size means the two
        // disagree about the crop and no pixel lines up.
        const RectD rod = regionOfDefinition(output);
        ASSERT_EQ(site.developedWidth, (int)rod.width()) << name;
        ASSERT_EQ(site.developedHeight, (int)rod.height()) << name;

        // Natron's rows count from the bottom.
        const int y1 = (int)rod.y2 - site.top - kCropSize;
        const RectI window((int)rod.x1 + site.left, y1, (int)rod.x1 + site.left + kCropSize, y1 + kCropSize);
        RenderedPlane plane;
        ASSERT_TRUE(renderRawTestPlane(output, window, &plane, &error)) << name << ": " << error;
        ASSERT_GE(plane.channels.size(), 3u) << name;

        const RawColorMath::Mat3 toRec709 = workingToRec709();
        ErrorStats stats;
        double sum = 0.;
        double level = 0.;
        std::vector<double> pixelErrors;
        pixelErrors.reserve((std::size_t)kCropSize * kCropSize);
        for (int row = 0; row < kCropSize; ++row) {
            const int y = window.y2 - 1 - row;
            for (int col = 0; col < kCropSize; ++col) {
                const int x = window.x1 + col;
                double pixelMax = 0.;
                const RawColorMath::Vec3 working = { rawTestPlaneValue(plane, x, y, 0), rawTestPlaneValue(plane, x, y, 1), rawTestPlaneValue(plane, x, y, 2) };
                const RawColorMath::Vec3 natron = RawColorMath::apply(toRec709, working);
                for (int ch = 0; ch < 3; ++ch) {
                    const double expected = expectedPixels[((std::size_t)row * kCropSize + col) * 3 + ch];
                    const double diff = std::fabs(natron[ch] - expected);
                    level += std::fabs(expected);
                    sum += diff;
                    pixelMax = std::max(pixelMax, diff);
                    if (diff > stats.max) {
                        stats.max = diff;
                        stats.maxX = col;
                        stats.maxY = row;
                    }
                }
                pixelErrors.push_back(pixelMax);
            }
        }
        const double count = 3. * kCropSize * kCropSize;
        stats.mean = sum / count;
        stats.level = level / count;
        ASSERT_GT(stats.level, 0.) << name;
        const double relMean = stats.mean / stats.level;
        const double relMax = stats.max / stats.level;
        stats.outliers = (int)std::count_if(pixelErrors.begin(), pixelErrors.end(), [&](double e) { return e > tolerance.max * stats.level; });
        std::printf("%s against darktable: mean error %g (%.4f%% of the mean level %g), max error %g (%.3f%%) at crop pixel %d,%d from the top-left, %d pixels over %.0f%%\n",
                    name, stats.mean, 100. * relMean, stats.level, stats.max, 100. * relMax, stats.maxX, stats.maxY, stats.outliers, 100. * tolerance.max);
        EXPECT_LE(relMean, tolerance.mean) << name;
        EXPECT_LE(stats.outliers, tolerance.outlierBudget) << name << ": largest error " << 100. * relMax << " % at " << stats.maxX << "," << stats.maxY;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_Native_RawReferenceSupport_h
