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

#include "PrimitiveMeshes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace {
struct Corner {
    int point;
    int st;
    PXR_NS::GfVec3f normal;
};

// Builds Y-up; finish() turns the mesh for a Z-up scene.
class MeshBuilder {
public:
    int addPoint(const PXR_NS::GfVec3d& p)
    {
        _mesh.points.push_back(PXR_NS::GfVec3f((float)p[0], (float)p[1], (float)p[2]));

        return (int)_mesh.points.size() - 1;
    }

    int addSt(double s,
              double t)
    {
        _mesh.st.push_back(PXR_NS::GfVec2f((float)s, (float)t));

        return (int)_mesh.st.size() - 1;
    }

    void addFace(std::initializer_list<Corner> corners)
    {
        _mesh.faceVertexCounts.push_back((int)corners.size());
        for (const Corner& c : corners) {
            _mesh.faceVertexIndices.push_back(c.point);
            _mesh.stIndices.push_back(c.st);
            _mesh.normals.push_back(c.normal);
        }
    }

    PrimitiveMesh finish(SceneMetadata::UpAxisEnum upAxis)
    {
        if (upAxis == SceneMetadata::eUpAxisZ) {
            // A quarter turn about X, (x, y, z) -> (x, -z, y): +Y goes to +Z, and the forward
            // axis +Z to -Y, which a Z-up viewport's default view looks from.
            for (PXR_NS::GfVec3f& p : _mesh.points) {
                p = PXR_NS::GfVec3f(p[0], -p[2], p[1]);
            }
            for (PXR_NS::GfVec3f& n : _mesh.normals) {
                n = PXR_NS::GfVec3f(n[0], -n[2], n[1]);
            }
        }

        return _mesh;
    }

private:
    PrimitiveMesh _mesh;
};

// The unit direction around the up axis at column of columns: 0 at the back (-Z), a quarter turn
// at the left (-X), so u increases left to right seen from the front.
PXR_NS::GfVec3d
around(int column,
       int columns)
{
    const double phi = 2. * M_PI * (double)column / (double)columns;

    return PXR_NS::GfVec3d(-std::sin(phi), 0., -std::cos(phi));
}

PXR_NS::GfVec3f
toFloat(const PXR_NS::GfVec3d& v)
{
    return PXR_NS::GfVec3f((float)v[0], (float)v[1], (float)v[2]);
}
} // anonymous namespace

namespace PrimitiveMeshes {
PrimitiveMesh
buildCard(double aspect,
          double height,
          int rows,
          int columns,
          SceneMetadata::UpAxisEnum upAxis)
{
    rows = std::max(rows, kCardMinSegments);
    columns = std::max(columns, kCardMinSegments);
    const double width = height * aspect;
    MeshBuilder b;
    for (int i = 0; i <= rows; ++i) {
        const double t = (double)i / (double)rows;
        for (int j = 0; j <= columns; ++j) {
            const double s = (double)j / (double)columns;
            b.addPoint(PXR_NS::GfVec3d((s - 0.5) * width, (t - 0.5) * height, 0.));
            b.addSt(s, t);
        }
    }
    const PXR_NS::GfVec3f normal(0.f, 0.f, 1.f);
    const auto at = [columns, &normal](int i, int j) {
        const int index = i * (columns + 1) + j;

        return Corner { index, index, normal };
    };
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < columns; ++j) {
            b.addFace({ at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i + 1, j) });
        }
    }

    return b.finish(upAxis);
}

PrimitiveMesh
buildSphere(double radius,
            int rows,
            int columns,
            SceneMetadata::UpAxisEnum upAxis)
{
    rows = std::max(rows, kSphereMinRows);
    columns = std::max(columns, kSphereMinColumns);
    MeshBuilder b;
    // Ring r (1 to rows - 1, from the top) at polar angle pi * r / rows.
    const auto direction = [rows, columns](int r, int j) {
        const double theta = M_PI * (double)r / (double)rows;
        PXR_NS::GfVec3d d = around(j, columns) * std::sin(theta);
        d[1] = std::cos(theta);

        return d;
    };
    const int top = b.addPoint(PXR_NS::GfVec3d(0., radius, 0.));
    const int firstRingPoint = top + 1;
    for (int r = 1; r < rows; ++r) {
        for (int j = 0; j < columns; ++j) {
            b.addPoint(direction(r, j) * radius);
        }
    }
    const int bottom = b.addPoint(PXR_NS::GfVec3d(0., -radius, 0.));

    // columns + 1 st per ring: the seam's points carry u = 0 on one side and u = 1 on the other.
    int firstRingSt = -1;
    for (int r = 1; r < rows; ++r) {
        for (int j = 0; j <= columns; ++j) {
            const int st = b.addSt((double)j / (double)columns, 1. - (double)r / (double)rows);
            if (firstRingSt < 0) {
                firstRingSt = st;
            }
        }
    }
    // A pole's st sits mid-column, under the middle of each triangle meeting there.
    std::vector<int> topSt(columns), bottomSt(columns);
    for (int j = 0; j < columns; ++j) {
        topSt[j] = b.addSt(((double)j + 0.5) / (double)columns, 1.);
        bottomSt[j] = b.addSt(((double)j + 0.5) / (double)columns, 0.);
    }

    const auto ring = [&](int r, int j) {
        return Corner { firstRingPoint + (r - 1) * columns + (j % columns),
                        firstRingSt + (r - 1) * (columns + 1) + j,
                        toFloat(direction(r, j)) };
    };
    const PXR_NS::GfVec3f up(0.f, 1.f, 0.f);
    for (int j = 0; j < columns; ++j) {
        b.addFace({ ring(1, j), ring(1, j + 1), Corner { top, topSt[j], up } });
    }
    for (int r = 1; r < rows - 1; ++r) {
        for (int j = 0; j < columns; ++j) {
            b.addFace({ ring(r + 1, j), ring(r + 1, j + 1), ring(r, j + 1), ring(r, j) });
        }
    }
    for (int j = 0; j < columns; ++j) {
        b.addFace({ Corner { bottom, bottomSt[j], -up }, ring(rows - 1, j + 1), ring(rows - 1, j) });
    }

    return b.finish(upAxis);
} // buildSphere

PrimitiveMesh
buildCube(const PXR_NS::GfVec3d& size,
          const int segments[3],
          SceneMetadata::UpAxisEnum upAxis)
{
    // Built Y-up, so a Z-up scene's Y and Z sizes swap before finish() turns them back.
    const bool zUp = (upAxis == SceneMetadata::eUpAxisZ);
    const PXR_NS::GfVec3d extent = zUp ? PXR_NS::GfVec3d(size[0], size[2], size[1]) : size;
    std::array<int, 3> n = { segments[0], segments[1], segments[2] };
    if (zUp) {
        std::swap(n[1], n[2]);
    }
    for (int& count : n) {
        count = std::max(count, kCubeMinSegments);
    }

    MeshBuilder b;
    // Points on the lattice of segment boundaries, one per lattice position, so faces share
    // their edges.
    std::map<std::array<int, 3>, int> lattice;
    const auto pointAt = [&](const std::array<int, 3>& l) {
        const auto found = lattice.find(l);
        if (found != lattice.end()) {
            return found->second;
        }
        PXR_NS::GfVec3d p;
        for (int a = 0; a < 3; ++a) {
            p[a] = extent[a] * ((double)l[a] / (double)n[a] - 0.5);
        }
        const int index = b.addPoint(p);
        lattice[l] = index;

        return index;
    };

    // Each face as its outward axis and the axes pointing right and up seen from outside
    // (axis index, direction), right x up = outward.
    struct Face {
        int axis, sign, rightAxis, rightSign, upAxis, upSign;
    };
    const Face faces[] = {
        { 0, 1, 2, -1, 1, 1 },
        { 0, -1, 2, 1, 1, 1 },
        { 1, 1, 0, 1, 2, -1 },
        { 1, -1, 0, 1, 2, 1 },
        { 2, 1, 0, 1, 1, 1 },
        { 2, -1, 0, -1, 1, 1 },
    };
    for (const Face& f : faces) {
        const int nRight = n[f.rightAxis];
        const int nUp = n[f.upAxis];
        PXR_NS::GfVec3f normal(0.f);
        normal[f.axis] = (float)f.sign;
        const auto latticeAt = [&](int s, int t) {
            std::array<int, 3> l;
            l[f.axis] = (f.sign > 0) ? n[f.axis] : 0;
            l[f.rightAxis] = (f.rightSign > 0) ? s : nRight - s;
            l[f.upAxis] = (f.upSign > 0) ? t : nUp - t;

            return l;
        };
        const int firstSt = b.addSt(0., 0.);
        for (int t = 0; t <= nUp; ++t) {
            for (int s = 0; s <= nRight; ++s) {
                if ((s > 0) || (t > 0)) {
                    b.addSt((double)s / (double)nRight, (double)t / (double)nUp);
                }
            }
        }
        const auto at = [&](int s, int t) {
            return Corner { pointAt(latticeAt(s, t)), firstSt + t * (nRight + 1) + s, normal };
        };
        for (int t = 0; t < nUp; ++t) {
            for (int s = 0; s < nRight; ++s) {
                b.addFace({ at(s, t), at(s + 1, t), at(s + 1, t + 1), at(s, t + 1) });
            }
        }
    }

    return b.finish(upAxis);
} // buildCube

PrimitiveMesh
buildCylinder(double radius,
              double height,
              int rows,
              int columns,
              bool topCap,
              bool bottomCap,
              SceneMetadata::UpAxisEnum upAxis)
{
    rows = std::max(rows, kCylinderMinRows);
    columns = std::max(columns, kCylinderMinColumns);
    MeshBuilder b;
    // Ring r (0 to rows) from the top.
    for (int r = 0; r <= rows; ++r) {
        const double y = height * (0.5 - (double)r / (double)rows);
        for (int j = 0; j < columns; ++j) {
            PXR_NS::GfVec3d p = around(j, columns) * radius;
            p[1] = y;
            b.addPoint(p);
        }
    }
    const int firstSideSt = b.addSt(0., 1.);
    for (int r = 0; r <= rows; ++r) {
        for (int j = 0; j <= columns; ++j) {
            if ((r > 0) || (j > 0)) {
                b.addSt((double)j / (double)columns, 1. - (double)r / (double)rows);
            }
        }
    }
    const auto side = [&](int r, int j) {
        return Corner { r * columns + (j % columns), firstSideSt + r * (columns + 1) + j, toFloat(around(j, columns)) };
    };
    for (int r = 0; r < rows; ++r) {
        for (int j = 0; j < columns; ++j) {
            b.addFace({ side(r + 1, j), side(r + 1, j + 1), side(r, j + 1), side(r, j) });
        }
    }

    // A cap's st is its disc seen from outside: from above with the front down, from below with
    // the front up.
    const auto addCap = [&](bool isTop) {
        const int r = isTop ? 0 : rows;
        const double y = isTop ? 0.5 * height : -0.5 * height;
        const PXR_NS::GfVec3f normal(0.f, isTop ? 1.f : -1.f, 0.f);
        const Corner centre { b.addPoint(PXR_NS::GfVec3d(0., y, 0.)), b.addSt(0.5, 0.5), normal };
        std::vector<int> st(columns);
        for (int j = 0; j < columns; ++j) {
            const PXR_NS::GfVec3d d = around(j, columns);
            st[j] = b.addSt(0.5 + 0.5 * d[0], isTop ? 0.5 - 0.5 * d[2] : 0.5 + 0.5 * d[2]);
        }
        const auto rim = [&](int j) {
            return Corner { r * columns + (j % columns), st[j % columns], normal };
        };
        for (int j = 0; j < columns; ++j) {
            if (isTop) {
                b.addFace({ centre, rim(j), rim(j + 1) });
            } else {
                b.addFace({ centre, rim(j + 1), rim(j) });
            }
        }
    };
    if (topCap) {
        addCap(true);
    }
    if (bottomCap) {
        addCap(false);
    }

    return b.finish(upAxis);
} // buildCylinder
} // namespace PrimitiveMeshes

NATRON_NAMESPACE_EXIT
