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

#include "SceneRaster.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include <QThreadPool>

#ifdef emit
#define NATRON_SCENERASTER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/types.h>
#ifdef NATRON_SCENERASTER_RESTORE_EMIT
#undef NATRON_SCENERASTER_RESTORE_EMIT
#define emit
#endif

#include "Engine/AppManager.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectI.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace SceneRaster {
namespace {
    // A triangle in raster space, x and y in pixels, with what its pixels interpolate.
    struct RasterTriangle {
        double x[3];
        double y[3];
        // 1 / clip w at each corner, for perspective-correct weights; 1 in texture space.
        float invW[3];
        GfVec3f world[3];
        float depth[3];
        GfVec3f normal;
        std::int32_t prim;
        int firstRow;
        int lastRow;
    };

    int
    maxRasterThreads()
    {
        return appPTR ? appPTR->getNCPUsAvailableForEffect() : QThreadPool::globalInstance()->maxThreadCount();
    }

    // The pixel rows whose centres the triangle's vertical extent spans; false when there are none.
    bool
    setRows(RasterTriangle* tri,
            int height)
    {
        const double minY = std::min({ tri->y[0], tri->y[1], tri->y[2] });
        const double maxY = std::max({ tri->y[0], tri->y[1], tri->y[2] });

        if (!std::isfinite(minY) || !std::isfinite(maxY)) {
            return false;
        }
        tri->firstRow = (int)std::max(0., std::ceil(minY - 0.5));
        tri->lastRow = (int)std::min((double)height - 1., std::floor(maxY - 0.5));

        return tri->firstRow <= tri->lastRow;
    }

    /**
     * Calls shade(tri, index, weights) for every pixel whose centre lies inside each triangle, with
     * the pixel's perspective-correct weights of the triangle's three corners. An edge two triangles
     * share is drawn once: of the two, only the triangle it runs upward (or leftward, when
     * horizontal) in, once wound counter-clockwise, owns the centres on it. Within a row triangles
     * are shaded in order; rows run in parallel, a chunk of them per task.
     **/
    template <typename Shade>
    void
    rasterize(int width,
              int height,
              const std::vector<RasterTriangle>& tris,
              const Shade& shade)
    {
        if ((width <= 0) || (height <= 0) || tris.empty()) {
            return;
        }
        // NativeEffectBase::makeDeepScanlineChunks()' partition: more chunks than threads, so that a
        // dense patch of the image does not leave most threads waiting on one chunk.
        const int maxChunks = std::max(1, QThreadPool::globalInstance()->maxThreadCount()) * 4;
        const int rowsPerChunk = (height + std::min(height, maxChunks) - 1) / std::min(height, maxChunks);
        std::vector<RectI> chunks;
        for (int y = 0; y < height; y += rowsPerChunk) {
            chunks.push_back(RectI(0, y, width, std::min(y + rowsPerChunk, height)));
        }
        std::vector<std::vector<std::size_t>> bins(chunks.size());
        for (std::size_t t = 0; t < tris.size(); ++t) {
            const int last = std::min((int)chunks.size() - 1, tris[t].lastRow / rowsPerChunk);
            for (int c = tris[t].firstRow / rowsPerChunk; c <= last; ++c) {
                bins[c].push_back(t);
            }
        }

        parallelForOnGlobalPool((int)chunks.size(), maxRasterThreads(), [&](int c) {
            const RectI& chunk = chunks[c];

            for (std::size_t t : bins[c]) {
                const RasterTriangle& tri = tris[t];
                const double area = (tri.x[1] - tri.x[0]) * (tri.y[2] - tri.y[0]) - (tri.y[1] - tri.y[0]) * (tri.x[2] - tri.x[0]);
                if ((area == 0.) || !std::isfinite(area)) {
                    continue;
                }
                const double sign = (area > 0.) ? 1. : -1.;
                // Edge i runs between the corners other than i, from corner i + 1 to corner i + 2.
                bool owns[3];
                for (int i = 0; i < 3; ++i) {
                    const int a = (i + 1) % 3;
                    const int b = (i + 2) % 3;
                    const double dx = sign * (tri.x[b] - tri.x[a]);
                    const double dy = sign * (tri.y[b] - tri.y[a]);
                    owns[i] = (dy > 0.) || ((dy == 0.) && (dx < 0.));
                }
                const double minX = std::min({ tri.x[0], tri.x[1], tri.x[2] });
                const double maxX = std::max({ tri.x[0], tri.x[1], tri.x[2] });
                const int firstX = (int)std::max(0., std::ceil(minX - 0.5));
                const int lastX = (int)std::min((double)width - 1., std::floor(maxX - 0.5));
                const int firstY = std::max(chunk.y1, tri.firstRow);
                const int lastY = std::min(chunk.y2 - 1, tri.lastRow);
                for (int y = firstY; y <= lastY; ++y) {
                    const double py = y + 0.5;
                    for (int x = firstX; x <= lastX; ++x) {
                        const double px = x + 0.5;
                        double e[3];
                        bool inside = true;
                        for (int i = 0; (i < 3) && inside; ++i) {
                            const int a = (i + 1) % 3;
                            const int b = (i + 2) % 3;
                            e[i] = sign * ((tri.x[b] - tri.x[a]) * (py - tri.y[a]) - (tri.y[b] - tri.y[a]) * (px - tri.x[a]));
                            inside = (e[i] > 0.) || ((e[i] == 0.) && owns[i]);
                        }
                        if (!inside) {
                            continue;
                        }
                        double w[3];
                        double sum = 0.;
                        for (int i = 0; i < 3; ++i) {
                            w[i] = e[i] * tri.invW[i];
                            sum += w[i];
                        }
                        if (sum <= 0.) {
                            continue;
                        }
                        const float weights[3] = { (float)(w[0] / sum), (float)(w[1] / sum), (float)(w[2] / sum) };
                        shade(tri, (std::size_t)x + (std::size_t)y * (std::size_t)width, weights);
                    }
                }
            }
        });
    }

    // A mesh's triangulation and world-space points at one time.
    struct MeshData {
        HdMeshTopology topology;
        VtVec3iArray triangles;
        VtIntArray primitiveParams;
        std::vector<GfVec3f> worldPoints;
        // Whether the world transform mirrors, which turns the winding's normal inward.
        bool mirrored = false;
    };

    void
    readMesh(const UsdGeomMesh& mesh,
             UsdTimeCode time,
             UsdGeomXformCache* xformCache,
             MeshData* data)
    {
        VtIntArray counts;
        VtIntArray indices;
        VtIntArray holes;
        TfToken orientation = UsdGeomTokens->rightHanded;
        TfToken scheme = UsdGeomTokens->catmullClark;
        VtVec3fArray points;

        mesh.GetFaceVertexCountsAttr().Get(&counts, time);
        mesh.GetFaceVertexIndicesAttr().Get(&indices, time);
        mesh.GetHoleIndicesAttr().Get(&holes, time);
        mesh.GetOrientationAttr().Get(&orientation, time);
        mesh.GetSubdivisionSchemeAttr().Get(&scheme, time);
        mesh.GetPointsAttr().Get(&points, time);

        data->topology = HdMeshTopology(scheme, orientation, counts, indices, holes);
        HdMeshUtil util(&data->topology, mesh.GetPath());
        util.ComputeTriangleIndices(&data->triangles, &data->primitiveParams);

        const GfMatrix4d toWorld = xformCache->GetLocalToWorldTransform(mesh.GetPrim());
        data->mirrored = toWorld.GetDeterminant3() < 0.;
        data->worldPoints.resize(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            data->worldPoints[i] = GfVec3f(toWorld.Transform(GfVec3d(points[i])));
        }
    }

    bool
    validTriangle(const GfVec3i& tri,
                  std::size_t numPoints)
    {
        for (int i = 0; i < 3; ++i) {
            if ((tri[i] < 0) || ((std::size_t)tri[i] >= numPoints)) {
                return false;
            }
        }

        return true;
    }

    // A corner of a triangle being clipped in clip space.
    struct ClipVertex {
        GfVec4d clip;
        GfVec3d world;
        double depth;
    };

    ClipVertex
    lerp(const ClipVertex& a,
         const ClipVertex& b,
         double t)
    {
        ClipVertex v;

        v.clip = a.clip + (b.clip - a.clip) * t;
        v.world = a.world + (b.world - a.world) * t;
        v.depth = a.depth + (b.depth - a.depth) * t;

        return v;
    }

    // Keeps the part of polygon where distance(v) >= 0 (Sutherland-Hodgman).
    template <typename Distance>
    void
    clipPolygon(std::vector<ClipVertex>* polygon,
                const Distance& distance)
    {
        std::vector<ClipVertex> out;
        const std::size_t n = polygon->size();

        for (std::size_t i = 0; i < n; ++i) {
            const ClipVertex& a = (*polygon)[i];
            const ClipVertex& b = (*polygon)[(i + 1) % n];
            const double da = distance(a.clip);
            const double db = distance(b.clip);
            if (da >= 0.) {
                out.push_back(a);
            }
            if ((da >= 0.) != (db >= 0.)) {
                out.push_back(lerp(a, b, da / (da - db)));
            }
        }
        polygon->swap(out);
    }

    // Appends the visible part of a world-space triangle, clipped to the near and far planes.
    void
    addCameraTriangle(const GfVec3f world[3],
                      const GfMatrix4d& view,
                      const GfMatrix4d& viewProjection,
                      int width,
                      int height,
                      std::int32_t prim,
                      std::vector<RasterTriangle>* tris)
    {
        std::vector<ClipVertex> polygon(3);

        for (int i = 0; i < 3; ++i) {
            const GfVec4d p(world[i][0], world[i][1], world[i][2], 1.);
            polygon[i].clip = p * viewProjection;
            polygon[i].world = GfVec3d(world[i]);
            polygon[i].depth = -(p * view)[2];
        }
        clipPolygon(&polygon, [](const GfVec4d& c) { return c[3] + c[2]; });
        clipPolygon(&polygon, [](const GfVec4d& c) { return c[3] - c[2]; });

        for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
            const ClipVertex* corners[3] = { &polygon[0], &polygon[i], &polygon[i + 1] };
            RasterTriangle tri;
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                const GfVec4d& c = corners[k]->clip;
                if (c[3] <= 0.) {
                    valid = false;
                    break;
                }
                tri.x[k] = (c[0] / c[3] * 0.5 + 0.5) * width;
                tri.y[k] = (c[1] / c[3] * 0.5 + 0.5) * height;
                tri.invW[k] = (float)(1. / c[3]);
                tri.world[k] = GfVec3f(corners[k]->world);
                tri.depth[k] = (float)corners[k]->depth;
            }
            if (!valid || !setRows(&tri, height)) {
                continue;
            }
            if ((std::max({ tri.x[0], tri.x[1], tri.x[2] }) < 0.5) || (std::min({ tri.x[0], tri.x[1], tri.x[2] }) > width - 0.5)) {
                continue;
            }
            tri.prim = prim;
            tris->push_back(tri);
        }
    }

    bool
    isDrawn(const UsdGeomImageable& imageable,
            UsdTimeCode time)
    {
        return (imageable.ComputeVisibility(time) != UsdGeomTokens->invisible) && (imageable.ComputePurpose() != UsdGeomTokens->guide);
    }

    // The primvar's values as 2D floats, flattened through its indices; false when it is not 2D.
    bool
    readTexCoords(const UsdGeomPrimvar& primvar,
                  UsdTimeCode time,
                  VtVec2fArray* values)
    {
        VtValue value;

        if (!primvar.ComputeFlattened(&value, time)) {
            return false;
        }
        if (value.IsHolding<VtVec2fArray>()) {
            *values = value.UncheckedGet<VtVec2fArray>();

            return true;
        }
        if (value.IsHolding<VtVec2dArray>()) {
            const VtVec2dArray& d = value.UncheckedGet<VtVec2dArray>();
            values->resize(d.size());
            for (std::size_t i = 0; i < d.size(); ++i) {
                (*values)[i] = GfVec2f(d[i]);
            }

            return true;
        }
        if (value.IsHolding<VtVec2hArray>()) {
            const VtVec2hArray& h = value.UncheckedGet<VtVec2hArray>();
            values->resize(h.size());
            for (std::size_t i = 0; i < h.size(); ++i) {
                (*values)[i] = GfVec2f(h[i]);
            }

            return true;
        }

        return false;
    }
} // anonymous namespace

bool
renderCamera(const UsdStageRefPtr& stage,
             const CameraArgs& args,
             CameraBuffers* buffers,
             std::string* errorMessage)
{
    if ((args.width <= 0) || (args.height <= 0)) {
        *errorMessage = "The camera raster size is empty.";

        return false;
    }
    const double aspect = args.width * args.pixelAspect / args.height;
    if (!HydraRender::cameraFrustum(stage, args.cameraPath, args.time, aspect, args.fit, &buffers->frustum, errorMessage)) {
        return false;
    }
    const GfMatrix4d view = buffers->frustum.ComputeViewMatrix();
    const GfMatrix4d viewProjection = view * buffers->frustum.ComputeProjectionMatrix();

    std::vector<SdfPath> roots = args.prims;
    if (roots.empty()) {
        roots.push_back(SdfPath::AbsoluteRootPath());
    }
    buffers->meshPaths.clear();
    std::set<SdfPath> seen;
    std::vector<RasterTriangle> tris;
    UsdGeomXformCache xformCache(args.time);
    for (const SdfPath& root : roots) {
        const UsdPrim start = stage->GetPrimAtPath(root);
        if (!start) {
            continue;
        }
        for (const UsdPrim& prim : UsdPrimRange(start, UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
            const UsdGeomMesh mesh(prim);
            if (!mesh || !seen.insert(prim.GetPath()).second || !isDrawn(mesh, args.time)) {
                continue;
            }
            MeshData data;
            readMesh(mesh, args.time, &xformCache, &data);
            const std::int32_t index = (std::int32_t)buffers->meshPaths.size();
            buffers->meshPaths.push_back(prim.GetPath());
            for (const GfVec3i& t : data.triangles) {
                if (!validTriangle(t, data.worldPoints.size())) {
                    continue;
                }
                const GfVec3f world[3] = { data.worldPoints[t[0]], data.worldPoints[t[1]], data.worldPoints[t[2]] };
                addCameraTriangle(world, view, viewProjection, args.width, args.height, index, &tris);
            }
        }
    }

    const std::size_t numPixels = (std::size_t)args.width * (std::size_t)args.height;
    buffers->width = args.width;
    buffers->height = args.height;
    buffers->depth.assign(numPixels, std::numeric_limits<float>::infinity());
    buffers->primIndex.assign(numPixels, -1);
    buffers->worldPosition.assign(numPixels, GfVec3f(0.f));

    rasterize(args.width, args.height, tris, [buffers](const RasterTriangle& tri, std::size_t i, const float w[3]) {
        const float depth = w[0] * tri.depth[0] + w[1] * tri.depth[1] + w[2] * tri.depth[2];
        if (depth < buffers->depth[i]) {
            buffers->depth[i] = depth;
            buffers->primIndex[i] = tri.prim;
            buffers->worldPosition[i] = tri.world[0] * w[0] + tri.world[1] * w[1] + tri.world[2] * w[2];
        }
    });

    return true;
} // renderCamera

bool
bakeUV(const UsdStageRefPtr& stage,
       const BakeArgs& args,
       BakeBuffers* buffers,
       std::string* errorMessage)
{
    const std::string primName = args.primPath.GetString();

    if ((args.width <= 0) || (args.height <= 0)) {
        *errorMessage = "The texture size is empty.";

        return false;
    }
    if (args.udimTile < 1001) {
        *errorMessage = "UDIM tile " + std::to_string(args.udimTile) + " is not a tile: tiles start at 1001.";

        return false;
    }
    const UsdGeomMesh mesh(stage ? stage->GetPrimAtPath(args.primPath) : UsdPrim());
    if (!mesh) {
        *errorMessage = primName + " is not a mesh on the input scene.";

        return false;
    }
    const UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(args.stName);
    const std::string primvarName = args.stName.GetString();
    VtVec2fArray st;
    if (!primvar || !primvar.HasValue()) {
        *errorMessage = primName + " has no " + primvarName + " primvar.";

        return false;
    }
    if (!readTexCoords(primvar, args.time, &st)) {
        *errorMessage = "The " + primvarName + " primvar of " + primName + " is not a 2D texture coordinate.";

        return false;
    }
    const TfToken interpolation = primvar.GetInterpolation();
    if ((interpolation == UsdGeomTokens->constant) || (primvar.GetElementSize() != 1)) {
        *errorMessage = "The " + primvarName + " primvar of " + primName + " has one value for the whole mesh, which maps no area.";

        return false;
    }

    UsdGeomXformCache xformCache(args.time);
    MeshData data;
    readMesh(mesh, args.time, &xformCache, &data);

    VtVec2fArray faceVaryingSt;
    if (interpolation == UsdGeomTokens->faceVarying) {
        HdMeshUtil util(&data.topology, args.primPath);
        VtValue triangulated;
        const HdMeshComputationResult result = util.ComputeTriangulatedFaceVaryingPrimvar(st.cdata(), (int)st.size(), HdTypeFloatVec2, &triangulated);
        if (result == HdMeshComputationResult::Success) {
            faceVaryingSt = triangulated.Get<VtVec2fArray>();
        } else if (result == HdMeshComputationResult::Unchanged) {
            faceVaryingSt = st;
        }
        if (faceVaryingSt.size() != data.triangles.size() * 3) {
            *errorMessage = "The faceVarying " + primvarName + " primvar of " + primName + " does not match its faces.";

            return false;
        }
    }

    const float tileU = (float)((args.udimTile - 1001) % 10);
    const float tileV = (float)((args.udimTile - 1001) / 10);
    std::vector<RasterTriangle> tris;
    tris.reserve(data.triangles.size());
    for (std::size_t t = 0; t < data.triangles.size(); ++t) {
        const GfVec3i& corners = data.triangles[t];
        if (!validTriangle(corners, data.worldPoints.size())) {
            continue;
        }
        GfVec2f uv[3];
        bool valid = true;
        for (int k = 0; (k < 3) && valid; ++k) {
            std::size_t index = 0;
            if (interpolation == UsdGeomTokens->faceVarying) {
                uv[k] = faceVaryingSt[t * 3 + k];
                continue;
            } else if (interpolation == UsdGeomTokens->uniform) {
                index = (std::size_t)HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(data.primitiveParams[t]);
            } else {
                index = (std::size_t)corners[k];
            }
            valid = index < st.size();
            if (valid) {
                uv[k] = st[index];
            }
        }
        if (!valid) {
            continue;
        }
        RasterTriangle tri;
        for (int k = 0; k < 3; ++k) {
            tri.x[k] = (uv[k][0] - tileU) * (double)args.width;
            tri.y[k] = (uv[k][1] - tileV) * (double)args.height;
            tri.invW[k] = 1.f;
            tri.world[k] = data.worldPoints[corners[k]];
            tri.depth[k] = 0.f;
        }
        if (!setRows(&tri, args.height)) {
            continue;
        }
        const GfVec3f normal = GfCross(tri.world[1] - tri.world[0], tri.world[2] - tri.world[0]);
        const float length = normal.GetLength();
        tri.normal = (length > 0.f) ? normal / (data.mirrored ? -length : length) : GfVec3f(0.f);
        tri.prim = 0;
        tris.push_back(tri);
    }

    const std::size_t numTexels = (std::size_t)args.width * (std::size_t)args.height;
    buffers->width = args.width;
    buffers->height = args.height;
    buffers->coverage.assign(numTexels, 0);
    buffers->worldPosition.assign(numTexels, GfVec3f(0.f));
    buffers->worldNormal.assign(numTexels, GfVec3f(0.f));

    rasterize(args.width, args.height, tris, [buffers](const RasterTriangle& tri, std::size_t i, const float w[3]) {
        buffers->coverage[i] = 1;
        buffers->worldPosition[i] = tri.world[0] * w[0] + tri.world[1] * w[1] + tri.world[2] * w[2];
        buffers->worldNormal[i] = tri.normal;
    });

    return true;
} // bakeUV
} // namespace SceneRaster

NATRON_NAMESPACE_EXIT
