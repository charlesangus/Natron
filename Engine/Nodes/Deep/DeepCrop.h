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

#ifndef Engine_Nodes_Deep_DeepCrop_h
#define Engine_Nodes_Deep_DeepCrop_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"

#define PLUGINID_NATRON_DEEPCROP "DeepCrop"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kDeepCropInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindDeep },
};

/**
 * @brief Crops the deep input to Bbox in X and Y and to Z Range in depth, either of which can be
 * switched off on its own. A sample kept by the Z crop is one whose Z is at or past Near and
 * whose ZBack (or Z, on a point sample) is at or before Far; nothing is split at the boundary.
 * Every channel of a kept sample is copied over unchanged, and the samples of a pixel keep their
 * relative order, so the output is as tidy as the input.
 *
 * With Reformat on, the output's format becomes Bbox's pixel rectangle rather than the input's.
 *
 * A crop that would drop nothing -- Bbox contains the input's region of definition, or Use Bbox
 * is off, and Use Z Range is off -- is an identity of the input: the deep pipeline resolves it to
 * the input's own cache entry rather than copying.
 **/
class DeepCrop
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepCrop(node);
    }

    explicit DeepCrop(NodePtr node)
        : NativeEffectBase(node)
        , _bbox()
        , _useBBox()
        , _zRange()
        , _useZRange()
        , _reformat()
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
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_DEEPCROP,
        .label = "DeepCrop",
        .description = "Crop the deep input to a rectangle in X and Y and/or to a range in Z. "
                       "A sample is kept only if its Z is at or past Near and its ZBack (or Z, "
                       "on a point sample) is at or before Far. Reformat sets the output format "
                       "to the crop rectangle instead of keeping the input's.",
        .grouping = PLUGIN_GROUP_DEEP,
        .majorVersion = 1,
        .minorVersion = 0,
        .outputKind = eDataKindDeep,
        .inputs = kDeepCropInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    RectD getBBoxCanonical(double time) const WARN_UNUSED_RETURN;

    KnobDoubleWPtr _bbox;
    KnobBoolWPtr _useBBox;
    KnobDoubleWPtr _zRange;
    KnobBoolWPtr _useZRange;
    KnobBoolWPtr _reformat;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepCrop_h
