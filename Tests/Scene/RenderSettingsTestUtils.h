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

#ifndef NATRON_TESTS_SCENE_RENDERSETTINGSTESTUTILS_H
#define NATRON_TESTS_SCENE_RENDERSETTINGSTESTUTILS_H

// Helpers for tests that hand USDRender its camera, format and renderer settings through a
// USDRenderSettings node (M253 - Delegate Render Settings).

#include <string>

#include <gtest/gtest.h>

#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "ofxNatron.h"

namespace RenderSettingsTestUtils {
/**
 * Sets the USDRenderSettings node settings to render through camera at width x height with par,
 * the aperture conformed per policy: cropAperture by default, which frames as USDRender's Fill
 * fit did when the references were recorded. False after a gtest failure.
 **/
inline bool
configure(const NATRON_NAMESPACE::NodePtr& settings,
          const std::string& camera,
          int width,
          int height,
          double par = 1.,
          const std::string& policy = "cropAperture")
{
    NATRON_NAMESPACE_USING
    if (!settings) {
        ADD_FAILURE() << "no USDRenderSettings node";

        return false;
    }
    KnobChoice* cameraKnob = dynamic_cast<KnobChoice*>(settings->getKnobByName(kUSDRenderSettingsParamCamera).get());
    KnobInt* size = dynamic_cast<KnobInt*>(settings->getKnobByName(kNatronParamFormatSize).get());
    KnobDouble* parKnob = dynamic_cast<KnobDouble*>(settings->getKnobByName(kNatronParamFormatPar).get());
    KnobChoice* policyKnob = dynamic_cast<KnobChoice*>(settings->getKnobByName(kUSDRenderSettingsParamConformPolicy).get());
    if (!cameraKnob || !size || !parKnob || !policyKnob) {
        ADD_FAILURE() << "USDRenderSettings lacks a camera, format or conform policy knob";

        return false;
    }
    if (!camera.empty()) {
        cameraKnob->setActiveEntry(ChoiceOption(camera, camera, ""));
    }
    size->setValue(width, ViewSpec::all(), 0);
    size->setValue(height, ViewSpec::all(), 1);
    parKnob->setValue(par);
    (void)policyKnob->setValueFromID(policy, 0);
    if (policyKnob->getActiveEntry().id != policy) {
        ADD_FAILURE() << "the conform policy menu has no " << policy;

        return false;
    }

    return true;
}

// The USDRenderSettings effect on render's Scene input, null if there is none.
inline NATRON_NAMESPACE::USDRenderSettings*
settingsOf(const NATRON_NAMESPACE::NodePtr& render)
{
    NATRON_NAMESPACE_USING
    NodePtr input = render ? render->getInput(0) : NodePtr();

    return input ? dynamic_cast<USDRenderSettings*>(input->getEffectInstance().get()) : nullptr;
}
} // namespace RenderSettingsTestUtils

#endif // NATRON_TESTS_SCENE_RENDERSETTINGSTESTUTILS_H
