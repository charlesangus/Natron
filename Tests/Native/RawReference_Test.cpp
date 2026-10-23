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
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawDevelop.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
// The crop tools/raw/make-reference.py takes from darktable's developed image: kCropSize pixels
// square, its top-left corner (left, top) from the top-left of the developed image.
const int kCropSize = 192;

/**
 * The bounds the errors of one demosaic method must stay within, in linear Rec. 709 relative to
 * the reference's mean value over the crop (see Tests/fixtures/raw/README.md for the reasoning).
 **/
struct ReferenceCase {
    const char* name;
    const RawFixture* fixture;
    int developedWidth;
    int developedHeight;
    int left;
    int top;
    bool xtrans;
    int method;
    const char* reference;
    double meanTolerance;
    double maxTolerance;
};

struct ErrorStats {
    double mean = 0.;
    double max = 0.;
    double level = 0.;
    int maxX = 0;
    int maxY = 0;
};

bool
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
} // namespace

class RawReferenceTest
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

    void compareWithReference(const ReferenceCase& c)
    {
        std::vector<float> reference;
        int refWidth = 0;
        int refHeight = 0;
        std::string error;
        ASSERT_TRUE(readReference(std::string(NATRON_TESTS_FIXTURES_DIR "/raw/reference/") + c.reference, &reference, &refWidth, &refHeight, &error)) << error;
        ASSERT_EQ(kCropSize, refWidth) << c.name;
        ASSERT_EQ(kCropSize, refHeight) << c.name;

        NodePtr read = createRead(*c.fixture);
        ASSERT_TRUE(bool(read));
        const RawDevelop::Chain chain = RawDevelop::buildChain(read);
        ASSERT_TRUE(chain.complete()) << c.name;
        EffectInstancePtr demosaic = chain.demosaic->getEffectInstance();
        demosaic->getKnobByNameAndType<KnobChoice>(c.xtrans ? kRawDemosaicParamXTransMethod : kRawDemosaicParamBayerMethod)->setValue(c.method);

        // darktable's image is the sensor crop RawPrepare keeps; a different size means the two
        // disagree about the crop and no pixel lines up.
        const RectD rod = regionOfDefinition(chain.inputColor);
        ASSERT_EQ(c.developedWidth, (int)rod.width()) << c.name;
        ASSERT_EQ(c.developedHeight, (int)rod.height()) << c.name;

        // Natron's rows count from the bottom.
        const int y1 = (int)rod.y2 - c.top - kCropSize;
        const RectI window((int)rod.x1 + c.left, y1, (int)rod.x1 + c.left + kCropSize, y1 + kCropSize);
        RenderedPlane plane;
        ASSERT_TRUE(renderRawTestPlane(chain.inputColor, window, &plane, &error)) << c.name << ": " << error;
        ASSERT_GE(plane.channels.size(), 3u) << c.name;

        const RawColorMath::Mat3 toRec709 = workingToRec709();
        ErrorStats stats;
        double sum = 0.;
        double level = 0.;
        for (int row = 0; row < kCropSize; ++row) {
            const int y = window.y2 - 1 - row;
            for (int col = 0; col < kCropSize; ++col) {
                const int x = window.x1 + col;
                const RawColorMath::Vec3 working = { rawTestPlaneValue(plane, x, y, 0), rawTestPlaneValue(plane, x, y, 1), rawTestPlaneValue(plane, x, y, 2) };
                const RawColorMath::Vec3 natron = RawColorMath::apply(toRec709, working);
                for (int ch = 0; ch < 3; ++ch) {
                    const double expected = reference[((std::size_t)row * kCropSize + col) * 3 + ch];
                    const double diff = std::fabs(natron[ch] - expected);
                    level += std::fabs(expected);
                    sum += diff;
                    if (diff > stats.max) {
                        stats.max = diff;
                        stats.maxX = col;
                        stats.maxY = row;
                    }
                }
            }
        }
        const double count = 3. * kCropSize * kCropSize;
        stats.mean = sum / count;
        stats.level = level / count;
        ASSERT_GT(stats.level, 0.) << c.name;
        const double relMean = stats.mean / stats.level;
        const double relMax = stats.max / stats.level;
        std::printf("%s against darktable: mean error %g (%.4f%% of the mean level %g), max error %g (%.3f%%) at crop pixel %d,%d from the top-left\n",
                    c.name, stats.mean, 100. * relMean, stats.level, stats.max, 100. * relMax, stats.maxX, stats.maxY);
        EXPECT_LE(relMean, c.meanTolerance) << c.name;
        EXPECT_LE(relMax, c.maxTolerance) << c.name;
    }
};

TEST_F(RawReferenceTest, BayerPPGMatchesDarktable)
{
    compareWithReference({ "Bayer PPG", &bayerFixture(), 5794, 3868, 3072, 1728, false, (int)eRawDemosaicBayerPPG, "canon-eos-5d-mark-iii-ppg.tif", 1e-3, 2e-2 });
}

TEST_F(RawReferenceTest, BayerRCDMatchesDarktable)
{
    compareWithReference({ "Bayer RCD", &bayerFixture(), 5794, 3868, 3072, 1728, false, (int)eRawDemosaicBayerRCD, "canon-eos-5d-mark-iii-rcd.tif", 1e-3, 2e-2 });
}

TEST_F(RawReferenceTest, BayerAMaZEMatchesDarktable)
{
    compareWithReference({ "Bayer AMaZE", &bayerFixture(), 5794, 3868, 3072, 1728, false, (int)eRawDemosaicBayerAMaZE, "canon-eos-5d-mark-iii-amaze.tif", 3e-3, 5e-2 });
}

TEST_F(RawReferenceTest, XTransMarkesteijnMatchesDarktable)
{
    compareWithReference({ "X-Trans Markesteijn 1-pass", &xtransFixture(), 6032, 4032, 3264, 3072, true, (int)eRawDemosaicXTransMarkesteijn1, "fujifilm-x-pro2-markesteijn.tif", 3e-3, 5e-2 });
}

TEST_F(RawReferenceTest, XTransMarkesteijn3MatchesDarktable)
{
    compareWithReference({ "X-Trans Markesteijn 3-pass", &xtransFixture(), 6032, 4032, 3264, 3072, true, (int)eRawDemosaicXTransMarkesteijn3, "fujifilm-x-pro2-markesteijn3.tif", 3e-3, 5e-2 });
}
