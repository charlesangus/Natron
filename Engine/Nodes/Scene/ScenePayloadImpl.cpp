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

#include "ScenePayloadImpl.h"

NATRON_NAMESPACE_ENTER

ScenePayload::ScenePayload()
    : _imp()
{
}

ScenePayload::ScenePayload(const std::shared_ptr<const ScenePayloadImpl>& imp)
    : _imp(imp)
{
}

ScenePayload::~ScenePayload()
{
}

bool
ScenePayload::isEmpty() const
{
    return !_imp;
}

std::size_t
ScenePayload::getNumLayers() const
{
    return _imp ? _imp->numLayers : 0;
}

U64
ScenePayload::getStackHash() const
{
    return _imp ? _imp->stackHash : 0;
}

const SceneMetadata&
ScenePayload::getMetadata() const
{
    static const SceneMetadata fallback;

    return _imp ? _imp->metadata : fallback;
}

bool
ScenePayload::isSameStack(const ScenePayload& other) const
{
    const ScenePayloadImpl* a = _imp.get();
    const ScenePayloadImpl* b = other._imp.get();

    // Stacks that share a base share everything below it, so the walk stops at the first
    // common link rather than at the bottom.
    while (a != b) {
        if (!a || !b || (a->numLayers != b->numLayers) || (a->top.layer != b->top.layer)) {
            return false;
        }
        a = a->base.get();
        b = b->base.get();
    }

    return true;
}

NATRON_NAMESPACE_EXIT
