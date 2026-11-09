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

#include "VectorFollowBake.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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
#include "Engine/VectorFollowModels.h"
#include "Engine/VectorFollowReduce.h"
#include "Engine/VectorSampler.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr int kMaxStrokeSamples = 256;
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

Transform::Matrix3x3
toTransformMatrix(const FollowMatrix& m)
{
    Transform::Matrix3x3 out;
    out.a = m.m[0];
    out.b = m.m[1];
    out.c = m.m[2];
    out.d = m.m[3];
    out.e = m.m[4];
    out.f = m.m[5];
    out.g = m.m[6];
    out.h = m.m[7];
    out.i = m.m[8];

    return out;
}

FollowMatrix
totalTransformAt(const RotoDrawableItem& item,
                 double time)
{
    Transform::Matrix3x3 m;
    item.getTransformAtTime(time, &m);

    return toFollowMatrix(m);
}

KnobDoublePtr
doubleKnob(const RotoDrawableItem& item,
           const char* name)
{
    return std::dynamic_pointer_cast<KnobDouble>(item.getKnobByName(name));
}

FollowMatrix
extraMatrixAt(const KnobDoublePtr& knob,
              double time)
{
    FollowMatrix out;
    for (int dim = 0; dim < 9; ++dim) {
        out.m[dim] = knob->getValueAtTime(time, dim);
    }

    return out;
}

void
insertIntegerKeys(const Curve& curve,
                  int first,
                  int last,
                  std::set<int>* frames)
{
    const KeyFrameSet keys = curve.getKeyFrames_mt_safe();
    for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        const double t = it->getTime();
        if ((t == std::floor(t)) && (t >= first) && (t <= last)) {
            frames->insert((int)t);
        }
    }
}

void
insertKnobKeys(const KnobDoublePtr& knob,
               int first,
               int last,
               std::set<int>* frames)
{
    if (!knob) {
        return;
    }
    for (int dim = 0; dim < knob->getDimension(); ++dim) {
        CurvePtr curve = knob->getCurve(ViewIdx(0), dim);
        if (curve) {
            insertIntegerKeys(*curve, first, last, frames);
        }
    }
}

// Copies of curves, which reduction edits off the main thread while the originals stay in use.
class CurveCopiesMotion
    : public VectorFollowKeyedMotion {
public:
    explicit CurveCopiesMotion(const std::set<int>& keys)
        : _keys(keys)
    {
    }

    virtual ~CurveCopiesMotion() { }

    virtual std::set<int> keys() const OVERRIDE FINAL
    {
        return _keys;
    }

    virtual void removeKey(int frame) OVERRIDE FINAL
    {
        std::vector<KeyFrame>& stash = _removed[frame];
        stash.assign(_curves.size(), KeyFrame());
        for (std::size_t i = 0; i < _curves.size(); ++i) {
            if (_curves[i]->getKeyFrameWithTime(frame, &stash[i])) {
                _curves[i]->removeKeyFrameWithTime(frame);
            }
        }
        _keys.erase(frame);
    }

    virtual void restoreKey(int frame) OVERRIDE FINAL
    {
        const std::vector<KeyFrame>& stash = _removed[frame];
        for (std::size_t i = 0; i < _curves.size(); ++i) {
            _curves[i]->addKeyFrame(stash[i]);
        }
        _keys.insert(frame);
    }

protected:
    std::size_t addCopy(const Curve& original)
    {
        CurvePtr copy = std::make_shared<Curve>();
        copy->clone(original);
        _curves.push_back(copy);

        return _curves.size() - 1;
    }

    std::vector<CurvePtr> _curves;

private:
    std::set<int> _keys;
    std::map<int, std::vector<KeyFrame>> _removed;
};

// Every position and tangent of a curve, in its own coordinates.
class PointCurvesMotion
    : public CurveCopiesMotion {
public:
    PointCurvesMotion(const Bezier& bezier,
                      const std::set<int>& keys)
        : CurveCopiesMotion(keys)
    {
        addPoints(bezier.getControlPoints_mt_safe());
        if (bezier.useFeatherPoints()) {
            addPoints(bezier.getFeatherPoints_mt_safe());
        }
    }

    virtual void evaluate(int frame,
                          std::vector<FollowPoint>* points) const OVERRIDE FINAL
    {
        points->resize(_curves.size() / 2);
        for (std::size_t i = 0; i < points->size(); ++i) {
            (*points)[i].x = _curves[i * 2]->getValueAt(frame);
            (*points)[i].y = _curves[i * 2 + 1]->getValueAt(frame);
        }
    }

private:
    void addPoints(const std::list<BezierCPPtr>& points)
    {
        for (std::list<BezierCPPtr>::const_iterator it = points.begin(); it != points.end(); ++it) {
            addCopy(*(*it)->getXCurve());
            addCopy(*(*it)->getYCurve());
            addCopy(*(*it)->getLeftXCurve());
            addCopy(*(*it)->getLeftYCurve());
            addCopy(*(*it)->getRightXCurve());
            addCopy(*(*it)->getRightYCurve());
        }
    }
};

// A translation moves every point alike, so one point stands for the shape.
class TranslateCurvesMotion
    : public CurveCopiesMotion {
public:
    TranslateCurvesMotion(const KnobDoublePtr& translate,
                          const std::set<int>& keys)
        : CurveCopiesMotion(keys)
    {
        addCopy(*translate->getCurve(ViewIdx(0), 0));
        addCopy(*translate->getCurve(ViewIdx(0), 1));
    }

    virtual void evaluate(int frame,
                          std::vector<FollowPoint>* points) const OVERRIDE FINAL
    {
        points->assign(1, FollowPoint(_curves[0]->getValueAt(frame), _curves[1]->getValueAt(frame)));
    }
};

// The extra matrix through what comes before it, on the corners of the shape's box, so a deviation
// is measured where it shows and not in the matrix's own, possibly scaled, space.
class MatrixCurvesMotion
    : public CurveCopiesMotion {
public:
    MatrixCurvesMotion(const KnobDoublePtr& extraMatrix,
                       const std::vector<FollowPoint>& corners,
                       const std::map<int, FollowMatrix>& before,
                       const std::set<int>& keys)
        : CurveCopiesMotion(keys)
        , _corners(corners)
        , _before(before)
    {
        for (int dim = 0; dim < 9; ++dim) {
            addCopy(*extraMatrix->getCurve(ViewIdx(0), dim));
        }
    }

    virtual void evaluate(int frame,
                          std::vector<FollowPoint>* points) const OVERRIDE FINAL
    {
        FollowMatrix extra;
        for (int dim = 0; dim < 9; ++dim) {
            extra.m[dim] = _curves[dim]->getValueAt(frame);
        }
        const FollowMatrix total = _before.at(frame) * extra;
        points->resize(_corners.size());
        for (std::size_t i = 0; i < _corners.size(); ++i) {
            (*points)[i] = total.apply(_corners[i]);
        }
    }

private:
    std::vector<FollowPoint> _corners;
    std::map<int, FollowMatrix> _before;
};

struct FollowShape {
    RotoDrawableItemPtr item;
    BezierPtr bezier;
    // Set instead of item and bezier for points that are not a curve's.
    VectorFollowPointsPtr points;
    VectorFollowShapeSetup setup;
    // A fit whose item's own transform knobs do not drive it: the fit is applied to reference and
    // keyed on the points instead.
    bool bakesFit = false;
    // The tracked points, as trackedPointsAt() gives them, at the first anchor.
    std::vector<FollowPoint> reference;
    // The control points tracked one by one, when the model says so, or a baked fit moves.
    std::vector<int> tracked;
    // The shape's point count when the follow began, which the indices in tracked refer to.
    int pointCount = 0;
    bool feather = false;
    std::set<int> anchorFrames;
    std::set<int> written;
    VectorFollowShapeOutcome outcome;
    std::unique_ptr<CurveCopiesMotion> motion;
    std::set<int> removable;
    std::set<int> removed;
};

std::vector<FollowPoint>
boxCorners(const std::vector<FollowPoint>& points)
{
    std::vector<FollowPoint> corners;
    if (points.empty()) {
        return corners;
    }
    double x1 = points[0].x;
    double x2 = x1;
    double y1 = points[0].y;
    double y2 = y1;
    for (const FollowPoint& p : points) {
        x1 = std::min(x1, p.x);
        x2 = std::max(x2, p.x);
        y1 = std::min(y1, p.y);
        y2 = std::max(y2, p.y);
    }
    if (x2 - x1 < kMinCornerSpan) {
        x1 -= kMinCornerSpan / 2.;
        x2 += kMinCornerSpan / 2.;
    }
    if (y2 - y1 < kMinCornerSpan) {
        y1 -= kMinCornerSpan / 2.;
        y2 += kMinCornerSpan / 2.;
    }
    corners.push_back(FollowPoint(x1, y1));
    corners.push_back(FollowPoint(x2, y1));
    corners.push_back(FollowPoint(x2, y2));
    corners.push_back(FollowPoint(x1, y2));

    return corners;
}

// The shape's outline and the points inside it, as they show at time.
std::vector<FollowPoint>
motionSamplesAt(const FollowShape& shape,
                double time)
{
    std::vector<FollowPoint> samples;
    if (shape.bezier) {
        std::list<std::list<ParametricPoint>> segments;
        RectD bbox;
        shape.bezier->evaluateAtTime_DeCasteljau_autoNbPoints(false, time, 0, &segments, &bbox);
        std::vector<FollowPoint> polygon;
        for (const std::list<ParametricPoint>& segment : segments) {
            for (const ParametricPoint& p : segment) {
                polygon.push_back(FollowPoint(p.x, p.y));
            }
        }
        samples = followGridSamplesInside(polygon);

        const FollowMatrix total = totalTransformAt(*shape.item, time);
        const std::list<BezierCPPtr> points = shape.bezier->getControlPoints_mt_safe();
        for (const BezierCPPtr& cp : points) {
            double x;
            double y;
            cp->getPositionAtTime(false, time, ViewIdx(0), &x, &y);
            samples.push_back(total.apply(FollowPoint(x, y)));
        }

        return samples;
    }

    const RotoStrokeItem* stroke = dynamic_cast<const RotoStrokeItem*>(shape.item.get());
    if (!stroke) {
        return samples;
    }
    std::list<std::list<std::pair<Point, double>>> strokes;
    stroke->evaluateStroke(0, time, &strokes);
    std::size_t count = 0;
    for (const std::list<std::pair<Point, double>>& part : strokes) {
        count += part.size();
    }
    const std::size_t stride = std::max<std::size_t>(1, count / kMaxStrokeSamples);
    std::size_t index = 0;
    for (const std::list<std::pair<Point, double>>& part : strokes) {
        for (const std::pair<Point, double>& p : part) {
            if (index++ % stride == 0) {
                samples.push_back(FollowPoint(p.first.x, p.first.y));
            }
        }
    }

    return samples;
}

// Position, left and right tangent, of the tracked control points then of their feather points.
std::vector<FollowPoint>
trackedPointsAt(const FollowShape& shape,
                double time)
{
    const FollowMatrix total = totalTransformAt(*shape.item, time);
    std::vector<FollowPoint> out;
    const auto append = [&](const BezierCPPtr& cp) {
        double x = 0.;
        double y = 0.;
        cp->getPositionAtTime(false, time, ViewIdx(0), &x, &y);
        out.push_back(total.apply(FollowPoint(x, y)));
        cp->getLeftBezierPointAtTime(false, time, ViewIdx(0), &x, &y, false);
        out.push_back(total.apply(FollowPoint(x, y)));
        cp->getRightBezierPointAtTime(false, time, ViewIdx(0), &x, &y, false);
        out.push_back(total.apply(FollowPoint(x, y)));
    };
    for (int index : shape.tracked) {
        append(shape.bezier->getControlPointAtIndex(index));
        if (shape.feather) {
            append(shape.bezier->getFeatherPointAtIndex(index));
        }
    }

    return out;
}

// Whether the shape is keyed on its points, rather than on its transform knobs.
bool
keysPoints(const FollowShape& shape)
{
    return (shape.setup.model == VectorFollowModel::ePerPoint) || shape.bakesFit;
}

std::vector<int>
pointsToTrack(const Bezier& bezier,
              const std::set<int>* selected)
{
    std::vector<int> tracked;
    const int count = bezier.getControlPointsCount();
    for (int i = 0; i < count; ++i) {
        if (selected && !selected->count(i)) {
            continue;
        }
        BezierCPPtr cp = bezier.getControlPointAtIndex(i);
        if (cp && !cp->isLinkedToTrack()) {
            tracked.push_back(i);
        }
    }

    return tracked;
}

} // anonymous namespace

struct VectorFollowRun::Imp {
    RotoCurveContextPtr context;
    VectorFollowParams params;
    std::vector<std::unique_ptr<FollowShape>> shapes;
    std::shared_ptr<FollowVectorsUndoCommand> undo;
    // Held from prepare() to finish(), so the undo snapshot, every write and the undo itself reach
    // each shape's own transform knobs, whatever the selection does in between.
    std::unique_ptr<RotoCurveContext::TransformTieSuspension> tieSuspension;
    bool aborted = false;
    bool userStopped = false;
    bool forwardOnly = false;
    std::vector<VectorFollowRefusal> refused;

    Imp(const RotoCurveContextPtr& context_,
        const VectorFollowParams& params_)
        : context(context_)
        , params(params_)
    {
    }

    std::unique_ptr<FollowShape> makeShape(const RotoDrawableItemPtr& item, std::string* refusal) const;
    std::unique_ptr<FollowShape> makePointsShape(const VectorFollowPointsPtr& points) const;
    void addAnchorFrames(FollowShape& shape) const;
    std::set<int> keyFramesOf(const FollowShape& shape, int first, int last) const;
    bool writePoints(FollowShape& shape, double time, const std::vector<FollowPoint>& points) const;
    void writeFrame(FollowShape& shape, const VectorFollowFrame& frame) const;
    void removeKeys(FollowShape& shape) const;
};

// The keys of what the shape's model drives: its points for per-point, its transform knobs for a fit.
std::set<int>
VectorFollowRun::Imp::keyFramesOf(const FollowShape& shape,
                                  int first,
                                  int last) const
{
    if (shape.points) {
        return shape.points->getKeyFrames(first, last);
    }
    std::set<int> frames;
    if (keysPoints(shape)) {
        std::set<double> keys;
        shape.bezier->getKeyframeTimes(&keys);
        for (double t : keys) {
            if ((t == std::floor(t)) && (t >= first) && (t <= last)) {
                frames.insert((int)t);
            }
        }

        return frames;
    }

    const RotoDrawableItem& item = *shape.item;
    insertKnobKeys(doubleKnob(item, kRotoDrawableItemTranslateParam), first, last, &frames);
    if (shape.setup.model != VectorFollowModel::eTranslate) {
        insertKnobKeys(doubleKnob(item, kRotoDrawableItemRotateParam), first, last, &frames);
        insertKnobKeys(doubleKnob(item, kRotoDrawableItemScaleParam), first, last, &frames);
        insertKnobKeys(doubleKnob(item, kRotoDrawableItemSkewXParam), first, last, &frames);
        insertKnobKeys(doubleKnob(item, kRotoDrawableItemSkewYParam), first, last, &frames);
        insertKnobKeys(doubleKnob(item, kRotoDrawableItemCenterParam), first, last, &frames);
        insertKnobKeys(doubleKnob(item, kRotoDrawableItemExtraMatrixParam), first, last, &frames);
    }

    return frames;
}

std::unique_ptr<FollowShape>
VectorFollowRun::Imp::makeShape(const RotoDrawableItemPtr& item,
                                std::string* refusal) const
{
    std::unique_ptr<FollowShape> shape(new FollowShape);
    shape->item = item;
    shape->bezier = std::dynamic_pointer_cast<Bezier>(item);
    shape->setup.direction = params.direction;
    shape->setup.confidenceThreshold = params.confidenceThreshold;
    shape->setup.first = params.first;
    shape->setup.last = params.last;
    shape->setup.model = params.model;
    shape->setup.forwardOnly = forwardOnly;

    if (shape->bezier) {
        if (shape->bezier->getControlPointsCount() == 0) {
            return nullptr;
        }
        shape->pointCount = shape->bezier->getControlPointsCount();
        shape->feather = shape->bezier->useFeatherPoints();
        std::map<RotoDrawableItem*, std::set<int>>::const_iterator selected = params.selectedPoints.find(item.get());
        const bool some = (selected != params.selectedPoints.end()) && !selected->second.empty() && ((int)selected->second.size() < shape->bezier->getControlPointsCount());
        if (some) {
            shape->setup.model = VectorFollowModel::ePerPoint;
        }
        if (shape->setup.model == VectorFollowModel::ePerPoint) {
            shape->tracked = pointsToTrack(*shape->bezier, some ? &selected->second : 0);
            if (shape->tracked.empty()) {
                return nullptr;
            }
        }
    } else {
        RotoStrokeItem* stroke = dynamic_cast<RotoStrokeItem*>(item.get());
        if (!stroke || stroke->isEmpty()) {
            return nullptr;
        }
        // A stroke's samples are not keyed, so it can only follow by its transform.
        if (shape->setup.model == VectorFollowModel::ePerPoint) {
            shape->setup.model = VectorFollowModel::eAffine;
        }
    }
    if ((shape->setup.model != VectorFollowModel::ePerPoint) && !context->fitKeysOwnTransform(*item)) {
        if (!shape->bezier) {
            *refusal = "its transform is not its own, and it has no points to key";

            return nullptr;
        }
        shape->bakesFit = true;
        shape->tracked = pointsToTrack(*shape->bezier, 0);
        if (shape->tracked.empty()) {
            return nullptr;
        }
    }

    addAnchorFrames(*shape);

    KnobDoublePtr translate = doubleKnob(*item, kRotoDrawableItemTranslateParam);
    for (int frame : shape->anchorFrames) {
        VectorFollowAnchor anchor;
        anchor.frame = frame;
        // Per-point needs these too: they decide when a shape stops, and move its unsure points.
        anchor.samples = motionSamplesAt(*shape, frame);
        if (anchor.samples.empty()) {
            return nullptr;
        }
        switch (shape->bakesFit ? VectorFollowModel::ePerPoint : shape->setup.model) {
        case VectorFollowModel::ePerPoint:
            anchor.points = trackedPointsAt(*shape, frame);
            break;
        case VectorFollowModel::eTranslate:
            anchor.transform = FollowMatrix::translation(translate->getValueAtTime(frame, 0), translate->getValueAtTime(frame, 1));
            break;
        case VectorFollowModel::eAffine:
        case VectorFollowModel::ePerspective:
            anchor.transform = totalTransformAt(*item, frame);
            break;
        }
        shape->setup.anchors.push_back(anchor);
    }

    if (shape->bakesFit) {
        /*
         * The fit's transform maps the first anchor's points to where they go, so each anchor's is
         * the fit of the first anchor's points to its own: the identity for the first, and for a
         * key that changed the shape otherwise, the nearest the model gets to it.
         */
        shape->reference = shape->setup.anchors.front().points;
        const std::vector<double> weights(shape->reference.size(), 1.);
        for (VectorFollowAnchor& anchor : shape->setup.anchors) {
            const VectorFollowFit fit = fitFollowMotion(shape->setup.model, shape->reference, anchor.points, weights);
            anchor.transform = fit.valid ? fit.matrix : FollowMatrix();
            anchor.points.clear();
        }
    }
    if (shape->setup.model != VectorFollowModel::ePerPoint) {
        const VectorFollowAnchor& firstAnchor = shape->setup.anchors.front();
        shape->setup.corners = boxCorners(firstAnchor.samples);
        if (shape->setup.model != VectorFollowModel::eTranslate) {
            FollowMatrix inverse;
            if (!firstAnchor.transform.inverted(&inverse)) {
                return nullptr;
            }
            for (FollowPoint& corner : shape->setup.corners) {
                corner = inverse.apply(corner);
            }
        }
    }

    return shape;
}

void
VectorFollowRun::Imp::addAnchorFrames(FollowShape& shape) const
{
    shape.anchorFrames = keyFramesOf(shape, params.first, params.last);
    const bool forward = params.direction == VectorFollowDirection::eForward;
    const int start = forward ? params.first : params.last;
    /*
     * What the start frame shows is only worth keeping when a key behind it shaped it. Then it is
     * an anchor like any key, and bridges to the next key ahead. Without one, a key ahead is the
     * only state the artist set, so the follow tracks back from it to the start frame instead.
     */
    const bool keyBehind = forward ? !keyFramesOf(shape, std::numeric_limits<int>::min(), params.first - 1).empty()
                                   : !keyFramesOf(shape, params.last + 1, std::numeric_limits<int>::max()).empty();
    if (shape.anchorFrames.empty() || (keyBehind && !shape.anchorFrames.count(start))) {
        shape.anchorFrames.insert(start);
        shape.setup.seedIsNew = true;
    }
}

std::unique_ptr<FollowShape>
VectorFollowRun::Imp::makePointsShape(const VectorFollowPointsPtr& points) const
{
    std::unique_ptr<FollowShape> shape(new FollowShape);
    shape->points = points;
    shape->setup.direction = params.direction;
    shape->setup.confidenceThreshold = params.confidenceThreshold;
    shape->setup.first = params.first;
    shape->setup.last = params.last;
    shape->setup.model = VectorFollowModel::ePerPoint;
    shape->setup.forwardOnly = forwardOnly;
    addAnchorFrames(*shape);
    for (int frame : shape->anchorFrames) {
        VectorFollowAnchor anchor;
        anchor.frame = frame;
        anchor.points = points->getPointsAtTime(frame);
        // With no outline around them, the points stand for the motion that moves an unsure one.
        anchor.samples = anchor.points;
        if (anchor.points.empty()) {
            return nullptr;
        }
        shape->setup.anchors.push_back(anchor);
    }
    shape->pointCount = (int)shape->setup.anchors.front().points.size();

    return shape;
}

bool
VectorFollowRun::Imp::writePoints(FollowShape& shape,
                                  double time,
                                  const std::vector<FollowPoint>& points) const
{
    // The host refuses point edits while the follow runs, but should one get through, the
    // tracked indices would land on other points than the ones followed.
    const std::size_t needed = shape.tracked.size() * (shape.feather ? 6 : 3);
    if ((shape.bezier->getControlPointsCount() != shape.pointCount) || (points.size() < needed)) {
        return false;
    }
    FollowMatrix inverse;
    if (!totalTransformAt(*shape.item, time).inverted(&inverse)) {
        return false;
    }
    const auto toPoint = [&](int index, bool feather, std::size_t at) {
        const FollowPoint p = inverse.apply(points[at]);
        const FollowPoint left = inverse.apply(points[at + 1]);
        const FollowPoint right = inverse.apply(points[at + 2]);
        Bezier::FollowedPoint out;
        out.index = index;
        out.feather = feather;
        out.x = p.x;
        out.y = p.y;
        out.leftX = left.x;
        out.leftY = left.y;
        out.rightX = right.x;
        out.rightY = right.y;

        return out;
    };
    std::vector<Bezier::FollowedPoint> followed;
    std::size_t at = 0;
    for (int index : shape.tracked) {
        followed.push_back(toPoint(index, false, at));
        at += 3;
        if (shape.feather) {
            followed.push_back(toPoint(index, true, at));
            at += 3;
        }
    }
    shape.bezier->setFollowedPoints(time, followed);

    return true;
}

void
VectorFollowRun::Imp::writeFrame(FollowShape& shape,
                                 const VectorFollowFrame& frame) const
{
    const double time = frame.frame;
    if (shape.points) {
        if ((int)frame.points.size() == shape.pointCount) {
            shape.points->setPointsAtTime(time, frame.points);
            shape.written.insert(frame.frame);
        }

        return;
    }
    if (shape.bakesFit) {
        std::vector<FollowPoint> moved(shape.reference.size());
        for (std::size_t i = 0; i < moved.size(); ++i) {
            moved[i] = frame.transform.apply(shape.reference[i]);
        }
        if (writePoints(shape, time, moved)) {
            shape.written.insert(frame.frame);
        }

        return;
    }
    switch (shape.setup.model) {
    case VectorFollowModel::ePerPoint:
        if (!writePoints(shape, time, frame.points)) {
            return;
        }
        break;
    case VectorFollowModel::eTranslate:
        shape.item->setTranslateKeyframe(time, frame.transform.m[2], frame.transform.m[5]);
        break;
    case VectorFollowModel::eAffine:
    case VectorFollowModel::ePerspective: {
        // The item's transform is what the other knobs make times the extra matrix, so the extra
        // matrix that gives another total is the current one times current total^-1 times that total.
        KnobDoublePtr extraKnob = doubleKnob(*shape.item, kRotoDrawableItemExtraMatrixParam);
        FollowMatrix inverse;
        if (!totalTransformAt(*shape.item, time).inverted(&inverse)) {
            return;
        }
        FollowMatrix extra = extraMatrixAt(extraKnob, time) * inverse * frame.transform;
        if (extra.m[8] != 0.) {
            const double scale = extra.m[8];
            for (double& v : extra.m) {
                v /= scale;
            }
        }
        shape.item->setExtraMatrix(true, time, toTransformMatrix(extra));
        if (shape.bezier) {
            shape.bezier->invalidatePolygonOrientation();
        }
        break;
    }
    }
    shape.written.insert(frame.frame);
}

void
VectorFollowRun::Imp::removeKeys(FollowShape& shape) const
{
    if (shape.removed.empty()) {
        return;
    }
    if (keysPoints(shape)) {
        std::set<double> times(shape.removed.begin(), shape.removed.end());
        shape.bezier->removeKeyframes(times);

        return;
    }

    KnobDoublePtr knob = doubleKnob(*shape.item, shape.setup.model == VectorFollowModel::eTranslate ? kRotoDrawableItemTranslateParam : kRotoDrawableItemExtraMatrixParam);
    KnobIPtr deletable = knob;
    knob->beginChanges();
    for (int frame : shape.removed) {
        for (int dim = 0; dim < knob->getDimension(); ++dim) {
            deletable->deleteValueAtTime(eCurveChangeReasonInternal, frame, ViewSpec::all(), dim, false);
        }
    }
    knob->endChanges();
    if (shape.bezier) {
        shape.bezier->invalidatePolygonOrientation();
    }
}

VectorFollowRun::VectorFollowRun(const RotoCurveContextPtr& context,
                                 const VectorFollowParams& params)
    : _imp(new Imp(context, params))
{
}

VectorFollowRun::~VectorFollowRun()
{
}

bool
VectorFollowRun::prepare(std::string* error)
{
    const VectorFollowParams& params = _imp->params;
    if (params.first > params.last) {
        *error = "The frame range to follow over is empty.";

        return false;
    }
    if (!VectorSampler::checkSource(params.source, _imp->context->getNode(), params.first, params.direction, error)) {
        return false;
    }
    _imp->forwardOnly = !VectorSampler::hasMotionLayers(params.source, params.first);

    _imp->tieSuspension.reset(new RotoCurveContext::TransformTieSuspension(_imp->context));
    std::list<RotoDrawableItemPtr> items;
    for (const RotoDrawableItemPtr& item : params.items) {
        std::string refusal;
        std::unique_ptr<FollowShape> shape = _imp->makeShape(item, &refusal);
        if (shape) {
            items.push_back(item);
            _imp->shapes.push_back(std::move(shape));
        } else if (!refusal.empty()) {
            _imp->refused.push_back(VectorFollowRefusal { item->getScriptName(), refusal });
        }
    }
    for (const VectorFollowPointsPtr& points : params.points) {
        std::unique_ptr<FollowShape> shape = points ? _imp->makePointsShape(points) : nullptr;
        if (shape) {
            _imp->shapes.push_back(std::move(shape));
        }
    }
    if (_imp->shapes.empty()) {
        _imp->tieSuspension.reset();
        *error = "None of the given shapes can follow vectors.";
        for (const VectorFollowRefusal& refused : _imp->refused) {
            *error += " " + refused.name + ": " + refused.reason + ".";
        }

        return false;
    }

    _imp->undo = std::make_shared<FollowVectorsUndoCommand>(_imp->context, items, !params.points.empty());

    // An anchor keyed on another knob only would slide once keys land around it on the extra matrix.
    for (const std::unique_ptr<FollowShape>& shape : _imp->shapes) {
        if (shape->bakesFit || ((shape->setup.model != VectorFollowModel::eAffine) && (shape->setup.model != VectorFollowModel::ePerspective))) {
            continue;
        }
        const KnobDoublePtr extraKnob = doubleKnob(*shape->item, kRotoDrawableItemExtraMatrixParam);
        for (int frame : shape->anchorFrames) {
            shape->item->setExtraMatrix(true, frame, toTransformMatrix(extraMatrixAt(extraKnob, frame)));
        }
    }

    return true;
}

void
VectorFollowRun::compute(VectorField& field,
                         const std::function<void(int, const VectorFollowFrame&)>& consumer,
                         const std::function<bool()>& aborted)
{
    const std::function<bool()> stop = [this, &aborted]() {
        const bool stops = aborted();
        _imp->userStopped = _imp->userStopped || stops;

        return stops;
    };
    for (std::size_t i = 0; i < _imp->shapes.size(); ++i) {
        FollowShape& shape = *_imp->shapes[i];
        shape.outcome = followVectorShape(field, shape.setup, [&](const VectorFollowFrame& frame) { consumer((int)i, frame); }, stop);
        if (shape.outcome.aborted) {
            _imp->aborted = true;

            return;
        }
    }
}

bool
VectorFollowRun::wasAborted() const
{
    return _imp->aborted;
}

void
VectorFollowRun::applyFrame(int shape,
                            const VectorFollowFrame& frame)
{
    _imp->writeFrame(*_imp->shapes[shape], frame);
}

void
VectorFollowRun::prepareReduction()
{
    for (const std::unique_ptr<FollowShape>& shape : _imp->shapes) {
        // Points that are not a curve's keep every key the follow wrote.
        if (shape->points) {
            continue;
        }
        shape->removable = shape->written;
        for (int frame : shape->anchorFrames) {
            shape->removable.erase(frame);
        }
        if (shape->removable.empty()) {
            continue;
        }

        std::set<int> keys = shape->written;
        keys.insert(shape->anchorFrames.begin(), shape->anchorFrames.end());
        switch (shape->bakesFit ? VectorFollowModel::ePerPoint : shape->setup.model) {
        case VectorFollowModel::ePerPoint:
            shape->motion.reset(new PointCurvesMotion(*shape->bezier, keys));
            break;
        case VectorFollowModel::eTranslate:
            shape->motion.reset(new TranslateCurvesMotion(doubleKnob(*shape->item, kRotoDrawableItemTranslateParam), keys));
            break;
        case VectorFollowModel::eAffine:
        case VectorFollowModel::ePerspective: {
            KnobDoublePtr extraKnob = doubleKnob(*shape->item, kRotoDrawableItemExtraMatrixParam);
            std::map<int, FollowMatrix> before;
            for (int frame = *keys.begin(); frame <= *keys.rbegin(); ++frame) {
                FollowMatrix inverseExtra;
                if (extraMatrixAt(extraKnob, frame).inverted(&inverseExtra)) {
                    before[frame] = totalTransformAt(*shape->item, frame) * inverseExtra;
                } else {
                    before[frame] = FollowMatrix();
                }
            }
            shape->motion.reset(new MatrixCurvesMotion(extraKnob, shape->setup.corners, before, keys));
            break;
        }
        }
    }
}

void
VectorFollowRun::reduce(const std::function<bool()>& aborted)
{
    for (const std::unique_ptr<FollowShape>& shape : _imp->shapes) {
        if (!shape->motion) {
            continue;
        }
        shape->removed = reduceVectorFollowKeys(*shape->motion, shape->removable, _imp->params.tolerance, aborted);
        if (aborted()) {
            _imp->aborted = true;
            _imp->userStopped = true;

            return;
        }
    }
}

void
VectorFollowRun::applyReduction()
{
    for (const std::unique_ptr<FollowShape>& shape : _imp->shapes) {
        _imp->removeKeys(*shape);
        shape->motion.reset();
    }
}

VectorFollowReport
VectorFollowRun::finish(bool sourceLost,
                        const std::string& error,
                        UndoCommandPtr* undo)
{
    VectorFollowReport report;
    report.aborted = _imp->aborted;
    report.userStopped = _imp->userStopped;
    report.refused = _imp->refused;
    report.sourceLost = sourceLost;
    report.error = error;

    bool wroteAnything = false;
    for (const std::unique_ptr<FollowShape>& shape : _imp->shapes) {
        wroteAnything = wroteAnything || !shape->written.empty();
        VectorFollowShapeReport shapeReport;
        shapeReport.name = shape->points ? shape->points->getName() : shape->item->getScriptName();
        shapeReport.inferred = shape->outcome.inferred;
        shapeReport.passStops = shape->outcome.passStops;
        shapeReport.unfollowed = shape->outcome.unfollowed;
        shapeReport.stopped = shape->outcome.stopped;
        shapeReport.stopFrame = shape->outcome.stopFrame;
        report.shapes.push_back(shapeReport);
    }

    if (_imp->undo) {
        // With nothing written, only the keys that pin anchors are there to take back.
        if (sourceLost || !wroteAnything) {
            _imp->undo->undo();
        } else {
            _imp->undo->captureResult();
            if (undo) {
                *undo = _imp->undo;
            } else {
                _imp->context->getNode()->pushUndoCommand(_imp->undo);
            }
        }
    }
    _imp->undo.reset();
    _imp->tieSuspension.reset();
    _imp->context->refreshTrackLinkListeners();
    _imp->context->emitRefreshViewerOverlays();
    _imp->context->evaluateChange();

    return report;
}

NATRON_NAMESPACE_EXIT
