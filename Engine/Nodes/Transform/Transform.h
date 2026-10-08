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

#ifndef Engine_Nodes_Transform_Transform_h
#define Engine_Nodes_Transform_Transform_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/Transform.h"

#define PLUGINID_NATRON_TRANSFORM "net.sf.openfx.TransformPlugin"
#define PLUGIN_MAJOR_NATRON_TRANSFORM 2
#define PLUGINID_NATRON_TRANSFORMMASKED "net.sf.openfx.TransformMaskedPlugin"
#define PLUGIN_MAJOR_NATRON_TRANSFORMMASKED 2

#define kTransformNodeParamTranslate "translate"
#define kTransformNodeParamRotate "rotate"
#define kTransformNodeParamScale "scale"
#define kTransformNodeParamUniform "uniform"
#define kTransformNodeParamSkewX "skewX"
#define kTransformNodeParamSkewY "skewY"
#define kTransformNodeParamSkewOrder "skewOrder"
#define kTransformNodeParamAmount "transformAmount"
#define kTransformNodeParamCenter "center"
#define kTransformNodeParamResetCenter "resetCenter"
#define kTransformNodeParamCenterChanged "transformCenterChanged"
#define kTransformNodeParamInteractOpen "transformInteractOpen"
#define kTransformNodeParamInteractive "interactive"
#define kTransformNodeParamHiDPI "hidpi"
#define kTransformNodeParamInvert "invert"
#define kTransformNodeParamMotionBlur "motionBlur"
#define kTransformNodeParamDirectionalBlur "directionalBlur"
#define kTransformNodeParamShutter "shutter"
#define kTransformNodeParamSrcClipChanged "srcClipChanged"

NATRON_NAMESPACE_ENTER

/**
 * @brief Transform: translates, rotates, scales and skews the image about a centre, resampling
 * it with one of the ten OpenFX filters, with optional motion blur over the shutter or
 * directional blur along the transform. Knob names, defaults, regions, identity rules and pixel
 * maths are the openfx-misc TransformPlugin's (through Resampler and TransformMath); it
 * registers under that plug-in's ID one major above.
 *
 * It concatenates: getTransform() hands the host its pixel matrix so a chain of transforms is
 * resampled once, and render() folds in the transform the host hands back with a concatenated
 * source image. TransformMasked is the same node with a Mask input and the maskInvert and mix
 * knobs; it applies concatenated upstream transforms but does not offer its own. Where mask x mix
 * is below 1 it shows its immediate input, which under concatenation it resamples through the
 * upstream transforms alone.
 *
 * The class is not called Transform because that name is the engine's matrix namespace.
 **/
class TransformNode
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TransformNode(node, false);
    }

    virtual ~TransformNode();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual bool rendersUnprocessedChannels() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return !_masked;
    }

    virtual bool getInputsHoldingTransform(std::list<int>* inputs) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getTransform(double time,
                                    const RenderScale& renderScale,
                                    bool draftRender,
                                    ViewIdx view,
                                    EffectInstancePtr* inputToTransform,
                                    Transform::Matrix3x3* transform) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The transform knob values at (time, view).
     **/
    void getTransformParams(double time, ViewIdx view, TransformMath::TransformParams* params) const;

    /**
     * @brief Moves the centre to the middle of the source's region of definition (the project
     * window when that is empty), changing translate so the image does not move, as the
     * OpenFX plug-in's resetCenter does. Nothing happens without a source or with an infinite
     * source region.
     **/
    void resetCenter(double time);

protected:
    TransformNode(NodePtr node,
                  bool masked);

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE WARN_UNUSED_RETURN;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    /**
     * @brief The canonical matrix function the resampler and the region helpers sample, reading
     * the knobs at the time they ask for.
     **/
    Resampler::CanonicalTransformFn makeCanonicalTransformFn(ViewIdx view) const;

    /**
     * @brief The blur knob values at (time, view), with the shutter as the knob holds it.
     **/
    Resampler::BlurSettings getBlurSettings(double time, ViewIdx view) const;

    /**
     * @brief Whether a mask is read: only for TransformMasked, with its mask connected and enabled.
     **/
    bool isMasking() const WARN_UNUSED_RETURN;

    Resampler::RegionParams getRegionParams(double time, ViewIdx view) const;

    bool getSourceRoD(double time, const RenderScale& scale, ViewIdx view, RectD* rod) const;

    RectD getProjectRect() const WARN_UNUSED_RETURN;

    void updateShutterEnabled();

    /**
     * @brief Shows the uniform knob once it is checked or animated; with allowHiding, hides it
     * otherwise, as the OpenFX plug-in does when an instance is created.
     **/
    void updateUniformVisibility(bool allowHiding);

    const bool _masked;
    KnobDoubleWPtr _translate;
    KnobDoubleWPtr _rotate;
    KnobDoubleWPtr _scale;
    KnobBoolWPtr _uniform;
    KnobDoubleWPtr _skewX;
    KnobDoubleWPtr _skewY;
    KnobChoiceWPtr _skewOrder;
    KnobDoubleWPtr _amount;
    KnobDoubleWPtr _center;
    KnobButtonWPtr _resetCenter;
    KnobBoolWPtr _centerChanged;
    KnobBoolWPtr _interactOpen;
    KnobBoolWPtr _interactive;
    KnobBoolWPtr _hiDPI;
    KnobBoolWPtr _invert;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobBoolWPtr _blackOutside;
    KnobDoubleWPtr _motionBlur;
    KnobBoolWPtr _directionalBlur;
    KnobDoubleWPtr _shutter;
    KnobChoiceWPtr _shutterOffset;
    KnobDoubleWPtr _shutterCustomOffset;
    KnobBoolWPtr _srcClipChanged;
};

/**
 * @brief TransformMasked: Transform with a Mask input and the maskInvert and mix knobs, under the
 * openfx-misc TransformMaskedPlugin's ID one major above.
 **/
class TransformMasked
    : public TransformNode {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TransformMasked(node);
    }

    explicit TransformMasked(NodePtr node)
        : TransformNode(node, true)
    {
    }

    virtual ~TransformMasked()
    {
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_Transform_h
