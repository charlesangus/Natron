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

#include "Serialization/Merge/MergeHandler.h"

#include <map>
#include <set>
#include <tuple>

#include "Serialization/Merge/MergeRules.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

using MergeDetail::Pick;
using MergeDetail::pick;

namespace {

    // The values of Engine's KeyframeTypeEnum whose tangents the user sets; every other
    // interpolation recomputes its tangents from the neighbouring keys.
    constexpr int kKeyframeTypeFree = 6;
    constexpr int kKeyframeTypeBroken = 7;

    typedef std::tuple<double, int, double, double, double, double> KeyState;

    // A key's own state. Recomputed tangents are left out, or editing one key would count as a
    // change to its neighbours.
    std::optional<KeyState>
    keyState(const KeyFrameSerialization* key)
    {
        if (!key) {
            return std::nullopt;
        }
        if ((key->interpolation == kKeyframeTypeFree) || (key->interpolation == kKeyframeTypeBroken)) {
            return KeyState(key->value, key->interpolation, key->leftDerivative, key->rightDerivative, key->leftWeight, key->rightWeight);
        }

        return KeyState(key->value, key->interpolation, 0., 0., 0., 0.);
    }

    std::map<double, const KeyFrameSerialization*>
    keysByTime(const CurveSerialization& curve)
    {
        std::map<double, const KeyFrameSerialization*> result;
        for (const KeyFrameSerialization& key : curve.keyframes) {
            result[key.time] = &key;
        }

        return result;
    }

    const KeyFrameSerialization*
    keyAt(const std::map<double, const KeyFrameSerialization*>& keys,
          double time)
    {
        std::map<double, const KeyFrameSerialization*>::const_iterator found = keys.find(time);

        return (found == keys.end()) ? nullptr : found->second;
    }

    std::optional<KeyFrameSerialization>
    copyOf(const KeyFrameSerialization* key)
    {
        return key ? std::optional<KeyFrameSerialization>(*key) : std::nullopt;
    }

} // namespace

CurveSerialization
mergeCurves(const CurveSerialization& base,
            const CurveSerialization& mine,
            const CurveSerialization& theirs,
            std::vector<CurveKeyConflict>* conflicts)
{
    const std::map<double, const KeyFrameSerialization*> baseKeys = keysByTime(base);
    const std::map<double, const KeyFrameSerialization*> mineKeys = keysByTime(mine);
    const std::map<double, const KeyFrameSerialization*> theirsKeys = keysByTime(theirs);
    std::set<double> times;
    for (const std::map<double, const KeyFrameSerialization*>* keys : { &baseKeys, &mineKeys, &theirsKeys }) {
        for (const std::pair<const double, const KeyFrameSerialization*>& entry : *keys) {
            times.insert(entry.first);
        }
    }

    CurveSerialization result;
    for (double time : times) {
        const KeyFrameSerialization* b = keyAt(baseKeys, time);
        const KeyFrameSerialization* m = keyAt(mineKeys, time);
        const KeyFrameSerialization* t = keyAt(theirsKeys, time);
        const Pick choice = pick(keyState(b), keyState(m), keyState(t));
        if (choice == Pick::eConflict) {
            CurveKeyConflict conflict;
            conflict.time = time;
            conflict.base = copyOf(b);
            conflict.mine = copyOf(m);
            conflict.theirs = copyOf(t);
            conflicts->push_back(conflict);
        }
        const KeyFrameSerialization* chosen = (choice == Pick::eTheirs) ? t : m;
        if (chosen) {
            result.keyframes.push_back(*chosen);
        }
    }

    return result;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
