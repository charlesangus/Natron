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

#ifndef Engine_Nodes_Loop_GeoSolverEnd_h
#define Engine_Nodes_Loop_GeoSolverEnd_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Loop/GeoSimCache.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderContext.h"

#define PLUGINID_NATRON_GEOSOLVEREND "GeoSolverEnd"

// The hidden variable of hiddenVariable(): the content hash of the result the substep starts from.
#define kGeoSolverHiddenPrevious "prev"

#define kGeoSolverEndParamStartFrame "startFrame"
#define kGeoSolverEndParamSubsteps "substeps"
#define kGeoSolverEndParamResetSimulation "resetSimulation"
#define kGeoSolverEndParamCacheSimulation "cacheSimulation"
#define kGeoSolverEndParamCacheMemory "cacheMemory"
#define kGeoSolverEndParamSpillToDisk "spillToDisk"
#define kGeoSolverEndParamLoopName "loopName"

#define kGeoSolverEndDefaultCacheMemoryMB 2048

NATRON_NAMESPACE_ENTER

/**
 * @brief Closes a geometry solver opened by the GeoSolverBegin nodes found walking up from its
 * Source input. At a frame past Start Frame, Source cooks Substeps times, each under a context
 * whose hidden variable "geoSolver.<loop name>.prev" holds the content hash of the result the
 * substep starts from (GeoForEachEnd::publishFeedback()), which GeoSolverBegins set to Prev
 * Frame output. The frame before starts it, and the first frame after Start Frame starts from
 * the Prev Frame GeoSolverBegin's input at Start Frame.
 *
 * Solved frames are kept in a GeoSimCache by (hash, context key, view, frame), so a frame
 * starts from the nearest earlier one kept and solves forward one frame at a time, never by
 * recursion. One solve walks a series at a time; a second request of it waits for the first.
 **/
class GeoSolverEnd
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOSOLVEREND,
        .label = "GeoSolverEnd",
        .description = "Closes a geometry solver. The nodes between the GeoSolverBegin nodes above it and "
                       "this node cook once per frame, or Substeps times per frame, each time starting from "
                       "the result before, which a GeoSolverBegin set to Prev Frame outputs: any chain of "
                       "geometry nodes becomes a simulation. Up to Start Frame the output is the Prev Frame "
                       "GeoSolverBegin's input. Each substep sets loop.substep, loop.numsubsteps and loop.dt "
                       "(the frames a substep covers), also as loops.<name>.*, and @TimeInc in a GeoWrangle "
                       "is that step in seconds. Solved frames are kept, in RAM and then on disk, so playing "
                       "back or scrubbing does not solve them again; an edit above this node or in the body "
                       "starts the simulation over.",
        .grouping = PLUGIN_GROUP_LOOP,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoSolverEnd(node);
    }

    explicit GeoSolverEnd(NodePtr node);
    virtual ~GeoSolverEnd();

    static bool isGeoSolverEnd(const NodePtr& node) WARN_UNUSED_RETURN;

    /** @brief The one GeoSolverEnd begin opens a solver of, or null for none or several. */
    static std::shared_ptr<GeoSolverEnd> pairedEndOf(const NodePtr& begin) WARN_UNUSED_RETURN;

    /** @brief The GeoSolverBegins found walking up from Source, by script name. */
    std::vector<NodePtr> getBegins() const WARN_UNUSED_RETURN;

    /** @brief The Loop Name, or the script name when it is empty. */
    std::string getLoopName() const WARN_UNUSED_RETURN;

    static RenderContextName hiddenVariable(const std::string& loopName) WARN_UNUSED_RETURN;

    /** @brief The loop.* variables each substep sets, and loops.<name>.* the same. */
    static const std::vector<std::string>& loopVariables() WARN_UNUSED_RETURN;

    struct Settings {
        int startFrame = 1;
        int substeps = 1;
        bool cacheSimulation = true;
        std::size_t cacheBytes = 0;
        bool spillToDisk = true;
    };

    Settings readSettings() const WARN_UNUSED_RETURN;

    /** @brief Forgets every solved frame and makes the next request solve from Start Frame. */
    void resetSimulation();

    /** @brief The frames solved since the node was made, each counted once it is whole. */
    std::uint64_t getSolvedFrameCount() const WARN_UNUSED_RETURN;

    const GeoSimCache& getSimCache() const WARN_UNUSED_RETURN;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual bool needsOrderedFrames() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual RenderContextReadSet getOverriddenContextVariables(int inputNb) const OVERRIDE FINAL;
    virtual bool pullsInputInRender(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    const RenderContextPtr& context) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;
    virtual void appendExtraHashDependents(std::vector<NodePtr>* dependents) const OVERRIDE FINAL;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void postMessage(MessageTypeEnum type,
                     const std::string& message);
    StatusEnum fail(const std::string& message,
                    std::string* errorMessage);

    RenderContextPtr substepContext(const RenderContextPtr& base,
                                    const std::string& loopName,
                                    int substep,
                                    int substeps,
                                    const GeoDetailConstPtr& previous) const;

    // Solves frame from previous, the result of the frame before, into *result.
    StatusEnum solveFrame(const EffectInstancePtr& body,
                          const GeoRenderActionArgs& args,
                          const RenderContextPtr& base,
                          const std::string& loopName,
                          const Settings& s,
                          int frame,
                          const GeoDetailConstPtr& previous,
                          GeoDetailConstPtr* result,
                          std::string* errorMessage);

    std::shared_ptr<std::timed_mutex> walkMutex(const GeoSimSeries& series);

    KnobIntWPtr _startFrame;
    KnobIntWPtr _substeps;
    KnobButtonWPtr _resetSimulation;
    KnobBoolWPtr _cacheSimulation;
    KnobIntWPtr _cacheMemory;
    KnobBoolWPtr _spillToDisk;
    KnobStringWPtr _loopName;

    mutable QMutex _pairingMutex;
    mutable U64 _pairingGeneration = 0;
    mutable bool _pairingValid = false;
    mutable std::vector<NodeWPtr> _begins;

    QMutex _messageMutex;
    std::string _posted;

    std::unique_ptr<GeoSimCache> _simCache;
    std::atomic<U64> _resetGeneration { 0 };
    std::atomic<std::uint64_t> _solvedFrames { 0 };

    std::mutex _walksMutex;
    std::map<GeoSimSeries, std::shared_ptr<std::timed_mutex>> _walks;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_GeoSolverEnd_h
