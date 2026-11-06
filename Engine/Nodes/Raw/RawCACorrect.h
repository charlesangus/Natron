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

/*
 * The maths are a port of darktable's raw chromatic aberrations module, DARKTABLE_REF
 * release-5.6.2 (commit 20891f6c6fa7), src/iop/cacorrect.c, Copyright (C) 2010-2026 darktable
 * developers, after RawTherapee's CA_correct_RT.cc by Emil Martinec and Ingo Weyrich,
 * distributed under the GNU GPL version 3 or later. The kernel is in RawCACorrectKernel.cpp.
 */

#ifndef Engine_Nodes_Raw_RawCACorrect_h
#define Engine_Nodes_Raw_RawCACorrect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWCACORRECT "RawCACorrect"
#define PLUGIN_MAJOR_NATRON_RAWCACORRECT 1

#define kRawCACorrectParamIterations "iterations"
#define kRawCACorrectParamAvoidShift "avoidColorShift"

// What RawNodeSupport::noteFrameComputed() names this node's whole-frame computation.
#define kRawCACorrectFrame "RawCACorrect.frame"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's raw chromatic aberrations module on a prepared Bayer mosaic: measures how
 * far red and blue are displaced from green across the image, fits a smooth field to the
 * displacements and moves red and blue back, Iterations times, optionally dividing out the colour
 * cast that leaves. X-Trans mosaics and data without a CFA pass through with a warning, as do
 * images too small or too flat to measure.
 *
 * The measurement needs the whole frame, so every render reads the whole source. The corrected
 * mosaic is computed once per source and settings, by whichever render gets there first while
 * the others wait, and every tile is cut from it, so tiles match the full frame bit for bit.
 * Renders at full resolution only.
 **/
class RawCACorrect
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawCACorrect(node);
    }

    explicit RawCACorrect(NodePtr node);

    virtual ~RawCACorrect();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Drops the whole-frame results kept under RawNodeSupport::frameKey(), as Clear Caches
     * and memory pressure ask; the next render computes them again.
     **/
    virtual void purgeCaches() OVERRIDE FINAL;

private:
    struct Settings;

    /**
     * @brief The whole corrected mosaic for one source and settings, and whether the kernel
     * corrected it.
     **/
    struct Frame {
        RawNodeSupport::TopDownBuffer mosaic;
        bool corrected = false;
    };

    std::optional<Settings> resolveSettings(double time, ViewIdx view) WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    /**
     * @brief Posts or withdraws the message frame calls for, measured under renderHash. The
     * caller holds _frameMutex, so that a decision taken on a missing frame cannot land after
     * another thread posted the message of the frame it computed.
     **/
    void postFrameMessage(const Frame& frame, U64 renderHash);

    KnobChoiceWPtr _iterations;
    KnobBoolWPtr _avoidShift;
    RawNodeSupport::OwnedMessage _message;

    std::mutex _frameMutex;
    U64 _frameKey;
    std::shared_ptr<const Frame> _frame;
    // The render hash of the frame whose message is posted, 0 for none. It outlives a purge of
    // _frame: Clear Caches refreshes every node's identity right after dropping the frame, and
    // the message still holds until the settings or the input change.
    U64 _messageHash;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawCACorrect_h
