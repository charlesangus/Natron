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

#ifndef Engine_Nodes_Merge_Merge_h
#define Engine_Nodes_Merge_Merge_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/Nodes/Merge/MergeOperators.h"

#define PLUGINID_NATRON_MERGE "net.sf.openfx.MergePlugin"
#define PLUGINID_NATRON_MERGE_PREFIX "net.sf.openfx.Merge"
#define PLUGIN_MAJOR_NATRON_MERGE 3

#define kMergeParamOperation "operation"
#define kMergeParamBBox "bbox"
#define kMergeParamAlphaMasking "screenAlpha"
#define kMergeParamAChannels "AChannels"
#define kMergeParamBChannels "BChannels"
#define kMergeParamOutputChannels "OutputChannels"

#define kMergeBBoxUnion "union"
#define kMergeBBoxIntersection "intersection"
#define kMergeBBoxA "a"
#define kMergeBBoxB "b"

// Input indices, in the OpenFX clip order that scripts connect by: B, A, Mask, then A2..A64.
#define kMergeInputB 0
#define kMergeInputA 1
#define kMergeInputMask 2
#define kMergeMaxAInputs 64

NATRON_NAMESPACE_ENTER

/**
 * @brief Which ID a MergeNode is registered under: the Merge node itself or one of its presets,
 * which differ only in ID, label, menu and default operation.
 **/
enum MergePresetEnum {
    eMergePresetMerge,
    eMergePresetPlus,
    eMergePresetMatte,
    eMergePresetMultiply,
    eMergePresetIn,
    eMergePresetOut,
    eMergePresetScreen,
    eMergePresetMax,
    eMergePresetMin,
    eMergePresetDifference
};

/**
 * @brief Pixel-by-pixel merge of one or more A inputs over B, with the operators, input handling,
 * region of definition and identity rules of openfx-misc's Merge 2.0, registered under its IDs
 * one major above.
 *
 * Input A is merged over B (over black and transparent when B is absent), then each later A
 * input over the running result. The A and B channel toggles zero the channels they turn off,
 * Output channels left off keep B, and the result is then mixed with B by mask x mix. With no A
 * input, an operator that leaves B unchanged over a transparent A copies B; any other operator
 * still runs once with a transparent A.
 *
 * The class is not named Merge because Engine/MergingEnum.h already declares a namespace of that
 * name in the Natron namespace.
 **/
class MergeNode
    : public NativeImageEffect {
public:
    MergeNode(NodePtr node,
              MergePresetEnum preset);

    virtual ~MergeNode();

    static std::string presetPluginID(MergePresetEnum preset);
    static MergeOperators::Operation presetOperation(MergePresetEnum preset);

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return kMergeMaxAInputs + 2;
    }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isInputMask(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual DataKindEnum getInputDataKind(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The metadata of the first connected A input (A, then A2, ...), or of B when no A is
     * connected. The Mask never contributes.
     **/
    virtual ImageMetadata getOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The input index of the i-th A input (0 for A, 1 for A2, ...), or -1 past the last.
     **/
    static int aInputIndex(int i) WARN_UNUSED_RETURN;

    /**
     * @brief `source`, or a private copy of it when it is one of `outputs`. The render holds every
     * output for writing while it reads its sources, and the image lock cannot be taken for
     * reading by the thread that holds it for writing.
     **/
    static ImagePtr sourceDetachedFromOutputs(const ImagePtr& source,
                                              const std::vector<ImagePtr>& outputs) WARN_UNUSED_RETURN;

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    MergeOperators::Operation getOperation(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    // Shows the operator next to the node's label and enables "Alpha masking" where it applies.
    void refreshOperationDependents(double time);

    // The region of definition of input inputNb, when it is connected and has one.
    bool getInputRoD(int inputNb, double time, const RenderScale& scale, ViewIdx view, RectD* rod) const WARN_UNUSED_RETURN;

    const MergePresetEnum _preset;
    KnobStringWPtr _subLabel;
    KnobChoiceWPtr _operation;
    KnobChoiceWPtr _bbox;
    KnobBoolWPtr _alphaMasking;
    KnobBoolWPtr _aChannels[4];
    KnobBoolWPtr _bChannels[4];
    KnobBoolWPtr _outputChannels[4];
};

/**
 * @brief The factory the built-in plug-in registry needs for each ID: it builds an effect
 * through a static function that takes no preset argument.
 **/
template <MergePresetEnum PRESET>
struct MergePreset {
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new MergeNode(node, PRESET);
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Merge_Merge_h
