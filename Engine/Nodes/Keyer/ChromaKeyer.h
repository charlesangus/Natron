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

#ifndef Engine_Nodes_Keyer_ChromaKeyer_h
#define Engine_Nodes_Keyer_ChromaKeyer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"

#define PLUGINID_NATRON_CHROMAKEYER "net.sf.openfx.ChromaKeyerPlugin"
#define PLUGIN_MAJOR_NATRON_CHROMAKEYER 2

#define kChromaKeyerParamKeyColor "keyColor"
#define kChromaKeyerParamColorspace "colorspace"
#define kChromaKeyerParamLinear "linearProcessing"
#define kChromaKeyerParamAcceptanceAngle "acceptanceAngle"
#define kChromaKeyerParamSuppressionAngle "suppressionAngle"
#define kChromaKeyerParamKeyLift "keyLift"
#define kChromaKeyerParamKeyGain "keyGain"
#define kChromaKeyerParamShow "show"
#define kChromaKeyerParamSourceAlpha "sourceAlphaHandling"

// Input indices, in the OpenFX clip order that scripts connect by.
#define kChromaKeyerInputSource 0
#define kChromaKeyerInputInsideMask 1
#define kChromaKeyerInputOutsideMask 2
#define kChromaKeyerInputBg 3

NATRON_NAMESPACE_ENTER

/**
 * @brief Chroma keyer after Keith Jack's "Video Demystified": the key colour and the foreground
 * are taken to Y'PbPr, the chrominance is rotated so the key colour lies on one axis, and the
 * distance along that axis inside the acceptance angle gives the background key. The inside and
 * outside masks, the key lift and gain, and the foreground suppression (within the suppression
 * angle) follow before the result is output as the source, the premultiplied or unpremultiplied
 * foreground, or the composite over Bg. The output is always RGBA, with the foreground key as
 * alpha. Knob names, defaults, maths and region of definition are the openfx-misc
 * ChromaKeyerPlugin's; it registers under that plug-in's ID one major above.
 *
 * Divergence: an RGB source has no alpha, which the OpenFX plug-in reads out of bounds. Here
 * "Normal" source alpha composites with an alpha of one there, and "Add to Inside Mask" adds
 * nothing.
 **/
class ChromaKeyer
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ChromaKeyer(node);
    }

    explicit ChromaKeyer(NodePtr node);

    virtual ~ChromaKeyer();

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Whether render() reads input inputNb as a mask: it is connected and its selector is on.
    bool isKeyMaskUsed(int inputNb) const WARN_UNUSED_RETURN;

    KnobColorWPtr _keyColor;
    KnobChoiceWPtr _colorspace;
    KnobBoolWPtr _linear;
    KnobDoubleWPtr _acceptanceAngle;
    KnobDoubleWPtr _suppressionAngle;
    KnobDoubleWPtr _keyLift;
    KnobDoubleWPtr _keyGain;
    KnobChoiceWPtr _show;
    KnobChoiceWPtr _sourceAlpha;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_ChromaKeyer_h
