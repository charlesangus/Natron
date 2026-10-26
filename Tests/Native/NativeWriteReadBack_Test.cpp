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

#include <bitset>
#include <cmath>
#include <cstddef>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QMetaObject>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kSRGBSpace = "sRGB Encoded Rec.709 (sRGB)";
const char* const kOutputSpaceMenuKnob = "ocioOutputSpaceIndex";

const int kWidth = 16;
const int kHeight = 8;
const int kFileWidth = 32;
const int kFileHeight = 12;
const double kFilePar = 2.;
const float kGray = 0.18f;
const float kOther = 0.5f;
} // namespace

class NativeWriteReadBackTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        project()->reset(false, true);
        project()->setAutoSetProjectFormatEnabled(true);
        project()->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeWriteReadBack", 1.));

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

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    void setFile(const std::string& path)
    {
        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(_writer->getKnobByName(kOfxImageEffectFileParamName).get());

        ASSERT_TRUE(file != NULL);
        file->setValue(path);
    }

    void setBitDepth(const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kNatronWriteParamBitDepth).get());

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

    void setChoice(const char* name,
                   const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(_writer->getKnobByName(name).get());

        ASSERT_TRUE(choice != NULL);
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return;
            }
        }
        ADD_FAILURE() << "no entry " << id << " in " << name;
    }

    // Makes the written file's format differ from the project's and the input's.
    void writeFixedFormatFile(const std::string& path)
    {
        setFile(path);
        setBitDepth("32f");
        pickOutputSpace(project()->getWorkingColorSpace());
        setChoice(kNatronWriteParamFormatType, kNatronWriteFormatTypeFixed);
        ASSERT_TRUE(setKnobValues(_writer, kNatronParamFormatSize, { (double)kFileWidth, (double)kFileHeight }));
        ASSERT_TRUE(setKnobValues(_writer, kNatronParamFormatPar, { kFilePar }));
        // Clipped to the input's RoD the file is written in the input's pixel aspect, not the format's.
        KnobBool* clipToRoD = dynamic_cast<KnobBool*>(_writer->getKnobByName(kNatronWriteParamClipToRoD).get());
        ASSERT_TRUE(clipToRoD != NULL);
        clipToRoD->setValue(false, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
        writeSequence();
    }

    RectI outputFormat() const
    {
        return _writer->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat();
    }

    double outputPar() const
    {
        return _writer->getEffectInstance()->getTimeInvariantMetadata()->getPixelAspectRatio(-1);
    }

    void pickOutputSpace(const std::string& space)
    {
        KnobChoice* menu = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kOutputSpaceMenuKnob).get());

        ASSERT_TRUE(menu != NULL);
        const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == space) {
                menu->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0, true);

                return;
            }
        }
        ADD_FAILURE() << "no colourspace " << space;
    }

    void setReadBack(bool on)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(_writer->getKnobByName(kNatronWriteParamReadBack).get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(on, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }

    void writeSequence()
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
        getApp()->startWritersRendering(false, works);
        ASSERT_FALSE(effect->isDoingSequentialRender());
    }

    void finishSequence()
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        RenderEnginePtr engine = effect->getRenderEngine();
        ASSERT_TRUE(bool(engine));
        ASSERT_TRUE(QMetaObject::invokeMethod(engine.get(), "renderFinished", Qt::DirectConnection, Q_ARG(int, 0)));
    }

    // Renders the Write's output at `time`, the way a viewer asks for it.
    bool outputAt(double time,
                  std::vector<float>* pixels)
    {
        std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> planes;
        std::string error;

        if (!renderNodePlanesDirect(_writer, time, ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), layers, &planes, &error) || planes.size() != 1) {
            return false;
        }
        *pixels = planes[0].pixels;

        return true;
    }

    bool outputIsGray(float value,
                      float tolerance)
    {
        std::vector<float> pixels;

        if (!outputAt(1., &pixels) || pixels.size() != (std::size_t)kWidth * kHeight * 4) {
            return false;
        }
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const float want = (i % 4 == 3) ? 1.f : value;
            if (std::fabs(pixels[i] - want) > tolerance) {
                return false;
            }
        }

        return true;
    }

    NativeWrite* writeEffect() const
    {
        return dynamic_cast<NativeWrite*>(_writer->getEffectInstance().get());
    }

    // The hash the Write's files must hold to be read back now.
    std::string currentHashText() const
    {
        return NativeWrite::outputHashText(writeEffect()->getOutputContentHash());
    }

    // Replaces the file with a flat float image of `value` in every colour channel and in each of
    // `extra`, 1 in alpha, stamped with the Write's current output content hash.
    void overwriteWithGray(const std::string& path,
                           float value,
                           const std::vector<std::string>& extra = std::vector<std::string>())
    {
        std::vector<std::string> names = { "R", "G", "B", "A" };
        names.insert(names.end(), extra.begin(), extra.end());
        const int n = (int)names.size();
        OIIO::ImageSpec spec(kWidth, kHeight, n, OIIO::TypeDesc::FLOAT);
        spec.channelnames = names;
        spec.alpha_channel = 3;
        spec.attribute(kNatronWriteOutputHashAttribute, currentHashText());
        std::vector<float> pixels((std::size_t)kWidth * kHeight * n, value);
        for (std::size_t i = 3; i < pixels.size(); i += (std::size_t)n) {
            pixels[i] = 1.f;
        }
        OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

        ASSERT_TRUE(bool(out));
        ASSERT_TRUE(out->open(path, spec));
        ASSERT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()));
        ASSERT_TRUE(out->close());
    }

    void setCheckerGray(float value)
    {
        for (const char* name : { kCheckerBoardParamColor0, kCheckerBoardParamColor1, kCheckerBoardParamColor2, kCheckerBoardParamColor3 }) {
            ASSERT_TRUE(setKnobValues(_checker, name, { (double)value, (double)value, (double)value, 1. }));
        }
    }

    void pressReload()
    {
        KnobButton* reload = dynamic_cast<KnobButton*>(_writer->getKnobByName(kNatronWriteParamReload).get());

        ASSERT_TRUE(reload != NULL);
        reload->trigger();
    }

    std::set<std::string> producedLayerIDs()
    {
        EffectInstance::ComponentsNeededMap comps;
        std::list<ImageLayerDesc> passThroughLayers;
        double passThroughTime = 0.;
        int passThroughView = 0;
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        int passThroughInputNb = -1;

        _writer->getEffectInstance()->getComponentsNeededAndProduced_public(_writer->getHashValue(), 1., ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);
        std::set<std::string> ids;
        const std::list<ImageLayerDesc>& produced = comps[-1];
        for (std::list<ImageLayerDesc>::const_iterator it = produced.begin(); it != produced.end(); ++it) {
            ids.insert(it->isColorLayer() ? std::string(kNatronColorLayerID) : it->getLayerID());
        }

        return ids;
    }

    std::string message() const
    {
        QString text;
        int type = 0;

        _writer->getPersistentMessage(&text, &type, false);

        return text.toStdString();
    }

    NodePtr _checker;
    NodePtr _writer;
    QTemporaryDir _dir;
};

TEST_F(NativeWriteReadBackTest, AnExrReadBackIsTheFilesPixelsInTheWorkingSpace)
{
    const std::string path = pathOf("back.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    overwriteWithGray(path, kOther);

    setReadBack(true);

    EXPECT_TRUE(outputIsGray(kOther, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, ReadBackIsOfferedOnlyForAnExr)
{
    KnobIPtr readBack = _writer->getKnobByName(kNatronWriteParamReadBack);
    KnobIPtr reload = _writer->getKnobByName(kNatronWriteParamReload);

    ASSERT_TRUE(bool(readBack));
    ASSERT_TRUE(bool(reload));
    setFile(pathOf("offer.png"));
    EXPECT_TRUE(readBack->getIsSecret());
    EXPECT_TRUE(reload->getIsSecret());

    setFile(pathOf("offer.exr"));
    EXPECT_FALSE(readBack->getIsSecret());
    EXPECT_FALSE(reload->getIsSecret());
}

TEST_F(NativeWriteReadBackTest, AnEncodedPngIsNeverReadBack)
{
    const std::string path = pathOf("back.png");
    setFile(path);
    pickOutputSpace(kSRGBSpace);
    writeSequence();
    setReadBack(true);

    EXPECT_TRUE(outputIsGray(kGray, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, AWrittenExrHoldsTheHashOfTheNode)
{
    const std::string path = pathOf("stored.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();

    OIIO::ImageInput::unique_ptr in = OIIO::ImageInput::open(path);
    ASSERT_TRUE(bool(in));
    EXPECT_EQ(currentHashText(), in->spec().get_string_attribute(kNatronWriteOutputHashAttribute));
}

TEST_F(NativeWriteReadBackTest, ReadBackShowsTheFileWhileItsHashMatchesAndTheInputWhenItDoesNot)
{
    const std::string path = pathOf("gate.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    overwriteWithGray(path, kOther);
    setReadBack(true);
    ASSERT_TRUE(outputIsGray(kOther, 1e-6f)) << message();

    setCheckerGray(0.3f);
    EXPECT_TRUE(outputIsGray(0.3f, 1e-6f)) << message();
    EXPECT_FALSE(_writer->hasPersistentMessage());

    setCheckerGray(kGray);
    EXPECT_TRUE(outputIsGray(kOther, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, AFileWithoutAHashIsNotShown)
{
    const std::string path = pathOf("unstamped.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    {
        OIIO::ImageSpec spec(kWidth, kHeight, 4, OIIO::TypeDesc::FLOAT);
        spec.channelnames = { "R", "G", "B", "A" };
        spec.alpha_channel = 3;
        std::vector<float> pixels((std::size_t)kWidth * kHeight * 4, kOther);
        OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
        ASSERT_TRUE(bool(out));
        ASSERT_TRUE(out->open(path, spec));
        ASSERT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()));
        ASSERT_TRUE(out->close());
    }
    setReadBack(true);

    EXPECT_TRUE(outputIsGray(kGray, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, TheReadBackKnobMovesTheNodeHashButNotTheOutputContentHash)
{
    setFile(pathOf("same.exr"));
    const U64 before = _writer->getHashValue();
    const U64 outputBefore = writeEffect()->getOutputContentHash();

    setReadBack(true);
    EXPECT_NE(before, _writer->getHashValue());
    EXPECT_EQ(outputBefore, writeEffect()->getOutputContentHash());

    setReadBack(false);
    EXPECT_EQ(before, _writer->getHashValue());
    EXPECT_EQ(outputBefore, writeEffect()->getOutputContentHash());
}

TEST_F(NativeWriteReadBackTest, AFileWrittenWithReadBackOnHoldsTheOutputContentHash)
{
    const std::string path = pathOf("written-on.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    setReadBack(true);
    writeSequence();
    finishSequence();

    OIIO::ImageInput::unique_ptr in = OIIO::ImageInput::open(path);
    ASSERT_TRUE(bool(in));
    EXPECT_EQ(currentHashText(), in->spec().get_string_attribute(kNatronWriteOutputHashAttribute));
}

TEST_F(NativeWriteReadBackTest, TurningItOffPassesTheInputThrough)
{
    const std::string path = pathOf("off.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    overwriteWithGray(path, kOther);
    setReadBack(true);
    ASSERT_TRUE(outputIsGray(kOther, 1e-6f)) << message();

    setReadBack(false);

    EXPECT_TRUE(outputIsGray(kGray, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, AFinishedSequenceDoesNotMoveTheHash)
{
    const std::string path = pathOf("hash.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    setReadBack(true);
    const U64 before = _writer->getHashValue();

    finishSequence();

    EXPECT_EQ(before, _writer->getHashValue());
}

TEST_F(NativeWriteReadBackTest, AMissingFrameShowsTheInputWithoutAnError)
{
    setFile(pathOf("seq.####.exr"));
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    setReadBack(true);

    std::vector<float> pixels;
    EXPECT_TRUE(outputAt(2., &pixels));
    EXPECT_FALSE(_writer->hasPersistentMessage()) << message();

    EXPECT_TRUE(outputAt(1., &pixels));
    EXPECT_FALSE(_writer->hasPersistentMessage()) << message();
}

TEST_F(NativeWriteReadBackTest, TheRenderButtonIsDisabledWhileReadBackIsOn)
{
    KnobIPtr button = _writer->getKnobByName(kNatronWriteParamStartRender);

    ASSERT_TRUE(bool(button));
    setFile(pathOf("button.exr"));
    EXPECT_TRUE(button->isEnabled(0));

    setReadBack(true);
    EXPECT_FALSE(button->isEnabled(0));

    setReadBack(false);
    EXPECT_TRUE(button->isEnabled(0));
}

TEST_F(NativeWriteReadBackTest, TheReloadButtonIsEnabledOnlyWhileReadingBack)
{
    KnobIPtr reload = _writer->getKnobByName(kNatronWriteParamReload);

    ASSERT_TRUE(bool(reload));
    setFile(pathOf("reload.exr"));
    EXPECT_FALSE(reload->getIsSecret());
    EXPECT_FALSE(reload->isEnabled(0));

    setReadBack(true);
    EXPECT_TRUE(reload->isEnabled(0));

    setReadBack(false);
    EXPECT_FALSE(reload->isEnabled(0));
}

TEST_F(NativeWriteReadBackTest, ReloadReadsTheLayersOfAFileChangedOnDisk)
{
    const std::string path = pathOf("layers.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    overwriteWithGray(path, kOther);
    setReadBack(true);
    ASSERT_EQ(std::set<std::string>({ kNatronColorLayerID }), producedLayerIDs());

    overwriteWithGray(path, kOther, { "depth.Z" });
    // The produced layers are cached under the node hash, which a file changed on disk leaves alone.
    EXPECT_EQ(std::set<std::string>({ kNatronColorLayerID }), producedLayerIDs());

    pressReload();
    EXPECT_EQ(std::set<std::string>({ kNatronColorLayerID, "depth" }), producedLayerIDs());
}

TEST_F(NativeWriteReadBackTest, ReadBackReportsTheFilesFormatAndPixelAspect)
{
    writeFixedFormatFile(pathOf("format.exr"));
    ASSERT_EQ(RectI(0, 0, kWidth, kHeight), outputFormat());
    ASSERT_EQ(1., outputPar());

    setReadBack(true);

    EXPECT_EQ(RectI(0, 0, kFileWidth, kFileHeight), outputFormat());
    EXPECT_EQ(kFilePar, outputPar());
}

TEST_F(NativeWriteReadBackTest, TurningReadBackOffReportsTheInputsFormatAgain)
{
    writeFixedFormatFile(pathOf("formatoff.exr"));
    setReadBack(true);
    ASSERT_EQ(RectI(0, 0, kFileWidth, kFileHeight), outputFormat());

    setReadBack(false);

    EXPECT_EQ(RectI(0, 0, kWidth, kHeight), outputFormat());
    EXPECT_EQ(1., outputPar());
}

TEST_F(NativeWriteReadBackTest, AMissingFileKeepsTheInputsFormat)
{
    setFile(pathOf("never-written.exr"));

    setReadBack(true);

    EXPECT_EQ(RectI(0, 0, kWidth, kHeight), outputFormat());
    EXPECT_EQ(1., outputPar());
}
