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

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawFixtures.h"
#include "RawTestSource.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawColorMatrices.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 24;
const int kHeight = 18;
const std::array<float, 3> kCameraRgb = { 0.3f, 0.2f, 0.45f };

// An XYZ (D65) to camera matrix of the kind a DNG carries, and a camera to XYZ one of the kind
// the decoder gives; unrelated, so a test can tell which one was used.
const std::array<double, 9> kDngXyzToCam = { 0.9407, -0.0889, -0.1347,
                                             -0.4287, 1.246, 0.2028,
                                             -0.2431, 0.5788, 1.5172 };
const std::array<double, 9> kCamToXyz = { 0.6722, 0.1636, 0.1194,
                                          0.2654, 0.8410, -0.1064,
                                          0.0214, -0.1563, 1.1250 };

// Entries of src/common/colormatrices.c, copied by hand: rXYZ, gXYZ, bXYZ, white.
struct TableEntry {
    std::array<int, 3> r, g, b, white;
};
const TableEntry kEnhanced5DMarkIII = { { 947891, 312958, -7126 }, { 163071, 1301834, -276596 }, { 75928, -363388, 1272232 }, { 741272, 757050, 662430 } };
const TableEntry kVendor400D = { { 561768, 248581, 21408 }, { 211548, 774429, -57526 }, { 190887, -22995, 861008 }, { 961594, 1000000, 1086395 } };
const TableEntry kAlternate400D = { { 773514, 302612, 25558 }, { 244278, 1107727, -177689 }, { 55725, -289902, 1080765 }, { 822388, 847488, 696823 } };

struct Camera {
    const char* make;
    const char* model;
};
const Camera k5DMarkIII = { "Canon", "EOS 5D Mark III" };
const Camera k400D = { "Canon", "EOS 400D" };

// The profile of an entry worked out without RawColorMath: primaries scaled so camera white is
// the entry's white at Y = 1, then Bradford straight to D65. Little CMS's detour through D50
// cancels, Bradford being a diagonal scaling in one cone space.
RawColorMath::Mat3
handComputed(const TableEntry& e)
{
    const std::array<int, 3>* primaries[3] = { &e.r, &e.g, &e.b };
    double m[3][3];
    for (int k = 0; k < 3; ++k) {
        const std::array<int, 3>& p = *primaries[k];
        const double sum = (double)p[0] + p[1] + p[2];
        const double x = p[0] / sum;
        const double y = p[1] / sum;
        m[0][k] = x / y;
        m[1][k] = 1.;
        m[2][k] = (1. - x - y) / y;
    }
    const double wsum = (double)e.white[0] + e.white[1] + e.white[2];
    const double wx = e.white[0] / wsum;
    const double wy = e.white[1] / wsum;
    const double white[3] = { wx / wy, 1., (1. - wx - wy) / wy };
    // Cramer's rule for the scales s with m * s = white.
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    double s[3];
    for (int k = 0; k < 3; ++k) {
        double a[3][3];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                a[r][c] = (c == k) ? white[r] : m[r][c];
            }
        }
        s[k] = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) / det;
    }
    RawColorMath::Mat3 toWhite;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            toWhite[r * 3 + c] = m[r][c] * s[c];
        }
    }
    const RawColorMath::Mat3 cone = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };
    const RawColorMath::Vec3 coneFrom = RawColorMath::apply(cone, { white[0], white[1], white[2] });
    const RawColorMath::Vec3 coneTo = RawColorMath::apply(cone, RawColorMath::d65White());
    RawColorMath::Mat3 scaled = cone;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            scaled[r * 3 + c] *= coneTo[r] / coneFrom[r];
        }
    }

    return RawColorMath::multiply(*RawColorMath::invert(cone), RawColorMath::multiply(scaled, toWhite));
}

std::optional<RawColorMath::Mat3>
whiteNormalisedXyzToCam(const std::array<double, 9>& xyzToCam)
{
    return RawColorMath::whiteNormalisedCameraToXyz(*RawColorMath::invert(xyzToCam));
}
} // namespace

class NativeRawInputColorProfilesTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    // A flat patch of kCameraRgb with metadata's camera keys, through RawDemosaic and
    // RawInputColor.
    NodePtr createChain(const ImageMetadata& camera)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr demosaic = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
        NodePtr inputColor = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));

        EXPECT_TRUE(source && demosaic && inputColor);
        if (!source || !demosaic || !inputColor) {
            return NodePtr();
        }
        ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
        for (const char* key : { kRawMetadataMake, kRawMetadataModel, kRawMetadataCamToXyz, kRawMetadataDngD65Matrix, kRawMetadataWbLateCorrection }) {
            if (const ImageMetadata::Value* value = camera.find(key)) {
                metadata.set(key, *value);
            }
        }
        const RawLayout layout = *RawLayout::fromPattern("RGGB");
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, [layout](int x, int y, int) { return kCameraRgb[layout.colourAt(x, y)]; }, metadata);
        connectNodes(source, demosaic, 0, true);
        connectNodes(demosaic, inputColor, 0, true);

        return inputColor;
    }

    static ImageMetadata cameraMetadata(const Camera& camera)
    {
        ImageMetadata metadata;
        RawMetadata::setMake(&metadata, camera.make);
        RawMetadata::setModel(&metadata, camera.model);

        return metadata;
    }

    static void overrideProfile(const NodePtr& node,
                                const char* id)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        effect->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride)->setValue(true);
        effect->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile)->setValueFromID(id, 0);
    }

    // kCameraRgb scaled per channel by correction, through camToXyz and the working space's
    // inverse.
    RawColorMath::Vec3 expectedWorking(const RawColorMath::Mat3& camToXyz,
                                       const RawColorMath::Vec3& correction = { 1., 1., 1. })
    {
        double workingToXyz[9];
        RawColorMath::Mat3 toXyz;
        EXPECT_TRUE(getApp()->getProject()->getColorManagement()->getWorkingToXYZ(workingToXyz));
        for (int i = 0; i < 9; ++i) {
            toXyz[i] = workingToXyz[i];
        }
        const RawColorMath::Mat3 total = RawColorMath::multiply(*RawColorMath::invert(toXyz), camToXyz);

        return RawColorMath::apply(total, { kCameraRgb[0] * correction[0], kCameraRgb[1] * correction[1], kCameraRgb[2] * correction[2] });
    }

    static void expectRendered(const NodePtr& node,
                               const RawColorMath::Vec3& expected,
                               const std::string& what)
    {
        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(4, 4, 12, 10), &plane, &error)) << what << ": " << error;
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(expected[c], rawTestPlaneValue(plane, 6, 6, c), 1e-4) << what << ", channel " << c;
        }
    }

    static void expectMatrixNear(const RawColorMath::Mat3& expected,
                                 const RawColorMath::Mat3& actual,
                                 const std::string& what)
    {
        for (int i = 0; i < 9; ++i) {
            EXPECT_NEAR(expected[i], actual[i], 1e-9) << what << ", element " << i;
        }
    }
};

// colorin reload_defaults(): the embedded matrix for a DNG that has one, the standard matrix for
// another raw file. The choice shows it while the override is off.
TEST_F(NativeRawInputColorProfilesTest, TheDefaultFollowsTheFile)
{
    struct Expectation {
        const RawFixture* fixture;
        RawInputColorProfileEnum profile;
        const char* id;
    };
    for (const Expectation& e : { Expectation { &bayerFixture(), eRawInputColorProfileStandardMatrix, kRawInputColorProfileStandardMatrix },
                                  Expectation { &linearFixture(), eRawInputColorProfileEmbeddedMatrix, kRawInputColorProfileEmbeddedMatrix } }) {
        SCOPED_TRACE(e.fixture->file);
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(e.fixture->path(), &error);
        ASSERT_TRUE(bool(header)) << error;
        EXPECT_EQ(e.profile, rawInputColorDefaultProfile(header->metadata));

        NodePtr node = createChain(header->metadata);
        ASSERT_TRUE(bool(node));
        KnobChoicePtr choice = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile);
        ASSERT_TRUE(bool(choice));
        EXPECT_EQ(std::string(e.id), choice->getActiveEntry().id);
        EXPECT_FALSE(choice->isEnabled(0));

        const std::optional<RawColorMath::Mat3> camToXyz = rawInputColorProfileMatrix(e.profile, header->metadata);
        ASSERT_TRUE(camToXyz.has_value());
        expectRendered(node, expectedWorking(*camToXyz), e.id);
    }
}

TEST_F(NativeRawInputColorProfilesTest, TheDefaultWithoutACameraMatrixIsLinearRec709)
{
    EXPECT_EQ(eRawInputColorProfileLinearRec709, rawInputColorDefaultProfile(ImageMetadata()));
    ImageMetadata both;
    RawMetadata::setCamToXyz(&both, kCamToXyz);
    EXPECT_EQ(eRawInputColorProfileStandardMatrix, rawInputColorDefaultProfile(both));
    RawMetadata::setDngD65Matrix(&both, kDngXyzToCam);
    EXPECT_EQ(eRawInputColorProfileEmbeddedMatrix, rawInputColorDefaultProfile(both));
}

TEST_F(NativeRawInputColorProfilesTest, TableMatricesMatchAHandComputation)
{
    struct Case {
        Camera camera;
        RawColorMatrices::Table table;
        const TableEntry* entry;
        RawInputColorProfileEnum profile;
        const char* id;
    };
    const Case cases[] = {
        { k5DMarkIII, RawColorMatrices::Table::Enhanced, &kEnhanced5DMarkIII, eRawInputColorProfileEnhancedMatrix, kRawInputColorProfileEnhancedMatrix },
        { k400D, RawColorMatrices::Table::Vendor, &kVendor400D, eRawInputColorProfileVendorMatrix, kRawInputColorProfileVendorMatrix },
        { k400D, RawColorMatrices::Table::Alternate, &kAlternate400D, eRawInputColorProfileAlternateMatrix, kRawInputColorProfileAlternateMatrix },
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.id);
        const RawColorMath::Mat3 expected = handComputed(*c.entry);
        const RawColorMatrices::Entry* entry = RawColorMatrices::find(c.table, RawColorMatrices::makerModel(c.camera.make, c.camera.model));
        ASSERT_TRUE(entry != nullptr);
        const std::optional<RawColorMath::Mat3> fromTable = RawColorMatrices::cameraToXyz(*entry);
        ASSERT_TRUE(fromTable.has_value());
        expectMatrixNear(expected, *fromTable, "table");

        // Camera white lands on D65 white.
        const RawColorMath::Vec3 white = RawColorMath::apply(*fromTable, { 1., 1., 1. });
        const RawColorMath::Vec3 d65 = RawColorMath::d65White();
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(d65[k], white[k], 1e-9) << k;
        }

        ImageMetadata metadata = cameraMetadata(c.camera);
        RawMetadata::setCamToXyz(&metadata, kCamToXyz);
        RawColorMath::Mat3 resolved;
        EXPECT_EQ(c.profile, rawInputColorResolveProfile(c.profile, metadata, &resolved));
        expectMatrixNear(expected, resolved, "resolved");

        NodePtr node = createChain(metadata);
        ASSERT_TRUE(bool(node));
        overrideProfile(node, c.id);
        expectRendered(node, expectedWorking(expected), c.id);
    }
}

// The enhanced table matches regardless of case, the vendor and alternate ones exactly.
TEST_F(NativeRawInputColorProfilesTest, TablesMatchTheMakerModelAsDarktableDoes)
{
    EXPECT_TRUE(RawColorMatrices::find(RawColorMatrices::Table::Enhanced, "CANON EOS 5D MARK III") != nullptr);
    EXPECT_TRUE(RawColorMatrices::find(RawColorMatrices::Table::Vendor, "Canon EOS 400D") != nullptr);
    EXPECT_TRUE(RawColorMatrices::find(RawColorMatrices::Table::Vendor, "CANON EOS 400D") == nullptr);
    EXPECT_TRUE(RawColorMatrices::find(RawColorMatrices::Table::Alternate, "canon EOS 400D") == nullptr);
    EXPECT_TRUE(RawColorMatrices::find(RawColorMatrices::Table::Vendor, "Canon EOS 5D Mark III") == nullptr);
}

// colorin commit_params(): a table matrix the camera has not falls back to the embedded matrix,
// that to the standard matrix, that to linear Rec709.
TEST_F(NativeRawInputColorProfilesTest, AnUnavailableProfileFallsBackAsDarktableDoes)
{
    const RawColorMath::Mat3 standard = *RawColorMath::whiteNormalisedCameraToXyz(kCamToXyz);
    const RawColorMath::Mat3 embedded = *whiteNormalisedXyzToCam(kDngXyzToCam);

    ImageMetadata camOnly = cameraMetadata(k5DMarkIII);
    RawMetadata::setCamToXyz(&camOnly, kCamToXyz);
    ImageMetadata withDng = camOnly;
    RawMetadata::setDngD65Matrix(&withDng, kDngXyzToCam);
    const ImageMetadata nothing = cameraMetadata(k5DMarkIII);

    for (RawInputColorProfileEnum missing : { eRawInputColorProfileVendorMatrix, eRawInputColorProfileAlternateMatrix }) {
        EXPECT_FALSE(rawInputColorProfileMatrix(missing, camOnly).has_value());
        RawColorMath::Mat3 m;
        EXPECT_EQ(eRawInputColorProfileStandardMatrix, rawInputColorResolveProfile(missing, camOnly, &m));
        expectMatrixNear(standard, m, "standard");
        EXPECT_EQ(eRawInputColorProfileEmbeddedMatrix, rawInputColorResolveProfile(missing, withDng, &m));
        expectMatrixNear(embedded, m, "embedded");
        EXPECT_EQ(eRawInputColorProfileLinearRec709, rawInputColorResolveProfile(missing, nothing, &m));
        expectMatrixNear(RawColorMath::srgbToXyz(), m, "linear Rec709");
    }
    RawColorMath::Mat3 m;
    EXPECT_EQ(eRawInputColorProfileStandardMatrix, rawInputColorResolveProfile(eRawInputColorProfileEmbeddedMatrix, camOnly, &m));
    EXPECT_EQ(eRawInputColorProfileLinearRec709, rawInputColorResolveProfile(eRawInputColorProfileStandardMatrix, nothing, &m));

    NodePtr node = createChain(camOnly);
    ASSERT_TRUE(bool(node));
    overrideProfile(node, kRawInputColorProfileVendorMatrix);
    expectRendered(node, expectedWorking(standard), "vendor falling back to standard");
}

// The late correction is folded into whichever profile's matrix is used.
TEST_F(NativeRawInputColorProfilesTest, TheLateCorrectionStillApplies)
{
    const RawColorMath::Vec3 correction = { 1.3, 1., 0.8 };
    ImageMetadata metadata = cameraMetadata(k5DMarkIII);
    RawMetadata::setCamToXyz(&metadata, kCamToXyz);
    RawMetadata::setWbLateCorrection(&metadata, correction);

    NodePtr node = createChain(metadata);
    ASSERT_TRUE(bool(node));
    expectRendered(node, expectedWorking(*RawColorMath::whiteNormalisedCameraToXyz(kCamToXyz), correction), "standard");
    overrideProfile(node, kRawInputColorProfileEnhancedMatrix);
    expectRendered(node, expectedWorking(handComputed(kEnhanced5DMarkIII), correction), "enhanced");
}

// A project saves the choice by id, so the order of the list does not matter.
TEST_F(NativeRawInputColorProfilesTest, ProfilesAreIdentifiedById)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));
    ASSERT_TRUE(bool(node));
    KnobChoicePtr choice = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile);
    ASSERT_TRUE(bool(choice));
    const RawInputColorProfileEnum profiles[] = {
        eRawInputColorProfileEmbeddedMatrix,
        eRawInputColorProfileStandardMatrix,
        eRawInputColorProfileEnhancedMatrix,
        eRawInputColorProfileVendorMatrix,
        eRawInputColorProfileAlternateMatrix,
        eRawInputColorProfileLinearRec709,
        eRawInputColorProfileCustom,
    };
    const char* const ids[] = {
        kRawInputColorProfileEmbeddedMatrix,
        kRawInputColorProfileStandardMatrix,
        kRawInputColorProfileEnhancedMatrix,
        kRawInputColorProfileVendorMatrix,
        kRawInputColorProfileAlternateMatrix,
        kRawInputColorProfileLinearRec709,
        kRawInputColorProfileCustom,
    };
    for (int i = 0; i < 7; ++i) {
        choice->setValueFromID(ids[i], 0);
        EXPECT_EQ(std::string(ids[i]), choice->getActiveEntry().id);
        EXPECT_EQ(profiles[i], rawInputColorProfileAt(choice->getValue()));
        EXPECT_EQ(choice->getValue(), rawInputColorProfileIndex(profiles[i]));
    }
}
