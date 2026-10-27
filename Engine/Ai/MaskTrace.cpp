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

#include "MaskTrace.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

NATRON_NAMESPACE_ENTER

namespace MaskTrace {

namespace {

    const double kPi = 3.14159265358979323846;

    inline Point
    operator+(const Point& a,
              const Point& b)
    {
        Point r;
        r.x = a.x + b.x;
        r.y = a.y + b.y;

        return r;
    }

    inline Point
    operator-(const Point& a,
              const Point& b)
    {
        Point r;
        r.x = a.x - b.x;
        r.y = a.y - b.y;

        return r;
    }

    inline Point
    operator*(const Point& a,
              double s)
    {
        Point r;
        r.x = a.x * s;
        r.y = a.y * s;

        return r;
    }

    inline double
    dot(const Point& a,
        const Point& b)
    {
        return a.x * b.x + a.y * b.y;
    }

    inline double
    length(const Point& a)
    {
        return std::sqrt(dot(a, a));
    }

    inline Point
    normalized(const Point& a)
    {
        const double l = length(a);

        return (l > 1e-12) ? a * (1. / l) : a;
    }

    inline double
    distanceToSegment(const Point& p,
                      const Point& a,
                      const Point& b)
    {
        const Point ab = b - a;
        const double l2 = dot(ab, ab);
        if (l2 <= 1e-24) {
            return length(p - a);
        }
        const double t = std::min(1., std::max(0., dot(p - a, ab) / l2));

        return length(p - (a + ab * t));
    }

    struct EdgeNode {
        Point position;
        int64_t next[2] = { -1, -1 };
        int count = 0;
        bool visited = false;
    };

    void
    link(std::unordered_map<int64_t, EdgeNode>& nodes,
         int64_t a,
         int64_t b)
    {
        EdgeNode& na = nodes[a];
        EdgeNode& nb = nodes[b];

        if (na.count < 2) {
            na.next[na.count++] = b;
        }
        if (nb.count < 2) {
            nb.next[nb.count++] = a;
        }
    }

    // Kept vertex indices of a closed polygon after Douglas-Peucker, ascending.
    std::vector<int>
    simplifyIndices(const std::vector<Point>& pts,
                    double tolerance)
    {
        const int n = (int)pts.size();
        if (n <= 3) {
            std::vector<int> all(n);
            for (int i = 0; i < n; ++i) {
                all[i] = i;
            }

            return all;
        }
        // A closed polygon has no endpoints: split it at vertex 0 and the vertex farthest from it.
        int far = 0;
        double farDist = -1.;
        for (int i = 1; i < n; ++i) {
            const double d = length(pts[i] - pts[0]);
            if (d > farDist) {
                farDist = d;
                far = i;
            }
        }
        std::vector<char> keep(n, 0);
        keep[0] = 1;
        keep[far] = 1;
        std::vector<std::pair<int, int>> stack;
        // Index n stands for vertex 0 closing the loop.
        stack.push_back(std::make_pair(0, far));
        stack.push_back(std::make_pair(far, n));
        while (!stack.empty()) {
            const std::pair<int, int> run = stack.back();
            stack.pop_back();
            const Point& a = pts[run.first % n];
            const Point& b = pts[run.second % n];
            int worst = -1;
            double worstDist = tolerance;
            for (int i = run.first + 1; i < run.second; ++i) {
                const double d = distanceToSegment(pts[i], a, b);
                if (d > worstDist) {
                    worstDist = d;
                    worst = i;
                }
            }
            if (worst >= 0) {
                keep[worst] = 1;
                stack.push_back(std::make_pair(run.first, worst));
                stack.push_back(std::make_pair(worst, run.second));
            }
        }
        std::vector<int> kept;
        for (int i = 0; i < n; ++i) {
            if (keep[i]) {
                kept.push_back(i);
            }
        }
        // Too coarse a tolerance can leave a segment; three spread vertices still bound an area.
        if (kept.size() < 3) {
            kept.clear();
            kept.push_back(0);
            kept.push_back(n / 3);
            kept.push_back((2 * n) / 3);
        }

        return kept;
    } // simplifyIndices

    // ---- Schneider, "An Algorithm for Automatically Fitting Digitized Curves", Graphics Gems I ----

    struct Cubic {
        Point p[4];
    };

    Point
    bezierPoint(const Point* v,
                int degree,
                double t)
    {
        Point tmp[4];
        for (int i = 0; i <= degree; ++i) {
            tmp[i] = v[i];
        }
        for (int i = 1; i <= degree; ++i) {
            for (int j = 0; j <= degree - i; ++j) {
                tmp[j] = tmp[j] * (1. - t) + tmp[j + 1] * t;
            }
        }

        return tmp[0];
    }

    Cubic
    generateBezier(const std::vector<Point>& d,
                   int first,
                   int last,
                   const std::vector<double>& u,
                   const Point& tHat1,
                   const Point& tHat2)
    {
        const int nPts = last - first + 1;
        double c[2][2] = { { 0., 0. }, { 0., 0. } };
        double x[2] = { 0., 0. };

        for (int i = 0; i < nPts; ++i) {
            const double t = u[i];
            const double mt = 1. - t;
            const double b0 = mt * mt * mt, b1 = 3. * t * mt * mt, b2 = 3. * t * t * mt, b3 = t * t * t;
            const Point a0 = tHat1 * b1;
            const Point a1 = tHat2 * b2;
            c[0][0] += dot(a0, a0);
            c[0][1] += dot(a0, a1);
            c[1][1] += dot(a1, a1);
            const Point tmp = d[first + i] - (d[first] * (b0 + b1) + d[last] * (b2 + b3));
            x[0] += dot(a0, tmp);
            x[1] += dot(a1, tmp);
        }
        c[1][0] = c[0][1];

        const double detC0C1 = c[0][0] * c[1][1] - c[1][0] * c[0][1];
        const double detC0X = c[0][0] * x[1] - c[1][0] * x[0];
        const double detXC1 = x[0] * c[1][1] - x[1] * c[0][1];
        double alphaL = (std::fabs(detC0C1) > 1e-12) ? detXC1 / detC0C1 : 0.;
        double alphaR = (std::fabs(detC0C1) > 1e-12) ? detC0X / detC0C1 : 0.;

        Cubic bez;
        bez.p[0] = d[first];
        bez.p[3] = d[last];
        const double segLength = length(d[last] - d[first]);
        const double epsilon = 1e-6 * segLength;
        if ((alphaL < epsilon) || (alphaR < epsilon)) {
            // Wildly wrong least squares: fall back on Wu and Barsky's heuristic.
            alphaL = alphaR = segLength / 3.;
        }
        bez.p[1] = bez.p[0] + tHat1 * alphaL;
        bez.p[2] = bez.p[3] + tHat2 * alphaR;

        return bez;
    } // generateBezier

    double
    newtonRaphsonRoot(const Cubic& q,
                      const Point& p,
                      double u)
    {
        Point q1[3], q2[2];
        for (int i = 0; i < 3; ++i) {
            q1[i] = (q.p[i + 1] - q.p[i]) * 3.;
        }
        for (int i = 0; i < 2; ++i) {
            q2[i] = (q1[i + 1] - q1[i]) * 2.;
        }
        const Point qu = bezierPoint(q.p, 3, u);
        const Point q1u = bezierPoint(q1, 2, u);
        const Point q2u = bezierPoint(q2, 1, u);
        const double numerator = dot(qu - p, q1u);
        const double denominator = dot(q1u, q1u) + dot(qu - p, q2u);
        if (std::fabs(denominator) < 1e-12) {
            return u;
        }

        return std::min(1., std::max(0., u - numerator / denominator));
    }

    std::vector<double>
    chordLengthParameterize(const std::vector<Point>& d,
                            int first,
                            int last)
    {
        std::vector<double> u(last - first + 1, 0.);
        for (int i = first + 1; i <= last; ++i) {
            u[i - first] = u[i - first - 1] + length(d[i] - d[i - 1]);
        }
        const double total = u.back();
        for (double& v : u) {
            v = (total > 0.) ? v / total : 0.;
        }

        return u;
    }

    double
    computeMaxError(const std::vector<Point>& d,
                    int first,
                    int last,
                    const Cubic& bez,
                    const std::vector<double>& u,
                    int* splitPoint)
    {
        *splitPoint = (last - first + 1) / 2 + first;
        double maxDist = 0.;
        for (int i = first + 1; i < last; ++i) {
            const Point v = bezierPoint(bez.p, 3, u[i - first]) - d[i];
            const double dist = dot(v, v);
            if (dist >= maxDist) {
                maxDist = dist;
                *splitPoint = i;
            }
        }

        return maxDist;
    }

    void
    fitCubic(const std::vector<Point>& d,
             int first,
             int last,
             const Point& tHat1,
             const Point& tHat2,
             double error,
             std::vector<Cubic>* out)
    {
        const int nPts = last - first + 1;

        if (nPts == 2) {
            const double dist = length(d[last] - d[first]) / 3.;
            Cubic bez;
            bez.p[0] = d[first];
            bez.p[3] = d[last];
            bez.p[1] = bez.p[0] + tHat1 * dist;
            bez.p[2] = bez.p[3] + tHat2 * dist;
            out->push_back(bez);

            return;
        }

        std::vector<double> u = chordLengthParameterize(d, first, last);
        Cubic bez = generateBezier(d, first, last, u, tHat1, tHat2);
        int splitPoint = 0;
        double maxError = computeMaxError(d, first, last, bez, u, &splitPoint);
        const double errorSq = error * error;
        if (maxError < errorSq) {
            out->push_back(bez);

            return;
        }
        if (maxError < errorSq * 4.) {
            for (int iteration = 0; iteration < 4; ++iteration) {
                for (int i = 0; i < nPts; ++i) {
                    u[i] = newtonRaphsonRoot(bez, d[first + i], u[i]);
                }
                bez = generateBezier(d, first, last, u, tHat1, tHat2);
                maxError = computeMaxError(d, first, last, bez, u, &splitPoint);
                if (maxError < errorSq) {
                    out->push_back(bez);

                    return;
                }
            }
        }

        splitPoint = std::min(std::max(splitPoint, first + 1), last - 1);
        const Point tHatCenter = normalized(d[splitPoint - 1] - d[splitPoint + 1]);
        fitCubic(d, first, splitPoint, tHat1, tHatCenter, error, out);
        fitCubic(d, splitPoint, last, tHatCenter * -1., tHat2, error, out);
    } // fitCubic

} // namespace

std::vector<Polygon>
isoContours(const std::vector<float>& mask,
            int width,
            int height,
            double threshold)
{
    std::vector<Polygon> result;

    if ((width <= 0) || (height <= 0) || (mask.size() < (size_t)width * height)) {
        return result;
    }
    // Samples sit at pixel centres, on a grid padded by one sample of "outside" all round.
    const int W = width + 2;
    const int H = height + 2;
    const double outside = threshold - 1.;
    auto value = [&](int I, int J) -> double {
        if ((I < 1) || (J < 1) || (I > width) || (J > height)) {
            return outside;
        }

        return mask[(size_t)(J - 1) * width + (I - 1)];
    };
    auto inside = [&](int I, int J) {
        return value(I, J) > threshold;
    };
    std::unordered_map<int64_t, EdgeNode> nodes;
    // The crossing on the edge from sample (I, J) to its right (vertical = 0) or below (vertical = 1).
    auto edge = [&](int I, int J, int vertical) -> int64_t {
        const int64_t id = ((int64_t)J * W + I) * 2 + vertical;
        EdgeNode& node = nodes[id];
        if (node.count == 0) {
            const int I2 = vertical ? I : I + 1;
            const int J2 = vertical ? J + 1 : J;
            const double a = value(I, J);
            const double b = value(I2, J2);
            const double t = (std::fabs(b - a) > 1e-12) ? std::min(1., std::max(0., (threshold - a) / (b - a))) : 0.5;
            node.position.x = (I + t * (I2 - I)) - 0.5;
            node.position.y = (J + t * (J2 - J)) - 0.5;
        }

        return id;
    };

    for (int J = 0; J < H - 1; ++J) {
        for (int I = 0; I < W - 1; ++I) {
            const bool c0 = inside(I, J);
            const bool c1 = inside(I + 1, J);
            const bool c2 = inside(I + 1, J + 1);
            const bool c3 = inside(I, J + 1);
            if ((c0 == c1) && (c1 == c2) && (c2 == c3)) {
                continue;
            }
            std::vector<int64_t> crossings;
            const bool topCross = c0 != c1;
            const bool rightCross = c1 != c2;
            const bool bottomCross = c3 != c2;
            const bool leftCross = c0 != c3;
            if (topCross && rightCross && bottomCross && leftCross) {
                // Saddle: the centre's value decides which diagonal pair is joined.
                const double centre = 0.25 * (value(I, J) + value(I + 1, J) + value(I + 1, J + 1) + value(I, J + 1));
                const int64_t top = edge(I, J, 0), right = edge(I + 1, J, 1), bottom = edge(I, J + 1, 0), left = edge(I, J, 1);
                if ((centre > threshold) == c0) {
                    link(nodes, top, right);
                    link(nodes, bottom, left);
                } else {
                    link(nodes, top, left);
                    link(nodes, right, bottom);
                }
                continue;
            }
            if (topCross) {
                crossings.push_back(edge(I, J, 0));
            }
            if (rightCross) {
                crossings.push_back(edge(I + 1, J, 1));
            }
            if (bottomCross) {
                crossings.push_back(edge(I, J + 1, 0));
            }
            if (leftCross) {
                crossings.push_back(edge(I, J, 1));
            }
            if (crossings.size() == 2) {
                link(nodes, crossings[0], crossings[1]);
            }
        }
    }

    // Walk in edge-id order so the output does not depend on hashing.
    std::vector<int64_t> ids;
    ids.reserve(nodes.size());
    for (const std::pair<const int64_t, EdgeNode>& entry : nodes) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    for (int64_t start : ids) {
        EdgeNode& startNode = nodes[start];
        if (startNode.visited || (startNode.count < 2)) {
            continue;
        }
        Polygon polygon;
        int64_t prev = -1;
        int64_t cur = start;
        for (;;) {
            EdgeNode& node = nodes[cur];
            node.visited = true;
            polygon.points.push_back(node.position);
            const int64_t next = (node.next[0] != prev) ? node.next[0] : node.next[1];
            if ((next < 0) || (next == start) || nodes[next].visited) {
                break;
            }
            prev = cur;
            cur = next;
        }
        if (polygon.points.size() >= 3) {
            result.push_back(std::move(polygon));
        }
    }

    return result;
} // isoContours

double
signedArea(const Polygon& polygon)
{
    const std::vector<Point>& p = polygon.points;
    const std::size_t n = p.size();
    double area = 0.;

    for (std::size_t i = 0; i < n; ++i) {
        const Point& a = p[i];
        const Point& b = p[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }

    return 0.5 * area;
}

bool
contains(const Polygon& polygon,
         const Point& q)
{
    const std::vector<Point>& p = polygon.points;
    const std::size_t n = p.size();
    bool in = false;

    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        if (((p[i].y > q.y) != (p[j].y > q.y)) && (q.x < (p[j].x - p[i].x) * (q.y - p[i].y) / (p[j].y - p[i].y) + p[i].x)) {
            in = !in;
        }
    }

    return in;
}

Polygon
simplify(const Polygon& polygon,
         double tolerance)
{
    Polygon result;

    for (int i : simplifyIndices(polygon.points, tolerance)) {
        result.points.push_back(polygon.points[i]);
    }

    return result;
}

std::vector<CurvePoint>
straightCurves(const Polygon& polygon)
{
    std::vector<CurvePoint> result;

    for (const Point& p : polygon.points) {
        CurvePoint cp;
        cp.position = p;
        cp.left = p;
        cp.right = p;
        result.push_back(cp);
    }

    return result;
}

std::vector<CurvePoint>
fitCurves(const Polygon& polygon,
          double maxError,
          double cornerAngle)
{
    const std::vector<Point>& pts = polygon.points;
    const int n = (int)pts.size();

    if (n < 4) {
        return straightCurves(polygon);
    }
    maxError = std::max(maxError, 0.01);

    // Corners are judged on the simplified outline, where pixel steps no longer read as turns.
    const std::vector<int> kept = simplifyIndices(pts, maxError);
    const double cosLimit = std::cos(cornerAngle * kPi / 180.);
    std::vector<std::pair<int, bool>> splits;
    for (std::size_t k = 0; k < kept.size(); ++k) {
        const Point& prev = pts[kept[(k + kept.size() - 1) % kept.size()]];
        const Point& cur = pts[kept[k]];
        const Point& next = pts[kept[(k + 1) % kept.size()]];
        const double cosTurn = dot(normalized(cur - prev), normalized(next - cur));
        if (cosTurn < cosLimit) {
            splits.push_back(std::make_pair(kept[k], true));
        }
    }
    // A closed run needs at least three points to stay a curve with area; add smooth joins.
    if (splits.size() < 3) {
        const int base = splits.empty() ? 0 : splits[0].first;
        for (int k = 0; k < 3; ++k) {
            const int index = (base + (k * n) / 3) % n;
            bool nearExisting = false;
            for (const std::pair<int, bool>& s : splits) {
                const int gap = std::abs(s.first - index);
                if (std::min(gap, n - gap) < std::max(2, n / 8)) {
                    nearExisting = true;
                    break;
                }
            }
            if (!nearExisting) {
                splits.push_back(std::make_pair(index, false));
            }
        }
        std::sort(splits.begin(), splits.end());
    }

    const int reach = std::max(1, std::min(3, n / 8));
    auto at = [&](int i) -> const Point& {
        return pts[((i % n) + n) % n];
    };
    auto smoothTangent = [&](int i) {
        return normalized(at(i + reach) - at(i - reach));
    };

    std::vector<Cubic> cubics;
    for (std::size_t k = 0; k < splits.size(); ++k) {
        const std::pair<int, bool>& a = splits[k];
        const std::pair<int, bool>& b = splits[(k + 1) % splits.size()];
        int end = b.first;
        if (end <= a.first) {
            end += n;
        }
        std::vector<Point> run;
        for (int i = a.first; i <= end; ++i) {
            run.push_back(at(i));
        }
        const int last = (int)run.size() - 1;
        const int r = std::max(1, std::min(reach, last));
        const Point tHat1 = a.second ? normalized(run[r] - run[0]) : smoothTangent(a.first);
        const Point tHat2 = b.second ? normalized(run[last - r] - run[last]) : smoothTangent(b.first) * -1.;
        fitCubic(run, 0, last, tHat1, tHat2, maxError, &cubics);
    }

    std::vector<CurvePoint> result;
    const std::size_t m = cubics.size();
    for (std::size_t k = 0; k < m; ++k) {
        CurvePoint cp;
        cp.position = cubics[k].p[0];
        cp.right = cubics[k].p[1];
        cp.left = cubics[(k + m - 1) % m].p[2];
        result.push_back(cp);
    }

    return result;
} // fitCurves

std::vector<Contour>
trace(const std::vector<float>& mask,
      int width,
      int height,
      const Options& options)
{
    std::vector<Polygon> polygons = isoContours(mask, width, height, options.threshold);
    const std::size_t n = polygons.size();

    struct Box {
        double x0, y0, x1, y1;
    };
    std::vector<Box> boxes(n);
    std::vector<double> areas(n);
    for (std::size_t i = 0; i < n; ++i) {
        Box b = { 1e300, 1e300, -1e300, -1e300 };
        for (const Point& p : polygons[i].points) {
            b.x0 = std::min(b.x0, p.x);
            b.y0 = std::min(b.y0, p.y);
            b.x1 = std::max(b.x1, p.x);
            b.y1 = std::max(b.y1, p.y);
        }
        boxes[i] = b;
        areas[i] = signedArea(polygons[i]);
    }

    // Contours never cross, so one vertex tells whether a contour lies inside another. Small
    // contours still count, so that depth parity stays right when they are dropped below.
    std::vector<int> depth(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const Point& probe = polygons[i].points[0];
        for (std::size_t j = 0; j < n; ++j) {
            if ((i == j) || (std::fabs(areas[j]) <= std::fabs(areas[i]))) {
                continue;
            }
            const Box& b = boxes[j];
            if ((probe.x < b.x0) || (probe.x > b.x1) || (probe.y < b.y0) || (probe.y > b.y1)) {
                continue;
            }
            if (contains(polygons[j], probe)) {
                ++depth[i];
            }
        }
    }

    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::fabs(areas[i]) < options.minArea) {
            continue;
        }
        if (!options.keepHoles && (depth[i] > 0)) {
            continue;
        }
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (depth[a] != depth[b]) {
            return depth[a] < depth[b];
        }

        return std::fabs(areas[a]) > std::fabs(areas[b]);
    });

    std::vector<Contour> result;
    for (std::size_t i : order) {
        Polygon& polygon = polygons[i];
        const bool hole = (depth[i] % 2) == 1;
        if ((areas[i] > 0.) == hole) {
            std::reverse(polygon.points.begin(), polygon.points.end());
        }
        Contour contour;
        contour.isHole = hole;
        contour.depth = depth[i];
        contour.area = std::fabs(areas[i]);
        contour.points = options.smooth ? fitCurves(polygon, options.simplify, options.cornerAngle)
                                        : straightCurves(simplify(polygon, options.simplify));
        if (contour.points.size() >= 3) {
            result.push_back(std::move(contour));
        }
    }

    return result;
} // trace

} // namespace MaskTrace

NATRON_NAMESPACE_EXIT
