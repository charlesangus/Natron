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

#ifndef Engine_Nodes_Deep_DeepReformat_h
#define Engine_Nodes_Deep_DeepReformat_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_DEEPREFORMAT "fr.natron.DeepReformat"

NATRON_NAMESPACE_ENTER

/**
 * @brief Repositions the deep input within a new output format, without filtering it: the input
 * is translated by a whole number of pixels and every sample is copied across untouched -- its
 * depths, its channels, and the order and count of the samples of a pixel all preserved -- so
 * the output is as tidy as the input and a round trip through this node is exact. This is Nuke's
 * Reformat with its resize type set to none.
 *
 * The new format is the one Format names, or Custom Size's width and height when Use Custom Size
 * is on. With Centre on, the input's format is placed in the middle of the new one, the offset
 * truncated to whole pixels; with it off the input stays at the origin, which makes the node a
 * metadata change alone and so an identity of its input.
 *
 * Centring is measured from the input's format rather than from its region of definition on
 * purpose: a deep stream's sample extent moves from frame to frame, and centring on that would
 * slide the image around under the format as it went.
 *
 * Two things this node deliberately leaves to others. Samples the shift pushes outside the new
 * format are kept rather than cropped -- a region of definition wider than the format is
 * ordinary here -- so that repositioning and cropping stay separable; DeepCrop downstream is how
 * a shot crops to its format. And the output's pixel aspect ratio stays the input's, since
 * changing it is a resample, which is the one thing this node exists not to do.
 **/
class DeepReformat
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepReformat(node);
    }

    explicit DeepReformat(NodePtr node)
        : NativeEffectBase(node)
        , _formatSize()
        , _useCustomSize()
        , _customSize()
        , _centre()
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

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool getTargetSize(int* width, int* height) const WARN_UNUSED_RETURN;

    void getTranslation(int* dx, int* dy) const;

    KnobIntWPtr _formatSize;
    KnobBoolWPtr _useCustomSize;
    KnobIntWPtr _customSize;
    KnobBoolWPtr _centre;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepReformat_h
