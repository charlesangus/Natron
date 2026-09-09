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

#ifndef DEEPIMAGEPARAMS_H
#define DEEPIMAGEPARAMS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/NonKeyParams.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Non-identifying data that goes along a DeepImageKey: currently just the bounds a
 * DeepImageCacheEntry was rendered over. Kept out of the key so bounds can grow for the same
 * (nodeHash, time, view, scale) without minting a new cache identity -- the "bounds growth, not
 * tiling" model.
 *
 * DeepImageCacheEntry does not use NonKeyParams' CacheEntryStorageInfo to describe this
 * DeepImage's real payload (that payload is variable-size, structured as a sample table plus a
 * set of per-channel buffers, not a fixed-stride typed buffer). The storage info here is fixed
 * to a trivial 1-byte RAM allocation so that CacheEntryHelper's own allocate-once/deallocate
 * bookkeeping (which the RAM path depends on) works normally; see DeepImageCacheEntry.
 **/
class DeepImageParams
    : public NonKeyParams {
public:
    DeepImageParams();

    DeepImageParams(const DeepImageParams& other);

    explicit DeepImageParams(const RectI& bounds);

    virtual ~DeepImageParams()
    {
    }

    const RectI& getBounds() const
    {
        return _bounds;
    }

    void setBounds(const RectI& bounds)
    {
        _bounds = bounds;
    }

    bool operator==(const DeepImageParams& other) const
    {
        return _bounds == other._bounds;
    }

    bool operator!=(const DeepImageParams& other) const
    {
        return !(*this == other);
    }

private:
    RectI _bounds;
};

NATRON_NAMESPACE_EXIT

#endif // DEEPIMAGEPARAMS_H
