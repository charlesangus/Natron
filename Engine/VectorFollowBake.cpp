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
#include "Engine/VectorSampler.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr int kMaxStrokeSamples = 256;
constexpr const char* kTooFewKeys = "it has fewer than two keys within the frame range";
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

FollowMatrix
localTransformAt(const RotoDrawableItem& item,
                 double time)
{
    Transform::Matrix3x3 m;
    item.getLocalTransformAtTime(time, &m);

    return toFollowMatrix(m);
}

KnobDoublePtr
doubleKnob(const RotoDrawableItem& item,
           const char* name)
{
    return std::dynamic_pointer_cast<KnobDouble>(item.getKnobByName(name));
}

/*
 * The item's own translate is the outermost step of its own transform, but what comes before it,
 * such as a parent layer's rotation or scale, turns and scales a translate on its way to where it
 * shows. A translate fit's motion is where it shows, so these carry it through what comes before.
 * The total is what comes before times the own transform, so what comes before, inverted, is the
 * own transform times the total inverted.
 */
struct TranslateFrame {
    FollowMatrix before;
    // The point the translate moves, in the space the translate is in, less the translate itself.
    FollowPoint untranslated;
};

bool
translateFrameAt(const RotoDrawableItem& item,
                 double time,
                 const FollowPoint& point,
                 TranslateFrame* out)
{
    const FollowMatrix local = localTransformAt(item, time);
    FollowMatrix inverseLocal;
    if (!local.inverted(&inverseLocal)) {
        return false;
    }
    const KnobDoublePtr translate = doubleKnob(item, kRotoDrawableItemTranslateParam);
    const FollowPoint moved = local.apply(point);
    out->before = totalTransformAt(item, time) * inverseLocal;
    out->untranslated = FollowPoint(moved.x - translate->getValueAtTime(time, 0), moved.y - translate->getValueAtTime(time, 1));

    return true;
}

// The own translate at time that shows point, in the item's coordinates, at shown.
bool
translateShowingAt(const RotoDrawableItem& item,
                   double time,
                   const FollowPoint& point,
                   const FollowPoint& shown,
                   FollowPoint* translate)
{
    FollowMatrix inverseTotal;
    if (!totalTransformAt(item, time).inverted(&inverseTotal)) {
        return false;
    }
    TranslateFrame frame;
    if (!translateFrameAt(item, time, point, &frame)) {
        return false;
    }
    const FollowPoint target = localTransformAt(item, time).apply(inverseTotal.apply(shown));
    *translate = FollowPoint(target.x - frame.untranslated.x, target.y - frame.untranslated.y);

    return true;
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

struct FollowShape {
    RotoDrawableItemPtr item;
    BezierPtr bezier;
    // Set instead of item and bezier for points that are not a curve's.
    VectorFollowPointsPtr points;
    VectorFollowShapeSetup setup;
    // A fit whose item's own transform knobs do not drive it: the fit moves the anchors' points,
    // which are keyed instead.
    bool bakesFit = false;
    // Under eTranslate, the point in the item's coordinates whose shown position the fit drives.
    FollowPoint translatePivot;
    // The control points tracked one by one, when the model says so, or a baked fit moves.
    std::vector<int> tracked;
    // The shape's point count when the follow began, which the indices in tracked refer to.
    int pointCount = 0;
    bool feather = false;
    std::set<int> anchorFrames;
    std::set<int> written;
    VectorFollowShapeOutcome outcome;
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
        if (params.mode == VectorFollowMode::eBlendBetweenKeys) {
            params.direction = VectorFollowDirection::eForward;
        }
    }

    bool blends() const
    {
        return params.mode == VectorFollowMode::eBlendBetweenKeys;
    }

    std::unique_ptr<FollowShape> makeShape(const RotoDrawableItemPtr& item, std::string* refusal) const;
    std::unique_ptr<FollowShape> makePointsShape(const VectorFollowPointsPtr& points, std::string* refusal) const;
    bool addAnchorFrames(FollowShape& shape) const;
    std::set<int> keyFramesOf(const FollowShape& shape, int first, int last) const;
    bool writePoints(FollowShape& shape, double time, const std::vector<FollowPoint>& points) const;
    void writeFrame(FollowShape& shape, const VectorFollowFrame& frame) const;
    void orderByTransformSources();
};

/*
 * A shape's keys are converted through its transform at their frame, so a shape whose transform
 * comes in part from another followed shape is keyed after it: converted through that shape's old
 * transform, its keys would move again as the new one lands.
 */
void
VectorFollowRun::Imp::orderByTransformSources()
{
    std::vector<std::unique_ptr<FollowShape>> pending;
    pending.swap(shapes);
    std::vector<std::list<RotoItemPtr>> sources(pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i) {
        if (pending[i]->item) {
            sources[i] = context->getTransformSources(*pending[i]->item);
        }
    }
    while (!pending.empty()) {
        std::set<const RotoItem*> waiting;
        for (const std::unique_ptr<FollowShape>& shape : pending) {
            if (shape->item) {
                waiting.insert(shape->item.get());
            }
        }
        std::vector<std::unique_ptr<FollowShape>> later;
        std::vector<std::list<RotoItemPtr>> laterSources;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            bool waits = false;
            for (const RotoItemPtr& source : sources[i]) {
                waits = waits || ((source.get() != pending[i]->item.get()) && waiting.count(source.get()));
            }
            if (waits) {
                later.push_back(std::move(pending[i]));
                laterSources.push_back(sources[i]);
            } else {
                shapes.push_back(std::move(pending[i]));
            }
        }
        // Links never form a cycle, but should one, its shapes keep their order rather than hang.
        if (later.size() == pending.size()) {
            for (std::unique_ptr<FollowShape>& shape : later) {
                shapes.push_back(std::move(shape));
            }
            later.clear();
            laterSources.clear();
        }
        pending.swap(later);
        sources.swap(laterSources);
    }
}

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
            *refusal = "its transform is not its own, and it has no points to key";

            return nullptr;
        }
    }

    if (!addAnchorFrames(*shape)) {
        *refusal = kTooFewKeys;

        return nullptr;
    }

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
         * key that changed the shape otherwise, the nearest the model gets to it. The anchors keep
         * their points, which the walk moves by the fit, so a key that changed the shape keeps
         * that change instead of being replaced by the first anchor's shape.
         */
        const std::vector<FollowPoint> reference = shape->setup.anchors.front().points;
        const std::vector<double> weights(reference.size(), 1.);
        for (VectorFollowAnchor& anchor : shape->setup.anchors) {
            const VectorFollowFit fit = fitFollowMotion(shape->setup.model, reference, anchor.points, weights);
            anchor.transform = fit.valid ? fit.matrix : FollowMatrix();
        }
    }
    if (shape->setup.model != VectorFollowModel::ePerPoint) {
        const VectorFollowAnchor& firstAnchor = shape->setup.anchors.front();
        shape->setup.corners = boxCorners(firstAnchor.samples);
        FollowMatrix inverse;
        if (!firstAnchor.transform.inverted(&inverse)) {
            return nullptr;
        }
        if ((shape->setup.model == VectorFollowModel::eTranslate) && !shape->bakesFit) {
            // The translation fitted is where the box's centre shows, which each anchor's transform takes it to.
            FollowPoint centre;
            for (const FollowPoint& corner : shape->setup.corners) {
                centre.x += corner.x / (double)shape->setup.corners.size();
                centre.y += corner.y / (double)shape->setup.corners.size();
            }
            shape->translatePivot = inverse.apply(centre);
            for (VectorFollowAnchor& anchor : shape->setup.anchors) {
                const FollowPoint shown = anchor.transform.apply(shape->translatePivot);
                anchor.transform = FollowMatrix::translation(shown.x, shown.y);
            }
        } else {
            for (FollowPoint& corner : shape->setup.corners) {
                corner = inverse.apply(corner);
            }
        }
    }

    return shape;
}

bool
VectorFollowRun::Imp::addAnchorFrames(FollowShape& shape) const
{
    if (blends()) {
        shape.anchorFrames = keyFramesOf(shape, params.first, params.last);
        if (shape.anchorFrames.size() < 2) {
            return false;
        }
        // Out past the outer keys there is nothing to blend toward, so those frames stay as they are.
        shape.setup.first = *shape.anchorFrames.begin();
        shape.setup.last = *shape.anchorFrames.rbegin();

        return true;
    }
    // The start frame is keyed too, with what it shows: keys landing on the frames next to it would
    // otherwise change what it interpolates to.
    shape.anchorFrames.clear();
    shape.anchorFrames.insert(params.direction == VectorFollowDirection::eForward ? params.first : params.last);
    shape.setup.seedIsNew = true;

    return true;
}

std::unique_ptr<FollowShape>
VectorFollowRun::Imp::makePointsShape(const VectorFollowPointsPtr& points,
                                      std::string* refusal) const
{
    std::unique_ptr<FollowShape> shape(new FollowShape);
    shape->points = points;
    shape->setup.direction = params.direction;
    shape->setup.confidenceThreshold = params.confidenceThreshold;
    shape->setup.first = params.first;
    shape->setup.last = params.last;
    shape->setup.model = VectorFollowModel::ePerPoint;
    shape->setup.forwardOnly = forwardOnly;
    if (!addAnchorFrames(*shape)) {
        *refusal = kTooFewKeys;

        return nullptr;
    }
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
        if (writePoints(shape, time, frame.points)) {
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
    case VectorFollowModel::eTranslate: {
        FollowPoint translate;
        if (!translateShowingAt(*shape.item, time, shape.translatePivot, FollowPoint(frame.transform.m[2], frame.transform.m[5]), &translate)) {
            return;
        }
        shape.item->setTranslateKeyframe(time, translate.x, translate.y);
        break;
    }
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
    if (!VectorSampler::checkSource(params.source, _imp->context->getNode(), params.first, params.direction, params.layers, error)) {
        return false;
    }
    _imp->forwardOnly = !VectorSampler::hasMotionLayers(params.source, params.first, params.layers);

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
    _imp->orderByTransformSources();
    for (const VectorFollowPointsPtr& points : params.points) {
        if (!points) {
            continue;
        }
        std::string refusal;
        std::unique_ptr<FollowShape> shape = _imp->makePointsShape(points, &refusal);
        if (shape) {
            _imp->shapes.push_back(std::move(shape));
        } else if (!refusal.empty()) {
            _imp->refused.push_back(VectorFollowRefusal { points->getName(), refusal });
        }
    }
    if (_imp->shapes.empty()) {
        _imp->tieSuspension.reset();
        *error = _imp->blends() ? "None of the given shapes has two keys within the frame range to blend between." : "None of the given shapes can follow vectors.";
        for (const VectorFollowRefusal& refused : _imp->refused) {
            *error += " " + refused.name + ": " + refused.reason + ".";
        }

        return false;
    }

    _imp->undo = std::make_shared<FollowVectorsUndoCommand>(_imp->context, items, !params.points.empty());
    _imp->undo->setText(_imp->blends() ? kVectorFollowBlendActionName : kVectorFollowActionName);

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

VectorFollowReport
VectorFollowRun::finish(bool sourceLost,
                        const std::string& error,
                        UndoCommandPtr* undo)
{
    VectorFollowReport report;
    report.action = _imp->blends() ? kVectorFollowBlendActionName : kVectorFollowActionName;
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
    for (const std::unique_ptr<FollowShape>& shape : _imp->shapes) {
        if (shape->points) {
            shape->points->followEnded();
        }
    }
    _imp->context->refreshTrackLinkListeners();
    _imp->context->emitRefreshViewerOverlays();
    _imp->context->evaluateChange();

    return report;
}

NATRON_NAMESPACE_EXIT
