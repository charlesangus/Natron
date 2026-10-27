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

#include "OCIOLookTransform.h"

#include <optional>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

void
OCIOLookTransformOp::createKnobs(KnobHolder* holder,
                                 const KnobPagePtr& page)
{
    // The working space until the user picks one, and again on a reset.
    const OcioSpaceKnobs::AutomaticSpace working = []() { return std::optional<std::string>(std::string()); };

    OcioSpaceKnobs::Description in;
    in.prefix = kOCIOLookTransformParamInputSpace;
    in.label = EffectInstance::tr("Input Colorspace");
    in.spaceHint = EffectInstance::tr("The OpenColorIO colourspace the input is in. Empty is the working space.");
    in.menuHint = EffectInstance::tr("The OpenColorIO colourspace the input is in.");
    in.setLabel = EffectInstance::tr("Input Colorspace Set");
    ocio.addSpace(holder, page, in, working);

    OcioSpaceKnobs::Description out;
    out.prefix = kOCIOLookTransformParamOutputSpace;
    out.label = EffectInstance::tr("Output Colorspace");
    out.spaceHint = EffectInstance::tr("The OpenColorIO colourspace to convert to. Empty is the working space.");
    out.menuHint = EffectInstance::tr("The OpenColorIO colourspace to convert to.");
    out.setLabel = EffectInstance::tr("Output Colorspace Set");
    ocio.addSpace(holder, page, out, working);

    KnobChoicePtr lookKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Look").toStdString());
    lookKnob->setName(kOCIOLookTransformParamLook);
    lookKnob->setHintToolTip(EffectInstance::tr("The look to apply while Single Look is checked, or to add to the Look "
                                                "Combination with Append.")
                                 .toStdString());
    lookKnob->setAnimationEnabled(false);
    lookKnob->setAddNewLine(false);
    ocio.addKnob(page, lookKnob);
    look = lookKnob;

    KnobButtonPtr appendKnob = AppManager::createKnob<KnobButton>(holder, EffectInstance::tr("Append").toStdString());
    appendKnob->setName(kOCIOLookTransformParamAppend);
    appendKnob->setHintToolTip(EffectInstance::tr("Adds the selected look to the end of the Look Combination.").toStdString());
    ocio.addKnob(page, appendKnob);
    append = appendKnob;

    KnobBoolPtr singleLookKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Single Look").toStdString());
    singleLookKnob->setName(kOCIOLookTransformParamSingleLook);
    singleLookKnob->setHintToolTip(EffectInstance::tr("When checked, only the selected look is applied; otherwise the Look "
                                                      "Combination is.")
                                       .toStdString());
    singleLookKnob->setAnimationEnabled(false);
    singleLookKnob->setDefaultValue(true);
    ocio.addKnob(page, singleLookKnob);
    singleLook = singleLookKnob;

    KnobStringPtr combinationKnob = AppManager::createKnob<KnobString>(holder, EffectInstance::tr("Look Combination").toStdString());
    combinationKnob->setName(kOCIOLookTransformParamLookCombination);
    combinationKnob->setHintToolTip(EffectInstance::tr("The looks to apply, in order, separated by commas. A look prefixed with "
                                                       "\"-\" is applied inverted, and \"+\" forward, the default. Alternatives "
                                                       "separated by \"|\" are tried in turn, the first the config has being "
                                                       "applied.")
                                        .toStdString());
    combinationKnob->setAnimationEnabled(false);
    ocio.addKnob(page, combinationKnob);
    combination = combinationKnob;

    KnobChoicePtr directionKnob = OcioNodeKnobs::createDirectionKnob(holder);
    directionKnob->setName(kOCIOLookTransformParamDirection);
    ocio.addKnob(page, directionKnob);
    direction = directionKnob;

    refreshLookMenu();
    ocio.addMenuRefresher([this]() { refreshLookMenu(); });
    refreshSingleLook();
}

OCIO_NAMESPACE::ConstTransformRcPtr
OCIOLookTransformOp::transform(const KnobReader& reader) const
{
    std::string looks;

    if (reader.value(singleLook)) {
        KnobChoicePtr lookKnob = look.lock();
        looks = lookKnob ? lookKnob->getActiveEntry().id : std::string();
    } else {
        looks = reader.value(combination);
    }
    const std::string from = ocio.spaceOrWorking(0);
    const std::string to = ocio.spaceOrWorking(1);
    if (from.empty() || to.empty()) {
        return OCIO_NAMESPACE::ConstTransformRcPtr();
    }
    OCIO_NAMESPACE::LookTransformRcPtr transform = OCIO_NAMESPACE::LookTransform::Create();
    transform->setLooks(looks.c_str());
    if (OcioNodeKnobs::direction(reader.value(direction)) == OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE) {
        // An inverse LookTransform goes from its destination back to its source; swapping them
        // keeps Input and Output what they say, the looks being undone from Output to Input.
        transform->setSrc(to.c_str());
        transform->setDst(from.c_str());
        transform->setDirection(OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE);
    } else {
        transform->setSrc(from.c_str());
        transform->setDst(to.c_str());
    }

    return transform;
}

void
OCIOLookTransformOp::refreshLookMenu()
{
    ProjectColorManagementPtr colorManagement = ocio.colorManagement();

    OcioNodeKnobs::populateMenu(look.lock(), colorManagement ? colorManagement->getLooks() : std::vector<std::string>());
}

void
OCIOLookTransformOp::appendLook()
{
    KnobChoicePtr lookKnob = look.lock();
    KnobStringPtr combinationKnob = combination.lock();

    if (!lookKnob || !combinationKnob) {
        return;
    }
    const std::string name = lookKnob->getActiveEntry().id;
    if (name.empty()) {
        return;
    }
    const std::string current = combinationKnob->getValue();
    combinationKnob->setValue(current.empty() ? name : (current + ", " + name));
}

void
OCIOLookTransformOp::refreshSingleLook()
{
    KnobBoolPtr singleLookKnob = singleLook.lock();
    KnobStringPtr combinationKnob = combination.lock();

    if (singleLookKnob && combinationKnob) {
        combinationKnob->setAllDimensionsEnabled(!singleLookKnob->getValue());
    }
}

OCIOLookTransform::OCIOLookTransform(NodePtr node)
    : OcioNode<OCIOLookTransformOp>(node)
{
}

OCIOLookTransform::~OCIOLookTransform()
{
}

void
OCIOLookTransform::onKnobsLoaded()
{
    op().refreshSingleLook();
    OcioNode<OCIOLookTransformOp>::onKnobsLoaded();
}

bool
OCIOLookTransform::knobChanged(KnobI* k,
                               ValueChangedReasonEnum reason,
                               ViewSpec view,
                               double time,
                               bool originatedFromMainThread)
{
    bool handled = false;

    if (reason != eValueChangedReasonTimeChanged) {
        KnobButtonPtr appendKnob = op().append.lock();
        KnobBoolPtr singleLookKnob = op().singleLook.lock();
        if (appendKnob && (k == appendKnob.get())) {
            op().appendLook();
            handled = true;
        } else if (singleLookKnob && (k == singleLookKnob.get())) {
            op().refreshSingleLook();
            handled = true;
        }
    }

    return OcioNode<OCIOLookTransformOp>::knobChanged(k, reason, view, time, originatedFromMainThread) || handled;
}

NATRON_NAMESPACE_EXIT
