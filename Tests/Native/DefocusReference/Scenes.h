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

#ifndef NATRON_TESTS_DEFOCUSREFERENCE_SCENES_H
#define NATRON_TESTS_DEFOCUSREFERENCE_SCENES_H

#include <string>
#include <vector>

#include "AnalyticScene.h"

namespace DefocusReference {

/**
 * One of the harness's fixture scenes, built for an image size: the same geometry and lens at
 * every size, so the circles of confusion scale with the width. Each leaves empty sky above its
 * farthest surface, so its alpha coverage is not the whole frame.
 **/
struct FixtureScene {
    std::string name;
    AnalyticScene scene;
    PinholeOptions pinhole;
    // Bounds on the planar depth of every surface the camera sees, the nearest one at nearDepth.
    double nearDepth = 0.;
    double farDepth = 0.;
};

/**
 * sharpFgBlurBg, blurFgSharpBg, aliasedEdge, grazingFloor, highlightsBehindEdge and
 * eclipsedBokeh: a sharp foreground over a blurred background, a blurred foreground over a sharp
 * one, a diagonal edge whose depth is averaged over 4x4 samples, a floor seen at a grazing angle,
 * emissive points behind a sharp foreground edge, and emissive points behind a blurred one.
 **/
const std::vector<std::string>& fixtureSceneNames();

// The scene `name` at width x height; an unknown name gives an empty one.
FixtureScene makeFixtureScene(const std::string& name, int width, int height);

} // namespace DefocusReference

#endif // NATRON_TESTS_DEFOCUSREFERENCE_SCENES_H
