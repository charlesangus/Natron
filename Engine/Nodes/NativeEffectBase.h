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

#include <functional>
#include <string>
#include <vector>

#include "Engine/AppManager.h" // for AppManager::createKnob
#include "Engine/DeepPixelOps.h"
#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/OutputEffectInstance.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Statically describes one input of a NativeEffectBase subclass: its label,
 * whether it may be left unconnected, and the DataKindEnum it accepts.
 **/
struct NativeInputDescription {
    std::string label;
    bool optional;
    DataKindEnum kind;

    explicit NativeInputDescription(const std::string& label_,
                                    bool optional_ = false,
                                    DataKindEnum kind_ = eDataKindImage)
        : label(label_)
        , optional(optional_)
        , kind(kind_)
    {
    }
};

/**
 * @brief Statically describes a NativeEffectBase subclass: plugin id/label/description,
 * grouping, version, its inputs, the DataKindEnum it outputs, and whether it is a writer. One
 * instance of this, returned by NativeEffectBase::getNativePluginDescription(), replaces the
 * half-dozen one-line EffectInstance accessor overrides a hand-written node otherwise repeats.
 **/
struct NativePluginDescription {
    std::string id;
    std::string label;
    std::string description;
    std::string grouping;
    int majorVersion;
    int minorVersion;
    std::vector<NativeInputDescription> inputs;
    DataKindEnum outputKind;
    bool isWriter;

    NativePluginDescription()
        : id()
        , label()
        , description()
        , grouping(PLUGIN_GROUP_OTHER)
        , majorVersion(1)
        , minorVersion(0)
        , inputs()
        , outputKind(eDataKindImage)
        , isWriter(false)
    {
    }
};

/**
 * @brief Convenience base class for native (non-OFX) nodes living under Engine/Nodes/, rooted at
 * OutputEffectInstance so a subclass can declare itself a render root (a writer, in practice) the
 * same way an OFX plugin does through its context, rather than being structurally unable to.
 *
 * A hand-written EffectInstance subclass otherwise repeats the same half-dozen
 * one-line overrides (getPluginID(), getPluginLabel(), getPluginGrouping(),
 * getMajorVersion(), getMinorVersion(), getNInputs(), getInputDataKind(), isWriter(),
 * isOutput(), ...) for every new node. NativeEffectBase asks for that metadata once, via a
 * single getNativePluginDescription() override, and implements those EffectInstance and
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
 *     virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
 *     {
 *         NativePluginDescription desc;
 *
 *         desc.id = "fr.natron.ExampleNode";
 *         desc.label = "Example";
 *         desc.description = "A minimal native node built on NativeEffectBase.";
 *         desc.grouping = PLUGIN_GROUP_OTHER;
 *         desc.majorVersion = 1;
 *         desc.minorVersion = 0;
 *         desc.inputs.push_back( NativeInputDescription("Source", false, eDataKindImage) );
 *         desc.outputKind = eDataKindImage;
 *         desc.isWriter = false;
 *
 *         return desc;
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

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().id;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().label;
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().description;
    }

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    {
        grouping->push_back(getNativePluginDescription().grouping);
    }

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().majorVersion;
    }

    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().minorVersion;
    }

    virtual bool isWriter() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().isWriter;
    }

    virtual bool isOutput() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().isWriter;
    }

    virtual int getNInputs() const OVERRIDE WARN_UNUSED_RETURN
    {
        return (int)getNativePluginDescription().inputs.size();
    }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;
    virtual bool isInputOptional(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;

    virtual DataKindEnum getOutputDataKind() const OVERRIDE WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().outputKind;
    }

    virtual DataKindEnum getInputDataKind(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;

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

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafeFrame;
    }

protected:
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
     * @brief Splits bounds into the scanline chunks renderDeepTwoPass() parallelizes over.
     * Exposed so a node needing its own parallel pass over the same rectangle can use the same
     * partition, and so tests can reason about it.
     **/
    static void makeDeepScanlineChunks(const RectI& bounds,
                                       std::vector<RectI>* chunks);

    /**
     * @brief Subclasses declare their static plugin metadata by overriding this.
     * It is queried on demand rather than cached: every call site above is off the
     * render path (registration, UI display, project load), so re-building the
     * small description each time keeps this class free of per-instance state.
     **/
    virtual NativePluginDescription getNativePluginDescription() const WARN_UNUSED_RETURN = 0;

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

private:
    typedef std::function<void(const RectI& chunk)> DeepChunkFunc;

    // Runs body over every chunk in parallel, each render thread carrying the calling thread's
    // TLS for the duration. Returns false if the render was aborted, in which case some chunks
    // were skipped.
    bool forEachDeepChunk(const std::vector<RectI>& chunks,
                          const DeepChunkFunc& body) WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_NativeEffectBase_h
