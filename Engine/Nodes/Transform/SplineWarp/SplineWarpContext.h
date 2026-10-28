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
#include <string>
#include <utility>
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
 * (hard) boundary is a closed curve outside of which nothing moves; a soft boundary, open or
 * closed, holds the image still along itself and masks nothing.
 **/
enum SplineWarpCurveRoleEnum {
    eSplineWarpCurveRoleUnlinked = 0,
    eSplineWarpCurveRoleLinked,
    eSplineWarpCurveRoleBoundary,
    eSplineWarpCurveRoleSoftBoundary
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

    const Curve& getCurve() const
    {
        return _curve;
    }

    /**
     * @brief Makes the static value value and the keyframes those of curve.
     **/
    void assign(double value, const Curve& curve);

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
 * @brief The values of the Transform page's knobs, one per dimension of translate, rotate, scale,
 * uniform, skewx, skewy, skewOrder, center and extraMatrix, in that order. A layer holds one; a
 * curve holds the same values in its own knobs. Not MT-safe on its own.
 **/
class SplineWarpTransform {
public:
    static const int kValueCount = 20;

    /**
     * @brief The identity, centred at the origin.
     **/
    SplineWarpTransform();

    /**
     * @brief The transform knobs' script names, in value order.
     **/
    static const std::vector<std::string>& knobNames();

    /**
     * @brief The index of the first value of the knob called name, or -1 when it is not a
     * transform knob. *dimensions gets the knob's dimension count.
     **/
    static int indexOf(const std::string& name, int* dimensions);

    SplineWarpAnimatedValue& value(int index)
    {
        return _values[index];
    }

    const SplineWarpAnimatedValue& value(int index) const
    {
        return _values[index];
    }

    /**
     * @brief The matrix the values make at time, as RotoDrawableItem::getLocalTransformAtTime().
     **/
    void getMatrixAt(double time, Transform::Matrix3x3* matrix) const;

    /**
     * @brief Takes the values, keyframes included, of the knobs of knobs named as transform knobs.
     **/
    void readKnobs(const std::list<KnobIPtr>& knobs);

    /**
     * @brief Gives the knobs of knobs named as transform knobs these values and keyframes.
     **/
    void writeKnobs(const std::list<KnobIPtr>& knobs) const;

    void appendContentHash(ContentHasher& h) const;

private:
    SplineWarpAnimatedValue _values[kValueCount];
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
 * Each layer has an amount, 1 by default, and a link's amount is multiplied by those of every
 * layer holding its destination curve.
 *
 * Correspondence parameters and start offsets follow control-point insertions and removals so they
 * keep their place on the curve. Deleting a curve deletes its link, its boundary role and its cookie
 * cutter flag; opening a curve clears its hard boundary role and its cookie cutter flag.
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

    bool isSoftBoundary(const BezierPtr& curve) const;

    /**
     * @brief Throws std::invalid_argument when making a soft boundary of a curve that is linked or
     * not in this context. Making a curve one kind of boundary clears the other kind.
     **/
    void setSoftBoundary(const BezierPtr& curve, bool softBoundary);

    std::list<BezierPtr> getSoftBoundaries() const;

    /**
     * @brief A cookie cutter cuts the Source to its shape and makes the output mask, whatever its
     * role. Throws std::invalid_argument when making a cookie cutter of a curve that is open or not
     * in this context.
     **/
    bool isCookieCutter(const BezierPtr& curve) const;
    void setCookieCutter(const BezierPtr& curve, bool cookieCutter);
    std::list<BezierPtr> getCookieCutters() const;

    /**
     * @brief Whether a cookie cutter is active at time (deactivated curves are ignored).
     **/
    bool hasActiveCookieCutter(double time) const;

    /**
     * @brief The layer's own amount at time, 1 unless set.
     **/
    double getLayerAmount(const RotoLayerPtr& layer, double time) const;
    SplineWarpAnimatedValue getLayerAmountValue(const RotoLayerPtr& layer) const;

    /**
     * @brief The product of the amounts of every layer holding item, at time.
     **/
    double getInheritedLayerAmount(const RotoItemPtr& item, double time) const;

    /**
     * @brief See SplineWarpAnimatedValue::setValue(). amount is clamped to [0, 1]. Throws
     * std::invalid_argument when layer is not in this context.
     **/
    void setLayerAmount(const RotoLayerPtr& layer, double amount, double time);
    void setLayerAmountAtTime(const RotoLayerPtr& layer, double time, double amount);

    /**
     * @brief Gives every layer of layers the static value value and the keyframes of curve, as a
     * node knob holds them.
     **/
    void assignLayerAmounts(const std::list<RotoLayerPtr>& layers, double value, const Curve& curve);

    /**
     * @brief The layer's own transform, which applies on top of its children's: the identity
     * centred at getDefaultTransformCenter() unless set.
     **/
    SplineWarpTransform getLayerTransform(const RotoLayerPtr& layer) const;

    /**
     * @brief Throws std::invalid_argument when layer is not in this context.
     **/
    void setLayerTransform(const RotoLayerPtr& layer, const SplineWarpTransform& transform);
    void assignLayerTransforms(const std::list<RotoLayerPtr>& layers, const SplineWarpTransform& transform);

    /**
     * @brief The curve or layer whose transform item takes in place of its own, null when none.
     * The parent layers of item still apply on top.
     **/
    RotoItemPtr getTransformLink(const RotoItemPtr& item) const;

    /**
     * @brief The items whose transform link is target.
     **/
    std::list<RotoItemPtr> getTransformLinkedItems(const RotoItemPtr& target) const;

    /**
     * @brief Makes item, a curve or a layer, take target's transform in place of its own, and
     * whatever target follows in turn. Replaces item's previous link. Throws
     * std::invalid_argument with getTransformLinkRefusal(item, target) when that is not empty.
     **/
    void linkTransform(const RotoItemPtr& item, const RotoItemPtr& target);

    /**
     * @brief Why linkTransform(item, target) would throw, empty when it would not: either is null
     * or not a curve or layer of this context, they are the same, one is inside the other, or
     * target already follows item.
     **/
    std::string getTransformLinkRefusal(const RotoItemPtr& item, const RotoItemPtr& target) const;

    /**
     * @brief Ends item's transform link, giving item the values it followed, so it stays in place.
     **/
    void unlinkTransform(const RotoItemPtr& item);

    /**
     * @brief The item whose own transform item takes: the end of its chain of transform links,
     * item itself without one.
     **/
    RotoItemPtr getTransformSource(const RotoItemPtr& item) const;

    /**
     * @brief The values of item's own transform: a curve's knobs or a layer's stored values.
     **/
    SplineWarpTransform getOwnTransform(const RotoItemPtr& item) const;

    /**
     * @brief Gives item's own transform these values: a curve's knobs or a layer's stored values.
     **/
    void setOwnTransform(const RotoItemPtr& item, const SplineWarpTransform& transform);

    /**
     * @brief Ties the transform knobs of each selected curve that is neither transform-linked nor
     * inside a selected layer, and unties the others. Main thread only.
     **/
    void refreshSelectedTransformTies();

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
     * @brief Gives every link of links the static value value and the keyframes of curve.
     **/
    void assignLinkAmounts(const std::vector<SplineWarpLinkPtr>& links, double value, const Curve& curve);

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
    virtual void onItemCreated(const RotoDrawableItemPtr& item, bool connectNodes) OVERRIDE;

    /**
     * @brief The centre of the Source's format, or else of the project's.
     **/
    virtual bool getDefaultTransformCenter(double time, double* x, double* y) const OVERRIDE;

    /**
     * @brief Replaces local by the transform item follows, if any, then applies its layers'.
     **/
    virtual void composeItemTransform(const RotoDrawableItem& item, double time, const Transform::Matrix3x3& local, Transform::Matrix3x3* matrix) const OVERRIDE;

    /**
     * @brief Saves the links (the same objects, with their values), the boundaries and the layer
     * amounts.
     **/
    virtual RotoCurveContextHostStatePtr saveHostState() const OVERRIDE;
    virtual void restoreHostState(const RotoCurveContextHostStatePtr& state) OVERRIDE;

    virtual void toSerialization(Serialization::RotoContextSerialization* serialization) const OVERRIDE;
    virtual void fromSerialization(const Serialization::RotoContextSerialization& serialization) OVERRIDE;

protected:
    virtual void appendHostContentHash(ContentHasher& h) const OVERRIDE;

    /**
     * @brief False for a transform-linked item, whose own transform knobs are not used.
     **/
    virtual bool tiesTransformKnobs(const RotoDrawableItem& item) const OVERRIDE;

private:
    bool ownsCurve(const BezierPtr& curve) const;
    void setBoundaryRole(const BezierPtr& curve, bool soft, bool on);
    SplineWarpAnimatedValue& layerAmountLocked(const RotoLayerPtr& layer);
    SplineWarpLinkPtr findLinkLocked(const Bezier* curve) const;
    void checkLinkable(const BezierPtr& curve, const char* what) const;
    void remapCurveParams(const BezierPtr& curve, const std::function<double(double)>& f);
    void warpDataChanged(bool evaluate);
    // The end of item's chain of transform links, null when item has none. _warpMutex is held.
    RotoItemPtr resolveTransformLinkLocked(const RotoItem* item) const;
    // Whether from, or an item it follows through transform links, is to. _warpMutex is held.
    bool transformLinkReachesLocked(const RotoItem* from, const RotoItem* to) const;
    // A layer's stored transform, or null. _warpMutex is held.
    const SplineWarpTransform* findLayerTransformLocked(const RotoItem* layer) const;
    bool ownsItem(const RotoItemPtr& item) const;
    void transformLinksChanged();
    // Tells the panel the Role and Linked To columns may have changed.
    void rolesChanged();

    mutable std::mutex _warpMutex;
    std::vector<SplineWarpLinkPtr> _links;
    std::list<BezierWPtr> _boundaries;
    std::list<BezierWPtr> _softBoundaries;
    std::list<BezierWPtr> _cookieCutters;
    std::list<std::pair<RotoLayerWPtr, SplineWarpAnimatedValue>> _layerAmounts;
    std::list<std::pair<RotoLayerWPtr, SplineWarpTransform>> _layerTransforms;
    // Each item with the item whose transform it takes.
    std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>> _transformLinks;
    bool _loading;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpContext_h
