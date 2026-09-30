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

#include <bitset>
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
#define kShuffleCopyParamBBox "bbox"

NATRON_NAMESPACE_ENTER

/**
 * @brief Moves channels between layers: the only node allowed to produce a plane whose
 * channels differ from its source's.
 *
 * Two input slots (in1, in2), each a layer, feed two output layers (out1, out2). Shuffle reads
 * both slots from its one input; ShuffleCopy reads in2 from its main input "2" and in1 from
 * input "1". Every output channel outK.i takes the source its mapping row names (a slot
 * channel, 0 or 1), or inK.i when it has no row. Only out1 and out2 are produced; every other
 * layer of the main input passes through untouched.
 *
 * The colour views (rgba, rgb, alpha, xy) all alias the one colour plane by channel bit, so a
 * colour output writes only the bits its view covers and the plane's other bits pass through
 * from the main input's colour plane. A colour channel an input lacks reads 0.
 **/
class Shuffle
    : public NativeEffectBase {
public:
    enum InputEnum {
        eInputMain = 0,
        eInputCopy1 = 1
    };

    /**
     * @brief A ShuffleCopy's region of definition when both inputs are connected, in the
     * order of its bbox knob's entries.
     **/
    enum BBoxEnum {
        eBBoxUnion = 0,
        eBBoxMain,
        eBBoxInput1,
        eBBoxIntersection
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

    /**
     * @brief The one connected input's region or, with both of a ShuffleCopy's inputs
     * connected, the combination its bbox knob selects. The format, pixel aspect ratio and
     * frame range follow input 0 regardless.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The bbox knob's choice; always eBBoxUnion on a Shuffle, which has no such knob.
     **/
    BBoxEnum getBBox() const WARN_UNUSED_RETURN;

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
     * @brief The layer ID output slot (1 or 2) writes, empty for None. out2 resolves to None
     * when it names the same layer as out1, or when both are colour views sharing a channel bit
     * (rgba and alpha overlap; rgb and alpha do not, and merge into the one colour plane).
     **/
    std::string getOutputLayer(int slot) const WARN_UNUSED_RETURN;

    /**
     * @brief The colour bits (see ResolvedLayer::channelBit) the colour-view outputs write: the
     * union of out1's and out2's view masks, empty when neither is a colour view.
     **/
    std::bitset<4> getOutputColorBits() const WARN_UNUSED_RETURN;

    /**
     * @brief getOutputColorBits(), whatever storage is: Shuffle has no host layer knob to read.
     **/
    virtual void getColorWriteBits(const ImageLayerDesc& storage, std::bitset<4>* bits) const OVERRIDE FINAL;

    /**
     * @brief The source outSlot's channel outIndex actually renders from. A mapping row is
     * returned as stored. Without one the source is implicit: 0 when slot K (K = outSlot) is
     * None. When outK and slot K are both colour views, it is the slot's channel on the same
     * colour bit (alpha's A reads rgba's A, xy's X reads rgba's R), or 0 when the slot's view
     * does not cover that bit. Otherwise it is inK.i (i = outIndex), even when slot K's layer
     * lacks that channel: checkExtraChannelsPresent() then fails the render on it.
     **/
    ShuffleSource getEffectiveSource(int outSlot, int outIndex, double time, ViewIdx view) const WARN_UNUSED_RETURN;

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
     * @brief Every output channel the render produces is checked through its effective source,
     * explicit row or implicit alike: a non-colour slot whose input is connected but does not
     * carry the slot's layer at (time, view), or whose layer lacks the channel, fails the render
     * naming that channel. A colour-view slot never fails: a colour channel its input lacks
     * reads 0. A disconnected input or a None slot is silent and renders 0, and a row whose
     * output channel the current output layer does not have is ignored. The mapping knob stores
     * overrides only, so this is the node that resolves their readability, mirroring
     * checkSelectedChannelsPresent()'s mask rule.
     **/
    virtual bool checkExtraChannelsPresent(double time,
                                           ViewIdx view,
                                           std::string* message) OVERRIDE FINAL WARN_UNUSED_RETURN;

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
     * Null when the input is disconnected or does not carry that layer. The colour plane is
     * fetched as kNatronColorLayerID, whatever view reads it.
     **/
    ImagePtr fetchInputPlane(const RenderActionArgs& args,
                             int inputNb,
                             const std::string& layerID,
                             std::vector<FetchedPlane>* fetched);

    /**
     * @brief The number of channels layerID has at (time, view): a colour view's own count (4,
     * 3, 1 or 2), otherwise resolved against the project registry and then inputNb's layers
     * present at (time, view). -1 when neither knows the layer.
     **/
    int layerChannelCount(const std::string& layerID, int inputNb, double time, ViewIdx view) const WARN_UNUSED_RETURN;

    bool slotIsRead(int slot, double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The plane the render produces for output layerID: for a colour view, the colour
     * storage plane of getOutputColorStorage().
     **/
    bool resolveOutputLayerDesc(const std::string& layerID,
                                double time,
                                ViewIdx view,
                                ImageLayerDesc* desc) WARN_UNUSED_RETURN;

    /**
     * @brief The colour storage plane inputNb carries at (time, view), false when the input is
     * disconnected or carries none.
     **/
    bool getInputColorStorage(int inputNb,
                              double time,
                              ViewIdx view,
                              ImageLayerDesc* storage) WARN_UNUSED_RETURN;

    /**
     * @brief The layout of the one colour plane the colour-view outputs produce: the main
     * input's when it carries every bit of getOutputColorBits(), otherwise RGBA.
     **/
    ImageLayerDesc getOutputColorStorage(double time, ViewIdx view) WARN_UNUSED_RETURN;

    /**
     * @brief The display label (e.g. "rgba", "diffuse") for layerID, resolved against
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
    KnobChoiceWPtr _bbox;
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
