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

#ifndef GEODETAILKEY_H
#define GEODETAILKEY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/KeyHelper.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Identifies a Cache<GeoDetailCacheEntry> entry: (nodeHash, time, view). There is no render
 * scale and no bounds, because geometry is pulled whole.
 *
 * Time is part of the identity only when something upstream varies with it. The caller decides
 * that and passes canonicalTime(), so a time-invariant chain maps every frame to one entry.
 **/
class GeoDetailKey
    : public KeyHelper<U64> {
public:
    U64 _nodeHashKey;
    double _time;
    int /*ViewIdx*/ _view; // store it locally as an int for easier serialization

    GeoDetailKey();

    GeoDetailKey(const CacheEntryHolder* holder,
                 U64 nodeHashKey,
                 double time,
                 ViewIdx view);

    /** @brief `time` when the chain varies with time, otherwise one fixed value for every frame. */
    static double canonicalTime(double time,
                                bool timeVaries)
    {
        return timeVaries ? time : 0.;
    }

    void fillHash(Hash64* hash) const;

    U64 getTreeVersion() const
    {
        return _nodeHashKey;
    }

    bool operator==(const GeoDetailKey& other) const;

    double getTime() const
    {
        return _time;
    }

    ViewIdx getView() const
    {
        return ViewIdx(_view);
    }
};

NATRON_NAMESPACE_EXIT

#endif // GEODETAILKEY_H
