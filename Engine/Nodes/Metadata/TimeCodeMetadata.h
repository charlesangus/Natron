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

#ifndef Engine_Nodes_Metadata_TimeCodeMetadata_h
#define Engine_Nodes_Metadata_TimeCodeMetadata_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_TIMECODEMETADATA "fr.natron.TimeCodeMetadata"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kTimeCodeMetadataInputs[] = {
    InputSpec { .label = "Source", .optional = false },
};

class TimeCodeMetadata
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TimeCodeMetadata(node);
    }

    explicit TimeCodeMetadata(NodePtr node);

    /**
     * @brief The whole number of frames a second holds at rate, rounded to nearest and at least 1.
     **/
    static int roundedRate(double rate) WARN_UNUSED_RETURN;

    /**
     * @brief The four fields of HH:MM:SS:FF, any run of non-digits separating them; all zero
     * when text holds fewer than four numbers. Numbers past the fourth are ignored.
     **/
    static std::array<int, 4> parseTimecode(const std::string& text) WARN_UNUSED_RETURN;

    /**
     * @brief The frame count of the timecode text at rate frames a second.
     **/
    static long long timecodeToFrames(const std::string& text, int rate) WARN_UNUSED_RETURN;

    /**
     * @brief The non-drop-frame HH:MM:SS:FF of a frame count at rate, wrapped into one day,
     * a negative count wrapping back from 24:00:00:00.
     **/
    static std::string framesToTimecode(long long frames, int rate) WARN_UNUSED_RETURN;

private:
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_TIMECODEMETADATA,
        .label = "TimeCodeMetadata",
        .description = "Sets ofx/timecode to a non-drop-frame timecode counted on from Start Timecode, "
                       "which lands on frame 1 (or on Start Frame when Use Start Frame is on), one frame "
                       "per frame, wrapping back to 00:00:00:00 after 23:59:59. The frames field counts "
                       "at Rate rounded to a whole number, or at the ofx/framerate of Source when Rate "
                       "From Metadata is on and Source has one; that rate is also published as "
                       "ofx/framerate. The image is passed through unchanged.",
        .grouping = PLUGIN_GROUP_METADATA,
        .majorVersion = 1,
        .minorVersion = 0,
        .inputs = kTimeCodeMetadataInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobStringWPtr _startTimecode;
    KnobDoubleWPtr _rate;
    KnobBoolWPtr _rateFromMetadata;
    KnobIntWPtr _startFrame;
    KnobBoolWPtr _useStartFrame;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_TimeCodeMetadata_h
