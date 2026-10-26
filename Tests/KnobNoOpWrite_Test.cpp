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

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Curve.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// A metadata derive is cached only if no knob write began or was in flight while it ran and no
// hash mark reached its node. Writing a knob the value it already has must leave both untouched,
// or a plug-in that re-sets its parameters from its clip preferences makes every derive around
// it uncacheable.

namespace {

// What a derive samples when it starts, and checks before caching its answer.
class DeriveWindow {
public:
    explicit DeriveWindow(const NodePtr& node)
        : _node(node)
        , _writes(KnobContentWrite::snapshot())
        , _marks(node->getHashMarkCount())
    {
    }

    bool settled() const
    {
        return KnobContentWrite::isSettled(_writes);
    }

    bool marked() const
    {
        return _node->getHashMarkCount() != _marks;
    }

private:
    NodePtr _node;
    U64 _writes;
    U64 _marks;
};

} // namespace

TEST_F(BaseTest, SameValueSetValueIsNoWrite)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(node->getEffectInstance().get(), std::string("noOpDouble"), 2, false);
    ASSERT_TRUE(bool(knob));
    knob->setValue(1.5, ViewSpec::all(), 0);
    knob->setValue(2.5, ViewSpec::all(), 1);
    const U64 hash = knob->getContentHash();

    {
        DeriveWindow window(node);
        EXPECT_EQ(KnobHelper::eValueChangedReturnCodeNothingChanged, knob->setValue(1.5, ViewSpec::all(), 0));
        EXPECT_EQ(KnobHelper::eValueChangedReturnCodeNothingChanged, knob->setValueFromPlugin(2.5, ViewSpec::all(), 1));
        knob->setValues(1.5, 2.5, ViewSpec::all(), eValueChangedReasonPluginEdited);
        EXPECT_TRUE(window.settled());
        EXPECT_FALSE(window.marked());
    }
    EXPECT_EQ(hash, knob->getContentHash());

    {
        DeriveWindow window(node);
        EXPECT_NE(KnobHelper::eValueChangedReturnCodeNothingChanged, knob->setValue(3.5, ViewSpec::all(), 0));
        EXPECT_FALSE(window.settled());
        EXPECT_TRUE(window.marked());
    }
    EXPECT_NE(hash, knob->getContentHash());
    EXPECT_EQ(3.5, knob->getValue(0));
}

TEST_F(BaseTest, SameValueStringSetValueIsNoWrite)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobStringPtr knob = AppManager::createKnob<KnobString>(node->getEffectInstance().get(), std::string("noOpString"), 1, false);
    ASSERT_TRUE(bool(knob));
    knob->setValue(std::string("same"));

    {
        DeriveWindow window(node);
        EXPECT_EQ(KnobHelper::eValueChangedReturnCodeNothingChanged, knob->setValue(std::string("same")));
        EXPECT_TRUE(window.settled());
        EXPECT_FALSE(window.marked());
    }
    {
        DeriveWindow window(node);
        knob->setValue(std::string("other"));
        EXPECT_FALSE(window.settled());
        EXPECT_TRUE(window.marked());
    }
    EXPECT_EQ(std::string("other"), knob->getValue());
}

TEST_F(BaseTest, ForcedSameValueSetValueStillNotifies)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobIntPtr knob = AppManager::createKnob<KnobInt>(node->getEffectInstance().get(), std::string("noOpForced"), 1, false);
    ASSERT_TRUE(bool(knob));
    knob->setValue(4);

    // A caller passing hasChanged, as a menu re-selecting its current entry does, still gets the
    // change reported though the value is the same.
    EXPECT_NE(KnobHelper::eValueChangedReturnCodeNothingChanged,
              knob->setValue(4, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL, true));
    EXPECT_EQ(4, knob->getValue());
}

TEST_F(BaseTest, SameKeyframeSetValueAtTimeIsNoWrite)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(node->getEffectInstance().get(), std::string("noOpKeys"), 1, false);
    ASSERT_TRUE(bool(knob));
    knob->setValueAtTime(10, 3., ViewSpec::all(), 0);
    knob->setValueAtTime(20, 5., ViewSpec::all(), 0);
    const U64 hash = knob->getContentHash();

    {
        DeriveWindow window(node);
        KeyFrame key;
        EXPECT_EQ(KnobHelper::eValueChangedReturnCodeNothingChanged,
                  knob->setValueAtTime(10, 3., ViewSpec::all(), 0, eValueChangedReasonPluginEdited, &key));
        EXPECT_TRUE(window.settled());
        EXPECT_FALSE(window.marked());
    }
    EXPECT_EQ(hash, knob->getContentHash());

    // The same value over a key of another interpolation resets it, so it is a write.
    KeyFrame changed;
    ASSERT_TRUE(knob->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, 10, eKeyframeTypeConstant, &changed));
    const U64 constantHash = knob->getContentHash();
    {
        DeriveWindow window(node);
        knob->setValueAtTime(10, 3., ViewSpec::all(), 0);
        EXPECT_FALSE(window.settled());
        EXPECT_TRUE(window.marked());
    }
    EXPECT_NE(constantHash, knob->getContentHash());

    {
        DeriveWindow window(node);
        knob->setValueAtTime(10, 4., ViewSpec::all(), 0);
        EXPECT_FALSE(window.settled());
        EXPECT_TRUE(window.marked());
    }
    EXPECT_EQ(4., knob->getValueAtTime(10, 0));
}

TEST_F(BaseTest, SameChoiceIsNoWrite)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(node->getEffectInstance().get(), std::string("noOpChoice"), 1, false);
    ASSERT_TRUE(bool(knob));
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("a", "A", ""));
    options.push_back(ChoiceOption("b", "B", ""));
    options.push_back(ChoiceOption("c", "C", ""));
    knob->populateChoices(options);
    knob->setValue(1);

    {
        DeriveWindow window(node);
        EXPECT_FALSE(knob->populateChoices(options));
        knob->setValueFromID("b", 0);
        knob->setValue(1);
        EXPECT_TRUE(window.settled());
        EXPECT_FALSE(window.marked());
    }
    EXPECT_EQ(std::string("b"), knob->getActiveEntry().id);

    {
        DeriveWindow window(node);
        knob->setValueFromID("c", 0);
        EXPECT_FALSE(window.settled());
        EXPECT_TRUE(window.marked());
    }
    EXPECT_EQ(2, knob->getValue());
    EXPECT_EQ(std::string("c"), knob->getActiveEntry().id);
}
