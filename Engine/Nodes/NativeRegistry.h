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

#ifndef Engine_Nodes_NativeRegistry_h
#define Engine_Nodes_NativeRegistry_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/NativeEffectBase.h"

/**
 * @brief The registry of native nodes. Each native node has a registration fragment beside its
 * class, Engine/Nodes/<Family>/<Node>.reg.inc, holding the single line
 *
 *     NATRON_NATIVE_NODE(Class, "Header.h")
 *
 * where Class is the registered class (the full instantiation for a templated node, so a
 * template registered under several IDs has one fragment per ID) and "Header.h" is the class's
 * header, relative to the fragment. Everything else the registry needs -- ID, label, grouping,
 * versions, icon and the deprecated and internal-use-only flags -- comes from the class's
 * NodeSpec. An I-node's fragment also holds a NATRON_I_VARIANT line, which this registry ignores
 * and IVariantRegistry collects.
 *
 * Engine/CMakeLists.txt globs the fragments, sorted by path, into the generated
 * NativeRegistryAll.inc. Included with NATRON_NATIVE_REGISTRY_HEADERS defined, as below, it
 * includes every fragment's header; included without, inside
 * AppManager::loadBuiltinNodePlugins(), it includes every fragment, each of which registers its
 * class. Registration is explicit rather than done by static initialisers because the linker
 * drops the object files of a static library that nothing references, and their nodes with them.
 *
 * Only AppManager.cpp includes this header.
 **/
#define NATRON_NATIVE_REGISTRY_HEADERS
#include "NativeRegistryAll.inc"
#undef NATRON_NATIVE_REGISTRY_HEADERS

#define NATRON_NATIVE_NODE(CLASS, HEADER) registerNativePlugin<CLASS>();

#endif // Engine_Nodes_NativeRegistry_h
