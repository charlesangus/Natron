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

#ifndef Engine_Nodes_Image_ColorMath_h
#define Engine_Nodes_Image_ColorMath_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EngineFwd.h"
#include "Engine/KnobTypes.h"

#define kColorMathParamLuminanceMath "luminanceMath"
#define kColorMathParamLuminanceMathLabel "Luminance Math"
#define kColorMathParamLuminanceMathHint "Formula used to compute luminance from RGB values."

NATRON_NAMESPACE_ENTER

/**
 * @brief Colour maths shared by the native colour, generator and keying nodes. The formulas and
 * coefficients are the openfx-misc ones (SupportExt/ofxsLut.h), written the same way so that
 * the native nodes compute the same values, with one deliberate exception: ACES AP1 uses the
 * AP1 coefficients, where the OpenFX Saturation fell through to CCIR 601.
 **/
namespace ColorMath {
enum LuminanceMathEnum {
    eLuminanceMathRec709 = 0,
    eLuminanceMathRec2020,
    eLuminanceMathAcesAP0,
    eLuminanceMathAcesAP1,
    eLuminanceMathCcir601,
    eLuminanceMathAverage,
    eLuminanceMathMax
};

inline double
luminance(LuminanceMathEnum math,
          double r,
          double g,
          double b)
{
    switch (math) {
    case eLuminanceMathRec709:
        // Single-precision constants, as in ofxsLut.h's rgb709_to_y().
        return 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
    case eLuminanceMathRec2020:
        return 0.2627002119 * r + 0.6779980711 * g + 0.0593017165 * b;
    case eLuminanceMathAcesAP0:
        return 0.3439664498 * r + 0.7281660966 * g + -0.0721325464 * b;
    case eLuminanceMathAcesAP1:
        return 0.2722287168 * r + 0.6740817658 * g + 0.0536895174 * b;
    case eLuminanceMathCcir601:
        return 0.2989 * r + 0.5866 * g + 0.1145 * b;
    case eLuminanceMathAverage:
        return (r + g + b) / 3;
    case eLuminanceMathMax:
        return (std::max)((std::max)(r, g), b);
    }

    return 0.;
}

/**
 * @brief Declares the luminanceMath choice with the OpenFX option IDs, labels and hints,
 * defaulting to Rec. 709, and adds it to page.
 **/
inline KnobChoicePtr
addLuminanceMathKnob(KnobHolder* holder,
                     const KnobPagePtr& page)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, std::string(kColorMathParamLuminanceMathLabel));

    knob->setName(kColorMathParamLuminanceMath);
    knob->setHintToolTip(std::string(kColorMathParamLuminanceMathHint));
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("rec709", "Rec. 709", "Use Rec. 709 (0.2126r + 0.7152g + 0.0722b)."));
    options.push_back(ChoiceOption("rec2020", "Rec. 2020", "Use Rec. 2020 (0.2627r + 0.6780g + 0.0593b)."));
    options.push_back(ChoiceOption("acesap0", "ACES AP0", "Use ACES AP0 (0.3439664498r + 0.7281660966g + -0.0721325464b)."));
    options.push_back(ChoiceOption("acesap1", "ACES AP1", "Use ACES AP1 (0.2722287168r +  0.6740817658g +  0.0536895174b)."));
    options.push_back(ChoiceOption("ccir601", "CCIR 601", "Use CCIR 601 (0.2989r + 0.5866g + 0.1145b)."));
    options.push_back(ChoiceOption("average", "Average", "Use average of r, g, b."));
    options.push_back(ChoiceOption("max", "Max", "Use max or r, g, b."));
    knob->populateChoices(options);
    knob->setDefaultValue((int)eLuminanceMathRec709);
    if (page) {
        page->addKnob(knob);
    }

    return knob;
}

/**
 * @brief The sRGB and Rec. 709 transfer functions: from_* decodes to linear, to_* encodes from
 * linear, negatives clamped to 0. Rec. 709 uses the Rec. 2020 constants, as ofxsLut.h does.
 **/
inline float
fromSRGB(float v)
{
    if (v < 0.04045f) {
        return (v < 0.0f) ? 0.0f : v * (1.0f / 12.92f);
    }

    return std::pow((v + 0.055f) * (1.0f / 1.055f), 2.4f);
}

inline float
toSRGB(float v)
{
    if (v < 0.0031308f) {
        return (v < 0.0f) ? 0.0f : v * 12.92f;
    }

    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

inline float
fromRec709(float v)
{
    if (v < 0.08145f) {
        return (v < 0.0f) ? 0.0f : v * (1.0f / 4.5f);
    }

    return std::pow((v + 0.0993f) * (1.0f / 1.0993f), (1.0f / 0.45f));
}

inline float
toRec709(float v)
{
    if (v < 0.0181f) {
        return (v < 0.0f) ? 0.0f : v * 4.5f;
    }

    return 1.0993f * std::pow(v, 0.45f) - (1.0993f - 1.f);
}
} // namespace ColorMath

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_ColorMath_h
