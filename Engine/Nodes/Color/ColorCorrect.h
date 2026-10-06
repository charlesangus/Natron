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

#ifndef Engine_Nodes_Color_ColorCorrect_h
#define Engine_Nodes_Color_ColorCorrect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"

#define PLUGINID_NATRON_COLORCORRECT "net.sf.openfx.ColorCorrectPlugin"
#define PLUGIN_MAJOR_NATRON_COLORCORRECT 3

#define kColorCorrectGroupMaster "Master"
#define kColorCorrectGroupShadows "Shadows"
#define kColorCorrectGroupMidtones "Midtones"
#define kColorCorrectGroupHighlights "Highlights"
#define kColorCorrectParamEnable "Enable"
#define kColorCorrectParamSaturation "Saturation"
#define kColorCorrectParamContrast "Contrast"
#define kColorCorrectParamGamma "Gamma"
#define kColorCorrectParamGain "Gain"
#define kColorCorrectParamOffset "Offset"
#define kColorCorrectParamRange "range"
#define kColorCorrectParamToneRanges "toneRanges"
#define kColorCorrectParamClampBlack "clampBlack"
#define kColorCorrectParamClampWhite "clampWhite"

NATRON_NAMESPACE_ENTER

/**
 * @brief ColorCorrect: saturation, contrast, gamma, gain and offset, once for the shadows,
 * midtones and highlights blended by the tone-range curves of the luminance, then once for the
 * whole image (Master). Each group applies, per channel:
 *   saturation: c = (1 - s) * luminance + s * c, on R, G and B only
 *   contrast:   c = pow(c / 0.18, contrast) * 0.18, for c > 0
 *   gamma:      c = pow(c, 1 / gamma), for c > 0
 *   gain, then offset
 * and clampBlack/clampWhite clamp the result. Knob names, defaults and maths are the
 * openfx-misc ColorCorrectPlugin's; it registers under that plugin's ID one major above.
 **/
class ColorCorrect
    : public NativeImageEffect {
public:
    enum GroupEnum {
        eGroupMaster = 0,
        eGroupShadows,
        eGroupMidtones,
        eGroupHighlights,
        eGroupCount
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ColorCorrect(node);
    }

    explicit ColorCorrect(NodePtr node);

    virtual ~ColorCorrect();

    virtual bool isPointOp() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    struct GroupKnobs {
        KnobBoolWPtr enable;
        KnobColorWPtr saturation;
        KnobColorWPtr contrast;
        KnobColorWPtr gamma;
        KnobColorWPtr gain;
        KnobColorWPtr offset;
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual PixelKernelPtr makeKernel(const KernelContext& context) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isIdentityOp(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void addGroup(const KnobPagePtr& page, GroupEnum group, const std::string& name, bool open);

    GroupKnobs _groups[eGroupCount];
    KnobDoubleWPtr _range;
    KnobParametricWPtr _toneRanges;
    KnobChoiceWPtr _luminanceMath;
    KnobBoolWPtr _clampBlack;
    KnobBoolWPtr _clampWhite;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorCorrect_h
