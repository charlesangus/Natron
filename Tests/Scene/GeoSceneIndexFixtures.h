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

#ifndef Tests_Scene_GeoSceneIndexFixtures_h
#define Tests_Scene_GeoSceneIndexFixtures_h

// The details the GeoSceneIndex tests and the parity checks run on. Header-only so any scene
// test can serve the same geometry.

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Engine/Geo/GeoPacked.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

namespace GeoSceneIndexFixtures {

inline NATRON_NAMESPACE::GeoDetailConstPtr
finish(NATRON_NAMESPACE::GeoDetailBuilder& b)
{
    return std::make_shared<const NATRON_NAMESPACE::GeoDetail>(b.finish());
}

inline NATRON_NAMESPACE::GeoAttribute&
addFloatAttribute(NATRON_NAMESPACE::GeoDetailBuilder& b,
                  NATRON_NAMESPACE::GeoAttribClassEnum cls,
                  const char* name,
                  int tupleSize,
                  NATRON_NAMESPACE::GeoTypeInfoEnum typeInfo = NATRON_NAMESPACE::eGeoTypeInfoNone)
{
    NATRON_NAMESPACE::GeoAttributeDesc desc;
    desc.name = name;
    desc.storage = NATRON_NAMESPACE::eGeoStorageFloat32;
    desc.tupleSize = tupleSize;
    desc.typeInfo = typeInfo;

    return b.addAttribute(cls, desc);
}

inline NATRON_NAMESPACE::GeoAttribute&
addPointColour(NATRON_NAMESPACE::GeoDetailBuilder& b)
{
    return addFloatAttribute(b, NATRON_NAMESPACE::eGeoAttribClassPoint, "Cd", 3, NATRON_NAMESPACE::eGeoTypeInfoColour);
}

inline NATRON_NAMESPACE::GeoAttribute&
addPathAttribute(NATRON_NAMESPACE::GeoDetailBuilder& b)
{
    NATRON_NAMESPACE::GeoAttributeDesc desc;
    desc.name = "path";
    desc.storage = NATRON_NAMESPACE::eGeoStorageString;

    return b.addAttribute(NATRON_NAMESPACE::eGeoAttribClassPrimitive, desc);
}

// A unit quad at (x, y), its corner colours derived from where they are, so a quad built in
// another order carries the same values.
inline NATRON_NAMESPACE::GeoOffset
addQuad(NATRON_NAMESPACE::GeoDetailBuilder& b,
        double x,
        double y,
        NATRON_NAMESPACE::GeoAttribute* colour)
{
    using namespace NATRON_NAMESPACE;
    const GeoOffset first = b.appendPoints(4);
    const double corners[4][2] = { { x, y }, { x + 1., y }, { x + 1., y + 1. }, { x, y + 1. } };
    GeoOffset offsets[4];
    for (int i = 0; i < 4; ++i) {
        offsets[i] = first + i;
        b.setPointPosition(offsets[i], corners[i][0], corners[i][1], 0.);
        if (colour) {
            const float c[3] = { float(corners[i][0]) * 0.1f, float(corners[i][1]) * 0.1f, 0.5f };
            colour->setTuple<float>(std::size_t(offsets[i]), std::span<const float>(c, 3));
        }
    }

    return b.appendPrimitive(eGeoPrimitivePolygon, offsets);
}

inline NATRON_NAMESPACE::GeoDetailConstPtr
makeCube()
{
    using namespace NATRON_NAMESPACE;
    GeoShapeParams params;
    params.shape = eGeoShapeCube;
    GeoDetailBuilder b;
    buildGeoShape(params, b);

    return finish(b);
}

// Three loose points with a colour, a weight and a pscale of @p pscale, 2 * @p pscale, ...; a
// zero @p pscale leaves pscale out, so the widths fall back to the convert's default.
inline NATRON_NAMESPACE::GeoDetailConstPtr
makePoints(float pscale = 0.1f,
           double shift = 0.)
{
    using namespace NATRON_NAMESPACE;
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(3);
    GeoAttribute& colour = addPointColour(b);
    GeoAttribute& weight = addFloatAttribute(b, eGeoAttribClassPoint, "weight", 1);
    GeoAttribute* scale = pscale != 0.f ? &addFloatAttribute(b, eGeoAttribClassPoint, "pscale", 1) : nullptr;
    for (int i = 0; i < 3; ++i) {
        b.setPointPosition(first + i, double(i) + shift, 0., 1.);
        const float c[3] = { 0.25f * float(i), 0.5f, 0.75f };
        colour.setTuple<float>(std::size_t(first + i), std::span<const float>(c, 3));
        weight.set<float>(std::size_t(first + i), float(i) * 0.5f);
        if (scale) {
            scale->set<float>(std::size_t(first + i), pscale * float(i + 1));
        }
    }

    return finish(b);
}

// A three point polyline.
inline NATRON_NAMESPACE::GeoDetailConstPtr
makeCurve()
{
    using namespace NATRON_NAMESPACE;
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(3);
    for (int i = 0; i < 3; ++i) {
        b.setPointPosition(first + i, double(i), double(i * i), -1.);
    }
    const GeoOffset points[] = { first, first + 1, first + 2 };
    b.appendPrimitive(eGeoPrimitivePolyline, points);

    return finish(b);
}

// Polylines of three and four points with a primitive attribute.
inline NATRON_NAMESPACE::GeoDetailConstPtr
makeCurves()
{
    using namespace NATRON_NAMESPACE;
    GeoDetailBuilder b;
    GeoAttribute& tension = addFloatAttribute(b, eGeoAttribClassPrimitive, "tension", 1);
    for (int c = 0; c < 2; ++c) {
        const GeoOffset first = b.appendPoints(3 + c);
        std::vector<GeoOffset> points;
        for (int i = 0; i < 3 + c; ++i) {
            b.setPointPosition(first + i, double(i), double(c), double(i * c));
            points.push_back(first + i);
        }
        b.appendPrimitive(eGeoPrimitivePolyline, std::span<const GeoOffset>(points.data(), points.size()));
        tension.set<float>(std::size_t(c), 0.5f + float(c));
    }

    return finish(b);
}

// Quads at /geo/a (x = 0) and /geo/<bName> (x = 5), "sel" holding the /geo/a quad. @p bFirst
// builds the second quad first, so only the primitive order differs.
inline NATRON_NAMESPACE::GeoDetailConstPtr
makeGrouped(const std::string& bName = "b",
            bool bFirst = false)
{
    using namespace NATRON_NAMESPACE;
    GeoDetailBuilder b;
    GeoAttribute& colour = addPointColour(b);
    addQuad(b, bFirst ? 5. : 0., 0., &colour);
    addQuad(b, bFirst ? 0. : 5., 0., &colour);
    GeoAttribute& path = addPathAttribute(b);
    path.setString(bFirst ? 1 : 0, "/geo/a");
    path.setString(bFirst ? 0 : 1, "/geo/" + bName);
    b.addGroup(eGeoAttribClassPrimitive, "sel").add(GeoOffset(bFirst ? 1 : 0));

    return finish(b);
}

// Two quads on /geo/shapes/a and one on /geo/b, "sel" holding the second quad and "all" every one.
inline NATRON_NAMESPACE::GeoDetailConstPtr
makeNested()
{
    using namespace NATRON_NAMESPACE;
    GeoDetailBuilder b;
    GeoAttribute& colour = addPointColour(b);
    for (int i = 0; i < 3; ++i) {
        addQuad(b, 2. * i, 0., &colour);
    }
    GeoAttribute& path = addPathAttribute(b);
    path.setString(0, "/geo/shapes/a");
    path.setString(1, "/geo/shapes/a");
    path.setString(2, "/geo/b");
    b.addGroup(eGeoAttribClassPrimitive, "sel").add(GeoOffset(1));
    auto& all = b.addGroup(eGeoAttribClassPrimitive, "all");
    for (int i = 0; i < 3; ++i) {
        all.add(GeoOffset(i));
    }

    return finish(b);
}

// One quad at (x, 0), plus a second one at (x, 3) when @p two; @p insideMove moves its first
// corner within the quad's bounds.
inline NATRON_NAMESPACE::GeoDetailConstPtr
makeQuads(double x,
          bool two = false,
          double insideMove = 0.)
{
    using namespace NATRON_NAMESPACE;
    GeoDetailBuilder b;
    addQuad(b, x, 0., nullptr);
    if (insideMove != 0.) {
        b.setPointPosition(0, x + insideMove, insideMove, 0.);
    }
    if (two) {
        addQuad(b, x, 3., nullptr);
    }

    return finish(b);
}

inline NATRON_NAMESPACE::GeoMatrix4
translation(double x,
            double y,
            double z)
{
    NATRON_NAMESPACE::GeoMatrix4 m = NATRON_NAMESPACE::geoMatrixIdentity();
    m[3] = x;
    m[7] = y;
    m[11] = z;

    return m;
}

// A packed USD primitive overriding /geo/xf with a translation by (x, y, z), and a quad at
// /geo/xf/m under it.
inline NATRON_NAMESPACE::GeoDetailConstPtr
makeTransformedParent(double x,
                      double y,
                      double z)
{
    using namespace NATRON_NAMESPACE;
    GeoDetailBuilder b;
    const GeoOffset pivot = b.appendPoints(1);
    geoAppendPackedUSD(b, pivot, "/geo/xf", geoMatrixIdentity(), translation(x, y, z), GeoBounds { { 0., 0., 0. }, { 1., 1., 0. } });
    const GeoOffset quad = addQuad(b, 0., 0., nullptr);
    GeoAttribute& path = addPathAttribute(b);
    path.setString(std::size_t(quad), "/geo/xf/m");

    return finish(b);
}

// One primitive at /geo/a in group "sel": a quad, or a four point polyline when @p polyline.
// Below /geo/a/m sits a quad when @p child.
inline NATRON_NAMESPACE::GeoDetailConstPtr
makeRetyped(bool polyline,
            bool child = false)
{
    using namespace NATRON_NAMESPACE;
    GeoDetailBuilder b;
    GeoOffset primitive;
    if (polyline) {
        const GeoOffset first = b.appendPoints(4);
        const GeoOffset points[] = { first, first + 1, first + 2, first + 3 };
        for (int i = 0; i < 4; ++i) {
            b.setPointPosition(first + i, double(i), 0., 0.);
        }
        primitive = b.appendPrimitive(eGeoPrimitivePolyline, points);
    } else {
        primitive = addQuad(b, 0., 0., nullptr);
    }
    const GeoOffset below = child ? addQuad(b, 3., 0., nullptr) : GeoOffset(-1);
    GeoAttribute& path = addPathAttribute(b);
    path.setString(std::size_t(primitive), "/geo/a");
    if (child) {
        path.setString(std::size_t(below), "/geo/a/m");
    }
    b.addGroup(eGeoAttribClassPrimitive, "sel").add(primitive);

    return finish(b);
}

} // namespace GeoSceneIndexFixtures

#endif // Tests_Scene_GeoSceneIndexFixtures_h
