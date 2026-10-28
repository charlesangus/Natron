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

#ifndef Engine_Nodes_Filter_RetouchAlgorithms_h
#define Engine_Nodes_Filter_RetouchAlgorithms_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <vector>

#include "Engine/Nodes/Filter/RetouchRender.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The per-shape algorithms of darktable's retouch module (src/iop/retouch.c) and the colour
 * and bilateral helpers they need. layer is the whole layer (layerRoi at 0, 0), 4 channels, top row
 * first; maskRoi is the region rt_build_scaled_mask() returns, inside the layer.
 **/
namespace RetouchCore {
/**
 * @brief rt_build_scaled_mask() at scale 1: the region of the shape's mask that is inside the layer
 * and whose source, dx, dy away, is too, with darktable's one-pixel shift and the heal's one-pixel
 * padding, and the mask over it (0 outside the shape's mask area). False when the region is empty.
 **/
bool buildRegionMask(const Shape& shape,
                     const Region& layerRoi,
                     int dx,
                     int dy,
                     std::vector<float>* mask,
                     Region* maskRoi);

/// _retouch_clone().
void retouchClone(float* layer,
                  const Region& layerRoi,
                  const float* mask,
                  const Region& maskRoi,
                  int dx,
                  int dy,
                  float opacity);

/// _retouch_heal(), on EdgeAware::poissonHeal().
void retouchHeal(float* layer,
                 const Region& layerRoi,
                 const float* mask,
                 const Region& maskRoi,
                 int dx,
                 int dy,
                 float opacity,
                 int maxIterations,
                 const EdgeAware::ParallelFor& parallel);

/// _retouch_fill() with darktable's fill colour: the brightness, or the colour plus it.
void retouchFill(float* layer,
                 const Region& layerRoi,
                 const float* mask,
                 const Region& maskRoi,
                 float opacity,
                 FillModeEnum mode,
                 const float color[3],
                 float brightness);

/// _retouch_blur(): radius in canonical pixels, times renderScale.
void retouchBlur(float* layer,
                 const Region& layerRoi,
                 const float* mask,
                 const Region& maskRoi,
                 float opacity,
                 BlurTypeEnum type,
                 float radius,
                 float renderScale,
                 const EdgeAware::ParallelFor& parallel);

/// rt_adjust_levels(): the preview levels on the Lab lightness. Nothing at (-3, 0, 3).
void adjustLevels(float* image,
                  int width,
                  int height,
                  const float levels[3],
                  const EdgeAware::ParallelFor& parallel);

/// Linear Rec. 709 to CIE Lab (D50), in place on count 4-channel pixels, as darktable's work profile.
void rgbToLab(float* pixels,
              std::size_t count);
void labToRgb(float* pixels,
              std::size_t count);

/**
 * @brief darktable's bilateral grid (src/common/bilateral.c) with detail -1: the lightness of
 * image (width x height x 4, Lab) replaced by its bilateral base layer, the other channels kept.
 **/
void bilateralBaseLayer(float* image,
                        int width,
                        int height,
                        float sigmaS,
                        float sigmaR);
} // namespace RetouchCore

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_RetouchAlgorithms_h
