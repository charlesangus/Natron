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

#include "USDSetAttributeLookup.h"

#include <algorithm>

#include <QString>

#include "Engine/Nodes/Scene/SceneOps.h"

NATRON_NAMESPACE_ENTER

namespace USDSetAttributeLookup {
namespace {
    const std::string kPrimvarsPrefix("primvars:");
} // anonymous namespace

const std::vector<std::string>&
getValueTypes()
{
    static const std::vector<std::string> types = {
        "bool",
        "int",
        "uint",
        "half",
        "float",
        "double",
        "float2",
        "float3",
        "float4",
        "double2",
        "double3",
        "double4",
        "color3f",
        "color4f",
        "color3d",
        "color4d",
        "point3f",
        "point3d",
        "normal3f",
        "normal3d",
        "vector3f",
        "vector3d",
        "texCoord2f",
        "texCoord2d",
        "string",
        "token",
        "asset",
    };

    return types;
}

std::string
getAttributeName(const std::string& name,
                 bool primvar)
{
    std::string trimmed = QString::fromStdString(name).trimmed().toStdString();

    if (primvar && (trimmed.compare(0, kPrimvarsPrefix.size(), kPrimvarsPrefix) == 0)) {
        trimmed = trimmed.substr(kPrimvarsPrefix.size());
    }

    return primvar ? (kPrimvarsPrefix + trimmed) : trimmed;
}

Result
lookup(const PXR_NS::UsdStageRefPtr& stage,
       const std::vector<PXR_NS::SdfPath>& prims,
       const std::string& name,
       bool primvar)
{
    Result result;
    const std::string attributeName = getAttributeName(name, primvar);

    if (!stage || attributeName.empty() || !PXR_NS::SdfPath::IsValidNamespacedIdentifier(attributeName)) {
        return result;
    }
    const PXR_NS::TfToken token(attributeName);
    PXR_NS::SdfValueTypeName firstType;
    for (const PXR_NS::SdfPath& path : prims) {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
        const PXR_NS::UsdAttribute attribute = prim ? prim.GetAttribute(token) : PXR_NS::UsdAttribute();
        if (!attribute) {
            continue;
        }
        const PXR_NS::SdfValueTypeName type = attribute.GetTypeName();
        if (!result.found) {
            result.found = true;
            firstType = type;
            result.primPath = path.GetString();
            const PXR_NS::SdfValueTypeName scalar = (primvar && type.IsArray()) ? type.GetScalarType() : type;
            result.typeName = scalar.GetAsToken().GetString();
            const std::vector<std::string>& types = getValueTypes();
            const std::vector<std::string>::const_iterator it = std::find(types.begin(), types.end(), result.typeName);
            const bool supported = (primvar == type.IsArray()) && (it != types.end());
            result.typeIndex = supported ? (int)(it - types.begin()) : -1;
            result.unsupported = !supported;
            if (result.unsupported) {
                result.typeName = type.GetAsToken().GetString();
            }
        } else if (type != firstType && !result.conflict) {
            result.conflict = true;
            result.conflictPath = path.GetString();
            result.conflictTypeName = type.GetAsToken().GetString();
        }
    }

    return result;
}

bool
lookupExistingAttribute(const PXR_NS::UsdStageRefPtr& stage,
                        const std::string& pattern,
                        const std::string& name,
                        bool primvar,
                        Result* result,
                        std::string* errorMessage)
{
    std::vector<PXR_NS::SdfPath> matches;

    *result = Result();
    if (!stage || !SceneOps::selectPrims(stage, pattern, &matches, errorMessage)) {
        return false;
    }
    *result = lookup(stage, matches, name, primvar);

    return true;
}
} // namespace USDSetAttributeLookup

NATRON_NAMESPACE_EXIT
