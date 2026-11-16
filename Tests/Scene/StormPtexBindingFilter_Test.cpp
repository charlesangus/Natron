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

#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdr/registry.h>

#include "Engine/Nodes/Scene/StormPtexBindingFilter.h"

NATRON_NAMESPACE_USING
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

const SdfPath kPtexMaterial("/Looks/ptex");
const SdfPath kPlainMaterial("/Looks/plain");
const SdfPath kStrayPtexMaterial("/Looks/strayPtex");

HdContainerDataSourceHandle
connectionTo(const std::string& node,
             const std::string& output)
{
    return HdMaterialConnectionSchema::Builder()
        .SetUpstreamNodePath(HdRetainedTypedSampledDataSource<TfToken>::New(TfToken(node)))
        .SetUpstreamNodeOutputName(HdRetainedTypedSampledDataSource<TfToken>::New(TfToken(output)))
        .Build();
}

HdContainerDataSourceHandle
node(const std::string& identifier,
     const HdContainerDataSourceHandle& inputConnections = HdContainerDataSourceHandle())
{
    return HdMaterialNodeSchema::Builder()
        .SetNodeIdentifier(HdRetainedTypedSampledDataSource<TfToken>::New(TfToken(identifier)))
        .SetInputConnections(inputConnections ? inputConnections : HdRetainedContainerDataSource::New())
        .Build();
}

// A UsdPreviewSurface as the surface; with diffuseFromPtex its diffuseColor reads a Ptex texture,
// and with strayPtex a Ptex texture sits in the network unconnected.
HdContainerDataSourceHandle
materialPrim(bool diffuseFromPtex,
             bool strayPtex)
{
    const HdDataSourceBaseHandle toTexture = connectionTo("texture", "color");
    const HdContainerDataSourceHandle surfaceInputs = diffuseFromPtex
        ? HdRetainedContainerDataSource::New(TfToken("diffuseColor"), HdRetainedSmallVectorDataSource::New(1, &toTexture))
        : HdContainerDataSourceHandle();
    TfTokenVector names { TfToken("surface") };
    std::vector<HdDataSourceBaseHandle> nodes { node("UsdPreviewSurface", surfaceInputs) };
    if (diffuseFromPtex || strayPtex) {
        names.push_back(TfToken("texture"));
        nodes.push_back(node("HwPtexTexture_1"));
    }
    const HdContainerDataSourceHandle network = HdMaterialNetworkSchema::Builder()
                                                    .SetNodes(HdRetainedContainerDataSource::New(names.size(), names.data(), nodes.data()))
                                                    .SetTerminals(HdRetainedContainerDataSource::New(HdMaterialTerminalTokens->surface, connectionTo("surface", "surface")))
                                                    .Build();
    const TfToken context = HdMaterialSchemaTokens->universalRenderContext;
    const HdDataSourceBaseHandle networks = network;

    return HdRetainedContainerDataSource::New(HdMaterialSchema::GetSchemaToken(), HdMaterialSchema::BuildRetained(1, &context, &networks));
}

HdContainerDataSourceHandle
boundTo(const SdfPath& material)
{
    const TfToken purpose = HdMaterialBindingsSchemaTokens->allPurpose;
    const HdDataSourceBaseHandle binding = HdMaterialBindingSchema::Builder()
                                               .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(material))
                                               .Build();

    return HdRetainedContainerDataSource::New(HdMaterialBindingsSchema::GetSchemaToken(), HdMaterialBindingsSchema::BuildRetained(1, &purpose, &binding));
}

SdfPath
boundMaterial(const HdSceneIndexBaseRefPtr& scene,
              const SdfPath& path)
{
    const HdSceneIndexPrim prim = scene->GetPrim(path);
    const HdPathDataSourceHandle material = HdMaterialBindingsSchema::GetFromParent(prim.dataSource).GetMaterialBinding().GetPath();

    return material ? material->GetTypedValue(0.f) : SdfPath();
}

class BindingRecorder
    : public HdSceneIndexObserver {
public:
    void PrimsAdded(const HdSceneIndexBase& /*sender*/,
                    const AddedPrimEntries& /*entries*/) override
    {
    }

    void PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                      const RemovedPrimEntries& /*entries*/) override
    {
    }

    void PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                      const DirtiedPrimEntries& entries) override
    {
        for (const DirtiedPrimEntry& entry : entries) {
            if (entry.dirtyLocators.Intersects(HdMaterialBindingsSchema::GetDefaultLocator())) {
                rebound.insert(entry.primPath);
            }
        }
    }

    void PrimsRenamed(const HdSceneIndexBase& sender,
                      const RenamedPrimEntries& entries) override
    {
        ConvertPrimsRenamedToRemovedAndAdded(sender, entries, this);
    }

    std::set<SdfPath> rebound;
};

bool
ptexKnownToSdr()
{
    return bool(SdrRegistry::GetInstance().GetShaderNodeByIdentifier(TfToken("HwPtexTexture_1")));
}

} // anonymous namespace

TEST(StormPtexBindingFilter, DropsPtexBindingsOfCurvesAndPointsOnly)
{
    ASSERT_TRUE(ptexKnownToSdr());
    HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
    scene->AddPrims({ { kPtexMaterial, HdPrimTypeTokens->material, materialPrim(true, false) },
                      { kPlainMaterial, HdPrimTypeTokens->material, materialPrim(false, false) },
                      { kStrayPtexMaterial, HdPrimTypeTokens->material, materialPrim(false, true) },
                      { SdfPath("/curvesPtex"), HdPrimTypeTokens->basisCurves, boundTo(kPtexMaterial) },
                      { SdfPath("/pointsPtex"), HdPrimTypeTokens->points, boundTo(kPtexMaterial) },
                      { SdfPath("/meshPtex"), HdPrimTypeTokens->mesh, boundTo(kPtexMaterial) },
                      { SdfPath("/curvesPlain"), HdPrimTypeTokens->basisCurves, boundTo(kPlainMaterial) },
                      { SdfPath("/curvesStray"), HdPrimTypeTokens->basisCurves, boundTo(kStrayPtexMaterial) } });
    const HdSceneIndexBaseRefPtr filter = StormPtexBindingFilter::New(scene);

    EXPECT_TRUE(boundMaterial(filter, SdfPath("/curvesPtex")).IsEmpty());
    EXPECT_TRUE(boundMaterial(filter, SdfPath("/pointsPtex")).IsEmpty());
    EXPECT_EQ(boundMaterial(filter, SdfPath("/meshPtex")), kPtexMaterial);
    EXPECT_EQ(boundMaterial(filter, SdfPath("/curvesPlain")), kPlainMaterial);
    EXPECT_EQ(boundMaterial(filter, SdfPath("/curvesStray")), kStrayPtexMaterial);
}

TEST(StormPtexBindingFilter, RebindsWhenTheMaterialArrivesOrChanges)
{
    ASSERT_TRUE(ptexKnownToSdr());
    HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
    scene->AddPrims({ { SdfPath("/curves"), HdPrimTypeTokens->basisCurves, boundTo(kPtexMaterial) } });
    const HdSceneIndexBaseRefPtr filter = StormPtexBindingFilter::New(scene);
    BindingRecorder recorder;
    filter->AddObserver(HdSceneIndexObserverPtr(&recorder));

    EXPECT_EQ(boundMaterial(filter, SdfPath("/curves")), kPtexMaterial);

    scene->AddPrims({ { kPtexMaterial, HdPrimTypeTokens->material, materialPrim(true, false) } });
    EXPECT_EQ(recorder.rebound.count(SdfPath("/curves")), 1u);
    EXPECT_TRUE(boundMaterial(filter, SdfPath("/curves")).IsEmpty());

    recorder.rebound.clear();
    scene->AddPrims({ { kPtexMaterial, HdPrimTypeTokens->material, materialPrim(false, false) } });
    EXPECT_EQ(recorder.rebound.count(SdfPath("/curves")), 1u);
    EXPECT_EQ(boundMaterial(filter, SdfPath("/curves")), kPtexMaterial);

    recorder.rebound.clear();
    scene->AddPrims({ { kPtexMaterial, HdPrimTypeTokens->material, materialPrim(true, false) } });
    EXPECT_TRUE(boundMaterial(filter, SdfPath("/curves")).IsEmpty());
    recorder.rebound.clear();
    scene->RemovePrims({ { SdfPath("/Looks") } });
    EXPECT_EQ(recorder.rebound.count(SdfPath("/curves")), 1u);
    EXPECT_EQ(boundMaterial(filter, SdfPath("/curves")), kPtexMaterial);
}
