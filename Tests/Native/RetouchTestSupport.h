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

#ifndef Tests_Native_RetouchTestSupport_h
#define Tests_Native_RetouchTestSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Bezier.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Retouch.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoItem.h"

NATRON_NAMESPACE_ENTER

namespace RetouchTest {
template <typename T>
std::shared_ptr<T>
shapeKnob(const RotoDrawableItemPtr& item,
          const char* name)
{
    return item ? std::dynamic_pointer_cast<T>(item->getKnobByName(name)) : std::shared_ptr<T>();
}

struct ShapeSpec {
    double cx = 24.;
    double cy = 24.;
    double diameter = 16.;
    RetouchCore::AlgorithmEnum algorithm = RetouchCore::eAlgorithmHeal;
    int scale = 0;
    double sourceX = 0.;
    double sourceY = 0.;
    double opacity = 1.;
    double feather = 0.;
    RetouchCore::BlurTypeEnum blurType = RetouchCore::eBlurTypeGaussian;
    double blurRadius = 10.;
    RetouchCore::FillModeEnum fillMode = RetouchCore::eFillModeErase;
    double fillColor[3] = { 0., 0., 0. };
    double fillBrightness = 0.;
};

/// Sets shape's Retouch knobs, opacity and feather to spec's. False when a knob is missing.
inline bool
applySpec(const BezierPtr& shape,
          const ShapeSpec& spec)
{
    KnobChoicePtr algorithm = shapeKnob<KnobChoice>(shape, kRetouchShapeParamAlgorithm);
    KnobIntPtr scale = shapeKnob<KnobInt>(shape, kRetouchShapeParamScale);
    KnobDoublePtr source = shapeKnob<KnobDouble>(shape, kRetouchShapeParamSourceOffset);
    KnobChoicePtr blurType = shapeKnob<KnobChoice>(shape, kRetouchShapeParamBlurType);
    KnobDoublePtr blurRadius = shapeKnob<KnobDouble>(shape, kRetouchShapeParamBlurRadius);
    KnobChoicePtr fillMode = shapeKnob<KnobChoice>(shape, kRetouchShapeParamFillMode);
    KnobColorPtr fillColor = shapeKnob<KnobColor>(shape, kRetouchShapeParamFillColor);
    KnobDoublePtr fillBrightness = shapeKnob<KnobDouble>(shape, kRetouchShapeParamFillBrightness);
    KnobDoublePtr opacity = shapeKnob<KnobDouble>(shape, "opacity");
    KnobDoublePtr feather = shapeKnob<KnobDouble>(shape, "feather");
    if (!algorithm || !scale || !source || !blurType || !blurRadius || !fillMode || !fillColor || !fillBrightness || !opacity || !feather) {
        return false;
    }
    algorithm->setValue((int)spec.algorithm);
    scale->setValue(spec.scale);
    source->setValue(spec.sourceX, ViewSpec::all(), 0);
    source->setValue(spec.sourceY, ViewSpec::all(), 1);
    blurType->setValue((int)spec.blurType);
    blurRadius->setValue(spec.blurRadius);
    fillMode->setValue((int)spec.fillMode);
    for (int d = 0; d < 3; ++d) {
        fillColor->setValue(spec.fillColor[d], ViewSpec::all(), d);
    }
    fillBrightness->setValue(spec.fillBrightness);
    opacity->setValue(spec.opacity);
    feather->setValue(spec.feather);

    return true;
}

/// An ellipse on the node's curve context with spec's settings, keyed at time.
inline BezierPtr
makeShape(const NodePtr& node,
          const ShapeSpec& spec,
          double time = 1.)
{
    RotoCurveContextPtr context = node ? node->getCurveContext() : RotoCurveContextPtr();
    if (!context) {
        return BezierPtr();
    }
    BezierPtr shape = context->makeEllipse(spec.cx, spec.cy, spec.diameter, true, time);
    // The context selects a new shape, slaving its knobs to the node's: deselect it so its own
    // values are the ones set and read.
    context->clearSelection(RotoItem::eSelectionReasonOther);
    if (!shape || !applySpec(shape, spec)) {
        return BezierPtr();
    }

    return shape;
}

/// One point of a closed Bezier: its position and its left (incoming) and right (outgoing) handles.
struct ShapePoint {
    double x, y, lx, ly, rx, ry;
};

/// A closed Bezier through points on the node's curve context with spec's settings, keyed at time.
inline BezierPtr
makeBezierShape(const NodePtr& node,
                const std::vector<ShapePoint>& points,
                const ShapeSpec& spec,
                double time = 1.)
{
    RotoCurveContextPtr context = node ? node->getCurveContext() : RotoCurveContextPtr();
    if (!context || points.empty()) {
        return BezierPtr();
    }
    BezierPtr shape = context->makeBezier(points[0].x, points[0].y, kRotoBezierBaseName, time, false);
    if (!shape) {
        return shape;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        shape->addControlPoint(points[i].x, points[i].y, time);
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        shape->setLeftBezierPoint((int)i, time, points[i].lx, points[i].ly);
        shape->setRightBezierPoint((int)i, time, points[i].rx, points[i].ry);
    }
    shape->setCurveFinished(true);
    context->clearSelection(RotoItem::eSelectionReasonOther);
    if (!applySpec(shape, spec)) {
        return BezierPtr();
    }

    return shape;
}

/**
 * The four-point Bezier of an ellipse centred on (cx, cy) with radii rx, ry, turned by rotation
 * degrees counter-clockwise, y up. Its radius strays from the ellipse's by under 0.03 %.
 **/
inline std::vector<ShapePoint>
ellipsePoints(double cx,
              double cy,
              double rx,
              double ry,
              double rotation)
{
    const double k = 4. / 3. * (std::sqrt(2.) - 1.);
    const double a = rotation * M_PI / 180.;
    const double c = std::cos(a);
    const double s = std::sin(a);
    std::vector<ShapePoint> points;

    for (int q = 0; q < 4; ++q) {
        const double phi = q * M_PI / 2.;
        const double ex = rx * std::cos(phi);
        const double ey = ry * std::sin(phi);
        const double tx = -rx * std::sin(phi) * k;
        const double ty = ry * std::cos(phi) * k;
        const double px = cx + c * ex - s * ey;
        const double py = cy + s * ex + c * ey;
        const double dx = c * tx - s * ty;
        const double dy = s * tx + c * ty;
        points.push_back({ px, py, px - dx, py - dy, px + dx, py + dy });
    }

    return points;
}
} // namespace RetouchTest

NATRON_NAMESPACE_EXIT

#endif // Tests_Native_RetouchTestSupport_h
