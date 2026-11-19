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

#ifndef NATRON_TESTS_DEFOCUSREFERENCE_NIIKLCANDIDATE_H
#define NATRON_TESTS_DEFOCUSREFERENCE_NIIKLCANDIDATE_H

#include <string>
#include <vector>

#include "AnalyticScene.h"

namespace DefocusReference {

enum class NiiklEdgeMode {
    // Every pixel scatters at its own depth.
    Off,
    // An edge pixel splits evenly between the front and back depths around it.
    Snap,
    // An edge pixel splits by where its colour falls between its front and back neighbours'.
    Unmix
};

struct NiiklSettings {
    NiiklEdgeMode edgeMode = NiiklEdgeMode::Unmix;
    // Fill the background behind the foreground before scattering it.
    bool fill = true;
    // Split occluding edges into separate slices, rather than the faster single pass.
    bool splitOcclusionEdges = true;
    // Feed the pinhole's position and normal passes, which the engine uses to tell surfaces apart.
    bool positionNormal = false;
};

// A short name for the settings, such as "unmix-fill-split-pn".
std::string describe(const NiiklSettings& settings);

/**
 * Defocuses a pinhole render through the scatter engine under `camera`'s lens, the image edge
 * repeated. Returns premultiplied RGBA of the pinhole's size, or false with `error` set.
 **/
bool runNiiklDefocus(const Camera& camera,
                     const PinholeImage& pinhole,
                     const NiiklSettings& settings,
                     std::vector<float>* rgba,
                     std::string* error);

} // namespace DefocusReference

#endif // NATRON_TESTS_DEFOCUSREFERENCE_NIIKLCANDIDATE_H
