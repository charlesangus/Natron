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
 * The Bayer path of darktable's neural restore, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/common/ai/restore_raw_bayer.c, restore.c and restore_common.h, Copyright (C) 2026 darktable
 * developers, distributed under the GNU GPL version 3 or later.
 */

#ifndef Engine_Nodes_Raw_RawNeuralDenoise_h
#define Engine_Nodes_Raw_RawNeuralDenoise_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <list>
#include <memory>
#include <optional>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWNEURALDENOISE "fr.natron.RawNeuralDenoise"
#define PLUGIN_MAJOR_NATRON_RAWNEURALDENOISE 1

#define kRawNeuralDenoiseParamStrength "strength"
#define kRawNeuralDenoiseParamTileSize "tileSize"

NATRON_NAMESPACE_ENTER

class ModelDownloader;
class ModelKnob;

/**
 * @brief Raw to image: RawNIND's Bayer model, which denoises and demosaics prepared raw data in
 * one pass, run in overlapping tiles as darktable's neural restore runs it. The four photosites
 * of each 2x2 block, from its red one, are packed into four half-resolution planes, and the model
 * returns camera RGB at full resolution, in the white-balanced frame RawDemosaic outputs. Strength
 * mixes the result with a bilinear demosaic of the same input, which also fills the rows and
 * columns the 2x2 blocks leave out. Only Bayer mosaics: anything else is a persistent error.
 *
 * A render reads the darktable tiles that reach its window, so a window is the same pixels as
 * the whole frame. Renders at full resolution only.
 **/
class RawNeuralDenoise
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawNeuralDenoise(node);
    }

    explicit RawNeuralDenoise(NodePtr node);

    virtual ~RawNeuralDenoise();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The downloader nodes and the Develop RAW chain use: the one set for tests, or
     * ModelDownloader::process().
     **/
    static ModelDownloader* modelDownloader();

    /**
     * @brief Points nodes created from now on, and the Develop RAW chain, at another downloader;
     * null restores ModelDownloader::process().
     **/
    static void setModelDownloaderForTests(ModelDownloader* downloader);

    // The source pixels a render of window needs at time and view.
    RectI sourceRegionFor(const RectI& window, double time, ViewIdx view) WARN_UNUSED_RETURN;

    struct Geometry;
    struct Policy;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog, const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

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

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The source's layout and what packing needs from its metadata, or the error why it can't be.
    std::optional<Geometry> resolveGeometry(double time, ViewIdx view, std::string* error) WARN_UNUSED_RETURN;

    // The model's policy and tile size, or nothing when the model can't run (the reason posted).
    std::optional<Policy> resolvePolicy(double time, ViewIdx view, std::string* error) WARN_UNUSED_RETURN;

    std::unique_ptr<ModelKnob> _modelKnob;
    KnobDoubleWPtr _strength;
    KnobIntWPtr _tileSize;
    RawNodeSupport::OwnedMessage _message;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawNeuralDenoise_h
