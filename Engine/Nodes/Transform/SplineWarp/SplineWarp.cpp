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

#include "SplineWarp.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

namespace {

// Points per edge of the Source's region of definition sent through the forward map for the
// union bbox.
const int kRoDEdgeSamples = 32;

// Where a coordinate lands outside every image, so it reads black.
const float kOutsideCoord = -1e9f;

// Maps pixel coordinates of this node's input through inv, the inverse of the concatenated
// upstream transform, into the upstream image's pixels, and chains jacobian (when not null) with
// the projective map's derivative.
void
mapRowThrough(const Mat3& inv,
              int n,
              float* sx,
              float* sy,
              float* jacobian)
{
    const double* m = inv.m;

    for (int i = 0; i < n; ++i) {
        const double x = sx[i];
        const double y = sy[i];
        const double w = m[6] * x + m[7] * y + m[8];
        if (!(w > 0.)) {
            sx[i] = kOutsideCoord;
            sy[i] = kOutsideCoord;
            continue;
        }
        const double qx = (m[0] * x + m[1] * y + m[2]) / w;
        const double qy = (m[3] * x + m[4] * y + m[5]) / w;
        sx[i] = (float)qx;
        sy[i] = (float)qy;
        if (!jacobian) {
            continue;
        }
        const double d00 = (m[0] - qx * m[6]) / w;
        const double d01 = (m[1] - qx * m[7]) / w;
        const double d10 = (m[3] - qy * m[6]) / w;
        const double d11 = (m[4] - qy * m[7]) / w;
        float* J = &jacobian[(std::size_t)i * 4];
        const double j0 = J[0], j1 = J[1], j2 = J[2], j3 = J[3];
        J[0] = (float)(d00 * j0 + d01 * j2);
        J[1] = (float)(d00 * j1 + d01 * j3);
        J[2] = (float)(d10 * j0 + d11 * j2);
        J[3] = (float)(d10 * j1 + d11 * j3);
    }
}

void
mergePoint(RectD* r,
           bool* any,
           double x,
           double y)
{
    if (!*any) {
        r->set(x, y, x, y);
        *any = true;
    } else {
        r->merge(x, y, x, y);
    }
}

} // anonymous namespace

SplineWarp::SplineWarp(NodePtr node)
    : StagedImageEffect(node)
{
}

SplineWarp::~SplineWarp()
{
}

const NodeSpec*
SplineWarp::nodeSpec() const
{
    return &kSplineWarpNodeSpec;
}

const ImagePipelineSpec&
SplineWarp::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kSplineWarpPipeline, kSplineWarpNodeSpec>();
}

std::string
SplineWarp::getInputHint(int inputNb) const
{
    if (inputNb == kSplineWarpInputSource) {
        return tr("The image to warp.").toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

EffectInstance::CurveContextKindEnum
SplineWarp::getCurveContextKind() const
{
    return eCurveContextKindGeometry;
}

RotoCurveContextPtr
SplineWarp::createGeometryCurveContext(const NodePtr& node) const
{
    return SplineWarpContext::create(node);
}

void
SplineWarp::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobDoublePtr amount = createKnob<KnobDouble>(tr("Amount"));
    amount->setName(kSplineWarpParamAmount);
    amount->setHintToolTip(tr("How far every link moves its destination toward its source before the solve, on top of "
                              "each link's own amount: at 0 nothing moves, at 1 each source curve lands on its "
                              "destination curve."));
    amount->setDefaultValue(1.);
    amount->setMinimum(0.);
    amount->setMaximum(1.);
    amount->setDisplayMinimum(0.);
    amount->setDisplayMaximum(1.);
    page->addKnob(amount);
    _amount = amount;

    KnobBoolPtr invert = createKnob<KnobBool>(tr("Invert"));
    invert->setName(kSplineWarpParamInvert);
    invert->setHintToolTip(tr("Swap the source and destination of every link, so the warp goes the other way."));
    invert->setDefaultValue(false);
    page->addKnob(invert);
    _invert = invert;

    KnobChoicePtr solver = createKnob<KnobChoice>(tr("Solver"));
    solver->setName(kSplineWarpParamSolver);
    solver->setHintToolTip(tr("How the warp between the curves is computed."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kSplineWarpSolverTps, tr("Thin Plate Spline").toStdString(),
                                       tr("A thin-plate spline through the curve samples: the smoothest map that moves "
                                          "each sample where it is asked to.")
                                           .toStdString()));
        options.push_back(ChoiceOption(kSplineWarpSolverMesh, tr("Mesh").toStdString(),
                                       tr("A triangle mesh through the curve samples, its edges along the curves: "
                                          "each triangle moves on its own, so the warp stays local to the curves "
                                          "and eases out to the edges of the image.")
                                           .toStdString()));
        solver->populateChoices(options);
    }
    solver->setDefaultValue(0);
    solver->setAnimationEnabled(false);
    page->addKnob(solver);
    _solver = solver;

    KnobDoublePtr smoothness = createKnob<KnobDouble>(tr("Smoothness"));
    smoothness->setName(kSplineWarpParamSmoothness);
    smoothness->setHintToolTip(tr("How much the solve may stray from the curves to keep the warp smooth. At 0 every "
                                  "curve sample lands exactly where it is asked to; higher values relax the fit, which "
                                  "tames folds where curves pull against each other."));
    smoothness->setDefaultValue(0.);
    smoothness->setMinimum(0.);
    smoothness->setDisplayMinimum(0.);
    smoothness->setDisplayMaximum(1.);
    page->addKnob(smoothness);
    _smoothness = smoothness;

    KnobDoublePtr curveResolution = createKnob<KnobDouble>(tr("Curve Resolution"));
    curveResolution->setName(kSplineWarpParamCurveResolution);
    curveResolution->setHintToolTip(tr("The distance, in full-resolution pixels, between consecutive samples along the "
                                       "curves. Smaller values follow the curves more closely and cost more to solve; "
                                       "the number of samples is capped."));
    curveResolution->setDefaultValue(4.);
    curveResolution->setMinimum(0.5);
    curveResolution->setDisplayMinimum(1.);
    curveResolution->setDisplayMaximum(32.);
    curveResolution->setAnimationEnabled(false);
    page->addKnob(curveResolution);
    _curveResolution = curveResolution;

    KnobDoublePtr fieldResolution = createKnob<KnobDouble>(tr("Field Resolution"));
    fieldResolution->setName(kSplineWarpParamFieldResolution);
    fieldResolution->setHintToolTip(tr("The spacing, in full-resolution pixels, of the grid the warp is evaluated on; "
                                       "pixels between grid points are interpolated. Smaller values are more accurate "
                                       "and slower."));
    fieldResolution->setDefaultValue(8.);
    fieldResolution->setMinimum(1.);
    fieldResolution->setDisplayMinimum(1.);
    fieldResolution->setDisplayMaximum(64.);
    fieldResolution->setAnimationEnabled(false);
    page->addKnob(fieldResolution);
    _fieldResolution = fieldResolution;

    KnobChoicePtr filter = createKnob<KnobChoice>(tr(kResamplerParamFilterTypeLabel));
    filter->setName(kResamplerParamFilterType);
    filter->setHintToolTip(tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)Resampler::eFilterCubic);
    filter->setAddNewLine(false);
    filter->setSpacingBetweenItems(1);
    page->addKnob(filter);
    _filter = filter;

    KnobBoolPtr clamp = createKnob<KnobBool>(tr(kResamplerParamFilterClampLabel));
    clamp->setName(kResamplerParamFilterClamp);
    clamp->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clamp->setDefaultValue(false);
    clamp->setAddNewLine(false);
    clamp->setSpacingBetweenItems(1);
    page->addKnob(clamp);
    _clamp = clamp;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutside->setName(kResamplerParamFilterBlackOutside);
    blackOutside->setHintToolTip(tr(kResamplerParamFilterBlackOutsideHint));
    blackOutside->setDefaultValue(true);
    page->addKnob(blackOutside);
    _blackOutside = blackOutside;

    KnobChoicePtr bbox = createKnob<KnobChoice>(tr("BBox"));
    bbox->setName(kSplineWarpParamBBox);
    bbox->setHintToolTip(tr("The region of definition of the output."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kSplineWarpBBoxSource, tr("Source").toStdString(),
                                       tr("The Source's region of definition.").toStdString()));
        options.push_back(ChoiceOption(kSplineWarpBBoxUnion, tr("Union").toStdString(),
                                       tr("The Source's region of definition, grown to hold where the warp moves its "
                                          "edges and the destination curves.")
                                           .toStdString()));
        bbox->populateChoices(options);
    }
    bbox->setDefaultValue(0);
    bbox->setAnimationEnabled(false);
    page->addKnob(bbox);
    _bbox = bbox;

    addStageKnobs(page);

    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
    updateSmoothnessVisibility();
} // SplineWarp::initializeKnobs

void
SplineWarp::updateClampEnabled(double time,
                               ViewIdx view)
{
    KnobChoicePtr filter = _filter.lock();
    KnobBoolPtr clamp = _clamp.lock();

    if (!filter || !clamp) {
        return;
    }
    clamp->setAllDimensionsEnabled(Resampler::filterHonoursClamp((Resampler::FilterEnum)filter->getValueAtTime(time, 0, view)));
}

void
SplineWarp::updateSmoothnessVisibility()
{
    KnobChoicePtr solver = _solver.lock();
    KnobDoublePtr smoothness = _smoothness.lock();

    if (!solver || !smoothness) {
        return;
    }
    smoothness->setSecret(solver->getValue() == 1);
}

void
SplineWarp::onKnobsLoaded()
{
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
    updateSmoothnessVisibility();
}

bool
SplineWarp::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec view,
                        double time,
                        bool /*originatedFromMainThread*/)
{
    KnobChoicePtr filter = _filter.lock();

    if (filter && (k == filter.get())) {
        updateClampEnabled(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }
    KnobChoicePtr solver = _solver.lock();
    if (solver && (k == solver.get())) {
        updateSmoothnessVisibility();

        return true;
    }

    return false;
}

void
SplineWarp::readSettings(double time,
                         ViewIdx view,
                         Settings* settings) const
{
    KnobDoublePtr amount = _amount.lock();
    if (amount) {
        settings->amount = std::max(0., std::min(1., amount->getValueAtTime(time, 0, view)));
    }
    KnobBoolPtr invert = _invert.lock();
    if (invert) {
        settings->invert = invert->getValueAtTime(time, 0, view);
    }
    KnobChoicePtr solver = _solver.lock();
    if (solver) {
        settings->mesh = solver->getValueAtTime(time, 0, view) == 1;
    }
    KnobDoublePtr smoothness = _smoothness.lock();
    if (smoothness) {
        settings->smoothness = std::max(0., smoothness->getValueAtTime(time, 0, view));
    }
    KnobDoublePtr curveResolution = _curveResolution.lock();
    if (curveResolution) {
        settings->curveResolution = std::max(0.5, curveResolution->getValueAtTime(time, 0, view));
    }
    KnobDoublePtr fieldResolution = _fieldResolution.lock();
    if (fieldResolution) {
        settings->fieldResolution = std::max(1., fieldResolution->getValueAtTime(time, 0, view));
    }
    KnobChoicePtr filter = _filter.lock();
    if (filter) {
        const int value = filter->getValueAtTime(time, 0, view);
        if ((value >= (int)Resampler::eFilterImpulse) && (value <= (int)Resampler::eFilterNotch)) {
            settings->filter = (Resampler::FilterEnum)value;
        }
    }
    KnobBoolPtr clamp = _clamp.lock();
    if (clamp) {
        settings->clamp = clamp->getValueAtTime(time, 0, view);
    }
    KnobBoolPtr blackOutside = _blackOutside.lock();
    if (blackOutside) {
        settings->blackOutside = blackOutside->getValueAtTime(time, 0, view);
    }
    KnobChoicePtr bbox = _bbox.lock();
    if (bbox) {
        settings->bboxUnion = bbox->getValueAtTime(time, 0, view) == 1;
    }
} // SplineWarp::readSettings

RectD
SplineWarp::getSourceBounds(double time,
                            ViewIdx view)
{
    EffectInstancePtr input = getInput(kSplineWarpInputSource);
    RectD rod;

    if (!input) {
        return rod;
    }
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, &rod, &isProjectFormat) == eStatusFailed) {
        return RectD();
    }

    return rod;
}

bool
SplineWarp::computeWarp(double time,
                        ViewIdx view,
                        const Settings& settings,
                        Warp* warp)
{
    *warp = Warp();
    NodePtr node = getNode();
    SplineWarpContextPtr context = node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
    if (!context) {
        return false;
    }
    if (settings.amount == 0.) {
        return true;
    }

    SampleKey key;
    key.contextHash = context->getContentHash();
    key.time = time;
    key.amount = settings.amount;
    key.invert = settings.invert;
    key.spacing = settings.curveResolution;
    SplineWarpSamplingSettings sampling;
    sampling.time = time;
    sampling.globalAmount = settings.amount;
    sampling.invert = settings.invert;
    sampling.spacing = settings.curveResolution;
    warp->samples = _samples.get(key, [&]() {
        return sampleSplineWarp(*context, sampling);
    });
    const SplineWarpSampleSet& samples = *warp->samples;

    // Boundary pairs never move, so only a link pair can, and a boundary holding none of them
    // keeps everything still.
    bool moves = false;
    if (!samples.hasBoundaries() || samples.linkSamplesInsideBoundaries) {
        for (int i = 0; i < samples.linkPairCount && !moves; ++i) {
            const SplineWarpSamplePair& pair = samples.pairs[i];
            moves = (pair.destination.x != pair.source.x) || (pair.destination.y != pair.source.y);
        }
    }
    if (!moves) {
        return true;
    }

    ContentHasher h;
    h.appendU64(samples.hash);
    h.appendBool(settings.mesh);
    if (settings.mesh) {
        warp->meshBox = SplineWarpMesh::paddedBox(samples.pairs, getSourceBounds(time, view));
        warp->mesh = _meshes.get(samples.hash, samples.pairs, samples.polylines, warp->meshBox);
        warp->identity = !warp->mesh || warp->mesh->isIdentity();
        h.appendDouble(warp->meshBox.x1);
        h.appendDouble(warp->meshBox.y1);
        h.appendDouble(warp->meshBox.x2);
        h.appendDouble(warp->meshBox.y2);
    } else {
        warp->map = _maps.get(samples.hash, settings.smoothness, samples.pairs);
        warp->identity = !warp->map || warp->map->isIdentity();
        h.appendDouble(settings.smoothness);
    }
    warp->hash = h.finalize();

    return true;
} // SplineWarp::computeWarp

SplineWarpFieldConstPtr
SplineWarp::computeField(const Warp& warp,
                         double time,
                         const Settings& settings,
                         const RectI& window,
                         double scaleX,
                         double scaleY)
{
    if (warp.identity || !warp.samples) {
        return SplineWarpFieldConstPtr();
    }
    if (warp.mesh) {
        return _fields.get(warp.hash, time, *warp.mesh, warp.samples->boundaries, window, scaleX, scaleY, settings.fieldResolution);
    }
    if (!warp.map) {
        return SplineWarpFieldConstPtr();
    }

    return _fields.get(warp.hash, time, *warp.map, warp.samples->boundaries, window, scaleX, scaleY, settings.fieldResolution);
}

double
SplineWarp::getOutputPixelAspectRatio(double time,
                                      ViewIdx view) const
{
    NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    const double par = metadata ? metadata->getPixelAspectRatio(-1) : 1.;

    return (par > 0.) ? par : 1.;
}

bool
SplineWarp::getInputsHoldingTransform(std::list<int>* inputs) const
{
    if (!inputs) {
        return false;
    }
    inputs->push_back(kSplineWarpInputSource);

    return true;
}

StatusEnum
SplineWarp::getRegionOfDefinition(U64 /*hash*/,
                                  double time,
                                  const RenderScale& scale,
                                  ViewIdx view,
                                  RectD* rod)
{
    *rod = RectD();
    EffectInstancePtr input = getInput(kSplineWarpInputSource);
    if (!input) {
        return eStatusOK;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;
    const StatusEnum status = input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat);
    if (status == eStatusFailed) {
        *rod = RectD();

        return status;
    }

    Settings settings;
    readSettings(time, view, &settings);
    if (!settings.bboxUnion || rod->isNull()) {
        return status;
    }
    Warp warp;
    if (!computeWarp(time, view, settings, &warp) || warp.identity) {
        return status;
    }
    const SplineWarpSampleSet& samples = *warp.samples;

    // Nothing outside every boundary's feather moves.
    std::vector<RectD> feathers;
    for (std::size_t b = 0; b < samples.boundaries.size(); ++b) {
        RectD bounds;
        bool any = false;
        const std::vector<Point>& feather = samples.boundaries[b].feather;
        for (std::size_t i = 0; i < feather.size(); ++i) {
            mergePoint(&bounds, &any, feather[i].x, feather[i].y);
        }
        if (any) {
            feathers.push_back(bounds);
        }
    }
    const auto canMove = [&](double x, double y) {
        if (!samples.hasBoundaries()) {
            return true;
        }
        for (std::size_t b = 0; b < feathers.size(); ++b) {
            if ((x >= feathers[b].x1) && (x <= feathers[b].x2) && (y >= feathers[b].y1) && (y <= feathers[b].y2)) {
                return true;
            }
        }

        return false;
    };

    RectD extra;
    bool any = false;
    for (int i = 0; i < samples.linkPairCount; ++i) {
        const Point& p = samples.pairs[i].destination;
        if (canMove(p.x, p.y)) {
            mergePoint(&extra, &any, p.x, p.y);
        }
    }
    if (!rod->isInfinite()) {
        // The forward map sends Source positions to where the warp shows them.
        std::vector<SplineWarpSamplePair> swapped(samples.pairs.size());
        for (std::size_t i = 0; i < samples.pairs.size(); ++i) {
            swapped[i].destination = samples.pairs[i].source;
            swapped[i].source = samples.pairs[i].destination;
        }
        std::function<Point(double, double)> forward;
        if (settings.mesh) {
            SplineWarpMeshConstPtr mesh = _forwardMeshes.get(samples.hash, swapped, samples.polylines, warp.meshBox);
            if (mesh && !mesh->isIdentity()) {
                forward = [mesh](double x, double y) {
                    return mesh->evaluate(x, y);
                };
            }
        } else {
            SplineWarpTpsConstPtr map = _forwardMaps.get(samples.hash, settings.smoothness, swapped);
            if (map && !map->isIdentity()) {
                forward = [map](double x, double y) {
                    return map->evaluate(x, y);
                };
            }
        }
        if (forward) {
            const RectD edges = *rod;
            for (int k = 0; k <= kRoDEdgeSamples; ++k) {
                const double t = (double)k / kRoDEdgeSamples;
                const double x = edges.x1 + t * (edges.x2 - edges.x1);
                const double y = edges.y1 + t * (edges.y2 - edges.y1);
                const Point points[4] = { { x, edges.y1 }, { x, edges.y2 }, { edges.x1, y }, { edges.x2, y } };
                for (int e = 0; e < 4; ++e) {
                    if (!canMove(points[e].x, points[e].y)) {
                        continue;
                    }
                    const Point q = forward(points[e].x, points[e].y);
                    mergePoint(&extra, &any, q.x, q.y);
                }
            }
        }
    }
    if (any) {
        rod->merge(extra);
    }

    return status;
} // SplineWarp::getRegionOfDefinition

void
SplineWarp::getRegionsOfInterest(double time,
                                 const RenderScale& scale,
                                 const RectD& outputRoD,
                                 const RectD& renderWindow,
                                 ViewIdx view,
                                 RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kSplineWarpInputSource);
    if (!source) {
        return;
    }
    Settings settings;
    readSettings(time, view, &settings);
    const double par = getOutputPixelAspectRatio(time, view);
    const OfxPointD s = scale.toOfxPointD();

    RectD region = renderWindow;
    Warp warp;
    if (computeWarp(time, view, settings, &warp) && !warp.identity) {
        const RectI window = PassExec::toPixelEnclosing(renderWindow, scale, par);
        SplineWarpFieldConstPtr field = computeField(warp, time, settings, window, s.x / par, s.y);
        if (field && !field->isIdentity()) {
            const double grow = Resampler::filterRoIExpansion(settings.filter);
            const RectD bounds = field->getSourceBounds();
            region.merge(bounds.x1 - grow * par / s.x, bounds.y1 - grow / s.y, bounds.x2 + grow * par / s.x, bounds.y2 + grow / s.y);
        }
    }

    const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
    RectD sourceRoD;
    bool isProjectFormat = false;
    if ((source->getRegionOfDefinition_public(source->getRenderHash(), time, inputScale, view, &sourceRoD, &isProjectFormat) != eStatusFailed) && !sourceRoD.isNull()) {
        region.clipIfOverlaps(sourceRoD);
    }
    (*ret)[source] = region;
} // SplineWarp::getRegionsOfInterest

bool
SplineWarp::isOpIdentity(double time,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx view)
{
    if (!getInput(kSplineWarpInputSource)) {
        return true;
    }
    Settings settings;
    readSettings(time, view, &settings);
    Warp warp;
    if (!computeWarp(time, view, settings, &warp)) {
        return true;
    }

    return warp.identity;
}

StatusEnum
SplineWarp::renderOp(const RenderActionArgs& args,
                     const StagePlan& plan)
{
    const float mix = (float)plan.mix;
    const bool maskInvert = plan.maskInvert;
    const bool passThrough = plan.stages.has(Stage::PassThroughUnprocessed);
    const int sourceInput = plan.sourceInput;
    Settings settings;
    readSettings(args.time, args.view, &settings);
    if (args.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    const bool impulse = settings.filter == Resampler::eFilterImpulse;
    const OfxPointD s = args.mappedScale.toOfxPointD();
    const double par = getOutputPixelAspectRatio(args.time, args.view);

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    Warp warp;
    computeWarp(args.time, args.view, settings, &warp);
    SplineWarpFieldConstPtr field = computeField(warp, args.time, settings, roi, s.x / par, s.y);
    if (!field) {
        field = std::make_shared<const SplineWarpField>();
    }

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = plan.applyMask;
    ImagePtr mask;
    const int maskChannel = plan.maskChannel;
    if (doMask) {
        if (maskChannel >= 0) {
            RectI maskRoI;
            mask = getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && (maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }
    if (!PassExec::isFloatImage(mask)) {
        return eStatusFailed;
    }

    struct Plane {
        ImagePtr dst;
        ImagePtr src;
        int nComps = 0;
        int srcNComps = 0;
        std::bitset<4> channels;
        RectI srcBounds;
        Transform::Matrix3x3Ptr inputTransform;
        bool concatenated = false;
        Mat3 inverse;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::shared_ptr<Image::WriteAccess> dstAccess;
        std::vector<float> converted;
        Resampler::SourceImage source;
    };
    std::vector<std::unique_ptr<Plane>> planes;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        std::unique_ptr<Plane> plane(new Plane());
        plane->dst = it->second;
        if (plane->dst) {
            plane->nComps = (int)plane->dst->getComponentsCount();
            plane->channels = PassExec::processedBitsForImage(it->first, plane->nComps, args.processChannels);
            ImageLayerDesc sourceLayer;
            if ((sourceInput >= 0) && getInput(sourceInput) && resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
                // Mapped to the clip's components, which is the layout the output plane is rendered in.
                RectI srcRoI;
                plane->src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &srcRoI, &plane->inputTransform);
            }
            if (!PassExec::isFloatImage(plane->dst) || !PassExec::isFloatImage(plane->src)) {
                return eStatusFailed;
            }
            // The fetched image is upstream of a concatenated transform, which maps its pixels to
            // this input's.
            if (plane->src && plane->inputTransform) {
                plane->concatenated = TransformMath::fromEngineMatrix(*plane->inputTransform).inverse(&plane->inverse);
                if (!plane->concatenated) {
                    plane->src.reset();
                }
            }
        }
        planes.push_back(std::move(plane));
    }

    // Images are locked and their bounds read here, on the calling thread; the band threads only
    // read through these.
    PassExec::MaskInput maskSource;
    maskSource.image = mask;
    maskSource.lock();
    for (std::size_t p = 0; p < planes.size(); ++p) {
        Plane& plane = *planes[p];
        if (!plane.dst) {
            continue;
        }
        const int nComps = plane.nComps;
        if (plane.src) {
            plane.srcBounds = plane.src->getBounds();
            const int srcNComps = (int)plane.src->getComponentsCount();
            plane.srcNComps = srcNComps;
            plane.srcAccess = std::make_shared<Image::ReadAccess>(plane.src.get());
            if (srcNComps == nComps) {
                plane.source = Resampler::SourceImage((const float*)plane.srcAccess->pixelAt(plane.srcBounds.x1, plane.srcBounds.y1), plane.srcBounds, nComps);
            } else {
                const int srcWidth = plane.srcBounds.width();
                plane.converted.assign((std::size_t)srcWidth * plane.srcBounds.height() * nComps, 0.f);
                for (int y = plane.srcBounds.y1; y < plane.srcBounds.y2; ++y) {
                    PassExec::readSourceRow(plane.src.get(), plane.srcAccess.get(), plane.srcBounds, srcNComps, plane.srcBounds.x1, y, srcWidth, nComps,
                                            &plane.converted[(std::size_t)(y - plane.srcBounds.y1) * srcWidth * nComps]);
                }
                plane.source = Resampler::SourceImage(plane.converted.empty() ? (const float*)0 : &plane.converted[0], plane.srcBounds, nComps);
            }
        } else {
            plane.source = Resampler::SourceImage((const float*)0, RectI(), nComps);
        }
        plane.dstAccess = std::make_shared<Image::WriteAccess>(plane.dst.get());
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeStageRowBands(roi, nThreads, &bandRects);
    std::vector<std::pair<std::size_t, RectI>> bands;
    for (std::size_t p = 0; p < planes.size(); ++p) {
        if (!planes[p]->dst) {
            continue;
        }
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(std::make_pair(p, bandRects[b]));
        }
    }

    RenderCancellation cancel(this);
    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const Plane& plane = *planes[bands[bandIndex].first];
        const RectI& band = bands[bandIndex].second;
        const int nComps = plane.nComps;
        const bool hasSource = plane.source.isValid();
        const std::size_t rowSize = (std::size_t)width * nComps;
        const bool direct = stageFinishIsCopy(doMask, mix, false, nComps, plane.channels);
        std::vector<float> warpedRow(direct ? 0 : rowSize);
        std::vector<float> referenceRow(direct ? 0 : rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<float> sx(width);
        std::vector<float> sy(width);
        std::vector<float> jacobian(impulse ? 0 : (std::size_t)width * 4);
        float* const jac = impulse ? NULL : jacobian.data();
        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % PassExec::kAbortCheckLines) == 0) && cancel.check()) {
                return;
            }
            float* dstPix = (float*)plane.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            float* const outRow = direct ? dstPix : warpedRow.data();
            if (!hasSource) {
                std::fill(outRow, outRow + rowSize, 0.f);
            } else {
                field->getRow(y, roi.x1, roi.x2, sx.data(), sy.data(), jac);
                if (plane.concatenated) {
                    mapRowThrough(plane.inverse, width, sx.data(), sy.data(), jac);
                }
                Resampler::resampleRowFromCoords(plane.source, sx.data(), sy.data(), jac, width, settings.filter, settings.clamp, settings.blackOutside,
                                                 Resampler::eWrapNone, Resampler::eWrapNone, outRow, nComps);
                if (!plane.concatenated) {
                    // A pixel the warp leaves in place is the Source pixel itself, which a filter
                    // would soften with its neighbours.
                    const float cy = (float)(y + 0.5);
                    for (int i = 0; i < width; ++i) {
                        if ((sx[i] != (float)(roi.x1 + i + 0.5)) || (sy[i] != cy)) {
                            continue;
                        }
                        float* dst = &outRow[(std::size_t)i * nComps];
                        const float* pix = plane.source.pixel(roi.x1 + i, y);
                        if (pix) {
                            std::copy(pix, pix + nComps, dst);
                        } else {
                            std::fill(dst, dst + nComps, 0.f);
                        }
                    }
                }
            }
            if (direct) {
                continue;
            }
            if (plane.concatenated) {
                // The reference is this node's immediate input: the fetched image through the
                // concatenated transform alone, black outside its own region.
                for (int i = 0; i < width; ++i) {
                    sx[i] = (float)(roi.x1 + i + 0.5);
                    sy[i] = (float)(y + 0.5);
                    if (jac) {
                        float* J = &jac[(std::size_t)i * 4];
                        J[0] = 1.f;
                        J[1] = 0.f;
                        J[2] = 0.f;
                        J[3] = 1.f;
                    }
                }
                mapRowThrough(plane.inverse, width, sx.data(), sy.data(), jac);
                Resampler::resampleRowFromCoords(plane.source, sx.data(), sy.data(), jac, width, settings.filter, settings.clamp, true,
                                                 Resampler::eWrapNone, Resampler::eWrapNone, &referenceRow[0], nComps);
            } else {
                PassExec::readSourceRow(plane.src.get(), plane.srcAccess.get(), plane.srcBounds, plane.src ? plane.srcNComps : nComps, roi.x1, y, width, nComps, &referenceRow[0]);
            }
            if (doMask) {
                PassExec::readChannelRow(mask.get(), maskSource.access.get(), maskSource.bounds, maskSource.nComps, maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &referenceRow[(std::size_t)i * nComps];
                const float* outPix = &warpedRow[(std::size_t)i * nComps];
                float alpha = mix;
                if (doMask) {
                    const float maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                    alpha = maskScale * mix;
                }
                for (int c = 0; c < nComps; ++c) {
                    if ((c >= 4) || !plane.channels[pixelKernelChannelBit(nComps, c)]) {
                        if (passThrough) {
                            dstPix[c] = srcPix[c];
                        }
                        continue;
                    }
                    float v = outPix[c];
                    if (alpha == 0.f) {
                        v = srcPix[c];
                    } else if (alpha != 1.f) {
                        v = v * alpha + (1.f - alpha) * srcPix[c];
                    }
                    dstPix[c] = v;
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // SplineWarp::renderOp

NATRON_NAMESPACE_EXIT
