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

#ifndef Engine_Nodes_Color_ColorMathNode_h
#define Engine_Nodes_Color_ColorMathNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"

#define PLUGINID_NATRON_ADD "net.sf.openfx.AddPlugin"
#define PLUGINID_NATRON_MULTIPLY "net.sf.openfx.MultiplyPlugin"
#define PLUGINID_NATRON_GAMMA "net.sf.openfx.GammaPlugin"
#define PLUGIN_MAJOR_NATRON_COLORMATH 3

#define kColorMathParamValue "value"
#define kColorMathParamInvert "invert"

NATRON_NAMESPACE_ENTER

enum ColorMathOperationEnum {
    eColorMathOperationAdd,
    eColorMathOperationMultiply,
    eColorMathOperationGamma
};

/**
 * @brief Add, Multiply and Gamma: one constant per channel, applied to the processed channels.
 *   Add:      out = in + value
 *   Multiply: out = in * value
 *   Gamma:    out = pow(in, e) for in > 0 and out = in otherwise, with e = 1 / max(1e-8, value),
 *             or e = value when invert is set.
 * The arithmetic is single-precision, as in the openfx-misc plug-ins of the same IDs, and the
 * node is registered under each of them one major above. Add accepts RGBA, RGB and Alpha;
 * Multiply and Gamma also accept XY.
 **/
class ColorMathNode
    : public NativeImageEffect {
public:
    ColorMathNode(NodePtr node,
                  ColorMathOperationEnum operation);

    virtual ~ColorMathNode();

    virtual bool isPointOp() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual PixelKernelPtr makeKernel(const KernelContext& context) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isIdentityOp(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    const ColorMathOperationEnum _operation;
    KnobColorWPtr _value;
    KnobBoolWPtr _invert;
};

/**
 * One factory per ID: the built-in plug-in registry builds an effect through a static function
 * that takes no operation argument.
 **/
struct ColorMathAdd {
    static EffectInstance* BuildEffect(NodePtr node);
};

struct ColorMathMultiply {
    static EffectInstance* BuildEffect(NodePtr node);
};

struct ColorMathGamma {
    static EffectInstance* BuildEffect(NodePtr node);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorMathNode_h
