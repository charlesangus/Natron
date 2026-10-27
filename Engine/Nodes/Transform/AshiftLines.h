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

#ifndef Engine_Nodes_Transform_AshiftLines_h
#define Engine_Nodes_Transform_AshiftLines_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

#include "Engine/Nodes/Transform/AshiftFit.h"

NATRON_NAMESPACE_ENTER

namespace Ashift {

enum EnhanceFlags {
    eEnhanceNone = 0,
    eEnhanceEdges = 1 << 0,
    eEnhanceDetail = 1 << 1,
};

/**
 * @brief One segment a line segment detector found: its end points in pixel coordinates (pixel
 * (i, j) at (i, j), y down), its width, the angle precision p it was grown with and its -log10(NFA).
 **/
struct Segment {
    double x1 = 0.;
    double y1 = 0.;
    double x2 = 0.;
    double y2 = 0.;
    double width = 1.;
    double p = 0.;
    double logNfa = 0.;
};

/**
 * @brief A line segment detector on a greyscale image of width x height values in [0, 256], row
 * major, top row first.
 **/
using SegmentDetector = std::function<std::vector<Segment>(const std::vector<double>& grey, int width, int height)>;

/**
 * @brief The detector detectLines() uses; none is set until one is registered.
 **/
void setSegmentDetector(const SegmentDetector& detector);
bool hasSegmentDetector();

/**
 * @brief darktable's line_detect() on an RGBA float image (row major, top row first): gamma,
 * detail and edge enhancement, segment detection, then each segment's weight and its
 * classification as vertical, horizontal or irrelevant. xScale (the pixel aspect ratio) maps the
 * segments to square pixels; the line set's width is the image width scaled by it. Returns false,
 * with error set, when no detector is registered or nothing was found.
 **/
bool detectLines(std::vector<float> rgba,
                 int width,
                 int height,
                 double xScale,
                 int enhance,
                 const SegmentDetector& detector,
                 LineSet* lines,
                 std::string* error);

/**
 * @brief _remove_outliers(): RANSAC over the selected vertical lines, then the horizontal ones,
 * deselecting those that do not share the best vanishing point. seed makes the shuffles
 * reproducible where darktable uses rand().
 **/
void removeOutliers(LineSet* lines, unsigned int seed);

/**
 * @brief The four lines of a quadrilateral, darktable's rectangle mode: points are the top left,
 * bottom left, top right and bottom right corners in pixel coordinates, y down.
 **/
LineSet quadLines(const double points[8], int width, int height);

/**
 * @brief Steps of line_detect(), exposed for tests.
 **/
void gammaCorrect(std::vector<float>* rgba);
void detailEnhance(std::vector<float>* rgba, int width, int height);
std::vector<double> rgbToGrey256(const std::vector<float>& rgba, int width, int height);
void edgeEnhance(std::vector<double>* grey, int width, int height);

} // namespace Ashift

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_AshiftLines_h
