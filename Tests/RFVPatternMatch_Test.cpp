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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/RFV/RFVPatternMatch.h"

NATRON_NAMESPACE_USING

namespace {

struct GlobCase {
    const char* glob;
    const char* value;
    bool matches;
};

const GlobCase kGlobCases[] = {
    { "sh010", "sh010", true },
    { "sh010", "sh0100", false },
    { "sh01*", "sh010", true },
    { "sh01*", "sh01", true },
    { "sh01*", "xsh010", false },
    { "*", "", true },
    { "*", "anything", true },
    { "*0", "sh010", true },
    { "*1*", "sh010", true },
    { "a*b*c", "aXbYbZc", true },
    { "a*b*c", "aXbYbZ", false },
    { "sh0?0", "sh020", true },
    { "sh0?0", "sh00", false },
    { "?", "\xC3\xA9", true },
    { "sh0[12]0", "sh020", true },
    { "sh0[12]0", "sh030", false },
    { "sh0[!12]0", "sh030", true },
    { "sh0[!12]0", "sh010", false },
    { "sh0[1-3]0", "sh030", true },
    { "sh0[1-3]0", "sh040", false },
    { "[]]", "]", true },
    { "[!]]", "]", false },
    { "a[b", "a[b", true },
    { "a[b", "ab", false },
    { "", "", true },
    { "SH010", "sh010", false },
};

} // namespace

TEST(RFVPatternMatch, Globs)
{
    for (const GlobCase& c : kGlobCases) {
        EXPECT_EQ(c.matches, RFV::matchesGlob(c.value, c.glob)) << "glob '" << c.glob << "' value '" << c.value << "'";
    }
}

TEST(RFVPatternMatch, RegexesMatchTheWholeValue)
{
    const std::vector<std::string> lines = { "re:sh0[2-9]0" };
    EXPECT_EQ(0, RFV::firstMatch("sh030", lines));
    EXPECT_EQ(-1, RFV::firstMatch("sh010", lines));
    EXPECT_EQ(-1, RFV::firstMatch("xsh030", lines));
    EXPECT_EQ(-1, RFV::firstMatch("sh0300", lines));
    EXPECT_EQ(0, RFV::firstMatch("v2", std::vector<std::string> { "re:v\\d+|w" }));
    EXPECT_EQ(0, RFV::firstMatch("w", std::vector<std::string> { "re:v\\d+|w" }));
    // A regex line is never a glob, and a glob line is never a regex.
    EXPECT_EQ(-1, RFV::firstMatch("sh010", std::vector<std::string> { "re:sh01*" }));
    EXPECT_EQ(0, RFV::firstMatch("sh0111", std::vector<std::string> { "re:sh01*" }));
    EXPECT_EQ(-1, RFV::firstMatch("sh020", std::vector<std::string> { "sh0[2-9]0+" }));
}

TEST(RFVPatternMatch, FirstMatchingLineWins)
{
    const std::vector<std::string> lines = { "sh01*", "re:sh0[2-9]0", "*" };
    EXPECT_EQ(0, RFV::firstMatch("sh010", lines));
    EXPECT_EQ(1, RFV::firstMatch("sh030", lines));
    EXPECT_EQ(2, RFV::firstMatch("other", lines));
    EXPECT_EQ(0, RFV::firstMatch("sh010", std::vector<std::string> { "*", "sh010" }));
    EXPECT_EQ(-1, RFV::firstMatch("sh010", std::vector<std::string>()));
}

TEST(RFVPatternMatch, EmptyLinesNeverMatchButKeepTheirIndex)
{
    EXPECT_EQ(-1, RFV::firstMatch("", std::vector<std::string> { "" }));
    EXPECT_EQ(2, RFV::firstMatch("b", std::vector<std::string> { "a", "", "b" }));
    EXPECT_EQ(1, RFV::firstMatch("", std::vector<std::string> { "", "*" }));

    EXPECT_EQ((std::vector<std::string> { "" }), RFV::splitPatternLines(""));
    EXPECT_EQ((std::vector<std::string> { "a", "", "b" }), RFV::splitPatternLines("a\n\nb"));
    EXPECT_EQ((std::vector<std::string> { "a", "b", "" }), RFV::splitPatternLines("a\r\nb\n"));
}

TEST(RFVPatternMatch, InvalidRegexNeverMatchesAndIsReported)
{
    std::string error;
    EXPECT_EQ(-1, RFV::firstMatch("sh010", std::vector<std::string> { "re:sh0[1" }, &error));
    EXPECT_EQ(0u, error.find("Input 0: "));

    EXPECT_EQ(1, RFV::firstMatch("sh010", std::vector<std::string> { "re:(", "sh010" }, &error));
    EXPECT_EQ(0u, error.find("Input 0: "));

    // Reported past the matching line too, so the node shows it whichever input is chosen.
    EXPECT_EQ(0, RFV::firstMatch("sh010", std::vector<std::string> { "sh010", "", "re:)" }, &error));
    EXPECT_EQ(0u, error.find("Input 2: "));

    EXPECT_EQ(0, RFV::firstMatch("sh010", std::vector<std::string> { "re:sh010" }, &error));
    EXPECT_TRUE(error.empty());
}
