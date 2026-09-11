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

#ifndef Engine_Nodes_Deep_DeepRecolor_h
#define Engine_Nodes_Deep_DeepRecolor_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_DEEPRECOLOR "fr.natron.DeepRecolor"

NATRON_NAMESPACE_ENTER

/**
 * @brief Gives every sample of the deep input A the colour of the Color image at its pixel,
 * scaled to the sample's own alpha -- rgb = color.rgb / color.a * sample.a -- so that a pixel's
 * samples flatten back to the Color image's colour while keeping their depths and alphas. A
 * sample under a pixel the Color image does not cover, or where its alpha is zero, goes black.
 *
 * With Target Input Alpha on, each pixel's alphas are rescaled as well, by one exponent shared
 * by all of its samples, so that the pixel's flattened alpha equals the Color image's -- exactly
 * so when the samples do not overlap -- and the colour is then scaled by those new alphas.
 *
 * Only the channels rewritten are given storage of their own: Z, ZBack, the sample table and,
 * unless Target Input Alpha is on, A stay shared with the input.
 **/
class DeepRecolor
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepRecolor(node);
    }

    explicit DeepRecolor(NodePtr node)
        : NativeEffectBase(node)
        , _targetInputAlpha()
    {
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    ImagePtr renderColorImage(const DeepRenderActionArgs& args,
                              bool* failed) WARN_UNUSED_RETURN;

    KnobBoolWPtr _targetInputAlpha;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepRecolor_h
