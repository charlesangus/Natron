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

#include "USDLayerEnd.h"

#include "Engine/Nodes/Scene/SceneOps.h"

NATRON_NAMESPACE_ENTER

USDLayerEnd::USDLayerEnd(NodePtr node)
    : NativeEffectBase(node)
{
}

USDLayerEnd::~USDLayerEnd()
{
}

const NodeSpec*
USDLayerEnd::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

StatusEnum
USDLayerEnd::renderScene(const SceneRenderActionArgs& args,
                         ScenePayload* outputScene,
                         std::string* /*errorMessage*/)
{
    const ScenePayload input = args.getInputScene(0);

    if (input.getNumOpenRegions() == 0) {
        setPersistentMessage(eMessageTypeWarning, tr("No layer region is open here: this USDLayerEnd closes nothing.").toStdString());
        *outputScene = input;

        return eStatusOK;
    }
    clearPersistentMessage(false);
    *outputScene = SceneOps::closeRegion(input);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
