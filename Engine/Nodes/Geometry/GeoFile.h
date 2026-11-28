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

#ifndef Engine_Nodes_Geometry_GeoFile_h
#define Engine_Nodes_Geometry_GeoFile_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/IO/ReadTimeDomainKnobs.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOFILE "GeoFile"
#define kGeoFileParamFile "file"
#define kGeoFileParamReload "reload"

NATRON_NAMESPACE_ENTER

enum GeoFileMissingFrameEnum {
    eGeoFileMissingFrameError = 0,
    eGeoFileMissingFrameHoldNearest,
    eGeoFileMissingFrameEmpty,
};

/**
 * @brief Reads native geometry files (.ngeo), one file or one file per frame. A frame number
 * in the name is written as #### (one # per digit), %04d, $F or $F4 (padded to 4 digits).
 * Which file a frame reads follows the time domain knobs Read has, with the same names and
 * behaviour: the first and last frame, what comes before and after them, the frame offset, and
 * what a missing frame gives, black being empty geometry. A file that exists but cannot be read
 * is always an error. An empty file name gives empty geometry.
 *
 * The files are stamped (FileStampRegistry) into the node hash, so geometry cached from them is
 * served until Reload or a GeoFileCache writing them re-stamps them.
 **/
class GeoFile
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOFILE,
        .label = "GeoFile",
        .description = "Read geometry from native geometry files (.ngeo), as written by GeoFileCache: "
                       "a single file, or a sequence with the frame number in the name as ####, "
                       "%04d or $F4. Every attribute, group and packed primitive is read back exactly.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoFile(node);
    }

    explicit GeoFile(NodePtr node);
    virtual ~GeoFile();

    /** @brief True when @p pattern holds a frame number token (#, %0Nd or $F). */
    static bool isFrameSequencePattern(const std::string& pattern);

    /** @brief @p pattern with every $F or $FN token rewritten as %d or %0Nd. */
    static std::string normalizeFrameTokens(const std::string& pattern);

    /**
     * @brief @p value, a file knob's value, with project variables expanded (a relative path made
     * relative to the project) and $F tokens normalized: the pattern the functions below take.
     **/
    static std::string expandPattern(EffectInstance* effect,
                                     const std::string& value);

    /** @brief The file @p pattern names at @p frame and @p view. */
    static std::string resolvePath(EffectInstance* effect,
                                   const std::string& pattern,
                                   int frame,
                                   ViewIdx view);

    /**
     * @brief The file of the sequence @p pattern names, among those its FileStampRegistry stamp
     * recorded as existing, whose frame is closest to @p frame, the earlier one on a tie; empty
     * when the name has no frame token or no frame was recorded.
     **/
    static std::string nearestExistingFrame(const std::string& pattern,
                                            int frame,
                                            ViewIdx view);

    /**
     * @brief The FileStampRegistry stamp of the file or sequence @p pattern, an expandPattern()
     * result, names; 0 for an empty pattern.
     **/
    static U64 patternStamp(const std::string& pattern);

    /**
     * @brief For refreshFileDependencies(): true when one of @p paths, just written or reloaded,
     * is a file of @p pattern, whose stamps are then dropped.
     **/
    static bool invalidatePatternIfCovered(const std::string& pattern,
                                           const std::vector<std::string>& paths);

    /**
     * @brief Reads the frame of @p pattern through the missing-frame rule, pinned to what
     * patternStamp() recorded: which files exist and their identities. A file added, removed or
     * rewritten since, before or while it is decoded, fails with
     * FileStampRegistry::changedOnDiskMessage() until the stamp is invalidated. False with
     * @p error set on failure; *detail is set on success.
     **/
    static bool readFrame(EffectInstance* effect,
                          const std::string& pattern,
                          GeoFileMissingFrameEnum missing,
                          int frame,
                          ViewIdx view,
                          GeoDetailConstPtr* detail,
                          std::string* error);

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;
    virtual bool refreshFileDependencies(const std::vector<std::string>& paths) OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getFrameRange(double* first, double* last) OVERRIDE FINAL;
    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onFilesReloaded() OVERRIDE FINAL;

    std::string currentPattern() const;

    /** @brief Records the frames of the files on disk in the hidden original frame range. */
    void refreshOriginalFrameRange();

    KnobFileWPtr _file;
    KnobButtonWPtr _reload;
    KnobIntWPtr _originalFrameRange;
    ReadTimeDomainKnobs _timeDomain;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoFile_h
