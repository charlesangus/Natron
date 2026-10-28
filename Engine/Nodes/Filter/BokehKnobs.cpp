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

#include "BokehKnobs.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kOrientationTangential = 1;

KnobDoublePtr
addFraction(KnobHolder* holder,
            const KnobPagePtr& page,
            const char* name,
            const QString& label,
            const QString& hint,
            double minimum,
            double maximum,
            double defaultValue)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setMinimum(minimum);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(minimum);
    knob->setDisplayMaximum(maximum);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

double
finiteOr(double value,
         double fallback)
{
    return std::isfinite(value) ? value : fallback;
}
} // anonymous namespace

void
BokehKnobs::addTextureKnobs(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    _filterShape = addFraction(holder, page, kBokehParamFilterShape, EffectInstance::tr("Filter Shape"),
                               EffectInstance::tr("Disc and Blades kernels: the bokeh's profile, from a Gaussian at 0 to the flat shape at 1."), 0., 1., 1.);
    _innerSize = addFraction(holder, page, kBokehParamInnerSize, EffectInstance::tr("Inner Size"),
                             EffectInstance::tr("Disc and Blades kernels: the size of the inner shape, the onion ring's inside, as a fraction of the bokeh."), 0., 1., 0.8);
    _innerFeather = addFraction(holder, page, kBokehParamInnerFeather, EffectInstance::tr("Inner Feather"),
                                EffectInstance::tr("Disc and Blades kernels: how the inner shape's brightness blends into the rim's. Above 0.5 it "
                                                   "feathers outward, reaching the rim at 1; below it, inward, from the centre at 0."),
                                0., 1., 1.);
    KnobDoublePtr brightness = addFraction(holder, page, kBokehParamInnerBrightness, EffectInstance::tr("Inner Brightness"),
                                           EffectInstance::tr("Disc and Blades kernels: the brightness of the inner shape against the rim's 1. Below 1 "
                                                              "the bokeh shows a bright ring."),
                                           0., 1., 1.);
    brightness->setMaximum(10.);
    _innerBrightness = brightness;

    KnobBoolPtr catadioptric = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Catadioptric"));
    catadioptric->setName(kBokehParamCatadioptric);
    catadioptric->setHintToolTip(EffectInstance::tr("Disc and Blades kernels: a hole in the bokeh's centre, as a mirror lens's "
                                                    "central obstruction gives.")
                                     .toStdString());
    catadioptric->setDefaultValue(false);
    catadioptric->setAddNewLine(false);
    page->addKnob(catadioptric);
    _catadioptric = catadioptric;

    _catadioptricSize = addFraction(holder, page, kBokehParamCatadioptricSize, EffectInstance::tr("Catadioptric Size"),
                                    EffectInstance::tr("Disc and Blades kernels, with Catadioptric: the hole's size as a fraction of the bokeh."), 0., 1., 0.3);
    _sphericalAberration = addFraction(holder, page, kBokehParamSphericalAberration, EffectInstance::tr("Spherical Aberration"),
                                       EffectInstance::tr("Disc and Blades kernels: brightens the bokeh's rim when positive and its centre when "
                                                          "negative, as an under- or over-corrected lens does."),
                                       -1., 1., 0.);
}

void
BokehKnobs::addLensKnobs(KnobHolder* holder,
                         const KnobPagePtr& page)
{
    KnobDoublePtr catEye = addFraction(holder, page, kBokehParamCatEye, EffectInstance::tr("Cat Eye"),
                                       EffectInstance::tr("Optical vignetting: away from the format's centre the bokeh is cut by a second aperture "
                                                          "offset by this many bokeh radii at the corners, turning discs into cat's eyes. Kernels "
                                                          "change in steps of 32 pixels."),
                                       0., 1., 0.);
    catEye->setMaximum(1.5);
    _catEye = catEye;

    KnobChoicePtr orientation = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Cat Eye Orientation"));
    orientation->setName(kBokehParamCatEyeOrientation);
    orientation->setHintToolTip(EffectInstance::tr("Which way the second aperture is offset.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kBokehParamCatEyeOrientationRadial, EffectInstance::tr("Radial").toStdString(),
                                       EffectInstance::tr("Towards the centre: the bokeh narrows across the radius, as a lens barrel cuts it.").toStdString()));
        options.push_back(ChoiceOption(kBokehParamCatEyeOrientationTangential, EffectInstance::tr("Tangential").toStdString(),
                                       EffectInstance::tr("Across the radius: the bokeh narrows along it, swirling around the centre.").toStdString()));
        orientation->populateChoices(options);
    }
    orientation->setDefaultValue(0);
    page->addKnob(orientation);
    _catEyeOrientation = orientation;

    KnobDoublePtr amount = addFraction(holder, page, kBokehParamChromaticAberration, EffectInstance::tr("Chromatic Aberration"),
                                       EffectInstance::tr("Scales each colour channel's bokeh by 1 plus this amount times its Chromatic Offset, "
                                                          "fringing the bokeh with colour. The alpha and other channels follow the green."),
                                       -0.5, 0.5, 0.);
    amount->setMinimum(-1.);
    amount->setMaximum(1.);
    _chromaticAberration = amount;

    KnobColorPtr offset = AppManager::createKnob<KnobColor>(holder, EffectInstance::tr("Chromatic Offset"), 3);
    offset->setName(kBokehParamChromaticOffset);
    offset->setHintToolTip(EffectInstance::tr("How much Chromatic Aberration scales the red, green and blue bokeh: by default the "
                                              "red grows, the blue shrinks and the green keeps its size.")
                               .toStdString());
    const double defaults[3] = { 1., 0., -1. };
    for (int i = 0; i < 3; ++i) {
        offset->setDefaultValue(defaults[i], i);
        offset->setMinimum(-DBL_MAX, i);
        offset->setMaximum(DBL_MAX, i);
        offset->setDisplayMinimum(-1., i);
        offset->setDisplayMaximum(1., i);
    }
    page->addKnob(offset);
    _chromaticOffset = offset;
}

void
BokehKnobs::readTexture(const KnobReader& reader,
                        BokehShapeParams* shape) const
{
    const BokehShapeParams flat;

    shape->filterShape = std::clamp(finiteOr(reader.value(_filterShape), flat.filterShape), 0., 1.);
    shape->innerSize = std::clamp(finiteOr(reader.value(_innerSize), flat.innerSize), 0., 1.);
    shape->innerFeather = std::clamp(finiteOr(reader.value(_innerFeather), flat.innerFeather), 0., 1.);
    shape->innerBrightness = std::max(0., finiteOr(reader.value(_innerBrightness), flat.innerBrightness));
    shape->catadioptric = reader.value(_catadioptric);
    shape->catadioptricSize = std::clamp(finiteOr(reader.value(_catadioptricSize), flat.catadioptricSize), 0., 1.);
    shape->sphericalAberration = std::clamp(finiteOr(reader.value(_sphericalAberration), flat.sphericalAberration), -1., 1.);
}

LensAberrationParams
BokehKnobs::readLens(const KnobReader& reader) const
{
    LensAberrationParams lens;

    lens.catEye = std::clamp(finiteOr(reader.value(_catEye), 0.), 0., 1.5);
    lens.catEyeOrientation = (reader.value(_catEyeOrientation) == kOrientationTangential) ? CatEyeOrientation::Tangential : CatEyeOrientation::Radial;
    lens.chromaticAberration = finiteOr(reader.value(_chromaticAberration), 0.);
    for (int c = 0; c < 3; ++c) {
        lens.chromaticOffset[c] = finiteOr(reader.value(_chromaticOffset, c), 0.);
    }

    return lens;
}

NATRON_NAMESPACE_EXIT
