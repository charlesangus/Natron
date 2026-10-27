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

/*
 * The 3DE4 models of openfx-misc's DistortionModel.cpp, which follow the 3DE4 Lens Distortion
 * Plugin Kit (ldpk) classes named on each parameter struct.
 */

#ifndef Engine_Nodes_Transform_LensModels3DE_h
#define Engine_Nodes_Transform_LensModels3DE_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Transform/LensModels.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The 3DE4 models. The core undistorts in 3DE4's diagonally normalized (dn) space; the
 * field-of-view and filmback mapping from pixels to dn, and each model's linear pre and post
 * stages (pixel aspect, rotation, squeeze, cylindric bending), are folded into Model::toCore and
 * Model::fromCore. Parameter names are 3DE4's.
 **/
namespace LensModels {
/// The built-in 3DE4 camera parameters every model shares.
struct Camera3DE {
    double xa_fov_unit, ya_fov_unit, xb_fov_unit, yb_fov_unit;
    double fl_cm;
    double fd_cm;
    double w_fb_cm, h_fb_cm;
    double x_lco_cm, y_lco_cm;
    double pa;
};

/// "3DE Classic LD Model" (ldpk classic_3de_mixed_distortion).
struct Classic3DEParams {
    double ld; // Distortion
    double sq; // Anamorphic Squeeze
    double cx; // Curvature X
    double cy; // Curvature Y
    double qu; // Quartic Distortion
};

/// "3DE4 Anamorphic, Degree 6" (ldpk generic_anamorphic_distortion, degree 6).
struct Anamorphic6Params {
    double cx02, cy02, cx22, cy22;
    double cx04, cy04, cx24, cy24, cx44, cy44;
    double cx06, cy06, cx26, cy26, cx46, cy46, cx66, cy66;
};

/// "3DE4 Radial - Fisheye, Degree 8" (ldpk tde4_ldp_radial_deg_8, equisolid-angle projection).
struct Fisheye8Params {
    double c2, c4, c6, c8;
};

/// "3DE4 Radial - Standard, Degree 4" (ldpk radial_decentered_distortion and cylindric_extender_2).
struct Standard3DEParams {
    double c2, u1, v1;
    double c4, u3, v3;
    /// Cylindric direction, degrees.
    double phi;
    /// Cylindric bending.
    double b;
};

/// "3DE4 Anamorphic - Standard, Degree 4" (ldpk tde4_ldp_anamorphic_deg_4_rotate_squeeze_xy).
struct Anamorphic4Params {
    double cx02, cy02, cx22, cy22;
    double cx04, cy04, cx24, cy24, cx44, cy44;
    /// Lens rotation, degrees.
    double phi;
    double sqx, sqy;
};

Model make3DEClassic(const Frame& frame, const Camera3DE& camera, const Classic3DEParams& params);
Model make3DEAnamorphic6(const Frame& frame, const Camera3DE& camera, const Anamorphic6Params& params);
Model make3DEFisheye8(const Frame& frame, const Camera3DE& camera, const Fisheye8Params& params);
Model make3DEStandard(const Frame& frame, const Camera3DE& camera, const Standard3DEParams& params);
Model make3DEAnamorphic4(const Frame& frame, const Camera3DE& camera, const Anamorphic4Params& params);

/// Classic, Anamorphic Degree 6, and Anamorphic Degree 4: one polynomial, with the terms a model lacks at zero.
bool coreAnamorphic3DE(const double* c, double x, double y, double* gx, double* gy, double J[4]);
bool coreFisheye3DE(const double* c, double x, double y, double* gx, double* gy, double J[4]);
bool coreStandard3DE(const double* c, double x, double y, double* gx, double* gy, double J[4]);
} // namespace LensModels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_LensModels3DE_h
