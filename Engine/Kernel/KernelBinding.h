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

#ifndef Engine_Kernel_KernelBinding_h
#define Engine_Kernel_KernelBinding_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Kernel/KernelTypes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A request to bind an @ reference. type comes from the prefix when typeExplicit is set,
 * otherwise from the language's table of well-known names, otherwise float. input is N for an
 * @opinputN_name reference and 0 otherwise.
 **/
struct KernelAttributeRequest {
    std::string name;
    std::int32_t input = 0;
    KernelType type;
    bool typeExplicit = false;
    bool write = false;
};

/** @brief A literal argument as the source writes it: type is eInt, eFloat or eString. */
struct KernelLiteral {
    KernelBaseType type = KernelBaseType::eVoid;
    std::int64_t intValue = 0;
    double floatValue = 0.;
    std::string text;
};

struct KernelBindResult {
    std::optional<KernelVariable> variable;
    // Reported at the reference when variable is empty.
    std::string error;
};

/**
 * @brief What a context (geometry, and later others) exposes to the kernel language. The front end
 * resolves every @ name and every function the language core does not provide through this
 * interface, so it never sees the context's data types. Which element class an attribute is read
 * from, and whether a missing attribute is created on write, is the provider's decision, recorded
 * in the returned variable's domain and id.
 **/
class KernelBinding {
public:
    virtual ~KernelBinding() = default;

    /**
     * @brief A read-only builtin variable such as an element number, an element count or the
     * frame, or nothing when name is not one.
     **/
    virtual std::optional<KernelVariable> builtinVariable(std::string_view name) const = 0;

    virtual KernelBindResult bindAttribute(const KernelAttributeRequest& request) const = 0;

    /**
     * @brief Appends the overloads of the function name the context provides, if any. Overloads
     * may differ only in their result type, in which case the type the caller expects picks one.
     **/
    virtual void functions(std::string_view name, std::vector<KernelFunctionSignature>& overloads) const = 0;

    /**
     * @brief Called for each argument of a context function call whose parameter is constant, once
     * the overload is chosen, so a context can resolve names its functions take (a parameter of
     * the node, say) when the kernel compiles rather than when it runs. A non-empty result is a
     * compile error reported at the argument.
     **/
    virtual std::string checkLiteralArgument(const KernelFunctionSignature& /*function*/,
                                             std::size_t /*argument*/,
                                             const KernelLiteral& /*value*/) const
    {
        return std::string();
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Kernel_KernelBinding_h
