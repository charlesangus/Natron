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

#include "ShapeGenerator.h"

#include <algorithm>
#include <cfloat>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

namespace ShapeGenerator {

namespace {
    std::string
    trStd(const char* text)
    {
        return EffectInstance::tr(text).toStdString();
    }
} // anonymous namespace

float
boundedFloat(double v,
             double limit,
             float fallback)
{
    if (v != v) {
        return fallback;
    }

    return (float)(std::max)(-limit, (std::min)(limit, v));
}

KnobColorPtr
addColorKnob(KnobHolder* holder,
             const KnobPagePtr& page,
             const char* name,
             const char* label,
             const char* hint,
             const double defaults[4])
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, trStd(label), 4);

    knob->setName(name);
    knob->setHintToolTip(trStd(hint));
    for (int i = 0; i < 4; ++i) {
        knob->setDefaultValue(defaults[i], i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
    }
    page->addKnob(knob);

    return knob;
}

KnobChoicePtr
addFalloffKnob(KnobHolder* holder,
               const KnobPagePtr& page,
               const char* hint,
               int defaultFalloff)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, trStd("Falloff"));

    knob->setName(kShapeParamFalloff);
    knob->setHintToolTip(trStd(hint));
    std::vector<ChoiceOption> options(ShapeFalloff::kFalloffCount);
    options[ShapeFalloff::kFalloffLinear] = ChoiceOption("linear", trStd("Linear"), trStd("Straight from colour 0 to colour 1."));
    options[ShapeFalloff::kFalloffSmooth] = ChoiceOption("smooth", trStd("Smooth"), trStd("Smoothstep: smooth at both ends."));
    options[ShapeFalloff::kFalloffEaseIn] = ChoiceOption("easein", trStd("Ease In"), trStd("Smooth start, linear end."));
    options[ShapeFalloff::kFalloffEaseOut] = ChoiceOption("easeout", trStd("Ease Out"), trStd("Linear start, smooth end."));
    options[ShapeFalloff::kFalloffQuadratic] = ChoiceOption("quadratic", trStd("Quadratic"), trStd("Two parabolas meeting halfway: smooth at both ends, steeper in the middle than Smooth."));
    knob->populateChoices(options);
    knob->setDefaultValue(defaultFalloff);
    page->addKnob(knob);

    return knob;
}

KnobDoublePtr
addSoftnessKnob(KnobHolder* holder,
                const KnobPagePtr& page,
                const char* hint)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, trStd("Softness"), 1);

    knob->setName(kShapeParamSoftness);
    knob->setHintToolTip(trStd(hint));
    knob->setMinimum(0.);
    knob->setMaximum(DBL_MAX);
    knob->setDisplayMinimum(0.);
    knob->setDisplayMaximum(500.);
    knob->setDefaultValue(0.);
    page->addKnob(knob);

    return knob;
}

void
readColor(const KnobReader& reader,
          const KnobColorWPtr& knob,
          const double fallback[4],
          float out[4])
{
    for (int i = 0; i < 4; ++i) {
        out[i] = boundedFloat(reader.valueOr(knob, fallback[i], i), kMaxColor, (float)fallback[i]);
    }
}

int
readFalloff(const KnobReader& reader,
            const KnobChoiceWPtr& knob,
            int fallback)
{
    const int v = reader.valueOr(knob, fallback);

    return (v >= 0) && (v < ShapeFalloff::kFalloffCount) ? v : fallback;
}

float
readSoftness(const KnobReader& reader,
             const KnobDoubleWPtr& knob)
{
    const float v = boundedFloat(reader.valueOr(knob, 0.), kMaxCoordinate, 0.f);

    // The band's coverage is divided by the softness, which overflows for a subnormal one.
    return v >= 1e-6f ? v : 0.f;
}

void
pixelSize(double par,
          double renderScaleX,
          double renderScaleY,
          float* pixelW,
          float* pixelH)
{
    const double w = (par > 0.) && (renderScaleX > 0.) ? par / renderScaleX : 1.;
    const double h = renderScaleY > 0. ? 1. / renderScaleY : 1.;

    *pixelW = boundedFloat(w, kMaxCoordinate, 1.f);
    *pixelH = boundedFloat(h, kMaxCoordinate, 1.f);
}

} // namespace ShapeGenerator

ShapeBoxPixelParams
shapeBoxPixelParams(double x1,
                    double y1,
                    double x2,
                    double y2,
                    double par,
                    double renderScaleX,
                    double renderScaleY)
{
    using ShapeGenerator::boundedFloat;
    using ShapeGenerator::kMaxCoordinate;

    ShapeBoxPixelParams p = {};
    p.x1 = boundedFloat(x1, kMaxCoordinate, 0.f);
    p.y1 = boundedFloat(y1, kMaxCoordinate, 0.f);
    p.x2 = boundedFloat(x2, kMaxCoordinate, 0.f);
    p.y2 = boundedFloat(y2, kMaxCoordinate, 0.f);
    if (!((double)p.x2 - p.x1 >= ShapeGenerator::kMinShapeSize) || !((double)p.y2 - p.y1 >= ShapeGenerator::kMinShapeSize)) {
        p.x2 = p.x1;
        p.y2 = p.y1;
    }
    ShapeGenerator::pixelSize(par, renderScaleX, renderScaleY, &p.pixelW, &p.pixelH);
    p.falloff = ShapeFalloff::kFalloffLinear;

    return p;
}

NATRON_NAMESPACE_EXIT
