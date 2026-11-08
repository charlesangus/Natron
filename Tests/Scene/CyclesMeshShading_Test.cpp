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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include <pxr/imaging/hd/legacyDisplayStyleSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/tokens.h>

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/PrimitiveMeshes.h"

NATRON_NAMESPACE_USING
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

const SdfPath kMeshPath("/Mesh");

HdContainerDataSourceHandle
primvar(const VtValue& value,
        const TfToken& interpolation)
{
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedSampledDataSource::New(value))
        .SetInterpolation(HdRetainedTypedSampledDataSource<TfToken>::New(interpolation))
        .Build();
}

// mesh as UsdImaging hands a polygonal mesh to Hydra, with a vertex primvar "weight" holding each
// point's index and a faceVarying "st".
HdContainerDataSourceHandle
meshPrim(const PrimitiveMesh& mesh,
         const TfToken& normalsInterpolation)
{
    const HdContainerDataSourceHandle topology = HdMeshTopologySchema::Builder()
                                                     .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(mesh.faceVertexCounts))
                                                     .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(mesh.faceVertexIndices))
                                                     .SetOrientation(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->rightHanded))
                                                     .Build();
    const HdContainerDataSourceHandle meshSchema = HdMeshSchema::Builder()
                                                       .SetTopology(topology)
                                                       .SetSubdivisionScheme(HdRetainedTypedSampledDataSource<TfToken>::New(TfToken("none")))
                                                       .Build();
    VtFloatArray weights(mesh.points.size());
    for (std::size_t i = 0; i < weights.size(); ++i) {
        weights[i] = (float)i;
    }
    VtVec2fArray st(mesh.stIndices.size());
    for (std::size_t i = 0; i < st.size(); ++i) {
        st[i] = mesh.st[(std::size_t)mesh.stIndices[i]];
    }
    const TfToken names[] = { HdTokens->points, HdTokens->normals, TfToken("weight"), TfToken("st") };
    const HdDataSourceBaseHandle values[] = {
        primvar(VtValue(mesh.points), HdPrimvarSchemaTokens->vertex),
        primvar(VtValue(mesh.normals), normalsInterpolation),
        primvar(VtValue(weights), HdPrimvarSchemaTokens->vertex),
        primvar(VtValue(st), HdPrimvarSchemaTokens->faceVarying),
    };
    const TfToken top[] = { HdMeshSchema::GetSchemaToken(), HdPrimvarsSchema::GetSchemaToken() };
    const HdDataSourceBaseHandle children[] = { meshSchema, HdRetainedContainerDataSource::New(4, names, values) };

    return HdRetainedContainerDataSource::New(2, top, children);
}

HdSceneIndexPrim
shaded(const HdContainerDataSourceHandle& prim)
{
    const HdRetainedSceneIndexRefPtr input = HdRetainedSceneIndex::New();
    input->AddPrims({ { kMeshPath, HdPrimTypeTokens->mesh, prim } });
    const HdSceneIndexBaseRefPtr shading = CyclesAovs::meshShadingSceneIndex(input);

    return shading->GetPrim(kMeshPath);
}

bool
flat(const HdSceneIndexPrim& prim)
{
    const HdBoolDataSourceHandle ds = HdLegacyDisplayStyleSchema::GetFromParent(prim.dataSource).GetFlatShadingEnabled();

    return ds && ds->GetTypedValue(0.f);
}

template <typename T>
VtArray<T>
primvarValue(const HdSceneIndexPrim& prim,
             const TfToken& name,
             TfToken* interpolation = nullptr)
{
    const HdPrimvarSchema schema = HdPrimvarsSchema::GetFromParent(prim.dataSource).GetPrimvar(name);
    if (interpolation) {
        *interpolation = schema.GetInterpolation() ? schema.GetInterpolation()->GetTypedValue(0.f) : TfToken();
    }
    const VtValue value = schema.GetPrimvarValue() ? schema.GetPrimvarValue()->GetValue(0.f) : VtValue();

    return value.IsHolding<VtArray<T>>() ? value.UncheckedGet<VtArray<T>>() : VtArray<T>();
}

VtIntArray
faceVertexIndices(const HdSceneIndexPrim& prim)
{
    const HdIntArrayDataSourceHandle ds = HdMeshSchema::GetFromParent(prim.dataSource).GetTopology().GetFaceVertexIndices();

    return ds ? ds->GetTypedValue(0.f) : VtIntArray();
}

// Cycles, which reads vertex normals only, gets mesh's normals at every face corner, at the
// corner's point, with the corner's weight and st.
void
expectAuthoredNormalsAsVertexNormals(const PrimitiveMesh& mesh,
                                     const HdSceneIndexPrim& prim)
{
    EXPECT_FALSE(flat(prim));
    const VtIntArray indices = faceVertexIndices(prim);
    ASSERT_EQ(mesh.faceVertexIndices.size(), indices.size());
    TfToken interpolation;
    const VtVec3fArray normals = primvarValue<GfVec3f>(prim, HdTokens->normals, &interpolation);
    EXPECT_EQ(HdPrimvarSchemaTokens->vertex, interpolation);
    const VtVec3fArray points = primvarValue<GfVec3f>(prim, HdTokens->points);
    const VtFloatArray weights = primvarValue<float>(prim, TfToken("weight"));
    const VtVec2fArray st = primvarValue<GfVec2f>(prim, TfToken("st"), &interpolation);
    EXPECT_EQ(HdPrimvarSchemaTokens->faceVarying, interpolation);
    ASSERT_EQ(mesh.stIndices.size(), st.size());
    for (std::size_t corner = 0; corner < indices.size(); ++corner) {
        const std::size_t point = (std::size_t)indices[corner];
        const std::size_t original = (std::size_t)mesh.faceVertexIndices[corner];
        ASSERT_LT(point, normals.size());
        ASSERT_LT(point, points.size());
        ASSERT_LT(point, weights.size());
        EXPECT_GT(GfDot(normals[point], mesh.normals[corner]), 0.999f) << "corner " << corner;
        EXPECT_EQ(mesh.points[original], points[point]) << "corner " << corner;
        EXPECT_EQ((float)original, weights[point]) << "corner " << corner;
        EXPECT_EQ(mesh.st[(std::size_t)mesh.stIndices[corner]], st[corner]) << "corner " << corner;
    }
}

} // anon namespace

// The cube's faceVarying normals are its faces' own: Cycles shades it flat, and its points stay shared.
TEST(CyclesMeshShading, HardFaceVaryingNormalsShadeFlat)
{
    const int segments[3] = { 2, 2, 2 };
    const PrimitiveMesh cube = PrimitiveMeshes::buildCube(GfVec3d(1., 2., 3.), segments, SceneMetadata::eUpAxisY);
    const HdSceneIndexPrim prim = shaded(meshPrim(cube, HdPrimvarSchemaTokens->faceVarying));
    EXPECT_TRUE(flat(prim));
    EXPECT_EQ(cube.faceVertexIndices, faceVertexIndices(prim));

    const PrimitiveMesh card = PrimitiveMeshes::buildCard(1.5, 1., 3, 4, SceneMetadata::eUpAxisZ);
    EXPECT_TRUE(flat(shaded(meshPrim(card, HdPrimvarSchemaTokens->faceVarying))));
}

// The sphere's normals agree at each point: they become vertex normals.
TEST(CyclesMeshShading, SmoothFaceVaryingNormalsBecomeVertexNormals)
{
    const PrimitiveMesh sphere = PrimitiveMeshes::buildSphere(1., 8, 12, SceneMetadata::eUpAxisY);
    expectAuthoredNormalsAsVertexNormals(sphere, shaded(meshPrim(sphere, HdPrimvarSchemaTokens->faceVarying)));
}

// The cylinder's rim points carry a side normal and a cap normal: the mesh gets a point per face
// corner.
TEST(CyclesMeshShading, SplitFaceVaryingNormalsUnweldTheMesh)
{
    const PrimitiveMesh cylinder = PrimitiveMeshes::buildCylinder(1., 2., 2, 12, true, true, SceneMetadata::eUpAxisY);
    const HdSceneIndexPrim prim = shaded(meshPrim(cylinder, HdPrimvarSchemaTokens->faceVarying));
    expectAuthoredNormalsAsVertexNormals(cylinder, prim);
    const VtIntArray indices = faceVertexIndices(prim);
    for (std::size_t corner = 0; corner < indices.size(); ++corner) {
        EXPECT_EQ((int)corner, indices[corner]);
    }
}

// Vertex normals are what Cycles reads already; a mesh without normals is faceted.
TEST(CyclesMeshShading, OtherNormalsAsAuthored)
{
    const int segments[3] = { 1, 1, 1 };
    PrimitiveMesh cube = PrimitiveMeshes::buildCube(GfVec3d(1., 1., 1.), segments, SceneMetadata::eUpAxisY);
    cube.normals = cube.points;
    const HdSceneIndexPrim vertex = shaded(meshPrim(cube, HdPrimvarSchemaTokens->vertex));
    EXPECT_FALSE(flat(vertex));
    EXPECT_EQ(cube.faceVertexIndices, faceVertexIndices(vertex));
    EXPECT_EQ(cube.points, primvarValue<GfVec3f>(vertex, HdTokens->normals));

    cube.normals.clear();
    EXPECT_TRUE(flat(shaded(meshPrim(cube, HdPrimvarSchemaTokens->faceVarying))));
}
