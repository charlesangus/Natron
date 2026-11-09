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

#ifndef Engine_VectorFollowBake_h
#define Engine_VectorFollowBake_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/VectorFollow.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Points a follow moves one by one, with no curve of their own to key, such as the two ends
 * of a SplineWarp correspondence. They always follow per point, whatever the model, after every
 * curve of the same follow, so they are written over the curves' new keys. Main thread only.
 **/
class VectorFollowPoints {
public:
    virtual ~VectorFollowPoints() { }

    // What the follow's report calls them.
    virtual std::string getName() const = 0;

    // Where they show at time, in the coordinates the vectors are read in.
    virtual std::vector<FollowPoint> getPointsAtTime(double time) const = 0;

    // The integer frames from first to last at which they are keyed.
    virtual std::set<int> getKeyFrames(int first, int last) const = 0;

    // Keys them at time where the follow moved them, as near as they can go.
    virtual void setPointsAtTime(double time, const std::vector<FollowPoint>& points) = 0;
};

typedef std::shared_ptr<VectorFollowPoints> VectorFollowPointsPtr;

struct VectorFollowParams {
    std::list<RotoDrawableItemPtr> items;
    std::list<VectorFollowPointsPtr> points;
    // Control points by index, for a curve of which only some were selected: those follow one by
    // one and the rest stays put, whatever the model. A curve not listed follows as a whole.
    std::map<RotoDrawableItem*, std::set<int>> selectedPoints;
    NodePtr source;
    int first = 0;
    int last = 0;
    VectorFollowDirection direction = VectorFollowDirection::eForward;
    VectorFollowModel model = VectorFollowModel::ePerPoint;
    bool reduce = false;
    double tolerance = kVectorFollowDefaultTolerance;
    double confidenceThreshold = kVectorFollowDefaultConfidenceThreshold;
    // Empty when nothing can stop it. Any thread.
    std::function<bool()> shouldAbort;
};

/**
 * @brief One follow, in steps that keep the writes to the shapes on the main thread and the reading
 * of vectors off it. Everything the off-thread steps need is copied by the main-thread step before.
 **/
class VectorFollowRun {
public:
    VectorFollowRun(const RotoCurveContextPtr& context,
                    const VectorFollowParams& params);

    ~VectorFollowRun();

    VectorFollowRun(const VectorFollowRun&) = delete;
    VectorFollowRun& operator=(const VectorFollowRun&) = delete;

    // Main thread. False, with the reason in error, when there is nothing to do or the source is refused.
    bool prepare(std::string* error);

    // Any thread. Gives the consumer each shape's frames as they are final, by index of the shape.
    void compute(VectorField& field,
                 const std::function<void(int, const VectorFollowFrame&)>& consumer,
                 const std::function<bool()>& aborted);

    // Whether compute() stopped early.
    bool wasAborted() const;

    // Main thread.
    void applyFrame(int shape, const VectorFollowFrame& frame);

    // Main thread, then any thread, then main thread. Call after every frame was applied.
    void prepareReduction();
    void reduce(const std::function<bool()>& aborted);
    void applyReduction();

    /**
     * @brief Main thread. The whole follow becomes one undo step, pushed on the node, or handed over
     * in undo when given. With sourceLost it is reverted instead, and there is no undo step.
     **/
    VectorFollowReport finish(bool sourceLost,
                              const std::string& error,
                              UndoCommandPtr* undo = 0);

private:
    struct Imp;
    std::unique_ptr<Imp> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_VectorFollowBake_h
