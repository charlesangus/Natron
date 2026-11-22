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

#include <cstdio>
#include <string>

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

    void setMissingFrames(GeoFileMissingFrameEnum mode)
    {
        const KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(_reader->getKnobByName(kGeoFileParamMissingFrames));
        ASSERT_TRUE(knob != NULL);
        knob->setValue((int)mode);
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

    setMissingFrames(eGeoFileMissingFrameHoldNearest);
    EXPECT_EQ(3, pointCountAt(2.));
    EXPECT_EQ(6, pointCountAt(3.));
    EXPECT_EQ(6, pointCountAt(10.));
    EXPECT_EQ(3, pointCountAt(-5.));

    setMissingFrames(eGeoFileMissingFrameEmpty);
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
    setMissingFrames(eGeoFileMissingFrameEmpty);

    std::string error;
    EXPECT_EQ(-1, pointCountAt(1., &error));
    EXPECT_NE(std::string::npos, error.find("bad.0001.ngeo")) << error;
}
