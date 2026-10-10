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
#include <cmath>
#include <cstddef>
#include <fstream>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <OpenColorIO/OpenColorIO.h>
#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/ReadColorSpace.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kSRGBSpace = "sRGB Encoded Rec.709 (sRGB)";
const char* const kGamma22Space = "Gamma 2.2 Encoded Rec.709";
const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";
const char* const kOtherWorkingSpace = "ACES2065-1";

const char* const kInputSpaceKnob = "ocioInputSpace";
const char* const kInputSpaceMenuKnob = "ocioInputSpaceIndex";
const char* const kInputSpaceSetKnob = "ocioInputSpaceSet";
const char* const kWorkingSpaceKnob = "ocioWorkingSpace";

const char* const kUnknownSpace = "Not A Colorspace";

std::string
fixture(const char* name)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/") + name;
}

struct Decoded {
    OIIO::ImageSpec spec;
    std::vector<float> pixels; // file order, top row first
};

bool
readWithOiio(const std::string& path,
             Decoded* decoded)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    if (!input) {
        return false;
    }
    decoded->spec = input->spec();
    decoded->pixels.resize((std::size_t)decoded->spec.width * decoded->spec.height * decoded->spec.nchannels);
    const bool ok = input->read_image(0, 0, 0, decoded->spec.nchannels, OIIO::TypeDesc::FLOAT, decoded->pixels.data());
    input->close();

    return ok;
}

// Returns an empty string when an optional format has no writer in this OIIO build.
std::string
writeImage(const QTemporaryDir& dir,
           const char* file,
           OIIO::TypeDesc type,
           int bitsPerSample,
           bool optional)
{
    const int width = 16;
    const int height = 8;
    const int channels = 3;
    const std::string path = (dir.path() + QString::fromUtf8("/") + QString::fromUtf8(file)).toStdString();
    OIIO::ImageSpec spec(width, height, channels, type);
    if (bitsPerSample > 0) {
        spec.attribute("oiio:BitsPerSample", bitsPerSample);
    }
    std::vector<float> pixels((std::size_t)width * height * channels);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = (float)((i * 37) % 101) / 100.f;
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    if (!out && optional) {
        OIIO::geterror();

        return std::string();
    }
    EXPECT_TRUE(bool(out)) << file;
    if (!out) {
        return std::string();
    }
    EXPECT_TRUE(out->open(path, spec)) << out->geterror();
    EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
    EXPECT_TRUE(out->close()) << out->geterror();

    return path;
}

std::string
persistentMessage(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return message.toStdString();
}

bool
nearlyEqual(float got,
            float want)
{
    return std::fabs(got - want) <= 1e-6f * std::max(1.f, std::fabs(want));
}
} // namespace

class NativeReadColorTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
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

    NodePtr createRead()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(node));
        EXPECT_TRUE(node && dynamic_cast<NativeRead*>(node->getEffectInstance().get()));

        return node;
    }

    void setFile(const NodePtr& node,
                 const std::string& path)
    {
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(path);
        node->getEffectInstance()->refreshMetadata_public(false);
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
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kInputSpaceSetKnob));

        EXPECT_TRUE(bool(knob));

        return knob && knob->getValue();
    }

    std::string menuValue(const NodePtr& node) const
    {
        KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kInputSpaceMenuKnob));

        EXPECT_TRUE(bool(menu));

        return menu ? menu->getEntry(menu->getValue()).id : std::string();
    }

    void userPicksInputSpace(const NodePtr& node,
                             const char* space)
    {
        KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kInputSpaceMenuKnob));

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
        ASSERT_EQ(std::string(space), stringValue(node, kInputSpaceKnob));
    }

    void renderFile(const NodePtr& node,
                    const RectI& window,
                    int nComps,
                    RenderedPlane* plane)
    {
        std::list<ImageLayerDesc> layers(1, nComps == 4 ? ImageLayerDesc::getRGBAComponents() : ImageLayerDesc::getRGBComponents());
        std::vector<RenderedPlane> planes;
        std::string error;

        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, window, layers, &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        ASSERT_EQ((std::size_t)window.width() * window.height() * nComps, planes[0].pixels.size());
        *plane = planes[0];
    }

    // OIIO's own decode of the file, in the rendered plane's bottom-up row order, optionally
    // converted by OCIO's own processor for the two colourspaces.
    std::vector<float> reference(const Decoded& ref,
                                 const std::string& from,
                                 const std::string& to) const
    {
        const int width = ref.spec.width;
        const int height = ref.spec.height;
        const int nComps = ref.spec.nchannels;
        std::vector<float> flipped((std::size_t)width * height * nComps);

        for (int row = 0; row < height; ++row) {
            const int fileRow = height - 1 - row;
            std::copy(ref.pixels.begin() + (std::ptrdiff_t)((std::size_t)fileRow * width * nComps),
                      ref.pixels.begin() + (std::ptrdiff_t)((std::size_t)(fileRow + 1) * width * nComps),
                      flipped.begin() + (std::ptrdiff_t)((std::size_t)row * width * nComps));
        }
        if (!from.empty()) {
            OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu = config()->getProcessor(from.c_str(), to.c_str())->getDefaultCPUProcessor();
            OCIO_NAMESPACE::PackedImageDesc desc(flipped.data(), width, height, nComps);
            cpu->apply(desc);
        }

        return flipped;
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

    void saveResetAndLoad(const QTemporaryDir& tmp,
                          const char* fileName)
    {
        const QString dirPath = tmp.path() + QLatin1Char('/');
        QString saved;

        ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8(fileName), &saved));
        project()->reset(false, true);
        ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8(fileName)));
    }
};

TEST_F(NativeReadColorTest, AnExrTakesTheColourspaceOfTheConfigsFileRule)
{
    const std::string path = fixture("flat-rgb-only.exr");
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));
    ASSERT_EQ(3, ref.spec.nchannels);
    ASSERT_EQ(std::string(kOtherWorkingSpace), std::string(config()->getColorSpaceFromFilepath("/shots/plate.exr")));
    ASSERT_NE(project()->getFileColorSpace(eFileColorCategoryFloat), std::string(kOtherWorkingSpace));

    EXPECT_EQ(std::string(kOtherWorkingSpace), ReadColorSpace::fileRuleColorSpace(config(), "/shots/plate.exr"));
    EXPECT_EQ(std::string(kOtherWorkingSpace), ReadColorSpace::fileRuleColorSpace(config(), "/shots/ACES2065-1/plate.exr"));
    EXPECT_EQ(std::string(), ReadColorSpace::fileRuleColorSpace(config(), "/shots/plate.tif"));
    EXPECT_EQ(std::string(kOtherWorkingSpace), ReadColorSpace::defaultInputSpace(*project(), "/shots/plate.exr", OIIO::ImageSpec(4, 4, 3, OIIO::TypeDesc::HALF)));

    NodePtr node = createRead();
    setFile(node, path);

    EXPECT_EQ(std::string(kOtherWorkingSpace), stringValue(node, kInputSpaceKnob));
    EXPECT_EQ(std::string(kOtherWorkingSpace), menuValue(node));
    EXPECT_EQ(project()->getWorkingColorSpace(), stringValue(node, kWorkingSpaceKnob));
    EXPECT_FALSE(userSet(node));

    RenderedPlane plane;
    renderFile(node, OiioReadSupport::dataWindowOf(ref.spec), 3, &plane);
    const std::vector<float> raw = reference(ref, std::string(), std::string());
    const std::vector<float> want = reference(ref, kOtherWorkingSpace, project()->getWorkingColorSpace());
    ASSERT_EQ(want.size(), plane.pixels.size());
    std::size_t mismatches = 0;
    std::size_t converted = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!nearlyEqual(plane.pixels[i], want[i]) && ++mismatches <= 5) {
            ADD_FAILURE() << "sample " << i << ": got " << plane.pixels[i] << ", want " << want[i];
        }
        if (plane.pixels[i] != raw[i]) {
            ++converted;
        }
    }
    EXPECT_EQ(0u, mismatches);
    EXPECT_GT(converted, 0u);
}

TEST_F(NativeReadColorTest, AUserPickOverridesTheFileRuleOnAnExrAndSticks)
{
    const std::string path = fixture("flat-rgb-only.exr");
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));

    NodePtr node = createRead();
    setFile(node, path);
    ASSERT_EQ(std::string(kOtherWorkingSpace), stringValue(node, kInputSpaceKnob));

    userPicksInputSpace(node, kLinearRec709Space);
    EXPECT_TRUE(userSet(node));
    EXPECT_EQ(std::string(kLinearRec709Space), menuValue(node));

    setFile(node, path);
    setFile(node, fixture("png-8bit.png"));
    setFile(node, path);
    EXPECT_EQ(std::string(kLinearRec709Space), stringValue(node, kInputSpaceKnob));
    EXPECT_EQ(std::string(kLinearRec709Space), menuValue(node));
    EXPECT_TRUE(userSet(node));

    RenderedPlane plane;
    renderFile(node, OiioReadSupport::dataWindowOf(ref.spec), 3, &plane);
    const std::vector<float> want = reference(ref, kLinearRec709Space, project()->getWorkingColorSpace());
    ASSERT_EQ(want.size(), plane.pixels.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!nearlyEqual(plane.pixels[i], want[i]) && ++mismatches <= 5) {
            ADD_FAILURE() << "sample " << i << ": got " << plane.pixels[i] << ", want " << want[i];
        }
    }
    EXPECT_EQ(0u, mismatches);
}

TEST_F(NativeReadColorTest, AFloatFileWithNoRuleOrTagTakesTheProjectFloatDefault)
{
    setProjectSpace("colorSpaceFloat", kLinearRec709Space);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeImage(dir, "plate.tif", OIIO::TypeDesc::FLOAT, 0, false);
    ASSERT_FALSE(path.empty());
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));
    ASSERT_EQ(eFileColorCategoryFloat, ReadColorSpace::categoryOf(ref.spec));
    ASSERT_TRUE(ReadColorSpace::fileRuleColorSpace(config(), path).empty());
    ASSERT_TRUE(ReadColorSpace::embeddedColorSpace(config(), ref.spec).empty());

    NodePtr node = createRead();
    setFile(node, path);

    EXPECT_EQ(std::string(kLinearRec709Space), stringValue(node, kInputSpaceKnob));
    EXPECT_EQ(std::string(kLinearRec709Space), menuValue(node));
    EXPECT_NE(project()->getWorkingColorSpace(), stringValue(node, kInputSpaceKnob));
    EXPECT_FALSE(userSet(node));

    RenderedPlane plane;
    renderFile(node, OiioReadSupport::dataWindowOf(ref.spec), 3, &plane);
    const std::vector<float> raw = reference(ref, std::string(), std::string());
    const std::vector<float> want = reference(ref, kLinearRec709Space, project()->getWorkingColorSpace());
    ASSERT_EQ(want.size(), plane.pixels.size());
    std::size_t mismatches = 0;
    std::size_t converted = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!nearlyEqual(plane.pixels[i], want[i]) && ++mismatches <= 5) {
            ADD_FAILURE() << "sample " << i << ": got " << plane.pixels[i] << ", want " << want[i];
        }
        if (plane.pixels[i] != raw[i]) {
            ++converted;
        }
    }
    EXPECT_EQ(0u, mismatches);
    EXPECT_GT(converted, 0u);
}

TEST_F(NativeReadColorTest, AValidEmbeddedColourspaceWinsOverTheProjectDefault)
{
    setProjectSpace("colorSpace16Bit", kGamma22Space);
    const std::string path = fixture("png-16bit.png");
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(header && !header->subimages.empty()) << error;
    ASSERT_EQ(eFileColorCategory16Bit, ReadColorSpace::categoryOf(header->subimages[0]));
    ASSERT_EQ(std::string(kSRGBSpace), ReadColorSpace::embeddedColorSpace(config(), header->subimages[0]));

    NodePtr node = createRead();
    setFile(node, path);

    EXPECT_EQ(std::string(kSRGBSpace), stringValue(node, kInputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), menuValue(node));
    EXPECT_FALSE(userSet(node));
}

TEST_F(NativeReadColorTest, ATenBitDpxTakesTheSixteenBitDefault)
{
    setProjectSpace("colorSpace8Bit", kSRGBSpace);
    setProjectSpace("colorSpace16Bit", kGamma22Space);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeImage(dir, "ten-bit.dpx", OIIO::TypeDesc::UINT16, 10, false);
    ASSERT_FALSE(path.empty());
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(header && !header->subimages.empty()) << error;
    const OIIO::ImageSpec& spec = header->subimages[0];
    ASSERT_EQ(10, spec.get_int_attribute("oiio:BitsPerSample", 0));
    ASSERT_TRUE(ReadColorSpace::embeddedColorSpace(config(), spec).empty()) << "OIIO tags the DPX with a colourspace of the config";

    NodePtr node = createRead();
    setFile(node, path);

    EXPECT_EQ(std::string(kGamma22Space), stringValue(node, kInputSpaceKnob));
    EXPECT_EQ(std::string(kGamma22Space), menuValue(node));
}

TEST_F(NativeReadColorTest, EveryFormatTakesTheCategoryOfItsPixelType)
{
    EXPECT_EQ(eFileColorCategory8Bit, ReadColorSpace::categoryOf(OIIO::ImageSpec(4, 4, 3, OIIO::TypeDesc::UINT8)));
    EXPECT_EQ(eFileColorCategory16Bit, ReadColorSpace::categoryOf(OIIO::ImageSpec(4, 4, 3, OIIO::TypeDesc::UINT16)));
    EXPECT_EQ(eFileColorCategoryFloat, ReadColorSpace::categoryOf(OIIO::ImageSpec(4, 4, 3, OIIO::TypeDesc::HALF)));
    EXPECT_EQ(eFileColorCategoryFloat, ReadColorSpace::categoryOf(OIIO::ImageSpec(4, 4, 3, OIIO::TypeDesc::FLOAT)));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    struct Case {
        const char* file;
        OIIO::TypeDesc type;
        int bitsPerSample;
        bool optional;
        FileColorCategoryEnum category;
    };
    const Case cases[] = {
        { "eight.png", OIIO::TypeDesc::UINT8, 0, false, eFileColorCategory8Bit },
        { "eight.jpg", OIIO::TypeDesc::UINT8, 0, false, eFileColorCategory8Bit },
        { "sixteen.tif", OIIO::TypeDesc::UINT16, 0, false, eFileColorCategory16Bit },
        { "ten.dpx", OIIO::TypeDesc::UINT16, 10, false, eFileColorCategory16Bit },
        { "half.exr", OIIO::TypeDesc::HALF, 0, false, eFileColorCategoryFloat },
        { "radiance.hdr", OIIO::TypeDesc::FLOAT, 0, false, eFileColorCategoryFloat },
        { "portable.pfm", OIIO::TypeDesc::FLOAT, 0, true, eFileColorCategoryFloat },
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const std::string path = writeImage(dir, cases[i].file, cases[i].type, cases[i].bitsPerSample, cases[i].optional);
        if (path.empty()) {
            continue;
        }
        std::string error;
        const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
        ASSERT_TRUE(header && !header->subimages.empty()) << cases[i].file << ": " << error;
        EXPECT_EQ(cases[i].category, ReadColorSpace::categoryOf(header->subimages[0])) << cases[i].file;
    }
}

TEST_F(NativeReadColorTest, AnUntaggedFileTakesTheProjectDefaultOfItsCategory)
{
    setProjectSpace("colorSpace8Bit", kGamma22Space);
    setProjectSpace("colorSpace16Bit", kLinearRec709Space);
    const std::string path("/nonexistent/untagged.tif");

    EXPECT_EQ(std::string(kGamma22Space), ReadColorSpace::defaultInputSpace(*project(), path, OIIO::ImageSpec(4, 4, 3, OIIO::TypeDesc::UINT8)));
    EXPECT_EQ(std::string(kLinearRec709Space), ReadColorSpace::defaultInputSpace(*project(), path, OIIO::ImageSpec(4, 4, 3, OIIO::TypeDesc::UINT16)));
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategoryFloat), ReadColorSpace::defaultInputSpace(*project(), path, OIIO::ImageSpec(4, 4, 3, OIIO::TypeDesc::FLOAT)));

    OIIO::ImageSpec tagged(4, 4, 3, OIIO::TypeDesc::UINT8);
    tagged.attribute("oiio:ColorSpace", kLinearRec709Space);
    EXPECT_EQ(std::string(kLinearRec709Space), ReadColorSpace::defaultInputSpace(*project(), path, tagged));

    OIIO::ImageSpec unknown(4, 4, 3, OIIO::TypeDesc::UINT8);
    unknown.attribute("oiio:ColorSpace", kUnknownSpace);
    EXPECT_EQ(std::string(kGamma22Space), ReadColorSpace::defaultInputSpace(*project(), path, unknown));
}

TEST_F(NativeReadColorTest, AnEightBitPngTakesTheEightBitDefaultAndMatchesOcio)
{
    const std::string path = fixture("png-8bit.png");
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));
    ASSERT_EQ(3, ref.spec.nchannels);
    ASSERT_EQ(eFileColorCategory8Bit, ReadColorSpace::categoryOf(ref.spec));

    NodePtr node = createRead();
    setFile(node, path);
    const std::string input = stringValue(node, kInputSpaceKnob);
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategory8Bit), input);
    EXPECT_EQ(std::string(kSRGBSpace), input);

    RenderedPlane plane;
    renderFile(node, OiioReadSupport::dataWindowOf(ref.spec), 3, &plane);
    const std::vector<float> raw = reference(ref, std::string(), std::string());
    const std::vector<float> want = reference(ref, input, project()->getWorkingColorSpace());
    ASSERT_EQ(want.size(), plane.pixels.size());
    std::size_t mismatches = 0;
    std::size_t converted = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!nearlyEqual(plane.pixels[i], want[i]) && ++mismatches <= 5) {
            ADD_FAILURE() << "sample " << i << ": got " << plane.pixels[i] << ", want " << want[i];
        }
        if (plane.pixels[i] != raw[i]) {
            ++converted;
        }
    }
    EXPECT_EQ(0u, mismatches);
    EXPECT_GT(converted, 0u);
}

TEST_F(NativeReadColorTest, AWorkingSpaceChangeReRendersTheRead)
{
    const std::string path = fixture("png-8bit.png");
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));
    const RectI window = OiioReadSupport::dataWindowOf(ref.spec);

    NodePtr node = createRead();
    setFile(node, path);
    RenderedPlane before;
    renderFile(node, window, 3, &before);
    const U64 hashBefore = node->getHashValue();

    setProjectSpace("workingSpace", kOtherWorkingSpace);
    EXPECT_EQ(std::string(kOtherWorkingSpace), stringValue(node, kWorkingSpaceKnob));
    EXPECT_NE(hashBefore, node->getHashValue());

    RenderedPlane after;
    renderFile(node, window, 3, &after);
    EXPECT_NE(before.pixels, after.pixels);
    const std::vector<float> want = reference(ref, stringValue(node, kInputSpaceKnob), kOtherWorkingSpace);
    ASSERT_EQ(want.size(), after.pixels.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!nearlyEqual(after.pixels[i], want[i]) && ++mismatches <= 5) {
            ADD_FAILURE() << "sample " << i << ": got " << after.pixels[i] << ", want " << want[i];
        }
    }
    EXPECT_EQ(0u, mismatches);
}

TEST_F(NativeReadColorTest, AUserPickSurvivesAFilenameChangeAndAGuessDoesNot)
{
    setProjectSpace("colorSpace16Bit", kGamma22Space);
    NodePtr node = createRead();
    setFile(node, fixture("png-8bit.png"));
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(node, kInputSpaceKnob));
    ASSERT_FALSE(userSet(node));

    userPicksInputSpace(node, kLinearRec709Space);
    EXPECT_TRUE(userSet(node));
    EXPECT_EQ(std::string(kLinearRec709Space), menuValue(node));

    setFile(node, fixture("flat-rgb-only.exr"));
    EXPECT_EQ(std::string(kLinearRec709Space), stringValue(node, kInputSpaceKnob));

    KnobBoolPtr spaceSet = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kInputSpaceSetKnob));
    ASSERT_TRUE(bool(spaceSet));
    spaceSet->setValue(false);
    setFile(node, fixture("png-8bit.png"));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(node, kInputSpaceKnob));
    setFile(node, fixture("flat-rgb-only.exr"));
    EXPECT_EQ(std::string(kOtherWorkingSpace), stringValue(node, kInputSpaceKnob));
    EXPECT_FALSE(userSet(node));
}

TEST_F(NativeReadColorTest, ALoadedNodeKeepsItsSavedInputSpace)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr picked = createRead();
    setFile(picked, fixture("png-8bit.png"));
    userPicksInputSpace(picked, kLinearRec709Space);
    NodePtr guessed = createRead();
    setFile(guessed, fixture("png-8bit.png"));
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(guessed, kInputSpaceKnob));
    const std::string pickedName = picked->getScriptName();
    const std::string guessedName = guessed->getScriptName();

    // A restored node that guessed again would take this new default.
    setProjectSpace("colorSpace8Bit", kGamma22Space);

    saveResetAndLoad(tmp, "native-read-colour.ntp");

    ASSERT_EQ(std::string(kGamma22Space), project()->getFileColorSpace(eFileColorCategory8Bit));
    picked = project()->getNodeByName(pickedName);
    guessed = project()->getNodeByName(guessedName);
    ASSERT_TRUE(bool(picked));
    ASSERT_TRUE(bool(guessed));
    EXPECT_EQ(std::string(kLinearRec709Space), stringValue(picked, kInputSpaceKnob));
    EXPECT_EQ(std::string(kLinearRec709Space), menuValue(picked));
    EXPECT_TRUE(userSet(picked));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(guessed, kInputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), menuValue(guessed));
    EXPECT_FALSE(userSet(guessed));
    EXPECT_EQ(project()->getWorkingColorSpace(), stringValue(guessed, kWorkingSpaceKnob));
}

TEST_F(NativeReadColorTest, AnUnknownInputSpaceIsReported)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = fixture("png-8bit.png");
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));

    NodePtr node = createRead();
    setFile(node, path);
    KnobStringBasePtr space = std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(kInputSpaceKnob));
    ASSERT_TRUE(bool(space));
    ASSERT_TRUE(space->getIsSecret());
    space->setValue(std::string(kUnknownSpace));
    EXPECT_FALSE(space->getIsSecret());

    project()->reportUnresolvedOCIOColorSpaces();
    std::string message = persistentMessage(node);
    EXPECT_NE(std::string::npos, message.find(kInputSpaceKnob)) << message;
    EXPECT_NE(std::string::npos, message.find(kUnknownSpace)) << message;

    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, OiioReadSupport::dataWindowOf(ref.spec), layers, &planes, &error));
    EXPECT_NE(std::string::npos, persistentMessage(node).find(kUnknownSpace)) << persistentMessage(node);

    const std::string name = node->getScriptName();
    saveResetAndLoad(tmp, "native-read-unknown-space.ntp");
    node = project()->getNodeByName(name);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(std::string(kUnknownSpace), stringValue(node, kInputSpaceKnob));
    message = persistentMessage(node);
    EXPECT_NE(std::string::npos, message.find(kInputSpaceKnob)) << message;
    EXPECT_NE(std::string::npos, message.find(kUnknownSpace)) << message;
}

TEST_F(NativeReadColorTest, TheInputColourspaceMenuNestsTheConfigsFamilies)
{
    ASSERT_EQ('/', config()->getFamilySeparator());
    NodePtr node = createRead();
    KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kInputSpaceMenuKnob));
    ASSERT_TRUE(bool(menu));
    EXPECT_TRUE(menu->isCascading());

    const std::vector<std::string> names = project()->getColorManagement()->getColorSpaces();
    const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
    ASSERT_EQ(names.size(), entries.size());
    std::set<std::string> labels;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(names[i], entries[i].id);
        OCIO_NAMESPACE::ConstColorSpaceRcPtr cs = config()->getColorSpace(entries[i].id.c_str());
        ASSERT_TRUE(bool(cs)) << entries[i].id;
        const std::string family = cs->getFamily() ? cs->getFamily() : "";
        EXPECT_EQ(family.empty() ? entries[i].id : family + "/" + entries[i].id, entries[i].label);
        EXPECT_TRUE(labels.insert(entries[i].label).second) << entries[i].label;
    }

    struct Expected {
        const char* id;
        const char* label;
    };
    const Expected expected[] = {
        { "ACEScg", "ACES/ACEScg" },
        { "ARRI LogC4", "Input/ARRI/ARRI LogC4" },
        { kLinearRec709Space, "Utility/Linear Rec.709 (sRGB)" },
        { "Camera Rec.709", "Utility/ITU/Camera Rec.709" },
    };
    for (std::size_t e = 0; e < sizeof(expected) / sizeof(expected[0]); ++e) {
        userPicksInputSpace(node, expected[e].id);
        EXPECT_EQ(std::string(expected[e].id), stringValue(node, kInputSpaceKnob));
        EXPECT_EQ(std::string(expected[e].id), menuValue(node));
        EXPECT_EQ(std::string(expected[e].label), menu->getEntry(menu->getValue()).label);
        EXPECT_TRUE(userSet(node));
    }

    KnobChoicePtr working = project()->getKnobByNameAndType<KnobChoice>("workingSpace");
    ASSERT_TRUE(bool(working));
    EXPECT_TRUE(working->isCascading());
    const std::vector<ChoiceOption> workingEntries = working->getEntries_mt_safe();
    ASSERT_EQ(entries.size(), workingEntries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(entries[i].id, workingEntries[i].id);
        EXPECT_EQ(entries[i].label, workingEntries[i].label);
    }
}

TEST_F(NativeReadColorTest, ColourspaceMenuPathsNeverCollideWithASubmenu)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = (dir.path() + QString::fromUtf8("/config.ocio")).toStdString();
    {
        std::ofstream out(path.c_str());
        out << "ocio_profile_version: 2\n"
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
               "    name: Utility\n"
               "  - !<ColorSpace>\n"
               "    name: lin\n"
               "    family: Utility\n"
               "  - !<ColorSpace>\n"
               "    name: Sub\n"
               "    family: Utility\n"
               "  - !<ColorSpace>\n"
               "    name: a/b\n"
               "    family: Utility/Sub\n";
        ASSERT_TRUE(out.good());
    }
    ProjectColorManagement cm;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cm.load(path, std::string(), &error)) << error;

    const std::vector<ChoiceOption> options = Project::colorSpaceOptions(cm);
    ASSERT_EQ(5u, options.size());
    const char* const ids[] = { "raw", "Utility", "lin", "Sub", "a/b" };
    const char* const labels[] = { "raw", "Utility/Utility", "Utility/lin", "Utility/Sub/Sub", "Utility/Sub/a\u2215b" };
    for (std::size_t i = 0; i < options.size(); ++i) {
        EXPECT_EQ(std::string(ids[i]), options[i].id);
        EXPECT_EQ(std::string(labels[i]), options[i].label);
    }
}

TEST_F(NativeReadColorTest, ResettingTheInputColourspaceMenuReturnsToTheAutomaticSpace)
{
    const std::string path = fixture("png-8bit.png");
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));
    const RectI window = OiioReadSupport::dataWindowOf(ref.spec);

    NodePtr node = createRead();
    setFile(node, path);
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(node, kInputSpaceKnob));
    userPicksInputSpace(node, kLinearRec709Space);
    ASSERT_TRUE(userSet(node));
    RenderedPlane picked;
    renderFile(node, window, 3, &picked);

    restoreDefaultAsTheGuiDoes(node->getKnobByName(kInputSpaceMenuKnob));

    EXPECT_FALSE(userSet(node));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(node, kInputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), menuValue(node));

    RenderedPlane reset;
    renderFile(node, window, 3, &reset);
    EXPECT_NE(picked.pixels, reset.pixels);
    const std::vector<float> want = reference(ref, kSRGBSpace, project()->getWorkingColorSpace());
    ASSERT_EQ(want.size(), reset.pixels.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!nearlyEqual(reset.pixels[i], want[i]) && ++mismatches <= 5) {
            ADD_FAILURE() << "sample " << i << ": got " << reset.pixels[i] << ", want " << want[i];
        }
    }
    EXPECT_EQ(0u, mismatches);

    setFile(node, fixture("flat-rgb-only.exr"));
    EXPECT_EQ(std::string(kOtherWorkingSpace), stringValue(node, kInputSpaceKnob));
}

TEST_F(NativeReadColorTest, ResettingTheInputColourspaceStringReturnsToTheAutomaticSpace)
{
    NodePtr node = createRead();
    setFile(node, fixture("png-8bit.png"));
    userPicksInputSpace(node, kLinearRec709Space);
    ASSERT_TRUE(userSet(node));

    KnobIPtr space = node->getKnobByName(kInputSpaceKnob);
    restoreDefaultAsTheGuiDoes(space);

    EXPECT_FALSE(userSet(node));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(node, kInputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), menuValue(node));
    EXPECT_TRUE(space->getIsSecret());
}

TEST_F(NativeReadColorTest, ResettingTheInputColourspaceOfAReadWithoutAFileConvertsNothing)
{
    NodePtr node = createRead();
    userPicksInputSpace(node, kLinearRec709Space);
    ASSERT_TRUE(userSet(node));

    restoreDefaultAsTheGuiDoes(node->getKnobByName(kInputSpaceMenuKnob));

    EXPECT_FALSE(userSet(node));
    EXPECT_EQ(std::string(), stringValue(node, kInputSpaceKnob));
    EXPECT_EQ(project()->getWorkingColorSpace(), menuValue(node));
}

// A config whose "shot" colourspace applies the LUT file named by the $LUT context variable.
class NativeReadContextTest
    : public NativeReadColorTest {
protected:
    void useContextConfig(const QTemporaryDir& dir)
    {
        ASSERT_TRUE(dir.isValid());
        const std::string root = dir.path().toStdString();
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

    // The first plane of the png fixture read through "shot", and the file's own pixels.
    void renderShot(const NodePtr& node,
                    std::vector<float>* converted,
                    std::vector<float>* raw)
    {
        Decoded ref;
        ASSERT_TRUE(readWithOiio(fixture("png-8bit.png"), &ref));
        RenderedPlane plane;
        renderFile(node, OiioReadSupport::dataWindowOf(ref.spec), 3, &plane);
        *converted = plane.pixels;
        *raw = reference(ref, std::string(), std::string());
    }

    void expectScaled(const std::vector<float>& got,
                      const std::vector<float>& raw,
                      float scale)
    {
        ASSERT_EQ(raw.size(), got.size());
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (!nearlyEqual(got[i], raw[i] * scale) && ++mismatches <= 5) {
                ADD_FAILURE() << "sample " << i << ": got " << got[i] << ", want " << raw[i] * scale;
            }
        }
        EXPECT_EQ(0u, mismatches);
    }

    NodePtr createShotRead()
    {
        NodePtr node = createRead();
        setFile(node, fixture("png-8bit.png"));
        userPicksInputSpace(node, "shot");

        return node;
    }
};

TEST_F(NativeReadContextTest, TheContextGroupHasFourKeyValuePairs)
{
    NodePtr node = createRead();

    EXPECT_TRUE(dynamic_cast<KnobGroup*>(node->getKnobByName("Context").get()) != NULL);
    for (int i = 1; i <= 4; ++i) {
        const std::string n = std::to_string(i);
        KnobStringPtr key = std::dynamic_pointer_cast<KnobString>(node->getKnobByName("key" + n));
        KnobStringPtr value = std::dynamic_pointer_cast<KnobString>(node->getKnobByName("value" + n));

        ASSERT_TRUE(bool(key)) << "key" << n;
        ASSERT_TRUE(bool(value)) << "value" << n;
        EXPECT_EQ(std::string(), key->getValue());
        EXPECT_EQ(std::string(), value->getValue());
        EXPECT_FALSE(key->getIsSecret());
    }
}

TEST_F(NativeReadContextTest, AContextVariableChangesTheConversionAndTheHash)
{
    QTemporaryDir dir;
    useContextConfig(dir);
    NodePtr node = createShotRead();

    std::vector<float> withDefault, raw;
    renderShot(node, &withDefault, &raw);
    expectScaled(withDefault, raw, 0.5f);
    const U64 hashBefore = node->getHashValue();

    setContext(node, 2, "LUT", "quarter");
    EXPECT_NE(hashBefore, node->getHashValue());

    std::vector<float> withContext;
    renderShot(node, &withContext, &raw);
    expectScaled(withContext, raw, 0.25f);

    setContext(node, 2, "", "quarter");
    std::vector<float> cleared;
    renderShot(node, &cleared, &raw);
    expectScaled(cleared, raw, 0.5f);
}

TEST_F(NativeReadContextTest, PairsWithAnEmptyKeyAreIgnored)
{
    QTemporaryDir dir;
    useContextConfig(dir);
    NodePtr node = createShotRead();

    std::vector<float> none, raw;
    renderShot(node, &none, &raw);

    setContext(node, 1, "", "quarter");
    setContext(node, 3, "", "");
    std::vector<float> emptyKeys;
    renderShot(node, &emptyKeys, &raw);
    EXPECT_EQ(none, emptyKeys);

    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.push_back(std::make_pair(std::string(), std::string("quarter")));
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr withIgnored, without;
    std::string error;
    ASSERT_TRUE(ReadColorSpace::toWorkingProcessor(*project(), "shot", "lin", pairs, &withIgnored, &error)) << error;
    ASSERT_TRUE(ReadColorSpace::toWorkingProcessor(*project(), "shot", "lin", std::vector<std::pair<std::string, std::string>>(), &without, &error)) << error;
    ASSERT_TRUE(bool(withIgnored));
    EXPECT_EQ(without, withIgnored);
}

TEST_F(NativeReadContextTest, EachPairOfTheGroupSetsAContextVariable)
{
    QTemporaryDir dir;
    useContextConfig(dir);
    NodePtr node = createShotRead();

    setContext(node, 1, "UNUSED", "x");
    setContext(node, 4, "LUT", "quarter");
    std::vector<float> got, raw;
    renderShot(node, &got, &raw);
    expectScaled(got, raw, 0.25f);
}
