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

#include <string>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "SceneTestEffect.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
authorIntAttribute(const SceneLayerEdit& edit,
                   PXR_NS::SdfSpecifier specifier,
                   const char* attributeName,
                   int value,
                   std::string* errorMessage)
{
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(edit.layer, PXR_NS::SdfPath(kSceneTestPrimPath));

    if (!prim) {
        *errorMessage = "could not create " kSceneTestPrimPath;

        return false;
    }
    prim->SetSpecifier(specifier);
    PXR_NS::SdfAttributeSpecHandle attribute = PXR_NS::SdfAttributeSpec::New(prim, attributeName, PXR_NS::SdfValueTypeNames->Int);
    if (!attribute) {
        *errorMessage = std::string("could not create ") + attributeName;

        return false;
    }
    attribute->SetDefaultValue(PXR_NS::VtValue(value));

    return true;
}
} // anonymous namespace

bool
sceneTestAuthorSource(const SceneLayerEdit& edit,
                      int value,
                      std::string* errorMessage)
{
    if (!authorIntAttribute(edit, PXR_NS::SdfSpecifierDef, kSceneTestSourceAttribute, value, errorMessage)) {
        return false;
    }
    edit.layer->SetDefaultPrim(PXR_NS::TfToken(std::string(kSceneTestPrimPath).substr(1)));

    return true;
}

bool
sceneTestAuthorOp(const SceneLayerEdit& edit,
                  int value,
                  std::string* errorMessage)
{
    return authorIntAttribute(edit, PXR_NS::SdfSpecifierOver, kSceneTestOpAttribute, value, errorMessage);
}

NATRON_NAMESPACE_EXIT
