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

#ifndef Engine_Nodes_Loop_GeoForEachBegin_h
#define Engine_Nodes_Loop_GeoForEachBegin_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOFOREACHBEGIN "GeoForEachBegin"

#define kGeoForEachBeginParamMethod "method"

NATRON_NAMESPACE_ENTER

class GeoForEachEnd;

enum GeoForEachBeginMethodEnum {
    eGeoForEachBeginExtractPiece = 0,
    eGeoForEachBeginFetchFeedback,
    eGeoForEachBeginFetchMetadata,
    eGeoForEachBeginFetchInput,
};

/**
 * @brief Opens a geometry loop closed by the GeoForEachEnd below it. Several may open one loop,
 * each with its own Method: the piece of the iteration taken from its input, the iteration
 * before under Feedback, a detail holding the iteration's metadata, or its input as is.
 **/
class GeoForEachBegin
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOFOREACHBEGIN,
        .label = "GeoForEachBegin",
        .description = "Opens a geometry loop closed by the GeoForEachEnd below it. The nodes between them "
                       "cook once per iteration. Method chooses what this node outputs in each iteration: "
                       "the piece or point of its input the iteration is about, the result of the "
                       "iteration before (its input in the first one), a detail-only geometry holding "
                       "the iteration's iteration, numiterations, value, ivalue and svalue attributes, or "
                       "its input unchanged. Several GeoForEachBegins may open one loop, each with its own "
                       "Method. The loop and its settings are the GeoForEachEnd's. Outside a cook of the "
                       "loop, a piece shows the Single Pass Iteration's piece, or the first.",
        .grouping = PLUGIN_GROUP_LOOP,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoForEachBegin(node);
    }

    explicit GeoForEachBegin(NodePtr node);
    virtual ~GeoForEachBegin();

    static bool isGeoForEachBegin(const NodePtr& node) WARN_UNUSED_RETURN;

    GeoForEachBeginMethodEnum getMethod() const WARN_UNUSED_RETURN;

    /** @brief The detail-only geometry Fetch Metadata outputs. */
    static GeoDetail metadataDetail(std::int64_t iteration,
                                    std::int64_t numIterations,
                                    double value,
                                    std::int64_t ivalue,
                                    const std::string& svalue);

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL;
    virtual bool pullsInputInRender(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    const RenderContextPtr& context) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::shared_ptr<GeoForEachEnd> pairedEnd() const WARN_UNUSED_RETURN;
    // Whether this node outputs something other than its input under context.
    bool computes(const std::shared_ptr<GeoForEachEnd>& end,
                  const RenderContextPtr& context) const WARN_UNUSED_RETURN;
    void postMessage(MessageTypeEnum type,
                     const std::string& message);

    KnobChoiceWPtr _method;

    mutable QMutex _pairingMutex;
    mutable U64 _pairingGeneration = 0;
    mutable bool _pairingValid = false;
    mutable std::weak_ptr<GeoForEachEnd> _pairedEnd;

    QMutex _messageMutex;
    std::string _posted;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_GeoForEachBegin_h
