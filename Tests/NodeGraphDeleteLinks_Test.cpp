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

#include "Gui/NodeGraphDeleteLinks.h"

NATRON_NAMESPACE_USING

namespace {
BrokenLink
link(const char* masterNode,
     const char* masterKnob,
     const char* slaveNode,
     const char* slaveKnob)
{
    BrokenLink l;
    l.masterNode = masterNode;
    l.masterKnob = masterKnob;
    l.slaveNode = slaveNode;
    l.slaveKnob = slaveKnob;

    return l;
}

int
occurrences(const QString& text,
            const QString& what)
{
    return text.count(what);
}
} // anonymous namespace

TEST(NodeGraphDeleteLinks, NoLinksMeansNoPrompt)
{
    EXPECT_TRUE(brokenLinksPrompt(std::vector<BrokenLink>()).isEmpty());
}

TEST(NodeGraphDeleteLinks, OnePromptNamesEveryLinkUnderItsDeletedNode)
{
    std::vector<BrokenLink> links;
    links.push_back(link("Blur1", "size", "Merge1", "mix"));
    links.push_back(link("Grade1", "gain", "Merge1", "mix"));
    links.push_back(link("Blur1", "size", "Merge2", "mix"));

    const QString text = brokenLinksPrompt(links);

    EXPECT_EQ(1, occurrences(text, QStringLiteral("Blur1:")));
    EXPECT_EQ(1, occurrences(text, QStringLiteral("Grade1:")));
    EXPECT_TRUE(text.contains(QStringLiteral("Merge1.mix reads Blur1.size")));
    EXPECT_TRUE(text.contains(QStringLiteral("Merge2.mix reads Blur1.size")));
    EXPECT_TRUE(text.contains(QStringLiteral("Merge1.mix reads Grade1.gain")));
    EXPECT_FALSE(text.contains(QStringLiteral("more")));
    EXPECT_TRUE(text.endsWith(QStringLiteral("Continue anyway?")));
}

TEST(NodeGraphDeleteLinks, RepeatedLinkCountsOnce)
{
    std::vector<BrokenLink> links;
    links.push_back(link("A", "k", "B", "j"));
    links.push_back(link("A", "k", "B", "j"));

    EXPECT_EQ(1u, countDistinctBrokenLinks(links));
    EXPECT_EQ(1, occurrences(brokenLinksPrompt(links), QStringLiteral("reads")));
}

TEST(NodeGraphDeleteLinks, LongListIsCappedWithACountOfTheRest)
{
    std::vector<BrokenLink> links;
    for (int i = 0; i < 40; ++i) {
        links.push_back(link("A", "k", ("S" + std::to_string(i)).c_str(), "j"));
    }

    const QString text = brokenLinksPrompt(links, 5);

    EXPECT_EQ(5, occurrences(text, QStringLiteral(" reads ")));
    EXPECT_TRUE(text.contains(QStringLiteral("and 35 more")));
    EXPECT_TRUE(text.contains(QStringLiteral("40 link(s)")));
}

TEST(NodeGraphDeleteLinks, CapReachedInsideOneNodeStillListsTheOthersAsMore)
{
    std::vector<BrokenLink> links;
    links.push_back(link("A", "k", "S1", "j"));
    links.push_back(link("A", "k", "S2", "j"));
    links.push_back(link("B", "k", "S3", "j"));

    const QString text = brokenLinksPrompt(links, 2);

    EXPECT_FALSE(text.contains(QStringLiteral("B:")));
    EXPECT_TRUE(text.contains(QStringLiteral("and 1 more")));
}
