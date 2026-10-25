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

#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QTemporaryDir>

#include <OpenColorIO/OpenColorIO.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "OcioNodeTestSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/OCIO/OCIOCDLTransform.h"
#include "Engine/Nodes/OCIO/OCIOFileTransform.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace OcioNodeTest;

namespace {

const double kSlope[3] = { 1.2, 0.9, 1.1 };
const double kOffset[3] = { 0.01, -0.02, 0.03 };
const double kPower[3] = { 1.1, 0.95, 1.0 };
const double kSaturation = 0.8;

OCIO_NAMESPACE::ConstTransformRcPtr
cdlTransform(const double slope[3],
             const double offset[3],
             const double power[3],
             double saturation)
{
    OCIO_NAMESPACE::CDLTransformRcPtr transform = OCIO_NAMESPACE::CDLTransform::Create();
    const double sop[9] = { slope[0], slope[1], slope[2], offset[0], offset[1], offset[2], power[0], power[1], power[2] };

    transform->setSOP(sop);
    transform->setSat(saturation);

    return transform;
}

OCIO_NAMESPACE::ConstTransformRcPtr
fileTransform(const std::string& src,
              const std::string& cccid = std::string())
{
    OCIO_NAMESPACE::FileTransformRcPtr transform = OCIO_NAMESPACE::FileTransform::Create();

    transform->setSrc(src.c_str());
    transform->setCCCId(cccid.c_str());
    transform->setInterpolation(OCIO_NAMESPACE::INTERP_LINEAR);

    return transform;
}

/**
 * @brief Writes a 1D .cube LUT that scales [0, 1] by \p scale.
 **/
bool
writeScaleLut(const std::string& path,
              double scale)
{
    std::ofstream out(path.c_str());

    out << "LUT_1D_SIZE 2\n0 0 0\n"
        << scale << " " << scale << " " << scale << "\n";
    out.close();

    return bool(out);
}

/**
 * @brief Writes a .ccc holding "a", an identity, then "b", the test CDL.
 **/
bool
writeCollection(const std::string& path)
{
    std::ofstream out(path.c_str());

    out << "<ColorCorrectionCollection xmlns=\"urn:ASC:CDL:v1.01\">\n"
        << "  <ColorCorrection id=\"a\">\n"
        << "    <SOPNode><Slope>1 1 1</Slope><Offset>0 0 0</Offset><Power>1 1 1</Power></SOPNode>\n"
        << "    <SatNode><Saturation>1</Saturation></SatNode>\n"
        << "  </ColorCorrection>\n"
        << "  <ColorCorrection id=\"b\">\n"
        << "    <SOPNode><Slope>" << kSlope[0] << " " << kSlope[1] << " " << kSlope[2] << "</Slope>"
        << "<Offset>" << kOffset[0] << " " << kOffset[1] << " " << kOffset[2] << "</Offset>"
        << "<Power>" << kPower[0] << " " << kPower[1] << " " << kPower[2] << "</Power></SOPNode>\n"
        << "    <SatNode><Saturation>" << kSaturation << "</Saturation></SatNode>\n"
        << "  </ColorCorrection>\n"
        << "</ColorCorrectionCollection>\n";
    out.close();

    return bool(out);
}

void
setCDLKnobs(const NodePtr& node)
{
    ASSERT_TRUE(setKnobValues(node, kOCIOCDLTransformParamSlope, std::vector<double>(kSlope, kSlope + 3)));
    ASSERT_TRUE(setKnobValues(node, kOCIOCDLTransformParamOffset, std::vector<double>(kOffset, kOffset + 3)));
    ASSERT_TRUE(setKnobValues(node, kOCIOCDLTransformParamPower, std::vector<double>(kPower, kPower + 3)));
    ASSERT_TRUE(setKnobValues(node, kOCIOCDLTransformParamSaturation, std::vector<double>(1, kSaturation)));
}

bool
pressButton(const NodePtr& node,
            const std::string& name)
{
    KnobButton* button = node ? dynamic_cast<KnobButton*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(button != NULL) << name;
    if (!button) {
        return false;
    }
    button->trigger();

    return true;
}

} // namespace

class NativeOCIOFileTransformsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        setConfig(getApp(), kStudioURI);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        // Files the tests wrote are gone with their directory; nothing of them may stay cached.
        OCIO_NAMESPACE::ClearAllCaches();
        BaseTest::TearDown();
    }

    ParityPair makePair(const char* id,
                        int major)
    {
        ParityPair pair = makeParityPair(getApp(), id, -1, major);

        EXPECT_TRUE(bool(pair.native));

        return pair;
    }

    OCIO_NAMESPACE::ConstCPUProcessorRcPtr reference(const OCIO_NAMESPACE::ConstTransformRcPtr& transform,
                                                     OCIO_NAMESPACE::TransformDirection direction = OCIO_NAMESPACE::TRANSFORM_DIR_FORWARD)
    {
        return referenceProcessor(config(getApp()), transform, direction);
    }
};

TEST_F(NativeOCIOFileTransformsTest, CDLMatchesCDLTransform)
{
    ParityPair pair = makePair(PLUGINID_NATRON_OCIOCDLTRANSFORM, PLUGIN_MAJOR_NATRON_OCIOCDLTRANSFORM);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(dynamic_cast<OCIOCDLTransform*>(pair.native->getEffectInstance().get()));

    // The default values are an identity.
    expectProcessed(pair.native, pair.source, OCIO_NAMESPACE::ConstCPUProcessorRcPtr(), "default");

    setCDLKnobs(pair.native);
    const OCIO_NAMESPACE::ConstTransformRcPtr cdl = cdlTransform(kSlope, kOffset, kPower, kSaturation);
    expectProcessed(pair.native, pair.source, reference(cdl), "forward");

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOCDLTransformParamDirection, "inverse"));
    expectProcessed(pair.native, pair.source, reference(cdl, OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE), "inverse");
}

TEST_F(NativeOCIOFileTransformsTest, CDLReadsACollectionFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = tmp.path().toStdString() + "/grades.ccc";
    ASSERT_TRUE(writeCollection(path));

    ParityPair pair = makePair(PLUGINID_NATRON_OCIOCDLTRANSFORM, PLUGIN_MAJOR_NATRON_OCIOCDLTRANSFORM);
    ASSERT_TRUE(bool(pair.native));

    ASSERT_TRUE(setKnobValues(pair.native, kOCIOCDLTransformParamReadFromFile, std::vector<double>(1, 1.)));
    ASSERT_TRUE(setKnobValue(pair.native, kOcioFileParamFile, path));

    // The menu lists the ids of the file.
    KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOcioFileParamCCCIdMenu));
    ASSERT_TRUE(bool(menu));
    const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
    ASSERT_EQ(2u, entries.size());
    EXPECT_EQ(std::string("a"), entries[0].id);
    EXPECT_EQ(std::string("b"), entries[1].id);

    ASSERT_TRUE(setKnobValue(pair.native, kOcioFileParamCCCId, "b"));
    expectProcessed(pair.native, pair.source, reference(fileTransform(path, "b")), "file b");

    // The knobs show the file's values.
    KnobColorPtr slope = std::dynamic_pointer_cast<KnobColor>(pair.native->getKnobByName(kOCIOCDLTransformParamSlope));
    KnobDoublePtr saturation = std::dynamic_pointer_cast<KnobDouble>(pair.native->getKnobByName(kOCIOCDLTransformParamSaturation));
    ASSERT_TRUE(bool(slope));
    ASSERT_TRUE(bool(saturation));
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(kSlope[i], slope->getValue(i), 1e-6) << i;
    }
    EXPECT_NEAR(kSaturation, saturation->getValue(), 1e-6);

    ASSERT_TRUE(setKnobValue(pair.native, kOcioFileParamCCCId, "a"));
    expectProcessed(pair.native, pair.source, OCIO_NAMESPACE::ConstCPUProcessorRcPtr(), "file a");
}

TEST_F(NativeOCIOFileTransformsTest, CDLExportsACCFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = tmp.path().toStdString() + "/grade.cc";

    ParityPair pair = makePair(PLUGINID_NATRON_OCIOCDLTRANSFORM, PLUGIN_MAJOR_NATRON_OCIOCDLTRANSFORM);
    ASSERT_TRUE(bool(pair.native));
    setCDLKnobs(pair.native);

    KnobOutputFilePtr exportKnob = std::dynamic_pointer_cast<KnobOutputFile>(pair.native->getKnobByName(kOCIOCDLTransformParamExport));
    ASSERT_TRUE(bool(exportKnob));
    // Only a path the user enters exports.
    exportKnob->setValue(path, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    ASSERT_TRUE(QFileInfo(QString::fromStdString(path)).exists());
    EXPECT_EQ(std::string(), exportKnob->getValue());

    // The file holds the grade: a FileTransform of it renders as the knobs do.
    ParityPair filePair = makePair(PLUGINID_NATRON_OCIOFILETRANSFORM, PLUGIN_MAJOR_NATRON_OCIOFILETRANSFORM);
    ASSERT_TRUE(bool(filePair.native));
    ASSERT_TRUE(setKnobValue(filePair.native, kOcioFileParamFile, path));
    expectProcessed(filePair.native, filePair.source, reference(cdlTransform(kSlope, kOffset, kPower, kSaturation)), "exported");
}

TEST_F(NativeOCIOFileTransformsTest, FileMatchesFileTransform)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = tmp.path().toStdString() + "/half.cube";
    ASSERT_TRUE(writeScaleLut(path, 0.5));

    ParityPair pair = makePair(PLUGINID_NATRON_OCIOFILETRANSFORM, PLUGIN_MAJOR_NATRON_OCIOFILETRANSFORM);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(dynamic_cast<OCIOFileTransform*>(pair.native->getEffectInstance().get()));

    // No file is an identity.
    expectProcessed(pair.native, pair.source, OCIO_NAMESPACE::ConstCPUProcessorRcPtr(), "no file");

    ASSERT_TRUE(setKnobValue(pair.native, kOcioFileParamFile, path));
    EXPECT_TRUE(persistentMessage(pair.native).isEmpty()) << persistentMessage(pair.native).toStdString();
    expectProcessed(pair.native, pair.source, reference(fileTransform(path)), "forward");

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOFileTransformParamDirection, "inverse"));
    expectProcessed(pair.native, pair.source, reference(fileTransform(path), OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE), "inverse");
}

TEST_F(NativeOCIOFileTransformsTest, MissingFileIsAnErrorAndPassesThrough)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = tmp.path().toStdString() + "/missing.cube";

    ParityPair pair = makePair(PLUGINID_NATRON_OCIOFILETRANSFORM, PLUGIN_MAJOR_NATRON_OCIOFILETRANSFORM);
    ASSERT_TRUE(bool(pair.native));

    ASSERT_TRUE(setKnobValue(pair.native, kOcioFileParamFile, path));
    EXPECT_FALSE(persistentMessage(pair.native).isEmpty());
    expectProcessed(pair.native, pair.source, OCIO_NAMESPACE::ConstCPUProcessorRcPtr(), "missing");

    // The error goes once the file can be read.
    ASSERT_TRUE(writeScaleLut(path, 0.5));
    ASSERT_TRUE(pressButton(pair.native, kOcioFileParamReload));
    EXPECT_TRUE(persistentMessage(pair.native).isEmpty()) << persistentMessage(pair.native).toStdString();
    expectProcessed(pair.native, pair.source, reference(fileTransform(path)), "written");
}

TEST_F(NativeOCIOFileTransformsTest, ReloadReadsAChangedFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = tmp.path().toStdString() + "/scale.cube";
    ASSERT_TRUE(writeScaleLut(path, 0.5));

    ParityPair pair = makePair(PLUGINID_NATRON_OCIOFILETRANSFORM, PLUGIN_MAJOR_NATRON_OCIOFILETRANSFORM);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, kOcioFileParamFile, path));
    expectProcessed(pair.native, pair.source, reference(fileTransform(path)), "before");

    ASSERT_TRUE(writeScaleLut(path, 0.25));
    ASSERT_TRUE(pressButton(pair.native, kOcioFileParamReload));

    // The reference is built after Reload cleared OpenColorIO's file cache, so it reads the new
    // file; it must differ from the old one for the test to mean anything.
    const OCIO_NAMESPACE::ConstCPUProcessorRcPtr after = reference(fileTransform(path));
    float probe[3] = { 1.f, 1.f, 1.f };
    after->applyRGB(probe);
    EXPECT_NEAR(0.25, probe[0], 1e-6);
    expectProcessed(pair.native, pair.source, after, "after");
}
