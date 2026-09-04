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

#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <sstream>

CLANG_DIAG_OFF(deprecated)
#include <QString>
#include <QStringList>
CLANG_DIAG_ON(deprecated)

#include "Engine/CLArgs.h"

NATRON_NAMESPACE_USING

namespace {

QStringList
makeArgs(std::initializer_list<const char*> tokens)
{
    QStringList args;
    for (const char* token : tokens) {
        args << QString::fromUtf8(token);
    }

    return args;
}

constexpr int kNoStep = std::numeric_limits<int>::min();

// The parser reports malformed input by writing straight to std::cout, which
// is expected (it is meant for a command-line renderer, not a test binary).
// Redirect it for the duration of one construction so the ctest log stays
// readable; this is purely cosmetic and asserts nothing about the messages.
class ScopedCoutSilencer {
public:
    ScopedCoutSilencer()
        : _oldBuf(std::cout.rdbuf(_sink.rdbuf()))
    {
    }

    ~ScopedCoutSilencer()
    {
        std::cout.rdbuf(_oldBuf);
    }

private:
    std::ostringstream _sink;
    std::streambuf* _oldBuf;
};

} // namespace

TEST(CLArgs, ReaderArgLandsInGetReaderArgs)
{
    CLArgs cl(makeArgs({ "NatronRenderer", "-i", "Read1", "/tmp/input.exr", "/tmp/project.ntp" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_EQ(1u, cl.getReaderArgs().size());
    const CLArgs::ReaderArg& r = cl.getReaderArgs().front();
    EXPECT_EQ(QString::fromUtf8("Read1"), r.name);
    EXPECT_EQ(QString::fromUtf8("/tmp/input.exr"), r.filename);
}

TEST(CLArgs, WriterArgWithExplicitFilenameLandsInGetWriterArgs)
{
    CLArgs cl(makeArgs({ "NatronRenderer", "-w", "Write1", "/tmp/output.exr", "/tmp/project.ntp" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_EQ(1u, cl.getWriterArgs().size());
    const CLArgs::WriterArg& w = cl.getWriterArgs().front();
    EXPECT_EQ(QString::fromUtf8("Write1"), w.name);
    EXPECT_EQ(QString::fromUtf8("/tmp/output.exr"), w.filename);
    EXPECT_FALSE(w.mustCreate);
}

TEST(CLArgs, WriterArgWithoutFilenameLeavesFilenameEmpty)
{
    // The next token is the project file (.ntp), which the optional-filename
    // heuristic explicitly excludes, so it is correctly left for the
    // positional project-file parsing below rather than being swallowed here.
    CLArgs cl(makeArgs({ "NatronRenderer", "-w", "Write1", "/tmp/project.ntp" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_EQ(1u, cl.getWriterArgs().size());
    const CLArgs::WriterArg& w = cl.getWriterArgs().front();
    EXPECT_EQ(QString::fromUtf8("Write1"), w.name);
    EXPECT_TRUE(w.filename.isEmpty());
    EXPECT_EQ(QString::fromUtf8("/tmp/project.ntp"), cl.getScriptFilename());
}

TEST(CLArgs, FrameRangeSimple)
{
    CLArgs cl(makeArgs({ "NatronRenderer", "/tmp/project.ntp", "10-20" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_TRUE(cl.hasFrameRange());
    ASSERT_EQ(1u, cl.getFrameRanges().size());
    const std::pair<int, std::pair<int, int>>& range = cl.getFrameRanges().front();
    EXPECT_EQ(kNoStep, range.first);
    EXPECT_EQ(10, range.second.first);
    EXPECT_EQ(20, range.second.second);
}

TEST(CLArgs, FrameRangeMultipleCommaSeparated)
{
    CLArgs cl(makeArgs({ "NatronRenderer", "/tmp/project.ntp", "1-10,20-30" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_TRUE(cl.hasFrameRange());
    ASSERT_EQ(2u, cl.getFrameRanges().size());
    auto it = cl.getFrameRanges().begin();
    EXPECT_EQ(kNoStep, it->first);
    EXPECT_EQ(1, it->second.first);
    EXPECT_EQ(10, it->second.second);
    ++it;
    EXPECT_EQ(kNoStep, it->first);
    EXPECT_EQ(20, it->second.first);
    EXPECT_EQ(30, it->second.second);
}

TEST(CLArgs, FrameRangeWithStep)
{
    CLArgs cl(makeArgs({ "NatronRenderer", "/tmp/project.ntp", "1-10:2" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_TRUE(cl.hasFrameRange());
    ASSERT_EQ(1u, cl.getFrameRanges().size());
    const std::pair<int, std::pair<int, int>>& range = cl.getFrameRanges().front();
    EXPECT_EQ(2, range.first);
    EXPECT_EQ(1, range.second.first);
    EXPECT_EQ(10, range.second.second);
}

TEST(CLArgs, FrameRangeBareSingleFrame)
{
    CLArgs cl(makeArgs({ "NatronRenderer", "/tmp/project.ntp", "5" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_TRUE(cl.hasFrameRange());
    ASSERT_EQ(1u, cl.getFrameRanges().size());
    const std::pair<int, std::pair<int, int>>& range = cl.getFrameRanges().front();
    EXPECT_EQ(kNoStep, range.first);
    EXPECT_EQ(5, range.second.first);
    EXPECT_EQ(5, range.second.second);
}

TEST(CLArgs, MalformedReaderMissingNameSetsError)
{
    ScopedCoutSilencer silence;
    // The project file comes first on purpose: it is what stops the "you must specify
    // the filename of a script or project" error at the end of parse() from standing in
    // for the -i validation, and it cannot come after "-i", which would take it for the
    // Read node's name.
    CLArgs cl(makeArgs({ "NatronRenderer", "/tmp/project.ntp", "-i" }), /*forceBackground=*/true);

    ASSERT_TRUE(cl.getError().has_value());
    EXPECT_EQ(1, *cl.getError());
    EXPECT_TRUE(cl.getReaderArgs().empty());
}

TEST(CLArgs, MalformedReaderMissingFilenameSetsError)
{
    ScopedCoutSilencer silence;
    CLArgs cl(makeArgs({ "NatronRenderer", "/tmp/project.ntp", "-i", "Read1" }), /*forceBackground=*/true);

    ASSERT_TRUE(cl.getError().has_value());
    EXPECT_EQ(1, *cl.getError());
}

TEST(CLArgs, MalformedWriterNameStartingWithDigitSetsError)
{
    ScopedCoutSilencer silence;
    CLArgs cl(makeArgs({ "NatronRenderer", "-w", "1Write", "/tmp/project.ntp" }), /*forceBackground=*/true);

    ASSERT_TRUE(cl.getError().has_value());
    EXPECT_EQ(1, *cl.getError());
    EXPECT_TRUE(cl.getWriterArgs().empty());
}

TEST(CLArgs, MalformedMissingProjectOrScriptSetsError)
{
    ScopedCoutSilencer silence;
    CLArgs cl(makeArgs({ "NatronRenderer", "-i", "Read1", "/tmp/input.exr" }), /*forceBackground=*/true);

    ASSERT_TRUE(cl.getError().has_value());
    EXPECT_EQ(1, *cl.getError());
    // The reader was already parsed and stashed before the missing-project
    // check runs at the end of parse(), so it is still visible even though
    // the overall parse is an error.
    EXPECT_EQ(1u, cl.getReaderArgs().size());
}

TEST(CLArgs, MalformedTrailingGarbageSetsError)
{
    ScopedCoutSilencer silence;
    CLArgs cl(makeArgs({ "NatronRenderer", "/tmp/project.ntp", "--not-a-real-option" }), /*forceBackground=*/true);

    ASSERT_TRUE(cl.getError().has_value());
    EXPECT_EQ(1, *cl.getError());
}

TEST(CLArgs, SurpriseWriterFilenameHeuristicSwallowsSteppedFrameRange)
{
    // Characterises a real surprise in the optional-filename heuristic at
    // Engine/CLArgs.cpp:1017: it excludes tokens matching the anchored
    // pattern "[0-9\\-,]*", which has no ':' in its character class. A plain
    // range like "10-20" fully matches that class and is correctly left
    // alone. But "1-10:2" contains ':', so the *anchored full-string* match
    // fails, the heuristic concludes the token is NOT a frame range, and it
    // is swallowed whole as the writer's output filename instead. The frame
    // range is then gone: it was consumed before the later frame-range
    // parser ever sees it, and the overall parse still reports no error.
    // This assertion documents that current (surprising) behavior, not the
    // desired one.
    CLArgs cl(makeArgs({ "NatronRenderer", "-w", "Write1", "1-10:2", "/tmp/project.ntp" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_EQ(1u, cl.getWriterArgs().size());
    EXPECT_EQ(QString::fromUtf8("1-10:2"), cl.getWriterArgs().front().filename);
    EXPECT_FALSE(cl.hasFrameRange());
    EXPECT_TRUE(cl.getFrameRanges().empty());
}

TEST(CLArgs, WriterFilenameHeuristicLeavesUnSteppedRangeAlone)
{
    // Contrast case for the surprise above: without a step, the token is
    // pure digits/hyphens, the anchored heuristic regex matches it fully,
    // and it is correctly recognized as a frame range rather than a
    // filename.
    CLArgs cl(makeArgs({ "NatronRenderer", "-w", "Write1", "10-20", "/tmp/project.ntp" }), /*forceBackground=*/true);

    ASSERT_FALSE(cl.getError().has_value());
    ASSERT_EQ(1u, cl.getWriterArgs().size());
    EXPECT_TRUE(cl.getWriterArgs().front().filename.isEmpty());
    EXPECT_TRUE(cl.hasFrameRange());
    ASSERT_EQ(1u, cl.getFrameRanges().size());
    EXPECT_EQ(10, cl.getFrameRanges().front().second.first);
    EXPECT_EQ(20, cl.getFrameRanges().front().second.second);
}
