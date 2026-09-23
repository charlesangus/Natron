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

#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QLocale>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

TEST(KnobLayers, ColumnTagsAreLocaleStableAndRoundTrip)
{
    KnobLayersPtr knob = std::make_shared<KnobLayers>(static_cast<KnobHolder*>(NULL), std::string("layers"), 1, false);

    EXPECT_EQ(std::string("Name"), knob->getColumnTag(0));
    EXPECT_EQ(std::string("Channels"), knob->getColumnTag(1));
    EXPECT_EQ(std::string("UsedBy"), knob->getColumnTag(2));

    std::list<std::vector<std::string>> rows;
    {
        std::vector<std::string> row;
        row.push_back("Color");
        row.push_back("R G B A");
        row.push_back("0");
        rows.push_back(row);
    }
    {
        std::vector<std::string> row;
        row.push_back("diffuse");
        row.push_back("R G B");
        row.push_back("1");
        rows.push_back(row);
    }

    QLocale savedLocale;
    QLocale::setDefault(QLocale(QLocale::French));
    std::string encoded = knob->encodeToKnobTableFormat(rows);
    QLocale::setDefault(savedLocale);

    EXPECT_NE(std::string::npos, encoded.find("<Name>Color</Name>"));
    EXPECT_NE(std::string::npos, encoded.find("<Channels>R G B A</Channels>"));
    EXPECT_NE(std::string::npos, encoded.find("<UsedBy>0</UsedBy>"));
    EXPECT_EQ(std::string::npos, encoded.find("<Layer>"));
    EXPECT_EQ(std::string::npos, encoded.find("Used by"));

    std::list<std::vector<std::string>> decoded;
    knob->decodeFromKnobTableFormat(encoded, &decoded);

    ASSERT_EQ(rows.size(), decoded.size());
    std::list<std::vector<std::string>>::const_iterator expectedIt = rows.begin();
    std::list<std::vector<std::string>>::const_iterator actualIt = decoded.begin();
    for (; expectedIt != rows.end(); ++expectedIt, ++actualIt) {
        EXPECT_EQ(*expectedIt, *actualIt);
    }
}

TEST(Knob, TableKnobRefusesExpressions)
{
    KnobLayersPtr knob = std::make_shared<KnobLayers>(static_cast<KnobHolder*>(NULL), std::string("layers"), 1, false);

    EXPECT_THROW(knob->setExpression(0, "1", false, true), std::invalid_argument);
}

TEST_F(BaseTest, ScalarKnobAcceptsExpressions)
{
    ProjectPtr project = getApp()->getProject();
    KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(project->getKnobByName("frameRate"));

    ASSERT_TRUE(bool(knob));
    EXPECT_NO_THROW(knob->setExpression(0, "1", false, true));
}

TEST_F(BaseTest, DuplicateOnHolderAndAliasKnobLayers)
{
    NodePtr nodeA = createNode(_generatorPluginID);
    NodePtr nodeB = createNode(_generatorPluginID);

    ASSERT_TRUE(bool(nodeA));
    ASSERT_TRUE(bool(nodeB));

    KnobLayersPtr source = AppManager::createKnob<KnobLayers>(nodeA->getEffectInstance().get(), std::string("layers"), 1, false);
    ASSERT_TRUE(bool(source));
    source->setName("layers");

    std::list<std::vector<std::string>> rows;
    std::vector<std::string> row;
    row.push_back("testLayer");
    row.push_back("R G B");
    row.push_back("0");
    rows.push_back(row);
    source->setTable(rows);

    KnobIPtr duplicate = source->createDuplicateOnHolder(nodeB->getEffectInstance().get(),
                                                         KnobPagePtr(),
                                                         KnobGroupPtr(),
                                                         -1,
                                                         false,
                                                         "layersDuplicate",
                                                         "Layers Duplicate",
                                                         "",
                                                         false,
                                                         true);

    ASSERT_TRUE(bool(duplicate));
    KnobIPtr sourceAsKnobI = source;
    EXPECT_EQ(sourceAsKnobI->typeName(), duplicate->typeName());

    KnobLayersPtr duplicateLayers = std::dynamic_pointer_cast<KnobLayers>(duplicate);
    ASSERT_TRUE(bool(duplicateLayers));
    EXPECT_EQ(source->getValue(), duplicateLayers->getValue());

    EXPECT_TRUE(source->setKnobAsAliasOfThis(duplicate, true));
}
