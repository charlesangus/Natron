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

#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Serialization/KnobSerialization.h"
#include "Serialization/BoostXml/KnobXml.h"

NATRON_NAMESPACE_USING

namespace {

// Every type a project file can create a knob of.
const char* const kKnobTypeNames[] = {
    "Int", "Bool", "Double", "Choice", "String", "Parametric", "Color", "Path", "Layers", "ChannelSet",
    "LayerSelect", "ChannelSelect", "ShuffleMap", "InputFile", "OutputFile", "Button", "Separator", "Group", "Page"
};

enum KnobVariantEnum
{
    eKnobVariantStatic = 0,
    eKnobVariantAnimated,
    eKnobVariantExpression,
    eKnobVariantLinked,
    eKnobVariantUserKnob
};

const KnobVariantEnum kKnobVariants[] = {
    eKnobVariantStatic, eKnobVariantAnimated, eKnobVariantExpression, eKnobVariantLinked, eKnobVariantUserKnob
};

std::string
variantName(KnobVariantEnum variant)
{
    switch (variant) {
    case eKnobVariantStatic:
        return "Static";
    case eKnobVariantAnimated:
        return "Animated";
    case eKnobVariantExpression:
        return "Expression";
    case eKnobVariantLinked:
        return "Linked";
    case eKnobVariantUserKnob:
        return "UserKnob";
    }

    return std::string();
}

// Double is two-dimensional so that its user-knob form saves HasOverlayHandle.
int
dimensionForKnobType(const std::string& typeName)
{
    if ( typeName == KnobDouble::typeNameStatic() ) {
        return 2;
    } else if ( typeName == KnobColor::typeNameStatic() ) {
        return 3;
    }

    return 1;
}

KnobIPtr
createKnobOfType(KnobHolder* holder,
                 const std::string& typeName,
                 const std::string& name)
{
    const int dimension = dimensionForKnobType(typeName);
    KnobIPtr knob;

    if ( typeName == KnobInt::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobInt>(holder, name, dimension, false);
    } else if ( typeName == KnobBool::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobBool>(holder, name, dimension, false);
    } else if ( typeName == KnobDouble::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobDouble>(holder, name, dimension, false);
    } else if ( typeName == KnobChoice::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobChoice>(holder, name, dimension, false);
    } else if ( typeName == KnobString::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobString>(holder, name, dimension, false);
    } else if ( typeName == KnobParametric::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobParametric>(holder, name, dimension, false);
    } else if ( typeName == KnobColor::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobColor>(holder, name, dimension, false);
    } else if ( typeName == KnobPath::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobPath>(holder, name, dimension, false);
    } else if ( typeName == KnobLayers::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobLayers>(holder, name, dimension, false);
    } else if ( typeName == KnobChannelSet::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobChannelSet>(holder, name, dimension, false);
    } else if ( typeName == KnobLayerSelect::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobLayerSelect>(holder, name, dimension, false);
    } else if ( typeName == KnobChannelSelect::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobChannelSelect>(holder, name, dimension, false);
    } else if ( typeName == KnobShuffleMap::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobShuffleMap>(holder, name, dimension, false);
    } else if ( typeName == KnobFile::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobFile>(holder, name, dimension, false);
    } else if ( typeName == KnobOutputFile::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobOutputFile>(holder, name, dimension, false);
    } else if ( typeName == KnobButton::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobButton>(holder, name, dimension, false);
    } else if ( typeName == KnobSeparator::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobSeparator>(holder, name, dimension, false);
    } else if ( typeName == KnobGroup::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobGroup>(holder, name, dimension, false);
    } else if ( typeName == KnobPage::typeNameStatic() ) {
        knob = AppManager::createKnob<KnobPage>(holder, name, dimension, false);
    }
    if (knob) {
        knob->setName(name);
    }

    return knob;
}

// What a plug-in declares for a knob before any value is set: the menu of a choice knob.
void
defineKnob(const KnobIPtr& knob)
{
    KnobChoice* isChoice = dynamic_cast<KnobChoice*>( knob.get() );
    if (isChoice) {
        std::vector<ChoiceOption> options;
        options.push_back( ChoiceOption("alpha", "Alpha", "The first entry") );
        options.push_back( ChoiceOption("beta", "Beta", "The second entry") );
        options.push_back( ChoiceOption("gamma", "Gamma", "The third entry") );
        isChoice->populateChoices(options);
    }
}

// The definition a user gives a knob created from the user-knob dialog.
void
makeUserKnob(const KnobIPtr& knob)
{
    knob->setAsUserKnob(true);
    knob->setHintToolTip( std::string("A user knob") );

    if ( KnobString* isString = dynamic_cast<KnobString*>( knob.get() ) ) {
        isString->setAsMultiLine();
        isString->setUsesRichText(true);
    } else if ( KnobFile* isFile = dynamic_cast<KnobFile*>( knob.get() ) ) {
        isFile->setAsInputImage();
    } else if ( KnobOutputFile* isOutputFile = dynamic_cast<KnobOutputFile*>( knob.get() ) ) {
        isOutputFile->setAsOutputImageFile();
    } else if ( KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>( knob.get() ) ) {
        isLayerSelect->setWithChannelButtons(true);
    } else if ( KnobDouble* isDouble = dynamic_cast<KnobDouble*>( knob.get() ) ) {
        std::vector<double> minimums(isDouble->getDimension(), -5.), maximums(isDouble->getDimension(), 5.);
        std::vector<double> displayMinimums(isDouble->getDimension(), -2.), displayMaximums(isDouble->getDimension(), 2.);
        isDouble->setMinimumsAndMaximums(minimums, maximums);
        isDouble->setDisplayMinimumsAndMaximums(displayMinimums, displayMaximums);
    }
}

// Values that differ from every type's default.
void
setDistinctiveValues(const KnobIPtr& knob)
{
    const std::string& typeName = knob->typeName();

    if ( KnobChoice* isChoice = dynamic_cast<KnobChoice*>( knob.get() ) ) {
        isChoice->setValue(2);
    } else if ( KnobIntBase* isInt = dynamic_cast<KnobIntBase*>( knob.get() ) ) {
        for (int i = 0; i < isInt->getDimension(); ++i) {
            isInt->setValue(7 + i, ViewSpec::all(), i);
        }
    } else if ( typeName == KnobBool::typeNameStatic() ) {
        KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>( knob.get() );
        isBool->setValue(true);
    } else if ( KnobParametric* isParametric = dynamic_cast<KnobParametric*>( knob.get() ) ) {
        StatusEnum status = isParametric->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 0.25, 0.75);
        EXPECT_EQ(eStatusOK, status);
    } else if ( KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>( knob.get() ) ) {
        for (int i = 0; i < isDouble->getDimension(); ++i) {
            isDouble->setValue(0.25 + i, ViewSpec::all(), i);
        }
    } else if ( KnobChannelSet* isChannelSet = dynamic_cast<KnobChannelSet*>( knob.get() ) ) {
        isChannelSet->addRegex("spec.*");
    } else if ( KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>( knob.get() ) ) {
        isLayerSelect->setLayer("diffuse");
    } else if ( (typeName == KnobString::typeNameStatic()) || (typeName == KnobFile::typeNameStatic()) ||
                (typeName == KnobOutputFile::typeNameStatic()) || (typeName == KnobPath::typeNameStatic()) ) {
        KnobStringBase* isString = dynamic_cast<KnobStringBase*>( knob.get() );
        isString->setValue( std::string("saved value") );
    }
}

bool
isAnimatableValueType(const std::string& typeName)
{
    return (typeName == KnobInt::typeNameStatic()) || (typeName == KnobChoice::typeNameStatic()) ||
           (typeName == KnobBool::typeNameStatic()) || (typeName == KnobDouble::typeNameStatic()) ||
           (typeName == KnobColor::typeNameStatic()) || (typeName == KnobString::typeNameStatic());
}

void
animateKnob(const KnobIPtr& knob)
{
    if ( KnobChoice* isChoice = dynamic_cast<KnobChoice*>( knob.get() ) ) {
        isChoice->setValueAtTime(10, 0, ViewSpec::all(), 0);
        isChoice->setValueAtTime(20, 2, ViewSpec::all(), 0);
    } else if ( KnobIntBase* isInt = dynamic_cast<KnobIntBase*>( knob.get() ) ) {
        for (int i = 0; i < isInt->getDimension(); ++i) {
            isInt->setValueAtTime(10, 1 + i, ViewSpec::all(), i);
            isInt->setValueAtTime(20, 9 + i, ViewSpec::all(), i);
        }
    } else if ( KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>( knob.get() ) ) {
        isBool->setValueAtTime(10, true, ViewSpec::all(), 0);
        isBool->setValueAtTime(20, false, ViewSpec::all(), 0);
    } else if ( KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>( knob.get() ) ) {
        for (int i = 0; i < isDouble->getDimension(); ++i) {
            isDouble->setValueAtTime(10, 0.5 + i, ViewSpec::all(), i);
            isDouble->setValueAtTime(20, 1.5 + i, ViewSpec::all(), i);
        }
    } else if ( KnobStringBase* isString = dynamic_cast<KnobStringBase*>( knob.get() ) ) {
        isString->setValueAtTime(10, std::string("first key"), ViewSpec::all(), 0);
        isString->setValueAtTime(20, std::string("second key"), ViewSpec::all(), 0);
    }
}

bool
variantApplies(const std::string& typeName,
               KnobVariantEnum variant,
               const KnobIPtr& knob)
{
    switch (variant) {
    case eKnobVariantStatic:
        return true;
    case eKnobVariantAnimated:
        return isAnimatableValueType(typeName) && knob->canAnimate();
    case eKnobVariantExpression:
        return (typeName == KnobInt::typeNameStatic()) || (typeName == KnobDouble::typeNameStatic()) ||
               (typeName == KnobColor::typeNameStatic());
    case eKnobVariantLinked:
        return isAnimatableValueType(typeName);
    case eKnobVariantUserKnob:
        // Pages and groups are saved as user-page layout, not as regular user knobs, and a
        // Layers knob was never offered as a user knob.
        return (typeName != KnobPage::typeNameStatic()) && (typeName != KnobGroup::typeNameStatic()) &&
               (typeName != KnobLayers::typeNameStatic());
    }

    return false;
}

std::string
saveToXml(const Serialization::KnobSerialization& serialization)
{
    std::stringstream buffer;
    {
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp("Knob", serialization);
    }

    return buffer.str();
}

std::string
saveKnobToXml(const KnobIPtr& knob)
{
    Serialization::KnobSerialization serialization;
    knob->toSerialization(&serialization);

    return saveToXml(serialization);
}

bool
loadFromXml(const std::string& xml,
            Serialization::KnobSerialization* serialization)
{
    try {
        std::stringstream buffer(xml);
        boost::archive::xml_iarchive archive(buffer);
        archive >> boost::serialization::make_nvp("Knob", *serialization);
    } catch (const std::exception& e) {
        ADD_FAILURE() << "Loading through Serialization/BoostXml failed: " << e.what();

        return false;
    }

    return true;
}

// The XML declaration and opening archive element a saved archive starts with, so that a
// hand-written legacy knob element is read with this build's archive library version.
std::string
archivePrologue(const std::string& savedXml)
{
    std::size_t knobElement = savedXml.find("<Knob ");
    if (knobElement == std::string::npos) {
        return std::string();
    }

    return savedXml.substr(0, knobElement);
}

const int*
intValue(const std::optional<Serialization::KnobValueSerialization>& value)
{
    return value ? std::get_if<int>( &(*value) ) : NULL;
}

const double*
doubleValue(const std::optional<Serialization::KnobValueSerialization>& value)
{
    return value ? std::get_if<double>( &(*value) ) : NULL;
}

} // anonymous namespace

// Byte-identity with the .ntp format itself is guarded by the golden project files of
// GoldenXml_Test; this checks that saving, loading and applying a knob loses nothing.
TEST_F(BaseTest, KnobXmlEquivalenceRoundTripIsByteIdentical)
{
    NodePtr sourceNode = createNode(_generatorPluginID);
    NodePtr targetNode = createNode(_generatorPluginID);
    NodePtr masterNode = createNode(_generatorPluginID);

    ASSERT_TRUE( bool(sourceNode) && bool(targetNode) && bool(masterNode) );

    KnobHolder* sourceHolder = sourceNode->getEffectInstance().get();
    KnobHolder* targetHolder = targetNode->getEffectInstance().get();
    KnobHolder* masterHolder = masterNode->getEffectInstance().get();
    const std::map<std::string, std::string> noRenamedScriptNames;

    for (std::size_t typeIndex = 0; typeIndex < sizeof(kKnobTypeNames) / sizeof(kKnobTypeNames[0]); ++typeIndex) {
        const std::string typeName(kKnobTypeNames[typeIndex]);
        for (std::size_t variantIndex = 0; variantIndex < sizeof(kKnobVariants) / sizeof(kKnobVariants[0]); ++variantIndex) {
            const KnobVariantEnum variant = kKnobVariants[variantIndex];
            const std::string knobName = std::string("knob") + typeName + variantName(variant);
            SCOPED_TRACE(knobName);

            KnobIPtr source = createKnobOfType(sourceHolder, typeName, knobName);
            if (!source) {
                ADD_FAILURE() << "Could not create a knob of type " << typeName;
                continue;
            }
            if ( !variantApplies(typeName, variant, source) ) {
                continue;
            }

            defineKnob(source);
            if (variant == eKnobVariantUserKnob) {
                makeUserKnob(source);
            }
            setDistinctiveValues(source);

            if (variant == eKnobVariantAnimated) {
                animateKnob(source);
            } else if (variant == eKnobVariantExpression) {
                const std::string expression = (typeName == KnobInt::typeNameStatic()) ? "3 + 4" : "0.5 * 3";
                EXPECT_NO_THROW( source->setExpression(0, expression, false, false) );
            } else if (variant == eKnobVariantLinked) {
                KnobIPtr master = createKnobOfType(masterHolder, typeName, std::string("master") + knobName);
                ASSERT_TRUE( bool(master) );
                defineKnob(master);
                setDistinctiveValues(master);
                EXPECT_TRUE( source->slaveTo(0, master, 0) );
            }

            const std::string savedXml = saveKnobToXml(source);

            Serialization::KnobSerialization loaded;
            if ( !loadFromXml(savedXml, &loaded) ) {
                continue;
            }
            EXPECT_EQ( savedXml, saveToXml(loaded) );

            KnobIPtr restored;
            if (variant == eKnobVariantUserKnob) {
                restored = createUserKnobFromSerialization(targetHolder, loaded);
            } else {
                restored = createKnobOfType(targetHolder, typeName, knobName);
                if (restored) {
                    defineKnob(restored);
                    EXPECT_TRUE( restored->fromSerialization(loaded) );
                }
            }
            if (!restored) {
                ADD_FAILURE() << "Could not restore the knob";
                continue;
            }
            restored->restoreExpressionsFromSerialization(loaded, noRenamedScriptNames);
            restored->storeLinksFromSerialization(loaded);
            EXPECT_NO_THROW( restored->restoreLinks(getApp()->getProject()->getNodes(), noRenamedScriptNames, true) );

            EXPECT_EQ( savedXml, saveKnobToXml(restored) );
        }
    }
}

TEST_F(BaseTest, KnobXmlEquivalenceFromSerializationRefusesAnotherType)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE( bool(node) );

    KnobIPtr source = createKnobOfType(node->getEffectInstance().get(), KnobInt::typeNameStatic(), "intSource");
    KnobIPtr target = createKnobOfType(node->getEffectInstance().get(), KnobDouble::typeNameStatic(), "doubleTarget");
    ASSERT_TRUE( bool(source) && bool(target) );

    Serialization::KnobSerialization saved;
    source->toSerialization(&saved);
    EXPECT_FALSE( target->fromSerialization(saved) );
}

TEST_F(BaseTest, KnobXmlEquivalenceFromSerializationFiltersAnUnknownChoiceOptionID)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE( bool(node) );

    KnobIPtr source = createKnobOfType(node->getEffectInstance().get(), KnobChoice::typeNameStatic(), "choiceSource");
    ASSERT_TRUE( bool(source) );
    defineKnob(source);

    Serialization::KnobSerialization saved;
    source->toSerialization(&saved);
    saved.choiceOptionID = std::string("retiredGamma");

    const KnobI::ChoiceOptionIDFilter renameRetiredOption = [](std::string* optionID) {
        if (*optionID == "retiredGamma") {
            *optionID = "gamma";
        }
    };

    KnobIPtr filtered = createKnobOfType(node->getEffectInstance().get(), KnobChoice::typeNameStatic(), "choiceFiltered");
    ASSERT_TRUE( bool(filtered) );
    defineKnob(filtered);
    EXPECT_TRUE( filtered->fromSerialization(saved, renameRetiredOption) );
    KnobChoice* filteredChoice = dynamic_cast<KnobChoice*>( filtered.get() );
    ASSERT_TRUE(filteredChoice != NULL);
    EXPECT_EQ( 2, filteredChoice->getValue() );
    EXPECT_EQ( std::string("gamma"), filteredChoice->getActiveEntry().id );

    KnobIPtr unfiltered = createKnobOfType(node->getEffectInstance().get(), KnobChoice::typeNameStatic(), "choiceUnfiltered");
    ASSERT_TRUE( bool(unfiltered) );
    defineKnob(unfiltered);
    EXPECT_TRUE( unfiltered->fromSerialization(saved) );
    KnobChoice* unfilteredChoice = dynamic_cast<KnobChoice*>( unfiltered.get() );
    ASSERT_TRUE(unfilteredChoice != NULL);
    EXPECT_EQ( 0, unfilteredChoice->getValue() );
}

TEST_F(BaseTest, KnobXmlEquivalenceToSerializationReportsWhetherANodeMustSaveTheKnob)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE( bool(node) );

    KnobHolder* holder = node->getEffectInstance().get();
    Serialization::KnobSerialization saved;

    KnobIPtr untouched = createKnobOfType(holder, KnobInt::typeNameStatic(), "untouchedInt");
    ASSERT_TRUE( bool(untouched) );
    EXPECT_FALSE( untouched->toSerialization(&saved) );

    KnobIPtr edited = createKnobOfType(holder, KnobInt::typeNameStatic(), "editedInt");
    ASSERT_TRUE( bool(edited) );
    setDistinctiveValues(edited);
    EXPECT_TRUE( edited->toSerialization(&saved) );

    KnobIPtr channelSet = createKnobOfType(holder, KnobChannelSet::typeNameStatic(), "untouchedChannels");
    ASSERT_TRUE( bool(channelSet) );
    EXPECT_TRUE( channelSet->toSerialization(&saved) );
}

TEST_F(BaseTest, KnobXmlEquivalenceReadsLegacyChoiceLabel)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE( bool(node) );

    KnobIPtr reference = createKnobOfType(node->getEffectInstance().get(), KnobInt::typeNameStatic(), "referenceKnob");
    ASSERT_TRUE( bool(reference) );
    const std::string prologue = archivePrologue( saveKnobToXml(reference) );
    ASSERT_FALSE( prologue.empty() );

    // Knob version 4 and value version 2: no alias flag, no expression, and the selected entry
    // saved as a Label inside the value rather than as the knob's ChoiceLabel.
    const std::string xml = prologue +
                            "<Knob class_id=\"0\" tracking_level=\"0\" version=\"4\">\n"
                            "<Name>legacyChoice</Name>\n"
                            "<Type>Choice</Type>\n"
                            "<Dimension>1</Dimension>\n"
                            "<Secret>0</Secret>\n"
                            "<item class_id=\"1\" tracking_level=\"0\" version=\"2\">\n"
                            "<Enabled>1</Enabled>\n"
                            "<HasAnimation>0</HasAnimation>\n"
                            "<Value>1</Value>\n"
                            "<Label>beta</Label>\n"
                            "<HasMaster>0</HasMaster>\n"
                            "</item>\n"
                            "</Knob>\n"
                            "</boost_serialization>\n";

    Serialization::KnobSerialization loaded;
    ASSERT_TRUE( loadFromXml(xml, &loaded) );
    EXPECT_EQ(4u, loaded.version);
    EXPECT_EQ(std::string("legacyChoice"), loaded.name);
    ASSERT_EQ(1u, loaded.dimensions.size());
    ASSERT_TRUE( intValue(loaded.dimensions[0].value) != NULL );
    EXPECT_EQ( 1, *intValue(loaded.dimensions[0].value) );
    EXPECT_FALSE( loaded.dimensions[0].defaultValue.has_value() );
    ASSERT_TRUE( loaded.choiceOptionID.has_value() );
    EXPECT_EQ( std::string("beta"), *loaded.choiceOptionID );
    EXPECT_FALSE(loaded.isUserKnob);
    EXPECT_FALSE(loaded.masterIsAlias);
    EXPECT_FALSE( loaded.dimensions[0].master.has_value() );
    EXPECT_TRUE( loaded.dimensions[0].expression.empty() );

    KnobIPtr restored = createKnobOfType(node->getEffectInstance().get(), KnobChoice::typeNameStatic(), "legacyChoice");
    ASSERT_TRUE( bool(restored) );
    defineKnob(restored);
    EXPECT_TRUE( restored->fromSerialization(loaded) );
    KnobChoice* restoredChoice = dynamic_cast<KnobChoice*>( restored.get() );
    ASSERT_TRUE(restoredChoice != NULL);
    EXPECT_EQ( 1, restoredChoice->getValue() );
    EXPECT_EQ( std::string("beta"), restoredChoice->getActiveEntry().id );
}

TEST_F(BaseTest, KnobXmlEquivalenceReadsLegacyUserDoubleWithDefaultsAndExpressionResults)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE( bool(node) );

    KnobIPtr reference = createKnobOfType(node->getEffectInstance().get(), KnobInt::typeNameStatic(), "referenceKnob");
    ASSERT_TRUE( bool(reference) );
    const std::string prologue = archivePrologue( saveKnobToXml(reference) );
    ASSERT_FALSE( prologue.empty() );

    // Knob version 12 (user knob with per-dimension DefaultValue elements and an overlay flag)
    // and value version 5 (with the since-removed ExprResults map).
    const std::string xml = prologue +
                            "<Knob class_id=\"0\" tracking_level=\"0\" version=\"12\">\n"
                            "<Name>legacyDouble</Name>\n"
                            "<Type>Double</Type>\n"
                            "<Dimension>2</Dimension>\n"
                            "<Secret>0</Secret>\n"
                            "<MasterIsAlias>0</MasterIsAlias>\n"
                            "<item class_id=\"1\" tracking_level=\"0\" version=\"5\">\n"
                            "<Enabled>1</Enabled>\n"
                            "<HasAnimation>0</HasAnimation>\n"
                            "<Value>0.5</Value>\n"
                            "<HasMaster>0</HasMaster>\n"
                            "<Expression></Expression>\n"
                            "<ExprHasRet>0</ExprHasRet>\n"
                            "<ExprResults class_id=\"2\" tracking_level=\"0\" version=\"0\">\n"
                            "<count>0</count>\n"
                            "<item_version>0</item_version>\n"
                            "</ExprResults>\n"
                            "</item>\n"
                            "<item>\n"
                            "<Enabled>0</Enabled>\n"
                            "<HasAnimation>0</HasAnimation>\n"
                            "<Value>1.5</Value>\n"
                            "<HasMaster>0</HasMaster>\n"
                            "<Expression></Expression>\n"
                            "<ExprHasRet>0</ExprHasRet>\n"
                            "<ExprResults>\n"
                            "<count>0</count>\n"
                            "<item_version>0</item_version>\n"
                            "</ExprResults>\n"
                            "</item>\n"
                            "<UserKnob>1</UserKnob>\n"
                            "<Label>Legacy Double</Label>\n"
                            "<Help>Saved by an old version</Help>\n"
                            "<NewLine>1</NewLine>\n"
                            "<Evaluate>1</Evaluate>\n"
                            "<Animates>1</Animates>\n"
                            "<Persistent>1</Persistent>\n"
                            "<Min>-10</Min>\n"
                            "<Max>10</Max>\n"
                            "<DMin>-1</DMin>\n"
                            "<DMax>1</DMax>\n"
                            "<HasOverlayHandle>1</HasOverlayHandle>\n"
                            "<DefaultValue>0.25</DefaultValue>\n"
                            "<DefaultValue>0.75</DefaultValue>\n"
                            "</Knob>\n"
                            "</boost_serialization>\n";

    Serialization::KnobSerialization loaded;
    ASSERT_TRUE( loadFromXml(xml, &loaded) );
    EXPECT_EQ(12u, loaded.version);
    ASSERT_EQ(2u, loaded.dimensions.size());
    ASSERT_TRUE( doubleValue(loaded.dimensions[0].value) != NULL );
    EXPECT_EQ( 0.5, *doubleValue(loaded.dimensions[0].value) );
    ASSERT_TRUE( doubleValue(loaded.dimensions[1].value) != NULL );
    EXPECT_EQ( 1.5, *doubleValue(loaded.dimensions[1].value) );
    EXPECT_TRUE(loaded.dimensions[0].enabled);
    EXPECT_FALSE(loaded.dimensions[1].enabled);
    EXPECT_TRUE(loaded.isUserKnob);
    EXPECT_EQ(std::string("Legacy Double"), loaded.label);
    EXPECT_EQ(std::string("Saved by an old version"), loaded.tooltip);
    const Serialization::KnobValueRangeSerialization* range = std::get_if<Serialization::KnobValueRangeSerialization>(&loaded.extraData);
    ASSERT_TRUE(range != NULL);
    EXPECT_EQ(-10., range->minimum);
    EXPECT_EQ(10., range->maximum);
    EXPECT_EQ(-1., range->displayMinimum);
    EXPECT_EQ(1., range->displayMaximum);
    ASSERT_TRUE( loaded.useHostOverlayHandle.has_value() );
    EXPECT_TRUE(*loaded.useHostOverlayHandle);
    EXPECT_FALSE( loaded.dimensions[0].defaultValue.has_value() );
    EXPECT_FALSE( loaded.dimensions[1].defaultValue.has_value() );
    EXPECT_TRUE(loaded.triggerNewLine);
    EXPECT_TRUE(loaded.evaluatesOnChange);
    EXPECT_TRUE(loaded.animationEnabled);
    EXPECT_TRUE(loaded.isPersistent);
    EXPECT_FALSE( loaded.choiceOptionID.has_value() );

    KnobIPtr restored = createUserKnobFromSerialization(node->getEffectInstance().get(), loaded);
    ASSERT_TRUE( bool(restored) );
    KnobDouble* restoredDouble = dynamic_cast<KnobDouble*>( restored.get() );
    ASSERT_TRUE(restoredDouble != NULL);
    EXPECT_EQ( 0.5, restoredDouble->getValue(0) );
    EXPECT_EQ( 1.5, restoredDouble->getValue(1) );
    EXPECT_EQ( -10., restoredDouble->getMinimum(0) );
    EXPECT_EQ( 1., restoredDouble->getDisplayMaximum(0) );
    EXPECT_TRUE( restoredDouble->isUserKnob() );
    EXPECT_EQ( std::string("Saved by an old version"), restoredDouble->getHintToolTip() );
}

TEST_F(BaseTest, KnobXmlEquivalenceReadsLegacySlavedTracksAndShortMaster)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE( bool(node) );

    KnobIPtr reference = createKnobOfType(node->getEffectInstance().get(), KnobInt::typeNameStatic(), "referenceKnob");
    ASSERT_TRUE( bool(reference) );
    const std::string prologue = archivePrologue( saveKnobToXml(reference) );
    ASSERT_FALSE( prologue.empty() );

    // Knob version 3 (a 2D Double named "center" followed by the since-removed slaved track
    // points), value version 1 and master version 1 (no full node name, no track name).
    const std::string xml = prologue +
                            "<Knob class_id=\"0\" tracking_level=\"0\" version=\"3\">\n"
                            "<Name>center</Name>\n"
                            "<Type>Double</Type>\n"
                            "<Dimension>2</Dimension>\n"
                            "<Secret>1</Secret>\n"
                            "<item class_id=\"1\" tracking_level=\"0\" version=\"1\">\n"
                            "<Enabled>1</Enabled>\n"
                            "<HasAnimation>0</HasAnimation>\n"
                            "<Value>0.25</Value>\n"
                            "<HasMaster>1</HasMaster>\n"
                            "<Master class_id=\"2\" tracking_level=\"0\" version=\"1\">\n"
                            "<MasterDimension>1</MasterDimension>\n"
                            "<MasterNodeName>Transform1</MasterNodeName>\n"
                            "<MasterKnobName>translate</MasterKnobName>\n"
                            "</Master>\n"
                            "</item>\n"
                            "<item>\n"
                            "<Enabled>1</Enabled>\n"
                            "<HasAnimation>0</HasAnimation>\n"
                            "<Value>0.75</Value>\n"
                            "<HasMaster>0</HasMaster>\n"
                            "</item>\n"
                            "<SlavePtsNo>1</SlavePtsNo>\n"
                            "<SlavePtNodeName>Roto1</SlavePtNodeName>\n"
                            "<SlavePtBezier>Bezier1</SlavePtBezier>\n"
                            "<SlavePtIndex>2</SlavePtIndex>\n"
                            "<SlavePtIsFeather>0</SlavePtIsFeather>\n"
                            "<OffsetTime>5</OffsetTime>\n"
                            "</Knob>\n"
                            "</boost_serialization>\n";

    Serialization::KnobSerialization loaded;
    ASSERT_TRUE( loadFromXml(xml, &loaded) );
    EXPECT_EQ(3u, loaded.version);
    EXPECT_TRUE(loaded.secret);
    EXPECT_FALSE(loaded.masterIsAlias);
    ASSERT_EQ(2u, loaded.dimensions.size());
    ASSERT_TRUE( loaded.dimensions[0].master.has_value() );
    EXPECT_EQ(1, loaded.dimensions[0].master->masterDimension);
    EXPECT_EQ(std::string("Transform1"), loaded.dimensions[0].master->masterNodeName);
    EXPECT_EQ(std::string("translate"), loaded.dimensions[0].master->masterKnobName);
    EXPECT_TRUE( loaded.dimensions[0].master->masterNodeNameFull.empty() );
    EXPECT_FALSE( loaded.dimensions[1].master.has_value() );
    ASSERT_TRUE( doubleValue(loaded.dimensions[0].value) != NULL );
    EXPECT_EQ( 0.25, *doubleValue(loaded.dimensions[0].value) );
    ASSERT_TRUE( doubleValue(loaded.dimensions[1].value) != NULL );
    EXPECT_EQ( 0.75, *doubleValue(loaded.dimensions[1].value) );
    EXPECT_FALSE( loaded.dimensions[0].defaultValue.has_value() );
    EXPECT_TRUE( loaded.dimensions[0].expression.empty() );
    EXPECT_TRUE( loaded.dimensions[0].master->masterTrackName.empty() );
    EXPECT_FALSE(loaded.isUserKnob);
}
