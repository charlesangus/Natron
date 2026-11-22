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

#ifndef Engine_Nodes_Geometry_GeoFileCache_h
#define Engine_Nodes_Geometry_GeoFileCache_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOFILECACHE "GeoFileCache"
#define kGeoFileCacheParamMode "mode"
// The same name as Write's file knob, which `NatronRenderer -w <node> <file>` overrides.
#define kGeoFileCacheParamFile "filename"
#define kGeoFileCacheParamVersion "version"
#define kGeoFileCacheParamMissingFrames "missingFrames"
#define kGeoFileCacheParamFrameRange "frameRange"
#define kGeoFileCacheParamFirstFrame "firstFrame"
#define kGeoFileCacheParamLastFrame "lastFrame"

NATRON_NAMESPACE_ENTER

/**
 * @brief Caches its input geometry in native geometry files (.ngeo), one per frame.
 *
 * In Save To Disk mode the node passes its input through, and rendering it (Render, the render
 * queue or `NatronRenderer -w`) writes each frame of its frame range to the file its Filename
 * names at that frame, frames in parallel unless a node upstream needs them in order. Each file
 * is written beside its final name and renamed over it, so a cancelled render leaves every frame
 * written before it whole. In Load From Disk mode it outputs the files, read as GeoFile reads
 * them, without evaluating its input; in Pass Through mode it outputs its input and writes
 * nothing.
 *
 * The Filename takes GeoFile's frame tokens and project variables, and $VER or $VERN for the
 * Version, padded to N digits.
 **/
class GeoFileCache
    : public NativeEffectBase {
public:
    enum ModeEnum {
        eModeSave = 0,
        eModeLoad,
        eModePassThrough,
    };

    enum FrameRangeEnum {
        eFrameRangeProject = 0,
        eFrameRangeManual,
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOFILECACHE,
        .label = "GeoFileCache",
        .description = "Cache geometry in native geometry files (.ngeo), one per frame. Save To Disk passes the "
                       "input through and writes the frame range when the node is rendered; Load From Disk reads "
                       "the files back exactly instead of evaluating the input; Pass Through does neither.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = true,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoFileCache(node);
    }

    explicit GeoFileCache(NodePtr node);
    virtual ~GeoFileCache();

    /** @brief @p value with every $VER or $VERN token replaced by @p version, padded to N digits. */
    static std::string substituteVersion(const std::string& value,
                                         int version);

    /** @brief The file pattern the node's knobs name, as GeoFile::expandPattern() returns it. */
    std::string currentPattern();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL;
    virtual bool dependsOnProjectFrameRange() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum writeGeometryFrame(double time,
                                          ViewIdx view,
                                          const GeoDetailConstPtr& detail) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool rendersFramesInOrder() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    ModeEnum mode() const;
    void refreshKnobVisibility();

    KnobChoiceWPtr _mode;
    KnobOutputFileWPtr _file;
    KnobIntWPtr _version;
    KnobChoiceWPtr _missingFrames;
    KnobChoiceWPtr _frameRange;
    KnobIntWPtr _firstFrame;
    KnobIntWPtr _lastFrame;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoFileCache_h
