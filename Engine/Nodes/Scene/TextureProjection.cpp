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

    // The camera an image was painted through, and the depth it saw when occlusion is wanted.
    struct Viewer {
        SceneRaster::CameraBuffers zbuffer;
        PXR_NS::GfMatrix4d viewMatrix;
        PXR_NS::GfMatrix4d viewProjection;
        PXR_NS::GfVec3d eye;
        PXR_NS::GfVec3d towardCamera;
        bool perspective = true;
        bool occlusion = true;
        bool cullBackFaces = true;
        double bias = 0.;
        int imageWidth = 0;
        int imageHeight = 0;

        bool
        prepare(const PXR_NS::UsdStageRefPtr& stage,
                const SourceImage& image,
                const Args& args,
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

            PXR_NS::GfFrustum frustum;
            if (args.occlusion) {
                if (!SceneRaster::renderCamera(stage, cameraArgs, &zbuffer, errorMessage)) {
                    return false;
                }
                frustum = zbuffer.frustum;
                // Only the depth is read.
                std::vector<std::int32_t>().swap(zbuffer.primIndex);
                std::vector<PXR_NS::GfVec3f>().swap(zbuffer.worldPosition);
                std::vector<PXR_NS::GfVec2f>().swap(zbuffer.st);
                std::vector<std::int32_t>().swap(zbuffer.udimTile);
            } else if (!SceneRaster::cameraFrustum(stage, cameraArgs, &frustum, errorMessage)) {
                return false;
            }
            viewMatrix = frustum.ComputeViewMatrix();
            viewProjection = viewMatrix * frustum.ComputeProjectionMatrix();
            perspective = frustum.GetProjectionType() == PXR_NS::GfFrustum::Perspective;
            eye = frustum.GetPosition();
            towardCamera = -frustum.ComputeViewDirection();
            occlusion = args.occlusion;
            cullBackFaces = !args.projectBackFaces;
            bias = args.depthBias;
            imageWidth = image.width;
            imageHeight = image.height;

            return true;
        }

        // Where in the image the point lies, when the camera sees it.
        bool
        sees(const PXR_NS::GfVec3f& position,
             const PXR_NS::GfVec3f& faceNormal,
             double* fx,
             double* fy) const
        {
            const PXR_NS::GfVec3d world(position);
            const PXR_NS::GfVec4d p(world[0], world[1], world[2], 1.);
            const double depth = -(p * viewMatrix)[2];
            if (!(depth > 0.)) {
                return false;
            }
            const PXR_NS::GfVec3d normal(faceNormal);
            if (cullBackFaces && (PXR_NS::GfDot(normal, perspective ? (eye - world) : towardCamera) <= 0.)) {
                return false;
            }
            const PXR_NS::GfVec4d clip = p * viewProjection;
            if (clip[3] <= 0.) {
                return false;
            }
            const double ndcX = clip[0] / clip[3];
            const double ndcY = clip[1] / clip[3];
            if ((ndcX < -1.) || (ndcX > 1.) || (ndcY < -1.) || (ndcY > 1.)) {
                return false;
            }
            if (occlusion) {
                const int zx = std::clamp((int)std::floor((ndcX * 0.5 + 0.5) * zbuffer.width), 0, zbuffer.width - 1);
                const int zy = std::clamp((int)std::floor((ndcY * 0.5 + 0.5) * zbuffer.height), 0, zbuffer.height - 1);
                const float nearest = zbuffer.depth[(std::size_t)zx + (std::size_t)zy * (std::size_t)zbuffer.width];
                if ((double)nearest < depth * (1. - bias)) {
                    return false;
                }
            }
            *fx = (ndcX * 0.5 + 0.5) * imageWidth;
            *fy = (ndcY * 0.5 + 0.5) * imageHeight;

            return true;
        }
    };

    // Rows per band of a mesh's layout: a band's bake buffers stay a few tens of megabytes at 8K.
    const int kBandRows = 256;
} // anonymous namespace

bool
project(const PXR_NS::UsdStageRefPtr& stage,
        const SourceImage& image,
        const Args& args,
        Result* result,
        std::string* errorMessage)
{
    Viewer viewer;

    if (!viewer.prepare(stage, image, args, errorMessage)) {
        return false;
    }

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
            double fx, fy;
            if (!bake.coverage[i] || !viewer.sees(bake.worldPosition[i], bake.worldNormal[i], &fx, &fy)) {
                continue;
            }
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

bool
projectOver(const PXR_NS::UsdStageRefPtr& stage,
            const SourceImage& paint,
            const float* mask,
            const Args& args,
            const std::vector<PXR_NS::SdfPath>& meshes,
            float* rgba,
            std::string* errorMessage)
{
    if (!rgba || !mask) {
        *errorMessage = "There is no texture to project onto.";

        return false;
    }
    if ((args.width <= 0) || (args.height <= 0)) {
        *errorMessage = "The texture size is empty.";

        return false;
    }
    Viewer viewer;
    if (!viewer.prepare(stage, paint, args, errorMessage)) {
        return false;
    }

    const RectI bounds(0, 0, paint.width, paint.height);
    const Resampler::SourceImage paintSource(paint.rgba, bounds, 4);
    const Resampler::SourceImage maskSource(mask, bounds, 1);
    const int width = args.width;
    for (const PXR_NS::SdfPath& mesh : meshes) {
        SceneRaster::BakeArgs bakeArgs;
        bakeArgs.primPath = mesh;
        bakeArgs.stName = args.stName;
        bakeArgs.time = args.time;
        bakeArgs.width = args.width;
        bakeArgs.height = args.height;
        bakeArgs.udimTile = args.udimTile;
        bakeArgs.cancelled = args.cancelled;
        SceneRaster::UVLayout layout;
        if (!layout.prepare(stage, bakeArgs, errorMessage)) {
            return false;
        }
        SceneRaster::BakeBuffers bake;
        for (int firstRow = 0; firstRow < args.height; firstRow += kBandRows) {
            const int rows = std::min(kBandRows, args.height - firstRow);
            if (!layout.bakeRows(firstRow, rows, &bake, errorMessage)) {
                return false;
            }
            std::atomic<bool> stop(false);
            parallelForOnGlobalPool(rows, maxThreads(), [&](int y) {
                if (stop.load(std::memory_order_relaxed)) {
                    return;
                }
                if (args.cancelled && args.cancelled()) {
                    stop = true;

                    return;
                }
                float* row = rgba + (std::size_t)(firstRow + y) * (std::size_t)width * 4;
                for (int x = 0; x < width; ++x) {
                    const std::size_t i = (std::size_t)x + (std::size_t)y * (std::size_t)width;
                    double fx, fy;
                    if (!bake.coverage[i] || !viewer.sees(bake.worldPosition[i], bake.worldNormal[i], &fx, &fy)) {
                        continue;
                    }
                    float m = 0.f;
                    float p[4] = { 0.f, 0.f, 0.f, 0.f };
                    Resampler::interpolate(Resampler::eFilterBilinear, false, fx, fy, maskSource, false, &m);
                    if (m <= 0.f) {
                        continue;
                    }
                    Resampler::interpolate(Resampler::eFilterBilinear, false, fx, fy, paintSource, false, p);
                    const float keep = std::max(0.f, 1.f - m);
                    float* out = row + (std::size_t)x * 4;
                    for (int c = 0; c < 4; ++c) {
                        out[c] = out[c] * keep + p[c];
                    }
                }
            });
            if (stop.load()) {
                errorMessage->clear();

                return false;
            }
        }
    }

    return true;
} // projectOver
} // namespace TextureProjection

NATRON_NAMESPACE_EXIT
