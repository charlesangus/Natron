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

#include "DeepImageKey.h"

NATRON_NAMESPACE_ENTER

DeepImageKey::DeepImageKey()
    : KeyHelper<U64>()
    , _nodeHashKey(0)
    , _time(0)
    , _view(0)
    , _mipmapLevel(0)
{
}

DeepImageKey::DeepImageKey(const CacheEntryHolder* holder,
                           U64 nodeHashKey,
                           double time,
                           ViewIdx view,
                           const RenderScale& scale)
    : KeyHelper<U64>(holder)
    , _nodeHashKey(nodeHashKey)
    , _time(time)
    , _view(view)
    , _mipmapLevel(scale.toMipmapLevel())
{
}

void
DeepImageKey::fillHash(Hash64* hash) const
{
    hash->append(_nodeHashKey);
    hash->append(_time);
    hash->append(_view);
    hash->append(_mipmapLevel);
}

bool
DeepImageKey::operator==(const DeepImageKey& other) const
{
    return _nodeHashKey == other._nodeHashKey && _time == other._time && _view == other._view && _mipmapLevel == other._mipmapLevel;
}

NATRON_NAMESPACE_EXIT
