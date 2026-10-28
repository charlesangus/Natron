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

#include "Ramp.h"

#include <cfloat>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/PixelKernel.h"

NATRON_NAMESPACE_ENTER

namespace {
const double kColor0Default[4] = { 0., 0., 0., 0. };
const double kColor1Default[4] = { 1., 1., 1., 1. };

// Points closer than this are coincident: the direction between them is noise.
const double kMinRampLength = 1e-6;

KnobDoublePtr
addPointKnob(KnobHolder* holder,
             const KnobPagePtr& page,
             const char* name,
             const char* label,
             const char* hint,
             double x,
             double y)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr(label).toStdString(), 2);

    knob->setName(name);
    knob->setHintToolTip(EffectInstance::tr(hint).toStdString());
    knob->setSpatial(true);
    knob->disableSlider();
    knob->setDefaultValue(x, 0);
    knob->setDefaultValue(y, 1);
    for (int d = 0; d < 2; ++d) {
        knob->setMinimum(-DBL_MAX, d);
        knob->setMaximum(DBL_MAX, d);
        knob->setDisplayMinimum(-10000., d);
        knob->setDisplayMaximum(10000., d);
    }
    page->addKnob(knob);

    return knob;
}
} // anonymous namespace

void
RampOp::createKnobs(KnobHolder* holder,
                    const KnobPagePtr& page)
{
    point0 = addPointKnob(holder, page, kRampParamPoint0, "Point 0", "Where the ramp is colour 0, in canonical coordinates.", 100., 100.);
    color0 = ShapeGenerator::addColorKnob(holder, page, kRampParamColor0, "Color 0", "The colour at point 0 and before it.", kColor0Default);
    point1 = addPointKnob(holder, page, kRampParamPoint1, "Point 1", "Where the ramp is colour 1, in canonical coordinates.", 100., 200.);
    color1 = ShapeGenerator::addColorKnob(holder, page, kRampParamColor1, "Color 1", "The colour at point 1 and beyond it.", kColor1Default);
    falloff = ShapeGenerator::addFalloffKnob(holder, page, "The curve the ramp follows from colour 0 to colour 1.", ShapeFalloff::kFalloffLinear);
}

RampOp::Params
RampOp::bake(const KnobReader& reader) const
{
    Params params;

    params.p0x = reader.valueOr(point0, 100., 0);
    params.p0y = reader.valueOr(point0, 100., 1);
    params.p1x = reader.valueOr(point1, 100., 0);
    params.p1y = reader.valueOr(point1, 200., 1);
    params.par = generator ? generator->getPixelAspectRatio(reader.time(), reader.view()) : 1.;
    params.falloff = ShapeGenerator::readFalloff(reader, falloff, ShapeFalloff::kFalloffLinear);
    ShapeGenerator::readColor(reader, color0, kColor0Default, params.color0);
    ShapeGenerator::readColor(reader, color1, kColor1Default, params.color1);

    return params;
}

RampPixelParams
RampOp::pixelParams(const Params& params,
                    double renderScaleX,
                    double renderScaleY)
{
    using ShapeGenerator::boundedFloat;
    using ShapeGenerator::kMaxCoordinate;

    RampPixelParams p = {};
    p.p0x = boundedFloat(params.p0x, kMaxCoordinate, 0.f);
    p.p0y = boundedFloat(params.p0y, kMaxCoordinate, 0.f);
    p.p1x = boundedFloat(params.p1x, kMaxCoordinate, 0.f);
    p.p1y = boundedFloat(params.p1y, kMaxCoordinate, 0.f);
    const double dx = (double)p.p1x - p.p0x;
    const double dy = (double)p.p1y - p.p0y;
    if (!(dx * dx + dy * dy >= kMinRampLength * kMinRampLength)) {
        p.p0x = p.p1x;
        p.p0y = p.p1y;
    }
    ShapeGenerator::pixelSize(params.par, renderScaleX, renderScaleY, &p.pixelW, &p.pixelH);
    p.falloff = params.falloff;
    for (int c = 0; c < 4; ++c) {
        p.color0[c] = params.color0[c];
        p.color1[c] = params.color1[c];
    }

    return p;
}

bool
RampOp::isIdentity(const Params& /*params*/,
                   const PassContext& /*context*/) const
{
    return false;
}

void
RampOp::row(const Params& params,
            const PassContext& context,
            const RowIO& io) const
{
    const OfxPointD renderScale = context.scale.toOfxPointD();
    const RampPixelParams p = pixelParams(params, renderScale.x, renderScale.y);
    const GeneratorRowChannels channels(io);
    float* dst = io.dst;

    for (int i = 0; i < io.width; ++i, dst += io.nComps) {
        float rgba[4];
        rampPixel(p, io.x0 + i, io.y, rgba);
        channels.write(rgba, dst);
    }
}

GpuOpRef
RampOp::gpu(const Params& /*params*/,
            const PassContext& /*context*/) const
{
    return GpuOpRef::none();
}

void
RampNode::initializeKnobs()
{
    GeneratorNode<RampOp>::initializeKnobs();

    NodePtr node = getNode();
    if (node) {
        node->addRampInteract(op().point0.lock(), op().point1.lock());
    }
}

NATRON_NAMESPACE_EXIT
