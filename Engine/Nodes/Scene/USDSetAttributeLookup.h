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

#ifndef Engine_Nodes_Scene_USDSetAttributeLookup_h
#define Engine_Nodes_Scene_USDSetAttributeLookup_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace USDSetAttributeLookup {
// The Sdf value types USDSetAttribute's Value Type menu offers, in menu order.
const std::vector<std::string>& getValueTypes();

/**
 * @brief What the first matched prim holding the attribute says of it. found is false when no
 * matched prim holds it, authored or defined by its schema. typeName is that attribute's Sdf
 * type (a primvar's with its array dropped) and typeIndex its place in getValueTypes(), -1 when
 * unsupported (arrays, matrices...). conflict is set when another matched prim holds it with a
 * different type, named by conflictPath and conflictTypeName.
 **/
struct Result {
    bool found = false;
    std::string typeName;
    int typeIndex = -1;
    bool unsupported = false;
    std::string primPath;
    bool conflict = false;
    std::string conflictPath;
    std::string conflictTypeName;
};

/**
 * @brief The attribute name on a prim: name as given, or primvars:<name> for a primvar, whose
 * name may already carry the prefix.
 **/
std::string getAttributeName(const std::string& name,
                             bool primvar);

/**
 * @brief The result for the attribute on prims, taken in the order given. The Sdf type of a value
 * does not depend on time, so no time is taken.
 **/
Result lookup(const PXR_NS::UsdStageRefPtr& stage,
              const std::vector<PXR_NS::SdfPath>& prims,
              const std::string& name,
              bool primvar);

/**
 * @brief lookup() on the prims pattern selects. False, with *errorMessage set, for a pattern
 * that does not parse.
 **/
bool lookupExistingAttribute(const PXR_NS::UsdStageRefPtr& stage,
                             const std::string& pattern,
                             const std::string& name,
                             bool primvar,
                             Result* result,
                             std::string* errorMessage);
} // namespace USDSetAttributeLookup

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDSetAttributeLookup_h
