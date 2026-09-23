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

#ifndef Engine_Nodes_Deep_DeepMerge_h
#define Engine_Nodes_Deep_DeepMerge_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_DEEPMERGE "fr.natron.DeepMerge"

NATRON_NAMESPACE_ENTER

/**
 * @brief Merges two deep inputs, A and B, one of two ways.
 *
 * Combine concatenates both inputs' samples into one pixel, sorted front-to-back but not
 * tidied: overlapping samples are left overlapping for whatever consumes them to tidy on demand,
 * so nothing is split or merged before it has to be. The output carries the union of the two
 * inputs' channels, a channel one input lacks reading as zero on that input's samples.
 *
 * Holdout keeps A's samples only, each attenuated by the transparency B accumulates in front of
 * it: an A sample behind a fully covering B sample goes to zero, one behind a half-transparent B sample
 * is halved, alpha included, since every channel is premultiplied. Where a B sample overlaps an
 * A sample the overlap is worked out with the OpenEXR "Interpreting Deep Pixels" split and merge
 * rules -- A's sample is cut at B's boundaries, the part of B in front of a piece attenuates it
 * in full and the part coincident with it by half -- which is what makes the result agree with
 * combining A and B and dropping B's colour. B's own colour never reaches the output.
 **/
class DeepMerge
    : public NativeEffectBase {
public:
    enum OperationEnum {
        eOperationCombine = 0,
        eOperationHoldout
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepMerge(node);
    }

    explicit DeepMerge(NodePtr node)
        : NativeEffectBase(node)
        , _operation()
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

    OperationEnum getOperation() const WARN_UNUSED_RETURN;

    StatusEnum renderCombine(const DeepRenderActionArgs& args,
                             const DeepImagePtr& a,
                             const DeepImagePtr& b) WARN_UNUSED_RETURN;

    StatusEnum renderHoldout(const DeepRenderActionArgs& args,
                             const DeepImagePtr& a,
                             const DeepImagePtr& b) WARN_UNUSED_RETURN;

    KnobChoiceWPtr _operation;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepMerge_h
