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

#ifndef Engine_Nodes_Filter_BokehKnobs_h
#define Engine_Nodes_Filter_BokehKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/BokehKernel.h"
#include "Engine/Nodes/Filter/LensAberration.h"

#define kBokehParamKernelTextureGroup "kernelTexture"
#define kBokehParamLensAberrationsGroup "lensAberrations"
#define kBokehParamFilterShape "filterShape"
#define kBokehParamInnerSize "innerSize"
#define kBokehParamInnerFeather "innerFeather"
#define kBokehParamInnerBrightness "innerBrightness"
#define kBokehParamCatadioptric "catadioptric"
#define kBokehParamCatadioptricSize "catadioptricSize"
#define kBokehParamSphericalAberration "sphericalAberration"
#define kBokehParamCatEye "catEye"
#define kBokehParamCatEyeOrientation "catEyeOrientation"
#define kBokehParamCatEyeOrientationRadial "radial"
#define kBokehParamCatEyeOrientationTangential "tangential"
#define kBokehParamChromaticAberration "chromaticAberration"
#define kBokehParamChromaticOffset "chromaticOffset"

NATRON_NAMESPACE_ENTER

class KnobReader;

/**
 * @brief The kernel texture and lens aberration knobs Defocus and ZDefocus share, under the same
 * script names on both.
 **/
class BokehKnobs {
public:
    /// The texture knobs of Disc and Blades kernels, Nuke's ZDefocus filter controls, in a closed group.
    void addTextureKnobs(KnobHolder* holder, const KnobPagePtr& page);

    /// The cat-eye and chromatic aberration knobs, in a closed group.
    void addLensKnobs(KnobHolder* holder, const KnobPagePtr& page);

    /// The texture knobs' values into shape, leaving its other fields.
    void readTexture(const KnobReader& reader, BokehShapeParams* shape) const;

    LensAberrationParams readLens(const KnobReader& reader) const WARN_UNUSED_RETURN;

private:
    KnobDoubleWPtr _filterShape;
    KnobDoubleWPtr _innerSize;
    KnobDoubleWPtr _innerFeather;
    KnobDoubleWPtr _innerBrightness;
    KnobBoolWPtr _catadioptric;
    KnobDoubleWPtr _catadioptricSize;
    KnobDoubleWPtr _sphericalAberration;
    KnobDoubleWPtr _catEye;
    KnobChoiceWPtr _catEyeOrientation;
    KnobDoubleWPtr _chromaticAberration;
    KnobColorWPtr _chromaticOffset;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_BokehKnobs_h
