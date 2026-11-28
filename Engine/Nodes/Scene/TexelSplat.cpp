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

#include "TexelSplat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "Engine/AppManager.h"
#include "Engine/Nodes/Merge/MergeOps.h"
#include "Engine/Nodes/Scene/USDTexturePaintView.h"
#include "Engine/PoolParallelFor.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace TexelSplat {
namespace {

    typedef TexelPages::PageKey PageKey;
    typedef TexelPages::PageData PageData;
    typedef RotoStrokeDabs::Dab Dab;
    typedef PaintMeshIndex::TriangleRef TriangleRef;

    constexpr int kPage = TexelPages::kPageSize;
    // The last row of the UDIM grid, tile 9991 to 10000.
    constexpr int kLastTileRow = 899;

    int
    threadCount(int threads)
    {
        if (threads > 0) {
            return threads;
        }

        return appPTR ? std::max(1, appPTR->getNCPUsAvailableForEffect()) : 1;
    }

    int
    localIndex(int x,
               int y)
    {
        return (x % kPage) + (y % kPage) * kPage;
    }

    struct UvBox {
        double u0 = 0.;
        double v0 = 0.;
        double u1 = 0.;
        double v1 = 0.;
        bool empty = true;

        void add(double u,
                 double v)
        {
            if (empty) {
                u0 = u1 = u;
                v0 = v1 = v;
                empty = false;

                return;
            }
            u0 = std::min(u0, u);
            u1 = std::max(u1, u);
            v0 = std::min(v0, v);
            v1 = std::max(v1, v);
        }

        UvBox intersect(const UvBox& other) const
        {
            UvBox box;
            if (empty || other.empty) {
                return box;
            }
            box.u0 = std::max(u0, other.u0);
            box.v0 = std::max(v0, other.v0);
            box.u1 = std::min(u1, other.u1);
            box.v1 = std::min(v1, other.v1);
            box.empty = !(box.u1 >= box.u0) || !(box.v1 >= box.v0);

            return box;
        }
    };

    int
    clampTexel(double t,
               int size)
    {
        if (!(t > 0.)) {
            return 0;
        }

        return (t >= (double)size) ? size : (int)t;
    }

    // Calls fn(tile, x1, y1, x2, y2) with the texels of each tile whose centres box may hold, a margin of one texel included.
    template <typename F>
    void
    forEachTile(const UvBox& box,
                int size,
                F&& fn)
    {
        if (box.empty || !std::isfinite(box.u0) || !std::isfinite(box.u1) || !std::isfinite(box.v0) || !std::isfinite(box.v1)) {
            return;
        }
        const int tu0 = std::max(0, (int)std::floor(std::max(box.u0, -1.)));
        const int tu1 = std::min(9, (int)std::floor(std::min(box.u1, 10.)));
        const int tv0 = std::max(0, (int)std::floor(std::max(box.v0, -1.)));
        const int tv1 = std::min(kLastTileRow, (int)std::floor(std::min(box.v1, (double)kLastTileRow + 1.)));
        for (int tv = tv0; tv <= tv1; ++tv) {
            for (int tu = tu0; tu <= tu1; ++tu) {
                const int x1 = clampTexel(std::floor((box.u0 - tu) * size - 0.5), size);
                const int x2 = clampTexel(std::ceil((box.u1 - tu) * size - 0.5) + 1., size);
                const int y1 = clampTexel(std::floor((box.v0 - tv) * size - 0.5), size);
                const int y2 = clampTexel(std::ceil((box.v1 - tv) * size - 0.5) + 1., size);
                if ((x1 < x2) && (y1 < y2)) {
                    fn(1001 + tu + 10 * tv, x1, y1, x2, y2);
                }
            }
        }
    }

    // A target triangle with st, and the affine map from its plane to st.
    struct Triangle {
        GfVec3d corner[3];
        GfVec3d e1;
        GfVec3d e2;
        GfVec3d normal;
        GfVec2d st[3];
        const GfVec2f* stf = nullptr;
        double g11 = 0.;
        double g12 = 0.;
        double g22 = 0.;
        double det = 0.;

        bool init(const SceneRaster::TriangulatedMesh& mesh,
                  int t)
        {
            if ((t < 0) || ((std::size_t)t >= mesh.triangles.size()) || ((std::size_t)t >= mesh.hasSt.size()) || !mesh.hasSt[t] || ((std::size_t)(3 * t + 2) >= mesh.st.size()) || ((std::size_t)t >= mesh.normals.size())) {
                return false;
            }
            const GfVec3i& indices = mesh.triangles[t];
            for (int k = 0; k < 3; ++k) {
                if ((indices[k] < 0) || ((std::size_t)indices[k] >= mesh.worldPoints.size())) {
                    return false;
                }
                corner[k] = GfVec3d(mesh.worldPoints[indices[k]]);
                st[k] = GfVec2d(mesh.st[3 * t + k]);
            }
            stf = &mesh.st[3 * t];
            normal = GfVec3d(mesh.normals[t]);
            e1 = corner[1] - corner[0];
            e2 = corner[2] - corner[0];
            g11 = GfDot(e1, e1);
            g12 = GfDot(e1, e2);
            g22 = GfDot(e2, e2);
            det = g11 * g22 - g12 * g12;

            return (det > 1e-12 * g11 * g22) && (GfDot(normal, normal) > 0.);
        }

        // X's projection onto the plane, as corner[0] + s * e1 + t * e2.
        void coordinates(const GfVec3d& x,
                         double* s,
                         double* t) const
        {
            const GfVec3d d = x - corner[0];
            const double r1 = GfDot(e1, d);
            const double r2 = GfDot(e2, d);
            *s = (g22 * r1 - g12 * r2) / det;
            *t = (g11 * r2 - g12 * r1) / det;
        }

        GfVec2d stAt(double s,
                     double t) const
        {
            return st[0] + s * (st[1] - st[0]) + t * (st[2] - st[0]);
        }

        // How fast st component k changes along the plane, per world unit, at most.
        double stGradient(int k) const
        {
            const double d1 = st[1][k] - st[0][k];
            const double d2 = st[2][k] - st[0][k];

            return std::sqrt(std::max(0., (g22 * d1 * d1 - 2. * g12 * d1 * d2 + g11 * d2 * d2) / det));
        }

        GfVec3d point(const float* weights) const
        {
            return (double)weights[0] * corner[0] + (double)weights[1] * corner[1] + (double)weights[2] * corner[2];
        }

        UvBox stBox() const
        {
            UvBox box;
            for (int k = 0; k < 3; ++k) {
                box.add(st[k][0], st[k][1]);
            }

            return box;
        }
    };

    struct View {
        GfFrustum frustum;
        GfMatrix4d worldToCamera;
        GfMatrix4d viewProjection;
        bool perspective = true;
        double viewHeight = 1.;
        double width = 1.;
        double height = 1.;

        bool init(const SceneRaster::ExplicitCamera& camera,
                  std::string* error)
        {
            const RectD bounds = USDTexturePaintView::paintSpaceBounds(camera.aspect);
            width = bounds.width();
            height = bounds.height();
            SceneRaster::CameraArgs args;
            args.explicitCamera = camera;
            args.width = (int)width;
            args.height = (int)height;
            if (!SceneRaster::cameraFrustum(UsdStageRefPtr(), args, &frustum, error)) {
                return false;
            }
            worldToCamera = frustum.ComputeViewMatrix();
            viewProjection = worldToCamera * frustum.ComputeProjectionMatrix();
            perspective = frustum.GetProjectionType() == GfFrustum::Perspective;
            viewHeight = camera.viewHeight;

            return true;
        }

        // The world size of one paint-space unit at p.
        double worldPerUnit(const GfVec3d& p) const
        {
            if (!perspective) {
                return viewHeight / height;
            }
            const double depth = -(GfVec4d(p[0], p[1], p[2], 1.) * worldToCamera)[2];

            return (depth > 0.) ? viewHeight * depth / height : 0.;
        }

        bool project(const GfVec3d& p,
                     double* x,
                     double* y) const
        {
            const GfVec4d clip = GfVec4d(p[0], p[1], p[2], 1.) * viewProjection;
            if (!(clip[3] > 0.)) {
                return false;
            }
            *x = (clip[0] / clip[3] + 1.) * 0.5 * width;
            *y = (clip[1] / clip[3] + 1.) * 0.5 * height;

            return std::isfinite(*x) && std::isfinite(*y);
        }

        GfRay ray(double x,
                  double y) const
        {
            return frustum.ComputeRay(GfVec2d(2. * x / width - 1., 2. * y / height - 1.));
        }
    };

    struct Setup {
        const Context* context = nullptr;
        const PaintMeshIndex* index = nullptr;
        View view;
        int size = 0;
        int threads = 1;

        bool cancelled() const
        {
            return context->cancelled && context->cancelled();
        }

        bool raycast(double x,
                     double y,
                     PaintMeshIndex::Hit* hit) const
        {
            return index->raycast(view.frustum, x / view.width, y / view.height, hit) && hit->hasSt && (hit->udimTile >= 1001);
        }
    };

    // The dab's opacity at r from its centre, as renderDot() draws its radial gradient.
    double
    dabAlpha(const Dab& dab,
             double r)
    {
        if (!(r <= dab.externalRadius)) {
            return 0.;
        }
        const std::vector<std::pair<double, double>>& stops = dab.opacityStops;
        if (stops.empty()) {
            return dab.opacity;
        }
        const double span = dab.externalRadius - dab.internalRadius;
        double t = (span > 0.) ? (r - dab.internalRadius) / span : ((r < dab.internalRadius) ? 0. : 1.);
        t = std::max(0., std::min(1., t));
        if (t <= stops.front().first) {
            return stops.front().second;
        }
        for (std::size_t i = 1; i < stops.size(); ++i) {
            if (t <= stops[i].first) {
                const std::pair<double, double>& a = stops[i - 1];
                const std::pair<double, double>& b = stops[i];
                const double w = (b.first > a.first) ? (t - a.first) / (b.first - a.first) : 1.;

                return a.second + (b.second - a.second) * w;
            }
        }

        return stops.back().second;
    }

    struct TexelHit {
        int tile = 0;
        int x = 0;
        int y = 0;
        float alpha = 0.f;
        int mesh = -1;
        int triangle = -1;
        GfVec3f position = GfVec3f(0.f);
        GfVec3f normal = GfVec3f(0.f);
    };

    bool
    texelBefore(const TexelHit& a,
                const TexelHit& b)
    {
        if (a.tile != b.tile) {
            return a.tile < b.tile;
        }
        if (a.y != b.y) {
            return a.y < b.y;
        }

        return a.x < b.x;
    }

    // The target triangles whose view bounds meet a rectangle of paint space.
    class ScreenBins {
    public:
        void build(const Setup& s,
                   const RectD& area)
        {
            _area = area;
            _cellWidth = std::max(1e-9, area.width() / kCells);
            _cellHeight = std::max(1e-9, area.height() / kCells);
            _cells.assign((std::size_t)kCells * kCells, std::vector<TriangleRef>());
            const std::vector<SceneRaster::TriangulatedMesh>& targets = s.index->targets();
            for (int m = 0; m < (int)targets.size(); ++m) {
                for (int t = 0; t < (int)targets[m].triangles.size(); ++t) {
                    Triangle tri;
                    if (!tri.init(targets[m], t)) {
                        continue;
                    }
                    TriangleRef ref;
                    ref.mesh = m;
                    ref.triangle = t;
                    double x0 = 0., y0 = 0., x1 = 0., y1 = 0.;
                    int projected = 0;
                    for (int k = 0; k < 3; ++k) {
                        double x, y;
                        if (!s.view.project(tri.corner[k], &x, &y)) {
                            continue;
                        }
                        x0 = projected ? std::min(x0, x) : x;
                        x1 = projected ? std::max(x1, x) : x;
                        y0 = projected ? std::min(y0, y) : y;
                        y1 = projected ? std::max(y1, y) : y;
                        ++projected;
                    }
                    if (projected == 0) {
                        continue;
                    }
                    if (projected < 3) {
                        _everywhere.push_back(ref);
                        continue;
                    }
                    if ((x1 < area.x1) || (x0 > area.x2) || (y1 < area.y1) || (y0 > area.y2)) {
                        continue;
                    }
                    int cx0, cy0, cx1, cy1;
                    cells(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1);
                    for (int cy = cy0; cy <= cy1; ++cy) {
                        for (int cx = cx0; cx <= cx1; ++cx) {
                            _cells[(std::size_t)cy * kCells + cx].push_back(ref);
                        }
                    }
                }
            }
        }

        void query(double x0,
                   double y0,
                   double x1,
                   double y1,
                   std::vector<TriangleRef>* refs) const
        {
            refs->assign(_everywhere.begin(), _everywhere.end());
            if ((x1 >= _area.x1) && (x0 <= _area.x2) && (y1 >= _area.y1) && (y0 <= _area.y2)) {
                int cx0, cy0, cx1, cy1;
                cells(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1);
                for (int cy = cy0; cy <= cy1; ++cy) {
                    for (int cx = cx0; cx <= cx1; ++cx) {
                        const std::vector<TriangleRef>& cell = _cells[(std::size_t)cy * kCells + cx];
                        refs->insert(refs->end(), cell.begin(), cell.end());
                    }
                }
            }
            std::sort(refs->begin(), refs->end());
            refs->erase(std::unique(refs->begin(), refs->end()), refs->end());
        }

    private:
        static constexpr int kCells = 64;

        void cells(double x0,
                   double y0,
                   double x1,
                   double y1,
                   int* cx0,
                   int* cy0,
                   int* cx1,
                   int* cy1) const
        {
            const auto cell = [](double t, double origin, double extent) {
                const double c = std::floor((t - origin) / extent);

                return (int)std::max(0., std::min((double)(kCells - 1), c));
            };
            *cx0 = cell(x0, _area.x1, _cellWidth);
            *cx1 = cell(x1, _area.x1, _cellWidth);
            *cy0 = cell(y0, _area.y1, _cellHeight);
            *cy1 = cell(y1, _area.y1, _cellHeight);
        }

        RectD _area;
        double _cellWidth = 1.;
        double _cellHeight = 1.;
        std::vector<std::vector<TriangleRef>> _cells;
        std::vector<TriangleRef> _everywhere;
    };

    struct DabPlan {
        bool valid = false;
        GfVec3d centre = GfVec3d(0.);
        double worldPerUnit = 0.;
        std::vector<TriangleRef> triangles;
    };

    // The st bounds, on tri's plane, of what the dab's disc covers in the view.
    UvBox
    viewClip(const Setup& s,
             const Dab& dab,
             const Triangle& tri,
             const UvBox& triBox)
    {
        // The polygon's corners lie outside the disc, so its preimage on the plane holds the disc's.
        const int kSides = 8;
        const double reach = dab.externalRadius / std::cos(M_PI / kSides);
        UvBox box;
        for (int i = 0; i < kSides; ++i) {
            const double angle = 2. * M_PI * i / kSides;
            const GfRay ray = s.view.ray(dab.center.x + reach * std::cos(angle), dab.center.y + reach * std::sin(angle));
            const GfVec3d direction = ray.GetDirection();
            const double denominator = GfDot(direction, tri.normal);
            if (!(std::abs(denominator) > 1e-12 * direction.GetLength())) {
                return triBox;
            }
            const double t = GfDot(tri.corner[0] - ray.GetStartPoint(), tri.normal) / denominator;
            if (!(t > 0.)) {
                return triBox;
            }
            double a, b;
            tri.coordinates(ray.GetStartPoint() + t * direction, &a, &b);
            const GfVec2d st = tri.stAt(a, b);
            box.add(st[0], st[1]);
        }

        return box.intersect(triBox);
    }

    void
    splatTriangle(const Setup& s,
                  const Dab& dab,
                  const DabPlan& plan,
                  const TriangleRef& ref,
                  std::vector<TexelHit>* hits)
    {
        Triangle tri;
        if (!tri.init(s.index->targets()[ref.mesh], ref.triangle)) {
            return;
        }
        const bool surface = s.context->footprint == eFootprintSurface;
        const double radius = dab.externalRadius * plan.worldPerUnit;
        const UvBox triBox = tri.stBox();
        UvBox clip;
        if (surface) {
            const double distance = GfDot(plan.centre - tri.corner[0], tri.normal);
            const double rho2 = radius * radius - distance * distance;
            if (rho2 < 0.) {
                return;
            }
            const double rho = std::sqrt(rho2);
            double a, b;
            tri.coordinates(plan.centre - distance * tri.normal, &a, &b);
            const GfVec2d st = tri.stAt(a, b);
            const double halfU = rho * tri.stGradient(0);
            const double halfV = rho * tri.stGradient(1);
            clip.add(st[0] - halfU, st[1] - halfV);
            clip.add(st[0] + halfU, st[1] + halfV);
            clip = clip.intersect(triBox);
        } else {
            clip = viewClip(s, dab, tri, triBox);
        }
        const GfVec3f normal(tri.normal);
        forEachTile(clip, s.size, [&](int tile, int x1, int y1, int x2, int y2) {
            SceneRaster::rasterizeTexels(tri.stf, tile, s.size, x1, y1, x2, y2, [&](int x, int y, const float* weights) {
                const GfVec3d p = tri.point(weights);
                double alpha;
                if (surface) {
                    const double r = (p - plan.centre).GetLength();
                    if (!(r <= radius)) {
                        return;
                    }
                    alpha = dabAlpha(dab, r / plan.worldPerUnit);
                } else {
                    double px, py;
                    if (!s.view.project(p, &px, &py)) {
                        return;
                    }
                    alpha = dabAlpha(dab, std::hypot(px - dab.center.x, py - dab.center.y));
                }
                if (!(alpha > 0.)) {
                    return;
                }
                TexelHit hit;
                hit.tile = tile;
                hit.x = x;
                hit.y = y;
                hit.alpha = (float)alpha;
                hit.mesh = ref.mesh;
                hit.triangle = ref.triangle;
                hit.position = GfVec3f(p);
                hit.normal = normal;
                hits->push_back(hit);
            });
        });
    }

    /**
     * The texels each dab covers, visible from the camera: per dab ordered by tile, row and column,
     * each texel once, with the greatest opacity any of its triangles gives it.
     **/
    std::vector<std::vector<TexelHit>>
    footprints(const Setup& s,
               const std::vector<Dab>& dabs,
               const ScreenBins* bins)
    {
        const int count = (int)dabs.size();
        std::vector<DabPlan> plans(count);
        parallelForOnGlobalPool(count, s.threads, [&](int i) {
            const Dab& dab = dabs[i];
            DabPlan& plan = plans[i];
            if (s.context->footprint == eFootprintSurface) {
                PaintMeshIndex::Hit hit;
                if (!s.index->raycast(s.view.frustum, dab.center.x / s.view.width, dab.center.y / s.view.height, &hit)) {
                    return;
                }
                plan.centre = GfVec3d(hit.position);
                plan.worldPerUnit = s.view.worldPerUnit(plan.centre);
                if (!(plan.worldPerUnit > 0.)) {
                    return;
                }
                plan.triangles = s.index->trianglesInSphere(hit.position, (float)(dab.externalRadius * plan.worldPerUnit));
            } else {
                const double r = dab.externalRadius;
                bins->query(dab.center.x - r, dab.center.y - r, dab.center.x + r, dab.center.y + r, &plan.triangles);
            }
            plan.valid = !plan.triangles.empty();
        });

        std::vector<std::pair<int, int>> pairs;
        for (int i = 0; i < count; ++i) {
            if (plans[i].valid) {
                for (int t = 0; t < (int)plans[i].triangles.size(); ++t) {
                    pairs.push_back(std::make_pair(i, t));
                }
            }
        }
        std::vector<std::vector<TexelHit>> pairHits(pairs.size());
        parallelForOnGlobalPool((int)pairs.size(), s.threads, [&](int p) {
            const int i = pairs[p].first;
            splatTriangle(s, dabs[i], plans[i], plans[i].triangles[pairs[p].second], &pairHits[p]);
        });

        std::vector<std::size_t> firstPair(count, pairs.size());
        for (std::size_t p = pairs.size(); p-- > 0;) {
            firstPair[pairs[p].first] = p;
        }

        std::vector<std::vector<TexelHit>> result(count);
        parallelForOnGlobalPool(count, s.threads, [&](int i) {
            if (!plans[i].valid) {
                return;
            }
            std::vector<TexelHit> all;
            for (std::size_t p = firstPair[i]; (p < pairs.size()) && (pairs[p].first == i); ++p) {
                all.insert(all.end(), pairHits[p].begin(), pairHits[p].end());
            }
            if (all.empty()) {
                return;
            }
            std::vector<GfVec3f> points(all.size());
            std::vector<GfVec3f> normals(all.size());
            for (std::size_t h = 0; h < all.size(); ++h) {
                points[h] = all[h].position;
                normals[h] = all[h].normal;
            }
            std::vector<std::uint8_t> seen(all.size(), 0);
            s.index->visible(points.data(), normals.data(), all.size(), s.view.frustum, s.context->depthBias, seen.data());
            std::vector<TexelHit> kept;
            kept.reserve(all.size());
            for (std::size_t h = 0; h < all.size(); ++h) {
                if (seen[h]) {
                    kept.push_back(all[h]);
                }
            }
            std::stable_sort(kept.begin(), kept.end(), texelBefore);
            std::vector<TexelHit>& out = result[i];
            for (const TexelHit& hit : kept) {
                if (!out.empty() && !texelBefore(out.back(), hit)) {
                    if (hit.alpha > out.back().alpha) {
                        out.back() = hit;
                    }
                    continue;
                }
                out.push_back(hit);
            }
        });

        return result;
    }

    RectD
    dabArea(const std::vector<Dab>& dabs)
    {
        RectD area;
        bool first = true;
        for (const Dab& dab : dabs) {
            const double r = dab.externalRadius;
            const RectD disc(dab.center.x - r, dab.center.y - r, dab.center.x + r, dab.center.y + r);
            if (first) {
                area = disc;
                first = false;
            } else {
                area.merge(disc);
            }
        }

        return area;
    }

    const PageData&
    zeroPage()
    {
        static const PageData page;

        return page;
    }

    // A page the stroke wrote, as the pages held it before; base holds what the stroke found when
    // the page held only its base.
    struct Written {
        TexelPages::Snapshot journal;
        std::vector<GfHalf> base;
    };

    // Pages as they were before the stroke, though it may have written some of them since; zero when null.
    class PageSource {
    public:
        explicit PageSource(TexelPages* pages,
                            const std::map<PageKey, Written>* written = nullptr)
            : _pages(pages)
            , _written(written)
        {
        }

        bool empty() const
        {
            return !_pages;
        }

        bool read(const PageKey& key,
                  PageData* page,
                  std::string* error) const
        {
            if (_written) {
                const std::map<PageKey, Written>::const_iterator it = _written->find(key);
                if (it != _written->end()) {
                    if (it->second.journal.page) {
                        return TexelPages::readPage(it->second.journal.page, page, error);
                    }
                    TexelPages::unpack(it->second.base.data(), page);

                    return true;
                }
            }
            if (!_pages) {
                *page = PageData();

                return true;
            }

            return _pages->read(key, page, error);
        }

    private:
        TexelPages* _pages;
        const std::map<PageKey, Written>* _written;
    };

    struct PageWork {
        PageKey key;
        // This update's hits on the page in dab order: (dab, index in its footprint).
        std::vector<std::pair<int, int>> entries;
        std::vector<float> mask;
        // Per texel, the index in hits of the first hit over it, or -1; 0 when the brush needs no geometry.
        std::vector<int> first;
        std::vector<TexelHit> hits;
        // Texels the brush changed.
        std::vector<std::uint8_t> laid;
        PageData data;
        std::string error;
    };
    typedef std::map<PageKey, PageWork> Works;

    // Reads texels of some pages, through a cache of its own, so that each thread needs one.
    class Sampler {
    public:
        // Reads overlay's pages, then under's, then source's.
        Sampler(const PageSource& source,
                int plane,
                int size,
                const Works* overlay = nullptr,
                const Works* under = nullptr)
            : _source(source)
            , _plane(plane)
            , _size(size)
            , _overlay(overlay)
            , _under(under)
        {
        }

        const PageData* page(const PageKey& key,
                             std::string* error)
        {
            if (_last && (key == _lastKey)) {
                return _last;
            }
            const PageData* found = nullptr;
            for (const Works* works : { _overlay, _under }) {
                if (!found && works) {
                    const Works::const_iterator it = works->find(key);
                    if (it != works->end()) {
                        found = &it->second.data;
                    }
                }
            }
            if (!found && _source.empty()) {
                found = &zeroPage();
            }
            if (!found) {
                std::unique_ptr<PageData>& cached = _cache[key];
                if (!cached) {
                    std::unique_ptr<PageData> read(new PageData);
                    if (!_source.read(key, read.get(), error)) {
                        _cache.erase(key);

                        return nullptr;
                    }
                    cached = std::move(read);
                }
                found = cached.get();
            }
            _lastKey = key;
            _last = found;

            return found;
        }

        bool texel(int tile,
                   int x,
                   int y,
                   float* rgba,
                   float* touched,
                   std::string* error)
        {
            const PageData* data = page(TexelPages::pageOf(tile, _plane, x, y), error);
            if (!data) {
                return false;
            }
            const int i = localIndex(x, y);
            std::copy_n(data->rgba.data() + (std::size_t)i * 4, 4, rgba);
            if (touched) {
                *touched = data->touched[i];
            }

            return true;
        }

        // Bilinearly at st within tile, the tile's edges clamped.
        bool bilinear(int tile,
                      const GfVec2f& st,
                      float* rgba,
                      std::string* error)
        {
            const double fx = (st[0] - (double)((tile - 1001) % 10)) * _size - 0.5;
            const double fy = (st[1] - (double)((tile - 1001) / 10)) * _size - 0.5;
            const double x0 = std::floor(fx);
            const double y0 = std::floor(fy);
            const float wx = (float)(fx - x0);
            const float wy = (float)(fy - y0);
            const auto clampIndex = [this](double t) {
                return (int)std::max(0., std::min((double)(_size - 1), t));
            };
            const int xs[2] = { clampIndex(x0), clampIndex(x0 + 1.) };
            const int ys[2] = { clampIndex(y0), clampIndex(y0 + 1.) };
            const float weights[4] = { (1.f - wx) * (1.f - wy), wx * (1.f - wy), (1.f - wx) * wy, wx * wy };
            std::fill_n(rgba, 4, 0.f);
            for (int k = 0; k < 4; ++k) {
                float value[4];
                if (!texel(tile, xs[k & 1], ys[k >> 1], value, nullptr, error)) {
                    return false;
                }
                for (int c = 0; c < 4; ++c) {
                    rgba[c] += weights[k] * value[c];
                }
            }

            return true;
        }

    private:
        PageSource _source;
        int _plane;
        int _size;
        const Works* _overlay;
        const Works* _under;
        std::map<PageKey, std::unique_ptr<PageData>> _cache;
        PageKey _lastKey;
        const PageData* _last = nullptr;
    };

    // out = from + (to - from) * m on the stroke's channels, from elsewhere.
    void
    blendTowards(const Stroke& stroke,
                 const float* from,
                 const float* to,
                 float m,
                 float* out)
    {
        for (int c = 0; c < 4; ++c) {
            out[c] = stroke.channels[c] ? from[c] + (to[c] - from[c]) * m : from[c];
        }
    }

    // source over below.
    void
    over(const float* source,
         const float* below,
         float* out)
    {
        for (int c = 0; c < 4; ++c) {
            out[c] = source[c] + below[c] * (1.f - source[3]);
        }
    }

    // The paint at the point of the view that (x, y) copies from through the 3x3 transform.
    bool
    transformPoint(const double* transform,
                   double x,
                   double y,
                   double* sx,
                   double* sy)
    {
        const double w = transform[6] * x + transform[7] * y + transform[8];
        if (!(std::abs(w) > 0.)) {
            return false;
        }
        *sx = (transform[0] * x + transform[1] * y + transform[2]) / w;
        *sy = (transform[3] * x + transform[4] * y + transform[5]) / w;

        return std::isfinite(*sx) && std::isfinite(*sy);
    }

    // Samples the surface at paint-space (x, y) of the view; false with *error empty on a miss.
    bool
    sampleView(const Setup& s,
               Sampler& sampler,
               double x,
               double y,
               float* rgba,
               std::string* error)
    {
        PaintMeshIndex::Hit hit;
        if (!s.raycast(x, y, &hit)) {
            return false;
        }

        return sampler.bilinear(hit.udimTile, hit.st, rgba, error);
    }

    bool
    sampleRay(const Setup& s,
              Sampler& sampler,
              const GfVec3d& origin,
              const GfVec3d& toward,
              float* rgba,
              bool* found,
              std::string* error)
    {
        GfVec3d direction = toward - origin;
        const double length = direction.Normalize();
        if (!(length > 0.)) {
            return true;
        }
        PaintMeshIndex::Hit hit;
        if (!s.index->raycast(GfVec3f(origin), GfVec3f(direction), 0.f, (float)length, &hit) || !hit.hasSt || (hit.udimTile < 1001)) {
            return true;
        }
        if (!sampler.bilinear(hit.udimTile, hit.st, rgba, error)) {
            return false;
        }
        *found = true;

        return true;
    }

    /**
     * The surface's paint at offset from geometry's position along its tangent plane. Off its triangle,
     * the surface is found by short rays: along the plane, for a surface rising in front of it; down
     * onto the point, for one carrying on flat or curving gently; from the point back under the texel,
     * for one folding away.
     **/
    bool
    sampleSurface(const Setup& s,
                  Sampler& sampler,
                  const Triangle& tri,
                  const GfVec3d& position,
                  const GfVec3d& offset,
                  float* rgba,
                  bool* found,
                  std::string* error)
    {
        *found = false;
        const GfVec3d target = position + offset;
        double a, b;
        tri.coordinates(target, &a, &b);
        const double kInside = -1e-6;
        if ((a >= kInside) && (b >= kInside) && (1. - a - b >= kInside)) {
            const GfVec2d st = tri.stAt(a, b);
            const int tile = SceneRaster::udimTileOf((float)st[0], (float)st[1]);
            if (tile < 1001) {
                return true;
            }
            if (!sampler.bilinear(tile, GfVec2f(st), rgba, error)) {
                return false;
            }
            *found = true;

            return true;
        }
        const double reach = offset.GetLength();
        const GfVec3d lift = tri.normal * reach;
        if (!sampleRay(s, sampler, position + tri.normal * (reach * 1e-3), target, rgba, found, error)) {
            return false;
        }
        if (!*found && !sampleRay(s, sampler, target + lift, target - lift, rgba, found, error)) {
            return false;
        }
        if (!*found && !sampleRay(s, sampler, target, position - lift, rgba, found, error)) {
            return false;
        }

        return true;
    }

    // A Gaussian of the paint around a texel over the surface, as RotoPaint blurs by the brush's effect.
    bool
    blurAt(const Setup& s,
           const Stroke& stroke,
           Sampler& sampler,
           const TexelHit& hit,
           const float* centre,
           float* rgba,
           std::string* error)
    {
        Triangle tri;
        const GfVec3d position(hit.position);
        const double worldPerUnit = s.view.worldPerUnit(position);
        if (!tri.init(s.index->targets()[hit.mesh], hit.triangle) || !(worldPerUnit > 0.)) {
            std::copy_n(centre, 4, rgba);

            return true;
        }
        const double sigma = stroke.effectStrength / 2.4 * worldPerUnit;
        if (!(sigma > 0.)) {
            std::copy_n(centre, 4, rgba);

            return true;
        }
        GfVec3d tangent = GfCross(tri.normal, (std::abs(tri.normal[0]) < 0.9) ? GfVec3d(1., 0., 0.) : GfVec3d(0., 1., 0.));
        tangent.Normalize();
        const GfVec3d bitangent = GfCross(tri.normal, tangent);
        const int kSteps = 6;
        const double step = sigma * 0.5;
        double sum[4] = { 0., 0., 0., 0. };
        double total = 0.;
        for (int j = -kSteps; j <= kSteps; ++j) {
            for (int i = -kSteps; i <= kSteps; ++i) {
                const double r2 = (double)(i * i + j * j) * step * step;
                if (r2 > 9. * sigma * sigma) {
                    continue;
                }
                const double weight = std::exp(-r2 / (2. * sigma * sigma));
                float value[4];
                bool found = true;
                if ((i == 0) && (j == 0)) {
                    std::copy_n(centre, 4, value);
                } else if (!sampleSurface(s, sampler, tri, position, (i * step) * tangent + (j * step) * bitangent, value, &found, error)) {
                    return false;
                }
                if (!found) {
                    continue;
                }
                for (int c = 0; c < 4; ++c) {
                    sum[c] += weight * value[c];
                }
                total += weight;
            }
        }
        for (int c = 0; c < 4; ++c) {
            rgba[c] = (float)(sum[c] / total);
        }

        return true;
    }

    // What the brush makes of one texel under mask m, into out; false with *error empty to leave it.
    bool
    brushTexel(const Setup& s,
               const Stroke& stroke,
               Sampler& source,
               const TexelHit& hit,
               const float* below,
               const float* base,
               float m,
               float* out,
               std::string* error)
    {
        float target[4];
        switch (stroke.type) {
        case eRotoStrokeTypeSolid:
            for (int c = 0; c < 3; ++c) {
                target[c] = (float)stroke.color[c];
            }
            target[3] = 1.f;
            break;
        case eRotoStrokeTypeEraser:
            std::copy_n(base, 4, target);
            break;
        case eRotoStrokeTypeDodge:
        case eRotoStrokeTypeBurn: {
            const MergeOps::AlphaSpan alpha = { below + 3, 4, 0 };
            MergeOps::mergeRow((stroke.type == eRotoStrokeTypeDodge) ? MergeOps::eColorDodge : MergeOps::eColorBurn, 4, below, alpha, below, alpha, 1, target);
            break;
        }
        case eRotoStrokeTypeReveal:
            over(base, below, target);
            break;
        case eRotoStrokeTypeClone: {
            double x, y, sx, sy;
            float copied[4];
            if (!s.view.project(GfVec3d(hit.position), &x, &y) || !transformPoint(stroke.sourceTransform, x, y, &sx, &sy) || !sampleView(s, source, sx, sy, copied, error)) {
                return false;
            }
            over(copied, below, target);
            break;
        }
        case eRotoStrokeTypeBlur:
        case eRotoStrokeTypeSharpen: {
            float blurred[4];
            if (!blurAt(s, stroke, source, hit, below, blurred, error)) {
                return false;
            }
            for (int c = 0; c < 4; ++c) {
                target[c] = (stroke.type == eRotoStrokeTypeBlur) ? blurred[c] : below[c] + (below[c] - blurred[c]);
            }
            break;
        }
        default:
            return false;
        }
        blendTowards(stroke, below, target, m, out);

        return true;
    }

    // Clone, Blur and Sharpen read the surface around a texel; the other brushes only the texel.
    bool
    needsGeometry(RotoStrokeType type)
    {
        return (type == eRotoStrokeTypeClone) || (type == eRotoStrokeTypeBlur) || (type == eRotoStrokeTypeSharpen);
    }

    /**
     * Adds dabs to the masks of the pages under them, each page copied into changed from works when
     * there, and lays the brush again from before on the texels whose mask they changed. Laying a
     * stroke's dabs in any number of runs ends with the same pages as laying them in one.
     **/
    bool
    layMasked(const Setup& s,
              const Stroke& stroke,
              const PageSource& before,
              const Works& works,
              const std::vector<Dab>& dabs,
              Works* changed,
              std::string* error)
    {
        const int plane = s.context->plane;
        std::unique_ptr<ScreenBins> bins;
        if (s.context->footprint == eFootprintView) {
            bins.reset(new ScreenBins);
            bins->build(s, dabArea(dabs));
        }
        const std::vector<std::vector<TexelHit>> hits = footprints(s, dabs, bins.get());
        if (s.cancelled()) {
            return false;
        }
        for (int d = 0; d < (int)dabs.size(); ++d) {
            for (int h = 0; h < (int)hits[d].size(); ++h) {
                const TexelHit& hit = hits[d][h];
                const PageKey key = TexelPages::pageOf(hit.tile, plane, hit.x, hit.y);
                Works::iterator it = changed->find(key);
                if (it == changed->end()) {
                    const Works::const_iterator old = works.find(key);
                    it = changed->insert(std::make_pair(key, (old != works.end()) ? old->second : PageWork())).first;
                    it->second.key = key;
                }
                it->second.entries.push_back(std::make_pair(d, h));
            }
        }
        std::vector<PageWork*> list;
        for (Works::iterator it = changed->begin(); it != changed->end(); ++it) {
            list.push_back(&it->second);
        }

        const bool geometry = needsGeometry(stroke.type);
        const PageSource source = (stroke.type == eRotoStrokeTypeClone) && stroke.cloneFromBase ? PageSource(s.context->base) : before;
        const PageSource base(s.context->base);
        const bool needsBase = (stroke.type == eRotoStrokeTypeEraser) || (stroke.type == eRotoStrokeTypeReveal);
        parallelForOnGlobalPool((int)list.size(), s.threads, [&](int p) {
            PageWork& work = *list[p];
            PageData pre;
            if (!before.read(work.key, &pre, &work.error)) {
                return;
            }
            if (work.mask.empty()) {
                work.mask.assign(TexelPages::kPageTexels, 0.f);
                work.first.assign(TexelPages::kPageTexels, -1);
                work.laid.assign(TexelPages::kPageTexels, 0);
                work.data = pre;
            }
            std::vector<std::uint8_t> hit((std::size_t)TexelPages::kPageTexels, 0);
            for (const std::pair<int, int>& entry : work.entries) {
                const TexelHit& texel = hits[entry.first][entry.second];
                const int i = localIndex(texel.x, texel.y);
                const float m = texel.alpha;
                float& mask = work.mask[i];
                if (work.first[i] < 0) {
                    if (geometry) {
                        work.first[i] = (int)work.hits.size();
                        work.hits.push_back(texel);
                    } else {
                        work.first[i] = 0;
                    }
                    mask = m;
                } else {
                    mask = stroke.buildUp ? m + mask * (1.f - m) : std::max(mask, m);
                }
                hit[i] = 1;
            }
            work.entries.clear();
            work.entries.shrink_to_fit();
            if (s.cancelled()) {
                return;
            }
            PageData basePage;
            if (needsBase && !base.read(work.key, &basePage, &work.error)) {
                return;
            }
            static const TexelHit noGeometry;
            Sampler sampler(source, plane, s.size);
            for (int i = 0; i < TexelPages::kPageTexels; ++i) {
                const float m = work.mask[i];
                if (!hit[i] || !(m > 0.f)) {
                    continue;
                }
                const float* below = pre.rgba.data() + (std::size_t)i * 4;
                float* rgba = work.data.rgba.data() + (std::size_t)i * 4;
                float out[4];
                if (!brushTexel(s, stroke, sampler, geometry ? work.hits[work.first[i]] : noGeometry, below, basePage.rgba.data() + (std::size_t)i * 4, m, out, &work.error)) {
                    if (!work.error.empty()) {
                        return;
                    }
                    std::copy_n(below, 4, rgba);
                    work.data.touched[i] = pre.touched[i];
                    work.laid[i] = 0;
                    continue;
                }
                std::copy_n(out, 4, rgba);
                const float touched = pre.touched[i];
                work.data.touched[i] = (stroke.type == eRotoStrokeTypeEraser) ? touched * (1.f - m) : m + touched * (1.f - m);
                work.laid[i] = 1;
            }
        });
        for (PageWork* work : list) {
            if (!work->error.empty()) {
                *error = work->error;

                return false;
            }
        }

        return !s.cancelled();
    }

    // The dabs RotoPaint's Smear lays along strokes: each copies what lies at from to to.
    struct SmearDab {
        Point from;
        Point to;
    };

    std::vector<SmearDab>
    smearDabs(const RotoStrokeDabs::Strokes& strokes,
              const RotoStrokeDabs::BrushParams& brush)
    {
        std::vector<SmearDab> dabs;
        const double spacing = std::max(brush.brushSpacing, 0.05);
        const double maxDistPerSegment = brush.brushSizePixel * spacing;
        const double halfSize = maxDistPerSegment / 2.;
        if ((brush.writeOnEnd - brush.writeOnStart) <= 0.) {
            return dabs;
        }
        for (const RotoStrokeDabs::StrokePoints& points : strokes) {
            if (points.size() <= 1) {
                continue;
            }
            const int firstPoint = (int)std::floor(points.size() * brush.writeOnStart);
            const int endPoint = (int)std::ceil(points.size() * brush.writeOnEnd);
            if ((firstPoint < 0) || (endPoint <= firstPoint) || (endPoint > (int)points.size())) {
                continue;
            }
            std::vector<std::pair<Point, double>> visible;
            RotoStrokeDabs::StrokePoints::const_iterator it = points.begin();
            std::advance(it, firstPoint);
            for (int i = firstPoint; i < endPoint; ++i, ++it) {
                visible.push_back(*it);
            }
            if (visible.size() <= 1) {
                continue;
            }
            std::pair<Point, double> prev = visible[0];
            std::pair<Point, double> renderPoint = visible[1];
            dabs.push_back(SmearDab { prev.first, renderPoint.first });
            prev = renderPoint;
            std::pair<Point, double> cur = (visible.size() > 2) ? visible[2] : prev;
            double distToNext = 0.;
            for (std::size_t i = 2; i < visible.size();) {
                const double dx = visible[i].first.x - cur.first.x;
                const double dy = visible[i].first.y - cur.first.y;
                const double dist = std::sqrt(dx * dx + dy * dy);
                distToNext += dist;
                if ((distToNext < maxDistPerSegment) || (dist == 0)) {
                    cur = visible[i];
                    ++i;
                    continue;
                }
                double a;
                if (maxDistPerSegment >= dist) {
                    a = ((distToNext - dist) == 0) ? (maxDistPerSegment - dist) / dist : (maxDistPerSegment - dist) / (distToNext - dist);
                } else {
                    a = maxDistPerSegment / dist;
                }
                renderPoint.first.x = dx * a + cur.first.x;
                renderPoint.first.y = dy * a + cur.first.y;
                renderPoint.second = (visible[i].second - cur.second) * a + cur.second;
                const double vx = renderPoint.first.x - prev.first.x;
                const double vy = renderPoint.first.y - prev.first.y;
                Point from;
                from.x = prev.first.x + std::max(0., std::min(.7, std::abs(vx / halfSize))) * vx;
                from.y = prev.first.y + std::max(0., std::min(.7, std::abs(vy / halfSize))) * vy;
                dabs.push_back(SmearDab { from, renderPoint.first });
                prev = renderPoint;
                cur = renderPoint;
                distToNext = 0;
            }
        }

        return dabs;
    }

    /**
     * Smear's dabs one after the other, each reading what the ones before it laid: the pages they
     * change are copied into changed from works when there, else read from before.
     **/
    bool
    laySmear(const Setup& s,
             const Stroke& stroke,
             const PageSource& before,
             const Works& works,
             const std::vector<SmearDab>& smears,
             Works* changed,
             std::string* error)
    {
        const int plane = s.context->plane;
        std::vector<Dab> dabs(smears.size());
        for (std::size_t i = 0; i < smears.size(); ++i) {
            Dab& dab = dabs[i];
            double spacing;
            getRenderDotParams(stroke.opacity, stroke.brush.brushSizePixel, stroke.brush.brushHardness, 0., 1., false, false, false,
                               &dab.internalRadius, &dab.externalRadius, &spacing, &dab.opacityStops);
            dab.center = smears[i].to;
            dab.pressure = 1.;
            dab.buildUp = true;
            dab.opacity = stroke.opacity;
        }
        std::unique_ptr<ScreenBins> bins;
        if ((s.context->footprint == eFootprintView) && !dabs.empty()) {
            bins.reset(new ScreenBins);
            bins->build(s, dabArea(dabs));
        }

        for (std::size_t d = 0; d < dabs.size(); ++d) {
            if (s.cancelled()) {
                return false;
            }
            const std::vector<std::vector<TexelHit>> found = footprints(s, std::vector<Dab>(1, dabs[d]), bins.get());
            const std::vector<TexelHit>& hits = found[0];
            if (hits.empty()) {
                continue;
            }
            const double shiftX = smears[d].from.x - smears[d].to.x;
            const double shiftY = smears[d].from.y - smears[d].to.y;
            const int kChunk = 256;
            const int chunks = ((int)hits.size() + kChunk - 1) / kChunk;
            std::vector<float> values(hits.size() * 5, 0.f);
            std::vector<std::uint8_t> laid(hits.size(), 0);
            std::vector<std::string> errors(chunks);
            parallelForOnGlobalPool(chunks, s.threads, [&](int c) {
                Sampler sampler(before, plane, s.size, changed, &works);
                for (int h = c * kChunk; h < std::min((int)hits.size(), (c + 1) * kChunk); ++h) {
                    const TexelHit& hit = hits[h];
                    float below[4];
                    float touched;
                    if (!sampler.texel(hit.tile, hit.x, hit.y, below, &touched, &errors[c])) {
                        return;
                    }
                    double x, y;
                    float copied[4];
                    if (!s.view.project(GfVec3d(hit.position), &x, &y) || !sampleView(s, sampler, x + shiftX, y + shiftY, copied, &errors[c])) {
                        if (!errors[c].empty()) {
                            return;
                        }
                        continue;
                    }
                    blendTowards(stroke, below, copied, hit.alpha, &values[(std::size_t)h * 5]);
                    values[(std::size_t)h * 5 + 4] = hit.alpha + touched * (1.f - hit.alpha);
                    laid[h] = 1;
                }
            });
            for (const std::string& message : errors) {
                if (!message.empty()) {
                    *error = message;

                    return false;
                }
            }
            for (std::size_t h = 0; h < hits.size(); ++h) {
                if (!laid[h]) {
                    continue;
                }
                const TexelHit& hit = hits[h];
                const PageKey key = TexelPages::pageOf(hit.tile, plane, hit.x, hit.y);
                Works::iterator it = changed->find(key);
                if (it == changed->end()) {
                    const Works::const_iterator old = works.find(key);
                    if (old != works.end()) {
                        it = changed->insert(*old).first;
                    } else {
                        PageWork work;
                        work.key = key;
                        if (!before.read(key, &work.data, error)) {
                            return false;
                        }
                        work.laid.assign(TexelPages::kPageTexels, 0);
                        it = changed->insert(std::make_pair(key, std::move(work))).first;
                    }
                }
                const int i = localIndex(hit.x, hit.y);
                std::copy_n(&values[h * 5], 4, it->second.data.rgba.data() + (std::size_t)i * 4);
                it->second.data.touched[i] = values[h * 5 + 4];
                it->second.laid[i] = 1;
            }
        }

        return !s.cancelled();
    }

    enum GutterState : std::uint8_t {
        eGutterFree,
        eGutterCovered,
        eGutterLaid
    };

    // What growing the seam gutter puts on a page: (texel, rgba and touched) of each texel it fills.
    typedef std::vector<std::pair<int, std::array<float, 5>>> Grown;

    // The pages whose gutter the paint on keys feeds: theirs and their neighbours'.
    std::vector<PageKey>
    gutterTargets(const TexelPages& pages,
                  const std::set<PageKey>& keys)
    {
        const int pagesPerSide = pages.pagesPerSide();
        std::set<PageKey> targets;
        for (const PageKey& page : keys) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    PageKey key = page;
                    key.px += dx;
                    key.py += dy;
                    if ((key.px >= 0) && (key.py >= 0) && (key.px < pagesPerSide) && (key.py < pagesPerSide)) {
                        targets.insert(key);
                    }
                }
            }
        }

        return std::vector<PageKey>(targets.begin(), targets.end());
    }

    /**
     * Grows the paint laidOn gives kSeamGutter texels into the texels no target covers, each taking
     * the mean of its laid neighbours, so that filtering across an island's edge reads the paint.
     * What each of targets takes depends only on the paint of it and its neighbours.
     **/
    std::vector<Grown>
    gutterOf(const Setup& s,
             const std::function<const PageWork*(const PageKey&)>& laidOn,
             const std::vector<PageKey>& targets)
    {
        const Coverage* coverage = s.context->coverage;
        TexelPages* pages = s.context->pages;
        const int g = TexelPages::kSeamGutter;
        // Per target page: (texel, rgba and touched) of each texel the gutter fills.
        std::vector<std::vector<std::pair<int, std::array<float, 5>>>> grown(targets.size());
        parallelForOnGlobalPool((int)targets.size(), s.threads, [&](int t) {
            const PageKey& key = targets[t];
            const RectI page = pages->texelsOf(key);
            const RectI window = RectI(page.x1 - g, page.y1 - g, page.x2 + g, page.y2 + g).intersect(RectI(0, 0, s.size, s.size));
            const int w = window.width();
            const int h = window.height();
            std::vector<int> owner((std::size_t)w * h, -1);
            coverage->owners(key.tile, window, owner.data());
            std::vector<std::uint8_t> state((std::size_t)w * h, eGutterFree);
            std::vector<std::array<float, 5>> value((std::size_t)w * h);
            bool anyLaid = false;
            PageKey workKey;
            const PageWork* work = nullptr;
            bool haveWork = false;
            for (int y = window.y1; y < window.y2; ++y) {
                for (int x = window.x1; x < window.x2; ++x) {
                    const std::size_t i = (std::size_t)(x - window.x1) + (std::size_t)(y - window.y1) * w;
                    const PageKey at = TexelPages::pageOf(key.tile, key.plane, x, y);
                    if (!haveWork || (at != workKey)) {
                        workKey = at;
                        work = laidOn(at);
                        haveWork = true;
                    }
                    const int local = localIndex(x, y);
                    if (work && !work->laid.empty() && work->laid[local]) {
                        state[i] = eGutterLaid;
                        std::copy_n(work->data.rgba.data() + (std::size_t)local * 4, 4, value[i].data());
                        value[i][4] = work->data.touched[local];
                        anyLaid = true;
                    } else if (owner[i] >= 0) {
                        state[i] = eGutterCovered;
                    }
                }
            }
            if (!anyLaid) {
                return;
            }
            for (int pass = 0; pass < g; ++pass) {
                const int margin = g - 1 - pass;
                const RectI region = RectI(page.x1 - margin, page.y1 - margin, page.x2 + margin, page.y2 + margin).intersect(window);
                std::vector<std::pair<std::size_t, std::array<float, 5>>> filled;
                for (int y = region.y1; y < region.y2; ++y) {
                    for (int x = region.x1; x < region.x2; ++x) {
                        const std::size_t i = (std::size_t)(x - window.x1) + (std::size_t)(y - window.y1) * w;
                        if (state[i] != eGutterFree) {
                            continue;
                        }
                        std::array<float, 5> sum = { 0.f, 0.f, 0.f, 0.f, 0.f };
                        int n = 0;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = x + dx;
                                const int ny = y + dy;
                                if ((nx < window.x1) || (ny < window.y1) || (nx >= window.x2) || (ny >= window.y2)) {
                                    continue;
                                }
                                const std::size_t j = (std::size_t)(nx - window.x1) + (std::size_t)(ny - window.y1) * w;
                                if (state[j] == eGutterLaid) {
                                    for (int c = 0; c < 5; ++c) {
                                        sum[c] += value[j][c];
                                    }
                                    ++n;
                                }
                            }
                        }
                        if (n > 0) {
                            for (int c = 0; c < 5; ++c) {
                                sum[c] /= (float)n;
                            }
                            filled.push_back(std::make_pair(i, sum));
                        }
                    }
                }
                for (const std::pair<std::size_t, std::array<float, 5>>& f : filled) {
                    state[f.first] = eGutterLaid;
                    value[f.first] = f.second;
                    const int x = window.x1 + (int)(f.first % w);
                    const int y = window.y1 + (int)(f.first / w);
                    if ((x >= page.x1) && (x < page.x2) && (y >= page.y1) && (y < page.y2)) {
                        grown[t].push_back(std::make_pair(localIndex(x, y), f.second));
                    }
                }
            }
        });
        return grown;
    }
} // namespace

struct Coverage::Implementation {
    const PaintMeshIndex* index = nullptr;
    int size = 0;
    // The triangles over each page, (tile, (py, px)), by mesh then triangle.
    std::map<std::pair<int, std::pair<int, int>>, std::vector<TriangleRef>> bins;
    std::vector<int> tiles;
};

Coverage::Coverage(const PaintMeshIndex& index,
                   int tileSize)
    : _imp(new Implementation)
{
    _imp->index = &index;
    _imp->size = std::max(0, tileSize);
    std::set<int> tiles;
    const std::vector<SceneRaster::TriangulatedMesh>& targets = index.targets();
    for (int m = 0; m < (int)targets.size(); ++m) {
        for (int t = 0; t < (int)targets[m].triangles.size(); ++t) {
            if (((std::size_t)t >= targets[m].hasSt.size()) || !targets[m].hasSt[t] || ((std::size_t)(3 * t + 2) >= targets[m].st.size())) {
                continue;
            }
            UvBox box;
            for (int k = 0; k < 3; ++k) {
                box.add(targets[m].st[3 * t + k][0], targets[m].st[3 * t + k][1]);
            }
            TriangleRef ref;
            ref.mesh = m;
            ref.triangle = t;
            forEachTile(box, _imp->size, [&](int tile, int x1, int y1, int x2, int y2) {
                tiles.insert(tile);
                for (int py = y1 / kPage; py <= (y2 - 1) / kPage; ++py) {
                    for (int px = x1 / kPage; px <= (x2 - 1) / kPage; ++px) {
                        _imp->bins[std::make_pair(tile, std::make_pair(py, px))].push_back(ref);
                    }
                }
            });
        }
    }
    _imp->tiles.assign(tiles.begin(), tiles.end());
}

Coverage::~Coverage()
{
}

int
Coverage::tileSize() const
{
    return _imp->size;
}

void
Coverage::owners(int tile,
                 const RectI& rect,
                 int* owner) const
{
    const int width = rect.width();
    std::fill_n(owner, (std::size_t)width * (std::size_t)rect.height(), -1);
    const RectI clipped = rect.intersect(RectI(0, 0, _imp->size, _imp->size));
    if (clipped.isNull()) {
        return;
    }
    const std::vector<SceneRaster::TriangulatedMesh>& targets = _imp->index->targets();
    for (int py = clipped.y1 / kPage; py <= (clipped.y2 - 1) / kPage; ++py) {
        for (int px = clipped.x1 / kPage; px <= (clipped.x2 - 1) / kPage; ++px) {
            const std::map<std::pair<int, std::pair<int, int>>, std::vector<TriangleRef>>::const_iterator bin = _imp->bins.find(std::make_pair(tile, std::make_pair(py, px)));
            if (bin == _imp->bins.end()) {
                continue;
            }
            const RectI area = clipped.intersect(RectI(px * kPage, py * kPage, (px + 1) * kPage, (py + 1) * kPage));
            for (const TriangleRef& ref : bin->second) {
                SceneRaster::rasterizeTexels(&targets[ref.mesh].st[3 * ref.triangle], tile, _imp->size, area.x1, area.y1, area.x2, area.y2,
                                             [&](int x, int y, const float*) {
                                                 owner[(std::size_t)(x - rect.x1) + (std::size_t)(y - rect.y1) * width] = ref.mesh;
                                             });
            }
        }
    }
}

std::vector<int>
Coverage::tiles() const
{
    return _imp->tiles;
}

TexelPages::BaseFill
meshColourBase(const std::shared_ptr<const Coverage>& coverage,
               const std::vector<GfVec3f>& colours)
{
    return [coverage, colours](int tile, int, const RectI& texels, float* rgba, std::string*) {
        const std::size_t count = (std::size_t)texels.width() * (std::size_t)texels.height();
        std::vector<int> owner(count, -1);
        coverage->owners(tile, texels, owner.data());
        for (std::size_t i = 0; i < count; ++i) {
            const int m = owner[i];
            float* texel = rgba + i * 4;
            if ((m >= 0) && ((std::size_t)m < colours.size())) {
                texel[0] = colours[m][0];
                texel[1] = colours[m][1];
                texel[2] = colours[m][2];
                texel[3] = 1.f;
            } else {
                std::fill_n(texel, 4, 0.f);
            }
        }

        return true;
    };
}

namespace {
    bool
    sameDab(const Dab& a,
            const Dab& b)
    {
        return (a.center.x == b.center.x) && (a.center.y == b.center.y) && (a.internalRadius == b.internalRadius) && (a.externalRadius == b.externalRadius) && (a.pressure == b.pressure) && (a.opacityStops == b.opacityStops) && (a.buildUp == b.buildUp) && (a.opacity == b.opacity);
    }

    bool
    sameSmear(const SmearDab& a,
              const SmearDab& b)
    {
        return (a.from.x == b.from.x) && (a.from.y == b.from.y) && (a.to.x == b.to.x) && (a.to.y == b.to.y);
    }

    // Whether laid is where now starts.
    template <typename T, typename Same>
    bool
    startsWith(const std::vector<T>& now,
               const std::vector<T>& laid,
               Same same)
    {
        if (laid.size() > now.size()) {
            return false;
        }
        for (std::size_t i = 0; i < laid.size(); ++i) {
            if (!same(laid[i], now[i])) {
                return false;
            }
        }

        return true;
    }

    // Whether a and b paint alike but for their points.
    bool
    sameSettings(const Stroke& a,
                 const Stroke& b)
    {
        const RotoStrokeDabs::BrushParams& p = a.brush;
        const RotoStrokeDabs::BrushParams& q = b.brush;

        return (a.type == b.type) && (p.brushSizePixel == q.brushSizePixel) && (p.brushSpacing == q.brushSpacing) && (p.brushHardness == q.brushHardness) && (p.writeOnStart == q.writeOnStart) && (p.writeOnEnd == q.writeOnEnd) && (p.pressureAffectsOpacity == q.pressureAffectsOpacity) && (p.pressureAffectsSize == q.pressureAffectsSize) && (p.pressureAffectsHardness == q.pressureAffectsHardness) && (a.buildUp == b.buildUp) && (a.opacity == b.opacity) && std::equal(a.color, a.color + 3, b.color) && std::equal(a.channels, a.channels + 4, b.channels) && (a.effectStrength == b.effectStrength) && std::equal(a.sourceTransform, a.sourceTransform + 9, b.sourceTransform) && (a.cloneFromBase == b.cloneFromBase);
    }

    bool
    sameContext(const Context& a,
                const Context& b)
    {
        const SceneRaster::ExplicitCamera& p = a.camera;
        const SceneRaster::ExplicitCamera& q = b.camera;

        return (a.index == b.index) && (a.coverage == b.coverage) && (a.footprint == b.footprint) && (a.pages == b.pages) && (a.plane == b.plane) && (a.base == b.base) && (a.depthBias == b.depthBias) && std::equal(p.cameraToWorld, p.cameraToWorld + 16, q.cameraToWorld) && (p.viewHeight == q.viewHeight) && (p.aspect == q.aspect) && (p.orthographic == q.orthographic) && (p.nearClip == q.nearClip) && (p.farClip == q.farClip);
    }

    bool
    fail(std::string* error,
         const char* message)
    {
        *error = message;

        return false;
    }
} // namespace

struct LiveStroke::Implementation {
    explicit Implementation(bool journal)
        : keepsJournal(journal)
    {
    }

    // Puts back the pages the stroke wrote.
    bool putBack(std::string* error)
    {
        if (written.empty()) {
            return true;
        }
        if (!keepsJournal) {
            return fail(error, "A stroke laid without a journal cannot be laid again.");
        }
        std::vector<TexelPages::Snapshot> journal;
        for (const std::pair<const PageKey, Written>& entry : written) {
            journal.push_back(entry.second.journal);
        }

        return context.pages->restore(journal, error);
    }

    void clear()
    {
        started = false;
        broken = false;
        dabs.clear();
        smears.clear();
        works.clear();
        grown.clear();
        written.clear();
    }

    bool keepsJournal;
    // Whether context and settings hold what the stroke was laid with.
    bool started = false;
    // Whether a failure left the pages between two updates, so that the stroke must be laid again.
    bool broken = false;
    // Whether takeJournal() has been called, or a stroke with no journal was written.
    bool settled = false;
    int restarts = 0;
    Context context;
    Stroke settings;
    std::vector<Dab> dabs;
    std::vector<SmearDab> smears;
    // What the brush laid on each page it touched, the gutter aside.
    Works works;
    std::map<PageKey, Grown> grown;
    std::map<PageKey, Written> written;
};

LiveStroke::LiveStroke(bool keepsJournal)
    : _imp(new Implementation(keepsJournal))
{
}

LiveStroke::~LiveStroke()
{
}

bool
LiveStroke::paint(const Context& context,
                  const Stroke& stroke,
                  std::string* error)
{
    error->clear();
    Implementation& st = *_imp;
    if (st.settled) {
        return fail(error, "A settled stroke cannot be laid again.");
    }
    if (!context.index || !context.pages) {
        return fail(error, "Texel painting needs a mesh index and pages to paint.");
    }
    Setup s;
    s.context = &context;
    s.index = context.index;
    s.size = context.pages->tileSize();
    s.threads = threadCount(context.threads);
    if (!s.view.init(context.camera, error)) {
        return false;
    }
    if (context.coverage && (context.coverage->tileSize() != s.size)) {
        return fail(error, "The texel coverage is not at the painted tiles' resolution.");
    }

    const bool smear = stroke.type == eRotoStrokeTypeSmear;
    std::vector<Dab> dabs;
    std::vector<SmearDab> smears;
    if (smear) {
        smears = smearDabs(stroke.points, stroke.brush);
    } else {
        RotoStrokeDabs::walk(stroke.points, 0., stroke.brush, stroke.buildUp, stroke.opacity, [&dabs](const Dab& dab) {
            dabs.push_back(dab);
        });
    }
    bool restart = st.broken;
    if (st.started && !restart) {
        restart = !sameContext(st.context, context) || !sameSettings(st.settings, stroke) || !startsWith(dabs, st.dabs, sameDab) || !startsWith(smears, st.smears, sameSmear);
    }
    if (restart) {
        if (!st.putBack(error)) {
            st.broken = true;

            return false;
        }
        st.clear();
        ++st.restarts;
    }
    if (!st.started) {
        st.context = context;
        st.context.beforeWrite = nullptr;
        st.context.cancelled = nullptr;
        st.settings = stroke;
        st.settings.points.clear();
    }

    const PageSource before(context.pages, &st.written);
    Works changed;
    if (smear && (smears.size() > st.smears.size())) {
        if (!laySmear(s, stroke, before, st.works, std::vector<SmearDab>(smears.begin() + (std::ptrdiff_t)st.smears.size(), smears.end()), &changed, error)) {
            return false;
        }
    } else if (!smear && (dabs.size() > st.dabs.size())) {
        if (!layMasked(s, stroke, before, st.works, std::vector<Dab>(dabs.begin() + (std::ptrdiff_t)st.dabs.size(), dabs.end()), &changed, error)) {
            return false;
        }
    }
    std::set<PageKey> dirty;
    for (const std::pair<const PageKey, PageWork>& entry : changed) {
        dirty.insert(entry.first);
    }
    const std::function<const PageWork*(const PageKey&)> laidOn = [&changed, &st](const PageKey& key) -> const PageWork* {
        Works::const_iterator it = changed.find(key);
        if (it != changed.end()) {
            return &it->second;
        }
        it = st.works.find(key);

        return (it != st.works.end()) ? &it->second : nullptr;
    };
    std::map<PageKey, Grown> grown;
    std::set<PageKey> keys = dirty;
    if (context.coverage && !dirty.empty()) {
        const std::vector<PageKey> targets = gutterTargets(*context.pages, dirty);
        std::vector<Grown> found = gutterOf(s, laidOn, targets);
        for (std::size_t t = 0; t < targets.size(); ++t) {
            grown[targets[t]] = std::move(found[t]);
            keys.insert(targets[t]);
        }
    }
    if (s.cancelled()) {
        return false;
    }

    // Every page the update may change, as the stroke now leaves it; one laid nowhere is put back.
    struct Out {
        PageKey key;
        bool laid = false;
        PageData data;
    };
    std::vector<Out> outs;
    for (const PageKey& key : keys) {
        const PageWork* work = laidOn(key);
        std::map<PageKey, Grown>::const_iterator fresh = grown.find(key);
        const Grown* g = nullptr;
        if (fresh != grown.end()) {
            g = &fresh->second;
        } else {
            const std::map<PageKey, Grown>::const_iterator kept = st.grown.find(key);
            g = (kept != st.grown.end()) ? &kept->second : nullptr;
        }
        const bool brushed = work && (std::find(work->laid.begin(), work->laid.end(), 1) != work->laid.end());
        const bool gutter = g && !g->empty();
        Out out;
        out.key = key;
        if (!brushed && !gutter) {
            if (st.written.count(key)) {
                outs.push_back(std::move(out));
            }
            continue;
        }
        out.laid = true;
        if (work) {
            out.data = work->data;
        } else if (!before.read(key, &out.data, error)) {
            return false;
        }
        if (gutter) {
            for (const std::pair<int, std::array<float, 5>>& texel : *g) {
                std::copy_n(texel.second.data(), 4, out.data.rgba.data() + (std::size_t)texel.first * 4);
                out.data.touched[texel.first] = texel.second[4];
            }
        }
        outs.push_back(std::move(out));
    }

    // The pages written for the first time: journalled, and kept as they were for what reads under the stroke.
    std::vector<PageKey> first;
    for (const Out& out : outs) {
        if (out.laid && !st.written.count(out.key)) {
            first.push_back(out.key);
        }
    }
    std::map<PageKey, Written> added;
    if (st.keepsJournal && !first.empty()) {
        std::vector<TexelPages::Snapshot> snapshots;
        if (!context.pages->snapshot(first, &snapshots, error)) {
            return false;
        }
        for (std::size_t i = 0; i < first.size(); ++i) {
            Written& entry = added[first[i]];
            entry.journal = std::move(snapshots[i]);
            if (!entry.journal.page) {
                PageData pre;
                if (!before.read(first[i], &pre, error)) {
                    return false;
                }
                TexelPages::pack(pre, &entry.base);
            }
        }
    }
    if (context.beforeWrite && !first.empty() && !context.beforeWrite(first, error)) {
        return false;
    }

    // From here the pages change, and a failure leaves the stroke to be laid again from its start.
    st.broken = true;
    for (std::pair<const PageKey, PageWork>& entry : changed) {
        st.works[entry.first] = std::move(entry.second);
    }
    for (std::pair<const PageKey, Grown>& entry : grown) {
        if (entry.second.empty()) {
            st.grown.erase(entry.first);
        } else {
            st.grown[entry.first] = std::move(entry.second);
        }
    }
    for (std::pair<const PageKey, Written>& entry : added) {
        st.written[entry.first] = std::move(entry.second);
    }
    std::vector<TexelPages::Snapshot> putBack;
    for (const Out& out : outs) {
        if (out.laid) {
            if (!context.pages->write(out.key, out.data, error)) {
                return false;
            }
            continue;
        }
        const std::map<PageKey, Written>::iterator it = st.written.find(out.key);
        putBack.push_back(std::move(it->second.journal));
        st.written.erase(it);
    }
    if (!putBack.empty() && !context.pages->restore(putBack, error)) {
        return false;
    }
    st.dabs.swap(dabs);
    st.smears.swap(smears);
    st.started = true;
    st.broken = false;
    if (!st.keepsJournal && !outs.empty()) {
        st.settled = true;
    }

    return true;
} // LiveStroke::paint

std::vector<TexelPages::Snapshot>
LiveStroke::takeJournal()
{
    Implementation& st = *_imp;
    std::vector<TexelPages::Snapshot> journal;

    for (std::pair<const PageKey, Written>& entry : st.written) {
        journal.push_back(std::move(entry.second.journal));
    }
    st.clear();
    st.settled = true;

    return journal;
}

int
LiveStroke::restarts() const
{
    return _imp->restarts;
}

std::size_t
LiveStroke::dabCount() const
{
    return _imp->dabs.size() + _imp->smears.size();
}

bool
paint(const Context& context,
      const Stroke& stroke,
      std::string* error)
{
    LiveStroke live(false);

    return live.paint(context, stroke, error);
}
} // namespace TexelSplat

NATRON_NAMESPACE_EXIT
