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

#ifndef DEEPIMAGEKEY_H
#define DEEPIMAGEKEY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/KeyHelper.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Identifies a Cache<DeepImageCacheEntry> entry: (nodeHash, time, view, scale). Deliberately
 * excludes bounds -- those live in DeepImageParams instead, so a future tiling scheme can vary
 * bounds under the same key without changing what identifies a render.
 **/
class DeepImageKey
    : public KeyHelper<U64> {
public:
    U64 _nodeHashKey;
    double _time;
    int /*ViewIdx*/ _view; // store it locally as an int for easier serialization
    unsigned int _mipmapLevel;

    DeepImageKey();

    DeepImageKey(const CacheEntryHolder* holder,
                 U64 nodeHashKey,
                 double time,
                 ViewIdx view,
                 const RenderScale& scale);

    void fillHash(Hash64* hash) const;

    U64 getTreeVersion() const
    {
        return _nodeHashKey;
    }

    bool operator==(const DeepImageKey& other) const;

    double getTime() const
    {
        return _time;
    }

    ViewIdx getView() const
    {
        return ViewIdx(_view);
    }

    RenderScale getRenderScale() const
    {
        return RenderScale::fromMipmapLevel(_mipmapLevel);
    }
};

NATRON_NAMESPACE_EXIT

#endif // DEEPIMAGEKEY_H
