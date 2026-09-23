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
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_SHUFFLE "fr.natron.Shuffle"

#define kShuffleParamIn1Input "in1Input"
#define kShuffleParamIn2Input "in2Input"
#define kShuffleParamIn1 "in1"
#define kShuffleParamIn2 "in2"
#define kShuffleParamOut1 "out1"
#define kShuffleParamOut2 "out2"
#define kShuffleParamMapping "mapping"

NATRON_NAMESPACE_ENTER

class KnobShuffleMap;

/**
 * @brief Moves channels between layers: the only node allowed to produce a plane whose
 * channels differ from its source's.
 *
 * Two input slots (in1, in2), each a layer read from input B or A, feed two output layers
 * (out1, out2). Every output channel takes its source from the mapping knob: a slot channel,
 * a constant 0 or 1, or keep, which is B's same channel of the same layer. Only out1 and out2
 * are produced; every other layer of B passes through untouched, Color included unless it is
 * an output.
 **/
class Shuffle
    : public NativeEffectBase {
public:
    enum InputEnum {
        eInputB = 0,
        eInputA = 1
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
     * @brief The input slot slot (1 or 2) reads from: eInputB or eInputA.
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

protected:
    virtual void getFrameRange(double* first, double* last) OVERRIDE FINAL;

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

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
     * @brief A row wired to a slot whose input is connected but that no longer carries the
     * slot's layer, or whose index is beyond it, fails the render naming that channel. A
     * disconnected input, a None slot or an unwired channel is silent (keep). The mapping
     * knob stores overrides only, so this is the node that resolves their readability, mirroring
     * checkSelectedChannelsPresent()'s mask rule.
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

    void syncSlotInputs();

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

    KnobChoiceWPtr _in1Input;
    KnobChoiceWPtr _in2Input;
    KnobLayerSelectWPtr _in1;
    KnobLayerSelectWPtr _in2;
    KnobLayerSelectWPtr _out1;
    KnobLayerSelectWPtr _out2;
    std::weak_ptr<KnobShuffleMap> _mapping;
    KnobStringWPtr _subLabel;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Channel_Shuffle_h
