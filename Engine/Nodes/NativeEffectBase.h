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

#ifndef Engine_Nodes_NativeEffectBase_h
#define Engine_Nodes_NativeEffectBase_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Engine/AppManager.h" // for AppManager::createKnob
#include "Engine/DeepPixelOps.h"
#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/GpuCostModel.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct GpuQuery;
struct GpuRenderArgs;

/**
 * @brief What the stages around a node's op resolve to at one time and view, as GPU placement
 * reads them. applyMask is whether the mask input is connected and enabled; maskChannel is the
 * channel of maskLayer it reads, -1 when it resolves to none. divisorChannel is the channel of
 * divisorLayer "(Un)premult by" divides the source by, -1 for none.
 **/
struct GpuStageSummary {
    bool applyMask = false;
    ImageLayerDesc maskLayer;
    int maskChannel = -1;
    double mix = 1.;
    ImageLayerDesc divisorLayer;
    int divisorChannel = -1;
};

/**
 * @brief What the GPU cost model reads from an op beyond its kind to pick a row of its tables:
 * sigma, the Gaussian-equivalent spread in pixels at the rendered scale (size / 2.4 for a
 * blur of diameter size), whether the filter is recursive (IIR), whose cost does not grow
 * with sigma, the Blur filter, for an erode or dilate its size in pixels at the rendered
 * scale, and whether the render averages several matrices per pixel (motion blur).
 **/
struct GpuCostHint {
    double sigma = 0.;
    bool iir = false;
    GpuBlurFilter filter = GpuBlurFilter::eAny;
    int size = 0;
    bool motionBlur = false;
};

/**
 * @brief The abort state of one render, shared with the pool threads running its bands.
 * EffectInstance::aborted() finds the render through the AbortableThread info or the effect's
 * TLS frame args of the asking thread, which only the thread running render() carries: a pool
 * thread asking is always told the render goes on. So only the thread that built this asks the
 * effect, and it publishes the answer in a flag the other threads read.
 **/
class RenderCancellation {
public:
    explicit RenderCancellation(const EffectInstance* effect);

    RenderCancellation(const RenderCancellation&) = delete;
    RenderCancellation& operator=(const RenderCancellation&) = delete;

    /**
     * @brief Whether the render was aborted. Asks the effect on the thread that built this,
     * and only reads the published flag on any other.
     **/
    bool check();

private:
    const EffectInstance* _effect;
    std::thread::id _owner;
    std::atomic<bool> _cancelled;
};

/**
 * @brief parallelForOnGlobalPool() that stops once the render is aborted: no index starts after
 * that, and while the calling thread waits for the helpers' last calls it wakes at short
 * intervals to ask the effect, so a body polling cancel.check() between rows returns soon after
 * an abort whichever thread runs it. A render that is not aborted runs every index exactly as
 * parallelForOnGlobalPool() does. Must be called on the thread that built cancel.
 **/
void parallelForCancellable(int count,
                            int maxThreads,
                            RenderCancellation& cancel,
                            const std::function<void(int)>& body);

/**
 * @brief Convenience base class for native (non-OFX) nodes living under Engine/Nodes/, rooted at
 * OutputEffectInstance so a subclass can declare itself a render root (a writer, in practice) the
 * same way an OFX plugin does through its context, rather than being structurally unable to.
 *
 * A hand-written EffectInstance subclass otherwise repeats the same half-dozen
 * one-line overrides (getPluginID(), getPluginLabel(), getPluginGrouping(),
 * getMajorVersion(), getMinorVersion(), getNInputs(), getInputDataKind(), isWriter(),
 * isOutput(), ...) for every new node. NativeEffectBase asks for that metadata once, via a
 * single nodeSpec() override, and implements those EffectInstance and
 * OutputEffectInstance virtuals from it.
 *
 * Everything else about OutputEffectInstance -- knobs, rendering, undo, serialization,
 * scheduling -- is unchanged; the one virtual this class adds beyond that metadata is
 * resolveOutputDataKind(), the hook by which a native node supplies its own data-kind
 * resolution policy.
 * A subclass still overrides initializeKnobs() and render() exactly as it would
 * on top of OutputEffectInstance directly, optionally using the createKnob() helper
 * below for the AppManager::createKnob() idiom.
 *
 * Worked example -- a minimal one-input, one-output native node:
 *
 * @code
 * #include "Engine/Nodes/NativeEffectBase.h"
 * #include "Engine/KnobTypes.h"
 *
 * NATRON_NAMESPACE_ENTER
 *
 * class ExampleNode
 *     : public NativeEffectBase
 * {
 * public:
 *     static EffectInstance* BuildEffect(NodePtr node)
 *     {
 *         return new ExampleNode(node);
 *     }
 *
 *     explicit ExampleNode(NodePtr node)
 *         : NativeEffectBase(node)
 *     {
 *     }
 *
 * private:
 *     virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
 *     {
 *         return staticNodeSpec<kExampleSpec>();
 *     }
 *
 *     virtual void initializeKnobs() OVERRIDE FINAL
 *     {
 *         KnobPagePtr page = createKnob<KnobPage>( tr("Controls") );
 *         KnobDoublePtr amount = createKnob<KnobDouble>( tr("Amount") );
 *
 *         amount->setDefaultValue(1.);
 *         page->addKnob(amount);
 *     }
 *
 *     virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
 *     {
 *         Q_UNUSED(args);
 *
 *         return eStatusOK;
 *     }
 * };
 *
 * NATRON_NAMESPACE_EXIT
 * @endcode
 **/
class NativeEffectBase
    : public OutputEffectInstance {
public:
    explicit NativeEffectBase(NodePtr node);

    virtual ~NativeEffectBase();

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL;
    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isWriter() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isOutput() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getNInputs() const OVERRIDE WARN_UNUSED_RETURN;
    virtual std::string getInputLabel(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;
    virtual bool isInputOptional(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;
    virtual bool isInputMask(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;
    virtual DataKindEnum getOutputDataKind() const OVERRIDE WARN_UNUSED_RETURN;
    virtual DataKindEnum getInputDataKind(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief The node's static description, which every query above answers from. A node
     * returns staticNodeSpec<kSpec>() of a constexpr spec, which rejects an invalid spec at
     * compile time. The spec must outlive the node and never change.
     **/
    virtual const NodeSpec* nodeSpec() const WARN_UNUSED_RETURN = 0;

    /**
     * @brief How the output at time and view relates to the inputs. For a node with a
     * nodeSpec(), isIdentity() answers from this: Input(i, t) is an identity of input i at t,
     * and Black an identity of no input, which the render paths output as transparent black.
     * The default is None: the node computes its output.
     **/
    virtual Route route(double time,
                        ViewIdx view) WARN_UNUSED_RETURN
    {
        Q_UNUSED(time);
        Q_UNUSED(view);

        return Route::none();
    }

    /**
     * @brief Supplies this node's data-kind resolution policy, consulted when its declared output
     * kind is eDataKindPolymorphic. The default delegates to the engine's structural resolution
     * (Node::resolveStructuralOutputDataKind()): the kinds reaching the node's polymorphic-declared
     * inputs, and the kinds its consumers require of it. A node whose kind follows something
     * narrower -- the input a switch has selected, say -- overrides this, returns that kind and
     * clears *isAmbiguous. Set *isAmbiguous instead when the policy leaves the node holding more
     * than one concrete kind at once; the returned kind is then ignored.
     * A policy that reads anything other than the node's connections -- a knob, typically -- must
     * call Node::invalidateEffectiveOutputDataKindCache() itself when that thing changes: the
     * engine only invalidates the cached answer when a connection changes.
     * Note that the engine cannot predict an overridden policy, so the connection-time check that
     * simulates what a not-yet-made edge would resolve to assumes the structural default; the
     * answer this returns is always what the node is actually taken to carry.
     **/
    virtual DataKindEnum resolveOutputDataKind(bool* isAmbiguous) const WARN_UNUSED_RETURN;

    /**
     * @brief Whether this node's output data kind follows what is connected to inputNb. That is
     * what lets a kind required of this node's output reach back through it to the node feeding
     * that input, resolving a whole chain of pass-throughs from one concrete consumer.
     * The default is true, matching the structural resolution above, which follows every
     * polymorphic-declared input at once. A node that overrides resolveOutputDataKind() to follow
     * something narrower -- the input a switch has selected, say -- must also override this, or
     * the branches its policy ignores are still typed, rendered and checked as the kind the node
     * outputs. An input declaring a concrete kind is unaffected either way: it never contributes.
     **/
    virtual bool inputParticipatesInDataKindPropagation(int /*inputNb*/) const WARN_UNUSED_RETURN
    {
        return true;
    }

    /**
     * @brief What the node's image op computes, which the scheduler's cost model and task records
     * read; empty for a node that is not an image op, which GPU placement never considers.
     **/
    virtual std::optional<OpKind> opKind() const WARN_UNUSED_RETURN
    {
        return std::nullopt;
    }

    /**
     * @brief The cost model's hint for a render at time and view, mipmapLevel levels below full
     * resolution; empty for an op whose cost the plug-in's plain row describes, as by default.
     **/
    virtual std::optional<GpuCostHint> gpuCostHint(double /*time*/,
                                                   ViewIdx /*view*/,
                                                   unsigned int /*mipmapLevel*/) const WARN_UNUSED_RETURN
    {
        return std::nullopt;
    }

    /**
     * @brief Whether renderGpu() can produce this query. Nodes that cannot are rendered on the
     * CPU, as every node is by default. The answer must depend only on the query and the node's
     * settings: placement asks before the region is recorded, and the region engine asks again
     * when it records it.
     **/
    virtual bool canRenderOnGpu(const GpuQuery& /*query*/) const WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief The region of input inputNb that renderGpu() reads to produce outputRect at time and
     * view; wider than outputRect for a node with a halo.
     **/
    virtual RectI getGpuInputRoI(int /*inputNb*/,
                                 double /*time*/,
                                 ViewIdx /*view*/,
                                 const RectI& outputRect,
                                 const RenderScale& /*scale*/) const WARN_UNUSED_RETURN
    {
        return outputRect;
    }

    /**
     * @brief The layer renderGpu() reads input inputNb in when this node renders plane at time and
     * view; the region engine hands that input over in it. False when the GPU path cannot read
     * the input at all, which keeps the node on the CPU, as the default does.
     **/
    virtual bool getGpuInputLayer(int /*inputNb*/,
                                  const ImageLayerDesc& /*plane*/,
                                  double /*time*/,
                                  ViewIdx /*view*/,
                                  ImageLayerDesc* /*layer*/) const WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief The layers beyond getGpuInputLayer()'s that renderGpu() also reads input inputNb in
     * when this node renders plane at time and view, appended to layers. False when the GPU path
     * cannot read them, which keeps the node on the CPU. The default asks for none.
     **/
    virtual bool getGpuExtraInputLayers(int /*inputNb*/,
                                        const ImageLayerDesc& /*plane*/,
                                        double /*time*/,
                                        ViewIdx /*view*/,
                                        std::vector<ImageLayerDesc>* /*layers*/) const WARN_UNUSED_RETURN
    {
        return true;
    }

    /**
     * @brief Whether renderGpu() reads input inputNb at all when this node renders plane at time
     * and view. The region engine hands an unread input over in no layer, so a plane made of
     * constants never asks its input for a layer the input does not carry. The default reads
     * every input.
     **/
    virtual bool readsGpuInput(int /*inputNb*/,
                               const ImageLayerDesc& /*plane*/,
                               double /*time*/,
                               ViewIdx /*view*/) const WARN_UNUSED_RETURN
    {
        return true;
    }

    /**
     * @brief Records the dispatches that fill args.output over args.outputRect. Only called
     * when canRenderOnGpu() accepted args.query; the default fails.
     **/
    virtual StatusEnum renderGpu(const GpuRenderArgs& /*args*/) WARN_UNUSED_RETURN
    {
        return eStatusFailed;
    }

    /**
     * @brief The mask, mix and "(Un)premult by" divisor the stages around the op resolve to at
     * time and view, which GPU placement folds into the query. The default is neutral: no mask,
     * a mix of 1 and no divisor.
     **/
    virtual GpuStageSummary getGpuStageSummary(double /*time*/,
                                               ViewIdx /*view*/) WARN_UNUSED_RETURN
    {
        return GpuStageSummary();
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafeFrame;
    }

    virtual bool rendersOnlyInsideRenderWindow() const OVERRIDE WARN_UNUSED_RETURN
    {
        return true;
    }

    /**
     * @brief The metadata of the image this node outputs at time and view: what
     * deriveOutputMetadata() gives, cached per (time, view) under the node's hash, which moves
     * whenever a knob of this node or anything upstream of it changes or a connection changes.
     * The cache is also dropped when the node's clip preferences are refreshed. Callable from
     * any thread, holding no lock: it reads the node's hash, which may recompute dirty hashes.
     **/
    ImageMetadata getOutputMetadata(double time, ViewIdx view) WARN_UNUSED_RETURN;

    /**
     * @brief The image of input inputNb at time, for the render args describes, over roiPixel
     * when given: getImage() at args' scale and view. time must be one getFramesNeeded() lists
     * for that input and view at args.time, which is what the scheduler planned; any other time
     * posts an error naming it and returns null, so that the render fails rather than pulling
     * an unplanned frame.
     **/
    ImagePtr fetchInputAt(int inputNb,
                          double time,
                          const RenderActionArgs& args,
                          const ImageLayerDesc* layer = NULL,
                          bool mapToClipPrefs = true,
                          RectI* roiPixel = NULL) WARN_UNUSED_RETURN;

    /**
     * @brief Whether getFramesNeeded() at time and view lists time needed of input inputNb.
     **/
    bool declaresInputTime(int inputNb, double time, ViewIdx view, double needed) WARN_UNUSED_RETURN;

protected:
    /**
     * @brief The metadata of the first connected non-mask input at time and view, or empty if
     * none is connected. Metadata of a native input comes from its getOutputMetadata(), that of
     * an OpenFX input (or a Read/Write container) from its output clip's metadata, converted by
     * OfxMetadataBridge; any other kind of effect contributes nothing.
     **/
    ImageMetadata getUpstreamMetadata(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief Derives the metadata getOutputMetadata() caches. The default passes through,
     * unchanged, getUpstreamMetadata(), so a node that does not override this is transparent to
     * metadata. A node that adds, edits or drops keys overrides it, typically starting from
     * getUpstreamMetadata() or getInputMetadata(). The result is cached until one of the events
     * getOutputMetadata() lists, so an override must read only this node's knobs, its upstream
     * metadata and project settings; anything else it reads must invalidate the node's hash
     * when it changes. Called from any thread, possibly more than once for the same frame, and
     * an override must not hold a lock of its own while asking upstream nodes.
     **/
    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) WARN_UNUSED_RETURN;

    /**
     * @brief For a change of this node's metadata that leaves its hash alone, such as a file
     * reload: drops the cached metadata of this node and of every node downstream, and moves the
     * content hashes of the expressions reading any of their metadata (see
     * Node::onMetadataChangedUnderSameHash()). A change that moves the hash needs only
     * dropCachedOutputMetadata(): a revision move would keep the readers from finding the renders
     * of an earlier state again.
     **/
    void invalidateOutputMetadata();

    /**
     * @brief The metadata of the effect connected to inputNb, or empty if nothing is. For a node
     * that picks its source input itself instead of taking the first connected one.
     **/
    ImageMetadata getInputMetadata(int inputNb, double time, ViewIdx view) const WARN_UNUSED_RETURN
    {
        EffectInstancePtr input = getInput(inputNb);

        return input ? getInputEffectMetadata(input, time, view) : ImageMetadata();
    }

    virtual void onNodeHashChanged(U64 hash) OVERRIDE;
    virtual void onCurrentFrameMetadataChanged(const NodeMetadata& metadata) OVERRIDE;

    /**
     * @brief For a node with a nodeSpec(), the answer of route(); otherwise not an identity,
     * as EffectInstance answers, unless a subclass overrides this.
     **/
    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief Drops every cached getOutputMetadata() result of this node alone, for a change that
     * moves its hash anyway, which is what the readers of its metadata follow.
     **/
    virtual void dropCachedOutputMetadata() OVERRIDE;

    /**
     * @brief Pass 1 of a two-pass deep render: returns how many samples the pixel at (x, y) --
     * absolute pixel coordinates within the output DeepImage's bounds -- will hold. Called once
     * per pixel, concurrently from several threads, so it must only read.
     **/
    typedef std::function<U32(int x, int y)> DeepSampleCountFunc;

    /**
     * @brief Pass 2 of a two-pass deep render: fills the pixel at (x, y). out points straight
     * into the output DeepImage's channel buffers at that pixel's offset and is already sized to
     * the count pass 1 returned for it, so this writes out.numSamples samples and nothing else.
     * Called once per pixel, concurrently from several threads; distinct pixels never share
     * storage, so two calls never collide.
     **/
    typedef std::function<void(int x, int y, const MutableDeepPixelView& out)> DeepFillSamplesFunc;

    /**
     * @brief Runs a deep render as the two passes deep evaluation must be split into, and is the
     * only sample-writing path a node built on this class should use. It covers the whole of the
     * output DeepImage's bounds, not just args.roi: a DeepImage has no bitmap, so a pixel left
     * unwritten is indistinguishable from a pixel that genuinely has no samples.
     *
     * A DeepImage's channel buffers are one contiguous array per channel indexed by a prefix sum
     * of per-pixel sample counts, so the total sample count -- and therefore every pixel's
     * offset -- has to be known before a single sample can be written. That rules out appending
     * samples as they are discovered, which is the shape a node author naturally reaches for and
     * which cannot be made both correct and parallel here. Instead: pass 1 asks countSamples for
     * every pixel's count in parallel over scanline chunks, the sample table's offsets and the
     * channel buffers are then built in one allocation, and pass 2 asks fillSamples to fill every
     * pixel in parallel over the same chunks, writing through non-owning views into that single
     * allocation. Nothing is allocated per pixel or per sample in either pass.
     *
     * channelNames are the value channels to allocate, in the order fillSamples' views index
     * them; "Z" and "ZBack" are always allocated on top of them and must not be listed.
     * alphaChannelIndex is the index within channelNames of the alpha channel, recorded in every
     * view handed to fillSamples so that the ops in DeepPixelOps cannot disagree about it.
     *
     * resultIsTidy records on the output whether this node guarantees the samples it just wrote
     * are sorted by depth and non-overlapping; it is not verified.
     **/
    StatusEnum renderDeepTwoPass(const DeepRenderActionArgs& args,
                                 const std::vector<std::string>& channelNames,
                                 int alphaChannelIndex,
                                 const DeepSampleCountFunc& countSamples,
                                 const DeepFillSamplesFunc& fillSamples,
                                 bool resultIsTidy = false) WARN_UNUSED_RETURN;

    /**
     * @brief The one pass of renderDeepFromInput(): rewrites the pixel at (x, y). in views the
     * input's samples there -- its channels are the input's minus "Z" and "ZBack", in the order
     * DeepImage::getChannels() lists them, in.alphaChannelIndex naming "A" among them or -1 when
     * the input has none -- and out points into the output's buffers for the channels
     * renderDeepFromInput() was told to write, in that order, over those same samples, "Z" and
     * "ZBack" included when they were listed -- out.z and out.zback then point at those same
     * buffers, and are null otherwise, the depths staying the input's. Called once per pixel
     * holding samples, concurrently from several threads; distinct pixels never share storage.
     **/
    typedef std::function<void(int x, int y, const DeepPixelView& in, const MutableDeepPixelView& out)> DeepRewriteSamplesFunc;

    /**
     * @brief Runs a deep render that keeps its input's sample structure -- which samples exist
     * and at what depths -- and rewrites the values of some channels, the way a grade or a
     * recolour does. Rather than filling the output from scratch, the output aliases the input
     * (DeepImage::aliasContentsOf()) when their bounds agree, and otherwise copies the input's
     * samples over the output's bounds, an input served from the cache being possibly wider than
     * the window being rendered. Only the channels named in channelsToWrite are then detached, or
     * created for a name the input lacks, and handed to rewrite; the sample table and every
     * other channel stay shared with the input for as long as both live, and nothing rewrite is
     * given can reach the input's storage.
     *
     * alphaChannelIndex is the index within channelsToWrite of the alpha channel, recorded in
     * every out view, or -1 when alpha is not being written. "Z" and "ZBack" may be listed, and
     * are handed to rewrite like any other channel; an input lacking "ZBack" gets one, zeroed
     * on the aliasing path and equal to "Z" on the copying path, for rewrite to fill.
     *
     * The output's tidiness is the input's: rewriting values moves no sample, and a node that
     * rewrites depths is the one to know whether its samples are still sorted. The cache
     * charges an aliased output for every channel it holds all the same (DeepImage's
     * getSizeInBytes() is aliasing-blind), so sharing evicts early rather than desyncing.
     **/
    StatusEnum renderDeepFromInput(const DeepRenderActionArgs& args,
                                   const DeepImagePtr& input,
                                   const std::vector<std::string>& channelsToWrite,
                                   int alphaChannelIndex,
                                   const DeepRewriteSamplesFunc& rewrite) WARN_UNUSED_RETURN;

    /**
     * @brief Runs a deep render that keeps its input's sample structure and every channel it does
     * not name, removing the channels in drop and adding a zero-filled one for each name in
     * addZero the input lacks. The output aliases the input the way renderDeepFromInput() does,
     * and copies the input's samples over the output's bounds when those differ, so the sample
     * table and every untouched channel stay shared on the aliasing path. A name in drop the
     * input lacks is ignored, and so is a name in addZero it already has: an existing channel is
     * never detached or zeroed. "Z" and "ZBack" are never dropped or added, whichever list names
     * them. The output's tidiness is the input's.
     **/
    StatusEnum renderDeepReshapingChannels(const DeepRenderActionArgs& args,
                                           const DeepImagePtr& input,
                                           const std::vector<std::string>& drop,
                                           const std::vector<std::string>& addZero) WARN_UNUSED_RETURN;

    /**
     * @brief Splits bounds into the scanline chunks renderDeepTwoPass() parallelizes over.
     * Exposed so a node needing its own parallel pass over the same rectangle can use the same
     * partition, and so tests can reason about it.
     **/
    static void makeDeepScanlineChunks(const RectI& bounds,
                                       std::vector<RectI>* chunks);

    /**
     * @brief Wraps the AppManager::createKnob() idiom every initializeKnobs() repeats.
     **/
    template <typename KNOB_TYPE>
    std::shared_ptr<KNOB_TYPE> createKnob(const std::string& label,
                                          int dimension = 1)
    {
        return AppManager::createKnob<KNOB_TYPE>(this, label, dimension);
    }

    template <typename KNOB_TYPE>
    std::shared_ptr<KNOB_TYPE> createKnob(const QString& label,
                                          int dimension = 1)
    {
        return AppManager::createKnob<KNOB_TYPE>(this, label, dimension);
    }

    /**
     * @brief For a scene node's renderScene(): sets *output to upstream with this node's layer on
     * top. The layer is authored through author only when this node's plug-in, knobs or
     * appendNodeContentHash() state, the project frame range, rate or scene units, or the upstream stack have
     * changed since the last one; otherwise the same layer is re-emitted. author must read only
     * those, and author everything from scratch into the empty layer it is handed.
     **/
    bool emitSceneLayer(const ScenePayload& upstream,
                        const SceneLayerMemo::AuthorFunc& author,
                        ScenePayload* output,
                        std::string* errorMessage) WARN_UNUSED_RETURN;

    /**
     * @brief How a node's layer key covers upstream and time: by default the whole upstream stack
     * and the project frame range.
     **/
    struct SceneLayerReads {
        SceneLayerMemo::Key::UpstreamReadEnum upstream = SceneLayerMemo::Key::eUpstreamReadWholeStack;
        // For eUpstreamReadFacts: a hash of every upstream fact the author reads.
        U64 factsDigest = 0;
        bool frameRange = true;
    };

    /**
     * @brief emitSceneLayer(), keyed on only what reads says the node reads, so an upstream edit
     * re-authors this layer only when something it reads changed. author must read nothing more.
     **/
    bool emitSceneLayer(const ScenePayload& upstream,
                        const SceneLayerReads& reads,
                        const SceneLayerMemo::AuthorFunc& author,
                        ScenePayload* output,
                        std::string* errorMessage) WARN_UNUSED_RETURN;

    /**
     * @brief The key emitSceneLayer() memoizes this node's layer on, for a node that memoizes
     * layers of its own on the same terms. Returns false with *errorMessage set while the node is
     * being destroyed.
     **/
    bool getSceneLayerKey(const SceneLayerReads& reads,
                          SceneLayerMemo::Key* key,
                          std::string* errorMessage) WARN_UNUSED_RETURN;

    /**
     * @brief Makes the next emitSceneLayer() author a new layer, for a change none of its inputs
     * see.
     **/
    void invalidateSceneLayer();

    // How many layers emitSceneLayer() has authored.
    U64 getSceneLayerAuthorCount() const WARN_UNUSED_RETURN;

private:
    static ImageMetadata getInputEffectMetadata(const EffectInstancePtr& connected,
                                                double time,
                                                ViewIdx view) WARN_UNUSED_RETURN;

    typedef std::pair<double, int> MetadataKey;

    // A leaf lock: never held while another effect is asked for its metadata, and taken by
    // invalidation, which runs upstream to downstream. See
    // docs/decisions/2026-09-10-metadata-cache-locking.md.
    std::mutex _metadataCacheMutex;
    std::map<MetadataKey, ImageMetadata> _metadataCache;
    U64 _metadataGeneration;
    U64 _metadataCacheHash; //< the node hash every entry of _metadataCache was derived under

    SceneLayerMemo _sceneLayerMemo;

    typedef std::function<void(const RectI& chunk)> DeepChunkFunc;

    // Fills the output over its own bounds with the input's samples: the input's channels but
    // those in dropNames, then a zero-filled one for each name in extraNames the input lacks.
    StatusEnum copyDeepInputOverOutputBounds(const DeepRenderActionArgs& args,
                                             const DeepImagePtr& input,
                                             const std::vector<std::string>& dropNames,
                                             const std::vector<std::string>& extraNames) WARN_UNUSED_RETURN;

    // Runs body over every chunk in parallel, each render thread carrying the calling thread's
    // TLS for the duration. Returns false if the render was aborted, in which case some chunks
    // were skipped.
    bool forEachDeepChunk(const std::vector<RectI>& chunks,
                          const DeepChunkFunc& body) WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_NativeEffectBase_h
