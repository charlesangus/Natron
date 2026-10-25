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

#include <list>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/PrecompNode.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/ViewIdx.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Yaml/PresetYaml.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING

namespace {

const char* const kOwnUserKnob = "ownUser";
const char* const kAddedUserKnob = "addedUser";

class NodePresetApply
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createBlur()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    }

    static Serialization::NodeSerialization snapshot(const NodePtr& node)
    {
        Serialization::NodeSerialization serialization;
        node->toSerialization(&serialization, false);

        return serialization;
    }

    static std::string yamlOf(const Serialization::NodeSerialization& serialization)
    {
        std::list<Serialization::NodeSerializationPtr> nodes;
        nodes.push_back(std::make_shared<Serialization::NodeSerialization>(serialization));

        return Serialization::encodePresetYaml(nodes);
    }

    // What a preset file holds, read back the way Import presets reads it.
    static Serialization::NodeSerialization presetFileOf(const Serialization::NodeSerialization& serialization)
    {
        std::list<Serialization::NodeSerializationPtr> decoded = Serialization::decodePresetYaml(yamlOf(serialization));
        EXPECT_EQ((std::size_t)1, decoded.size());

        return decoded.empty() ? Serialization::NodeSerialization() : *decoded.front();
    }

    static bool carries(const Serialization::NodeSerialization& preset,
                        const std::string& name)
    {
        for (std::list<Serialization::KnobSerialization>::const_iterator it = preset.knobs.begin(); it != preset.knobs.end(); ++it) {
            if (it->name == name) {
                return true;
            }
        }

        return false;
    }

    static bool carriesUserKnob(const Serialization::NodeSerialization& preset,
                                const std::string& name)
    {
        for (std::list<Serialization::GroupKnobSerialization>::const_iterator page = preset.userPages.begin(); page != preset.userPages.end(); ++page) {
            for (std::list<Serialization::GroupKnobChildSerialization>::const_iterator it = page->children.begin(); it != page->children.end(); ++it) {
                if (!it->isGroup && (it->knob.name == name)) {
                    return true;
                }
            }
        }

        return false;
    }

    template <class K>
    static std::shared_ptr<K> knob(const NodePtr& node,
                                   const std::string& name)
    {
        return std::dynamic_pointer_cast<K>(node->getKnobByName(name));
    }

    static void setBothDimensions(const KnobDoublePtr& knob,
                                  double value)
    {
        ASSERT_TRUE(bool(knob));
        for (int i = 0; i < knob->getDimension(); ++i) {
            knob->setValue(value, ViewSpec::all(), i);
        }
    }

    static KnobDoublePtr addUserDouble(const NodePtr& node,
                                       const std::string& name,
                                       double value)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        KnobDoublePtr added = effect->createDoubleKnob(name, name, 1);
        if (added) {
            effect->getOrCreateUserPageKnob()->addKnob(added);
            added->setValue(value);
        }

        return added;
    }

    // Whether the node's Python object has an attribute for the parameter, which the interpreter
    // reports by raising when it is missing.
    bool pythonExposes(const NodePtr& node,
                       const std::string& param)
    {
        const std::string nodeObj = getApp()->getAppIDString() + "." + node->getFullyQualifiedName();
        const std::string script = "if not hasattr(" + nodeObj + ", '" + param + "'):\n    raise AttributeError('" + param + "')\n";
        std::string error;
        std::string output;

        return NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &error, &output);
    }

    // Modifies every kind of state a preset apply must clear: keyframes, an expression, a link
    // and a plain value, plus the exceptions a preset leaves alone.
    void modifyTarget(const NodePtr& target,
                      const NodePtr& linkSource)
    {
        KnobDoublePtr size = knob<KnobDouble>(target, kBlurParamSize);
        KnobBoolPtr crop = knob<KnobBool>(target, kBlurParamCropToFormat);
        KnobChoicePtr filter = knob<KnobChoice>(target, kBlurParamFilter);
        KnobBoolPtr sourceCrop = knob<KnobBool>(linkSource, kBlurParamCropToFormat);
        KnobBoolPtr disable = knob<KnobBool>(target, kDisableNodeKnobName);
        KnobStringPtr label = knob<KnobString>(target, kUserLabelKnobName);
        ASSERT_TRUE(size && crop && filter && sourceCrop && disable && label);

        size->setValueAtTime(1, 5., ViewSpec::all(), 0);
        size->setValueAtTime(10, 20., ViewSpec::all(), 0);
        ASSERT_NO_THROW(size->setExpression(1, "3.5", false, true));
        sourceCrop->setValue(!sourceCrop->getDefaultValue(0));
        ASSERT_TRUE(crop->slaveTo(0, sourceCrop, 0));
        ASSERT_GT(filter->getNumEntries(), 1);
        filter->setValue((filter->getDefaultValue(0) + 1) % filter->getNumEntries());
        disable->setValue(true);
        label->setValue(std::string("kept label"));

        ASSERT_EQ(2, size->getKeyFramesCount(ViewSpec::current(), 0));
        ASSERT_EQ(std::string("3.5"), size->getExpression(1));
        ASSERT_TRUE(crop->isSlave(0));
    }
};

} // namespace

TEST_F(NodePresetApply, APresetFromADefaultNodeResetsKeyframesExpressionsLinksAndValues)
{
    NodePtr source = createBlur();
    NodePtr linkSource = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(source && linkSource && target);
    ASSERT_NO_FATAL_FAILURE(modifyTarget(target, linkSource));

    const Serialization::NodeSerialization preset = presetFileOf(snapshot(source));
    std::list<KnobIPtr> created;
    target->applyPresetSerialization(preset, Node::ePresetApplyModePreset, &created);

    KnobDoublePtr size = knob<KnobDouble>(target, kBlurParamSize);
    KnobBoolPtr crop = knob<KnobBool>(target, kBlurParamCropToFormat);
    KnobChoicePtr filter = knob<KnobChoice>(target, kBlurParamFilter);
    for (int i = 0; i < size->getDimension(); ++i) {
        EXPECT_EQ(0, size->getKeyFramesCount(ViewSpec::current(), i)) << "dimension " << i;
        EXPECT_TRUE(size->getExpression(i).empty()) << "dimension " << i;
        EXPECT_DOUBLE_EQ(size->getDefaultValue(i), size->getValue(i)) << "dimension " << i;
    }
    EXPECT_FALSE(crop->isSlave(0));
    EXPECT_EQ(crop->getDefaultValue(0), crop->getValue());
    EXPECT_EQ(filter->getDefaultValue(0), filter->getValue());
    EXPECT_TRUE(created.empty());
}

TEST_F(NodePresetApply, APresetKeepsTheLabelDisableSwitchAndUserKnobsItCarriesOtherValuesFor)
{
    NodePtr source = createBlur();
    NodePtr linkSource = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(source && linkSource && target);
    ASSERT_NO_FATAL_FAILURE(modifyTarget(target, linkSource));
    knob<KnobBool>(source, kDisableNodeKnobName)->setValue(true);
    knob<KnobBool>(target, kDisableNodeKnobName)->setValue(false);
    knob<KnobString>(source, kUserLabelKnobName)->setValue(std::string("preset label"));
    ASSERT_TRUE(bool(addUserDouble(source, kOwnUserKnob, 7.)));
    KnobDoublePtr own = addUserDouble(target, kOwnUserKnob, 2.);
    ASSERT_TRUE(bool(own));

    const Serialization::NodeSerialization preset = presetFileOf(snapshot(source));
    ASSERT_TRUE(carries(preset, kDisableNodeKnobName));
    ASSERT_TRUE(carries(preset, kUserLabelKnobName));
    ASSERT_TRUE(carriesUserKnob(preset, kOwnUserKnob));
    std::list<KnobIPtr> created;
    target->applyPresetSerialization(preset, Node::ePresetApplyModePreset, &created);

    EXPECT_FALSE(knob<KnobBool>(target, kDisableNodeKnobName)->getValue());
    EXPECT_EQ(std::string("kept label"), knob<KnobString>(target, kUserLabelKnobName)->getValue());
    KnobDoublePtr ownAfter = knob<KnobDouble>(target, kOwnUserKnob);
    ASSERT_TRUE(bool(ownAfter));
    EXPECT_EQ(own, ownAfter);
    EXPECT_DOUBLE_EQ(2., ownAfter->getValue());
    EXPECT_TRUE(created.empty());
}

TEST_F(NodePresetApply, APresetKeepsAWritersFileSublabelLabelAndDisableSwitch)
{
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE));
    NodePtr target = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE));
    ASSERT_TRUE(source && target);
    KnobOutputFilePtr sourceFile = knob<KnobOutputFile>(source, kOfxImageEffectFileParamName);
    KnobOutputFilePtr file = knob<KnobOutputFile>(target, kOfxImageEffectFileParamName);
    KnobStringPtr sourceSublabel = knob<KnobString>(source, kNatronOfxParamStringSublabelName);
    KnobStringPtr sublabel = knob<KnobString>(target, kNatronOfxParamStringSublabelName);
    ASSERT_TRUE(sourceFile && file && sourceSublabel && sublabel);
    sourceFile->setValue(std::string("/tmp/preset_source.####.exr"));
    sourceSublabel->setValue(std::string("preset sublabel"));
    knob<KnobString>(source, kUserLabelKnobName)->setValue(std::string("preset label"));
    knob<KnobBool>(source, kDisableNodeKnobName)->setValue(true);
    file->setValue(std::string("/tmp/preset_target.####.exr"));
    sublabel->setValue(std::string("target sublabel"));
    knob<KnobString>(target, kUserLabelKnobName)->setValue(std::string("target label"));

    const Serialization::NodeSerialization preset = presetFileOf(snapshot(source));
    ASSERT_TRUE(carries(preset, kOfxImageEffectFileParamName));
    ASSERT_TRUE(carries(preset, kNatronOfxParamStringSublabelName));
    ASSERT_TRUE(carries(preset, kUserLabelKnobName));
    ASSERT_TRUE(carries(preset, kDisableNodeKnobName));
    target->applyPresetSerialization(preset, Node::ePresetApplyModePreset, NULL);

    EXPECT_EQ(std::string("/tmp/preset_target.####.exr"), file->getValue());
    EXPECT_EQ(std::string("target sublabel"), sublabel->getValue());
    EXPECT_EQ(std::string("target label"), knob<KnobString>(target, kUserLabelKnobName)->getValue());
    EXPECT_FALSE(knob<KnobBool>(target, kDisableNodeKnobName)->getValue());
}

TEST_F(NodePresetApply, APresetKeepsAPrecompsProjectFileAndUndoingWithTheSnapshotRestoresTheNodeExactly)
{
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_PRECOMP));
    NodePtr target = createNode(QString::fromUtf8(PLUGINID_NATRON_PRECOMP));
    ASSERT_TRUE(source && target);
    KnobFilePtr sourceFile = knob<KnobFile>(source, kPrecompParamProjectFilename);
    KnobFilePtr file = knob<KnobFile>(target, kPrecompParamProjectFilename);
    KnobChoicePtr sourceOnError = knob<KnobChoice>(source, "onError");
    ASSERT_TRUE(sourceFile && file && sourceOnError);
    ASSERT_GT(sourceOnError->getNumEntries(), 1);
    sourceFile->setValue(std::string("/tmp/preset_source.ntp"));
    sourceOnError->setValue((sourceOnError->getDefaultValue(0) + 1) % sourceOnError->getNumEntries());
    file->setValue(std::string("/tmp/preset_target.ntp"));

    const Serialization::NodeSerialization before = snapshot(target);
    const Serialization::NodeSerialization preset = presetFileOf(snapshot(source));
    ASSERT_TRUE(carries(preset, kPrecompParamProjectFilename));
    target->applyPresetSerialization(preset, Node::ePresetApplyModePreset, NULL);

    EXPECT_EQ(std::string("/tmp/preset_target.ntp"), file->getValue());
    EXPECT_EQ(sourceOnError->getValue(), knob<KnobChoice>(target, "onError")->getValue());

    target->applyPresetSerialization(before, Node::ePresetApplyModeSnapshot, NULL);
    EXPECT_EQ(yamlOf(before), yamlOf(snapshot(target)));
}

TEST_F(NodePresetApply, APresetKeepsTheRotoShapesAndUndoingWithTheSnapshotRestoresThem)
{
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    NodePtr target = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(source && target);
    RotoContextPtr sourceRoto = source->getRotoContext();
    RotoContextPtr roto = target->getRotoContext();
    ASSERT_TRUE(sourceRoto && roto);
    ASSERT_TRUE(bool(sourceRoto->makeEllipse(10., 10., 20., true, 0.)));
    ASSERT_TRUE(bool(sourceRoto->makeEllipse(50., 50., 20., true, 0.)));
    ASSERT_TRUE(bool(roto->makeEllipse(100., 100., 40., true, 0.)));
    const std::list<RotoDrawableItemPtr> shapes = roto->getCurvesByRenderOrder(false);
    ASSERT_EQ((std::size_t)1, shapes.size());

    const Serialization::NodeSerialization before = snapshot(target);
    const Serialization::NodeSerialization preset = presetFileOf(snapshot(source));
    ASSERT_TRUE(preset.hasRotoContext);
    target->applyPresetSerialization(preset, Node::ePresetApplyModePreset, NULL);

    EXPECT_EQ(shapes, roto->getCurvesByRenderOrder(false));

    target->applyPresetSerialization(before, Node::ePresetApplyModeSnapshot, NULL);
    EXPECT_EQ(yamlOf(before), yamlOf(snapshot(target)));
}

TEST_F(NodePresetApply, ASparsePresetSetsItsKnobsAndResetsTheRest)
{
    NodePtr source = createBlur();
    NodePtr linkSource = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(source && linkSource && target);
    setBothDimensions(knob<KnobDouble>(source, kBlurParamSize), 4.);
    ASSERT_NO_FATAL_FAILURE(modifyTarget(target, linkSource));

    Serialization::NodeSerialization sparse = snapshot(source);
    sparse.knobs.remove_if([](const Serialization::KnobSerialization& k) { return k.name != kBlurParamSize; });
    ASSERT_EQ((std::size_t)1, sparse.knobs.size());
    target->applyPresetSerialization(presetFileOf(sparse), Node::ePresetApplyModePreset, NULL);

    KnobDoublePtr size = knob<KnobDouble>(target, kBlurParamSize);
    for (int i = 0; i < size->getDimension(); ++i) {
        EXPECT_DOUBLE_EQ(4., size->getValue(i)) << "dimension " << i;
        EXPECT_EQ(0, size->getKeyFramesCount(ViewSpec::current(), i)) << "dimension " << i;
        EXPECT_TRUE(size->getExpression(i).empty()) << "dimension " << i;
    }
    EXPECT_FALSE(knob<KnobBool>(target, kBlurParamCropToFormat)->isSlave(0));
    KnobChoicePtr filter = knob<KnobChoice>(target, kBlurParamFilter);
    EXPECT_EQ(filter->getDefaultValue(0), filter->getValue());
}

TEST_F(NodePresetApply, APresetAddsItsUserKnobsAndUndoingWithTheSnapshotRestoresTheNodeExactly)
{
    NodePtr source = createBlur();
    NodePtr linkSource = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(source && linkSource && target);
    setBothDimensions(knob<KnobDouble>(source, kBlurParamSize), 9.);
    ASSERT_TRUE(bool(addUserDouble(source, kAddedUserKnob, 6.)));
    ASSERT_NO_FATAL_FAILURE(modifyTarget(target, linkSource));
    ASSERT_TRUE(bool(addUserDouble(target, kOwnUserKnob, 2.)));

    const Serialization::NodeSerialization before = snapshot(target);
    const std::string beforeYaml = yamlOf(before);

    std::list<KnobIPtr> created;
    target->applyPresetSerialization(presetFileOf(snapshot(source)), Node::ePresetApplyModePreset, &created);
    KnobDoublePtr added = knob<KnobDouble>(target, kAddedUserKnob);
    ASSERT_TRUE(bool(added));
    EXPECT_DOUBLE_EQ(6., added->getValue());
    ASSERT_EQ((std::size_t)1, created.size());
    EXPECT_EQ(KnobIPtr(added), created.front());
    EXPECT_TRUE(pythonExposes(target, kAddedUserKnob));
    EXPECT_DOUBLE_EQ(9., knob<KnobDouble>(target, kBlurParamSize)->getValue(0));
    EXPECT_NE(beforeYaml, yamlOf(snapshot(target)));

    std::list<KnobIWPtr> toDelete(created.begin(), created.end());
    added.reset();
    created.clear();
    target->deleteUserKnobs(toDelete);
    target->applyPresetSerialization(before, Node::ePresetApplyModeSnapshot, NULL);

    EXPECT_FALSE(bool(target->getKnobByName(kAddedUserKnob)));
    EXPECT_FALSE(pythonExposes(target, kAddedUserKnob));
    KnobDoublePtr own = knob<KnobDouble>(target, kOwnUserKnob);
    ASSERT_TRUE(bool(own));
    EXPECT_DOUBLE_EQ(2., own->getValue());
    KnobDoublePtr size = knob<KnobDouble>(target, kBlurParamSize);
    EXPECT_EQ(2, size->getKeyFramesCount(ViewSpec::current(), 0));
    EXPECT_EQ(std::string("3.5"), size->getExpression(1));
    KnobBoolPtr crop = knob<KnobBool>(target, kBlurParamCropToFormat);
    ASSERT_TRUE(crop->isSlave(0));
    EXPECT_EQ(knob<KnobBool>(linkSource, kBlurParamCropToFormat), crop->getMaster(0).second);
    EXPECT_EQ(beforeYaml, yamlOf(snapshot(target)));
}

TEST_F(NodePresetApply, ApplyingAFreshSnapshotOfANodeChangesNothing)
{
    NodePtr linkSource = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(linkSource && target);
    ASSERT_NO_FATAL_FAILURE(modifyTarget(target, linkSource));
    ASSERT_TRUE(bool(addUserDouble(target, kOwnUserKnob, 2.)));

    const Serialization::NodeSerialization before = snapshot(target);
    std::list<KnobIPtr> created;
    target->applyPresetSerialization(before, Node::ePresetApplyModeSnapshot, &created);

    EXPECT_TRUE(created.empty());
    EXPECT_EQ(yamlOf(before), yamlOf(snapshot(target)));
}
