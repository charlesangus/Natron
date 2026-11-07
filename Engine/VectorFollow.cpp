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

#include "VectorFollow.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include "Engine/VectorFollowModels.h"

NATRON_NAMESPACE_ENTER

FollowMatrix
FollowMatrix::translation(double tx,
                          double ty)
{
    FollowMatrix t;
    t.m[2] = tx;
    t.m[5] = ty;

    return t;
}

FollowPoint
FollowMatrix::apply(const FollowPoint& p) const
{
    const double w = m[6] * p.x + m[7] * p.y + m[8];
    const double x = m[0] * p.x + m[1] * p.y + m[2];
    const double y = m[3] * p.x + m[4] * p.y + m[5];
    if (w == 0.) {
        return FollowPoint(x, y);
    }

    return FollowPoint(x / w, y / w);
}

FollowMatrix
FollowMatrix::operator*(const FollowMatrix& o) const
{
    FollowMatrix r;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            r.m[row * 3 + col] = m[row * 3] * o.m[col] + m[row * 3 + 1] * o.m[3 + col] + m[row * 3 + 2] * o.m[6 + col];
        }
    }

    return r;
}

bool
FollowMatrix::inverted(FollowMatrix* out) const
{
    const double c0 = m[4] * m[8] - m[5] * m[7];
    const double c1 = m[5] * m[6] - m[3] * m[8];
    const double c2 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (det == 0.) {
        return false;
    }
    out->m[0] = c0 / det;
    out->m[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    out->m[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    out->m[3] = c1 / det;
    out->m[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    out->m[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    out->m[6] = c2 / det;
    out->m[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    out->m[8] = (m[0] * m[4] - m[1] * m[3]) / det;

    return true;
}

namespace {

void
displacementOf(const VectorSample& sample,
               bool forward,
               double* dx,
               double* dy,
               double* confidence)
{
    *dx = forward ? sample.forwardX : sample.backwardX;
    *dy = forward ? sample.forwardY : sample.backwardY;
    *confidence = forward ? sample.forwardConfidence : sample.backwardConfidence;
}

std::vector<FollowPoint>
blendPoints(const std::vector<FollowPoint>& a,
            const std::vector<FollowPoint>& b,
            double weight)
{
    std::vector<FollowPoint> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i].x = a[i].x * (1. - weight) + b[i].x * weight;
        out[i].y = a[i].y * (1. - weight) + b[i].y * weight;
    }

    return out;
}

FollowMatrix
blendTransforms(const VectorFollowShapeSetup& setup,
                const FollowMatrix& a,
                const FollowMatrix& b,
                double weight)
{
    std::vector<FollowPoint> atA, atB;
    atA.reserve(setup.corners.size());
    atB.reserve(setup.corners.size());
    for (const FollowPoint& corner : setup.corners) {
        atA.push_back(a.apply(corner));
        atB.push_back(b.apply(corner));
    }
    const VectorFollowFit fit = solveFollowMotion(setup.model, setup.corners, blendPoints(atA, atB, weight));

    return fit.valid ? fit.matrix : a;
}

class ShapeWalker {
public:
    enum StepResult {
        eStepDone,
        eStepStopped,
        eStepUnreadable
    };

    ShapeWalker(const VectorFollowShapeSetup& setup,
                const VectorFollowAnchor& anchor,
                bool forward)
        : _setup(setup)
        , _forward(forward)
        , _frame(anchor.frame)
        , _points(anchor.points)
        , _samples(anchor.samples)
        , _transform(anchor.transform)
        , _inferred(false)
    {
    }

    int frame() const { return _frame; }

    bool inferred() const { return _inferred; }

    const std::vector<FollowPoint>& points() const { return _points; }

    const FollowMatrix& transform() const { return _transform; }

    StepResult step(VectorField& field)
    {
        if (!field.prepareFrame(_frame)) {
            return eStepUnreadable;
        }

        std::vector<FollowPoint> moved(_samples.size());
        std::vector<double> weights(_samples.size());
        std::size_t confident = 0;
        for (std::size_t i = 0; i < _samples.size(); ++i) {
            double dx, dy, confidence;
            displacementOf(field.sample(_frame, _samples[i].x, _samples[i].y), _forward, &dx, &dy, &confidence);
            moved[i] = FollowPoint(_samples[i].x + dx, _samples[i].y + dy);
            const bool isConfident = confidence >= _setup.confidenceThreshold;
            weights[i] = isConfident ? confidence : 0.;
            confident += isConfident ? 1 : 0;
        }
        if (_samples.empty() || ((double)confident < kVectorFollowMinConfidentFraction * (double)_samples.size())) {
            return eStepStopped;
        }

        const VectorFollowFit fit = fitFollowMotion(_setup.model, _samples, moved, weights);
        if (!fit.valid) {
            return eStepStopped;
        }

        if (_setup.model == VectorFollowModel::ePerPoint) {
            stepPerPoint(field, fit, moved, weights);
        } else {
            for (FollowPoint& sample : _samples) {
                sample = fit.matrix.apply(sample);
            }
            _transform = fit.matrix * _transform;
            _inferred = confident < _samples.size();
        }
        _frame += _forward ? 1 : -1;

        return eStepDone;
    }

private:
    void stepPerPoint(VectorField& field,
                      const VectorFollowFit& fit,
                      const std::vector<FollowPoint>& moved,
                      const std::vector<double>& weights)
    {
        for (std::size_t i = 0; i < _samples.size(); ++i) {
            _samples[i] = (weights[i] > 0.) ? moved[i] : fit.matrix.apply(_samples[i]);
        }
        _inferred = false;
        for (FollowPoint& point : _points) {
            double dx, dy, confidence;
            displacementOf(field.sample(_frame, point.x, point.y), _forward, &dx, &dy, &confidence);
            if (confidence >= _setup.confidenceThreshold) {
                point = FollowPoint(point.x + dx, point.y + dy);
            } else {
                point = fit.matrix.apply(point);
                _inferred = true;
            }
        }
    }

    const VectorFollowShapeSetup& _setup;
    const bool _forward;
    int _frame;
    std::vector<FollowPoint> _points;
    std::vector<FollowPoint> _samples;
    FollowMatrix _transform;
    bool _inferred;
};

class ShapeFollower {
public:
    ShapeFollower(VectorField& field,
                  const VectorFollowShapeSetup& setup,
                  const std::function<void(const VectorFollowFrame&)>& emit,
                  const std::function<bool()>& aborted)
        : _field(field)
        , _setup(setup)
        , _emit(emit)
        , _aborted(aborted)
    {
    }

    VectorFollowShapeOutcome run()
    {
        const std::vector<VectorFollowAnchor>& anchors = _setup.anchors;
        if (anchors.empty()) {
            return _outcome;
        }
        if (_setup.seedIsNew) {
            VectorFollowFrame seed;
            seed.frame = anchors.front().frame;
            seed.points = anchors.front().points;
            seed.transform = anchors.front().transform;
            emitFrame(seed);
        }

        const bool forward = _setup.direction == VectorFollowDirection::eForward;
        const int count = (int)anchors.size();
        if (forward) {
            if (!oneWay(anchors.front(), false, _setup.first)) {
                return finish();
            }
            for (int i = 0; i + 1 < count; ++i) {
                if (!bridge(anchors[i], anchors[i + 1], true)) {
                    return finish();
                }
            }
            oneWay(anchors.back(), true, _setup.last);
        } else {
            if (!oneWay(anchors.back(), true, _setup.last)) {
                return finish();
            }
            for (int i = count - 1; i > 0; --i) {
                if (!bridge(anchors[i - 1], anchors[i], false)) {
                    return finish();
                }
            }
            oneWay(anchors.front(), false, _setup.first);
        }

        return finish();
    }

private:
    struct HeldState {
        std::vector<FollowPoint> points;
        FollowMatrix transform;
        bool inferred = false;
        bool stopped = false;
    };

    VectorFollowShapeOutcome finish()
    {
        for (std::set<int>::const_iterator it = _inferredFrames.begin(); it != _inferredFrames.end(); ++it) {
            if (!_outcome.inferred.empty() && (_outcome.inferred.back().last + 1 == *it)) {
                _outcome.inferred.back().last = *it;
            } else {
                VectorFollowFrameRange range;
                range.first = *it;
                range.last = *it;
                _outcome.inferred.push_back(range);
            }
        }

        return _outcome;
    }

    void emitFrame(const VectorFollowFrame& frame)
    {
        if (frame.inferred) {
            _inferredFrames.insert(frame.frame);
        }
        _emit(frame);
    }

    void recordStop(int frame)
    {
        if (!_outcome.stopped) {
            _outcome.stopped = true;
            _outcome.stopFrame = frame;
        }
    }

    // False when the walker's field could not go on or an abort was requested.
    bool stepWalker(ShapeWalker& walker,
                    bool* stopped)
    {
        if (_aborted()) {
            _outcome.aborted = true;

            return false;
        }
        switch (walker.step(_field)) {
        case ShapeWalker::eStepDone:
            break;
        case ShapeWalker::eStepStopped:
            recordStop(walker.frame());
            *stopped = true;
            break;
        case ShapeWalker::eStepUnreadable:
            _outcome.aborted = true;

            return false;
        }

        return true;
    }

    bool oneWay(const VectorFollowAnchor& anchor,
                bool forward,
                int end)
    {
        ShapeWalker walker(_setup, anchor, forward);
        while (walker.frame() != end) {
            if (forward ? (walker.frame() > end) : (walker.frame() < end)) {
                return true;
            }
            bool stopped = false;
            if (!stepWalker(walker, &stopped)) {
                return false;
            }
            if (stopped) {
                return true;
            }
            VectorFollowFrame frame;
            frame.frame = walker.frame();
            frame.points = walker.points();
            frame.transform = walker.transform();
            frame.inferred = walker.inferred();
            emitFrame(frame);
        }

        return true;
    }

    /*
     * The side away from the follow's direction is worked out first and only held in memory, then
     * the other side bakes frame by frame: an abort leaves a run of finished frames next to the
     * anchor the follow comes from.
     */
    bool bridge(const VectorFollowAnchor& a,
                const VectorFollowAnchor& b,
                bool forward)
    {
        const int gapFrames = b.frame - a.frame - 1;
        if (gapFrames <= 0) {
            return true;
        }

        std::vector<HeldState> far(gapFrames);
        {
            ShapeWalker walker(_setup, forward ? b : a, !forward);
            bool stopped = false;
            for (int n = 0; n < gapFrames; ++n) {
                if (!stopped && !stepWalker(walker, &stopped)) {
                    return false;
                }
                const int frame = forward ? (b.frame - 1 - n) : (a.frame + 1 + n);
                HeldState& state = far[frame - a.frame - 1];
                state.points = walker.points();
                state.transform = walker.transform();
                state.inferred = !stopped && walker.inferred();
                state.stopped = stopped;
            }
        }

        ShapeWalker walker(_setup, forward ? a : b, forward);
        bool stopped = false;
        for (int n = 0; n < gapFrames; ++n) {
            if (!stopped && !stepWalker(walker, &stopped)) {
                return false;
            }
            const int frameNumber = forward ? (a.frame + 1 + n) : (b.frame - 1 - n);
            const HeldState& other = far[frameNumber - a.frame - 1];
            if (stopped && other.stopped) {
                continue;
            }
            const double weight = followBridgeWeight(frameNumber, a.frame, b.frame);
            VectorFollowFrame frame;
            frame.frame = frameNumber;
            frame.points = forward ? blendPoints(walker.points(), other.points, weight)
                                   : blendPoints(other.points, walker.points(), weight);
            if (_setup.model != VectorFollowModel::ePerPoint) {
                frame.transform = forward ? blendTransforms(_setup, walker.transform(), other.transform, weight)
                                          : blendTransforms(_setup, other.transform, walker.transform(), weight);
            }
            frame.inferred = (!stopped && walker.inferred()) || other.inferred;
            emitFrame(frame);
        }

        return true;
    }

    VectorField& _field;
    const VectorFollowShapeSetup& _setup;
    const std::function<void(const VectorFollowFrame&)>& _emit;
    const std::function<bool()>& _aborted;
    VectorFollowShapeOutcome _outcome;
    std::set<int> _inferredFrames;
};

} // anonymous namespace

bool
integrateFollowPoints(VectorField& field,
                      const std::vector<FollowPoint>& points,
                      int fromFrame,
                      int toFrame,
                      FollowTrajectory* out)
{
    const bool forward = toFrame >= fromFrame;
    const int steps = forward ? (toFrame - fromFrame) : (fromFrame - toFrame);

    out->firstFrame = std::min(fromFrame, toFrame);
    out->points.assign(steps + 1, std::vector<FollowPoint>());
    out->confidence.assign(steps + 1, std::vector<double>());

    std::vector<FollowPoint> current(points);
    std::vector<double> confidence(points.size(), 1.);
    int frame = fromFrame;
    out->points[frame - out->firstFrame] = current;
    out->confidence[frame - out->firstFrame] = confidence;
    for (int n = 0; n < steps; ++n) {
        if (!field.prepareFrame(frame)) {
            return false;
        }
        for (std::size_t i = 0; i < current.size(); ++i) {
            double dx, dy;
            displacementOf(field.sample(frame, current[i].x, current[i].y), forward, &dx, &dy, &confidence[i]);
            current[i].x += dx;
            current[i].y += dy;
        }
        frame += forward ? 1 : -1;
        out->points[frame - out->firstFrame] = current;
        out->confidence[frame - out->firstFrame] = confidence;
    }

    return true;
}

double
followBridgeWeight(int frame,
                   int a,
                   int b)
{
    if (b <= a) {
        return 0.;
    }
    const double t = std::min(1., std::max(0., double(frame - a) / double(b - a)));

    return t * t * (3. - 2. * t);
}

bool
bridgeFollowPoints(VectorField& field,
                   const std::vector<FollowPoint>& atA,
                   const std::vector<FollowPoint>& atB,
                   int a,
                   int b,
                   FollowTrajectory* out)
{
    FollowTrajectory fromA, fromB;
    if (!integrateFollowPoints(field, atA, a, b, &fromA) || !integrateFollowPoints(field, atB, b, a, &fromB)) {
        return false;
    }
    out->firstFrame = a;
    out->points.resize(fromA.points.size());
    out->confidence.resize(fromA.points.size());
    for (int frame = a; frame <= b; ++frame) {
        const std::size_t index = frame - a;
        const double weight = followBridgeWeight(frame, a, b);
        out->points[index] = blendPoints(fromA.points[index], fromB.points[index], weight);
        out->confidence[index].resize(atA.size());
        for (std::size_t i = 0; i < atA.size(); ++i) {
            out->confidence[index][i] = fromA.confidence[index][i] * (1. - weight) + fromB.confidence[index][i] * weight;
        }
    }

    return true;
}

VectorFollowShapeOutcome
followVectorShape(VectorField& field,
                  const VectorFollowShapeSetup& setup,
                  const std::function<void(const VectorFollowFrame&)>& emit,
                  const std::function<bool()>& aborted)
{
    return ShapeFollower(field, setup, emit, aborted).run();
}

std::string
formatVectorFollowReport(const VectorFollowReport& report)
{
    std::ostringstream text;

    if (report.sourceLost) {
        text << "Follow Vectors: the vector source went away, so the follow was reverted.";

        return text.str();
    }
    if (!report.error.empty()) {
        text << "Follow Vectors: " << report.error << "\n";
    } else if (report.aborted) {
        text << "Follow Vectors was aborted. The keys of the completed frames stay.\n";
    } else {
        text << "Follow Vectors is done.\n";
    }
    for (const VectorFollowShapeReport& shape : report.shapes) {
        text << shape.name << ":";
        if (shape.inferred.empty() && !shape.stopped) {
            text << " followed the vectors on every frame";
        }
        if (!shape.inferred.empty()) {
            text << " inferred motion on frames";
            for (std::size_t i = 0; i < shape.inferred.size(); ++i) {
                text << (i ? ", " : " ") << shape.inferred[i].first;
                if (shape.inferred[i].last != shape.inferred[i].first) {
                    text << "-" << shape.inferred[i].last;
                }
            }
        }
        if (shape.stopped) {
            text << (shape.inferred.empty() ? "" : ";") << " stopped at frame " << shape.stopFrame
                 << ", where too few of its points had confident vectors";
        }
        text << "\n";
    }

    return text.str();
}

NATRON_NAMESPACE_EXIT
