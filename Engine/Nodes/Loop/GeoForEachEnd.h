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

#ifndef Engine_Nodes_Loop_GeoForEachEnd_h
#define Engine_Nodes_Loop_GeoForEachEnd_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Loop/GeoForEachPieces.h"
#include "Engine/Nodes/Loop/LoopBlock.h"
#include "Engine/Nodes/Loop/LoopEndCooks.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderContext.h"

#define PLUGINID_NATRON_GEOFOREACHEND "GeoForEachEnd"

// The hidden variables of hiddenVariable(): the piece an iteration is about, and the content hash of
// the result of the iteration before under Feedback.
#define kGeoForEachHiddenPiece "piece"
#define kGeoForEachHiddenFeedback "feedback"

#define kGeoForEachEndParamLoopName "loopName"
#define kGeoForEachEndParamIterationMethod "iterationMethod"
#define kGeoForEachEndParamGatherMethod "gatherMethod"
#define kGeoForEachEndParamIterations "iterations"
#define kGeoForEachEndParamStartValue "startValue"
#define kGeoForEachEndParamIncrement "increment"
#define kGeoForEachEndParamPieceElements "pieceElements"
#define kGeoForEachEndParamPieceMethod "pieceMethod"
#define kGeoForEachEndParamPieceAttribute "pieceAttribute"
#define kGeoForEachEndParamMaxIterations "maxIterations"
#define kGeoForEachEndParamSinglePass "singlePass"
#define kGeoForEachEndParamSinglePassIteration "singlePassIteration"
#define kGeoForEachEndParamStopCondition "stopCondition"
#define kGeoForEachEndParamStopAttribute "stopAttribute"

#define kGeoForEachEndDefaultMaxIterations 100000

NATRON_NAMESPACE_ENTER

enum GeoForEachIterationEnum {
    eGeoForEachIteratePieces = 0,
    eGeoForEachIterateCount,
};

enum GeoForEachGatherEnum {
    eGeoForEachGatherMerge = 0,
    eGeoForEachGatherFeedback,
};

/**
 * @brief Closes a geometry loop opened by the GeoForEachBegin nodes found walking up from its
 * Source input. It cooks Source once per iteration, under a context that sets the loop
 * variables and the hidden variables its GeoForEachBegins read, and gathers the results: merged
 * in iteration order, or each iteration starting from the one before and the last kept.
 *
 * The hidden variables live in the namespace "geoForEach.<loop name>": "piece" names the piece
 * of the iteration (by value, see GeoPiecePartition), so each GeoForEachBegin extracts it from
 * its own input; "feedback" is the content hash of the iteration before, published while the
 * loop cooks (findFeedback()).
 **/
class GeoForEachEnd
    : public NativeEffectBase,
      public LoopBlockEnd {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false },
        InputSpec { .label = "Piece", .kind = eDataKindGeometry, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOFOREACHEND,
        .label = "GeoForEachEnd",
        .description = "Closes a geometry loop. The nodes between the GeoForEachBegin nodes above it and "
                       "this node cook once per iteration: once per piece of the geometry, or a number of "
                       "times. Each iteration sets the loop variables loop.iteration, loop.numiterations, "
                       "loop.value, loop.ivalue, loop.svalue and loop.frame (the frame being rendered; also as "
                       "loops.<name>.*), which knob "
                       "drivers in the body read. The iterations are merged together, or, under Feedback, "
                       "each starts from the result of the one before and the last is output. The Piece "
                       "input, when connected, gives the geometry whose pieces are iterated over; "
                       "otherwise it is the input of a GeoForEachBegin set to Extract Piece or Point.",
        .grouping = PLUGIN_GROUP_LOOP,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoForEachEnd(node);
    }

    explicit GeoForEachEnd(NodePtr node);
    virtual ~GeoForEachEnd();

    static bool isGeoForEachEnd(const NodePtr& node) WARN_UNUSED_RETURN;

    /** @brief The one GeoForEachEnd begin opens a loop of, or null for none or several. */
    static std::shared_ptr<GeoForEachEnd> pairedEndOf(const NodePtr& begin) WARN_UNUSED_RETURN;

    /** @brief The GeoForEachBegins found walking up from Source, by script name. */
    std::vector<NodePtr> getBegins() const WARN_UNUSED_RETURN;

    /** @brief The Loop Name, or the script name when it is empty. */
    std::string getLoopName() const WARN_UNUSED_RETURN;

    static RenderContextName hiddenVariable(const std::string& loopName,
                                            const std::string& name) WARN_UNUSED_RETURN;

    struct Settings {
        GeoForEachIterationEnum iteration = eGeoForEachIteratePieces;
        GeoForEachGatherEnum gather = eGeoForEachGatherMerge;
        GeoPieceSettings pieces;
        int iterations = 0;
        double startValue = 0.;
        double increment = 1.;
        int maxIterations = kGeoForEachEndDefaultMaxIterations;
        bool singlePass = false;
        int singlePassIteration = 0;
        std::string stopCondition;
        std::string stopAttribute;
    };

    Settings readSettings() const WARN_UNUSED_RETURN;

    /** @brief What a GeoForEachBegin's output reads of this node's settings. */
    void appendBeginKnobsHash(ContentHasher& h) const;

    /** @brief Keeps detail findable by its content hash while something holds it; returns the hash. */
    static U64 publishFeedback(const GeoDetailConstPtr& detail) WARN_UNUSED_RETURN;
    static GeoDetailConstPtr findFeedback(U64 contentHash) WARN_UNUSED_RETURN;

    /** @brief The loop.* variables each iteration sets, and loops.<name>.* the same. */
    static const std::vector<std::string>& loopVariables() WARN_UNUSED_RETURN;

    virtual std::string getBlockLoopName() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::vector<NodePtr> getBlockBegins() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual KnobStringPtr getBlockNameKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const std::vector<std::string>& getBlockVariables() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief base with the variables of iteration Single Pass Iteration. Pieces are those of the
     * last cook; before any, the iteration's index stands for its value.
     **/
    virtual RenderContextPtr getBlockPreviewContext(const RenderContextPtr& base,
                                                    double time) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    struct Iteration {
        std::int64_t index = 0;
        double value = 0.;
        std::int64_t ivalue = 0;
        std::string svalue;
        RenderContextValue piece;
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual RenderContextReadSet getOverriddenContextVariables(int inputNb) const OVERRIDE FINAL;
    virtual bool pullsInputInRender(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    const RenderContextPtr& context) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void appendExtraHashDependents(std::vector<NodePtr>* dependents) const OVERRIDE FINAL;
    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    virtual void abortCooksStaleAfterEdit() OVERRIDE FINAL;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshKnobVisibility();
    void refreshStopDependencies();
    // Shows, or clears when empty, this node's message.
    void postMessage(MessageTypeEnum type,
                     const std::string& message);
    StatusEnum fail(const std::string& message,
                    std::string* errorMessage);

    static constexpr int kStopFailed = -1;
    static constexpr int kStopAborted = -2;

    // The iterations before the first the stop condition is true for, kStopFailed with error
    // set, or kStopAborted when the render is aborted while evaluating.
    int evaluateStop(const std::vector<Iteration>& iterations,
                     const std::string& loopName,
                     double time,
                     std::string* error) const;

    RenderContextPtr iterationContext(const RenderContextPtr& base,
                                      const std::string& loopName,
                                      const Iteration& iteration,
                                      std::int64_t count,
                                      double frame,
                                      bool pieces,
                                      const GeoDetailConstPtr& feedback) const;

    KnobStringWPtr _loopName;
    KnobChoiceWPtr _iterationMethod;
    KnobChoiceWPtr _gatherMethod;
    KnobIntWPtr _iterations;
    KnobDoubleWPtr _startValue;
    KnobDoubleWPtr _increment;
    KnobChoiceWPtr _pieceElements;
    KnobChoiceWPtr _pieceMethod;
    KnobStringWPtr _pieceAttribute;
    KnobIntWPtr _maxIterations;
    KnobBoolWPtr _singlePass;
    KnobIntWPtr _singlePassIteration;
    KnobStringWPtr _stopCondition;
    KnobStringWPtr _stopAttribute;

    LoopEndPairing _pairing;

    mutable QMutex _cookedMutex;
    std::vector<Iteration> _cookedPieces;

    QMutex _messageMutex;
    std::string _posted;

    LoopEndCooks _cooks { this };
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_GeoForEachEnd_h
