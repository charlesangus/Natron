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

#ifndef Engine_Nodes_Metadata_ViewMetadata_h
#define Engine_Nodes_Metadata_ViewMetadata_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_VIEWMETADATA "ViewMetadata"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kViewMetadataInputs[] = {
    InputSpec { .label = "Source", .optional = false },
};

class ViewMetadata
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ViewMetadata(node);
    }

    explicit ViewMetadata(NodePtr node);

    /**
     * @brief Rebuilds the table from this node's metadata at time. Main thread only.
     **/
    void refreshMetadataDisplay(double time);

    /**
     * @brief The table the settings panel currently shows.
     **/
    std::string getMetadataDisplayText() const WARN_UNUSED_RETURN;

private:
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_VIEWMETADATA,
        .label = "ViewMetadata",
        .description = "Shows the metadata of its input at the current frame in its Metadata tab. The "
                       "list follows the timeline and upstream changes; Refresh updates it on demand. "
                       "The image and its metadata are passed through unchanged.",
        .grouping = PLUGIN_GROUP_METADATA,
        .majorVersion = 1,
        .minorVersion = 0,
        .inputs = kViewMetadataInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

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

#endif // Engine_Nodes_Metadata_ViewMetadata_h
