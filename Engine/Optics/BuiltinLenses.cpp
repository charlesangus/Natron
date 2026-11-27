/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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
 * The built-in prescriptions, ported from Niik-l's LensTrace (niikl/RB-2.6
 * Engine/Dev/PostFX/LensTrace.cpp, GPL-2.0-or-later) with their citations.
 *
 * Rows are in the pbrt / Kolb format: radius, thickness, n_d, clear diameter (mm), and the Abbe
 * number where it is known. A row with radius 0 and index 0 is the stop. The first four are as
 * published with pbrt's example scenes, which cite the original designs in W. J. Smith, "Modern
 * Lens Design", and carry no Abbe numbers.
 */

#include "Engine/Optics/Prescription.h"

namespace Natron {
namespace Optics {

    namespace {

        struct BuiltinRow {
            double r, t, n, ap, v = 0.;
        };

        struct Builtin {
            const char* id;
            const char* name;
            const char* source;
            std::vector<BuiltinRow> rows;
        };

        const std::vector<Builtin>&
        builtins()
        {
            static const std::vector<Builtin> b = {
                { "dgauss50", "Double-Gauss 50mm f/2", "Double-Gauss f/2, 22 deg half field. US patent 2,673,491 (Tronnier); W. J. Smith, Modern Lens Design p. 312; scaled to 50 mm", { { 29.475, 3.76, 1.67, 25.2 }, { 84.83, 0.12, 1, 25.2 }, { 19.275, 4.025, 1.67, 23 }, { 40.77, 3.275, 1.699, 23 }, { 12.75, 5.705, 1, 18 }, { 0, 4.5, 0, 17.1 }, { -14.495, 1.18, 1.603, 17 }, { 40.77, 6.065, 1.658, 20 }, { -20.385, 0.19, 1, 20 }, { 437.065, 3.22, 1.717, 20 }, { -39.73, 0, 1, 20 } } },
                { "wide22", "Wide-angle 22mm (38 deg)", "Wide-angle, 38 deg (Nakamura). W. J. Smith, Modern Lens Design p. 360; scaled to 22 mm", { { 35.98738, 1.21638, 1.54, 23.716 }, { 11.69718, 9.9957, 1, 17.996 }, { 13.08714, 5.12622, 1.772, 12.364 }, { -22.63294, 1.76924, 1.617, 9.812 }, { 71.05802, 0.8184, 1, 9.152 }, { 0, 2.27766, 0, 8.756 }, { -9.58584, 2.43254, 1.617, 8.184 }, { -11.28864, 0.11506, 1, 9.152 }, { -166.7765, 3.09606, 1.713, 10.648 }, { -7.5911, 1.32682, 1.805, 11.44 }, { -16.7662, 3.98068, 1, 12.276 }, { -7.70286, 1.21638, 1.617, 13.42 }, { -11.97328, 0, 1, 17.996 } } },
                { "tele250", "Telephoto 250mm f/5.6", "Super-achromat telephoto f/5.6 (Sigler). W. J. Smith, Modern Lens Design p. 175; scaled to 250 mm", { { 54.6275, 12.52, 1.529, 47.5 }, { -86.365, 3.755, 1.599, 44.5 }, { 271.7625, 2.8175, 1, 41.5 }, { 0, 67.4125, 0, 40.5 }, { -32.13, 3.755, 1.613, 31.5 }, { 49.5325, 12.52, 1.603, 33.5 }, { -50.945, 0, 1, 37 } } },
                { "fisheye10", "Fisheye 10mm (156 deg)", "Fisheye 16mm f/4, 155.9 deg (Muller). W. J. Smith, Modern Lens Design p. 164; scaled to 10 mm", { { 30.2249, 0.8335, 1.62, 30.34 }, { 11.3931, 7.4136, 1, 20.68 }, { 75.2019, 1.0654, 1.639, 17.8 }, { 8.3349, 11.1549, 1, 13.42 }, { 9.5882, 2.0054, 1.654, 9.02 }, { 43.8677, 5.3895, 1, 8.14 }, { 0, 1.4163, 0, 6.08 }, { 29.4541, 2.1934, 1.517, 5.96 }, { -5.2265, 0.9714, 1.805, 5.84 }, { -14.2884, 0.0627, 1, 5.96 }, { -22.3726, 0.94, 1.673, 5.96 }, { -15.0404, 0, 1, 6.52 } } },
                { "triplet50", "Cooke Triplet 50mm f/4", "Cooke triplet f/4, 20 deg half field (Geiser, Optical Engineering; via J. Sasian, OPTI 517 lecture 20). Glass LaK9 / SF5; clear apertures computed (full beam to 0.7 field)", { { 23.713, 4.831, 1.691, 18.417, 54.71 }, { 7331.288, 5.06, 1, 15.457 }, { 0, 0.8, 0, 9.622 }, { -24.456, 0.975, 1.6727, 9.902, 32.21 }, { 21.896, 4.822, 1, 9.804 }, { 86.759, 3.127, 1.691, 14.762, 54.71 }, { -20.4942, 0, 1, 15.873 } } },
                { "tessar50", "Tessar 50mm f/3.5", "Zeiss Tessar 50mm f/3.5, 48 deg. CH 314381 A example 1, data via B. Claff, PhotonsToPhotos OpticalBench (clear diameters estimated there)", { { 15.1503, 3.059, 1.62041, 15.64, 60.3 }, { 380.5595, 3.174, 1, 14.76 }, { -34.9072, 0.889, 1.59551, 13.48, 39.2 }, { 12.7472, 2.05, 1, 12.76 }, { 0, 0.417, 0, 11.586 }, { 329.5515, 0.861, 1.58144, 12.66, 40.8 }, { 10.8812, 6.214, 1.67003, 13.5, 47.2 }, { -24.3596, 0, 1, 13.5 } } },
                { "sonnar50", "Sonnar 50mm f/1.5", "Zeiss Sonnar 50mm f/1.5 (Contax). US 2,186,621 fig. 1 (Bertele), f=100 halved; stop position and clear diameters estimated by B. Claff, PhotonsToPhotos OpticalBench", { { 34.605, 4.665, 1.671, 33.35, 47.2 }, { 216.92, 0.19, 1, 33.35 }, { 17.93, 5.905, 1.671, 29, 47.2 }, { 42.89, 3.525, 1.4892, 29, 70.1 }, { -323.155, 0.95, 1.7394, 29, 28.2 }, { 11.755, 6.49, 1, 19.56 }, { 0, 1.13, 0, 19.076 }, { 0, 1.24, 1.5232, 20.08, 50.9 }, { 25.545, 9.905, 1.6578, 20.08, 51.2 }, { -11.06, 2.285, 1.5894, 20.08, 61.2 }, { -51.565, 0, 1, 23.66 } } },
                { "petzval100", "Petzval 100mm f/2.4", "Petzval portrait type f/2.4 (M. J. Kidger, Fundamental Optical Design p.191, via the COMSOL Petzval Lens model); field flattener removed for the classic curved field and swirl, clear apertures enlarged to cover full frame with natural vignetting", { { 98.45049, 13, 1.5168, 70.089, 64.17 }, { -83.29386, 4, 1.65412, 63.735, 39.7 }, { -1421.38828, 40, 1, 61.94 }, { 0, 40, 0, 31.153 }, { 59.00613, 12, 1.60738, 51.805, 56.65 }, { -54.3647, 3, 1.6727, 50.817, 32.25 }, { -549.36547, 0, 1, 50.581 } } },
                { "planar50", "Planar 50mm f/1.4", "Zeiss Planar T 50mm f/1.4 double-Gauss. US 3,874,771 example 5, data via B. Claff, PhotonsToPhotos OpticalBench (clear diameters estimated there)", { { 44.452, 5.09105, 1.717, 37.5, 47.99 }, { 288.6605, 0.05885, 1, 37.5 }, { 21.8035, 4.85565, 1.78831, 32.71, 47.37 }, { 34.062, 1.5891, 1, 32.71 }, { 47.086, 1.18695, 1.68893, 31.19, 31.17 }, { 15.324, 7.136325, 1, 24.88 }, { 0, 7.136325, 0, 24.289 }, { -17.3185, 1.1477, 1.7283, 25.2, 28.68 }, { 374.0215, 5.1499, 1.78831, 30.35, 47.37 }, { -30.357, 0.28445, 1, 30.35 }, { -87.426, 4.3848, 1.78831, 31.61, 47.37 }, { -28.25, 0.1275, 1, 31.61 }, { 164.6795, 2.8349, 1.744, 30.57, 44.77 }, { -130.808, 0, 1, 30.57 } } },
            };
            return b;
        }

    } // namespace

    std::vector<std::string>
    builtinLensIds()
    {
        std::vector<std::string> ids;
        for (const Builtin& b : builtins()) {
            ids.push_back(b.id);
        }

        return ids;
    }

    bool
    builtinLens(const std::string& id,
                Prescription* out)
    {
        for (const Builtin& b : builtins()) {
            if (id != b.id) {
                continue;
            }
            Prescription p;
            p.id = b.id;
            p.name = b.name;
            p.source = b.source;
            for (const BuiltinRow& r : b.rows) {
                Surface s;
                s.type = r.r == 0. && r.n == 0. ? SurfaceType::Stop : SurfaceType::Standard;
                s.curvature = r.r == 0. ? 0. : 1. / r.r;
                s.spacing = r.t;
                s.nd = r.n <= 0. ? 1. : r.n;
                s.vd = r.v;
                s.semiDiameter = 0.5 * r.ap;
                p.surfaces.push_back(s);
            }
            *out = p;

            return true;
        }

        return false;
    }

} // namespace Optics
} // namespace Natron
