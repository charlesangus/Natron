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
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_USING

namespace {

// 16384 x 8448 x 4 floats is 2.2 GB, past the 2^31 bytes a signed 32 bit offset or stride holds.
const int kWidth = 16384;
const int kHeight = 8448;
const int kChannels = 4;
const std::uint64_t kFrameBytes = (std::uint64_t)kWidth * kHeight * kChannels * sizeof(float);

// Written pixels of the support-level test: every row differs from its neighbours and every
// channel from the others, so a misplaced row, a shifted band or a swapped channel shows.
float
procedural(int x,
           int row,
           int channel)
{
    const std::uint32_t mixed = (std::uint32_t)x * 31u + (std::uint32_t)row * 17u + (std::uint32_t)channel * 7u;

    return (float)(mixed & 4095u) / 4096.f;
}

std::uint64_t
availableMemoryBytes()
{
    std::ifstream in("/proc/meminfo");
    std::string key;
    std::uint64_t kilobytes = 0;
    std::string unit;

    while (in >> key >> kilobytes >> unit) {
        if (key == "MemAvailable:") {
            return kilobytes * 1024;
        }
    }

    return 0;
}

// The test binary's own directory is inside the build tree, which has room for the frame; /tmp is
// often a small tmpfs.
QString
scratchRoot()
{
    if (QCoreApplication::instance()) {
        return QCoreApplication::applicationDirPath();
    }

    return QString::fromStdString(std::filesystem::current_path().string());
}

std::string
megabytes(std::uint64_t bytes)
{
    return std::to_string(bytes >> 20) + " MiB";
}

// Why the machine cannot run a frame of this size, or empty when it can.
std::string
missingRoom(const std::string& directory,
            std::uint64_t diskNeeded,
            std::uint64_t memoryNeeded)
{
    std::error_code ec;
    const std::filesystem::space_info space = std::filesystem::space(directory, ec);
    const std::uint64_t disk = ec ? 0 : (std::uint64_t)space.available;
    const std::uint64_t memory = availableMemoryBytes();

    if (disk < diskNeeded) {
        return "need " + megabytes(diskNeeded) + " of disk under " + directory + ", have " + megabytes(disk);
    }
    if (memory < memoryNeeded) {
        return "need " + megabytes(memoryNeeded) + " of available memory, have " + megabytes(memory);
    }

    return std::string();
}

// This gtest has no GTEST_SKIP, so a test that returns early is reported as passed. The skip is
// printed and recorded in the XML report so that it shows, and NATRON_REQUIRE_LARGE_TESTS=1 makes it
// a failure for a run that must exercise the large frame.
void
reportSkip(const std::string& missing)
{
    const char* required = std::getenv("NATRON_REQUIRE_LARGE_TESTS");

    if (required && (std::string(required) == "1")) {
        ADD_FAILURE() << "NATRON_REQUIRE_LARGE_TESTS is set but the test cannot run: " << missing;

        return;
    }
    std::cerr << "SKIPPED: " << ::testing::UnitTest::GetInstance()->current_test_info()->name() << ": " << missing << std::endl;
    ::testing::Test::RecordProperty("skipped", missing);
}

// The scanlines checked: the first, one a third of the way down, the middle and the last.
std::vector<int>
probeRows()
{
    return { 0, 3000, kHeight / 2, kHeight - 1 };
}

bool
readRow(OIIO::ImageInput& input,
        int row,
        std::vector<float>* pixels)
{
    pixels->assign((std::size_t)kWidth * kChannels, 0.f);

    return input.read_scanline(row, 0, OIIO::TypeDesc::FLOAT, pixels->data());
}
} // namespace

TEST(NativeWriteLarge, SupportWritesAFrameOverTwoGigabytes)
{
    QTemporaryDir dir(QDir(scratchRoot()).filePath(QStringLiteral("NativeWriteLarge-XXXXXX")));

    ASSERT_TRUE(dir.isValid());
    const std::string missing = missingRoom(dir.path().toStdString(), kFrameBytes + (kFrameBytes >> 4), (std::uint64_t)512 << 20);
    if (!missing.empty()) {
        reportSkip(missing);

        return;
    }
    const std::string path = dir.filePath(QStringLiteral("support.exr")).toStdString();

    OiioWriteSupport::WritePart part;
    part.spec = OIIO::ImageSpec(kWidth, kHeight, kChannels, OIIO::TypeDesc::FLOAT);
    part.spec.attribute("compression", "none");
    const OiioWriteSupport::RowSource source = [](int /*part*/, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride) {
        for (int row = rowBegin; row < rowEnd; ++row) {
            float* out = (float*)((char*)band + (std::ptrdiff_t)(row - rowBegin) * (std::ptrdiff_t)rowStride);
            for (int x = 0; x < kWidth; ++x) {
                for (int c = 0; c < kChannels; ++c) {
                    *out++ = procedural(x, row, c);
                }
            }
        }

        return true;
    };

    std::string error;
    ASSERT_TRUE(OiioWriteSupport::writeFile(path, { part }, source, []() { return false; }, &error)) << error;

    std::error_code ec;
    const std::uint64_t written = (std::uint64_t)std::filesystem::file_size(path, ec);
    std::cout << "ran: support wrote " << written << " bytes (" << megabytes(written) << ") to " << path << std::endl;
    EXPECT_GT(written, kFrameBytes);

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    ASSERT_TRUE(bool(input)) << OIIO::geterror();
    ASSERT_EQ(kWidth, input->spec().width);
    ASSERT_EQ(kHeight, input->spec().height);
    ASSERT_EQ(kChannels, input->spec().nchannels);
    std::vector<float> pixels;
    for (const int row : probeRows()) {
        SCOPED_TRACE(row);
        ASSERT_TRUE(readRow(*input, row, &pixels)) << input->geterror();
        int wrong = 0;
        for (int x = 0; x < kWidth; ++x) {
            for (int c = 0; c < kChannels; ++c) {
                if (pixels[(std::size_t)x * kChannels + (std::size_t)c] != procedural(x, row, c)) {
                    ++wrong;
                }
            }
        }
        EXPECT_EQ(0, wrong);
    }
    input->close();
}

class NativeWriteLargeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeWriteLarge", 1.));

        _checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        ASSERT_TRUE(bool(_checker));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamBoxSize, { 4096., 2112. }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamLineWidth, { 0. }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamCenterLineWidth, { 0. }));
        for (int i = 0; i < 4; ++i) {
            _colors[i][0] = 0.1f + 0.2f * (float)i;
            _colors[i][1] = 0.9f - 0.25f * (float)i;
            _colors[i][2] = 0.05f + 0.3f * (float)i;
            _colors[i][3] = 1.f - 0.125f * (float)i;
        }
        const char* const names[4] = { kCheckerBoardParamColor0, kCheckerBoardParamColor1, kCheckerBoardParamColor2, kCheckerBoardParamColor3 };
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(setKnobValues(_checker, names[i], { _colors[i][0], _colors[i][1], _colors[i][2], _colors[i][3] }));
        }

        _writer = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(bool(_writer));
        // The values are compared with the constants that went in, so the file is written without conversion.
        KnobStringBase* outputSpace = dynamic_cast<KnobStringBase*>(_writer->getKnobByName("ocioOutputSpace").get());
        KnobBool* outputSpaceSet = dynamic_cast<KnobBool*>(_writer->getKnobByName("ocioOutputSpaceSet").get());
        ASSERT_TRUE(outputSpace != NULL);
        ASSERT_TRUE(outputSpaceSet != NULL);
        outputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
        outputSpaceSet->setValue(true);
        connectNodes(_checker, _writer, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        _writer.reset();
        _checker.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    bool setChoice(const char* name,
                   const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(_writer->getKnobByName(name).get());

        if (!choice) {
            return false;
        }
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return true;
            }
        }

        return false;
    }

    NodePtr _checker;
    NodePtr _writer;
    float _colors[4][4];
};

TEST_F(NativeWriteLargeTest, NodeWritesAFrameOverTwoGigabytes)
{
    QTemporaryDir dir(QDir(scratchRoot()).filePath(QStringLiteral("NativeWriteLarge-XXXXXX")));

    ASSERT_TRUE(dir.isValid());
    const std::string missing = missingRoom(dir.path().toStdString(), kFrameBytes + (kFrameBytes >> 4), kFrameBytes * 3);
    if (!missing.empty()) {
        reportSkip(missing);

        return;
    }
    const std::string path = dir.filePath(QStringLiteral("node.exr")).toStdString();

    KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(_writer->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(path);
    ASSERT_TRUE(setChoice(kNatronWriteParamFormatType, kNatronWriteFormatTypeFixed));
    ASSERT_TRUE(setKnobValues(_writer, kNatronParamFormatSize, { (double)kWidth, (double)kHeight }));
    ASSERT_TRUE(setKnobValues(_writer, kNatronParamFormatPar, { 1. }));
    ASSERT_TRUE(setChoice(kNatronWriteParamBitDepth, "32f"));
    ASSERT_TRUE(setChoice(kNatronWriteParamCompression, "none"));

    OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
    getApp()->startWritersRendering(eRenderModeBlocking, works);

    std::error_code ec;
    const std::uint64_t written = (std::uint64_t)std::filesystem::file_size(path, ec);
    std::cout << "ran: node wrote " << written << " bytes (" << megabytes(written) << ") to " << path << std::endl;
    ASSERT_GT(written, kFrameBytes);

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    ASSERT_TRUE(bool(input)) << OIIO::geterror();
    ASSERT_EQ(kWidth, input->spec().width);
    ASSERT_EQ(kHeight, input->spec().height);
    ASSERT_EQ(kChannels, input->spec().nchannels);
    ASSERT_EQ(OIIO::TypeDesc(OIIO::TypeDesc::FLOAT), input->spec().format);

    // The file's rows run downwards from the top of the frame, the generator's upwards.
    const CheckerBoardGeometry g = checkerBoardGeometry(4096., 2112., 0., 0., RectD(0, 0, kWidth, kHeight), 1., 1., 1.);
    std::vector<float> pixels;
    for (const int row : probeRows()) {
        SCOPED_TRACE(row);
        ASSERT_TRUE(readRow(*input, row, &pixels)) << input->geterror();
        const CheckerBoardRow checkerRow = checkerBoardRow(g, kHeight - 1 - row);
        double maxError = 0.;
        for (int x = 0; x < kWidth;) {
            CheckerBoardColorEnum color = eCheckerBoardColor0;
            const int end = checkerBoardRunEnd(g, checkerRow, x, kWidth, &color);
            ASSERT_LT(color, 4);
            for (; x < end; ++x) {
                for (int c = 0; c < kChannels; ++c) {
                    maxError = std::max(maxError, (double)std::fabs(pixels[(std::size_t)x * kChannels + (std::size_t)c] - _colors[color][c]));
                }
            }
        }
        EXPECT_LE(maxError, 1e-6);
    }
    input->close();
}
