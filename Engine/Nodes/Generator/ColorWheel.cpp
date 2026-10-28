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

#include "ColorWheel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
KnobDoublePtr
addDoubleKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const char* label,
              const char* hint,
              double defaultValue,
              double minimum,
              double maximum,
              double displayMinimum,
              double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr(label).toStdString(), 1);

    knob->setName(name);
    knob->setHintToolTip(EffectInstance::tr(hint).toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(displayMinimum);
    knob->setDisplayMaximum(displayMaximum);
    page->addKnob(knob);

    return knob;
}

float
readUnit(const KnobReader& reader,
         const KnobDoubleWPtr& knob,
         double fallback)
{
    const double v = reader.valueOr(knob, fallback);

    return v == v ? (float)(std::max)(0., (std::min)(1., v)) : (float)fallback;
}

float
readValue(const KnobReader& reader,
          const KnobDoubleWPtr& knob,
          double fallback)
{
    const float v = ShapeGenerator::boundedFloat(reader.valueOr(knob, fallback), ShapeGenerator::kMaxColor, (float)fallback);

    return v > 0.f ? v : 0.f;
}
} // anonymous namespace

void
ColorWheelOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    centerSaturation = addDoubleKnob(holder, page, kColorWheelParamCenterSaturation, "Center Saturation",
                                     "The HSV saturation at the centre of the wheel.", 0., 0., 1., 0., 1.);
    edgeSaturation = addDoubleKnob(holder, page, kColorWheelParamEdgeSaturation, "Edge Saturation",
                                   "The HSV saturation at the edge of the wheel.", 1., 0., 1., 0., 1.);
    centerValue = addDoubleKnob(holder, page, kColorWheelParamCenterValue, "Center Value",
                                "The HSV value at the centre of the wheel. Above 1 is HDR.", 1., 0., DBL_MAX, 0., 1.);
    edgeValue = addDoubleKnob(holder, page, kColorWheelParamEdgeValue, "Edge Value",
                              "The HSV value at the edge of the wheel. Above 1 is HDR.", 1., 0., DBL_MAX, 0., 1.);
    rotate = addDoubleKnob(holder, page, kColorWheelParamRotate, "Rotate",
                           "The rotation of the hues around the wheel, in degrees. Negative values rotate clockwise.",
                           0., -DBL_MAX, DBL_MAX, -180., 180.);
    softness = ShapeGenerator::addSoftnessKnob(holder, page,
                                               "How far inside the wheel's edge, in canonical pixels, its alpha fades to 0. At 0 the edge is hard but antialiased.");
}

ColorWheelOp::Params
ColorWheelOp::bake(const KnobReader& reader) const
{
    Params params;
    RectD rod;

    if (generator) {
        rod = generator->getRegionOfDefinition(reader.time(), reader.view());
        params.par = generator->getPixelAspectRatio(reader.time(), reader.view());
    } else {
        params.par = 1.;
    }
    params.x1 = rod.x1;
    params.y1 = rod.y1;
    params.x2 = rod.x2;
    params.y2 = rod.y2;
    params.rotate = reader.valueOr(rotate, 0.);
    params.softness = ShapeGenerator::readSoftness(reader, softness);
    params.centerSaturation = readUnit(reader, centerSaturation, 0.);
    params.edgeSaturation = readUnit(reader, edgeSaturation, 1.);
    params.centerValue = readValue(reader, centerValue, 1.);
    params.edgeValue = readValue(reader, edgeValue, 1.);

    return params;
}

ColorWheelPixelParams
ColorWheelOp::pixelParams(const Params& params,
                          double renderScaleX,
                          double renderScaleY)
{
    const ShapeBoxPixelParams box = shapeBoxPixelParams(params.x1, params.y1, params.x2, params.y2, params.par, renderScaleX, renderScaleY);
    ColorWheelPixelParams p = {};

    p.cx = 0.5f * (box.x1 + box.x2);
    p.cy = 0.5f * (box.y1 + box.y2);
    p.radius = 0.5f * (std::min)(box.x2 - box.x1, box.y2 - box.y1);
    p.softness = params.softness;
    p.pixelW = box.pixelW;
    p.pixelH = box.pixelH;
    p.centerSaturation = params.centerSaturation;
    p.edgeSaturation = params.edgeSaturation;
    p.centerValue = params.centerValue;
    p.edgeValue = params.edgeValue;
    const double rotate = std::isfinite(params.rotate) ? std::fmod(params.rotate, 360.) : 0.;
    double turns = rotate / 360.;
    turns -= std::floor(turns);
    p.rotateTurns = turns < 1. ? (float)turns : 0.f;

    return p;
}

bool
ColorWheelOp::isIdentity(const Params& /*params*/,
                         const PassContext& /*context*/) const
{
    return false;
}

void
ColorWheelOp::row(const Params& params,
                  const PassContext& context,
                  const RowIO& io) const
{
    const OfxPointD renderScale = context.scale.toOfxPointD();
    const ColorWheelPixelParams p = pixelParams(params, renderScale.x, renderScale.y);
    const GeneratorRowChannels channels(io);
    float* dst = io.dst;

    for (int i = 0; i < io.width; ++i, dst += io.nComps) {
        float rgba[4];
        colorWheelPixel(p, io.x0 + i, io.y, rgba);
        channels.write(rgba, dst);
    }
}

GpuOpRef
ColorWheelOp::gpu(const Params& params,
                  const PassContext& context) const
{
#ifdef NATRON_HAVE_GPU
    const OfxPointD renderScale = context.scale.toOfxPointD();

    return GpuOpRef::compiled((int)gpu::GpuKernelId::colorwheel, pixelParams(params, renderScale.x, renderScale.y));
#else
    Q_UNUSED(params);
    Q_UNUSED(context);

    return GpuOpRef::none();
#endif
}

NATRON_NAMESPACE_EXIT
