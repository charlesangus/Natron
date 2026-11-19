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

#ifndef NATRON_TESTS_DEFOCUSREFERENCE_DEFOCUSMETRIC_H
#define NATRON_TESTS_DEFOCUSREFERENCE_DEFOCUSMETRIC_H

#include <string>
#include <vector>

#include "AnalyticScene.h"

namespace DefocusReference {

/**
 * The pixels near a depth discontinuity of a pinhole depth pass: 1 within the circle of
 * confusion's radius (the larger of the two sides') of any pair of 4-neighbours whose diameters
 * differ by more than a pixel, 0 elsewhere. A depth of 0 or below counts as infinitely far.
 **/
std::vector<unsigned char> edgeBand(const Camera& camera, const std::vector<float>& depth);

/**
 * A candidate's error against the truth, both premultiplied RGBA of the same size. The RMSEs are
 * over every channel. Halo is the colour a candidate adds where the truth is uncovered, the sum of
 * (1 - truth alpha) times its positive RGB error; hole is the colour it loses where the truth is
 * covered, the sum of truth alpha times its negative RGB error. Both are per pixel of the image.
 **/
struct DefocusScore {
    double rmse = 0.;
    double edgeRmse = 0.;
    double halo = 0.;
    double hole = 0.;
    int edgePixels = 0;
};

DefocusScore scoreDefocus(int width,
                          int height,
                          const std::vector<float>& truth,
                          const std::vector<float>& candidate,
                          const std::vector<unsigned char>& band);

/**
 * Writes the window of at most size x size pixels centred on (centreX, centreY) of an RGBA image,
 * clamped to the image, as a zip-compressed half-float EXR. Returns false with `error` set when
 * OpenImageIO cannot write it.
 **/
bool writeCrop(const std::string& path,
               int width,
               int height,
               const std::vector<float>& rgba,
               int centreX,
               int centreY,
               int size,
               std::string* error);

// The candidate minus the truth, with alpha the absolute alpha error.
std::vector<float> differenceImage(const std::vector<float>& truth, const std::vector<float>& candidate);

// The centre of the edge band's pixels, or of the image when the band is empty.
void edgeBandCentre(int width, int height, const std::vector<unsigned char>& band, int* x, int* y);

} // namespace DefocusReference

#endif // NATRON_TESTS_DEFOCUSREFERENCE_DEFOCUSMETRIC_H
