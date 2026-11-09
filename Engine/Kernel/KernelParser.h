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

#ifndef Engine_Kernel_KernelParser_h
#define Engine_Kernel_KernelParser_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <string_view>

#include "Global/GlobalDefines.h"

#include "Engine/Kernel/KernelBinding.h"
#include "Engine/Kernel/KernelLexer.h"
#include "Engine/Kernel/KernelTypes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Compiles a kernel snippet to the batched interpreter's bytecode.
 *
 * The language is a C-like, per-element snippet language:
 *
 *   types           int float vector2 vector vector4 matrix3 matrix string, and arrays of each (T name[])
 *   bindings        @name, with an optional type prefix f@ i@ u@ v@ p@ s@ 3@ 4@ (and f[]@ ... for
 *                   arrays); unprefixed well-known names (P N v Cd uv up orient id pscale name ...)
 *                   take their usual type, other unprefixed names are float. @opinputN_name reads
 *                   input N. Builtin variables (element numbers, counts, time) come from the binding.
 *   statements      declarations, = and compound assignment, ++ and --, if/else, for, foreach,
 *                   while, do/while, break, continue, return, blocks
 *   functions       [function] type name(type a, b; type c) { ... } at the top level. Parameters
 *                   are passed by reference unless declared const; recursion is an error.
 *   expressions     C operators and precedence, ?:, swizzles (.x .y .z .w, .r .g .b .a, .xyz ...),
 *                   v[i] and array[i], {1, 2, 3} literals, function-style casts such as vector(x)
 *
 * Implicit conversions: int to float, float to int (truncating), a scalar to any vector (every
 * component) and to a matrix (a scaled identity). Arithmetic between a scalar and a vector or matrix
 * applies componentwise; vector * matrix transforms a row vector, a vector by a 4x4 matrix as a
 * point. Functions whose overloads differ only in result type (noise, rand, ident and the
 * binding's readers) pick the one the context expects, float when nothing does.
 *
 * Every @ name and every function the core lacks resolves through binding, so this front end is free
 * of any context's data types. Returns false and fills error, with a 1-based line and column, on the
 * first error.
 **/
bool kernelCompile(std::string_view source, const KernelBinding& binding, KernelProgram& program,
                   KernelCompileError& error);

const char* kernelOpName(KernelOp op);

/**
 * @brief One instruction per line, with registers written f3, i0, s1, a2.
 **/
std::string kernelDisassemble(const KernelProgram& program);

NATRON_NAMESPACE_EXIT

#endif // Engine_Kernel_KernelParser_h
