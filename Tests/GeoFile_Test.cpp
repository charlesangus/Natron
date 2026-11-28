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
#include <climits>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoFileFormat.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoFile.h"
#include "Engine/Nodes/IO/ReadTimeDomain.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

static_assert(GeoFile::kNodeSpec.outputKind == eDataKindGeometry, "GeoFile produces geometry");

TEST(GeoFileTest, FrameTokens)
{
    EXPECT_TRUE(GeoFile::isFrameSequencePattern("mesh.####.ngeo"));
    EXPECT_TRUE(GeoFile::isFrameSequencePattern("mesh.%04d.ngeo"));
    EXPECT_TRUE(GeoFile::isFrameSequencePattern("mesh.$F.ngeo"));
    EXPECT_TRUE(GeoFile::isFrameSequencePattern("mesh.$F4.ngeo"));
    EXPECT_FALSE(GeoFile::isFrameSequencePattern("mesh.ngeo"));
    EXPECT_FALSE(GeoFile::isFrameSequencePattern("mesh.%v.ngeo"));
    EXPECT_FALSE(GeoFile::isFrameSequencePattern("$FOO/mesh.ngeo"));

    EXPECT_EQ(std::string("mesh.%d.ngeo"), GeoFile::normalizeFrameTokens("mesh.$F.ngeo"));
    EXPECT_EQ(std::string("mesh.%04d.ngeo"), GeoFile::normalizeFrameTokens("mesh.$F4.ngeo"));
    EXPECT_EQ(std::string("$FOO/mesh.%02d.ngeo"), GeoFile::normalizeFrameTokens("$FOO/mesh.$F2.ngeo"));
    EXPECT_EQ(std::string("mesh.####.ngeo"), GeoFile::normalizeFrameTokens("mesh.####.ngeo"));
}

namespace {
GeoDetail
makePoints(GeoIndex count)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(count);
    for (GeoIndex i = 0; i < count; ++i) {
        b.setPointPosition(first + i, double(i), 0., 0.);
    }

    return b.finish();
}
} // namespace

class GeoFileNodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ASSERT_TRUE(_tmp.isValid());
        _reader = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOFILE));
        ASSERT_TRUE(_reader != NULL);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_reader) {
            appPTR->removeAllCacheEntriesForHolder(_reader.get(), true);
            _reader->destroyNode(false, false);
            _reader.reset();
        }
        BaseTest::TearDown();
    }

    std::string path(const char* name) const
    {
        return _tmp.filePath(QString::fromUtf8(name)).toStdString();
    }

    KnobFilePtr fileKnob() const
    {
        return std::dynamic_pointer_cast<KnobFile>(_reader->getKnobByName(kGeoFileParamFile));
    }

    void setMissingFrames(ReadTimeDomain::MissingFrame mode)
    {
        setChoice("onMissingFrame", (int)mode);
    }

    void setChoice(const char* name,
                   int value)
    {
        const KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(_reader->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    // As the user edits it, so the time domain no longer follows the files.
    void editInt(const char* name,
                 int value)
    {
        const KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(_reader->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }

    int intValue(const char* name,
                 int dimension = 0) const
    {
        const KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(_reader->getKnobByName(name));

        return knob ? knob->getValue(dimension) : INT_MIN;
    }

    void frameRange(double* first,
                    double* last) const
    {
        _reader->getEffectInstance()->getFrameRange_public(0, first, last, true);
    }

    // Through the geometry cache, so a wrong cache key serves stale geometry. -1 on failure.
    GeoIndex pointCountAt(double time,
                          std::string* error = NULL)
    {
        GeoDetailConstPtr detail;
        std::string message;
        const EffectInstance::RenderRoIRetCode ret = _reader->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0), false /*byPassCache*/), &detail, &message);
        if (error) {
            *error = message;
        }
        if (ret != EffectInstance::eRenderRoIRetCodeOk || !detail) {
            return -1;
        }

        return detail->pointCount();
    }

    QTemporaryDir _tmp;
    NodePtr _reader;
};

TEST_F(GeoFileNodeTest, ReadsASingleFileExactly)
{
    const GeoDetail written = makePoints(5);
    geoFileWrite(written, path("mesh.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.ngeo"));
    EXPECT_FALSE(_reader->getEffectInstance()->getTimeInvariantMetadata()->getIsFrameVarying());

    GeoDetailConstPtr detail;
    std::string error;
    const EffectInstance::RenderRoIRetCode ret = _reader->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0), false /*byPassCache*/), &detail, &error);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, ret) << error;
    ASSERT_TRUE(detail != nullptr);
    EXPECT_EQ(written.contentHash(), detail->contentHash());
}

TEST_F(GeoFileNodeTest, EmptyNameGivesEmptyGeometry)
{
    EXPECT_EQ(0, pointCountAt(1.));
}

TEST_F(GeoFileNodeTest, ASequenceIsFrameVaryingAndEachFrameReadsItsFile)
{
    geoFileWrite(makePoints(3), path("mesh.0001.ngeo"));
    geoFileWrite(makePoints(4), path("mesh.0002.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);

    file->setValue(path("mesh.$F4.ngeo"));
    EXPECT_TRUE(_reader->getEffectInstance()->getTimeInvariantMetadata()->getIsFrameVarying());
    EXPECT_EQ(3, pointCountAt(1.));
    EXPECT_EQ(4, pointCountAt(2.));
    EXPECT_EQ(3, pointCountAt(1.));

    file->setValue(path("mesh.####.ngeo"));
    EXPECT_EQ(4, pointCountAt(2.));
}

TEST_F(GeoFileNodeTest, MissingFrames)
{
    geoFileWrite(makePoints(3), path("mesh.0001.ngeo"));
    geoFileWrite(makePoints(6), path("mesh.0004.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.####.ngeo"));

    std::string error;
    EXPECT_EQ(-1, pointCountAt(2., &error));
    EXPECT_NE(std::string::npos, error.find("mesh.0002.ngeo")) << error;

    setMissingFrames(ReadTimeDomain::eMissingNearest);
    EXPECT_EQ(3, pointCountAt(2.));
    EXPECT_EQ(6, pointCountAt(3.));
    EXPECT_EQ(6, pointCountAt(10.));
    EXPECT_EQ(3, pointCountAt(-5.));

    setMissingFrames(ReadTimeDomain::eMissingBlack);
    EXPECT_EQ(0, pointCountAt(2.));
    EXPECT_EQ(6, pointCountAt(4.));
}

TEST_F(GeoFileNodeTest, AnUnreadableFileIsAnErrorWhateverTheMissingFrameRule)
{
    {
        FILE* f = fopen(path("bad.0001.ngeo").c_str(), "wb");
        ASSERT_TRUE(f != NULL);
        fputs("not geometry", f);
        fclose(f);
    }
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("bad.####.ngeo"));
    setMissingFrames(ReadTimeDomain::eMissingBlack);

    std::string error;
    EXPECT_EQ(-1, pointCountAt(1., &error));
    EXPECT_NE(std::string::npos, error.find("bad.0001.ngeo")) << error;
}

TEST_F(GeoFileNodeTest, AFramelessFileIsReadOnceAcrossFramesUntilReload)
{
    geoFileWrite(makePoints(5), path("mesh.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.ngeo"));
    EXPECT_EQ(5, pointCountAt(1.));

    // Gone from disk, the file is still served from the geometry cache at every frame.
    ASSERT_EQ(0, std::remove(path("mesh.ngeo").c_str()));
    EXPECT_EQ(5, pointCountAt(2.));
    EXPECT_EQ(5, pointCountAt(7.));

    geoFileWrite(makePoints(8), path("mesh.ngeo"));
    EXPECT_EQ(5, pointCountAt(3.));
    KnobButton* reload = dynamic_cast<KnobButton*>(_reader->getKnobByName(kGeoFileParamReload).get());
    ASSERT_TRUE(reload != NULL);
    reload->trigger();
    EXPECT_EQ(8, pointCountAt(3.));
}

TEST_F(GeoFileNodeTest, ACorruptAttributeNameIsAnError)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(4);
    for (GeoIndex i = 0; i < 4; ++i) {
        b.setPointPosition(first + i, double(i), 0., 0.);
    }
    (void)b.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "marker", eGeoStorageInt32, 1, eGeoTypeInfoNone, false });
    geoFileWrite(b.finish(), path("mesh.ngeo"));

    std::vector<char> bytes;
    {
        std::ifstream in(path("mesh.ngeo"), std::ios::binary);
        ASSERT_TRUE(in.good());
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string name = "marker";
    const std::vector<char>::iterator at = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
    ASSERT_TRUE(at != bytes.end());
    // "markes": still a valid name, so only the content hash can tell.
    *(at + (name.size() - 1)) = 's';
    {
        std::ofstream out(path("mesh.ngeo"), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out.write(bytes.data(), (std::streamsize)bytes.size());
    }

    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.ngeo"));
    std::string error;
    EXPECT_EQ(-1, pointCountAt(1., &error));
    EXPECT_NE(std::string::npos, error.find("mesh.ngeo")) << error;
    EXPECT_NE(std::string::npos, error.find("content hash")) << error;
}

TEST_F(GeoFileNodeTest, ASequenceIsReadAsStampedUntilReload)
{
    geoFileWrite(makePoints(3), path("mesh.0001.ngeo"));
    geoFileWrite(makePoints(6), path("mesh.0003.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.####.ngeo"));
    setMissingFrames(ReadTimeDomain::eMissingNearest);
    EXPECT_EQ(3, pointCountAt(1.));

    geoFileWrite(makePoints(7), path("mesh.0001.ngeo"));
    geoFileWrite(makePoints(4), path("mesh.0002.ngeo"));
    ASSERT_EQ(0, std::remove(path("mesh.0003.ngeo").c_str()));
    appPTR->removeAllCacheEntriesForHolder(_reader.get(), true);

    std::string error;
    EXPECT_EQ(-1, pointCountAt(1., &error));
    EXPECT_NE(std::string::npos, error.find("changed on disk")) << error;
    EXPECT_EQ(-1, pointCountAt(2., &error));
    EXPECT_NE(std::string::npos, error.find("mesh.0002.ngeo")) << error;
    EXPECT_EQ(-1, pointCountAt(3., &error));
    EXPECT_NE(std::string::npos, error.find("mesh.0003.ngeo")) << error;

    KnobButton* reload = dynamic_cast<KnobButton*>(_reader->getKnobByName(kGeoFileParamReload).get());
    ASSERT_TRUE(reload != NULL);
    reload->trigger();
    EXPECT_EQ(7, pointCountAt(1.));
    EXPECT_EQ(4, pointCountAt(2.));
    EXPECT_EQ(4, pointCountAt(3.));
}

TEST_F(GeoFileNodeTest, TheFrameRangeFollowsTheFilesUntilEdited)
{
    geoFileWrite(makePoints(3), path("mesh.0002.ngeo"));
    geoFileWrite(makePoints(4), path("mesh.0005.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.####.ngeo"));

    EXPECT_EQ(2, intValue(kReaderParamNameOriginalFrameRange, 0));
    EXPECT_EQ(5, intValue(kReaderParamNameOriginalFrameRange, 1));
    EXPECT_EQ(2, intValue("firstFrame"));
    EXPECT_EQ(5, intValue("lastFrame"));
    double first = 0.;
    double last = 0.;
    frameRange(&first, &last);
    EXPECT_EQ(2., first);
    EXPECT_EQ(5., last);

    editInt("lastFrame", 4);
    geoFileWrite(makePoints(5), path("mesh.0007.ngeo"));
    KnobButton* reload = dynamic_cast<KnobButton*>(_reader->getKnobByName(kGeoFileParamReload).get());
    ASSERT_TRUE(reload != NULL);
    reload->trigger();
    EXPECT_EQ(7, intValue(kReaderParamNameOriginalFrameRange, 1));
    EXPECT_EQ(4, intValue("lastFrame"));
}

TEST_F(GeoFileNodeTest, ASingleFileIsFrameOne)
{
    geoFileWrite(makePoints(5), path("mesh.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.ngeo"));

    EXPECT_EQ(1, intValue("firstFrame"));
    EXPECT_EQ(1, intValue("lastFrame"));
    EXPECT_EQ(5, pointCountAt(-3.));
    EXPECT_EQ(5, pointCountAt(40.));

    setChoice("afterLastFrame", (int)ReadTimeDomain::eBeforeAfterBlack);
    EXPECT_TRUE(_reader->getEffectInstance()->getTimeInvariantMetadata()->getIsFrameVarying());
    EXPECT_EQ(5, pointCountAt(1.));
    EXPECT_EQ(0, pointCountAt(2.));
}

TEST_F(GeoFileNodeTest, BeforeAndAfterTheRangeHoldLoopBounceOrAreEmptyOrAnError)
{
    for (int frame = 1; frame <= 3; ++frame) {
        geoFileWrite(makePoints(frame), path(("mesh.000" + std::to_string(frame) + ".ngeo").c_str()));
    }
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.####.ngeo"));

    EXPECT_EQ(1, pointCountAt(-4.));
    EXPECT_EQ(3, pointCountAt(9.));

    setChoice("afterLastFrame", (int)ReadTimeDomain::eBeforeAfterLoop);
    EXPECT_EQ(1, pointCountAt(4.));
    EXPECT_EQ(2, pointCountAt(5.));

    setChoice("afterLastFrame", (int)ReadTimeDomain::eBeforeAfterBounce);
    EXPECT_EQ(2, pointCountAt(4.));
    EXPECT_EQ(1, pointCountAt(5.));

    setChoice("beforeFirstFrame", (int)ReadTimeDomain::eBeforeAfterBlack);
    EXPECT_EQ(0, pointCountAt(0.));

    setChoice("beforeFirstFrame", (int)ReadTimeDomain::eBeforeAfterError);
    std::string error;
    EXPECT_EQ(-1, pointCountAt(0., &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(GeoFileNodeTest, TheFrameOffsetShiftsTheSequence)
{
    geoFileWrite(makePoints(3), path("mesh.0001.ngeo"));
    geoFileWrite(makePoints(4), path("mesh.0002.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.####.ngeo"));

    editInt("startAt", 10);
    EXPECT_EQ(9, intValue("frameOffset"));
    EXPECT_EQ(3, pointCountAt(10.));
    EXPECT_EQ(4, pointCountAt(11.));
    double first = 0.;
    double last = 0.;
    frameRange(&first, &last);
    EXPECT_EQ(10., first);
    EXPECT_EQ(11., last);

    setChoice("frameOffsetMode", (int)ReadTimeDomain::eFrameModeTimeOffset);
    editInt("frameOffset", -1);
    EXPECT_EQ(3, pointCountAt(0.));
    EXPECT_EQ(4, pointCountAt(1.));
}

TEST_F(GeoFileNodeTest, AMissingFrameHoldsThePreviousOrLoadsTheNext)
{
    geoFileWrite(makePoints(3), path("mesh.0001.ngeo"));
    geoFileWrite(makePoints(6), path("mesh.0004.ngeo"));
    const KnobFilePtr file = fileKnob();
    ASSERT_TRUE(file != NULL);
    file->setValue(path("mesh.####.ngeo"));

    setMissingFrames(ReadTimeDomain::eMissingPrevious);
    EXPECT_EQ(3, pointCountAt(3.));
    setMissingFrames(ReadTimeDomain::eMissingNext);
    EXPECT_EQ(6, pointCountAt(2.));
}
