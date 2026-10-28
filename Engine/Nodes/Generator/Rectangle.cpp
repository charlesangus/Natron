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

#include "Rectangle.h"

#include <cfloat>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

namespace {
const double kColor0Default[4] = { 0., 0., 0., 0. };
const double kColor1Default[4] = { 1., 1., 1., 1. };
} // anonymous namespace

void
RectangleOp::createKnobs(KnobHolder* holder,
                         const KnobPagePtr& page)
{
    KnobDoublePtr corner = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Corner Radius").toStdString(), 2);
    corner->setName(kRectangleParamCornerRadius);
    corner->setHintToolTip(EffectInstance::tr("The horizontal and vertical radii of the rounded corners, in canonical pixels. "
                                              "Each is clamped to half the rectangle's shorter side.")
                               .toStdString());
    corner->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        corner->setDefaultValue(0., d);
        corner->setMinimum(0., d);
        corner->setMaximum(DBL_MAX, d);
        corner->setDisplayMinimum(0., d);
        corner->setDisplayMaximum(100., d);
    }
    page->addKnob(corner);
    cornerRadius = corner;
    softness = ShapeGenerator::addSoftnessKnob(holder, page, "How far inside the edge, in canonical pixels, the rectangle fades from colour 1 to colour 0. At 0 the edge is hard but antialiased.");
    falloff = ShapeGenerator::addFalloffKnob(holder, page, "The curve the edge follows across its softness.", ShapeFalloff::kFalloffQuadratic);
    color0 = ShapeGenerator::addColorKnob(holder, page, kRectangleParamColor0, "Color 0", "The colour outside the rectangle.", kColor0Default);
    color1 = ShapeGenerator::addColorKnob(holder, page, kRectangleParamColor1, "Color 1", "The colour inside the rectangle.", kColor1Default);
}

RectangleOp::Params
RectangleOp::bake(const KnobReader& reader) const
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
    params.cornerX = reader.valueOr(cornerRadius, 0., 0);
    params.cornerY = reader.valueOr(cornerRadius, 0., 1);
    params.softness = ShapeGenerator::readSoftness(reader, softness);
    params.falloff = ShapeGenerator::readFalloff(reader, falloff, ShapeFalloff::kFalloffQuadratic);
    ShapeGenerator::readColor(reader, color0, kColor0Default, params.color0);
    ShapeGenerator::readColor(reader, color1, kColor1Default, params.color1);

    return params;
}

ShapeBoxPixelParams
RectangleOp::pixelParams(const Params& params,
                         double renderScaleX,
                         double renderScaleY)
{
    ShapeBoxPixelParams p = shapeBoxPixelParams(params.x1, params.y1, params.x2, params.y2, params.par, renderScaleX, renderScaleY);

    p.cornerX = ShapeGenerator::boundedFloat(params.cornerX, ShapeGenerator::kMaxCoordinate, 0.f);
    p.cornerY = ShapeGenerator::boundedFloat(params.cornerY, ShapeGenerator::kMaxCoordinate, 0.f);
    p.softness = params.softness;
    p.falloff = params.falloff;
    for (int c = 0; c < 4; ++c) {
        p.color0[c] = params.color0[c];
        p.color1[c] = params.color1[c];
    }

    return p;
}

bool
RectangleOp::isIdentity(const Params& /*params*/,
                        const PassContext& /*context*/) const
{
    return false;
}

void
RectangleOp::row(const Params& params,
                 const PassContext& context,
                 const RowIO& io) const
{
    const OfxPointD renderScale = context.scale.toOfxPointD();
    const ShapeBoxPixelParams p = pixelParams(params, renderScale.x, renderScale.y);
    const GeneratorRowChannels channels(io);
    float* dst = io.dst;

    for (int i = 0; i < io.width; ++i, dst += io.nComps) {
        float rgba[4];
        rectanglePixel(p, io.x0 + i, io.y, rgba);
        channels.write(rgba, dst);
    }
}

GpuOpRef
RectangleOp::gpu(const Params& /*params*/,
                 const PassContext& /*context*/) const
{
    return GpuOpRef::none();
}

NATRON_NAMESPACE_EXIT
