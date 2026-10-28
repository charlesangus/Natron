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

#ifndef Engine_Nodes_Metadata_CompareMetadata_h
#define Engine_Nodes_Metadata_CompareMetadata_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_COMPAREMETADATA "fr.natron.CompareMetadata"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kCompareMetadataInputs[] = {
    InputSpec { .label = "A", .optional = false },
    InputSpec { .label = "B", .optional = true },
};

class CompareMetadata
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new CompareMetadata(node);
    }

    explicit CompareMetadata(NodePtr node);

    /**
     * @brief One line per key A and B disagree on, in ascending key order: "A only: key=value",
     * "B only: key=value" or "differs: key: A=value B=value". Keys both hold with the same
     * value print nothing, so identical metadata gives an empty text.
     **/
    static std::string differences(const ImageMetadata& a, const ImageMetadata& b) WARN_UNUSED_RETURN;

    /**
     * @brief Rebuilds the display from the metadata of A and B at time. Main thread only.
     **/
    void refreshDifferencesDisplay(double time);

    /**
     * @brief The text the settings panel currently shows.
     **/
    std::string getDifferencesDisplayText() const WARN_UNUSED_RETURN;

private:
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COMPAREMETADATA,
        .label = "CompareMetadata",
        .description = "Shows, in its Differences box, the metadata keys on which A and B disagree at the "
                       "current frame: a key only one of them holds, or one both hold with different "
                       "values. Keys with equal values are not listed. The list follows the timeline and "
                       "upstream changes; Refresh updates it on demand. The image and metadata of A are "
                       "passed through unchanged.",
        .grouping = PLUGIN_GROUP_METADATA,
        .majorVersion = 1,
        .minorVersion = 0,
        .inputs = kCompareMetadataInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The RoD of A, whose image is the output; B only contributes metadata to compare.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void refreshExtraStateAfterTimeChanged(bool isPlayback, double time) OVERRIDE FINAL;
    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    virtual void onCurrentFrameMetadataChanged(const NodeMetadata& metadata) OVERRIDE FINAL;

    void scheduleRefresh();

    KnobStringWPtr _display;
    KnobButtonWPtr _refreshButton;
    std::atomic<bool> _refreshPending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_CompareMetadata_h
