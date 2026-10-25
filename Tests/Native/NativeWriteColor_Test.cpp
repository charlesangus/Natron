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
#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenColorIO/OpenColorIO.h>
#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kSRGBSpace = "sRGB Encoded Rec.709 (sRGB)";
const char* const kGamma22Space = "Gamma 2.2 Encoded Rec.709";
const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";
const char* const kRuleSpace = "ACES2065-1";

const char* const kOutputSpaceKnob = "ocioOutputSpace";
const char* const kOutputSpaceMenuKnob = "ocioOutputSpaceIndex";
const char* const kOutputContextKnob = "Context";
const char* const kOutputSpaceSetKnob = "ocioOutputSpaceSet";
const char* const kWorkingSpaceKnob = "ocioWorkingSpace";

const int kWidth = 16;
const int kHeight = 8;
const float kGray = 0.18f;

struct Decoded {
    OIIO::ImageSpec spec;
    std::vector<float> pixels;
};

bool
decode(const std::string& path,
       Decoded* decoded)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    if (!input) {
        return false;
    }
    decoded->spec = input->spec();
    decoded->pixels.assign((std::size_t)decoded->spec.width * decoded->spec.height * decoded->spec.nchannels, 0.f);
    const bool ok = input->read_image(0, 0, 0, decoded->spec.nchannels, OIIO::TypeDesc::FLOAT, decoded->pixels.data());
    input->close();

    return ok;
}

bool
nearlyEqual(float got,
            float want,
            float tolerance)
{
    return std::fabs(got - want) <= tolerance;
}
} // namespace

class NativeWriteColorTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        project()->reset(false, true);
        project()->setAutoSetProjectFormatEnabled(true);
        project()->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeWriteColor", 1.));

        _checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        ASSERT_TRUE(bool(_checker));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamBoxSize, { 4096., 2112. }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamLineWidth, { 0. }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamCenterLineWidth, { 0. }));
        for (const char* name : { kCheckerBoardParamColor0, kCheckerBoardParamColor1, kCheckerBoardParamColor2, kCheckerBoardParamColor3 }) {
            ASSERT_TRUE(setKnobValues(_checker, name, { (double)kGray, (double)kGray, (double)kGray, 1. }));
        }
        _writer = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(bool(_writer));
        connectNodes(_checker, _writer, 0, true);
        ASSERT_TRUE(_dir.isValid());
    }

    virtual void TearDown() OVERRIDE
    {
        _writer.reset();
        _checker.reset();
        project()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    OCIO_NAMESPACE::ConstConfigRcPtr config() const
    {
        return project()->getColorManagement()->getConfig();
    }

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    void setFile(const NodePtr& node,
                 const std::string& path)
    {
        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());

        ASSERT_TRUE(file != NULL);
        file->setValue(path);
    }

    void setBitDepth(const NodePtr& node,
                     const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(node->getKnobByName(kNatronWriteParamBitDepth).get());

        ASSERT_TRUE(choice != NULL);
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return;
            }
        }
        ADD_FAILURE() << "no bit depth " << id;
    }

    void setProjectSpace(const char* knobName,
                         const char* space)
    {
        KnobChoicePtr knob = project()->getKnobByNameAndType<KnobChoice>(knobName);

        ASSERT_TRUE(bool(knob)) << knobName;
        knob->setValueFromID(space, 0);
        ASSERT_EQ(std::string(space), knob->getActiveEntry().id) << knobName;
    }

    std::string stringValue(const NodePtr& node,
                            const char* name) const
    {
        KnobStringBasePtr knob = std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(name));

        EXPECT_TRUE(bool(knob)) << name;

        return knob ? knob->getValue() : std::string();
    }

    bool userSet(const NodePtr& node) const
    {
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kOutputSpaceSetKnob));

        EXPECT_TRUE(bool(knob));

        return knob && knob->getValue();
    }

    std::string menuValue(const NodePtr& node) const
    {
        KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kOutputSpaceMenuKnob));

        EXPECT_TRUE(bool(menu));

        return menu ? menu->getEntry(menu->getValue()).id : std::string();
    }

    void userPicksOutputSpace(const NodePtr& node,
                              const char* space)
    {
        KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kOutputSpaceMenuKnob));

        ASSERT_TRUE(bool(menu));
        const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
        int index = -1;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == space) {
                index = (int)i;
                break;
            }
        }
        ASSERT_GE(index, 0) << space;
        // The Gui's menu sends no change for the entry already shown, only a reselection.
        if (index == menu->getValue()) {
            ASSERT_TRUE(menu->getNotifiesOnReselect());
            menu->setValue(index, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0, true);
        } else {
            menu->setValue(index, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
        }
        ASSERT_EQ(std::string(space), stringValue(node, kOutputSpaceKnob));
    }

    // The same calls, in the same order, as the Gui's "Reset to default" on one knob.
    void restoreDefaultAsTheGuiDoes(const KnobIPtr& knob)
    {
        ASSERT_TRUE(bool(knob));
        KnobHolder* holder = knob->getHolder();
        ASSERT_TRUE(holder != NULL);

        holder->beginChanges();
        holder->beginChanges();
        for (int d = 0; d < knob->getDimension(); ++d) {
            knob->resetToDefaultValue(d);
        }
        holder->endChanges(true);
        holder->onKnobValueChanged_public(knob.get(), eValueChangedReasonRestoreDefault, 0., ViewIdx(0), true);
        holder->endChanges();
        holder->incrHashAndEvaluate(true, true);
    }

    void render(const NodePtr& writer)
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
    }

    // The first colour sample of the written file, as an 8-bit code when the file is 8-bit.
    float firstSample(const std::string& path)
    {
        Decoded decoded;

        EXPECT_TRUE(decode(path, &decoded)) << path;
        EXPECT_GE(decoded.spec.nchannels, 3);

        return decoded.pixels.empty() ? -1.f : decoded.pixels[0];
    }

    // What OCIO itself makes of a mid-gray in the working space, in the colourspace `to`.
    float converted(const std::string& to) const
    {
        float rgb[3] = { kGray, kGray, kGray };
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu = config()->getProcessor(project()->getWorkingColorSpace().c_str(), to.c_str())->getDefaultCPUProcessor();
        OCIO_NAMESPACE::PackedImageDesc desc(rgb, 1, 1, 3);

        cpu->apply(desc);

        return rgb[0];
    }

    void saveResetAndLoad(const char* fileName)
    {
        const QString dirPath = _dir.path() + QLatin1Char('/');
        QString saved;

        ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8(fileName), &saved));
        project()->reset(false, true);
        ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8(fileName)));
    }

    NodePtr _checker;
    NodePtr _writer;
    QTemporaryDir _dir;
};

TEST_F(NativeWriteColorTest, AnEightBitPngTakesTheProjectEightBitDefault)
{
    setProjectSpace("colorSpace8Bit", kSRGBSpace);
    setFile(_writer, pathOf("out.png"));

    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategory8Bit), stringValue(_writer, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(_writer, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), menuValue(_writer));
    EXPECT_EQ(project()->getWorkingColorSpace(), stringValue(_writer, kWorkingSpaceKnob));
    EXPECT_FALSE(userSet(_writer));
}

TEST_F(NativeWriteColorTest, AnExrTakesTheColourspaceOfTheConfigsFileRule)
{
    ASSERT_EQ(std::string(kRuleSpace), std::string(config()->getColorSpaceFromFilepath("/shots/plate.exr")));
    ASSERT_NE(project()->getFileColorSpace(eFileColorCategoryFloat), std::string(kRuleSpace));

    setFile(_writer, pathOf("out.exr"));

    EXPECT_EQ(std::string(kRuleSpace), stringValue(_writer, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kRuleSpace), menuValue(_writer));
    EXPECT_FALSE(userSet(_writer));
}

TEST_F(NativeWriteColorTest, AFileWithoutARuleTakesTheDefaultOfItsDataType)
{
    setProjectSpace("colorSpace8Bit", kSRGBSpace);
    setProjectSpace("colorSpace16Bit", kGamma22Space);
    setProjectSpace("colorSpaceFloat", kLinearRec709Space);
    ASSERT_TRUE(IoColorSpace::fileRuleColorSpace(config(), "/shots/plate.tif").empty());

    setFile(_writer, pathOf("out.tif"));
    EXPECT_EQ(std::string(kGamma22Space), stringValue(_writer, kOutputSpaceKnob)) << "tif is 16-bit";

    setBitDepth(_writer, "8i");
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(_writer, kOutputSpaceKnob));

    setBitDepth(_writer, "32f");
    EXPECT_EQ(std::string(kLinearRec709Space), stringValue(_writer, kOutputSpaceKnob));
    EXPECT_FALSE(userSet(_writer));
}

TEST_F(NativeWriteColorTest, ATenBitDpxTakesTheLogDefault)
{
    setProjectSpace("colorSpace16Bit", kGamma22Space);
    setProjectSpace("colorSpaceLog", kLinearRec709Space);
    ASSERT_TRUE(IoColorSpace::fileRuleColorSpace(config(), "/shots/plate.dpx").empty());

    setFile(_writer, pathOf("out.dpx"));
    setBitDepth(_writer, "10i");

    EXPECT_EQ(std::string(kLinearRec709Space), stringValue(_writer, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kLinearRec709Space), menuValue(_writer));
}

TEST_F(NativeWriteColorTest, MidGrayWrittenToAnEightBitPngEncodesToCode118)
{
    setProjectSpace("colorSpace8Bit", kSRGBSpace);
    const std::string path = pathOf("gray.png");
    setFile(_writer, path);
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(_writer, kOutputSpaceKnob));

    render(_writer);

    const long code = std::lround(firstSample(path) * 255.f);
    EXPECT_EQ(std::lround(converted(kSRGBSpace) * 255.f), code);
    EXPECT_NEAR(118, code, 1);
}

TEST_F(NativeWriteColorTest, AnOutputSpaceEqualToTheWorkingSpaceConvertsNothing)
{
    const std::string path = pathOf("same.exr");
    setFile(_writer, path);
    setBitDepth(_writer, "32f");
    userPicksOutputSpace(_writer, project()->getWorkingColorSpace().c_str());

    render(_writer);

    Decoded decoded;
    ASSERT_TRUE(decode(path, &decoded));
    ASSERT_GE(decoded.spec.nchannels, 4);
    for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(nearlyEqual(decoded.pixels[(std::size_t)c], kGray, 1e-6f)) << c;
    }
    EXPECT_TRUE(nearlyEqual(decoded.pixels[3], 1.f, 1e-6f)) << "alpha";
}

TEST_F(NativeWriteColorTest, TheAlphaIsLeftUntouched)
{
    const std::string path = pathOf("alpha.exr");
    setFile(_writer, path);
    setBitDepth(_writer, "32f");
    userPicksOutputSpace(_writer, kSRGBSpace);

    render(_writer);

    Decoded decoded;
    ASSERT_TRUE(decode(path, &decoded));
    ASSERT_GE(decoded.spec.nchannels, 4);
    EXPECT_TRUE(nearlyEqual(decoded.pixels[0], converted(kSRGBSpace), 1e-5f));
    EXPECT_TRUE(nearlyEqual(decoded.pixels[3], 1.f, 1e-6f));
}

TEST_F(NativeWriteColorTest, TheFileCarriesItsColourspace)
{
    const std::string path = pathOf("tagged.exr");
    setFile(_writer, path);
    setBitDepth(_writer, "32f");
    ASSERT_EQ(std::string(kRuleSpace), stringValue(_writer, kOutputSpaceKnob));

    render(_writer);

    Decoded decoded;
    ASSERT_TRUE(decode(path, &decoded));
    // OpenEXR files carry the Color Interop ID of the space rather than its config name.
    OCIO_NAMESPACE::ConstColorSpaceRcPtr space = config()->getColorSpace(kRuleSpace);
    ASSERT_TRUE(bool(space));
    const std::string interopID = space->getInteropID() ? std::string(space->getInteropID()) : std::string();
    const std::string tag = decoded.spec.get_string_attribute("oiio:ColorSpace");
    EXPECT_TRUE(tag == kRuleSpace || (!interopID.empty() && tag == interopID)) << tag;
    EXPECT_EQ(std::string(kRuleSpace), IoColorSpace::embeddedColorSpace(config(), decoded.spec));
    EXPECT_TRUE(nearlyEqual(decoded.pixels[0], converted(kRuleSpace), 1e-5f));
}

TEST_F(NativeWriteColorTest, AnExistingWriteKeepsItsSpaceAcrossADefaultChangeAndReload)
{
    setProjectSpace("colorSpace8Bit", kSRGBSpace);
    setFile(_writer, pathOf("keep.png"));
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(_writer, kOutputSpaceKnob));
    const std::string name = _writer->getScriptName_mt_safe();

    setProjectSpace("colorSpace8Bit", kGamma22Space);
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(_writer, kOutputSpaceKnob));

    saveResetAndLoad("keep.ntpz");

    ASSERT_EQ(std::string(kGamma22Space), project()->getFileColorSpace(eFileColorCategory8Bit));
    NodePtr loaded = project()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(loaded, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), menuValue(loaded));
    EXPECT_EQ(project()->getWorkingColorSpace(), stringValue(loaded, kWorkingSpaceKnob));
}

TEST_F(NativeWriteColorTest, AUserPickSticksAcrossAFilenameChange)
{
    setProjectSpace("colorSpace8Bit", kSRGBSpace);
    setFile(_writer, pathOf("pick.png"));
    userPicksOutputSpace(_writer, kLinearRec709Space);
    ASSERT_TRUE(userSet(_writer));

    setFile(_writer, pathOf("other.jpg"));
    setBitDepth(_writer, "8i");

    EXPECT_EQ(std::string(kLinearRec709Space), stringValue(_writer, kOutputSpaceKnob));
    EXPECT_TRUE(userSet(_writer));
}

TEST_F(NativeWriteColorTest, ResettingTheOutputColourspaceReturnsToTheAutomaticSpaceAndChangesThePixels)
{
    setProjectSpace("colorSpace8Bit", kSRGBSpace);
    const std::string path = pathOf("reset.png");
    setFile(_writer, path);
    userPicksOutputSpace(_writer, kLinearRec709Space);
    ASSERT_TRUE(userSet(_writer));
    render(_writer);
    const long picked = std::lround(firstSample(path) * 255.f);
    EXPECT_EQ(std::lround(converted(kLinearRec709Space) * 255.f), picked);

    restoreDefaultAsTheGuiDoes(_writer->getKnobByName(kOutputSpaceMenuKnob));

    EXPECT_FALSE(userSet(_writer));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(_writer, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), menuValue(_writer));
    render(_writer);
    const long reset = std::lround(firstSample(path) * 255.f);
    EXPECT_NE(picked, reset);
    EXPECT_EQ(std::lround(converted(kSRGBSpace) * 255.f), reset);
}

TEST_F(NativeWriteColorTest, ReselectingTheShownEntryRecordsAUserChoice)
{
    setProjectSpace("colorSpace8Bit", kSRGBSpace);
    setFile(_writer, pathOf("reselect.png"));
    ASSERT_EQ(std::string(kSRGBSpace), menuValue(_writer));
    ASSERT_FALSE(userSet(_writer));

    userPicksOutputSpace(_writer, kSRGBSpace);

    EXPECT_TRUE(userSet(_writer));
}

TEST_F(NativeWriteColorTest, TheOutputColourspaceKnobsSitBeforeTheFormatOptions)
{
    const std::vector<KnobIPtr> knobs = _writer->getEffectInstance()->getKnobs_mt_safe();
    int frameIncrement = -1;
    int menu = -1;
    int context = -1;
    int bitDepth = -1;

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        const std::string name = knobs[i]->getName();
        if (name == kNatronWriteParamFrameStep) {
            frameIncrement = (int)i;
        } else if (name == kOutputSpaceMenuKnob) {
            menu = (int)i;
        } else if (name == kOutputContextKnob) {
            context = (int)i;
        } else if (name == kNatronWriteParamBitDepth) {
            bitDepth = (int)i;
        }
    }
    ASSERT_GE(frameIncrement, 0);
    ASSERT_GE(menu, 0);
    ASSERT_GE(context, 0);
    ASSERT_GE(bitDepth, 0);
    EXPECT_GT(menu, frameIncrement);
    EXPECT_GT(context, menu);
    EXPECT_LT(context, bitDepth);
}

// A config whose "shot" colourspace applies the LUT file named by the $LUT context variable.
class NativeWriteContextTest
    : public NativeWriteColorTest {
protected:
    void useContextConfig()
    {
        const std::string root = _dir.path().toStdString();
        const char* const luts[][2] = { { "half", "0.5" }, { "quarter", "0.25" } };
        for (std::size_t i = 0; i < 2; ++i) {
            std::ofstream lut((root + "/" + luts[i][0] + ".cube").c_str());
            lut << "LUT_1D_SIZE 2\n0 0 0\n"
                << luts[i][1] << " " << luts[i][1] << " " << luts[i][1] << "\n";
            ASSERT_TRUE(lut.good());
        }
        const std::string path = root + "/config.ocio";
        {
            std::ofstream out(path.c_str());
            out << "ocio_profile_version: 2\n"
                   "search_path: .\n"
                   "environment:\n"
                   "  LUT: half\n"
                   "roles:\n"
                   "  default: raw\n"
                   "  scene_linear: lin\n"
                   "file_rules:\n"
                   "  - !<Rule> {name: Default, colorspace: default}\n"
                   "displays:\n"
                   "  sRGB:\n"
                   "    - !<View> {name: Raw, colorspace: raw}\n"
                   "active_displays: []\n"
                   "active_views: []\n"
                   "colorspaces:\n"
                   "  - !<ColorSpace>\n"
                   "    name: raw\n"
                   "    isdata: true\n"
                   "  - !<ColorSpace>\n"
                   "    name: lin\n"
                   "  - !<ColorSpace>\n"
                   "    name: shot\n"
                   "    to_scene_reference: !<FileTransform> {src: $LUT.cube, interpolation: linear}\n";
            ASSERT_TRUE(out.good());
        }
        KnobChoicePtr configChoice = project()->getKnobByNameAndType<KnobChoice>("ocioConfig");
        KnobStringBasePtr configFile = project()->getKnobByNameAndType<KnobStringBase>("ocioConfigFile");
        ASSERT_TRUE(bool(configChoice));
        ASSERT_TRUE(bool(configFile));
        configChoice->setValueFromID("Custom config", 0);
        configFile->setValue(path);
        ASSERT_EQ(path, project()->getColorManagement()->getConfigSource());
        setProjectSpace("workingSpace", "lin");
    }

    void setContext(const NodePtr& node,
                    int pair,
                    const char* key,
                    const char* value)
    {
        const std::string n = std::to_string(pair);
        KnobStringPtr keyKnob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName("key" + n));
        KnobStringPtr valueKnob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName("value" + n));

        ASSERT_TRUE(bool(keyKnob));
        ASSERT_TRUE(bool(valueKnob));
        keyKnob->setValue(std::string(key));
        valueKnob->setValue(std::string(value));
    }

    // Writing into "shot" inverts its LUT, which scales by the reciprocal of the LUT's slope.
    void expectWritten(const std::string& path,
                       float scale)
    {
        render(_writer);

        Decoded decoded;
        ASSERT_TRUE(decode(path, &decoded));
        ASSERT_GE(decoded.spec.nchannels, 3);
        EXPECT_TRUE(nearlyEqual(decoded.pixels[0], kGray * scale, 1e-5f)) << decoded.pixels[0] << " vs " << kGray * scale;
    }
};

TEST_F(NativeWriteContextTest, AContextVariableChangesTheConversionAndTheHash)
{
    useContextConfig();
    const std::string path = pathOf("shot.exr");
    setFile(_writer, path);
    setBitDepth(_writer, "32f");
    userPicksOutputSpace(_writer, "shot");

    expectWritten(path, 2.f);
    const U64 hashBefore = _writer->getHashValue();

    setContext(_writer, 2, "LUT", "quarter");
    EXPECT_NE(hashBefore, _writer->getHashValue());
    expectWritten(path, 4.f);

    setContext(_writer, 2, "", "quarter");
    expectWritten(path, 2.f);
}
