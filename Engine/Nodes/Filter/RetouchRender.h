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

#ifndef Engine_Nodes_Filter_RetouchRender_h
#define Engine_Nodes_Filter_RetouchRender_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The image processing of the Retouch node, ported from darktable's src/iop/retouch.c: the
 * image is split into à-trous wavelet scales and each shape applies its algorithm on its scale,
 * in order, before the scales are recomposed.
 *
 * Images are 4-channel interleaved floats, top row first, as in darktable. Shapes come
 * rasterised, so this has no Natron dependency and the tests run it directly.
 **/
namespace RetouchCore {
enum AlgorithmEnum {
    eAlgorithmClone = 0,
    eAlgorithmHeal,
    eAlgorithmBlur,
    eAlgorithmFill,
};

enum BlurTypeEnum {
    eBlurTypeGaussian = 0,
    eBlurTypeBilateral,
};

enum FillModeEnum {
    eFillModeErase = 0,
    eFillModeColor,
};

enum OutputEnum {
    eOutputResult = 0,
    eOutputCurrentScale,
    eOutputResidual,
};

/// The most detail scales, as in darktable. Scale 0 is the image before the decomposition.
const int kMaxScales = 15;
/// darktable's preview levels range and defaults.
const float kPreviewLevelMin = -3.f;
const float kPreviewLevelMax = 3.f;

/// A rectangle of pixels, top-down, as darktable's dt_iop_roi_t.
struct Region {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct ShapeSettings {
    AlgorithmEnum algorithm = eAlgorithmHeal;
    /// 0 for the image, 1 to numScales for a detail scale, numScales + 1 (or more) for the residual.
    int scale = 0;
    BlurTypeEnum blurType = eBlurTypeGaussian;
    /// In canonical pixels.
    float blurRadius = 10.f;
    FillModeEnum fillMode = eFillModeErase;
    float fillColor[3] = { 0.f, 0.f, 0.f };
    float fillBrightness = 0.f;
    float opacity = 1.f;
};

/**
 * @brief One shape for one render. mask covers maskRoi (row 0 its top row) and is 0 outside the
 * shape and its feather; maskRoi is the shape's bounding box with feather, darktable's form mask
 * area, and may reach outside the image. dx, dy go from the source to the shape, in render-scale
 * pixels, x right and y down.
 **/
struct Shape {
    ShapeSettings settings;
    Region maskRoi;
    std::vector<float> mask;
    float dx = 0.f;
    float dy = 0.f;
};

struct Params {
    int numScales = 0;
    int mergeFromScale = 0;
    /// 0 for the result, 1 to numScales for a detail scale, numScales + 1 for the residual.
    int returnLayer = 0;
    float previewLevels[3] = { kPreviewLevelMin, 0.f, kPreviewLevelMax };
    int maxHealIterations = 2000;
    /// The render scale: the wavelet preview scale and the blur radius factor.
    float renderScale = 1.f;
};

/// The returnLayer of an output mode.
int returnLayerForOutput(OutputEnum output, int numScales, int currentScale);

/**
 * @brief Whether a shape on shapeScale is applied with numScales scales: a scale up to
 * numScales + 1, the residual. As in darktable, a shape on a higher scale is skipped.
 **/
bool shapeScaleProcessed(int shapeScale, int numScales);

/**
 * @brief Runs the retouch on image (width x height x 4) in place: darktable's process() and
 * rt_process_forms(), without the darkroom's mask display. The output is the recomposed image
 * or the layer returnLayer asks for, a detail scale shown through the preview levels.
 **/
void process(float* image,
             int width,
             int height,
             const Params& params,
             const std::vector<Shape>& shapes,
             const EdgeAware::ParallelFor& parallel);

/**
 * @brief Applies the shapes on scale to layer, one wavelet layer of an image (width x height x 4),
 * in order: rt_process_forms() for one layer. scales is the number of scales the decomposition
 * has after capping.
 **/
void processLayer(float* layer,
                  int width,
                  int height,
                  int scale,
                  int scales,
                  int returnLayer,
                  int mergeFromScale,
                  const Params& params,
                  const std::vector<Shape>& shapes,
                  const EdgeAware::ParallelFor& parallel);
} // namespace RetouchCore

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_RetouchRender_h
