// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/usd/usdGeom/sphere.h>

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/USDSetAttributeLookup.h"

NATRON_NAMESPACE_USING

namespace {
using USDSetAttributeLookup::Result;

PXR_NS::UsdStageRefPtr
makeStage()
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
    PXR_NS::UsdGeomSphere::Define(stage, PXR_NS::SdfPath("/World/Ball"));
    PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/World/A"));
    PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/World/B"));
    stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball")).CreateAttribute(PXR_NS::TfToken("m:tint"), PXR_NS::SdfValueTypeNames->Color3f, true);
    stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball")).CreateAttribute(PXR_NS::TfToken("m:points"), PXR_NS::SdfValueTypeNames->Point3fArray, true);
    stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball")).CreateAttribute(PXR_NS::TfToken("primvars:foo"), PXR_NS::SdfValueTypeNames->FloatArray, true);
    stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A")).CreateAttribute(PXR_NS::TfToken("m:shared"), PXR_NS::SdfValueTypeNames->Float, true);
    stage->GetPrimAtPath(PXR_NS::SdfPath("/World/B")).CreateAttribute(PXR_NS::TfToken("m:shared"), PXR_NS::SdfValueTypeNames->Int, true);

    return stage;
}

Result
find(const PXR_NS::UsdStageRefPtr& stage,
     const std::string& pattern,
     const std::string& name,
     bool primvar)
{
    Result result;
    std::string error;

    EXPECT_TRUE(USDSetAttributeLookup::lookupExistingAttribute(stage, pattern, name, primvar, &result, &error)) << error;

    return result;
}
} // anonymous namespace

TEST(USDSetAttributeLookup, AnAuthoredCustomAttributeGivesItsType)
{
    PXR_NS::UsdStageRefPtr stage = makeStage();
    const Result result = find(stage, "/World/Ball", "m:tint", false);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(std::string("color3f"), result.typeName);
    EXPECT_EQ(std::string("/World/Ball"), result.primPath);
    EXPECT_FALSE(result.unsupported);
    EXPECT_FALSE(result.conflict);
    ASSERT_GE(result.typeIndex, 0);
    EXPECT_EQ(std::string("color3f"), USDSetAttributeLookup::getValueTypes()[result.typeIndex]);
}

TEST(USDSetAttributeLookup, ASchemaAttributeGivesItsType)
{
    PXR_NS::UsdStageRefPtr stage = makeStage();
    const Result result = find(stage, "//{isa:Sphere}", "radius", false);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(std::string("double"), result.typeName);
    EXPECT_FALSE(result.unsupported);
}

TEST(USDSetAttributeLookup, APrimvarGivesItsScalarType)
{
    PXR_NS::UsdStageRefPtr stage = makeStage();
    const Result result = find(stage, "/World/Ball", "foo", true);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(std::string("float"), result.typeName);
    EXPECT_FALSE(result.unsupported);

    const Result schemaPrimvar = find(stage, "/World/Ball", "primvars:displayColor", true);
    EXPECT_TRUE(schemaPrimvar.found);
    EXPECT_EQ(std::string("color3f"), schemaPrimvar.typeName);
}

TEST(USDSetAttributeLookup, AnArrayAttributeIsUnsupported)
{
    PXR_NS::UsdStageRefPtr stage = makeStage();
    const Result result = find(stage, "/World/Ball", "m:points", false);

    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.unsupported);
    EXPECT_EQ(-1, result.typeIndex);
    EXPECT_EQ(std::string("point3f[]"), result.typeName);
}

TEST(USDSetAttributeLookup, ConflictingTypesTakeTheFirstMatchAndFlagTheConflict)
{
    PXR_NS::UsdStageRefPtr stage = makeStage();
    const Result result = find(stage, "/World/A /World/B", "m:shared", false);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(std::string("float"), result.typeName);
    EXPECT_EQ(std::string("/World/A"), result.primPath);
    EXPECT_TRUE(result.conflict);
    EXPECT_EQ(std::string("int"), result.conflictTypeName);
    EXPECT_EQ(std::string("/World/B"), result.conflictPath);
}

TEST(USDSetAttributeLookup, AMissingAttributeIsNotFound)
{
    PXR_NS::UsdStageRefPtr stage = makeStage();

    EXPECT_FALSE(find(stage, "/World/Ball", "m:nothing", false).found);
    EXPECT_FALSE(find(stage, "/World/Nothing", "m:tint", false).found);
}
