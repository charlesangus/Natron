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

#include "OCIOColorSpace.h"

#include <optional>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {
// The spaces a preset sets, input then output; both empty for Custom.
std::pair<std::string, std::string>
presetSpaces(const std::string& preset)
{
    if (preset == kOCIOColorSpacePresetLogToLin) {
        return std::make_pair(std::string(OCIO_NAMESPACE::ROLE_COMPOSITING_LOG), std::string(OCIO_NAMESPACE::ROLE_SCENE_LINEAR));
    }
    if (preset == kOCIOColorSpacePresetLinToLog) {
        return std::make_pair(std::string(OCIO_NAMESPACE::ROLE_SCENE_LINEAR), std::string(OCIO_NAMESPACE::ROLE_COMPOSITING_LOG));
    }

    return std::make_pair(std::string(), std::string());
}
} // anonymous namespace

void
OCIOColorSpaceOp::createKnobs(KnobHolder* holder,
                              const KnobPagePtr& page)
{
    // The working space until the user picks one, and again on a reset.
    const OcioSpaceKnobs::AutomaticSpace working = []() { return std::optional<std::string>(std::string()); };
    const OcioSpaceKnobs::ChoiceMade chosen = [this]() { spacesChosen(); };

    OcioSpaceKnobs::Description in;
    in.prefix = kOCIOColorSpaceParamInputSpace;
    in.label = EffectInstance::tr("Input Colorspace");
    in.spaceHint = EffectInstance::tr("The OpenColorIO colourspace the input is in. Empty is the working space.");
    in.menuHint = EffectInstance::tr("The OpenColorIO colourspace the input is in.");
    in.setLabel = EffectInstance::tr("Input Colorspace Set");
    ocio.addSpace(holder, page, in, working, chosen);

    OcioSpaceKnobs::Description out;
    out.prefix = kOCIOColorSpaceParamOutputSpace;
    out.label = EffectInstance::tr("Output Colorspace");
    out.spaceHint = EffectInstance::tr("The OpenColorIO colourspace to convert to. Empty is the working space.");
    out.menuHint = EffectInstance::tr("The OpenColorIO colourspace to convert to.");
    out.setLabel = EffectInstance::tr("Output Colorspace Set");
    ocio.addSpace(holder, page, out, working, chosen);

    inputSpace = std::dynamic_pointer_cast<KnobString>(holder->getKnobByName(kOCIOColorSpaceParamInputSpace));
    outputSpace = std::dynamic_pointer_cast<KnobString>(holder->getKnobByName(kOCIOColorSpaceParamOutputSpace));

    KnobChoicePtr presetKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Preset").toStdString());
    presetKnob->setName(kOCIOColorSpaceParamPreset);
    presetKnob->setHintToolTip(EffectInstance::tr("Sets both colourspaces at once. Log to Lin converts from the config's "
                                                  "compositing_log role to its scene_linear role, Lin to Log the other way. "
                                                  "Changing a colourspace by hand returns this to Custom.")
                                   .toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kOCIOColorSpacePresetCustom, EffectInstance::tr("Custom").toStdString(), EffectInstance::tr("The colourspaces as set.").toStdString()));
    options.push_back(ChoiceOption(kOCIOColorSpacePresetLogToLin, EffectInstance::tr("Log to Lin").toStdString(), EffectInstance::tr("From compositing_log to scene_linear.").toStdString()));
    options.push_back(ChoiceOption(kOCIOColorSpacePresetLinToLog, EffectInstance::tr("Lin to Log").toStdString(), EffectInstance::tr("From scene_linear to compositing_log.").toStdString()));
    presetKnob->populateChoices(options);
    presetKnob->setDefaultValue(0);
    presetKnob->setAnimationEnabled(false);
    // The spaces it sets are what render.
    presetKnob->setEvaluateOnChange(false);
    ocio.addKnob(page, presetKnob);
    preset = presetKnob;
}

OCIO_NAMESPACE::ConstTransformRcPtr
OCIOColorSpaceOp::transform(const KnobReader& /*reader*/) const
{
    return ocio.spaceTransform(ocio.spaceOrWorking(0), ocio.spaceOrWorking(1));
}

void
OCIOColorSpaceOp::applyPreset()
{
    KnobChoicePtr presetKnob = preset.lock();
    KnobStringPtr in = inputSpace.lock();
    KnobStringPtr out = outputSpace.lock();

    if (!presetKnob || !in || !out) {
        return;
    }
    const std::pair<std::string, std::string> spaces = presetSpaces(presetKnob->getActiveEntry().id);
    if (spaces.first.empty()) {
        return;
    }
    applyingPreset = true;
    in->setValue(spaces.first);
    out->setValue(spaces.second);
    applyingPreset = false;
}

void
OCIOColorSpaceOp::spacesChosen()
{
    KnobChoicePtr presetKnob = preset.lock();

    if (applyingPreset || !presetKnob) {
        return;
    }
    const std::pair<std::string, std::string> spaces = presetSpaces(presetKnob->getActiveEntry().id);
    if (!spaces.first.empty() && ((ocio.space(0) != spaces.first) || (ocio.space(1) != spaces.second))) {
        presetKnob->setValueFromID(kOCIOColorSpacePresetCustom, 0);
    }
}

OCIOColorSpace::OCIOColorSpace(NodePtr node)
    : OcioNode<OCIOColorSpaceOp>(node)
{
}

OCIOColorSpace::~OCIOColorSpace()
{
}

bool
OCIOColorSpace::knobChanged(KnobI* k,
                            ValueChangedReasonEnum reason,
                            ViewSpec view,
                            double time,
                            bool originatedFromMainThread)
{
    KnobChoicePtr presetKnob = op().preset.lock();

    if (presetKnob && (k == presetKnob.get())) {
        // A script setting the preset applies it too; a saved one matches the spaces saved with it.
        if (reason != eValueChangedReasonTimeChanged) {
            op().applyPreset();
        }

        return true;
    }

    return OcioNode<OCIOColorSpaceOp>::knobChanged(k, reason, view, time, originatedFromMainThread);
}

NATRON_NAMESPACE_EXIT
