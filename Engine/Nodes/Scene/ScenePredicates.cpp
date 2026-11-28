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

#include "ScenePredicates.h"

#include <utility>

NATRON_NAMESPACE_ENTER

namespace ScenePredicates {
const PXR_NS::UsdObjectPredicateLibrary&
library()
{
    static const PXR_NS::UsdObjectPredicateLibrary lib = []() {
        PXR_NS::UsdObjectPredicateLibrary result = PXR_NS::UsdGetCollectionPredicateLibrary();
        auto purposeIs = [](const PXR_NS::UsdObject& object, const std::string& purpose) {
            return hasPurpose(object.GetPrim(), PXR_NS::TfToken(purpose));
        };
        result.Define("purpose", std::move(purposeIs), { "purpose" });

        return result;
    }();

    return lib;
}

bool
hasPurpose(const PXR_NS::UsdPrim& prim,
           const PXR_NS::TfToken& purpose)
{
    if (!prim || !prim.IsA<PXR_NS::UsdGeomImageable>()) {
        return false;
    }

    return PXR_NS::UsdGeomImageable(prim).ComputePurposeInfo().purpose == purpose;
}

bool
link(const PXR_NS::SdfPathExpression& expression,
     const PXR_NS::UsdObjectPredicateLibrary& library,
     PathEval* eval,
     std::string* errorMessage)
{
    std::string error;
    {
        PXR_NS::TfErrorMark mark;
        *eval = PXR_NS::SdfMakePathExpressionEval(expression, library);
        for (PXR_NS::TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
            if (!error.empty()) {
                error += "; ";
            }
            error += it->GetCommentary();
        }
        mark.Clear();
    }
    if (!error.empty() || (eval->IsEmpty() && !expression.IsEmpty())) {
        *eval = PathEval();
        *errorMessage = error.empty() ? std::string("a predicate is unknown or takes other arguments") : error;

        return false;
    }

    return true;
}
} // namespace ScenePredicates

NATRON_NAMESPACE_EXIT
