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

#include "GeoScatter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"

NATRON_NAMESPACE_ENTER

namespace {
typedef std::array<double, 3> Vec3;

Vec3
sub(const Vec3& a,
    const Vec3& b)
{
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

Vec3
cross(const Vec3& a,
      const Vec3& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

double
dot(const Vec3& a,
    const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double
length(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

// The (u, v) of q on the quad a b c d parametrised bilinearly, a at (0, 0), b at (1, 0), c at
// (1, 1) and d at (0, 1), solved in the plane of the quad's Newell normal so a warped quad still
// gives an answer in [0, 1]^2.
std::array<double, 2>
inverseBilinear(const Vec3& a,
                const Vec3& b,
                const Vec3& c,
                const Vec3& d,
                const Vec3& q)
{
    const Vec3 corners[4] = { a, b, c, d };
    Vec3 n = { 0., 0., 0. };
    for (int i = 0; i < 4; ++i) {
        const Vec3& p0 = corners[i];
        const Vec3& p1 = corners[(i + 1) % 4];
        n[0] += (p0[1] - p1[1]) * (p0[2] + p1[2]);
        n[1] += (p0[2] - p1[2]) * (p0[0] + p1[0]);
        n[2] += (p0[0] - p1[0]) * (p0[1] + p1[1]);
    }
    const Vec3 helperAxis = (std::abs(n[0]) < std::abs(n[1]) && std::abs(n[0]) < std::abs(n[2])) ? Vec3 { 1., 0., 0. }
        : (std::abs(n[1]) < std::abs(n[2]))                                                      ? Vec3 { 0., 1., 0. }
                                                                                                 : Vec3 { 0., 0., 1. };
    const Vec3 s = cross(n, helperAxis);
    const Vec3 t = cross(n, s);
    auto project = [&](const Vec3& p) {
        const Vec3 r = sub(p, a);

        return std::array<double, 2> { dot(r, s), dot(r, t) };
    };
    auto cross2 = [](const std::array<double, 2>& x,
                     const std::array<double, 2>& y) {
        return x[0] * y[1] - x[1] * y[0];
    };
    const std::array<double, 2> pb = project(b);
    const std::array<double, 2> pc = project(c);
    const std::array<double, 2> pd = project(d);
    const std::array<double, 2> h = project(q);
    const std::array<double, 2> e = pb;
    const std::array<double, 2> f = pd;
    const std::array<double, 2> g = { pc[0] - pb[0] - pd[0], pc[1] - pb[1] - pd[1] };
    const double k2 = cross2(g, f);
    const double k1 = cross2(e, f) + cross2(h, g);
    const double k0 = cross2(h, e);
    auto uFor = [&](double v) {
        const double dx = e[0] + g[0] * v;
        const double dy = e[1] + g[1] * v;

        return std::abs(dx) > std::abs(dy) ? (h[0] - f[0] * v) / dx : (h[1] - f[1] * v) / dy;
    };
    auto clamped = [](double u,
                      double v) {
        return std::array<double, 2> { std::isfinite(u) ? std::clamp(u, 0., 1.) : 0., std::isfinite(v) ? std::clamp(v, 0., 1.) : 0. };
    };
    const double scale = std::max(std::abs(k1), std::max(std::abs(k0), 1e-300));
    if (std::abs(k2) <= 1e-12 * scale) {
        const double v = -k0 / k1;

        return clamped(uFor(v), v);
    }
    const double discriminant = std::sqrt(std::max(0., k1 * k1 - 4. * k0 * k2));
    const double v0 = (-k1 - discriminant) / (2. * k2);
    const double v1 = (-k1 + discriminant) / (2. * k2);
    const double u0 = uFor(v0);
    const double u1 = uFor(v1);
    auto outside = [](double u,
                      double v) {
        const double tolerance = 1e-9;

        return !std::isfinite(u) || !std::isfinite(v) || std::max(std::max(-u, u - 1.), std::max(-v, v - 1.)) > tolerance;
    };

    return (outside(u0, v0) && !outside(u1, v1)) ? clamped(u1, v1) : clamped(u0, v0);
}

struct Triangle {
    GeoOffset primitive;
    GeoIndex primitiveIndex;
    int fanIndex;
    std::array<GeoOffset, 3> vertices;
    std::array<GeoOffset, 3> points;
    std::array<Vec3, 3> p;
};

struct Sample {
    std::size_t triangle;
    // Barycentric weights of the triangle's three corners.
    std::array<double, 3> w;
};

Vec3
samplePosition(const Triangle& t,
               const std::array<double, 3>& w)
{
    Vec3 out;
    for (int c = 0; c < 3; ++c) {
        out[c] = w[0] * t.p[0][c] + w[1] * t.p[1][c] + w[2] * t.p[2][c];
    }

    return out;
}

// The closest point of the triangle to q, as barycentric weights (Ericson, Real-Time Collision Detection 5.1.5).
std::array<double, 3>
closestBarycentric(const Triangle& t,
                   const Vec3& q)
{
    const Vec3 ab = sub(t.p[1], t.p[0]);
    const Vec3 ac = sub(t.p[2], t.p[0]);
    const Vec3 ap = sub(q, t.p[0]);
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    if ((d1 <= 0.) && (d2 <= 0.)) {
        return { 1., 0., 0. };
    }
    const Vec3 bp = sub(q, t.p[1]);
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if ((d3 >= 0.) && (d4 <= d3)) {
        return { 0., 1., 0. };
    }
    const double vc = d1 * d4 - d3 * d2;
    if ((vc <= 0.) && (d1 >= 0.) && (d3 <= 0.)) {
        const double v = d1 / (d1 - d3);

        return { 1. - v, v, 0. };
    }
    const Vec3 cp = sub(q, t.p[2]);
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if ((d6 >= 0.) && (d5 <= d6)) {
        return { 0., 0., 1. };
    }
    const double vb = d5 * d2 - d1 * d6;
    if ((vb <= 0.) && (d2 >= 0.) && (d6 <= 0.)) {
        const double w = d2 / (d2 - d6);

        return { 1. - w, 0., w };
    }
    const double va = d3 * d6 - d5 * d4;
    if ((va <= 0.) && ((d4 - d3) >= 0.) && ((d5 - d6) >= 0.)) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));

        return { 0., 1. - w, w };
    }
    const double denom = 1. / (va + vb + vc);
    const double v = vb * denom;
    const double w = vc * denom;

    return { 1. - v - w, v, w };
}

// mt19937_64's sequence is fixed by the standard, unlike the distributions, so the unit
// interval is derived by hand to keep points identical across standard libraries.
double
unitRandom(std::mt19937_64& rng)
{
    return (double)(rng() >> 11) * (1. / 9007199254740992.);
}

void
relax(const std::vector<Triangle>& triangles,
      double totalArea,
      int iterations,
      std::vector<Sample>& samples)
{
    if (samples.size() < 2) {
        return;
    }
    // Twice the mean spacing of points spread evenly over the area.
    const double radius = 2. * std::sqrt(totalArea / (double)samples.size());
    if (!(radius > 0.)) {
        return;
    }
    const double inverseCell = 1. / radius;
    auto cellOf = [inverseCell](const Vec3& p) {
        return std::array<std::int64_t, 3> { (std::int64_t)std::floor(p[0] * inverseCell),
                                             (std::int64_t)std::floor(p[1] * inverseCell),
                                             (std::int64_t)std::floor(p[2] * inverseCell) };
    };
    auto key = [](std::int64_t x,
                  std::int64_t y,
                  std::int64_t z) {
        return (std::uint64_t)(x * 73856093) ^ (std::uint64_t)(y * 19349663) ^ (std::uint64_t)(z * 83492791);
    };
    std::vector<Vec3> positions(samples.size());
    std::vector<Vec3> moves(samples.size());
    for (int it = 0; it < iterations; ++it) {
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> grid;
        grid.reserve(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            positions[i] = samplePosition(triangles[samples[i].triangle], samples[i].w);
            const std::array<std::int64_t, 3> c = cellOf(positions[i]);
            grid[key(c[0], c[1], c[2])].push_back(i);
        }
        for (std::size_t i = 0; i < samples.size(); ++i) {
            Vec3 move = { 0., 0., 0. };
            const std::array<std::int64_t, 3> c = cellOf(positions[i]);
            for (std::int64_t dx = -1; dx <= 1; ++dx) {
                for (std::int64_t dy = -1; dy <= 1; ++dy) {
                    for (std::int64_t dz = -1; dz <= 1; ++dz) {
                        const auto found = grid.find(key(c[0] + dx, c[1] + dy, c[2] + dz));
                        if (found == grid.end()) {
                            continue;
                        }
                        for (std::size_t j : found->second) {
                            if (j == i) {
                                continue;
                            }
                            const Vec3 d = sub(positions[i], positions[j]);
                            const double distance = length(d);
                            if ((distance <= 0.) || (distance >= radius)) {
                                continue;
                            }
                            const double push = 0.5 * (radius - distance) / distance;
                            for (int k = 0; k < 3; ++k) {
                                move[k] += push * d[k];
                            }
                        }
                    }
                }
            }
            moves[i] = move;
        }
        // Moves are damped so a crowded point does not overshoot its neighbours.
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const Vec3 target = { positions[i][0] + 0.25 * moves[i][0], positions[i][1] + 0.25 * moves[i][1],
                                  positions[i][2] + 0.25 * moves[i][2] };
            samples[i].w = closestBarycentric(triangles[samples[i].triangle], target);
        }
    }
} // relax

enum TransferSourceEnum {
    eTransferPoint = 0,
    eTransferVertex,
    eTransferPrimitive,
};

struct Transfer {
    const GeoAttribute* source = nullptr;
    TransferSourceEnum from = eTransferPoint;
    GeoAttribute* destination = nullptr;
};

// The most specific class carrying the attribute wins, so per-corner values beat per-point ones.
const GeoAttribute*
findTransferSource(const GeoDetail& input,
                   const char* name,
                   TransferSourceEnum* from)
{
    if (const GeoAttribute* a = input.findAttribute(eGeoAttribClassVertex, name)) {
        *from = eTransferVertex;

        return a;
    }
    if (const GeoAttribute* a = input.findAttribute(eGeoAttribClassPoint, name)) {
        *from = eTransferPoint;

        return a;
    }
    if (const GeoAttribute* a = input.findAttribute(eGeoAttribClassPrimitive, name)) {
        *from = eTransferPrimitive;

        return a;
    }

    return nullptr;
}
} // anonymous namespace

bool
scatterGeometry(const GeoDetail& input,
                const GeoScatterParams& params,
                GeoDetailBuilder& output,
                std::string* errorMessage)
{
    GeoElementMask selected;
    if (!geoSelectElements(input, eGeoAttribClassPrimitive, params.group, &selected, errorMessage)) {
        return false;
    }

    std::vector<Triangle> triangles;
    std::vector<double> cumulativeArea;
    double totalArea = 0.;
    auto addPrimitive = [&](GeoIndex index,
                            GeoOffset primitive) {
        if (!selected[std::size_t(primitive)] || input.primitiveType(primitive) != eGeoPrimitivePolygon) {
            return;
        }
        const GeoVertexRange range = input.primitiveVertices(primitive);
        for (GeoOffset k = 1; k + 1 < range.count; ++k) {
            Triangle t;
            t.primitive = primitive;
            t.primitiveIndex = index;
            t.fanIndex = (int)(k - 1);
            t.vertices = { range.start, range.start + k, range.start + k + 1 };
            for (int c = 0; c < 3; ++c) {
                t.points[c] = input.vertexPoint(t.vertices[c]);
                t.p[c] = input.pointPosition(t.points[c]);
            }
            const double area = 0.5 * length(cross(sub(t.p[1], t.p[0]), sub(t.p[2], t.p[0])));
            if (!(area > 0.)) {
                continue;
            }
            totalArea += area;
            triangles.push_back(t);
            cumulativeArea.push_back(totalArea);
        }
    };
    input.primitiveMap().forEachLive(addPrimitive);

    output.setPointPrecision64(input.pointPrecision64());
    long long count = (params.generateBy == eGeoScatterGenerateByCount) ? (long long)params.count
                                                                        : std::llround(std::max(0., params.density) * totalArea);
    if (triangles.empty() || (count <= 0)) {
        return true;
    }

    // Stratifying the area-weighted pick covers the surface more evenly than independent picks
    // while keeping the count exact.
    std::mt19937_64 rng((std::uint64_t)(std::int64_t)params.seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull);
    std::vector<Sample> samples((std::size_t)count);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double pick = ((double)i + unitRandom(rng)) / (double)count * totalArea;
        std::size_t t = (std::size_t)(std::upper_bound(cumulativeArea.begin(), cumulativeArea.end(), pick) - cumulativeArea.begin());
        t = std::min(t, triangles.size() - 1);
        const double s = std::sqrt(unitRandom(rng));
        const double r = unitRandom(rng);
        samples[i].triangle = t;
        samples[i].w = { 1. - s, s * (1. - r), s * r };
    }
    if (params.relaxPoints && (params.relaxIterations > 0)) {
        relax(triangles, totalArea, params.relaxIterations, samples);
    }

    const GeoOffset first = output.appendPoints((GeoIndex)count);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Vec3 p = samplePosition(triangles[samples[i].triangle], samples[i].w);
        output.setPointPosition(first + (GeoOffset)i, p[0], p[1], p[2]);
    }

    std::vector<Transfer> transfers;
    bool computeNormals = false;
    const std::pair<bool, const char*> wanted[] = {
        { params.transferN, "N" },
        { params.transferUV, "uv" },
        { params.transferCd, "Cd" },
    };
    for (const auto& [enabled, name] : wanted) {
        if (!enabled) {
            continue;
        }
        Transfer transfer;
        transfer.source = findTransferSource(input, name, &transfer.from);
        if (transfer.source) {
            GeoAttributeDesc desc = transfer.source->desc();
            transfer.destination = &output.addAttribute(eGeoAttribClassPoint, desc);
            transfers.push_back(transfer);
        } else if (std::string(name) == "N") {
            computeNormals = true;
        }
    }
    GeoAttribute* normals = nullptr;
    if (computeNormals) {
        GeoAttributeDesc desc;
        desc.name = "N";
        desc.storage = eGeoStorageFloat32;
        desc.tupleSize = 3;
        desc.typeInfo = eGeoTypeInfoNormal;
        normals = &output.addAttribute(eGeoAttribClassPoint, desc);
    }
    GeoAttribute* sourcePrim = nullptr;
    if (params.outputSourcePrim) {
        GeoAttributeDesc desc;
        desc.name = "sourceprim";
        desc.storage = eGeoStorageInt32;
        sourcePrim = &output.addAttribute(eGeoAttribClassPoint, desc);
    }
    GeoAttribute* sourcePrimUV = nullptr;
    if (params.outputSourcePrimUV) {
        GeoAttributeDesc desc;
        desc.name = "sourceprimuv";
        desc.storage = eGeoStorageFloat32;
        desc.tupleSize = 3;
        sourcePrimUV = &output.addAttribute(eGeoAttribClassPoint, desc);
    }

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Triangle& t = triangles[samples[i].triangle];
        const std::array<double, 3>& w = samples[i].w;
        const std::size_t point = (std::size_t)(first + (GeoOffset)i);
        for (const Transfer& transfer : transfers) {
            switch (transfer.from) {
            case eTransferPoint:
                geoBlendElement(*transfer.destination, point, *transfer.source, t.points, w);
                break;
            case eTransferVertex:
                geoBlendElement(*transfer.destination, point, *transfer.source, t.vertices, w);
                break;
            case eTransferPrimitive:
                geoCopyElement(*transfer.destination, point, *transfer.source, (std::size_t)t.primitive);
                break;
            }
        }
        if (normals) {
            Vec3 n = cross(sub(t.p[1], t.p[0]), sub(t.p[2], t.p[0]));
            const double l = length(n);
            const float out[3] = { (float)(n[0] / l), (float)(n[1] / l), (float)(n[2] / l) };
            normals->setTuple<float>(point, std::span<const float>(out, 3));
        }
        if (sourcePrim) {
            sourcePrim->set<std::int32_t>(point, (std::int32_t)t.primitiveIndex);
        }
        if (sourcePrimUV) {
            // Houdini's primitive uv: barycentric on a triangle, bilinear on a quad. Larger
            // polygons keep the fan triangle's barycentrics with its place in the fan as w.
            float uvw[3] = { (float)w[1], (float)w[2], (float)t.fanIndex };
            const GeoVertexRange range = input.primitiveVertices(t.primitive);
            if (range.count == 3) {
                uvw[2] = 0.f;
            } else if (range.count == 4) {
                std::array<Vec3, 4> quad;
                for (int c = 0; c < 4; ++c) {
                    quad[(std::size_t)c] = input.pointPosition(input.vertexPoint(range.start + c));
                }
                const std::array<double, 2> uv = inverseBilinear(quad[0], quad[1], quad[2], quad[3], samplePosition(t, w));
                uvw[0] = (float)uv[0];
                uvw[1] = (float)uv[1];
                uvw[2] = 0.f;
            }
            sourcePrimUV->setTuple<float>(point, std::span<const float>(uvw, 3));
        }
    }

    return true;
} // scatterGeometry

GeoScatter::GeoScatter(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoScatter::~GeoScatter()
{
}

const NodeSpec*
GeoScatter::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoScatter::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kGeoScatterParamGroup);
    group->setHintToolTip(tr("The primitives to scatter over: a group pattern (group names, globs, element numbers, @attr comparisons, ! and ^). Empty scatters over every polygon.").toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr generateBy = createKnob<KnobChoice>(tr("Generate By").toStdString());
    generateBy->setName(kGeoScatterParamGenerateBy);
    generateBy->setHintToolTip(tr("How many points to scatter.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("count", tr("Count").toStdString(), tr("Exactly Count points over the whole surface.").toStdString()));
        options.push_back(ChoiceOption("density", tr("Density").toStdString(),
                                       tr("Density points per unit of area, rounded to the nearest whole point.").toStdString()));
        generateBy->populateChoices(options);
    }
    generateBy->setDefaultValue((int)eGeoScatterGenerateByCount);
    generateBy->setAnimationEnabled(false);
    page->addKnob(generateBy);
    _generateBy = generateBy;

    KnobIntPtr count = createKnob<KnobInt>(tr("Count").toStdString());
    count->setName(kGeoScatterParamCount);
    count->setHintToolTip(tr("The number of points to scatter.").toStdString());
    count->setDefaultValue(1000);
    count->setMinimum(0);
    count->setDisplayMinimum(0);
    count->setDisplayMaximum(100000);
    page->addKnob(count);
    _count = count;

    KnobDoublePtr density = createKnob<KnobDouble>(tr("Density").toStdString());
    density->setName(kGeoScatterParamDensity);
    density->setHintToolTip(tr("Points per unit of surface area.").toStdString());
    density->setDefaultValue(10.);
    density->setMinimum(0.);
    density->setDisplayMinimum(0.);
    density->setDisplayMaximum(1000.);
    page->addKnob(density);
    _density = density;

    KnobIntPtr seed = createKnob<KnobInt>(tr("Seed").toStdString());
    seed->setName(kGeoScatterParamSeed);
    seed->setHintToolTip(tr("Changes the random placement. The same seed always places the same points.").toStdString());
    seed->setDefaultValue(0);
    page->addKnob(seed);
    _seed = seed;

    KnobBoolPtr relaxPoints = createKnob<KnobBool>(tr("Relax Points").toStdString());
    relaxPoints->setName(kGeoScatterParamRelaxPoints);
    relaxPoints->setHintToolTip(tr("Pushes points that landed close together apart, for a more even spread. Each point "
                                   "stays on the triangle it was scattered on.")
                                    .toStdString());
    relaxPoints->setDefaultValue(false);
    relaxPoints->setAnimationEnabled(false);
    page->addKnob(relaxPoints);
    _relaxPoints = relaxPoints;

    KnobIntPtr relaxIterations = createKnob<KnobInt>(tr("Relax Iterations").toStdString());
    relaxIterations->setName(kGeoScatterParamRelaxIterations);
    relaxIterations->setHintToolTip(tr("How many times points are pushed apart. More is more even, and slower.").toStdString());
    relaxIterations->setDefaultValue(10);
    relaxIterations->setMinimum(0);
    relaxIterations->setDisplayMinimum(0);
    relaxIterations->setDisplayMaximum(100);
    relaxIterations->setAnimationEnabled(false);
    page->addKnob(relaxIterations);
    _relaxIterations = relaxIterations;

    auto createToggle = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip) {
        KnobBoolPtr knob = createKnob<KnobBool>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(false);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    _transferN = createToggle(tr("Transfer N"), kGeoScatterParamTransferN,
                              tr("Gives each point the surface normal N interpolated where it landed, or the face's own "
                                 "normal when the input has no N."));
    _transferUV = createToggle(tr("Transfer UV"), kGeoScatterParamTransferUV,
                               tr("Gives each point the texture coordinates uv interpolated where it landed."));
    _transferCd = createToggle(tr("Transfer Cd"), kGeoScatterParamTransferCd,
                               tr("Gives each point the colour Cd interpolated where it landed."));
    _outputSourcePrim = createToggle(tr("Output Source Prim"), kGeoScatterParamOutputSourcePrim,
                                     tr("Records on each point, as sourceprim, the number of the polygon it landed on."));
    _outputSourcePrimUV = createToggle(tr("Output Source Prim UV"), kGeoScatterParamOutputSourcePrimUV,
                                       tr("Records on each point, as sourceprimuv, where on its polygon it landed: on a triangle "
                                          "the weights of its second and third corners, on a quad its bilinear (u, v) from "
                                          "the first corner (u towards the second, v towards the fourth). On larger polygons, "
                                          "the weights of the second and third corners of the fan triangle it landed on, "
                                          "and as third component which triangle of the polygon that is, counting from "
                                          "the first corner round."));

    updateEnabled();
} // GeoScatter::initializeKnobs

void
GeoScatter::updateEnabled()
{
    const KnobChoicePtr generateBy = _generateBy.lock();
    const KnobBoolPtr relaxPoints = _relaxPoints.lock();
    if (!generateBy || !relaxPoints) {
        return;
    }
    const bool byCount = generateBy->getValue() == (int)eGeoScatterGenerateByCount;
    if (const KnobIntPtr count = _count.lock()) {
        count->setSecret(!byCount);
    }
    if (const KnobDoublePtr density = _density.lock()) {
        density->setSecret(byCount);
    }
    if (const KnobIntPtr iterations = _relaxIterations.lock()) {
        iterations->setAllDimensionsEnabled(relaxPoints->getValue());
    }
}

void
GeoScatter::onKnobsLoaded()
{
    updateEnabled();
}

bool
GeoScatter::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr generateBy = _generateBy.lock();
    const KnobBoolPtr relaxPoints = _relaxPoints.lock();
    if ((generateBy && (k == generateBy.get())) || (relaxPoints && (k == relaxPoints.get()))) {
        updateEnabled();

        return true;
    }

    return false;
}

GeoScatterParams
GeoScatter::readParams(double time,
                       ViewSpec view) const
{
    GeoScatterParams p;
    if (const KnobStringPtr group = _group.lock()) {
        p.group = group->getValueAtTime(time, 0, view);
    }
    if (const KnobChoicePtr generateBy = _generateBy.lock()) {
        p.generateBy = generateBy->getValueAtTime(time, 0, view) == (int)eGeoScatterGenerateByDensity
            ? eGeoScatterGenerateByDensity
            : eGeoScatterGenerateByCount;
    }
    if (const KnobIntPtr count = _count.lock()) {
        p.count = count->getValueAtTime(time, 0, view);
    }
    if (const KnobDoublePtr density = _density.lock()) {
        p.density = density->getValueAtTime(time, 0, view);
    }
    if (const KnobIntPtr seed = _seed.lock()) {
        p.seed = seed->getValueAtTime(time, 0, view);
    }
    if (const KnobBoolPtr relaxPoints = _relaxPoints.lock()) {
        p.relaxPoints = relaxPoints->getValueAtTime(time, 0, view);
    }
    if (const KnobIntPtr relaxIterations = _relaxIterations.lock()) {
        p.relaxIterations = relaxIterations->getValueAtTime(time, 0, view);
    }
    auto toggle = [time, view](const KnobBoolWPtr& weak,
                               bool fallback) {
        const KnobBoolPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    p.transferN = toggle(_transferN, p.transferN);
    p.transferUV = toggle(_transferUV, p.transferUV);
    p.transferCd = toggle(_transferCd, p.transferCd);
    p.outputSourcePrim = toggle(_outputSourcePrim, p.outputSourcePrim);
    p.outputSourcePrimUV = toggle(_outputSourcePrimUV, p.outputSourcePrimUV);

    return p;
} // GeoScatter::readParams

StatusEnum
GeoScatter::renderGeometry(const GeoRenderActionArgs& args,
                           GeoDetailConstPtr* outputDetail,
                           std::string* errorMessage)
{
    const GeoDetailConstPtr input = args.getInputDetail(0);
    if (!input) {
        if (errorMessage) {
            *errorMessage = tr("GeoScatter needs geometry on its Source input.").toStdString();
        }

        return eStatusFailed;
    }
    const GeoScatterParams params = readParams(args.time, args.view);

    return renderGeometryGenerator([&input, &params](GeoDetailBuilder& detail, std::string* error) {
        return scatterGeometry(*input, params, detail, error) ? eStatusOK : eStatusFailed;
    },
                                   outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
