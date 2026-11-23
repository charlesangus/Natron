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

#ifndef Engine_Nodes_Loop_GeoSolverBegin_h
#define Engine_Nodes_Loop_GeoSolverBegin_h

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

#define PLUGINID_NATRON_GEOSOLVERBEGIN "GeoSolverBegin"

#define kGeoSolverBeginParamMethod "method"

NATRON_NAMESPACE_ENTER

class GeoSolverEnd;

enum GeoSolverBeginMethodEnum {
    eGeoSolverBeginPrevFrame = 0,
    eGeoSolverBeginInput,
};

/**
 * @brief Opens a geometry solver closed by the GeoSolverEnd below it. Under Prev Frame it outputs
 * the result the substep being solved starts from; its input starts the simulation. Under Input
 * it passes its input through at the time being solved, so the body can also read live geometry.
 * Several GeoSolverBegins may open one solver.
 **/
class GeoSolverBegin
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOSOLVERBEGIN,
        .label = "GeoSolverBegin",
        .description = "Opens a geometry solver closed by the GeoSolverEnd below it. The nodes between them "
                       "cook once per frame, or once per substep. Method chooses what this node outputs: the "
                       "result of the frame or substep before (Prev Frame), whose input is the geometry the "
                       "simulation starts from at Start Frame, or its input at the time being solved (Input), "
                       "to bring live geometry into the simulation. Several GeoSolverBegins may open one "
                       "solver, each with its own Method. Outside a cook of the solver it outputs its input.",
        .grouping = PLUGIN_GROUP_LOOP,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoSolverBegin(node);
    }

    explicit GeoSolverBegin(NodePtr node);
    virtual ~GeoSolverBegin();

    static bool isGeoSolverBegin(const NodePtr& node) WARN_UNUSED_RETURN;

    GeoSolverBeginMethodEnum getMethod() const WARN_UNUSED_RETURN;

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

    std::shared_ptr<GeoSolverEnd> pairedEnd() const WARN_UNUSED_RETURN;

    // Whether this node outputs the previous result under context rather than its input.
    bool fetchesPrevious(const std::shared_ptr<GeoSolverEnd>& end,
                         const RenderContextPtr& context) const WARN_UNUSED_RETURN;

    void postMessage(MessageTypeEnum type,
                     const std::string& message);

    KnobChoiceWPtr _method;

    mutable QMutex _pairingMutex;
    mutable U64 _pairingGeneration = 0;
    mutable bool _pairingValid = false;
    mutable std::weak_ptr<GeoSolverEnd> _pairedEnd;

    QMutex _messageMutex;
    std::string _posted;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_GeoSolverBegin_h
