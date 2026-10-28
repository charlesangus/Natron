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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpContext_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpContext_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

#include "Engine/Curve.h"
#include "Engine/EngineFwd.h"
#include "Engine/RotoCurveContext.h"

NATRON_NAMESPACE_ENTER

namespace Serialization {
struct SplineWarpAnimatedValueSerialization;
}

/**
 * @brief What a curve does in the warp. A linked curve is the source or destination of a link; a
 * boundary is a closed curve outside of which nothing moves.
 **/
enum SplineWarpCurveRoleEnum {
    eSplineWarpCurveRoleUnlinked = 0,
    eSplineWarpCurveRoleLinked,
    eSplineWarpCurveRoleBoundary
};

/**
 * @brief A value that is static until it is keyed, as a knob dimension is. Not MT-safe on its
 * own: SplineWarpLink guards its values.
 **/
class SplineWarpAnimatedValue {
public:
    explicit SplineWarpAnimatedValue(double value = 0.);
    SplineWarpAnimatedValue(const SplineWarpAnimatedValue& other);
    SplineWarpAnimatedValue& operator=(const SplineWarpAnimatedValue& other);

    double getValueAt(double time) const;

    double getStaticValue() const
    {
        return _value;
    }

    bool isAnimated() const;

    /**
     * @brief Keys value at time when the value is animated, sets the static value otherwise.
     **/
    void setValue(double value, double time);

    /**
     * @brief Keys value at time, animating the value if it was not.
     **/
    void setValueAtTime(double time, double value);

    void removeKeyFrame(double time);

    std::vector<double> getKeyFrameTimes() const;

    /**
     * @brief Replaces the static value and every keyframe's value v by f(v).
     **/
    void mapValues(const std::function<double(double)>& f);

    void appendContentHash(ContentHasher& h) const;

    void toSerialization(Serialization::SplineWarpAnimatedValueSerialization* serialization) const;
    void fromSerialization(const Serialization::SplineWarpAnimatedValueSerialization& serialization);

private:
    double _value;
    Curve _curve;
};

/**
 * @brief A source-curve parameter paired with a destination-curve parameter. Parameters are those
 * of SplineWarpCurveShape: segment index plus Bezier t.
 **/
struct SplineWarpCorrespondence {
    SplineWarpAnimatedValue source;
    SplineWarpAnimatedValue destination;
};

/**
 * @brief A pair of curves of a SplineWarpContext. The destination is lerped toward the source by
 * the link's amount, and the warp maps destination positions to source positions.
 *
 * The alignment says where pairing starts on the destination: at parameter getStartOffset() of a
 * closed destination, walking backward when reversed; an open destination starts at its first
 * point, or at its last when reversed, and keeps a start offset of 0. The source always starts at
 * parameter 0 and walks forward.
 *
 * Read from any thread; edited through the context, on the main thread.
 **/
class SplineWarpLink {
public:
    SplineWarpLink(const BezierPtr& source,
                   const BezierPtr& destination);

    BezierPtr getSource() const;
    BezierPtr getDestination() const;

    double getStartOffset() const;
    bool isReversed() const;

    double getAmount(double time) const;
    SplineWarpAnimatedValue getAmountValue() const;

    int getCorrespondenceCount() const;

    /**
     * @brief Returns false if index is out of range.
     **/
    bool getCorrespondence(int index, double time, double* sourceParam, double* destinationParam) const;

    std::vector<SplineWarpCorrespondence> getCorrespondences() const;

private:
    friend class SplineWarpContext;

    mutable std::mutex _mutex;
    BezierWPtr _source;
    BezierWPtr _destination;
    double _startOffset;
    bool _reversed;
    SplineWarpAnimatedValue _amount;
    std::vector<SplineWarpCorrespondence> _correspondences;
};

typedef std::shared_ptr<SplineWarpLink> SplineWarpLinkPtr;

class SplineWarpContext;
typedef std::shared_ptr<SplineWarpContext> SplineWarpContextPtr;

/**
 * @brief The curve context of a SplineWarp node: its Beziers, plus the role of each curve and the
 * links between them. A curve is in at most one link, a boundary is never linked, and only a closed
 * curve can be a boundary. Every edit is made on the main thread; the queries are MT-safe.
 *
 * Correspondence parameters and start offsets follow control-point insertions and removals so they
 * keep their place on the curve. Deleting a curve deletes its link or its boundary role, and
 * opening a boundary curve clears its role.
 **/
class SplineWarpContext
    : public RotoCurveContext {
    struct MakeSharedEnabler;

protected:
    explicit SplineWarpContext(const NodePtr& node);

public:
    static SplineWarpContextPtr create(const NodePtr& node);

    virtual ~SplineWarpContext();

    SplineWarpCurveRoleEnum getRole(const BezierPtr& curve) const;

    bool isBoundary(const BezierPtr& curve) const;

    /**
     * @brief Throws std::invalid_argument when making a boundary of a curve that is open, linked or
     * not in this context.
     **/
    void setBoundary(const BezierPtr& curve, bool boundary);

    std::list<BezierPtr> getBoundaries() const;

    /**
     * @brief Links source to destination, aligned by computeSplineWarpAlignment() at the current
     * time, or at offset 0 and not reversed for a duplicate. Throws std::invalid_argument when
     * either curve is null, not in this context, a boundary or already linked, or when both are
     * the same curve.
     **/
    SplineWarpLinkPtr link(const BezierPtr& source, const BezierPtr& destination, bool duplicate = false);

    /**
     * @brief Same as link() with the given alignment.
     **/
    SplineWarpLinkPtr linkWithAlignment(const BezierPtr& source, const BezierPtr& destination, double startOffset, bool reversed);

    /**
     * @brief Removes the link curve is in, if any.
     **/
    void unlink(const BezierPtr& curve);

    void removeLink(const SplineWarpLinkPtr& link);

    SplineWarpLinkPtr getLink(const BezierPtr& curve) const;

    std::vector<SplineWarpLinkPtr> getLinks() const;

    /**
     * @brief Throws std::invalid_argument when startOffset is outside the destination's
     * parameter range.
     **/
    void setLinkAlignment(const SplineWarpLinkPtr& link, double startOffset, bool reversed);

    /**
     * @brief Makes link's destination its source and the reverse, swaps the two ends of every
     * correspondence, keys included, and sets the alignment on the new destination. Throws
     * std::invalid_argument when startOffset is outside that curve's parameter range.
     **/
    void swapLinkCurves(const SplineWarpLinkPtr& link, double startOffset, bool reversed);

    /**
     * @brief See SplineWarpAnimatedValue::setValue(). amount is clamped to [0, 1].
     **/
    void setLinkAmount(const SplineWarpLinkPtr& link, double amount, double time);
    void setLinkAmountAtTime(const SplineWarpLinkPtr& link, double time, double amount);

    /**
     * @brief Adds a static correspondence and returns its index. Throws std::invalid_argument when
     * a parameter is outside its curve's range.
     **/
    int addCorrespondence(const SplineWarpLinkPtr& link, double sourceParam, double destinationParam);

    /**
     * @brief See SplineWarpAnimatedValue::setValue(). Throws std::invalid_argument when index or a
     * parameter is out of range.
     **/
    void setCorrespondence(const SplineWarpLinkPtr& link, int index, double sourceParam, double destinationParam, double time);
    void setCorrespondenceAtTime(const SplineWarpLinkPtr& link, int index, double time, double sourceParam, double destinationParam);

    void removeCorrespondence(const SplineWarpLinkPtr& link, int index);

    /**
     * @brief The new parameter of a point at parameter u after addControlPointAfterIndex(afterIndex,
     * t) on a curve of pointCount points (before the insertion).
     **/
    static double paramAfterInsertion(double u, int pointCount, bool closed, int afterIndex, double t);

    /**
     * @brief The new parameter of a point at parameter u after removing the control point at index
     * from a curve of pointCount points (before the removal). The two segments around the point
     * merge into one, the first one taking firstWeight of its parameter range.
     **/
    static double paramAfterRemoval(double u, int pointCount, bool closed, int index, double firstWeight);

    virtual void onControlPointInserted(const BezierPtr& curve, int afterIndex, double t) OVERRIDE;
    virtual void onControlPointAboutToBeRemoved(const BezierPtr& curve, int index) OVERRIDE;
    virtual void onCurveFinishedChanged(const BezierPtr& curve) OVERRIDE;
    virtual void onItemRemoved(const RotoItemPtr& item) OVERRIDE;

    /**
     * @brief Saves the links (the same objects, with their values) and the boundaries.
     **/
    virtual RotoCurveContextHostStatePtr saveHostState() const OVERRIDE;
    virtual void restoreHostState(const RotoCurveContextHostStatePtr& state) OVERRIDE;

    virtual void toSerialization(Serialization::RotoContextSerialization* serialization) const OVERRIDE;
    virtual void fromSerialization(const Serialization::RotoContextSerialization& serialization) OVERRIDE;

protected:
    virtual void appendHostContentHash(ContentHasher& h) const OVERRIDE;

private:
    bool ownsCurve(const BezierPtr& curve) const;
    SplineWarpLinkPtr findLinkLocked(const Bezier* curve) const;
    void checkLinkable(const BezierPtr& curve, const char* what) const;
    void remapCurveParams(const BezierPtr& curve, const std::function<double(double)>& f);
    void warpDataChanged(bool evaluate);
    // Tells the panel the Role and Linked To columns may have changed.
    void rolesChanged();

    mutable std::mutex _warpMutex;
    std::vector<SplineWarpLinkPtr> _links;
    std::list<BezierWPtr> _boundaries;
    bool _loading;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpContext_h
