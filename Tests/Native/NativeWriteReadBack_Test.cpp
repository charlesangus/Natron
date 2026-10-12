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
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
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
        return _writer->getEffectInstance()->getOutputFormat();
    }

    double outputPar() const
    {
        return _writer->getEffectInstance()->getAspectRatio(-1);
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
                      float tolerance,
                      double time = 1.)
    {
        std::vector<float> pixels;

        if (!outputAt(time, &pixels) || pixels.size() != (std::size_t)kWidth * kHeight * 4) {
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

    std::string storedHash(const std::string& path) const
    {
        OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
        if (!input) {
            return std::string();
        }
        const std::string hash = input->spec().get_string_attribute(kNatronWriteOutputHashAttribute);
        input->close();

        return hash;
    }

    // Replaces the file with a flat float image: `value` in every colour channel and in each of
    // `extra`, 1 in alpha. With `keepHash` the new file carries the hash of the one it replaces,
    // so it still stands for this node's render of the frame.
    void overwrite(const std::string& path,
                   float value,
                   bool keepHash,
                   const std::vector<std::string>& extra = std::vector<std::string>())
    {
        const std::string hash = keepHash ? storedHash(path) : std::string();
        if (keepHash) {
            ASSERT_FALSE(hash.empty()) << path;
        }
        std::vector<std::string> names = { "R", "G", "B", "A" };
        names.insert(names.end(), extra.begin(), extra.end());
        const int n = (int)names.size();
        OIIO::ImageSpec spec(kWidth, kHeight, n, OIIO::TypeDesc::FLOAT);
        spec.channelnames = names;
        spec.alpha_channel = 3;
        if (!hash.empty()) {
            spec.attribute(kNatronWriteOutputHashAttribute, hash);
        }
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

    // Renders the file with a working-space float EXR, then replaces its pixels with kOther under
    // the same hash, so that a read back and the input tell apart.
    void renderAndMark(const std::string& path)
    {
        setFile(path);
        setBitDepth("32f");
        pickOutputSpace(project()->getWorkingColorSpace());
        writeSequence();
        overwrite(path, kOther, true);
    }

    void setCheckerColour(float value)
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
    renderAndMark(pathOf("back.exr"));

    setReadBack(true);

    EXPECT_TRUE(outputIsGray(kOther, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, ARenderedExrCarriesTheHashOfItsFrame)
{
    const std::string path = pathOf("stored.exr");
    setFile(path);
    writeSequence();

    const std::string hash = storedHash(path);
    EXPECT_EQ(16u, hash.size()) << hash;
    EXPECT_EQ(std::string::npos, hash.find_first_not_of("0123456789abcdef")) << hash;
}

TEST_F(NativeWriteReadBackTest, TheStoredHashIsNotMetadataOfTheFile)
{
    const std::string path = pathOf("metadata.exr");
    setFile(path);
    writeSequence();
    ASSERT_FALSE(storedHash(path).empty());

    NodePtr read = createWorkingSpaceRead(path);
    ASSERT_TRUE(bool(read));
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(read->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    const ImageMetadata metadata = effect->getOutputMetadata(1., ViewIdx(0));
    EXPECT_FALSE(metadata.empty());
    for (ImageMetadata::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
        EXPECT_EQ(std::string::npos, it->first.find("outputHash")) << it->first;
    }
}

TEST_F(NativeWriteReadBackTest, AFileWithoutTheHashShowsTheInput)
{
    const std::string path = pathOf("foreign.exr");
    renderAndMark(path);
    overwrite(path, kOther, false);

    setReadBack(true);

    EXPECT_TRUE(outputIsGray(kGray, 1e-6f)) << message();
    EXPECT_FALSE(_writer->hasPersistentMessage()) << message();
}

TEST_F(NativeWriteReadBackTest, AnUpstreamChangeShowsTheInputAndUndoingItShowsTheFileAgain)
{
    renderAndMark(pathOf("upstream.exr"));
    setReadBack(true);
    ASSERT_TRUE(outputIsGray(kOther, 1e-6f)) << message();

    setCheckerColour(0.3f);
    EXPECT_TRUE(outputIsGray(0.3f, 1e-6f)) << message();

    setCheckerColour(kGray);
    EXPECT_TRUE(outputIsGray(kOther, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, AWriteKnobThatChangesThePixelsShowsTheInput)
{
    renderAndMark(pathOf("depth.exr"));
    setReadBack(true);
    ASSERT_TRUE(outputIsGray(kOther, 1e-6f)) << message();

    setBitDepth("16f");
    EXPECT_TRUE(outputIsGray(kGray, 1e-6f)) << message();

    setBitDepth("32f");
    EXPECT_TRUE(outputIsGray(kOther, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, AWriteKnobThatLeavesThePixelsAloneKeepsTheMatch)
{
    renderAndMark(pathOf("range.exr"));
    setReadBack(true);
    ASSERT_TRUE(outputIsGray(kOther, 1e-6f)) << message();

    ASSERT_TRUE(setKnobValues(_writer, kNatronWriteParamOverwrite, { 0. }));
    setChoice(kNatronWriteParamFrameRange, kNatronWriteFrameRangeManual);
    ASSERT_TRUE(setKnobValues(_writer, kNatronWriteParamLastFrame, { 7. }));
    ASSERT_TRUE(setKnobValues(_writer, kNatronWriteParamFrameStep, { 2. }));

    EXPECT_TRUE(outputIsGray(kOther, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, TogglingReadBackAndReloadingKeepTheMatch)
{
    renderAndMark(pathOf("toggle.exr"));
    setReadBack(true);
    ASSERT_TRUE(outputIsGray(kOther, 1e-6f)) << message();

    setReadBack(false);
    EXPECT_TRUE(outputIsGray(kGray, 1e-6f)) << message();

    setReadBack(true);
    EXPECT_TRUE(outputIsGray(kOther, 1e-6f)) << message();

    pressReload();
    EXPECT_TRUE(outputIsGray(kOther, 1e-6f)) << message();
}

TEST_F(NativeWriteReadBackTest, ANonExrFileOffersNoReadBack)
{
    setFile(pathOf("back.png"));
    pickOutputSpace(kSRGBSpace);
    writeSequence();
    setReadBack(true);

    KnobIPtr readBack = _writer->getKnobByName(kNatronWriteParamReadBack);
    KnobIPtr reload = _writer->getKnobByName(kNatronWriteParamReload);
    KnobIPtr render = _writer->getKnobByName(kNatronWriteParamStartRender);
    ASSERT_TRUE(readBack && reload && render);
    EXPECT_TRUE(readBack->getIsSecret());
    EXPECT_FALSE(readBack->isEnabled(0));
    EXPECT_TRUE(reload->getIsSecret());
    EXPECT_FALSE(reload->isEnabled(0));
    EXPECT_TRUE(render->isEnabled(0));
    EXPECT_TRUE(outputIsGray(kGray, 1e-6f)) << message();

    setFile(pathOf("back.exr"));
    EXPECT_FALSE(readBack->getIsSecret());
    EXPECT_TRUE(reload->isEnabled(0));
    EXPECT_FALSE(render->isEnabled(0));
}

TEST_F(NativeWriteReadBackTest, AFinishedSequenceChangesTheHashSoTheViewerReadsAgain)
{
    const std::string path = pathOf("hash.exr");
    setFile(path);
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    setReadBack(true);
    const U64 before = _writer->getHashValue();

    finishSequence();

    EXPECT_NE(before, _writer->getHashValue());
}

TEST_F(NativeWriteReadBackTest, AFinishedSequenceLeavesTheHashAloneWhileReadBackIsOff)
{
    const std::string path = pathOf("idle.exr");
    setFile(path);
    setBitDepth("32f");
    writeSequence();
    const U64 before = _writer->getHashValue();

    finishSequence();

    EXPECT_EQ(before, _writer->getHashValue());
}

TEST_F(NativeWriteReadBackTest, AMissingFrameShowsTheInput)
{
    setFile(pathOf("seq.####.exr"));
    setBitDepth("32f");
    pickOutputSpace(project()->getWorkingColorSpace());
    writeSequence();
    overwrite(pathOf("seq.0001.exr"), kOther, true);
    setReadBack(true);

    EXPECT_TRUE(outputIsGray(kGray, 1e-6f, 2.)) << message();
    EXPECT_FALSE(_writer->hasPersistentMessage()) << message();

    EXPECT_TRUE(outputIsGray(kOther, 1e-6f, 1.)) << message();
}

TEST_F(NativeWriteReadBackTest, TheRenderButtonIsDisabledWhileReadBackIsOn)
{
    setFile(pathOf("button.exr"));
    KnobIPtr button = _writer->getKnobByName(kNatronWriteParamStartRender);

    ASSERT_TRUE(bool(button));
    EXPECT_TRUE(button->isEnabled(0));

    setReadBack(true);
    EXPECT_FALSE(button->isEnabled(0));

    setReadBack(false);
    EXPECT_TRUE(button->isEnabled(0));
}

TEST_F(NativeWriteReadBackTest, TheReloadButtonIsEnabledOnlyWhileReadingBack)
{
    setFile(pathOf("reload.exr"));
    KnobIPtr reload = _writer->getKnobByName(kNatronWriteParamReload);

    ASSERT_TRUE(bool(reload));
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
    renderAndMark(path);
    setReadBack(true);
    ASSERT_EQ(std::set<std::string>({ kNatronColorLayerID }), producedLayerIDs());

    overwrite(path, kOther, true, { "depth.Z" });
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
