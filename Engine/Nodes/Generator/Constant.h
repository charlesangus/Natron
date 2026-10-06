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

#ifndef Engine_Nodes_Generator_Constant_h
#define Engine_Nodes_Generator_Constant_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeGenerator.h"

#define PLUGINID_NATRON_CONSTANT "net.sf.openfx.ConstantPlugin"
#define PLUGINID_NATRON_SOLID "net.sf.openfx.Solid"
#define PLUGIN_MAJOR_NATRON_CONSTANT 2

#define kConstantParamColor "color"

NATRON_NAMESPACE_ENTER

/**
 * @brief Constant: fills the selected channels of its target layer with `color` (RGBA), unclamped.
 * Knob names, defaults and output are the openfx-misc ConstantPlugin's; it registers under that
 * plugin's ID one major above.
 **/
class Constant
    : public NativeGenerator {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Constant(node, false);
    }

    virtual ~Constant();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

protected:
    Constant(NodePtr node,
             bool solid);

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual PixelKernelPtr makeKernel(const KernelContext& context) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool _solid;
    KnobColorWPtr _color;
};

/**
 * @brief Solid: Constant with an RGB `color` and an alpha of 1, under the openfx-misc Solid ID.
 **/
class Solid
    : public Constant {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Solid(node);
    }

    explicit Solid(NodePtr node);

    virtual ~Solid();

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_Constant_h
