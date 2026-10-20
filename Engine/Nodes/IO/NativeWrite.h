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

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

// The EXR header attribute holding the content hash of the Write when the file was written.
#define kNatronWriteOutputHashAttribute "natron:outputHash"

#define kNatronWriteParamReload "reload"

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
 * ocioWorkingSpace, writeMetadata, readBack, reload.
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

    // The output holds only what the file holds: neither the colour plane nor any other layer of
    // the input passes through around the planes written, so downstream layer menus list the
    // written layers alone.
    virtual bool producesMetadataLayerImplicitly() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual EffectInstance::PassThroughEnum isPassThroughForNonRenderedLayers() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return EffectInstance::ePassThroughBlockNonRenderedLayers;
    }

    // While reading back a frame whose file is current, the output is the file: the input is not
    // needed, so a Viewer shows the file with the input disconnected, and the channel set does
    // not gate the render.
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool outputFollowsLayerKnob(double time, ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Forgets what was read of the read-back files and the images cached for them.
    virtual void purgeCaches() OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getFrameRange(double* first, double* last) OVERRIDE FINAL;

    virtual bool dependsOnProjectFrameRange() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // How an output content hash is stored in the file's header.
    static std::string outputHashText(U64 hash) WARN_UNUSED_RETURN;

    /**
     * @brief The hash a written EXR stores and read back compares: the node hash's content less
     * the readBack knob. The node hash keeps readBack, because the file and the input it stands
     * for are not the same pixels and metadata, while a file written with read back off must
     * still match once it is on. Takes the input's render hash, so inside a render it is the
     * hash of the frame being rendered.
     **/
    U64 getOutputContentHash() const WARN_UNUSED_RETURN;

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

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source" },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_WRITE,
        .label = "Write",
        .description = "Write an image or image sequence.",
        .grouping = PLUGIN_GROUP_IMAGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_WRITE,
        .minorVersion = 0,
        .isWriter = true,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

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

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;

    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // A frame whose file can hold none of the selected channels declares no plane, so its render
    // action is never reached; this is where a render to disk learns it has nothing to write.
    virtual StatusEnum checkFrameToWrite(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The planes produced are those the file of the frame holds, so the output mirrors the file:
    // the planned file while writing or pulling the input, the file on disk while reading it back.
    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    // The input's planes the channel set selects, whole, before the file's format narrows them.
    std::vector<ImageLayerDesc> inputPlanes(double time, ViewIdx view) WARN_UNUSED_RETURN;

    // The colour plane, when the file has one, and every other layer of `header` as the native
    // Read groups them for `view`; read back declares these whatever the input offers.
    std::list<ImageLayerDesc> readBackLayers(const OiioReadSupport::Header& header, ViewIdx view) const WARN_UNUSED_RETURN;

    // The views a render of `view` puts in its file: several when one file takes every view of
    // the project (rendered under view 0), none when another view's render writes the file.
    std::vector<ViewIdx> viewsToWrite(ViewIdx view) const WARN_UNUSED_RETURN;

    void refreshViewsMenu();

    // The format window the format type selects, in pixels.
    void selectedFormat(double time, ViewIdx view, RectI* display, double* par) const;

    bool clipToRoDActive() const;

    bool resolveWindow(double time, ViewIdx view, Window* window) const WARN_UNUSED_RETURN;

    // Whether read back is offered: only an EXR file can hold the hash it is gated on.
    bool readBackAvailable() const WARN_UNUSED_RETURN;

    // Whether the knob asks for the file and the file is not being written. The frame shows the
    // file only when readBackFrame() finds one.
    bool readBackActive() const WARN_UNUSED_RETURN;

    // The headers of the file of `time` and `view` when read back is on and the hash stored in the
    // file is getOutputContentHash(), so the file is what the input would render; null otherwise,
    // and the frame passes the input through.
    std::shared_ptr<const OiioReadSupport::Header> readBackFrame(double time, ViewIdx view, std::string* path) const WARN_UNUSED_RETURN;

    struct ReadBackFile {
        std::shared_ptr<const OiioReadSupport::Header> header; //< null when no readable file is there
        std::string storedHash; //< the file's natron:outputHash, empty if it has none
    };

    // What readBackFrame() found at `path`, looked up on disk the first time only.
    ReadBackFile readBackFile(const std::string& path) const WARN_UNUSED_RETURN;

    StatusEnum renderReadBack(const RenderActionArgs& args, const OiioReadSupport::Header& header, const std::string& path) WARN_UNUSED_RETURN;

    // The Render button is off while the node shows the file, and Reload is on only then.
    void refreshReadBackState();

    // A finished sequence wrote files that the cached images, layers and headers do not reflect.
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
    KnobButtonWPtr _reload;
    // The files read back, kept until purgeCaches() (Reload, a finished render) so that rendering
    // a frame does not look at the disk again.
    mutable std::mutex _readBackFilesMutex;
    mutable std::map<std::string, ReadBackFile> _readBackFiles;
    mutable std::uint64_t _readBackFilesPurges = 0;
    KnobChoiceWPtr _views;
    std::string _viewToKeep;
    KnobChoiceWPtr _frameRange;
    KnobIntWPtr _firstFrame;
    KnobIntWPtr _lastFrame;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_NativeWrite_h
