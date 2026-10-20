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

#include "Dissolve.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <list>
#include <string>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

void
DissolveOp::createKnobs(KnobHolder* holder,
                        const KnobPagePtr& page)
{
    KnobDoublePtr whichKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Which").toStdString());

    whichKnob->setName(kDissolveParamWhich);
    whichKnob->setHintToolTip(EffectInstance::tr("Mix factor between the inputs.").toStdString());
    whichKnob->setDefaultValue(0.);
    whichKnob->setMinimum(0.);
    whichKnob->setMaximum(kDissolveSourceCount - 1);
    whichKnob->setDisplayMinimum(0.);
    whichKnob->setDisplayMaximum(1.);
    page->addKnob(whichKnob);
    which = whichKnob;
}

DissolveOp::Params
DissolveOp::bake(const KnobReader& reader) const
{
    Params params;
    const double value = reader.value(which);

    params.which = std::max(0., std::min(value, (double)(kDissolveSourceCount - 1)));
    params.prev = (int)std::floor(params.which);
    params.next = (int)std::ceil(params.which);
    params.blend = (float)(params.which - params.prev);

    return params;
}

bool
DissolveOp::isIdentity(const Params& params) const
{
    return params.prev == params.next;
}

Route
DissolveOp::route(const Params& params,
                  const RouteContext& context) const
{
    const int input = dissolveSourceInput(params.prev);

    if ((params.prev == params.next) && context.isConnected(input)) {
        return Route::input(input, context.time());
    }

    return Route::none();
}

CompositeInputs
DissolveOp::inputs(const Params& params,
                   const RouteContext& context) const
{
    CompositeInputs inputs;

    inputs.base = CompositeInput { dissolveSourceInput(params.prev), context.time() };
    if (params.next != params.prev) {
        inputs.add(dissolveSourceInput(params.next), context.time());
    }

    return inputs;
}

void
DissolveOp::step(const Params& params,
                 const RowIO& io) const
{
    const int nComps = io.nComps;
    bool processed[4] = { false, false, false, false };

    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        processed[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float blend = params.blend;
    const float* from = io.src[0];
    const float* to = io.src[1];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, from += nComps, to += nComps, dst += nComps) {
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (processed[c]) {
                dst[c] = (to[c] - from[c]) * blend + from[c];
            }
        }
    }
}

Dissolve::Dissolve(NodePtr node)
    : ImageNode<DissolveOp>(node)
{
}

Dissolve::~Dissolve()
{
}

void
Dissolve::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    ImageNode<DissolveOp>::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

void
Dissolve::onInputChanged(int /*inputNo*/)
{
    updateRange();
}

void
Dissolve::updateRange()
{
    KnobDoublePtr which = op().which.lock();

    if (!which) {
        return;
    }
    int maxConnected = 1;
    for (int k = 2; k < kDissolveSourceCount; ++k) {
        if (isSourceConnected(k)) {
            maxConnected = k;
        }
    }
    which->setDisplayMinimum(0.);
    which->setDisplayMaximum(maxConnected);
}

bool
Dissolve::isSourceConnected(int k) const
{
    return bool(getInput(sourceInput(k)));
}

StatusEnum
Dissolve::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    const DissolveOp::Params params = bakeParams(op(), time, view);
    const double which = params.which;
    const int prev = (int)which;
    const int next = std::min((int)which + 1, kDissolveSourceCount - 1);

    const std::function<StatusEnum(int, RectD*)> sourceRoD = [&](int k, RectD* r) {
        EffectInstancePtr input = getInput(sourceInput(k));
        const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
        bool isProjectFormat = false;

        return input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, r, &isProjectFormat);
    };

    if ((which <= 0.) && isSourceConnected(0)) {
        return (sourceRoD(0, rod) == eStatusFailed) ? eStatusFailed : eStatusOK;
    }
    if (((double)prev == which) && isSourceConnected(prev) && !isMaskApplied()) {
        return (sourceRoD(prev, rod) == eStatusFailed) ? eStatusFailed : eStatusOK;
    }
    if (isSourceConnected(prev) && isSourceConnected(next)) {
        RectD fromRoD;
        RectD toRoD;
        if ((sourceRoD(prev, &fromRoD) == eStatusFailed) || (sourceRoD(next, &toRoD) == eStatusFailed)) {
            return eStatusFailed;
        }
        *rod = fromRoD;
        rod->merge(toRoD);

        return eStatusOK;
    }

    // OpenFX's default region in the general context when every clip is optional: the project
    // extent from the origin, not the union of the connected inputs.
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    const RectD canonical = format.toCanonical_noClipping(0, format.getPixelAspectRatio());
    *rod = RectD(0., 0., canonical.right(), canonical.top());

    return eStatusOK;
}

void
Dissolve::getRegionsOfInterest(double time,
                               const RenderScale& /*scale*/,
                               const RectD& /*outputRoD*/,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret)
{
    const DissolveOp::Params params = bakeParams(op(), time, view);

    if (isSourceConnected(params.prev)) {
        ret->insert(std::make_pair(getInput(sourceInput(params.prev)), renderWindow));
    }
    if (isSourceConnected(params.next)) {
        ret->insert(std::make_pair(getInput(sourceInput(params.next)), renderWindow));
    }
    if ((params.prev != params.next) && isMaskApplied()) {
        ret->insert(std::make_pair(getInput(kDissolveMaskInput), renderWindow));
    }
}

FramesNeededMap
Dissolve::getFramesNeeded(double time,
                          ViewIdx view)
{
    FramesNeededMap ret;
    RangeD range;

    range.min = range.max = time;
    std::vector<RangeD> ranges;
    ranges.push_back(range);
    FrameRangesMap viewRanges;
    viewRanges.insert(std::make_pair(view, ranges));

    const DissolveOp::Params params = bakeParams(op(), time, view);
    if (isSourceConnected(params.prev)) {
        ret.insert(std::make_pair(sourceInput(params.prev), viewRanges));
    }
    if (isSourceConnected(params.next)) {
        ret.insert(std::make_pair(sourceInput(params.next), viewRanges));
    }
    if ((params.prev != params.next) && isMaskApplied()) {
        ret.insert(std::make_pair((int)kDissolveMaskInput, viewRanges));
    }

    return ret;
}

NATRON_NAMESPACE_EXIT
