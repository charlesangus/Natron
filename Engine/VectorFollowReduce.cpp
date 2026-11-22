/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "VectorFollowReduce.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

NATRON_NAMESPACE_ENTER

namespace {

class Reducer {
public:
    Reducer(VectorFollowKeyedMotion& motion,
            double tolerance,
            const std::function<bool()>& aborted)
        : _motion(motion)
        , _tolerance(tolerance)
        , _aborted(aborted)
        , _firstFrame(0)
        , _isAborted(false)
    {
    }

    std::set<int> run(const std::set<int>& removable)
    {
        std::set<int> removed;
        _keys = _motion.keys();
        if (_keys.size() < 2) {
            return removed;
        }
        recordReference();

        for (int key : removable) {
            if (_keys.count(key)) {
                _candidates.insert(key);
            }
        }
        std::map<int, double> costs;
        for (int key : _candidates) {
            costs[key] = costOfRemoving(key);
            if (_isAborted) {
                return removed;
            }
        }

        while ((_keys.size() > 1) && !costs.empty()) {
            std::map<int, double>::const_iterator best = costs.begin();
            for (std::map<int, double>::const_iterator it = costs.begin(); it != costs.end(); ++it) {
                if (it->second < best->second) {
                    best = it;
                }
            }
            if (best->second > _tolerance) {
                break;
            }
            const int key = best->first;
            std::vector<int> neighbours = neighboursOf(key);
            _motion.removeKey(key);
            _keys.erase(key);
            _candidates.erase(key);
            costs.erase(key);
            removed.insert(key);
            for (int neighbour : neighbours) {
                if (!_candidates.count(neighbour)) {
                    continue;
                }
                costs[neighbour] = costOfRemoving(neighbour);
                if (_isAborted) {
                    return removed;
                }
            }
        }

        return removed;
    }

private:
    void recordReference()
    {
        _firstFrame = *_keys.begin();
        const int lastFrame = *_keys.rbegin();
        _reference.resize(lastFrame - _firstFrame + 1);
        for (int frame = _firstFrame; frame <= lastFrame; ++frame) {
            _motion.evaluate(frame, &_reference[frame - _firstFrame]);
        }
    }

    // A key's tangents depend on its neighbours, so removing one changes the curve up to two keys away.
    std::vector<int> neighboursOf(int key) const
    {
        std::vector<int> neighbours;
        std::set<int>::const_iterator it = _keys.find(key);
        std::set<int>::const_iterator down = it;
        for (int i = 0; (i < kReach) && (down != _keys.begin()); ++i) {
            --down;
            neighbours.push_back(*down);
        }
        std::set<int>::const_iterator up = it;
        for (int i = 0; i < kReach; ++i) {
            ++up;
            if (up == _keys.end()) {
                break;
            }
            neighbours.push_back(*up);
        }

        return neighbours;
    }

    double costOfRemoving(int key)
    {
        if (_aborted()) {
            _isAborted = true;

            return std::numeric_limits<double>::max();
        }
        const std::vector<int> neighbours = neighboursOf(key);
        int from = key;
        int to = key;
        for (int neighbour : neighbours) {
            from = std::min(from, neighbour);
            to = std::max(to, neighbour);
        }
        if (from == *_keys.begin()) {
            from = _firstFrame;
        }
        if (to == *_keys.rbegin()) {
            to = _firstFrame + (int)_reference.size() - 1;
        }

        _motion.removeKey(key);
        double largest = 0.;
        std::vector<FollowPoint> points;
        for (int frame = from; frame <= to; ++frame) {
            _motion.evaluate(frame, &points);
            const std::vector<FollowPoint>& reference = _reference[frame - _firstFrame];
            for (std::size_t i = 0; i < points.size(); ++i) {
                largest = std::max(largest, std::hypot(points[i].x - reference[i].x, points[i].y - reference[i].y));
            }
        }
        _motion.restoreKey(key);

        return largest;
    }

    static constexpr int kReach = 2;

    VectorFollowKeyedMotion& _motion;
    const double _tolerance;
    const std::function<bool()>& _aborted;
    std::set<int> _keys;
    std::set<int> _candidates;
    std::vector<std::vector<FollowPoint>> _reference;
    int _firstFrame;
    bool _isAborted;
};

} // anonymous namespace

std::set<int>
reduceVectorFollowKeys(VectorFollowKeyedMotion& motion,
                       const std::set<int>& removable,
                       double tolerance,
                       const std::function<bool()>& aborted)
{
    return Reducer(motion, tolerance, aborted).run(removable);
}

NATRON_NAMESPACE_EXIT
