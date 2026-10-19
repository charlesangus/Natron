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

#ifndef Engine_Nodes_Image_StagedImageEffect_h
#define Engine_Nodes_Image_StagedImageEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Base for flat 2D native image nodes described by a NodeSpec and an ImagePipelineSpec:
 * float-only, tile-capable, multi-resolution, render scale supported, and FullySafe.
 *
 * The host's side of the node is fixed here and follows the pipeline spec: the layer knob, the
 * channel checkboxes, the "(Un)premult by" selector, and how render() and isIdentity() wrap the
 * op. A node supplies only its op, through renderOp() and isOpIdentity(), which ImageNode
 * implements from an Op type.
 *
 * Identity is composed: a route() other than None, then, with the MaskMix stage, a mix of 0,
 * then the op's own identity, then, with the MaskMix stage, a non-inverted mask whose region
 * misses the window. Every stage identity is one of the node's first Source input at the same
 * time.
 **/
class StagedImageEffect
    : public NativeEffectBase {
public:
    explicit StagedImageEffect(NodePtr node);

    virtual ~StagedImageEffect();

    /**
     * @brief How the host runs this node. Must outlive the node and never change.
     **/
    virtual const ImagePipelineSpec& imagePipelineSpec() const WARN_UNUSED_RETURN = 0;

    virtual bool supportsTiles() const OVERRIDE WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsMultiResolution() const OVERRIDE WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE;

    virtual bool wantsHostUnPremultSelector() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool rendersUnprocessedChannels() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool defaultProcessesAllLayers() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isHostChannelSelectorSupported(bool* defaultR,
                                                bool* defaultG,
                                                bool* defaultB,
                                                bool* defaultA) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The input the op reads and the stages blend against: the first Source input of
     * nodeSpec(), -1 when it has none, and input 0 for a node without a spec.
     **/
    int getSourceInput() const WARN_UNUSED_RETURN;

    /**
     * @brief The input the MaskMix stage reads as its mask: the first input described as a mask,
     * or -1.
     **/
    int getMaskInput() const WARN_UNUSED_RETURN;

protected:
    /**
     * @brief Declares maskInvert (false) and mix (1, range 0..1) with the OpenFX names, labels
     * and hints, and adds them to page. The host moves both to the end of the page.
     **/
    void addMaskMixKnobs(const KnobPagePtr& page);

    /**
     * @brief The page the op's knobs go on, which ImageNode creates before its op's knobs.
     **/
    KnobPagePtr createControlsPage();

    /**
     * @brief Adds the stage knobs the pipeline spec asks for to page, after the op's knobs.
     **/
    void addStageKnobs(const KnobPagePtr& page);

    double getMixValue(double time, ViewIdx view) const WARN_UNUSED_RETURN;
    bool getMaskInvertValue(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief Whether the MaskMix stage reads a mask: the mask input is connected and enabled.
     **/
    bool isMaskApplied() const WARN_UNUSED_RETURN;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Resolves the StagePlan once and hands it to renderOp().
     **/
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Whether the op alone leaves its source unchanged at (time, view) over roi.
     **/
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view)
        = 0;

    /**
     * @brief Renders args through plan, the stages resolved for this render.
     **/
    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan)
        = 0;

private:
    StageKnobs _stageKnobs;
};

/**
 * @brief An image node made of an Op, its NodeSpec and its ImagePipelineSpec, the last two
 * defaulting to OP::kNodeSpec and OP::kPipeline. The op is a member, so its bake() can read
 * knob handles it holds. When OP has createKnobs(KnobHolder*, const KnobPagePtr&), the
 * node's knobs are the op's on a "Controls" page, followed by the stage knobs; a subclass that
 * needs more overrides initializeKnobs() and calls addStageKnobs() itself.
 **/
template <typename OP, const NodeSpec& SPEC = OP::kNodeSpec, const ImagePipelineSpec& PIPELINE = OP::kPipeline>
    requires OpOfKind<OP, PIPELINE.op>
class ImageNode
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ImageNode(node);
    }

    explicit ImageNode(NodePtr node)
        : StagedImageEffect(node)
        , _op()
    {
    }

    virtual ~ImageNode()
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<SPEC>();
    }

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticImagePipelineSpec<PIPELINE, SPEC>();
    }

protected:
    OP& op()
    {
        return _op;
    }

    const OP& op() const
    {
        return _op;
    }

    virtual void initializeKnobs() OVERRIDE
    {
        const KnobPagePtr page = createControlsPage();

        if constexpr (requires(OP& o, KnobHolder* holder, const KnobPagePtr& p) { o.createKnobs(holder, p); }) {
            _op.createKnobs(static_cast<KnobHolder*>(this), page);
        }
        addStageKnobs(page);
    }

private:
    virtual bool isOpIdentity(double time,
                              const RenderScale& /*scale*/,
                              const RectI& /*roi*/,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _op.isIdentity(bakeParams(_op, time, view));
    }

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const typename OP::Params params = bakeParams(_op, args.time, args.view);
        const OP& rowOp = _op;
        const auto row = [&rowOp, &params](const RowIO& io) {
            rowOp.row(params, io);
        };

        return runStagesCpu(*this, plan, args, StageRowFn(row));
    }

    OP _op;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_StagedImageEffect_h
