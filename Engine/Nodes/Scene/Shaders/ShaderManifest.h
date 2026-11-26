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

#ifndef Engine_Nodes_Scene_Shaders_ShaderManifest_h
#define Engine_Nodes_Scene_Shaders_ShaderManifest_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <QByteArray>

#include "Engine/Nodes/Network/NetworkNode.h"

// The menu path of each renderer's shaders.
#define kShaderGroupCycles "Shader/Cycles"
#define kShaderGroupMoonRay "Shader/MoonRay"

// The target name of each renderer, as network layouts and builders list it.
#define kShaderTargetCycles "cycles"
#define kShaderTargetMoonRay "moonray"

NATRON_NAMESPACE_ENTER

/**
 * @brief The label of a knob or node from its script name: words split at '_', '.', a lower to
 * upper case change and a letter to digit change, each capitalised, known acronyms upper case
 * (base_color is "Base Color", ior is "IOR").
 **/
std::string shaderLabelFromScriptName(const std::string& scriptName);

/**
 * @brief The label of a choice option from its manifest name. A name with anything but letters,
 * digits, '_' and ' ' is its own label, as is any word with a capital in it, so signs, ranges
 * and names such as sRGB survive. Other words, split at '_', ' ' and a capital after two lower
 * case letters, are capitalised, and known acronyms upper case.
 **/
std::string shaderChoiceLabel(const std::string& name);

/**
 * @brief Builds one layout per node of a renderer's manifest and its layout file (the page
 * groups and key ports, keyed like the manifest's nodes). The renderer is the manifest's
 * "renderer". Every attribute with a value is a knob; a node with key ports starts with those
 * as inputs, one without starts with every linkable attribute, and port-only attributes are
 * always inputs. False, with *error set, when either document cannot be read.
 **/
bool parseShaderManifest(const QByteArray& manifest,
                         const QByteArray& layout,
                         std::vector<NetworkNodeLayout>* layouts,
                         std::string* error);

/**
 * @brief Registers the type table and the stripe colour of each renderer the manifests cover.
 * Main thread.
 **/
void registerShaderPortTypeTables();

/**
 * @brief Registers a plug-in for every node of the built-in manifests, once, and returns how many
 * are registered. A node's layout is built when the first node of it is: a manifest error in one
 * node is reported then, and that node has no layout. Main thread.
 **/
std::size_t registerBuiltinShaderPlugins();

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Shaders_ShaderManifest_h
