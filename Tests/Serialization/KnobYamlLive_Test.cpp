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
#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/Yaml/KnobYaml.h"

NATRON_NAMESPACE_USING

// The YAML encoder writes the knobs a node's toSerialization() keeps, so a knob left at its
// default never reaches the file, and an edited one restores through the usual path once
// completed with the live knob's type.
TEST_F(BaseTest, KnobYamlLeavesDefaultValuedKnobsOut)
{
    NodePtr node = createNode(_generatorPluginID);
    NodePtr target = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node) && bool(target));

    KnobHolder* holder = node->getEffectInstance().get();
    KnobIntPtr untouched = AppManager::createKnob<KnobInt>(holder, std::string("untouchedInt"), 1, false);
    KnobIntPtr edited = AppManager::createKnob<KnobInt>(holder, std::string("editedInt"), 1, false);
    ASSERT_TRUE(bool(untouched) && bool(edited));
    untouched->setName("untouchedInt");
    edited->setName("editedInt");
    edited->setValue(7);

    Serialization::NodeSerialization saved;
    node->toSerialization(&saved);
    const std::string yaml = Serialization::encodeKnobsYaml(saved.knobs);
    EXPECT_EQ(std::string::npos, yaml.find("untouchedInt")) << yaml;
    EXPECT_NE(std::string::npos, yaml.find("editedInt: 7\n")) << yaml;

    std::list<Serialization::KnobSerialization> decoded = Serialization::decodeKnobsYaml(yaml);
    Serialization::KnobSerialization* editedData = nullptr;
    for (Serialization::KnobSerialization& knob : decoded) {
        if (knob.name == "editedInt") {
            editedData = &knob;
        }
    }
    ASSERT_TRUE(editedData != nullptr);
    Serialization::completeKnobSerialization(editedData, KnobInt::typeNameStatic(), edited->getDimension());

    KnobIntPtr restored = AppManager::createKnob<KnobInt>(target->getEffectInstance().get(), std::string("editedInt"), 1, false);
    ASSERT_TRUE(bool(restored));
    restored->setName("editedInt");
    EXPECT_TRUE(restored->fromSerialization(*editedData));
    EXPECT_EQ(7, restored->getValue());
}
