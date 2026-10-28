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

#ifndef Engine_Nodes_Filter_DepthOfField_h
#define Engine_Nodes_Filter_DepthOfField_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/// What a depth channel holds: the distance to the camera, or its inverse.
enum class DepthUnits {
    Distance,
    InverseDistance
};

enum class DepthOfFieldMode {
    // The depth of field and the blur size are set directly.
    Manual,
    // The blur follows from a thin lens: focal length, f-stop and film back.
    Lens
};

/**
 * @brief The knobs of a depth-to-circle-of-confusion model. focalPlane and depthOfField are in the
 * depth channel's own units; the circle of confusion comes out as a diameter in canonical pixels.
 **/
struct DepthOfFieldParams {
    DepthUnits units = DepthUnits::InverseDistance;
    DepthOfFieldMode mode = DepthOfFieldMode::Manual;
    // The depth in focus, and in Manual mode the width of the slice of depths around it that is.
    double focalPlane = 1.;
    double depthOfField = 0.;
    // Manual: the diameter far behind the in-focus slice, in canonical pixels.
    double size = 0.;
    // No circle of confusion exceeds this diameter.
    double maxSize = 50.;
    // Lens: in millimetres, and the f-stop.
    double focalLength = 50.;
    double fStop = 2.8;
    double filmBackWidth = 36.;
    // Lens: the image width in canonical pixels, which the film back spans.
    double imageWidth = 0.;
    // Lens: millimetres per unit of distance in the depth channel.
    double millimetresPerUnit = 1000.;
};

/**
 * @brief The distance a raw depth value stands for, +infinity for a value <= 0 or non-finite in
 * either unit.
 **/
double depthDistance(double raw, DepthUnits units) WARN_UNUSED_RETURN;

/**
 * @brief Maps depth to a signed circle of confusion: negative in front of the in-focus slice,
 * positive behind it, 0 inside it, and monotone in distance on each side.
 *
 * Both modes share the thin-lens shape a * |1/E - 1/D|, D the distance and E the nearest edge of
 * the in-focus slice. Lens mode takes a from the lens, f^2 / (N (S - f)) * S, which is
 * f^2 / (N (S - f)) * |D - S| / D; Manual mode chooses it so that infinitely far is Size.
 **/
class DepthOfField {
public:
    explicit DepthOfField(const DepthOfFieldParams& params);

    /// The signed diameter, in canonical pixels, of a raw depth value.
    double coc(double raw) const WARN_UNUSED_RETURN;

    /// The largest diameters in front of and behind the in-focus slice over every depth.
    double frontMax() const WARN_UNUSED_RETURN
    {
        return _frontMax;
    }

    double backMax() const WARN_UNUSED_RETURN
    {
        return _backMax;
    }

    /// The model's terms, for a caller that evaluates coc() elsewhere, as the GPU does.
    DepthUnits units() const WARN_UNUSED_RETURN
    {
        return _units;
    }

    double nearEdge() const WARN_UNUSED_RETURN
    {
        return _near;
    }

    double farEdge() const WARN_UNUSED_RETURN
    {
        return _far;
    }

    double scale() const WARN_UNUSED_RETURN
    {
        return _scale;
    }

    double minDistance() const WARN_UNUSED_RETURN
    {
        return _minDistance;
    }

    double maxSize() const WARN_UNUSED_RETURN
    {
        return _maxSize;
    }

private:
    double cocOfDistance(double distance) const WARN_UNUSED_RETURN;

    DepthUnits _units = DepthUnits::InverseDistance;
    double _maxSize = 0.;
    // The in-focus slice's edges, as distances; near > 0, far >= near, either may be infinite.
    double _near = 0.;
    double _far = 0.;
    // Diameter = _scale * |1/edge - 1/distance|, in canonical pixels; infinite when any distance
    // off the slice clamps to the maximum.
    double _scale = 0.;
    // Lens: distances below this one clamp to the maximum in front.
    double _minDistance = 0.;
    double _frontMax = 0.;
    double _backMax = 0.;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_DepthOfField_h
