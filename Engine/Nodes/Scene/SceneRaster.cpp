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
#include <atomic>
#include <cmath>
#include <functional>
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
        GfVec2f st[3];
        bool hasSt = false;
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
     * are shaded in order; rows run in parallel, a chunk of them per task. False when cancelled
     * returned true, polled per triangle, which leaves the buffers partly shaded.
     **/
    template <typename Shade>
    bool
    rasterize(int width,
              int height,
              const std::vector<RasterTriangle>& tris,
              const std::function<bool()>& cancelled,
              const Shade& shade)
    {
        if ((width <= 0) || (height <= 0) || tris.empty()) {
            return !(cancelled && cancelled());
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

        std::atomic<bool> stop(false);
        parallelForOnGlobalPool((int)chunks.size(), maxRasterThreads(), [&](int c) {
            const RectI& chunk = chunks[c];

            for (std::size_t t : bins[c]) {
                if (stop.load(std::memory_order_relaxed)) {
                    return;
                }
                if (cancelled && cancelled()) {
                    stop = true;

                    return;
                }
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

        return !stop.load() && !(cancelled && cancelled());
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
        GfVec2d st;
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
        v.st = a.st + (b.st - a.st) * t;

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
                      const GfVec2f* st,
                      std::vector<RasterTriangle>* tris)
    {
        std::vector<ClipVertex> polygon(3);

        for (int i = 0; i < 3; ++i) {
            const GfVec4d p(world[i][0], world[i][1], world[i][2], 1.);
            polygon[i].clip = p * viewProjection;
            polygon[i].world = GfVec3d(world[i]);
            polygon[i].depth = -(p * view)[2];
            polygon[i].st = st ? GfVec2d(st[i]) : GfVec2d(0.);
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
                tri.st[k] = GfVec2f(corners[k]->st);
            }
            if (!valid || !setRows(&tri, height)) {
                continue;
            }
            if ((std::max({ tri.x[0], tri.x[1], tri.x[2] }) < 0.5) || (std::min({ tri.x[0], tri.x[1], tri.x[2] }) > width - 0.5)) {
                continue;
            }
            tri.hasSt = (st != nullptr);
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
    // The mesh's texture coordinates at the three corners of each of its triangles, flattened
    // through the primvar's indices and interpolation (vertex, varying, faceVarying or uniform).
    // (*valid)[t] is 0 for a triangle whose corners the primvar does not cover. False, with *error
    // naming the problem, when the primvar is missing, not 2D, or has one value for the whole mesh.
    bool
    readTriangleTexCoords(const UsdGeomMesh& mesh,
                          const TfToken& stName,
                          UsdTimeCode time,
                          MeshData* data,
                          std::vector<GfVec2f>* corners,
                          std::vector<std::uint8_t>* valid,
                          std::string* error)
    {
        const std::string primName = mesh.GetPath().GetString();
        const UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(stName);
        const std::string primvarName = stName.GetString();
        VtVec2fArray st;

        if (!primvar || !primvar.HasValue()) {
            *error = primName + " has no " + primvarName + " primvar.";

            return false;
        }
        if (!readTexCoords(primvar, time, &st)) {
            *error = "The " + primvarName + " primvar of " + primName + " is not a 2D texture coordinate.";

            return false;
        }
        const TfToken interpolation = primvar.GetInterpolation();
        if ((interpolation == UsdGeomTokens->constant) || (primvar.GetElementSize() != 1)) {
            *error = "The " + primvarName + " primvar of " + primName + " has one value for the whole mesh, which maps no area.";

            return false;
        }

        VtVec2fArray faceVaryingSt;
        if (interpolation == UsdGeomTokens->faceVarying) {
            HdMeshUtil util(&data->topology, mesh.GetPath());
            VtValue triangulated;
            const HdMeshComputationResult result = util.ComputeTriangulatedFaceVaryingPrimvar(st.cdata(), (int)st.size(), HdTypeFloatVec2, &triangulated);
            if (result == HdMeshComputationResult::Success) {
                faceVaryingSt = triangulated.Get<VtVec2fArray>();
            } else if (result == HdMeshComputationResult::Unchanged) {
                faceVaryingSt = st;
            }
            if (faceVaryingSt.size() != data->triangles.size() * 3) {
                *error = "The faceVarying " + primvarName + " primvar of " + primName + " does not match its faces.";

                return false;
            }
        }

        corners->assign(data->triangles.size() * 3, GfVec2f(0.f));
        valid->assign(data->triangles.size(), 0);
        for (std::size_t t = 0; t < data->triangles.size(); ++t) {
            const GfVec3i& tri = data->triangles[t];
            bool ok = validTriangle(tri, data->worldPoints.size());
            for (int k = 0; (k < 3) && ok; ++k) {
                if (interpolation == UsdGeomTokens->faceVarying) {
                    (*corners)[t * 3 + k] = faceVaryingSt[t * 3 + k];
                    continue;
                }
                const std::size_t index = (interpolation == UsdGeomTokens->uniform)
                    ? (std::size_t)HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(data->primitiveParams[t])
                    : (std::size_t)tri[k];
                ok = index < st.size();
                if (ok) {
                    (*corners)[t * 3 + k] = st[index];
                }
            }
            (*valid)[t] = ok ? 1 : 0;
        }

        return true;
    }

    // The UDIM tile holding (u, v), 0 outside the 10-wide tile grid.
    std::int32_t
    udimTileOf(float u,
               float v)
    {
        const float tu = std::floor(u);
        const float tv = std::floor(v);

        if (!(tu >= 0.f) || !(tu < 10.f) || !(tv >= 0.f) || !(tv < 1000.f)) {
            return 0;
        }

        return 1001 + (std::int32_t)tu + 10 * (std::int32_t)tv;
    }
} // anonymous namespace

bool
cameraFrustum(const UsdStageRefPtr& stage,
              const CameraArgs& args,
              GfFrustum* frustum,
              std::string* errorMessage)
{
    if ((args.width <= 0) || (args.height <= 0)) {
        *errorMessage = "The camera raster size is empty.";

        return false;
    }
    if (args.explicitCamera) {
        const ExplicitCamera& cam = *args.explicitCamera;
        if (!(cam.viewHeight > 0.) || !(cam.aspect > 0.) || !(cam.nearClip > 0.) || !(cam.farClip > cam.nearClip)) {
            *errorMessage = "The explicit camera has an empty view or an invalid clip range.";

            return false;
        }
        const double halfHeight = cam.viewHeight * 0.5;
        GfMatrix4d cameraToWorld;
        for (int r = 0; r < 4; ++r) {
            cameraToWorld.SetRow(r, GfVec4d(cam.cameraToWorld[r * 4], cam.cameraToWorld[r * 4 + 1], cam.cameraToWorld[r * 4 + 2], cam.cameraToWorld[r * 4 + 3]));
        }
        *frustum = GfFrustum();
        frustum->SetPositionAndRotationFromMatrix(cameraToWorld);
        frustum->SetProjectionType(cam.orthographic ? GfFrustum::Orthographic : GfFrustum::Perspective);
        frustum->SetWindow(GfRange2d(GfVec2d(-halfHeight * cam.aspect, -halfHeight), GfVec2d(halfHeight * cam.aspect, halfHeight)));
        frustum->SetNearFar(GfRange1d(cam.nearClip, cam.farClip));

        return true;
    }
    const double aspect = args.width * args.pixelAspect / args.height;

    return HydraRender::cameraFrustum(stage, args.cameraPath, args.time, aspect, args.fit, frustum, errorMessage);
} // cameraFrustum

bool
renderCamera(const UsdStageRefPtr& stage,
             const CameraArgs& args,
             CameraBuffers* buffers,
             std::string* errorMessage)
{
    if (!cameraFrustum(stage, args, &buffers->frustum, errorMessage)) {
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
    const bool wantSt = !args.stName.IsEmpty();
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
            std::vector<GfVec2f> corners;
            std::vector<std::uint8_t> covered;
            std::string ignored;
            const bool hasSt = wantSt && readTriangleTexCoords(mesh, args.stName, args.time, &data, &corners, &covered, &ignored);
            for (std::size_t ti = 0; ti < data.triangles.size(); ++ti) {
                const GfVec3i& t = data.triangles[ti];
                if (!validTriangle(t, data.worldPoints.size())) {
                    continue;
                }
                const GfVec3f world[3] = { data.worldPoints[t[0]], data.worldPoints[t[1]], data.worldPoints[t[2]] };
                const bool triangleHasSt = hasSt && covered[ti];
                addCameraTriangle(world, view, viewProjection, args.width, args.height, index, triangleHasSt ? &corners[ti * 3] : nullptr, &tris);
            }
        }
    }

    const std::size_t numPixels = (std::size_t)args.width * (std::size_t)args.height;
    buffers->width = args.width;
    buffers->height = args.height;
    buffers->depth.assign(numPixels, std::numeric_limits<float>::infinity());
    buffers->primIndex.assign(numPixels, -1);
    buffers->worldPosition.assign(numPixels, GfVec3f(0.f));
    buffers->st.clear();
    buffers->udimTile.clear();
    if (wantSt) {
        buffers->st.assign(numPixels, GfVec2f(0.f));
        buffers->udimTile.assign(numPixels, 0);
    }

    const bool drawn = rasterize(args.width, args.height, tris, args.cancelled, [buffers, wantSt](const RasterTriangle& tri, std::size_t i, const float w[3]) {
        const float depth = w[0] * tri.depth[0] + w[1] * tri.depth[1] + w[2] * tri.depth[2];
        if (depth < buffers->depth[i]) {
            buffers->depth[i] = depth;
            buffers->primIndex[i] = tri.prim;
            buffers->worldPosition[i] = tri.world[0] * w[0] + tri.world[1] * w[1] + tri.world[2] * w[2];
            if (wantSt) {
                if (tri.hasSt) {
                    const GfVec2f st = tri.st[0] * w[0] + tri.st[1] * w[1] + tri.st[2] * w[2];
                    buffers->st[i] = st;
                    buffers->udimTile[i] = udimTileOf(st[0], st[1]);
                } else {
                    buffers->st[i] = GfVec2f(0.f);
                    buffers->udimTile[i] = 0;
                }
            }
        }
    });
    if (!drawn) {
        errorMessage->clear();

        return false;
    }

    return true;
} // renderCamera

struct UVLayout::Data {
    BakeArgs args;
    bool wantReader = false;
    // In tile pixels, rows over the whole tile.
    std::vector<RasterTriangle> tris;
};

UVLayout::UVLayout()
    : _data(new Data)
{
}

UVLayout::~UVLayout()
{
}

bool
UVLayout::prepare(const UsdStageRefPtr& stage,
                  const BakeArgs& args,
                  std::string* errorMessage)
{
    const std::string primName = args.primPath.GetString();

    _data->tris.clear();
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
    UsdGeomXformCache xformCache(args.time);
    MeshData data;
    readMesh(mesh, args.time, &xformCache, &data);

    std::vector<GfVec2f> stCorners;
    std::vector<std::uint8_t> covered;
    if (!readTriangleTexCoords(mesh, args.stName, args.time, &data, &stCorners, &covered, errorMessage)) {
        return false;
    }
    const bool wantReader = !args.readerStName.IsEmpty();
    std::vector<GfVec2f> readerCorners;
    std::vector<std::uint8_t> readerCovered;
    if (wantReader && !readTriangleTexCoords(mesh, args.readerStName, args.time, &data, &readerCorners, &readerCovered, errorMessage)) {
        return false;
    }

    const float tileU = (float)((args.udimTile - 1001) % 10);
    const float tileV = (float)((args.udimTile - 1001) / 10);
    std::vector<RasterTriangle>& tris = _data->tris;
    tris.reserve(data.triangles.size());
    for (std::size_t t = 0; t < data.triangles.size(); ++t) {
        const GfVec3i& corners = data.triangles[t];
        if (!covered[t]) {
            continue;
        }
        const GfVec2f* uv = &stCorners[t * 3];
        RasterTriangle tri;
        for (int k = 0; k < 3; ++k) {
            tri.x[k] = (uv[k][0] - tileU) * (double)args.width;
            tri.y[k] = (uv[k][1] - tileV) * (double)args.height;
            tri.invW[k] = 1.f;
            tri.world[k] = data.worldPoints[corners[k]];
            tri.depth[k] = 0.f;
            tri.st[k] = GfVec2f(0.f);
        }
        if (wantReader && readerCovered[t]) {
            tri.hasSt = true;
            for (int k = 0; k < 3; ++k) {
                tri.st[k] = readerCorners[t * 3 + k];
            }
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
    _data->args = args;
    _data->wantReader = wantReader;

    return true;
} // UVLayout::prepare

bool
UVLayout::bakeRows(int firstRow,
                   int rowCount,
                   BakeBuffers* buffers,
                   std::string* errorMessage) const
{
    const BakeArgs& args = _data->args;
    const bool wantReader = _data->wantReader;

    firstRow = std::max(0, firstRow);
    rowCount = std::max(0, std::min(rowCount, args.height - firstRow));

    // Shifting by a whole number of rows moves every centre test by exactly that much, so a band
    // draws the texels the whole tile would.
    std::vector<RasterTriangle> band;
    for (const RasterTriangle& source : _data->tris) {
        if ((source.lastRow < firstRow) || (source.firstRow >= firstRow + rowCount)) {
            continue;
        }
        RasterTriangle tri = source;
        for (int k = 0; k < 3; ++k) {
            tri.y[k] -= (double)firstRow;
        }
        if (setRows(&tri, rowCount)) {
            band.push_back(tri);
        }
    }

    const std::size_t numTexels = (std::size_t)args.width * (std::size_t)rowCount;
    buffers->width = args.width;
    buffers->height = rowCount;
    buffers->coverage.assign(numTexels, 0);
    buffers->worldPosition.assign(numTexels, GfVec3f(0.f));
    buffers->worldNormal.assign(numTexels, GfVec3f(0.f));
    buffers->readerSt.clear();
    if (wantReader) {
        buffers->readerSt.assign(numTexels, GfVec2f(0.f));
    }

    const bool drawn = rasterize(args.width, rowCount, band, args.cancelled, [buffers, wantReader](const RasterTriangle& tri, std::size_t i, const float w[3]) {
        buffers->coverage[i] = 1;
        buffers->worldPosition[i] = tri.world[0] * w[0] + tri.world[1] * w[1] + tri.world[2] * w[2];
        buffers->worldNormal[i] = tri.normal;
        if (wantReader) {
            buffers->readerSt[i] = tri.hasSt ? (tri.st[0] * w[0] + tri.st[1] * w[1] + tri.st[2] * w[2]) : GfVec2f(0.f);
        }
    });
    if (!drawn) {
        errorMessage->clear();

        return false;
    }

    return true;
} // UVLayout::bakeRows

bool
bakeUV(const UsdStageRefPtr& stage,
       const BakeArgs& args,
       BakeBuffers* buffers,
       std::string* errorMessage)
{
    UVLayout layout;

    return layout.prepare(stage, args, errorMessage) && layout.bakeRows(0, args.height, buffers, errorMessage);
}

bool
udimTiles(const UsdStageRefPtr& stage,
          const SdfPath& primPath,
          const TfToken& stName,
          UsdTimeCode time,
          std::set<int>* tiles,
          std::string* errorMessage)
{
    const UsdGeomMesh mesh(stage ? stage->GetPrimAtPath(primPath) : UsdPrim());

    if (!mesh) {
        *errorMessage = primPath.GetString() + " is not a mesh on the input scene.";

        return false;
    }
    UsdGeomXformCache xformCache(time);
    MeshData data;
    readMesh(mesh, time, &xformCache, &data);
    std::vector<GfVec2f> corners;
    std::vector<std::uint8_t> covered;
    if (!readTriangleTexCoords(mesh, stName, time, &data, &corners, &covered, errorMessage)) {
        return false;
    }

    // A face whose edge lies on a tile boundary covers no area of the tile beyond it.
    const double edge = 1e-5;
    for (std::size_t t = 0; t < data.triangles.size(); ++t) {
        if (!covered[t]) {
            continue;
        }
        const GfVec2f* uv = &corners[t * 3];
        const double minU = std::min({ uv[0][0], uv[1][0], uv[2][0] });
        const double maxU = std::max({ uv[0][0], uv[1][0], uv[2][0] });
        const double minV = std::min({ uv[0][1], uv[1][1], uv[2][1] });
        const double maxV = std::max({ uv[0][1], uv[1][1], uv[2][1] });
        if (!std::isfinite(minU) || !std::isfinite(maxU) || !std::isfinite(minV) || !std::isfinite(maxV)) {
            continue;
        }
        const int firstU = std::max(0, (int)std::floor(minU + edge));
        const int lastU = std::min(9, (int)std::floor(maxU - edge));
        const int firstV = std::max(0, (int)std::floor(minV + edge));
        const int lastV = (int)std::floor(maxV - edge);
        for (int v = firstV; v <= lastV; ++v) {
            for (int u = firstU; u <= lastU; ++u) {
                tiles->insert(1001 + u + 10 * v);
            }
        }
    }

    return true;
} // udimTiles
} // namespace SceneRaster

NATRON_NAMESPACE_EXIT
