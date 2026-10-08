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

#ifndef Engine_Nodes_Keyer_Keyer_h
#define Engine_Nodes_Keyer_Keyer_h

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

#define PLUGINID_NATRON_KEYER "net.sf.openfx.KeyerPlugin"
#define PLUGIN_MAJOR_NATRON_KEYER 2

#define kKeyerParamKeyColor "keyColor"
#define kKeyerParamMode "mode"
#define kKeyerParamLuminanceMath "luminanceMath"
#define kKeyerParamSoftnessLower "softnessLower"
#define kKeyerParamToleranceLower "toleranceLower"
#define kKeyerParamCenter "center"
#define kKeyerParamToleranceUpper "toleranceUpper"
#define kKeyerParamSoftnessUpper "softnessUpper"
#define kKeyerParamDespill "despill"
#define kKeyerParamDespillAngle "despillAngle"
#define kKeyerParamShow "show"
#define kKeyerParamSourceAlpha "sourceAlphaHandling"

// Input indices, in the OpenFX clip order that scripts connect by.
#define kKeyerInputSource 0
#define kKeyerInputInsideMask 1
#define kKeyerInputOutsideMask 2
#define kKeyerInputBg 3

NATRON_NAMESPACE_ENTER

/**
 * @brief Luminance, colour and screen keyer, and a despiller. A foreground key is computed from
 * the source RGB by the keyer mode (the luminance, the projection on the key colour, or that
 * projection minus the distance to the key colour's axis), the piecewise-linear function of
 * the softness, tolerance and centre knobs turns it into a background key, the inside and
 * outside masks are mixed into that key, and, in Screen and None modes, the colour is despilled
 * within a cone around the key colour. The result is output as the source, the premultiplied or
 * unpremultiplied foreground, or the composite over Bg; the output is always RGBA with the
 * foreground key as alpha. Knob names, defaults, maths and region of definition are the
 * openfx-misc KeyerPlugin's; it registers under that plug-in's ID one major above. Editing the
 * key colour or the mode resets the thresholds to span the key colour, as the plug-in does.
 *
 * Divergence: an RGB source has no alpha, which the OpenFX plug-in reads out of bounds. Here
 * "Normal" source alpha composites with an alpha of one there, and "Add to Inside Mask" adds
 * nothing.
 **/
class Keyer
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Keyer(node);
    }

    explicit Keyer(NodePtr node);

    virtual ~Keyer();

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

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Whether render() reads input inputNb as a mask: it is connected and its selector is on.
    bool isKeyMaskUsed(int inputNb) const WARN_UNUSED_RETURN;

    // Shows the knobs the current mode uses, and writes the mode into the sublabel.
    void refreshModeDependents(double time, ViewIdx view);

    // Sets the five threshold knobs so that they span the key colour for the mode.
    void setThresholdsFromKeyColor(double time, ViewIdx view);

    KnobStringWPtr _subLabel;
    KnobColorWPtr _keyColor;
    KnobChoiceWPtr _mode;
    KnobChoiceWPtr _luminanceMath;
    KnobDoubleWPtr _softnessLower;
    KnobDoubleWPtr _toleranceLower;
    KnobDoubleWPtr _center;
    KnobDoubleWPtr _toleranceUpper;
    KnobDoubleWPtr _softnessUpper;
    KnobDoubleWPtr _despill;
    KnobDoubleWPtr _despillAngle;
    KnobChoiceWPtr _show;
    KnobChoiceWPtr _sourceAlpha;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_Keyer_h
