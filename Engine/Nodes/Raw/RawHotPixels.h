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

/*
 * The maths are a port of darktable's hotpixels module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/hotpixels.c, Copyright (C) 2011-2026 darktable developers,
 * distributed under the GNU GPL version 3 or later.
 */

#ifndef Engine_Nodes_Raw_RawHotPixels_h
#define Engine_Nodes_Raw_RawHotPixels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWHOTPIXELS "fr.natron.RawHotPixels"
#define PLUGIN_MAJOR_NATRON_RAWHOTPIXELS 1

#define kRawHotPixelsParamThreshold "threshold"
#define kRawHotPixelsParamStrength "strength"
#define kRawHotPixelsParamPermissive "permissive"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kRawHotPixelsInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

/**
 * @brief darktable's hot pixels module on prepared raw data. A photosite above the threshold is
 * hot when strength / 2 times its value exceeds each of its four nearest neighbours of the same
 * colour (three with Detect By 3 Neighbours), and it then becomes the largest of those
 * neighbours. The neighbours are two photosites away on a Bayer mosaic, the four nearest of the
 * same colour on an X-Trans one, and the adjacent pixels of the same channel on data without a
 * CFA. Photosites closer to the image's edge than the neighbours reach (2, or 1 without a CFA)
 * are left as they are, as darktable does. A strength of 0 is an identity.
 **/
struct RawHotPixelsOp {
    /**
     * @brief The four neighbours tested at each X-Trans cell, as (dx, dy) in Natron pixels, and
     * how many of them the pattern has.
     **/
    struct XTransNeighbours {
        std::array<std::array<std::array<int, 2>, 4>, 36> offsets = {};
        std::array<int, 36> count = {};
    };

    struct Params {
        RawLayout layout;
        float threshold = 0.f;
        float multiplier = 0.f;
        int minNeighbours = 4;
        XTransNeighbours xtrans;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RAWHOTPIXELS,
        .label = "RawHotPixels",
        .description = "Removes hot pixels from prepared raw data: a photosite much brighter than its nearest neighbours of the "
                       "same colour becomes the brightest of them. Goes after RawPrepare.",
        .grouping = PLUGIN_GROUP_RAW,
        .majorVersion = PLUGIN_MAJOR_NATRON_RAWHOTPIXELS,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWHOTPIXELS ".png",
        .outputKind = eDataKindRaw,
        .inputs = kRawHotPixelsInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .layers = LayerPolicy::none(),
        .op = OpKind::Passes,
        .rawStage = RawStageRequirement::Prepared,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader,
                const ImageMetadata& source) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    PassList passes(const Params& params,
                    const PassContext& context) const;

    KnobDoubleWPtr threshold;
    KnobDoubleWPtr strength;
    KnobBoolWPtr permissive;
};

static_assert(PassesOp<RawHotPixelsOp>);

class RawHotPixels
    : public ImageNode<RawHotPixelsOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawHotPixels(node);
    }

    explicit RawHotPixels(NodePtr node);

    virtual ~RawHotPixels();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawHotPixels_h
