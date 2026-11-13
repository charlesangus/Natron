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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "TextureProjection.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "Engine/AppManager.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace TextureProjection {
namespace {
    int
    maxThreads()
    {
        return appPTR ? appPTR->getNCPUsAvailableForEffect() : 1;
    }

    // Grows rgba by one texel per pass into the texels filled does not mark, each taking the mean of
    // its filled neighbours; a texel with none stays as it is.
    void
    dilate(int width,
           int height,
           int passes,
           std::vector<std::uint8_t>* filled,
           std::vector<float>* rgba)
    {
        std::vector<std::uint8_t> nextFilled;
        std::vector<float> next;

        for (int pass = 0; pass < passes; ++pass) {
            nextFilled = *filled;
            next = *rgba;
            bool grew = false;
            std::vector<std::uint8_t> rowGrew((std::size_t)height, 0);
            parallelForOnGlobalPool(height, maxThreads(), [&](int y) {
                for (int x = 0; x < width; ++x) {
                    const std::size_t i = (std::size_t)x + (std::size_t)y * (std::size_t)width;
                    if ((*filled)[i]) {
                        continue;
                    }
                    float sum[4] = { 0.f, 0.f, 0.f, 0.f };
                    int count = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx;
                            const int ny = y + dy;
                            if (((dx == 0) && (dy == 0)) || (nx < 0) || (ny < 0) || (nx >= width) || (ny >= height)) {
                                continue;
                            }
                            const std::size_t n = (std::size_t)nx + (std::size_t)ny * (std::size_t)width;
                            if (!(*filled)[n]) {
                                continue;
                            }
                            for (int c = 0; c < 4; ++c) {
                                sum[c] += (*rgba)[n * 4 + (std::size_t)c];
                            }
                            ++count;
                        }
                    }
                    if (count > 0) {
                        for (int c = 0; c < 4; ++c) {
                            next[i * 4 + (std::size_t)c] = sum[c] / (float)count;
                        }
                        nextFilled[i] = 1;
                        rowGrew[(std::size_t)y] = 1;
                    }
                }
            });
            for (std::uint8_t g : rowGrew) {
                grew = grew || g;
            }
            filled->swap(nextFilled);
            rgba->swap(next);
            if (!grew) {
                break;
            }
        }
    }
} // anonymous namespace

bool
project(const PXR_NS::UsdStageRefPtr& stage,
        const SourceImage& image,
        const Args& args,
        Result* result,
        std::string* errorMessage)
{
    if (!image.rgba || (image.width <= 0) || (image.height <= 0) || !(image.pixelAspect > 0.)) {
        *errorMessage = "The image to project is empty.";

        return false;
    }
    if (!stage) {
        *errorMessage = "There is no scene to project onto.";

        return false;
    }

    SceneRaster::CameraArgs cameraArgs;
    cameraArgs.cameraPath = args.cameraPath;
    cameraArgs.explicitCamera = args.explicitCamera;
    cameraArgs.time = args.time;
    cameraArgs.width = image.width;
    cameraArgs.height = image.height;
    cameraArgs.pixelAspect = image.pixelAspect;
    cameraArgs.fit = args.fit;
    cameraArgs.cancelled = args.cancelled;

    SceneRaster::CameraBuffers zbuffer;
    PXR_NS::GfFrustum frustum;
    if (args.occlusion) {
        if (!SceneRaster::renderCamera(stage, cameraArgs, &zbuffer, errorMessage)) {
            return false;
        }
        frustum = zbuffer.frustum;
    } else if (!SceneRaster::cameraFrustum(stage, cameraArgs, &frustum, errorMessage)) {
        return false;
    }
    const PXR_NS::GfMatrix4d viewMatrix = frustum.ComputeViewMatrix();
    const PXR_NS::GfMatrix4d viewProjection = viewMatrix * frustum.ComputeProjectionMatrix();
    const bool perspective = frustum.GetProjectionType() == PXR_NS::GfFrustum::Perspective;
    const PXR_NS::GfVec3d eye = frustum.GetPosition();
    const PXR_NS::GfVec3d towardCamera = -frustum.ComputeViewDirection();

    SceneRaster::BakeArgs bakeArgs;
    bakeArgs.primPath = args.meshPath;
    bakeArgs.stName = args.stName;
    bakeArgs.time = args.time;
    bakeArgs.width = args.width;
    bakeArgs.height = args.height;
    bakeArgs.udimTile = args.udimTile;
    bakeArgs.cancelled = args.cancelled;
    SceneRaster::BakeBuffers bake;
    if (!SceneRaster::bakeUV(stage, bakeArgs, &bake, errorMessage)) {
        return false;
    }

    const Resampler::SourceImage source(image.rgba, RectI(0, 0, image.width, image.height), 4);
    const int width = args.width;
    const int height = args.height;
    const double bias = args.depthBias;
    const bool occlusion = args.occlusion;
    const bool cullBackFaces = !args.projectBackFaces;
    result->width = width;
    result->height = height;
    result->rgba.assign((std::size_t)width * (std::size_t)height * 4, 0.f);
    result->coverage.assign((std::size_t)width * (std::size_t)height, 0);
    std::atomic<bool> stop(false);
    parallelForOnGlobalPool(height, maxThreads(), [&](int y) {
        if (stop.load(std::memory_order_relaxed)) {
            return;
        }
        if (args.cancelled && args.cancelled()) {
            stop = true;

            return;
        }
        for (int x = 0; x < width; ++x) {
            const std::size_t i = (std::size_t)x + (std::size_t)y * (std::size_t)width;
            if (!bake.coverage[i]) {
                continue;
            }
            const PXR_NS::GfVec3d world(bake.worldPosition[i]);
            const PXR_NS::GfVec4d p(world[0], world[1], world[2], 1.);
            const double depth = -(p * viewMatrix)[2];
            if (!(depth > 0.)) {
                continue;
            }
            const PXR_NS::GfVec3d normal(bake.worldNormal[i]);
            if (cullBackFaces && (PXR_NS::GfDot(normal, perspective ? (eye - world) : towardCamera) <= 0.)) {
                continue;
            }
            const PXR_NS::GfVec4d clip = p * viewProjection;
            if (clip[3] <= 0.) {
                continue;
            }
            const double ndcX = clip[0] / clip[3];
            const double ndcY = clip[1] / clip[3];
            if ((ndcX < -1.) || (ndcX > 1.) || (ndcY < -1.) || (ndcY > 1.)) {
                continue;
            }
            if (occlusion) {
                const int zx = std::clamp((int)std::floor((ndcX * 0.5 + 0.5) * zbuffer.width), 0, zbuffer.width - 1);
                const int zy = std::clamp((int)std::floor((ndcY * 0.5 + 0.5) * zbuffer.height), 0, zbuffer.height - 1);
                const float nearest = zbuffer.depth[(std::size_t)zx + (std::size_t)zy * (std::size_t)zbuffer.width];
                if ((double)nearest < depth * (1. - bias)) {
                    continue;
                }
            }
            const double fx = (ndcX * 0.5 + 0.5) * image.width;
            const double fy = (ndcY * 0.5 + 0.5) * image.height;
            Resampler::interpolate(Resampler::eFilterBilinear, false, fx, fy, source, false, &result->rgba[i * 4]);
            result->coverage[i] = 1;
        }
    });
    if (stop.load()) {
        errorMessage->clear();

        return false;
    }

    if (args.edgeDilation > 0) {
        dilate(width, height, args.edgeDilation, &bake.coverage, &result->rgba);
    }

    return true;
} // project
} // namespace TextureProjection

NATRON_NAMESPACE_EXIT
