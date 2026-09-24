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

#ifndef Engine_Nodes_Channel_Shuffle_h
#define Engine_Nodes_Channel_Shuffle_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_SHUFFLE "fr.natron.Shuffle"
#define PLUGINID_NATRON_SHUFFLECOPY "fr.natron.ShuffleCopy"

#define kShuffleParamIn1 "in1"
#define kShuffleParamIn2 "in2"
#define kShuffleParamOut1 "out1"
#define kShuffleParamOut2 "out2"
#define kShuffleParamMapping "mapping"

NATRON_NAMESPACE_ENTER

/**
 * @brief Moves channels between layers: the only node allowed to produce a plane whose
 * channels differ from its source's.
 *
 * Two input slots (in1, in2), each a layer, feed two output layers (out1, out2). Shuffle reads
 * both slots from its one input; ShuffleCopy reads in2 from its main input "2" and in1 from
 * input "1". Every output channel outK.i takes the source its mapping row names (a slot
 * channel, 0 or 1), or inK.i when it has no row. Only out1 and out2 are produced; every other
 * layer of the main input passes through untouched, Color included unless it is an output.
 **/
class Shuffle
    : public NativeEffectBase {
public:
    enum InputEnum {
        eInputMain = 0,
        eInputCopy1 = 1
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Shuffle(node);
    }

    explicit Shuffle(NodePtr node);

    virtual ~Shuffle();

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool producesMetadataLayerImplicitly() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Whether this is a ShuffleCopy, whose in1 reads input "1" rather than the main input.
     **/
    virtual bool isCopy() const WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief The input slot (1 or 2) reads from: eInputMain, or eInputCopy1 for a ShuffleCopy's in1.
     **/
    int getSlotInput(int slot) const WARN_UNUSED_RETURN;

    /**
     * @brief The layer ID slot (1 or 2) reads, empty for None.
     **/
    std::string getSlotLayer(int slot) const WARN_UNUSED_RETURN;

    /**
     * @brief The layer ID output slot (1 or 2) writes, empty for None. out2 naming the same
     * layer as out1 resolves to None.
     **/
    std::string getOutputLayer(int slot) const WARN_UNUSED_RETURN;

    /**
     * @brief The source outSlot's channel outIndex actually renders from. A mapping row is
     * returned as stored. Without one, the implicit inK.i (K = outSlot, i = outIndex) is 0 when
     * slot K is None or when i is at or beyond the channel count of slot K's layer, Color
     * counting as RGBA.
     **/
    ShuffleSource getEffectiveSource(int outSlot, int outIndex) const WARN_UNUSED_RETURN;

protected:
    virtual void getFrameRange(double* first, double* last) OVERRIDE FINAL;

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief An explicit row wired to a slot whose input is connected but that no longer carries
     * the slot's layer, or whose index is beyond it, fails the render naming that channel. A
     * disconnected input, a None slot or a channel with no row is silent and renders 0. The
     * mapping knob stores overrides only, so this is the node that resolves their readability,
     * mirroring checkSelectedChannelsPresent()'s mask rule.
     **/
    virtual bool checkExtraChannelsPresent(std::string* message) OVERRIDE FINAL WARN_UNUSED_RETURN;

    struct FetchedPlane {
        int inputNb;
        std::string layerID;
        ImagePtr image;

        FetchedPlane()
            : inputNb(-1)
            , layerID()
            , image()
        {
        }
    };

    /**
     * @brief inputNb's plane layerID for this render, fetched once and remembered in fetched.
     * Null when the input is disconnected or does not carry that layer.
     **/
    ImagePtr fetchInputPlane(const RenderActionArgs& args,
                             int inputNb,
                             const std::string& layerID,
                             std::vector<FetchedPlane>* fetched);

    /**
     * @brief The number of channels layerID has, Color counting as RGBA, resolved against the
     * project registry and then inputNb's present layers. -1 when neither knows the layer.
     **/
    int layerChannelCount(const std::string& layerID, int inputNb) const WARN_UNUSED_RETURN;

    bool slotIsRead(int slot) const WARN_UNUSED_RETURN;

    bool mappingReadsInput(int inputNb) const WARN_UNUSED_RETURN;

    bool resolveOutputLayerDesc(const std::string& layerID,
                                double time,
                                ViewIdx view,
                                ImageLayerDesc* desc) WARN_UNUSED_RETURN;

    /**
     * @brief The display label (e.g. "Color", "diffuse") for layerID, resolved against
     * inputNb's present layers when the project registry does not know it. Falls back to
     * the raw ID so an unresolved layer never leaves the sub-label blank.
     **/
    std::string resolveLayerLabel(const std::string& layerID, int inputNb, double time, ViewIdx view);

    /**
     * @brief Builds the node-graph sub-label text (unparenthesized) from the current
     * in1/in2/out1/out2 selections, following kNatronOfxParamStringSublabelName's
     * PrecompNode precedent: Node wraps and displays it, this only computes the text.
     **/
    std::string buildSubLabel();

    void refreshSubLabel();

    KnobLayerSelectWPtr _in1;
    KnobLayerSelectWPtr _in2;
    KnobLayerSelectWPtr _out1;
    KnobLayerSelectWPtr _out2;
    std::weak_ptr<KnobShuffleMap> _mapping;
    KnobStringWPtr _subLabel;
};

/**
 * @brief A Shuffle whose in1 reads a second input: input 0 ("2") is the main input and feeds
 * in2, input 1 ("1") feeds in1. By default out1's RGB comes from "2" and its alpha from "1".
 **/
class ShuffleCopy
    : public Shuffle {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ShuffleCopy(node);
    }

    explicit ShuffleCopy(NodePtr node);

    virtual ~ShuffleCopy();

    virtual bool isCopy() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Channel_Shuffle_h
