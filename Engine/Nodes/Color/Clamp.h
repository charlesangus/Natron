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

#ifndef Engine_Nodes_Color_Clamp_h
#define Engine_Nodes_Color_Clamp_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"

#define PLUGINID_NATRON_CLAMP "net.sf.openfx.Clamp"
#define PLUGIN_MAJOR_NATRON_CLAMP 3

#define kClampParamMinimum "minimum"
#define kClampParamMinimumEnable "minimumEnable"
#define kClampParamMaximum "maximum"
#define kClampParamMaximumEnable "maximumEnable"
#define kClampParamMinClampTo "minClampTo"
#define kClampParamMinClampToEnable "minClampToEnable"
#define kClampParamMaxClampTo "maxClampTo"
#define kClampParamMaxClampToEnable "maxClampToEnable"

NATRON_NAMESPACE_ENTER

/**
 * @brief Clamp: per processed channel, a value below `minimum` becomes minClampTo when
 * minClampToEnable is on and `minimum` otherwise, and a value above `maximum` becomes maxClampTo
 * or `maximum` likewise, each side only when its enable is on. The minimum side is tested first.
 * Knob names, defaults and maths are the openfx-misc Clamp plugin's; it registers under that
 * plugin's ID one major above.
 **/
class Clamp
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Clamp(node);
    }

    explicit Clamp(NodePtr node);

    virtual ~Clamp();

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

    KnobColorWPtr _minimum;
    KnobBoolWPtr _minimumEnable;
    KnobColorWPtr _maximum;
    KnobBoolWPtr _maximumEnable;
    KnobColorWPtr _minClampTo;
    KnobBoolWPtr _minClampToEnable;
    KnobColorWPtr _maxClampTo;
    KnobBoolWPtr _maxClampToEnable;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Clamp_h
