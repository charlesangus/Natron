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

#include "GeoSolverBegin.h"

#include <cstdint>
#include <variant>

#include <QtCore/QString>

#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/GeoForEachEnd.h"
#include "Engine/Nodes/Loop/GeoSolverEnd.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

GeoSolverBegin::GeoSolverBegin(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoSolverBegin::~GeoSolverBegin()
{
}

const NodeSpec*
GeoSolverBegin::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

bool
GeoSolverBegin::isGeoSolverBegin(const NodePtr& node)
{
    return node && (node->getPluginID() == PLUGINID_NATRON_GEOSOLVERBEGIN);
}

GeoSolverBeginMethodEnum
GeoSolverBegin::getMethod() const
{
    KnobChoicePtr method = _method.lock();

    return method ? (GeoSolverBeginMethodEnum)method->getValue() : eGeoSolverBeginPrevFrame;
}

void
GeoSolverBegin::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobChoicePtr method = createKnob<KnobChoice>(tr("Method").toStdString());
    method->setName(kGeoSolverBeginParamMethod);
    method->setAnimationEnabled(false);
    method->setHintToolTip(tr("What this node outputs to the body of its solver.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("prevFrame", tr("Prev Frame").toStdString(),
                                       tr("The result of the frame or substep before. At the first frame after Start Frame, the "
                                          "input at Start Frame: the input is what the simulation starts from.")
                                           .toStdString()));
        options.push_back(ChoiceOption("input", tr("Input").toStdString(),
                                       tr("The input at the time being solved.").toStdString()));
        method->populateChoices(options);
    }
    method->setDefaultValue((int)eGeoSolverBeginPrevFrame);
    page->addKnob(method);
    _method = method;
}

bool
GeoSolverBegin::knobChanged(KnobI* k,
                            ValueChangedReasonEnum /*reason*/,
                            ViewSpec /*view*/,
                            double /*time*/,
                            bool /*originatedFromMainThread*/)
{
    KnobChoicePtr method = _method.lock();

    if (method && (k == method.get())) {
        invalidateRenderContextReads();

        return true;
    }

    return false;
}

std::shared_ptr<GeoSolverEnd>
GeoSolverBegin::pairedEnd() const
{
    const U64 generation = LoopEnd::getGraphGeneration();
    {
        QMutexLocker k(&_pairingMutex);
        if (_pairingValid && (_pairingGeneration == generation)) {
            return _pairedEnd.lock();
        }
    }
    const std::shared_ptr<GeoSolverEnd> end = GeoSolverEnd::pairedEndOf(getNode());

    QMutexLocker k(&_pairingMutex);
    _pairedEnd = end;
    _pairingGeneration = generation;
    _pairingValid = true;

    return end;
}

bool
GeoSolverBegin::fetchesPrevious(const std::shared_ptr<GeoSolverEnd>& end,
                                const RenderContextPtr& context) const
{
    return end && context && (getMethod() == eGeoSolverBeginPrevFrame) && context->find(GeoSolverEnd::hiddenVariable(end->getLoopName()));
}

Route
GeoSolverBegin::route(double time,
                      ViewIdx /*view*/)
{
    if (fetchesPrevious(pairedEnd(), getCurrentRenderContext())) {
        return Route::none();
    }

    return Route::input(0, time);
}

RenderContextReadSet
GeoSolverBegin::getExtraContextReads() const
{
    RenderContextReadSet reads;
    const std::shared_ptr<GeoSolverEnd> end = pairedEnd();

    if (end && (getMethod() == eGeoSolverBeginPrevFrame)) {
        reads.add(GeoSolverEnd::hiddenVariable(end->getLoopName()));
    }

    return reads;
}

bool
GeoSolverBegin::pullsInputInRender(int inputNb,
                                   double /*time*/,
                                   ViewIdx /*view*/,
                                   const RenderContextPtr& context) const
{
    return (inputNb == 0) && fetchesPrevious(pairedEnd(), context);
}

void
GeoSolverBegin::appendNodeContentHash(ContentHasher& h) const
{
    const std::shared_ptr<GeoSolverEnd> end = pairedEnd();

    h.appendBool(bool(end));
    if (end) {
        h.appendString(end->getLoopName());
    }
}

void
GeoSolverBegin::postMessage(MessageTypeEnum type,
                            const std::string& message)
{
    QMutexLocker k(&_messageMutex);

    refreshLoopMessage(getNode(), type, message, &_posted);
}

StatusEnum
GeoSolverBegin::renderGeometry(const GeoRenderActionArgs& args,
                               GeoDetailConstPtr* outputDetail,
                               std::string* errorMessage)
{
    const std::shared_ptr<GeoSolverEnd> end = pairedEnd();
    const RenderContextPtr context = getCurrentRenderContext();

    if (!end) {
        postMessage(eMessageTypeWarning, getScriptName_mt_safe() + ": " + tr("no GeoSolverEnd closes this solver, or several do.").toStdString());
    } else {
        postMessage(eMessageTypeInfo, std::string());
    }

    if (fetchesPrevious(end, context)) {
        const RenderContextValue* value = context->find(GeoSolverEnd::hiddenVariable(end->getLoopName()));
        const std::int64_t* hash = value ? std::get_if<std::int64_t>(value) : NULL;
        *outputDetail = hash ? GeoForEachEnd::findFeedback((U64)*hash) : GeoDetailConstPtr();
        if (!*outputDetail) {
            *errorMessage = getScriptName_mt_safe() + ": " + tr("the result of the frame before is no longer available.").toStdString();

            return eStatusFailed;
        }

        return eStatusOK;
    }

    *outputDetail = args.getInputDetail(0);
    if (!*outputDetail) {
        *outputDetail = std::make_shared<const GeoDetail>();
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
