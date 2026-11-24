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

#include "RotoKeyframeReduce.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Curve.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/RotoContextPrivate.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/Transform.h"
#include "Engine/VectorFollow.h"
#include "Engine/VectorFollowReduce.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr double kMinCornerSpan = 1.;

FollowMatrix
toFollowMatrix(const Transform::Matrix3x3& m)
{
    FollowMatrix out;
    out.m[0] = m.a;
    out.m[1] = m.b;
    out.m[2] = m.c;
    out.m[3] = m.d;
    out.m[4] = m.e;
    out.m[5] = m.f;
    out.m[6] = m.g;
    out.m[7] = m.h;
    out.m[8] = m.i;

    return out;
}

bool
isWholeFrame(double time)
{
    return time == std::floor(time);
}

/*
 * One animated value, as a copy reduction edits while the original stays as it is. With its last
 * key gone it holds that key's value, as a curve point or knob does once its last key is deleted.
 */
struct Channel {
    CurvePtr curve;
    double constant = 0.;
    // The frame whose removal took its last key, which the removal must also take last.
    int emptiedBy = 0;
    std::map<int, KeyFrame> removed;

    double valueAt(int frame) const
    {
        return (curve->getKeyFramesCount() > 0) ? curve->getValueAt(frame) : constant;
    }

    void remove(int frame)
    {
        KeyFrame key;
        if (!curve->getKeyFrameWithTime(frame, &key)) {
            return;
        }
        if (curve->getKeyFramesCount() == 1) {
            constant = key.getValue();
            emptiedBy = frame;
        }
        removed[frame] = key;
        curve->removeKeyFrameWithTime(frame);
    }

    void restore(int frame)
    {
        std::map<int, KeyFrame>::iterator found = removed.find(frame);
        if (found != removed.end()) {
            curve->addKeyFrame(found->second);
            removed.erase(found);
        }
    }

    bool emptied() const
    {
        return curve->getKeyFramesCount() == 0;
    }
};

// A value that is a channel when reduction keys it, read frame by frame when it animates otherwise,
// or still.
struct Value {
    int channel = -1;
    int live = -1;
    double still = 0.;
};

struct TransformValues {
    Value translate[2];
    Value rotate;
    Value scale[2];
    Value skewX;
    Value skewY;
    Value center[2];
    Value extraMatrix[9];
};

// What the item's own transform does not key but still reads, frame by frame.
struct FrameContext {
    // The total transform with the own transform taken out, so the own one can change under it.
    FollowMatrix before;
    bool uniform = false;
    bool skewOrderYX = false;
    std::vector<double> live;
};

class ShapeMotion
    : public VectorFollowKeyedMotion {
public:
    ShapeMotion(const RotoDrawableItemPtr& item,
                bool ownTransform)
        : _item(item)
        , _bezier(std::dynamic_pointer_cast<Bezier>(item))
    {
        if (_bezier) {
            addPoints(_bezier->getControlPoints_mt_safe());
            if (_bezier->useFeatherPoints()) {
                addPoints(_bezier->getFeatherPoints_mt_safe());
            }
        }
        for (std::size_t i = 0; i < _channels.size(); ++i) {
            _bezierChannels.push_back((int)i);
        }
        addKnob(kRotoDrawableItemTranslateParam, _transform.translate, 2, ownTransform);
        addKnob(kRotoDrawableItemRotateParam, &_transform.rotate, 1, ownTransform);
        addKnob(kRotoDrawableItemScaleParam, _transform.scale, 2, ownTransform);
        addKnob(kRotoDrawableItemSkewXParam, &_transform.skewX, 1, ownTransform);
        addKnob(kRotoDrawableItemSkewYParam, &_transform.skewY, 1, ownTransform);
        addKnob(kRotoDrawableItemCenterParam, _transform.center, 2, ownTransform);
        addKnob(kRotoDrawableItemExtraMatrixParam, _transform.extraMatrix, 9, ownTransform);

        for (const Channel& channel : _channels) {
            const KeyFrameSet keys = channel.curve->getKeyFrames_mt_safe();
            for (const KeyFrame& key : keys) {
                if (isWholeFrame(key.getTime())) {
                    _keys.insert((int)key.getTime());
                }
            }
        }
        if (_keys.empty()) {
            return;
        }
        const int first = *_keys.begin();
        const int last = *_keys.rbegin();
        for (int frame = first; frame <= last; ++frame) {
            _frames[frame] = frameContextAt(frame);
        }
        if (!_bezier) {
            setStrokeCorners(first);
        }
    }

    virtual ~ShapeMotion() { }

    virtual std::set<int> keys() const OVERRIDE FINAL
    {
        return _keys;
    }

    virtual void removeKey(int frame) OVERRIDE FINAL
    {
        for (Channel& channel : _channels) {
            channel.remove(frame);
        }
        _keys.erase(frame);
    }

    virtual void restoreKey(int frame) OVERRIDE FINAL
    {
        for (Channel& channel : _channels) {
            channel.restore(frame);
        }
        _keys.insert(frame);
    }

    virtual void evaluate(int frame,
                          std::vector<FollowPoint>* points) const OVERRIDE FINAL
    {
        const FollowMatrix shown = _frames.at(frame).before * localAt(frame);
        points->clear();
        if (_bezier) {
            for (const std::array<int, 6>& point : _points) {
                for (int i = 0; i < 6; i += 2) {
                    points->push_back(shown.apply(FollowPoint(_channels[point[i]].valueAt(frame), _channels[point[i + 1]].valueAt(frame))));
                }
            }
            for (const FollowPoint& still : _stillPoints) {
                points->push_back(shown.apply(still));
            }

            return;
        }
        for (const FollowPoint& corner : _corners) {
            points->push_back(shown.apply(corner));
        }
    }

    bool hasKeys() const
    {
        return !_keys.empty();
    }

    // Takes the removed frames off the item itself, as they went from the copies.
    void apply(const std::set<int>& removed) const
    {
        if (removed.empty()) {
            return;
        }
        if (_bezier && !_bezierChannels.empty()) {
            const Channel& channel = _channels[_bezierChannels.front()];
            std::set<double> times;
            for (int frame : removed) {
                if (!channel.emptied() || (frame != channel.emptiedBy)) {
                    times.insert(frame);
                }
            }
            _bezier->removeKeyframes(times);
            if (channel.emptied() && removed.count(channel.emptiedBy)) {
                _bezier->removeKeyframe(channel.emptiedBy);
            }
            _bezier->invalidatePolygonOrientation();
        }
        for (const KnobChannels& knob : _knobs) {
            KnobIPtr deletable = knob.knob;
            knob.knob->beginChanges();
            for (std::size_t dim = 0; dim < knob.channels.size(); ++dim) {
                const Channel& channel = _channels[knob.channels[dim]];
                for (int frame : removed) {
                    if (!channel.emptied() || (frame != channel.emptiedBy)) {
                        deletable->deleteValueAtTime(eCurveChangeReasonInternal, frame, ViewSpec::all(), (int)dim, false);
                    }
                }
                if (channel.emptied() && removed.count(channel.emptiedBy)) {
                    deletable->deleteValueAtTime(eCurveChangeReasonInternal, channel.emptiedBy, ViewSpec::all(), (int)dim, true);
                }
            }
            knob.knob->endChanges();
        }
    }

private:
    struct KnobChannels {
        KnobDoublePtr knob;
        std::vector<int> channels;
    };

    int addChannel(const Curve& original)
    {
        Channel channel;
        channel.curve = std::make_shared<Curve>();
        channel.curve->clone(original);
        _channels.push_back(channel);

        return (int)_channels.size() - 1;
    }

    void addPoints(const std::list<BezierCPPtr>& points)
    {
        for (const BezierCPPtr& cp : points) {
            if (cp->getXCurve()->getKeyFramesCount() == 0) {
                double x = 0.;
                double y = 0.;
                cp->getPositionAtTime(false, 0., ViewIdx(0), &x, &y);
                _stillPoints.push_back(FollowPoint(x, y));
                cp->getLeftBezierPointAtTime(false, 0., ViewIdx(0), &x, &y);
                _stillPoints.push_back(FollowPoint(x, y));
                cp->getRightBezierPointAtTime(false, 0., ViewIdx(0), &x, &y);
                _stillPoints.push_back(FollowPoint(x, y));
                continue;
            }
            std::array<int, 6> point;
            point[0] = addChannel(*cp->getXCurve());
            point[1] = addChannel(*cp->getYCurve());
            point[2] = addChannel(*cp->getLeftXCurve());
            point[3] = addChannel(*cp->getLeftYCurve());
            point[4] = addChannel(*cp->getRightXCurve());
            point[5] = addChannel(*cp->getRightYCurve());
            _points.push_back(point);
        }
    }

    // A transform knob that is not the item's own to key, or that another knob or an expression
    // drives, keeps its keys, and its values are read as they are.
    void addKnob(const char* name,
                 Value* values,
                 int dimensions,
                 bool ownTransform)
    {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(_item->getKnobByName(name));
        if (!knob) {
            return;
        }
        bool reducible = ownTransform;
        for (int dim = 0; dim < dimensions; ++dim) {
            values[dim].still = knob->getValue(dim);
            if (!knob->getExpression(dim).empty() || knob->isSlave(dim)) {
                reducible = false;
            }
        }
        bool animated = false;
        for (int dim = 0; dim < dimensions; ++dim) {
            animated = animated || (knob->getCurve(ViewIdx(0), dim)->getKeyFramesCount() > 0);
        }
        if (!reducible) {
            for (int dim = 0; dim < dimensions; ++dim) {
                values[dim].live = (int)_liveDimensions.size();
                _liveDimensions.push_back(std::make_pair(knob, dim));
            }

            return;
        }
        if (!animated) {
            return;
        }
        KnobChannels channels;
        channels.knob = knob;
        for (int dim = 0; dim < dimensions; ++dim) {
            const CurvePtr curve = knob->getCurve(ViewIdx(0), dim);
            if (curve->getKeyFramesCount() == 0) {
                channels.channels.push_back(-1);
                continue;
            }
            values[dim].channel = addChannel(*curve);
            channels.channels.push_back(values[dim].channel);
        }
        channels.channels.erase(std::remove(channels.channels.begin(), channels.channels.end(), -1), channels.channels.end());
        _knobs.push_back(channels);
    }

    double valueOf(const Value& value,
                   int frame) const
    {
        if (value.channel >= 0) {
            return _channels[value.channel].valueAt(frame);
        }
        if (value.live >= 0) {
            return _frames.at(frame).live[value.live];
        }

        return value.still;
    }

    FollowMatrix localAt(int frame) const
    {
        const FrameContext& context = _frames.at(frame);
        const double sx = valueOf(_transform.scale[0], frame);
        const double sy = context.uniform ? sx : valueOf(_transform.scale[1], frame);
        Transform::Matrix3x3 local = Transform::matTransformCanonical(valueOf(_transform.translate[0], frame),
                                                                      valueOf(_transform.translate[1], frame),
                                                                      sx,
                                                                      sy,
                                                                      valueOf(_transform.skewX, frame),
                                                                      valueOf(_transform.skewY, frame),
                                                                      context.skewOrderYX,
                                                                      Transform::toRadians(valueOf(_transform.rotate, frame)),
                                                                      valueOf(_transform.center[0], frame),
                                                                      valueOf(_transform.center[1], frame));
        Transform::Matrix3x3 extra;
        extra.a = valueOf(_transform.extraMatrix[0], frame);
        extra.b = valueOf(_transform.extraMatrix[1], frame);
        extra.c = valueOf(_transform.extraMatrix[2], frame);
        extra.d = valueOf(_transform.extraMatrix[3], frame);
        extra.e = valueOf(_transform.extraMatrix[4], frame);
        extra.f = valueOf(_transform.extraMatrix[5], frame);
        extra.g = valueOf(_transform.extraMatrix[6], frame);
        extra.h = valueOf(_transform.extraMatrix[7], frame);
        extra.i = valueOf(_transform.extraMatrix[8], frame);

        return toFollowMatrix(Transform::matMul(local, extra));
    }

    FrameContext frameContextAt(int frame)
    {
        FrameContext context;
        KnobBoolPtr uniform = std::dynamic_pointer_cast<KnobBool>(_item->getKnobByName(kRotoDrawableItemScaleUniformParam));
        KnobChoicePtr skewOrder = std::dynamic_pointer_cast<KnobChoice>(_item->getKnobByName(kRotoDrawableItemSkewOrderParam));
        context.uniform = uniform && uniform->getValueAtTime(frame);
        context.skewOrderYX = skewOrder && (skewOrder->getValueAtTime(frame) == 1);
        for (const std::pair<KnobDoublePtr, int>& live : _liveDimensions) {
            context.live.push_back(live.first->getValueAtTime(frame, live.second));
        }
        Transform::Matrix3x3 total;
        _item->getTransformAtTime(frame, &total);
        Transform::Matrix3x3 local;
        _item->getLocalTransformAtTime(frame, &local);
        FollowMatrix inverseLocal;
        if (toFollowMatrix(local).inverted(&inverseLocal)) {
            context.before = toFollowMatrix(total) * inverseLocal;
        }

        return context;
    }

    void setStrokeCorners(int frame)
    {
        const RotoStrokeItem* stroke = dynamic_cast<const RotoStrokeItem*>(_item.get());
        if (!stroke) {
            return;
        }
        std::list<std::list<std::pair<Point, double>>> strokes;
        stroke->evaluateStroke(0, frame, &strokes);
        FollowMatrix inverseTotal;
        Transform::Matrix3x3 total;
        _item->getTransformAtTime(frame, &total);
        if (!toFollowMatrix(total).inverted(&inverseTotal)) {
            return;
        }
        bool any = false;
        double x1 = 0.;
        double x2 = 0.;
        double y1 = 0.;
        double y2 = 0.;
        for (const std::list<std::pair<Point, double>>& part : strokes) {
            for (const std::pair<Point, double>& p : part) {
                const FollowPoint own = inverseTotal.apply(FollowPoint(p.first.x, p.first.y));
                x1 = any ? std::min(x1, own.x) : own.x;
                x2 = any ? std::max(x2, own.x) : own.x;
                y1 = any ? std::min(y1, own.y) : own.y;
                y2 = any ? std::max(y2, own.y) : own.y;
                any = true;
            }
        }
        if (!any) {
            return;
        }
        if (x2 - x1 < kMinCornerSpan) {
            x1 -= kMinCornerSpan / 2.;
            x2 += kMinCornerSpan / 2.;
        }
        if (y2 - y1 < kMinCornerSpan) {
            y1 -= kMinCornerSpan / 2.;
            y2 += kMinCornerSpan / 2.;
        }
        _corners = { FollowPoint(x1, y1), FollowPoint(x2, y1), FollowPoint(x2, y2), FollowPoint(x1, y2) };
    }

    RotoDrawableItemPtr _item;
    BezierPtr _bezier;
    std::vector<Channel> _channels;
    std::vector<int> _bezierChannels;
    std::vector<std::array<int, 6>> _points;
    std::vector<FollowPoint> _stillPoints;
    std::vector<FollowPoint> _corners;
    TransformValues _transform;
    std::vector<KnobChannels> _knobs;
    std::vector<std::pair<KnobDoublePtr, int>> _liveDimensions;
    std::map<int, FrameContext> _frames;
    std::set<int> _keys;
};

}

std::vector<RotoReducedItem>
reduceRotoKeyframes(const RotoCurveContextPtr& context,
                    const std::list<RotoDrawableItemPtr>& items,
                    int first,
                    int last,
                    double tolerance,
                    UndoCommandPtr* undo)
{
    std::vector<RotoReducedItem> reduced;
    if (!context || (first > last)) {
        return reduced;
    }

    std::vector<std::pair<RotoDrawableItemPtr, std::unique_ptr<ShapeMotion>>> motions;
    for (const RotoDrawableItemPtr& item : items) {
        if (!item) {
            continue;
        }
        std::unique_ptr<ShapeMotion> motion(new ShapeMotion(item, context->fitKeysOwnTransform(*item)));
        if (motion->hasKeys()) {
            motions.push_back(std::make_pair(item, std::move(motion)));
        }
    }

    const std::function<bool()> never = []() {
        return false;
    };
    std::vector<std::pair<std::size_t, std::set<int>>> removals;
    for (std::size_t i = 0; i < motions.size(); ++i) {
        std::set<int> removable;
        for (int frame : motions[i].second->keys()) {
            if ((frame >= first) && (frame <= last)) {
                removable.insert(frame);
            }
        }
        if (removable.empty()) {
            continue;
        }
        const std::set<int> removed = reduceVectorFollowKeys(*motions[i].second, removable, tolerance, never);
        if (!removed.empty()) {
            removals.push_back(std::make_pair(i, removed));
        }
    }
    if (removals.empty()) {
        return reduced;
    }

    std::list<RotoDrawableItemPtr> changed;
    for (const std::pair<std::size_t, std::set<int>>& removal : removals) {
        changed.push_back(motions[removal.first].first);
    }
    std::shared_ptr<FollowVectorsUndoCommand> command = std::make_shared<FollowVectorsUndoCommand>(context, changed, false);
    command->setText(kRotoReduceKeyframesActionName);
    {
        RotoCurveContext::TransformTieSuspension untied(context);
        for (const std::pair<std::size_t, std::set<int>>& removal : removals) {
            motions[removal.first].second->apply(removal.second);
            RotoReducedItem item;
            item.item = motions[removal.first].first;
            item.removed = removal.second;
            reduced.push_back(item);
        }
    }
    command->captureResult();
    if (undo) {
        *undo = command;
    } else {
        context->getNode()->pushUndoCommand(command);
    }
    context->refreshTrackLinkListeners();
    context->emitRefreshViewerOverlays();
    context->evaluateChange();

    return reduced;
}

NATRON_NAMESPACE_EXIT
