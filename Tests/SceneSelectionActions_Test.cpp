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

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <gtest/gtest.h>

#include "Engine/KnobTypes.h"
#include "Gui/SceneSelectionActions.h"

NATRON_NAMESPACE_USING

namespace {

std::string
edit(SceneSelectionActions::PrimEditEnum action,
     const std::string& current,
     const QStringList& paths)
{
    return SceneSelectionActions::editPrimPathsText(KnobString::ePrimPathHintPattern, action, current, paths);
}

std::string
removeFrom(const std::string& current,
           const char* path)
{
    return edit(SceneSelectionActions::ePrimEditRemove, current, QStringList(QString::fromUtf8(path)));
}

std::string
addTo(const std::string& current,
      const char* path)
{
    return edit(SceneSelectionActions::ePrimEditAdd, current, QStringList(QString::fromUtf8(path)));
}

} // namespace

TEST(SceneSelectionActions, RemoveTakesAnEntryOutOfAPathList)
{
    EXPECT_EQ(removeFrom("/a /b /c", "/b"), "/a /c");
    EXPECT_EQ(removeFrom("/a", "/a"), "");
    EXPECT_EQ(removeFrom("/a /b", "/c"), "/a /b");
}

TEST(SceneSelectionActions, RemoveFromAnExplicitUnionOfPathsDropsTheEntry)
{
    EXPECT_EQ(removeFrom("/a + /b", "/b"), "/a");
    EXPECT_EQ(removeFrom("/a+/b", "/a"), "/b");
}

TEST(SceneSelectionActions, RemoveFromACompactDifferenceSubtracts)
{
    EXPECT_EQ(removeFrom("/a-/b", "/a"), "(/a-/b) - /a");
    EXPECT_EQ(removeFrom("/a - /b", "/a"), "(/a - /b) - /a");
}

TEST(SceneSelectionActions, RemoveFromAUnionWithAWildcardSubtracts)
{
    EXPECT_EQ(removeFrom("/a/*+/b", "/a/c"), "(/a/*+/b) - /a/c");
}

TEST(SceneSelectionActions, RemoveFromAPredicatePatternSubtracts)
{
    EXPECT_EQ(removeFrom("/World/{kind:component}", "/World/Box"), "(/World/{kind:component}) - /World/Box");
}

TEST(SceneSelectionActions, RemoveFromAWildcardSubtractsAndKeepsLiteralEntriesRemovable)
{
    EXPECT_EQ(removeFrom("/World/*", "/World/Cam"), "/World/* - /World/Cam");
    EXPECT_EQ(removeFrom("/World/* /x", "/x"), "/World/*");
    EXPECT_EQ(removeFrom("/World/* /x", "/World/Cam"), "(/World/* /x) - /World/Cam");
}

TEST(SceneSelectionActions, AddToAnExpressionWithOperatorsParenthesizesIt)
{
    EXPECT_EQ(addTo("/a-/b", "/c"), "(/a-/b) /c");
    EXPECT_EQ(addTo("/a /b", "/c"), "/a /b /c");
    EXPECT_EQ(addTo("/a /b", "/b"), "/a /b");
    EXPECT_EQ(addTo("", "/c"), "/c");
}
