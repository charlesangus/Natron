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

#include "GeoForEachBegin.h"

#include <algorithm>
#include <cmath>
#include <variant>

#include <QtCore/QString>

#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/GeoForEachEnd.h"
#include "Engine/Nodes/Loop/GeoForEachPieces.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kMetadataVariables[] = { "iteration", "numiterations", "value", "ivalue", "svalue" };

template <typename T>
bool
contextValue(const RenderContextPtr& context,
             const RenderContextName& name,
             T* value)
{
    const RenderContextValue* found = context ? context->find(name) : NULL;
    const T* typed = found ? std::get_if<T>(found) : NULL;

    if (!typed) {
        return false;
    }
    *value = *typed;

    return true;
}

} // anonymous namespace

GeoForEachBegin::GeoForEachBegin(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoForEachBegin::~GeoForEachBegin()
{
}

const NodeSpec*
GeoForEachBegin::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

bool
GeoForEachBegin::isGeoForEachBegin(const NodePtr& node)
{
    return node && (node->getPluginID() == PLUGINID_NATRON_GEOFOREACHBEGIN);
}

GeoForEachBeginMethodEnum
GeoForEachBegin::getMethod() const
{
    KnobChoicePtr method = _method.lock();

    return method ? (GeoForEachBeginMethodEnum)method->getValue() : eGeoForEachBeginExtractPiece;
}

GeoDetail
GeoForEachBegin::metadataDetail(std::int64_t iteration,
                                std::int64_t numIterations,
                                double value,
                                std::int64_t ivalue,
                                const std::string& svalue)
{
    GeoDetailBuilder builder;

    builder.addAttribute(eGeoAttribClassDetail, GeoAttributeDesc { "iteration", eGeoStorageInt64 }).set<std::int64_t>(0, iteration);
    builder.addAttribute(eGeoAttribClassDetail, GeoAttributeDesc { "numiterations", eGeoStorageInt64 }).set<std::int64_t>(0, numIterations);
    builder.addAttribute(eGeoAttribClassDetail, GeoAttributeDesc { "value", eGeoStorageFloat64 }).set<double>(0, value);
    builder.addAttribute(eGeoAttribClassDetail, GeoAttributeDesc { "ivalue", eGeoStorageInt64 }).set<std::int64_t>(0, ivalue);
    builder.addAttribute(eGeoAttribClassDetail, GeoAttributeDesc { "svalue", eGeoStorageString }).setString(0, svalue);

    return builder.finish();
}

void
GeoForEachBegin::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobChoicePtr method = createKnob<KnobChoice>(tr("Method").toStdString());
    method->setName(kGeoForEachBeginParamMethod);
    method->setAnimationEnabled(false);
    method->setHintToolTip(tr("What this node outputs in each iteration of its loop.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("extract", tr("Extract Piece or Point").toStdString(),
                                       tr("The elements of the input the iteration is about, found by the value the GeoForEachEnd "
                                          "splits its pieces by. Under Iteration Method Count, the input.")
                                           .toStdString()));
        options.push_back(ChoiceOption("feedback", tr("Fetch Feedback").toStdString(),
                                       tr("Under Gather Method Feedback, the result of the iteration before; the input in the first iteration.").toStdString()));
        options.push_back(ChoiceOption("metadata", tr("Fetch Metadata").toStdString(),
                                       tr("A geometry holding only detail attributes: iteration, numiterations, value, ivalue and svalue "
                                          "of the iteration. The input is not used.")
                                           .toStdString()));
        options.push_back(ChoiceOption("input", tr("Fetch Input").toStdString(),
                                       tr("The input, the same in every iteration.").toStdString()));
        method->populateChoices(options);
    }
    method->setDefaultValue((int)eGeoForEachBeginExtractPiece);
    page->addKnob(method);
    _method = method;
}

bool
GeoForEachBegin::knobChanged(KnobI* k,
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

std::shared_ptr<GeoForEachEnd>
GeoForEachBegin::pairedEnd() const
{
    const U64 generation = LoopEnd::getGraphGeneration();
    {
        QMutexLocker k(&_pairingMutex);
        if (_pairingValid && (_pairingGeneration == generation)) {
            return _pairedEnd.lock();
        }
    }
    const std::shared_ptr<GeoForEachEnd> end = GeoForEachEnd::pairedEndOf(getNode());

    QMutexLocker k(&_pairingMutex);
    _pairedEnd = end;
    _pairingGeneration = generation;
    _pairingValid = true;

    return end;
}

bool
GeoForEachBegin::computes(const std::shared_ptr<GeoForEachEnd>& end,
                          const RenderContextPtr& context) const
{
    switch (getMethod()) {
    case eGeoForEachBeginExtractPiece:

        return end && (end->readSettings().iteration == eGeoForEachIteratePieces);
    case eGeoForEachBeginFetchFeedback:

        return end && context && context->find(GeoForEachEnd::hiddenVariable(end->getLoopName(), kGeoForEachHiddenFeedback));
    case eGeoForEachBeginFetchMetadata:

        return true;
    case eGeoForEachBeginFetchInput:
    default:

        return false;
    }
}

Route
GeoForEachBegin::route(double time,
                       ViewIdx /*view*/)
{
    const std::shared_ptr<GeoForEachEnd> end = pairedEnd();

    if (computes(end, getCurrentRenderContext())) {
        return Route::none();
    }
    refreshPairingMessage(end);

    return Route::input(0, time);
}

RenderContextReadSet
GeoForEachBegin::getExtraContextReads() const
{
    RenderContextReadSet reads;
    const std::shared_ptr<GeoForEachEnd> end = pairedEnd();

    if (!end) {
        return reads;
    }
    const std::string loopName = end->getLoopName();
    switch (getMethod()) {
    case eGeoForEachBeginExtractPiece:
        reads.add(GeoForEachEnd::hiddenVariable(loopName, kGeoForEachHiddenPiece));
        break;
    case eGeoForEachBeginFetchFeedback:
        reads.add(GeoForEachEnd::hiddenVariable(loopName, kGeoForEachHiddenFeedback));
        break;
    case eGeoForEachBeginFetchMetadata:
        for (const char* name : kMetadataVariables) {
            reads.add(RenderContextName::loops(loopName, name));
        }
        break;
    case eGeoForEachBeginFetchInput:
    default:
        break;
    }

    return reads;
}

bool
GeoForEachBegin::pullsInputInRender(int inputNb,
                                    double /*time*/,
                                    ViewIdx /*view*/,
                                    const RenderContextPtr& context) const
{
    if (inputNb != 0) {
        return false;
    }
    switch (getMethod()) {
    case eGeoForEachBeginFetchMetadata:

        return true;
    case eGeoForEachBeginFetchFeedback:

        return computes(pairedEnd(), context);
    default:

        return false;
    }
}

void
GeoForEachBegin::appendNodeContentHash(ContentHasher& h) const
{
    const std::shared_ptr<GeoForEachEnd> end = pairedEnd();

    h.appendBool(bool(end));
    if (end) {
        h.appendString(end->getLoopName());
        end->appendBeginKnobsHash(h);
    }
}

void
GeoForEachBegin::postMessage(MessageTypeEnum type,
                             const std::string& message)
{
    QMutexLocker k(&_messageMutex);

    refreshLoopMessage(getNode(), type, message, &_posted);
}

void
GeoForEachBegin::refreshPairingMessage(const std::shared_ptr<GeoForEachEnd>& end)
{
    if (!end) {
        postMessage(eMessageTypeWarning, getScriptName_mt_safe() + ": " + tr("no GeoForEachEnd closes this loop, or several do.").toStdString());
    } else {
        postMessage(eMessageTypeInfo, std::string());
    }
}

StatusEnum
GeoForEachBegin::renderGeometry(const GeoRenderActionArgs& args,
                                GeoDetailConstPtr* outputDetail,
                                std::string* errorMessage)
{
    const std::shared_ptr<GeoForEachEnd> end = pairedEnd();
    const RenderContextPtr context = getCurrentRenderContext();
    const GeoForEachBeginMethodEnum method = getMethod();

    refreshPairingMessage(end);

    if (method == eGeoForEachBeginFetchMetadata) {
        std::int64_t iteration = 0;
        std::int64_t numIterations = 0;
        double value = 0.;
        std::int64_t ivalue = 0;
        std::string svalue;
        const std::string loopName = end ? end->getLoopName() : std::string();
        if (!end || !contextValue(context, RenderContextName::loops(loopName, "iteration"), &iteration)) {
            // Outside a cook of the loop: the first iteration of a count, as far as it is known here.
            if (end && (end->readSettings().iteration == eGeoForEachIterateCount)) {
                const GeoForEachEnd::Settings s = end->readSettings();
                numIterations = s.iterations;
                value = s.startValue;
                ivalue = (std::int64_t)std::llround(value);
            }
        } else {
            (void)contextValue(context, RenderContextName::loops(loopName, "numiterations"), &numIterations);
            (void)contextValue(context, RenderContextName::loops(loopName, "value"), &value);
            (void)contextValue(context, RenderContextName::loops(loopName, "ivalue"), &ivalue);
            (void)contextValue(context, RenderContextName::loops(loopName, "svalue"), &svalue);
        }
        *outputDetail = std::make_shared<const GeoDetail>(metadataDetail(iteration, numIterations, value, ivalue, svalue));

        return eStatusOK;
    }

    if (method == eGeoForEachBeginFetchFeedback) {
        std::int64_t hash = 0;
        if (!end || !contextValue(context, GeoForEachEnd::hiddenVariable(end->getLoopName(), kGeoForEachHiddenFeedback), &hash)) {
            *outputDetail = args.getInputDetail(0);
        } else {
            *outputDetail = GeoForEachEnd::findFeedback((U64)hash);
            if (!*outputDetail) {
                *errorMessage = getScriptName_mt_safe() + ": " + tr("the result of the iteration before is no longer available.").toStdString();

                return eStatusFailed;
            }
        }
        if (!*outputDetail) {
            *outputDetail = std::make_shared<const GeoDetail>();
        }

        return eStatusOK;
    }

    const GeoDetailConstPtr input = args.getInputDetail(0);
    if (!input) {
        *outputDetail = std::make_shared<const GeoDetail>();

        return eStatusOK;
    }
    if ((method != eGeoForEachBeginExtractPiece) || !end) {
        *outputDetail = input;

        return eStatusOK;
    }
    const GeoForEachEnd::Settings s = end->readSettings();
    if (s.iteration != eGeoForEachIteratePieces) {
        *outputDetail = input;

        return eStatusOK;
    }
    std::string partitionError;
    const GeoPiecePartitionConstPtr partition = GeoForEachPieces::partition(*input, s.pieces, &partitionError);
    if (!partition) {
        *errorMessage = getScriptName_mt_safe() + ": " + partitionError;
        postMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    std::ptrdiff_t piece = -1;
    const RenderContextValue* current = context ? context->find(GeoForEachEnd::hiddenVariable(end->getLoopName(), kGeoForEachHiddenPiece)) : NULL;
    if (current) {
        // By value, so pieces line up with the GeoForEachEnd's Piece input when it has one.
        if (const std::string* name = std::get_if<std::string>(current)) {
            piece = partition->hasStringValues() ? partition->find(*name) : -1;
        } else if (const std::int64_t* number = std::get_if<std::int64_t>(current)) {
            piece = partition->hasStringValues() ? -1 : partition->find(*number);
        }
    } else if (partition->size() > 0) {
        // Outside a cook of the loop: the piece Single Pass would cook, or the first.
        piece = s.singlePass ? std::min<std::ptrdiff_t>(s.singlePassIteration, (std::ptrdiff_t)partition->size() - 1) : 0;
    }
    if (piece < 0) {
        *outputDetail = std::make_shared<const GeoDetail>();

        return eStatusOK;
    }
    *outputDetail = std::make_shared<const GeoDetail>(GeoForEachPieces::extract(*input, *partition, (std::size_t)piece));

    return eStatusOK;
} // GeoForEachBegin::renderGeometry

NATRON_NAMESPACE_EXIT
