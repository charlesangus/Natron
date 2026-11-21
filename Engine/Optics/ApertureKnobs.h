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

#ifndef Engine_Optics_ApertureKnobs_h
#define Engine_Optics_ApertureKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Optics/Aperture.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace Optics {

/**
 * @brief Knobs ApertureKnobs::create() leaves out, for a caller that already has them; params() then
 * returns ApertureParams' default for them, for the caller to replace.
 **/
struct ApertureKnobOptions {
    bool blades = true;
    bool rotation = true;
};

/**
 * @brief The aperture controls as knobs, in a Shape group (open) and Grating, Scratches, Dust and
 * Image groups (closed). Script names are the prefix followed by the control's name (prefix "aperture"
 * gives apertureSize, apertureGratingStrength, ...), labels labelPrefix followed by the readable name.
 **/
class ApertureKnobs {
public:
    static ApertureKnobs create(KnobHolder* holder,
                                const KnobPagePtr& page,
                                const std::string& prefix,
                                const std::string& labelPrefix,
                                const ApertureKnobOptions& options = ApertureKnobOptions());

    ApertureParams params(double time,
                          ViewIdx view = ViewIdx(0)) const WARN_UNUSED_RETURN;

private:
    KnobDoubleWPtr _size;
    KnobIntWPtr _blades;
    KnobDoubleWPtr _roundness;
    KnobDoubleWPtr _rotation;
    KnobDoubleWPtr _softness;

    KnobDoubleWPtr _gratingStrength;
    KnobDoubleWPtr _gratingDensity;
    KnobDoubleWPtr _gratingLength;
    KnobDoubleWPtr _gratingWidth;
    KnobDoubleWPtr _gratingSoftness;

    KnobDoubleWPtr _scratchesStrength;
    KnobDoubleWPtr _scratchesDensity;
    KnobDoubleWPtr _scratchesLength;
    KnobDoubleWPtr _scratchesWidth;
    KnobDoubleWPtr _scratchesRotation;
    KnobDoubleWPtr _scratchesRotationVariation;
    KnobDoubleWPtr _scratchesSoftness;
    KnobDoubleWPtr _scratchesParallax;

    KnobDoubleWPtr _dustStrength;
    KnobDoubleWPtr _dustDensity;
    KnobDoubleWPtr _dustRadius;
    KnobDoubleWPtr _dustSoftness;
    KnobDoubleWPtr _dustParallax;

    KnobDoubleWPtr _imageStrength;
    KnobDoubleWPtr _imageSize;
    KnobDoubleWPtr _imageBlack;
    KnobDoubleWPtr _imageWhite;
};

} // namespace Optics

NATRON_NAMESPACE_EXIT

#endif // Engine_Optics_ApertureKnobs_h
