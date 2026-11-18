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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "OverlayPath.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_ENTER

namespace {

// Below this the matrix cannot be inverted in double precision.
const double kSingularDeterminant = 1e-24;

// A scale no larger than the smallest one a transform applies is a collapsed axis: a Transform
// scale of 0 is applied as TransformMath::kScaleMin, which inverts to a huge but finite scale.
const double kCollapsedScale = TransformMath::kScaleMin * (1. + 1e-6);

const int kMaxRetimeIterations = 8;
const double kRetimeTolerance = 1e-6;

// One edge of a path: node reads the next node of the path through input inputNb.
struct PathStep {
    EffectInstance* node;
    int inputNb;
};

class OverlayPathResolver {
public:
    explicit OverlayPathResolver(ViewIdx view)
        : _view(view)
    {
    }

    // The connected inputs of node, non-optional before optional, each from last to first.
    static std::vector<int> orderedInputs(EffectInstance* node)
    {
        std::vector<int> nonOptional;
        std::vector<int> optional;
        for (int i = node->getNInputs() - 1; i >= 0; --i) {
            if (!node->getInput(i)) {
                continue;
            }
            if (node->isInputOptional(i)) {
                optional.push_back(i);
            } else {
                nonOptional.push_back(i);
            }
        }
        nonOptional.insert(nonOptional.end(), optional.begin(), optional.end());

        return nonOptional;
    }

    // The first frame node reads from input inputNb at time; found is false when it reads none.
    // single is whether that is one frame rather than a range.
    double neededFrame(EffectInstance* node,
                       int inputNb,
                       double time,
                       bool* found,
                       bool* single) const
    {
        *found = false;
        *single = false;
        const FramesNeededMap needed = node->getFramesNeeded_public(node->getHash(), time, _view, 0);
        const FramesNeededMap::const_iterator input = needed.find(inputNb);
        if ((input == needed.end()) || input->second.empty()) {
            return time;
        }
        FrameRangesMap::const_iterator ranges = input->second.find(_view);
        if (ranges == input->second.end()) {
            ranges = input->second.begin();
        }
        if (ranges->second.empty()) {
            return time;
        }
        *found = true;
        *single = (ranges->second.size() == 1) && (ranges->second.front().min == ranges->second.front().max);

        return ranges->second.front().min;
    }

    // Whether node reads the single frame held from inputNb whatever its own time around time.
    bool isHold(EffectInstance* node,
                int inputNb,
                double time,
                double held) const
    {
        for (double probe : { time + 1., time - 1. }) {
            bool found = false;
            bool single = false;
            const double frame = neededFrame(node, inputNb, probe, &found, &single);
            if (!found || !single || (frame != held)) {
                return false;
            }
        }

        return true;
    }

    // The frame of input inputNb that node shows at time.
    double forwardTime(EffectInstance* node,
                       int inputNb,
                       double time,
                       bool* visible) const
    {
        if (node->getNode()->isNodeDisabled(time)) {
            return time;
        }
        bool found = false;
        bool single = false;
        const double frame = neededFrame(node, inputNb, time, &found, &single);
        if (!found || (frame == time)) {
            return time;
        }
        if (single && isHold(node, inputNb, time, frame) && (time != frame)) {
            *visible = false;
        }

        return frame;
    }

    // The frame of node that shows inputTime of input inputNb.
    double inverseTime(EffectInstance* node,
                       int inputNb,
                       double inputTime,
                       bool* visible) const
    {
        if (node->getNode()->isNodeDisabled(inputTime)) {
            return inputTime;
        }
        bool found = false;
        bool single = false;
        const double frame = neededFrame(node, inputNb, inputTime, &found, &single);
        if (!found || (frame == inputTime)) {
            return inputTime;
        }
        if (single && isHold(node, inputNb, inputTime, frame)) {
            if (inputTime != frame) {
                *visible = false;
            }

            return inputTime;
        }

        // Newton steps on the retime curve, its slope probed one frame on: exact at once for an
        // offset or a constant speed.
        double time = inputTime + (inputTime - frame);
        for (int iteration = 0; iteration < kMaxRetimeIterations; ++iteration) {
            const double at = neededFrame(node, inputNb, time, &found, &single);
            if (!found || (std::fabs(at - inputTime) < kRetimeTolerance)) {
                break;
            }
            const double next = neededFrame(node, inputNb, time + 1., &found, &single);
            const double slope = next - at;
            if (!found || (slope == 0.)) {
                break;
            }
            time += (inputTime - at) / slope;
        }
        const double rounded = std::round(time);
        if (std::fabs(rounded - time) < kRetimeTolerance) {
            time = rounded;
        }

        return time;
    }

    // The canonical transform node applies to input inputNb at time, identity when it applies none
    // or applies it to another input.
    Transform::Matrix3x3 edgeMatrix(EffectInstance* node,
                                    int inputNb,
                                    double time,
                                    StatusEnum* status,
                                    EffectInstancePtr* transformedInput = NULL) const
    {
        *status = eStatusReplyDefault;
        if (node->getNode()->isNodeDisabled(time)) {
            return Transform::Matrix3x3();
        }
        EffectInstancePtr input;
        Transform::Matrix3x3 mat;
        if (transformedInput) {
            *transformedInput = EffectInstancePtr();
        }
        // Draft mode, so a motion-blurred transform still reports its matrix.
        *status = node->getOverlayTransform_public(time, RenderScale::identity, true, _view, &input, &mat);
        if (*status != eStatusOK) {
            return Transform::Matrix3x3();
        }
        if (!input || ((inputNb >= 0) && (input != node->getInput(inputNb)))) {
            *status = eStatusReplyDefault;

            return Transform::Matrix3x3();
        }
        if (transformedInput) {
            *transformedInput = input;
        }
        const NodeMetadataConstPtr metadata = input->getMetadata(time, _view);
        const double par = metadata ? metadata->getPixelAspectRatio(-1) : 1.;

        // getOverlayTransform answers in pixel coordinates.
        mat = Transform::matMul(Transform::matPixelToCanonical(par, 1, 1, false), mat);

        return Transform::matMul(mat, Transform::matCanonicalToPixel(par, 1, 1, false));
    }

    static int inputIndexOf(EffectInstance* node,
                            const EffectInstancePtr& input)
    {
        for (int i = 0; i < node->getNInputs(); ++i) {
            if (node->getInput(i) == input) {
                return i;
            }
        }

        return -1;
    }

    // Depth-first from current down to target, following a transform's own input and otherwise
    // every input in preference order.
    bool forwardWalk(EffectInstance* current,
                     EffectInstance* target,
                     double time,
                     Transform::Matrix3x3* matrix,
                     double* targetTime,
                     bool* visible,
                     std::set<EffectInstance*>* explored) const
    {
        if (current == target) {
            *matrix = Transform::Matrix3x3();
            *targetTime = time;

            return true;
        }
        if (!explored->insert(current).second) {
            return false;
        }

        StatusEnum status = eStatusReplyDefault;
        EffectInstancePtr transformedInput;
        const Transform::Matrix3x3 local = edgeMatrix(current, -1, time, &status, &transformedInput);
        if (status == eStatusFailed) {
            return false;
        }
        std::vector<int> inputs;
        if (status == eStatusOK) {
            const int inputNb = inputIndexOf(current, transformedInput);
            if (inputNb >= 0) {
                inputs.push_back(inputNb);
            }
        } else {
            inputs = orderedInputs(current);
        }

        for (int inputNb : inputs) {
            bool legVisible = true;
            const double inputTime = forwardTime(current, inputNb, time, &legVisible);
            Transform::Matrix3x3 below;
            if (forwardWalk(current->getInput(inputNb).get(), target, inputTime, &below, targetTime, &legVisible, explored)) {
                *matrix = Transform::matMul(local, below);
                *visible = *visible && legVisible;

                return true;
            }
        }

        return false;
    }

    // Depth-first over inputs in preference order from node; steps gets the path to the first
    // node accept takes, node first. A node explored once is not explored again: nothing it reads
    // was taken then, nor will be.
    template <class Accept>
    static bool searchInputs(EffectInstance* node,
                             const Accept& accept,
                             std::vector<PathStep>* steps,
                             std::set<EffectInstance*>* explored)
    {
        if (accept(node)) {
            return true;
        }
        if (!explored->insert(node).second) {
            return false;
        }
        for (int inputNb : orderedInputs(node)) {
            steps->push_back(PathStep { node, inputNb });
            if (searchInputs(node->getInput(inputNb).get(), accept, steps, explored)) {
                return true;
            }
            steps->pop_back();
        }

        return false;
    }

    // Every node root reads, root included.
    static void collectAncestors(EffectInstance* root,
                                 std::set<EffectInstance*>* ancestors)
    {
        std::vector<EffectInstance*> stack(1, root);
        while (!stack.empty()) {
            EffectInstance* node = stack.back();
            stack.pop_back();
            if (!ancestors->insert(node).second) {
                continue;
            }
            for (int inputNb : orderedInputs(node)) {
                stack.push_back(node->getInput(inputNb).get());
            }
        }
    }

    // The preference-order path from root to target, root first; false when root does not read it.
    static bool pathTo(EffectInstance* root,
                       EffectInstance* target,
                       std::vector<PathStep>* steps)
    {
        std::set<EffectInstance*> explored;

        return searchInputs(root, [target](EffectInstance* node) { return node == target; }, steps, &explored);
    }

    // Composes steps forward from the time the first of them is shown at.
    Transform::Matrix3x3 composeForward(const std::vector<PathStep>& steps,
                                        double time,
                                        double* endTime,
                                        bool* visible) const
    {
        Transform::Matrix3x3 matrix;
        for (const PathStep& step : steps) {
            StatusEnum status = eStatusReplyDefault;
            matrix = Transform::matMul(matrix, edgeMatrix(step.node, step.inputNb, time, &status));
            if (status == eStatusFailed) {
                *visible = false;
            }
            time = forwardTime(step.node, step.inputNb, time, visible);
        }
        *endTime = time;

        return matrix;
    }

    // Composes steps forward, their times found back from the time the last one reads. The first
    // step is the overlay node's own edge: only its retime counts, as its overlay is drawn in the
    // space of its input.
    Transform::Matrix3x3 composeBackward(const std::vector<PathStep>& steps,
                                         double endTime,
                                         double* startTime,
                                         bool* visible) const
    {
        Transform::Matrix3x3 matrix;
        double time = endTime;
        for (std::vector<PathStep>::const_reverse_iterator step = steps.rbegin(); step != steps.rend(); ++step) {
            time = inverseTime(step->node, step->inputNb, time, visible);
            if (step + 1 == steps.rend()) {
                break;
            }
            StatusEnum status = eStatusReplyDefault;
            matrix = Transform::matMul(edgeMatrix(step->node, step->inputNb, time, &status), matrix);
            if (status == eStatusFailed) {
                *visible = false;
            }
        }
        *startTime = time;

        return matrix;
    }

private:
    ViewIdx _view;
};

// A viewer only displays its inputs, so a node it reads directly counts as the viewed node.
bool
isViewedNode(EffectInstance* viewed,
             EffectInstance* node)
{
    if (node == viewed) {
        return true;
    }
    if (!dynamic_cast<ViewerInstance*>(viewed)) {
        return false;
    }
    for (int i = 0; i < viewed->getNInputs(); ++i) {
        if (viewed->getInput(i).get() == node) {
            return true;
        }
    }

    return false;
}

// Whether m collapses the plane onto a line or a point: singular, or its derivative at the
// origin has a singular value no larger than kCollapsedScale.
bool
collapsesPlane(const Transform::Matrix3x3& m)
{
    const double det = Transform::matDeterminant(m);
    if (!std::isfinite(det) || (std::fabs(det) < kSingularDeterminant)) {
        return true;
    }
    if (m.i == 0.) {
        return false;
    }
    const double w2 = m.i * m.i;
    const double a = (m.a * m.i - m.c * m.g) / w2;
    const double b = (m.b * m.i - m.c * m.h) / w2;
    const double d = (m.d * m.i - m.f * m.g) / w2;
    const double e = (m.e * m.i - m.f * m.h) / w2;
    const double jacobianDet = std::fabs(a * e - b * d);
    const double sumSquares = a * a + b * b + d * d + e * e;
    const double largest = std::sqrt((sumSquares + std::sqrt(std::max(0., sumSquares * sumSquares - 4. * jacobianDet * jacobianDet))) / 2.);
    if (!std::isfinite(largest) || !(largest > 0.)) {
        return true;
    }

    return (jacobianDet / largest) <= kCollapsedScale;
}

// The node whose overlay space a Group's overlay is drawn in: its output node's input.
NodePtr
resolvedOverlayNode(const NodePtr& overlayNode)
{
    NodePtr target = overlayNode;
    while (target) {
        NodeGroup* group = dynamic_cast<NodeGroup*>(target->getEffectInstance().get());
        if (!group) {
            break;
        }
        target = group->getOutputNodeInput(false);
    }

    return target;
}

OverlayPathResult
resolveResolvedOverlayPath(EffectInstance* viewed,
                           EffectInstance* targetEffect,
                           double time,
                           ViewIdx view,
                           bool mapSiblings)
{
    OverlayPathResult result;
    result.time = time;
    if (!viewed || !targetEffect) {
        return result;
    }

    OverlayPathResolver resolver(view);

    {
        std::set<EffectInstance*> explored;
        Transform::Matrix3x3 matrix;
        double targetTime = time;
        bool visible = true;
        if (resolver.forwardWalk(viewed, targetEffect, time, &matrix, &targetTime, &visible, &explored)) {
            result.relation = OverlayPathRelation::eDownstream;
            result.matrix = matrix;
            result.time = targetTime;
            result.visible = visible;

            return result;
        }
    }

    std::set<EffectInstance*> viewedAncestors;
    OverlayPathResolver::collectAncestors(viewed, &viewedAncestors);

    std::vector<PathStep> fromOverlay;
    std::vector<PathStep> fromViewed;
    if (viewedAncestors.count(targetEffect)) {
        // Reached only through an input a transform does not move, such as a mask, or across a
        // leg whose transform failed, which the composition hides.
        result.relation = OverlayPathRelation::eDownstream;
        OverlayPathResolver::pathTo(viewed, targetEffect, &fromViewed);
        bool visible = true;
        double targetTime = time;
        result.matrix = resolver.composeForward(fromViewed, time, &targetTime, &visible);
        result.time = targetTime;
        result.visible = visible;

        return result;
    }

    EffectInstance* ancestor = NULL;
    {
        std::set<EffectInstance*> explored;
        const auto isViewed = [viewed, targetEffect, &ancestor](EffectInstance* node) {
            if ((node == targetEffect) || !isViewedNode(viewed, node)) {
                return false;
            }
            ancestor = node;

            return true;
        };
        if (OverlayPathResolver::searchInputs(targetEffect, isViewed, &fromOverlay, &explored)) {
            result.relation = OverlayPathRelation::eUpstream;
        }
    }
    if (!ancestor) {
        fromOverlay.clear();
        std::set<EffectInstance*> explored;
        const auto isShared = [targetEffect, &viewedAncestors, &ancestor](EffectInstance* node) {
            if ((node == targetEffect) || !viewedAncestors.count(node)) {
                return false;
            }
            ancestor = node;

            return true;
        };
        if (!OverlayPathResolver::searchInputs(targetEffect, isShared, &fromOverlay, &explored)) {
            return result;
        }
        result.relation = OverlayPathRelation::eSibling;
        if (!mapSiblings) {
            return result;
        }
    }
    OverlayPathResolver::pathTo(viewed, ancestor, &fromViewed);

    bool visible = true;
    double ancestorTime = time;
    const Transform::Matrix3x3 toViewed = resolver.composeForward(fromViewed, time, &ancestorTime, &visible);
    double overlayTime = ancestorTime;
    const Transform::Matrix3x3 fromAncestor = resolver.composeBackward(fromOverlay, ancestorTime, &overlayTime, &visible);

    if (collapsesPlane(fromAncestor)) {
        result.matrix = Transform::Matrix3x3();
        result.time = overlayTime;
        result.visible = false;

        return result;
    }
    result.matrix = Transform::matMul(toViewed, Transform::matInverse(fromAncestor, Transform::matDeterminant(fromAncestor)));
    result.time = overlayTime;
    result.visible = visible;

    return result;
} // resolveResolvedOverlayPath

} // anonymous namespace

OverlayPathResult
resolveOverlayPath(EffectInstance* viewed,
                   const NodePtr& overlayNode,
                   double time,
                   ViewIdx view,
                   bool mapSiblings)
{
    const NodePtr target = resolvedOverlayNode(overlayNode);

    return resolveResolvedOverlayPath(viewed, target ? target->getEffectInstance().get() : NULL, time, view, mapSiblings);
}

OverlayPathResult
OverlayPathCache::resolve(EffectInstance* viewed,
                          const NodePtr& overlayNode,
                          double time,
                          ViewIdx view,
                          bool mapSiblings)
{
    const NodePtr target = resolvedOverlayNode(overlayNode);
    const NodePtr viewedNode = viewed ? viewed->getNode() : NodePtr();
    if (!overlayNode || !target || !viewedNode) {
        return resolveResolvedOverlayPath(viewed, target ? target->getEffectInstance().get() : NULL, time, view, mapSiblings);
    }
    // Read before resolving, so an edit made while resolving leaves the entry stale.
    const U64 viewedMarks = viewedNode->getHashMarkCount();
    const U64 resolvedMarks = target->getHashMarkCount();

    std::map<const Node*, Entry>::iterator it = _entries.find(overlayNode.get());
    if ((it != _entries.end()) && (it->second.overlayNode.lock() == overlayNode) && (it->second.resolvedNode.lock() == target) && (it->second.viewed == viewed) && (it->second.viewedMarks == viewedMarks) && (it->second.resolvedMarks == resolvedMarks) && (it->second.time == time) && (it->second.view == view) && (it->second.mapSiblings == mapSiblings)) {
        return it->second.result;
    }

    const OverlayPathResult result = resolveResolvedOverlayPath(viewed, target->getEffectInstance().get(), time, view, mapSiblings);
    if (it == _entries.end()) {
        for (std::map<const Node*, Entry>::iterator entry = _entries.begin(); entry != _entries.end();) {
            if (entry->second.overlayNode.expired()) {
                entry = _entries.erase(entry);
            } else {
                ++entry;
            }
        }
        it = _entries.insert(std::make_pair(overlayNode.get(), Entry())).first;
    }
    Entry& entry = it->second;
    entry.overlayNode = overlayNode;
    entry.resolvedNode = target;
    entry.viewed = viewed;
    entry.viewedMarks = viewedMarks;
    entry.resolvedMarks = resolvedMarks;
    entry.time = time;
    entry.view = view;
    entry.mapSiblings = mapSiblings;
    entry.result = result;

    return result;
} // OverlayPathCache::resolve

void
OverlayPathCache::clear()
{
    _entries.clear();
}

bool
mapToOverlaySpace(const OverlayPathResult& result,
                  double x,
                  double y,
                  bool keepBehind,
                  double* overlayX,
                  double* overlayY)
{
    if (result.matrix.isIdentity()) {
        *overlayX = x;
        *overlayY = y;

        return true;
    }
    const double det = Transform::matDeterminant(result.matrix);
    if (!std::isfinite(det) || (std::fabs(det) < kSingularDeterminant)) {
        return false;
    }
    Transform::Point3D p(x, y, 1.);
    p = Transform::matApply(Transform::matInverse(result.matrix, det), p);
    if (p.z < 0.) {
        if (!keepBehind) {
            return false;
        }
        p.z = -p.z;
    }
    if (p.z == 0.) {
        return false;
    }
    *overlayX = p.x / p.z;
    *overlayY = p.y / p.z;

    return true;
}

NATRON_NAMESPACE_EXIT
