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

#ifndef Engine_Kernel_KernelLexer_h
#define Engine_Kernel_KernelLexer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A compile error. line and column are 1-based; the column counts bytes.
 **/
struct KernelCompileError {
    int line = 0;
    int column = 0;
    std::string message;
};

struct KernelToken {
    enum class Kind : std::uint8_t {
        eEnd,
        eIdentifier,
        eInt,
        eFloat,
        eString,
        // An @ reference. text is the name; prefix is the type prefix as written ("f", "v[]", ...),
        // empty when there is none.
        eBinding,
        ePunct,
    };

    Kind kind = Kind::eEnd;
    std::string text;
    std::string prefix;
    std::int64_t intValue = 0;
    double floatValue = 0.;
    int line = 1;
    int column = 1;

    bool isPunct(std::string_view p) const { return kind == Kind::ePunct && text == p; }
    bool isIdentifier(std::string_view name) const { return kind == Kind::eIdentifier && text == name; }
};

/**
 * @brief Splits source into tokens ending with an eEnd token. Returns false and fills error on a
 * malformed token.
 **/
bool kernelLex(std::string_view source, std::vector<KernelToken>& tokens, KernelCompileError& error);

NATRON_NAMESPACE_EXIT

#endif // Engine_Kernel_KernelLexer_h
