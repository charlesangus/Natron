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

#ifndef Engine_VectorFollow_h
#define Engine_VectorFollow_h

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief The curve follower's math. It knows nothing of nodes, images or shapes: everything is in
 * canonical coordinates, over a VectorField.
 **/

struct FollowPoint {
    double x = 0.;
    double y = 0.;

    FollowPoint() = default;

    FollowPoint(double x_,
                double y_)
        : x(x_)
        , y(y_)
    {
    }
};

// A homogeneous 3x3 matrix, by rows.
struct FollowMatrix {
    double m[9] = { 1., 0., 0., 0., 1., 0., 0., 0., 1. };

    static FollowMatrix translation(double tx, double ty);

    FollowPoint apply(const FollowPoint& p) const;
    FollowMatrix operator*(const FollowMatrix& other) const;

    // False, leaving out alone, when the matrix is singular.
    bool inverted(FollowMatrix* out) const;
};

struct VectorSample {
    double forwardX = 0.;
    double forwardY = 0.;
    double backwardX = 0.;
    double backwardY = 0.;
    double forwardConfidence = 1.;
    double backwardConfidence = 1.;
};

class VectorField {
public:
    virtual ~VectorField() { }

    // Readies a frame for sample(). False when it cannot be read, which ends the follow.
    virtual bool prepareFrame(int frame) = 0;

    // Forward is the displacement to frame + 1, Backward the displacement to frame - 1.
    virtual VectorSample sample(int frame, double x, double y) = 0;
};

enum class VectorFollowDirection {
    eForward,
    eBackward
};

enum class VectorFollowModel {
    ePerPoint,
    eTranslate,
    eAffine,
    ePerspective
};

constexpr double kVectorFollowDefaultConfidenceThreshold = 0.5;
constexpr double kVectorFollowDefaultTolerance = 0.5;
constexpr double kVectorFollowMinConfidentFraction = 0.25;

struct FollowTrajectory {
    int firstFrame = 0;
    // By frame, from firstFrame up.
    std::vector<std::vector<FollowPoint>> points;
    std::vector<std::vector<double>> confidence;

    const std::vector<FollowPoint>& pointsAt(int frame) const { return points[frame - firstFrame]; }
};

/**
 * @brief Carries points frame by frame from fromFrame to toFrame, by Forward going up and by Backward
 * going down. A point's confidence at a frame is that of the vector which brought it there, 1 at
 * fromFrame. False if a frame could not be read.
 **/
bool integrateFollowPoints(VectorField& field,
                           const std::vector<FollowPoint>& points,
                           int fromFrame,
                           int toFrame,
                           FollowTrajectory* out);

// 0 at a, 1 at b.
double followBridgeWeight(int frame, int a, int b);

/**
 * @brief Tracks forward from atA and backward from atB, then crossfades the two over [a, b], so the
 * result lands exactly on both.
 **/
bool bridgeFollowPoints(VectorField& field,
                        const std::vector<FollowPoint>& atA,
                        const std::vector<FollowPoint>& atB,
                        int a,
                        int b,
                        FollowTrajectory* out);

// The state a shape is known to have at a frame: an existing key, or the start of a follow.
struct VectorFollowAnchor {
    int frame = 0;
    // The tracked points; a fit tracks none and leaves this empty.
    std::vector<FollowPoint> points;
    // The grid inside the shape and its control points, which the shape's motion is measured on.
    std::vector<FollowPoint> samples;
    // The matrix a fit drives: the shape's transform, or its translation under eTranslate.
    FollowMatrix transform;
};

struct VectorFollowShapeSetup {
    VectorFollowModel model = VectorFollowModel::ePerPoint;
    VectorFollowDirection direction = VectorFollowDirection::eForward;
    double confidenceThreshold = kVectorFollowDefaultConfidenceThreshold;
    int first = 0;
    int last = 0;
    // By frame, up. With seedIsNew the one the follow starts from, the first going forward and the
    // last going backward, is a start state that is not a key yet.
    std::vector<VectorFollowAnchor> anchors;
    bool seedIsNew = false;
    // The source has no backward layer, so a bridge between two anchors has only its forward pass.
    bool forwardOnly = false;
    // A quad around the shape, in the coordinates its transform applies to. Its centre is what two
    // transforms blend about.
    std::vector<FollowPoint> corners;
};

struct VectorFollowFrame {
    int frame = 0;
    std::vector<FollowPoint> points;
    FollowMatrix transform;
    bool inferred = false;
};

struct VectorFollowFrameRange {
    int first = 0;
    int last = 0;

    bool empty() const { return first > last; }
};

enum class VectorFollowPassEnd {
    eLowConfidence,
    eUnreadable,
    eNoLayer
};

// A pass between two anchors that ended before it reached the far one, or a pass the source has no
// layer for, between two anchors or out from one.
struct VectorFollowPassStop {
    VectorFollowDirection direction = VectorFollowDirection::eForward;
    VectorFollowPassEnd reason = VectorFollowPassEnd::eLowConfidence;
    // The last frame it reached.
    int frame = 0;
    // The frames the other pass carried alone.
    VectorFollowFrameRange otherOnly = { 1, 0 };
};

struct VectorFollowShapeReport {
    std::string name;
    std::vector<VectorFollowFrameRange> inferred;
    std::vector<VectorFollowPassStop> passStops;
    // Frames between two anchors that neither pass reached, which keep their old motion.
    std::vector<VectorFollowFrameRange> unfollowed;
    bool stopped = false;
    int stopFrame = 0;
};

struct VectorFollowReport {
    std::vector<VectorFollowShapeReport> shapes;
    bool aborted = false;
    bool sourceLost = false;
    std::string error;
};

std::string formatVectorFollowReport(const VectorFollowReport& report);

// A follow that ran to its end on every shape with no pass stop and no unfollowed frame.
bool isCleanVectorFollowReport(const VectorFollowReport& report);

struct VectorFollowShapeOutcome {
    std::vector<VectorFollowFrameRange> inferred;
    std::vector<VectorFollowPassStop> passStops;
    std::vector<VectorFollowFrameRange> unfollowed;
    bool stopped = false;
    int stopFrame = 0;
    bool aborted = false;
};

/**
 * @brief Follows one shape over setup's range. Each frame is given to emit once it is final, so
 * whatever was emitted before an abort stands on its own. Anchors are not emitted, a new seed is.
 * Between two anchors, a pass that ends early leaves the frames past that point to the other pass
 * alone, which is reported and is no failure. Out from an anchor, a direction the source has no
 * layer for is reported and those frames are left alone. Calls emit and aborted on the calling
 * thread.
 **/
VectorFollowShapeOutcome followVectorShape(VectorField& field,
                                           const VectorFollowShapeSetup& setup,
                                           const std::function<void(const VectorFollowFrame&)>& emit,
                                           const std::function<bool()>& aborted);

NATRON_NAMESPACE_EXIT

#endif // Engine_VectorFollow_h
