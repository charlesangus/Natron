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

#ifndef Engine_Nodes_Color_Grade_h
#define Engine_Nodes_Color_Grade_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"

#define PLUGINID_NATRON_GRADE "net.sf.openfx.GradePlugin"
#define PLUGIN_MAJOR_NATRON_GRADE 3

#define kGradeParamBlackPoint "blackPoint"
#define kGradeParamWhitePoint "whitePoint"
#define kGradeParamBlack "black"
#define kGradeParamWhite "white"
#define kGradeParamMultiply "multiply"
#define kGradeParamOffset "offset"
#define kGradeParamGamma "gamma"
#define kGradeParamNormalize "normalize"
#define kGradeParamReverse "reverse"
#define kGradeParamClampBlack "clampBlack"
#define kGradeParamClampWhite "clampWhite"

NATRON_NAMESPACE_ENTER

/**
 * @brief Grade: maps blackPoint/whitePoint to black (Lift)/white (Gain), then applies multiply,
 * offset and gamma, per channel:
 *   A = multiply * (white - black) / (whitePoint - blackPoint), or 0 when the points coincide
 *   B = offset + black - A * blackPoint
 *   output = pow(A * input + B, 1 / gamma)
 * with the OpenFX Grade's rules for gamma <= 0, gamma == 1 and values <= 0, reverse applying the
 * inverse, and clampBlack/clampWhite clamping the result to [0, 1]. Knob names, defaults and
 * maths are the openfx-misc GradePlugin's; it registers under that plugin's ID one major above.
 **/
class Grade
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Grade(node);
    }

    explicit Grade(NodePtr node);

    virtual ~Grade();

    virtual bool isPointOp() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
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

    /**
     * @brief Sets blackPoint and whitePoint to the per-channel minimum and maximum of the source
     * at (time, view), as one undoable edit.
     **/
    void normalize(double time, ViewIdx view);

    KnobColorWPtr _blackPoint;
    KnobColorWPtr _whitePoint;
    KnobColorWPtr _black;
    KnobColorWPtr _white;
    KnobColorWPtr _multiply;
    KnobColorWPtr _offset;
    KnobColorWPtr _gamma;
    KnobButtonWPtr _normalize;
    KnobBoolWPtr _reverse;
    KnobBoolWPtr _clampBlack;
    KnobBoolWPtr _clampWhite;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Grade_h
