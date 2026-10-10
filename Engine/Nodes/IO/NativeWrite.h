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

#ifndef Engine_Nodes_IO_NativeWrite_h
#define Engine_Nodes_IO_NativeWrite_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <optional>
#include <string>

#include "Engine/EffectInstance.h" // for PLUGINID_NATRON_WRITE
#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/OcioSpaceKnobs.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define kNatronWriteParamFrameStep "frameIncr"
#define kNatronWriteParamFrameStepLabel "Frame Increment"
#define kNatronWriteParamFrameStepHint "The number of frames the timeline should step before rendering the new frame. " \
                                       "If 1, all frames will be rendered, if 2 only 1 frame out of 2, "                \
                                       "etc. This number cannot be less than 1."

#define kNatronWriteParamReadBack "readBack"
#define kNatronWriteParamReadBackLabel "Read back file"
#define kNatronWriteParamReadBackHint "When checked, the output of this node comes from reading the written file instead of the input node"

#define kNatronWriteParamReadBackStamp "readBackStamp"

#define kNatronWriteParamStartRender "startRender"

#define kNatronWriteParamOverwrite "overwrite"
#define kNatronWriteParamFormatType "formatType"
#define kNatronWriteParamClipToRoD "clipToRoD"
#define kNatronWriteParamBitDepth "bitDepth"
#define kNatronWriteParamCompression "compression"
#define kNatronWriteParamQuality "quality"
#define kNatronWriteParamDWACompressionLevel "dwaCompressionLevel"
#define kNatronWriteParamZIPCompressionLevel "zipCompressionLevel"
#define kNatronWriteParamTileSize "tileSize"
#define kNatronWriteParamFrameRange "frameRange"
#define kNatronWriteParamPartSplitting "partSplitting"
#define kNatronWriteParamWriteMetadata "writeMetadata"
#define kNatronWriteParamViewsSelector "viewsSelector"
#define kNatronWriteParamFirstFrame "firstFrame"
#define kNatronWriteParamLastFrame "lastFrame"
#define kNatronWriteParamOCIOOutputSpace "ocioOutputSpace"

#define kNatronWriteFrameRangeUnion "union"
#define kNatronWriteFrameRangeProject "project"
#define kNatronWriteFrameRangeManual "manual"

#define kNatronWriteFormatTypeInput "input"
#define kNatronWriteFormatTypeProject "project"
#define kNatronWriteFormatTypeFixed "fixed"

#define kNatronWritePartSplittingSingle "single"
#define kNatronWritePartSplittingViews "views"
#define kNatronWritePartSplittingViewsLayers "views_layers"

#define kNatronWriteViewsAll "All"

#define kNatronWriteBitDepthAuto "auto"
#define kNatronWriteCompressionDefault "default"

#define PLUGIN_MAJOR_NATRON_WRITE 2

NATRON_NAMESPACE_ENTER

/**
 * @brief The native image writer, registered under PLUGINID_NATRON_WRITE at major 2.
 *
 * The planned knobs keep WriteOIIO's script names, because tests, tools and host code look knobs up by name:
 * filename, overwrite, formatType, the host format knobs, clipToRoD, frameRange, firstFrame,
 * lastFrame, frameIncr, bitDepth, compression, quality, dwaCompressionLevel, zipCompressionLevel,
 * tileSize, partSplitting, viewsSelector, ocioOutputSpace, ocioOutputSpaceIndex,
 * ocioOutputSpaceSet, the Context group key1..4 and value1..4, the hidden ocioConfigFile and
 * ocioWorkingSpace, writeMetadata, readBack.
 **/
class NativeWrite
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new NativeWrite(node);
    }

    explicit NativeWrite(NodePtr node);

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool isViewAware() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual SequentialPreferenceEnum getSequentialPreference() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eSequentialPreferenceNotSequential;
    }

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec(LayerKnobSpec::eKindChannelSet, LayerKnobSpec::eRoleInputBound, true);
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getFrameRange(double* first, double* last) OVERRIDE FINAL;

private:
    /**
     * @brief What a frame writes: the data window, and the display window it sits in, both in the
     * file's pixels, and the file's pixel aspect ratio.
     **/
    struct Window {
        RectD rod;
        RectI display;
        double par;
    };

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;

    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The views a render of `view` puts in its file: several when one file takes every view of
    // the project (rendered under view 0), none when another view's render writes the file.
    std::vector<ViewIdx> viewsToWrite(ViewIdx view) const WARN_UNUSED_RETURN;

    void refreshViewsMenu();

    // The format window the format type selects, in pixels.
    void selectedFormat(double time, ViewIdx view, RectI* display, double* par) const;

    bool clipToRoDActive() const;

    bool resolveWindow(double time, ViewIdx view, Window* window) const WARN_UNUSED_RETURN;

    // Whether the output comes from the written file: the knob is on and the file is not being written.
    bool readBackActive() const WARN_UNUSED_RETURN;

    // The headers of the file of `time` and `view`, or null with a persistent error set.
    std::shared_ptr<const OiioReadSupport::Header> readBackHeader(double time, ViewIdx view, std::string* path) WARN_UNUSED_RETURN;

    StatusEnum renderReadBack(const RenderActionArgs& args) WARN_UNUSED_RETURN;

    // The Render button is off while the node shows the file.
    void refreshReadBackState();

    // A finished sequence changed the file, which the hash must tell the viewer.
    void onSequenceRenderFinished();

    // Menus and visibility follow the format of the file name and the chosen compression.
    void refreshOptions();

    bool rejectUnsupportedFormat(const std::string& path);

    // The range a Manual first or last frame starts from: the input's, or the project's when the
    // input has none.
    void seedFrame(const KnobIntPtr& knob, bool isFirst, bool force);

    // The colourspace of the file: the config's file rules for its name, else the project default
    // for the kind of data it holds.
    std::optional<std::string> automaticOutputSpace() const WARN_UNUSED_RETURN;

    OcioSpaceKnobs _outputSpace;
    KnobOutputFileWPtr _filename;
    KnobBoolWPtr _overwrite;
    KnobChoiceWPtr _formatType;
    KnobChoiceWPtr _format;
    KnobIntWPtr _formatSize;
    KnobDoubleWPtr _formatPar;
    KnobBoolWPtr _clipToRoD;
    KnobChoiceWPtr _bitDepth;
    KnobChoiceWPtr _compression;
    KnobIntWPtr _quality;
    KnobDoubleWPtr _dwaCompressionLevel;
    KnobIntWPtr _zipCompressionLevel;
    KnobChoiceWPtr _tileSize;
    KnobChoiceWPtr _partSplitting;
    KnobBoolWPtr _writeMetadata;
    KnobBoolWPtr _readBack;
    KnobIntWPtr _readBackStamp;
    KnobChoiceWPtr _views;
    KnobChoiceWPtr _frameRange;
    KnobIntWPtr _firstFrame;
    KnobIntWPtr _lastFrame;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_NativeWrite_h
