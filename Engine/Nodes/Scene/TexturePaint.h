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

#ifndef Engine_Nodes_Scene_TexturePaint_h
#define Engine_Nodes_Scene_TexturePaint_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/PrimSelector.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_TEXTUREPAINT "TexturePaint"

#define kTexturePaintParamPrimPattern "primPattern"
#define kTexturePaintParamUvPrimvar "uvPrimvar"
#define kTexturePaintParamTileResolution "tileResolution"
#define kTexturePaintParamBufferResolution "bufferResolution"
#define kTexturePaintParamTextureMode "textureMode"
#define kTexturePaintParamBakeInputsPath "bakeInputsPath"
#define kTexturePaintParamBakeInputs "bakeInputs"
#define kTexturePaintParamBakedInputs "bakedInputs"
#define kTexturePaintParamBakeOutputPath "bakeOutputPath"
#define kTexturePaintParamBakeOutput "bakeOutput"
#define kTexturePaintParamBakedOutput "bakedOutput"
#define kTexturePaintParamBakeGeneration "bakeGeneration"
#define kTexturePaintParamOutputTile "outputTile"

/**
 * The output metadata names the material input each plane of the texture set feeds, under this
 * prefix followed by the plane: <prefix>rgba is "diffuseColor", and the plane of any other paint
 * layer, such as <prefix>specular, holds that layer's name.
 **/
#define kTexturePaintMetadataPurposePrefix "natron/texturePaint/purpose/"
#define kTexturePaintPurposeDiffuseColor "diffuseColor"

// The knobs every session layer carries (RotoLayer::getHostKnobs()).
#define kTexturePaintSessionParamCameraToWorld "cameraToWorld"
#define kTexturePaintSessionParamViewHeight "viewHeight"
#define kTexturePaintSessionParamAspect "aspect"
#define kTexturePaintSessionParamOrthographic "orthographic"
#define kTexturePaintSessionParamNearClip "nearClip"
#define kTexturePaintSessionParamFarClip "farClip"
#define kTexturePaintSessionParamFrozen "frozen"

NATRON_NAMESPACE_ENTER

namespace TexturePaintCache {
class TileDirectoryClaim;
}

class TexturePaintBaker;
struct TexturePaintFileStep;
class TexturePaintInteract;

namespace SceneRaster {
struct ExplicitCamera;
}

/**
 * @brief Paints on the meshes of a scene through the Viewer's 3D view, into one UDIM texture set
 * the matched meshes share. The node hosts a paint RotoContext: its strokes are RotoPaint's,
 * stored in normalized view coordinates. A session, the strokes painted through one view camera,
 * is a layer under the base layer carrying that camera and whether the session is frozen (baked
 * into the Bake Output files) as layer knobs, saved with the project.
 *
 * The output is the UDIM tile Output Tile names, at Tile Resolution: the colour plane, and the
 * Layer knob's plane when it is another one and a session has strokes. Tiles are baked from the
 * sessions on first use and cached on disk (TexturePaintCache), keyed by a hash of the strokes,
 * the sessions and their cameras, the scene, the knobs that shape the texture and the Bake Inputs
 * files, which once baked are the base the sessions are painted over.
 *
 * Bake Output writes the output, every plane of every tile the meshes cover, to the Bake Output
 * files and freezes the sessions with strokes: from then on the tiles start from those files and
 * only the sessions not frozen are laid over them, so a frozen session is never rendered or
 * projected again. The strokes stay the truth: undoing the bake unfreezes its sessions, puts back
 * the output files it overwrote, and the tiles are rebuilt from the strokes.
 **/
class TexturePaint
    : public NativeEffectBase {
public:
    enum TextureModeEnum {
        eTextureModeStatic = 0,
        eTextureModeAnimated
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_TEXTUREPAINT,
        .label = "TexturePaint",
        .description = "Paint on the meshes of the input scene that Prim Pattern selects, in the Viewer's 3D view, with "
                       "RotoPaint's brushes. The matched meshes share one UDIM texture set, laid out by their UV Primvar. "
                       "Strokes land in a paint buffer of Buffer Resolution through the view camera they were painted "
                       "through; each such camera is a session. Sessions are projected into the UDIM tiles, at Tile "
                       "Resolution, with occlusion. Bake Inputs Path and Bake Output Path name the texture files the "
                       "node writes, with a <UDIM> token for the tile number. The output image is the tile Output Tile "
                       "names.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .outputKind = eDataKindImage,
        .inputs = kInputs,
        .primPathKnob = kTexturePaintParamPrimPattern,
        .primPathKnobTakesPattern = true,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TexturePaint(node);
    }

    explicit TexturePaint(NodePtr node);

    virtual ~TexturePaint();

    virtual CurveContextKindEnum getCurveContextKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eCurveContextKindPaint;
    }

    // The internal paint nodes take their mask from the strokes, not from a render of this node.
    virtual bool isRotoPaintNode() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec(LayerKnobSpec::eKindLayerSelect, LayerKnobSpec::eRoleTarget, true);
    }

    // The scene input is not an image the strokes could merge over: each session's own view is.
    virtual NodePtr getPaintTreeSource() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return NodePtr();
    }

    virtual NodePtr getPaintTreeSourceFor(const RotoDrawableItem& item) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual NodePtr getPaintTreeRevealSource(const RotoDrawableItem& item, int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isPaintTreeRoot(const RotoLayer& layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onCurveLayerCreated(const RotoLayerPtr& layer) OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;

    /**
     * @brief Cancels the bake in progress and drops the queued ones, as the node is deleted or the
     * project closes. Never waits, blocking or not: the running bake stops at its next poll.
     **/
    virtual void quitBackgroundWork(bool blocking) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void refreshExtraStateAfterTimeChanged(bool isPlayback, double time) OVERRIDE FINAL;

    /**
     * @brief The paint toolbar and brush settings this node shares with RotoPaint, and the
     * stroke builder that paints with them into a session's layer.
     **/
    RotoPaintInteractPtr getPaintInteract() const;

    // What paints this node from a 3D view, created on first use. Main thread.
    TexturePaintInteract* getViewInteract();

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onEffectCreated(bool mayCreateFileDialog, const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual Route route(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The scene input is read by the node itself, not pulled as an image.
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    // The width and height of a UDIM tile, from Tile Resolution.
    int getTileResolution() const WARN_UNUSED_RETURN;

    // The paint buffer's longer side, from Buffer Resolution.
    int getBufferResolution() const WARN_UNUSED_RETURN;

    TextureModeEnum getTextureMode() const WARN_UNUSED_RETURN;
    /**
     * @brief The frame the texture is built at for a render at time: time itself when Animated,
     * else the first frame of the project range, so that a Static texture does not depend on
     * the frame that happened to be rendered first.
     **/
    double getTextureTime(double time) const WARN_UNUSED_RETURN;

    /**
     * @brief Starts a session painting through camera: a new layer at the top of the base layer,
     * selected, which strokes painted next go into. Main thread.
     **/
    RotoLayerPtr addSession(const SceneRaster::ExplicitCamera& camera);

    // The sessions, the layers directly under the base layer, oldest first.
    std::list<RotoLayerPtr> getSessions() const WARN_UNUSED_RETURN;

    // False when layer does not carry the session knobs, as the base layer does not.
    static bool getSessionCamera(const RotoLayerPtr& layer, SceneRaster::ExplicitCamera* camera) WARN_UNUSED_RETURN;
    static void setSessionCamera(const RotoLayerPtr& layer, const SceneRaster::ExplicitCamera& camera);

    static bool isSessionFrozen(const RotoLayerPtr& layer) WARN_UNUSED_RETURN;
    static void setSessionFrozen(const RotoLayerPtr& layer, bool frozen);

    // The session item is painted in: the layer under the base layer it is in. Null outside one.
    static RotoLayerPtr getSessionOf(const RotoItem& item) WARN_UNUSED_RETURN;

    /**
     * @brief A session's paint buffer: Buffer Resolution on the view's longer side, the other
     * following the session camera's aspect, as 4 floats per pixel (the plane's channels in
     * order, zero past them), row-major and bottom-up.
     **/
    struct SessionBuffer {
        int width = 0;
        int height = 0;
        std::vector<float> rgba;
        // 1 where the strokes changed the view; paint of exactly the view's value reads as 0.
        std::vector<std::uint8_t> touched;
    };

    // The size renderSessionBuffer() gives session's buffer at the current Buffer Resolution.
    void getSessionBufferSize(const RotoLayerPtr& session, int* width, int* height) const;

    /**
     * @brief Renders session's strokes over its view into *buffer, for plane. The strokes paint
     * the Layer knob's plane, and of it only the channels the knob selects; any other plane, or
     * channel, is the view's, untouched. Independent of the node's own output. Main thread: the
     * session's view is created on first use. False with *error set when it cannot be rendered.
     **/
    bool renderSessionBuffer(const RotoLayerPtr& session,
                             const ImageLayerDesc& plane,
                             double time,
                             ViewIdx view,
                             SessionBuffer* buffer,
                             std::string* error);

    // The view session's strokes are painted over, created on first use. Main thread.
    NodePtr getSessionView(const RotoLayerPtr& session);

    /**
     * @brief renderSessionBuffer() over viewNode, which must be session's view. Any thread. With
     * abortInfo, its setAborted() stops the render, which then returns false with *error empty.
     * A view that renders nothing is transparent black; strokes that render nothing paint nothing.
     **/
    bool renderSessionBufferFrom(const NodePtr& viewNode,
                                 const RotoLayerPtr& session,
                                 const ImageLayerDesc& plane,
                                 double time,
                                 ViewIdx view,
                                 SessionBuffer* buffer,
                                 std::string* error,
                                 const AbortableRenderInfoPtr& abortInfo = AbortableRenderInfoPtr());

    /**
     * @brief Puts strokes, a render of a session's strokes over base, in *buffer: the channels
     * not selected stay base's, and a texel is touched where a selected channel differs. Null
     * strokes, a render with nothing in it, leave base in *buffer with nothing touched. Takes
     * base's contents. The caller renders strokes and base over the same pixels; should their
     * sizes differ anyway, returns false with *error set before reading either or writing
     * *buffer.
     **/
    static bool layStrokesOverBase(std::vector<float>& base,
                                   const std::vector<float>* strokes,
                                   const bool selected[4],
                                   SessionBuffer* buffer,
                                   std::string* error) WARN_UNUSED_RETURN;

    /**
     * @brief The planes the node outputs at time and view: the colour plane, then the Layer
     * knob's plane when it is not the colour plane and a session has strokes.
     **/
    std::list<ImageLayerDesc> getOutputPlanes(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief Bake Inputs: bakes the look of the materials bound to the matched meshes at time
     * into the Bake Inputs Path files (MaterialBake), one per UDIM tile they cover and material
     * input, and makes them the base the strokes are painted over. *files receives the files
     * written, *warning what the bake stood fallbacks in for. False with *error set on failure.
     * Waits for the bake, which runs on a thread of its own so that live textures can render:
     * for tests and scripts; the Bake Inputs button uses enqueueBakeInputs().
     **/
    bool bakeInputs(double time,
                    ViewIdx view,
                    std::vector<std::string>* files,
                    std::string* warning,
                    std::string* error);

    /**
     * @brief The Bake Inputs files the tiles are painted over, as the last bake wrote them:
     * the file of each purpose at each tile, Bake Inputs Path expanded. Empty before any bake.
     **/
    std::vector<std::string> getBakedInputFiles() const WARN_UNUSED_RETURN;

    /**
     * @brief Bake Output: writes the output at time and view, each output plane of each UDIM tile
     * the matched meshes cover, to the Bake Output Path files, one half-float OpenEXR file per
     * tile and plane named by its purpose (MaterialBake::purposePath()), and freezes the sessions
     * that have strokes. *files receives the files written. Returns the undo step of the bake,
     * already done, or null with *error set on failure. Main thread; waits for the bake.
     **/
    UndoCommandPtr bakeOutput(double time,
                              ViewIdx view,
                              std::vector<std::string>* files,
                              std::string* error);

    // The Bake Output files the tiles start from, as the last bake wrote them. Empty before any bake.
    std::vector<std::string> getBakedOutputFiles() const WARN_UNUSED_RETURN;

    // How many session buffers have been rendered over the node's lifetime.
    int getSessionRenderCount() const WARN_UNUSED_RETURN
    {
        return _sessionRenders.load();
    }

    /**
     * @brief What a Bake Output changes, so that its undo step can put it back: the saved record
     * of the bake, and the sessions it froze.
     **/
    struct BakeOutputState {
        std::string record;
        // The record the bake set, for redoing it.
        std::string bakedRecord;
        std::list<RotoLayerWPtr> frozen;
    };

    /**
     * @brief The files one Bake Output wrote and what its undo step swaps them with. Backups and
     * stashes sit in a hidden directory beside each target, so that every swap is a rename. Only
     * steps of the TexturePaintFileQueue touch one once its bake has begun.
     **/
    struct BakeOutputFiles {
        /**
         * @brief A file's device, inode, size and modification time. A rename or a hard link
         * changes none of them; a file rewritten in place keeps its inode but not its
         * modification time. Where the file system makes the node copy instead, the copy has an
         * identity of its own, so a swap that moved files records their identity again.
         **/
        struct FileIdentity {
            std::uint64_t device = 0;
            std::uint64_t inode = 0;
            std::int64_t size = -1;
            std::int64_t mtimeSeconds = 0;
            std::int64_t mtimeNanoseconds = 0;
            bool known = false;

            bool operator==(const FileIdentity& other) const
            {
                return (known == other.known) && (device == other.device) && (inode == other.inode) && (size == other.size) && (mtimeSeconds == other.mtimeSeconds) && (mtimeNanoseconds == other.mtimeNanoseconds);
            }

            bool operator!=(const FileIdentity& other) const
            {
                return !(*this == other);
            }

            // The identity of the file at path, or an unknown one when it cannot be read.
            static FileIdentity of(const std::string& path);
        };

        struct Entry {
            std::string target;
            // target's TexturePaintFileQueue::claimKey().
            std::string key;
            // What target held before the bake, while it is not at target.
            std::string backup;
            // What the bake wrote, while the bake is undone.
            std::string stash;
            bool hasBackup = false;
            // Whether the bake's file reached target; a target it never reached is not the bake's
            // to put back.
            bool wrote = false;
            // The file the bake wrote, recorded right after it reached target and moved by rename
            // in every swap: a target that no longer matches it was written by something else since.
            FileIdentity written;
        };
        std::vector<Entry> entries;
        // The claim keys of the entries.
        std::unordered_set<std::string> targets;
        // Where the backups and stashes are, deleted with them.
        std::set<std::string> directories;
        std::string token;
        // The bake's identity in the TexturePaintFileQueue claims, given when the bake is queued or
        // with its first claim; 0 before.
        std::uint64_t bake = 0;
        // The node that bakes, by TexturePaintFileQueue::newId(), which names it; 0 for none.
        std::uint64_t owner = 0;
        bool undone = false;
        // Whether the backups and stashes are deleted, after which nothing can be swapped.
        bool discarded = false;
    };

    /**
     * @brief Before target is written, on the TexturePaintFileQueue: claims it, adds it to *files
     * and, when it exists, keeps what it holds as its backup, by a hard link so that the write's
     * rename leaves the link holding it. False with *error set when another bake holds target or
     * it cannot be kept; *heldBySameOwner tells whether the holder is an earlier bake of the same
     * node, which will land or be put back.
     **/
    static bool backUpBakeTarget(BakeOutputFiles* files,
                                 const std::string& target,
                                 std::string* error,
                                 bool* heldBySameOwner = 0) WARN_UNUSED_RETURN;

    /**
     * @brief Puts every target the bake of *files wrote back as it was before the bake, and
     * deletes the backups; a target the bake never wrote is left as it is. A written target that
     * is no longer the file the bake wrote is left as it is too, and its backup, what it held
     * before the bake, is moved beside it as <name>.pre-bake rather than deleted. A backup that cannot be moved stays where it is, with its directory. *kept
     * receives where each such backup is. With evict, drops the targets' cached headers. Leaves
     * the claims of *files to the caller.
     **/
    static void rollBackBakeOutputFiles(BakeOutputFiles* files,
                                        bool evict = true,
                                        std::vector<std::string>* kept = 0);

    /**
     * @brief Submits rollBackBakeOutputFiles() to the TexturePaintFileQueue, for a Bake Output that
     * failed or whose completion was dropped, then settles the claims of files; returns at once
     * with the step to wait for. Bumps *epoch before and after the files go back. A backup kept
     * is reported in the error log and as node's warning.
     **/
    static std::shared_ptr<TexturePaintFileStep> enqueueBakeOutputRollback(const std::shared_ptr<BakeOutputFiles>& files,
                                                                           const std::shared_ptr<std::atomic<std::uint64_t>>& epoch,
                                                                           const std::weak_ptr<TexturePaint>& node = std::weak_ptr<TexturePaint>());

    // Drops the TexturePaintFileQueue claims of files, once an undo step owns them.
    static void settleBakeOutputFiles(const BakeOutputFiles& files);

    // Marks target, backed up in *files, as written by the bake and records its identity, right after the bake's file reached it.
    static void markBakeTargetWritten(BakeOutputFiles* files,
                                      const std::string& target);

    // Records again the identity of each file the bake wrote, after a swap put them back.
    static void sealBakeOutputFiles(BakeOutputFiles* files);

    /**
     * @brief Undoing, moves each target to its stash and its backup, if any, to the target;
     * redoing, the reverse, backing up whatever is at the target by then. Asking for the state
     * the files are already in moves nothing and succeeds. Refuses, moving nothing, when another
     * bake holds a target, and, undoing, when a target is no longer the file the bake wrote. A
     * failure puts back what it moved and returns false with *error set. Refuses once files are
     * discarded. With evict, drops the targets' cached headers.
     **/
    static bool swapBakeOutputFiles(BakeOutputFiles* files,
                                    bool undo,
                                    std::string* error,
                                    bool evict = true) WARN_UNUSED_RETURN;

    /**
     * @brief The hidden file beside path that the bake of process pid, started at the given
     * clock tick in the pid namespace space names, writes before renaming it over path; serial
     * tells its writes apart.
     **/
    static std::string bakeTempPath(const std::string& path,
                                    const std::string& space,
                                    long pid,
                                    unsigned long long started,
                                    unsigned int serial) WARN_UNUSED_RETURN;

    /**
     * @brief A short hex token naming this boot's pid namespace of this process, or an empty
     * string when it cannot be read.
     **/
    static const std::string& bakeProcessSpace() WARN_UNUSED_RETURN;

    /**
     * @brief The start time of process pid in clock ticks since boot, as seen from this pid
     * namespace; false when it could not be read, which does not mean the process is gone.
     **/
    static bool processStartTime(long pid, unsigned long long* ticks) WARN_UNUSED_RETURN;

    /**
     * @brief Deletes the bakeTempPath() files in directory written in this process's pid
     * namespace by processes that are no longer running, the pid now naming another process
     * included.
     **/
    static void removeStaleBakeTemps(const std::string& directory);

    // Deletes the backups and stashes of files.
    static void discardBakeOutputFiles(const BakeOutputFiles& files);

    // Submits discardBakeOutputFiles() to the TexturePaintFileQueue, behind every swap of files.
    static void enqueueBakeOutputDiscard(const std::shared_ptr<BakeOutputFiles>& files);

    /**
     * @brief Bake Output without its undo step: writes the files, sets the record and freezes the
     * sessions with strokes, which *state receives along with the record as it was. *undoFiles
     * receives what the undo step swaps, the targets that existed before backed up. False with
     * *error set on failure, the files put back as they were and nothing changed, or while a
     * queued Bake Output of the node, or an undo or redo of one, has not landed.
     **/
    bool writeBakeOutput(double time,
                         ViewIdx view,
                         std::vector<std::string>* files,
                         std::shared_ptr<BakeOutputFiles>* undoFiles,
                         BakeOutputState* state,
                         std::string* error);

    // Undoes writeBakeOutput(): unfreezes state's sessions and sets the record back to state's.
    void restoreBakeOutputState(const BakeOutputState& state);
    // Puts back what restoreBakeOutputState() removed, without baking: the record and the frozen sessions of state.
    void reapplyBakeOutputState(const BakeOutputState& state);

    /**
     * @brief Queues an undo (or redo) of the Bake Output of files and state on the node's baker,
     * behind every bake queued before it. When the baker reaches it, the main thread checks that
     * the record is still the one the bake set (undo) or found (redo); if so the files swap on
     * the TexturePaintFileQueue and, once that lands, the state is restored (undo) or reapplied.
     * Otherwise, or when the files cannot be swapped, nothing changes and the persistent message
     * says why. *applied, which the undo step shares with every swap it queues, tells whether its
     * bake is applied as the swaps landed so far left it: a swap asking for the state it is in
     * already does nothing, so that the swap after a refused one leaves the files as they are. A
     * swap the baker drops never moves the files, or puts back what it moved; when they cannot
     * be put back, the node takes the state the files are in and the persistent message says so.
     * Main thread.
     **/
    bool enqueueBakeOutputSwap(const std::shared_ptr<BakeOutputFiles>& files,
                               const BakeOutputState& state,
                               bool undo,
                               const std::shared_ptr<bool>& applied);

    // Makes putting back the files of a dropped undo or redo fail, as a full disk would.
    static void setFailReverseSwapsForTesting(bool fail);

    // Bake Outputs that land hand their undo step to sink rather than pushing it, when set.
    void setUndoCommandSinkForTesting(const std::function<void(const UndoCommandPtr&)>& sink);

    /**
     * @brief The bakes below queue on the node's TexturePaintBaker, in order, and return at once;
     * main thread. Each lands later on the main thread, where it changes the node as the
     * synchronous bake of the same name does and bumps the bake generation, which refreshes the
     * output. A bake that fails shows its error as the node's persistent message. False, with the
     * message set, when it cannot be queued.
     *
     * A Bake Output pushes its undo step when it lands: the step names files and sessions that
     * the bake changes, so it cannot exist, and cannot be undone, before then.
     **/
    bool enqueueBakeInputs(double time, ViewIdx view);
    bool enqueueBakeOutput(double time, ViewIdx view);

    /**
     * @brief Projects session, a closed one, into the tile cache at time and view: every tile the
     * meshes cover is baked, so rendering finds them cached. The session's strokes stay what is
     * drawn until this lands.
     **/
    bool enqueueSessionBake(const RotoLayerPtr& session, double time, ViewIdx view);

    // Whether a bake is queued, running, or landing.
    bool isBaking() const WARN_UNUSED_RETURN;

    // Whether a session bake queued for session has not landed yet. Main thread.
    bool isSessionBakePending(const RotoLayerPtr& session) const WARN_UNUSED_RETURN;

    // How many bakes have landed successfully, since the node was created.
    quint64 getBakeGeneration() const WARN_UNUSED_RETURN;

    // The node's baker, created on first use. Main thread.
    TexturePaintBaker* getBaker();

protected:
    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    // What the baker's closures hold instead of this, so that a bake never outlives the node unseen.
    std::weak_ptr<TexturePaint> weakSelf();

    struct BakeControl {
        std::function<bool()> cancelled;
        std::function<void(double, const QString&)> progress;
    };
    struct BakeInputsRun;
    struct BakeOutputRun;

    bool prepareBakeInputs(double time, ViewIdx view, BakeInputsRun* run, std::string* error);
    bool runBakeInputs(BakeInputsRun& run, const BakeControl& control, std::string* error);
    void applyBakeInputs(BakeInputsRun& run, std::vector<std::string>* files, std::string* warning);

    bool prepareBakeOutput(double time, ViewIdx view, BakeOutputRun* run, std::string* error);
    /**
     * @brief Main thread, as the bake begins: takes the planes and the sessions run renders and
     * freezes, leaving out one a stroke is going into. With closeOpenSession the open session,
     * when it is taken, is closed, so that the next stroke starts another.
     **/
    void snapshotBakeOutput(BakeOutputRun* run, bool closeOpenSession);
    // Whether every session of run's snapshot is unchanged, live and still a session. Main thread.
    bool isBakeOutputSnapshotCurrent(const BakeOutputRun& run) const WARN_UNUSED_RETURN;
    /**
     * @brief On the baker: has the main thread take run's snapshot, and waits for it. False when
     * the node is gone or the bake is cancelled first.
     **/
    static bool awaitBakeOutputSnapshot(const std::weak_ptr<TexturePaint>& weak,
                                        const std::shared_ptr<BakeOutputRun>& run,
                                        const std::function<bool()>& cancelled) WARN_UNUSED_RETURN;
    bool runBakeOutput(BakeOutputRun& run, const BakeControl& control, std::string* error);
    /**
     * @brief Writes rgba to a temporary file beside path on the calling thread, then backs path up
     * and renames the temporary file over it on the TexturePaintFileQueue, waiting for that. The
     * temporary file is gone once the step ran, whether or not the write landed.
     **/
    static bool writeBakeTarget(const BakeOutputRun& run,
                                const std::string& path,
                                const std::shared_ptr<std::vector<float>>& rgba,
                                const BakeControl& control,
                                std::string* error);
    void applyBakeOutput(BakeOutputRun& run, BakeOutputState* state);

    bool runSessionBake(double time, ViewIdx view, const BakeControl& control, std::string* error);

    // Makes the strokes' lifetime knobs count when Animated and not otherwise, in the stroke tree.
    void syncStrokeLifetimes();
    // The frames a bake covers: the project range when Animated, else the pinned frame alone.
    void getBakeFrames(int* first, int* last) const;
    // pattern with its frame token expanded to frame, then purposePath().
    bool bakedFilePath(const std::string& pattern, const std::string& purpose, int tile, int frame, std::string* path, std::string* error) const WARN_UNUSED_RETURN;
    // Whether pattern names a different file at each frame.
    static bool hasFrameToken(const std::string& pattern) WARN_UNUSED_RETURN;
    // The frame of a baked file the texture at time is built over, within the frames record names.
    int recordFrame(double time, int firstFrame, int lastFrame) const WARN_UNUSED_RETURN;
    void landBake(bool ok);

    struct SessionView {
        RotoLayerWPtr session;
        NodePtr node;
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;

    NodePtr findSessionView(const RotoLayer* session) const WARN_UNUSED_RETURN;

    // Creates session's view if it has none and brings it in step; *created tells which.
    NodePtr ensureSessionView(const RotoLayerPtr& session, bool* created);

    void syncSessionView(const RotoLayerPtr& session, const NodePtr& viewNode);

    // Brings every view in step after a change to the scene input or a knob they copy.
    void syncSessionViews();

    // The plane strokes paint: the Layer knob's, or the colour plane when it selects none.
    ImageLayerDesc getPaintedPlane(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    bool sessionHasStrokes(const RotoLayerPtr& session) const WARN_UNUSED_RETURN;

    // Bake Inputs Path with [Project] expanded; false with *error set unless it is absolute.
    bool getBakeInputsPattern(std::string* pattern, std::string* error) const WARN_UNUSED_RETURN;

    // The purposes and UDIM tiles the last Bake Inputs wrote, from the saved record of it.
    void getBakedInputs(std::vector<std::string>* purposes, std::vector<int>* tiles, std::pair<int, int>* frames = 0) const;

    // The baked file of purpose at tile, empty when the last bake wrote none.
    std::string getBakedInputPath(const std::string& purpose, int tile, double time) const WARN_UNUSED_RETURN;

    // Bake Output Path with [Project] expanded; false with *error set unless it is absolute.
    bool getBakeOutputPattern(std::string* pattern, std::string* error) const WARN_UNUSED_RETURN;

    // Whether a Bake Output is in effect, so the tiles start from its files.
    bool hasBakedOutput() const WARN_UNUSED_RETURN;

    // The Bake Output file of purpose at tile, empty when the last bake wrote none.
    std::string getBakedOutputPath(const std::string& purpose, int tile, double time) const WARN_UNUSED_RETURN;

    // The file a tile of purpose starts from: the Bake Output file, else the Bake Inputs one.
    std::string getBasePath(const std::string& purpose, int tile, double time) const WARN_UNUSED_RETURN;

    // The colour plane's base files as TexturePaintView::setBase() takes them.
    std::string getViewBaseFiles(double time) const WARN_UNUSED_RETURN;

    /**
     * @brief The directory the tiles of this node are cached in, named by its cache ID, in
     * *directory. False with *error set when it cannot be claimed: no tile is written to a
     * directory nothing holds, and the claim is tried again after a while.
     **/
    bool getTileDirectory(std::string* directory, std::string* error) const WARN_UNUSED_RETURN;

    /**
     * @brief The sessions a tile lays over its base, in order. With only, the sessions that are
     * not frozen are those of only alone; *narrowed tells whether that left any out.
     **/
    std::vector<RotoLayerPtr> getLaidSessions(const std::vector<RotoLayerPtr>* only, bool* narrowed) const WARN_UNUSED_RETURN;

    /**
     * @brief What the tiles of view are cached under; changes with anything they depend on. In
     * Animated mode the frame is in the tile's file name instead, so the key is that of every frame.
     * With only, the key of tiles laying getLaidSessions(only) alone, *narrowed as it tells.
     **/
    U64 getTileCacheKey(double time,
                        ViewIdx view,
                        const std::vector<RotoLayerPtr>* only = nullptr,
                        bool* narrowed = nullptr) const WARN_UNUSED_RETURN;

    // Hashes the baked files' paths and modification stamps, read once per bake record and epoch.
    U64 getBakedFilesDigest() const WARN_UNUSED_RETURN;

    // Composes and writes planes of tile into paths, which are not cached. As ensureTiles().
    bool buildTiles(double time,
                    ViewIdx view,
                    int tile,
                    const std::list<ImageLayerDesc>& planes,
                    const std::vector<std::string>& paths,
                    std::string* note,
                    std::string* error,
                    const std::function<bool()>& cancelled,
                    const std::vector<RotoLayerPtr>* only);

    // Deletes the cached tiles no view, reader or build still uses. Without _tilesMutex held.
    void removeUnusedTiles(const std::string& directory);

    /**
     * @brief Bakes and caches planes of tile at time and view unless they are cached already;
     * *paths receives the file of each plane, in order, and *note the overlap warning of the
     * tile, empty when its meshes' UV layouts do not overlap. False with *error set on failure,
     * and with *error empty when the render was aborted.
     *
     * One tile is built once however many threads ask for it, the others waiting for it, while
     * different tiles build at once. When lease is set it receives what keeps *paths from being
     * deleted as stale; the files may be read until it is released. With only, the tiles lay
     * getLaidSessions(only).
     **/
    bool ensureTiles(double time,
                     ViewIdx view,
                     int tile,
                     const std::list<ImageLayerDesc>& planes,
                     std::vector<std::string>* paths,
                     std::string* note,
                     std::string* error,
                     const std::function<bool()>& cancel = std::function<bool()>(),
                     std::shared_ptr<void>* lease = nullptr,
                     const std::vector<RotoLayerPtr>* only = nullptr);

    // Views are looked up from render threads while the main thread creates them.
    mutable std::mutex _viewsMutex;
    std::map<const RotoLayer*, SessionView> _views;

    struct TileBuild {
        U64 key = 0;
        bool done = false;
        bool ok = false;
        std::string error;
    };

    // Guards the tile builds, readers and live keys below, never held across a build.
    std::mutex _tilesMutex;
    std::condition_variable _tileBuilt;
    // The tile builds running, by the key, frame and tile they write.
    std::map<std::string, std::shared_ptr<TileBuild>> _tileBuilds;
    // How many ensureTiles() leases hold the files of each key.
    std::map<U64, int> _tileReaders;
    // The keys whose files removeUnusedTiles() is deleting.
    std::set<U64> _pruningKeys;
    // The key each view last asked for, so that one view's tiles do not evict another's.
    std::map<int, U64> _liveTileKeys;
    // Bumped by undo, redo and a Bake Output that is rolled back, so that no tile composed before
    // over the files they changed is used again. Shared with the rollback of a Bake Output whose
    // completion was dropped, which must not keep the node alive.
    const std::shared_ptr<std::atomic<std::uint64_t>> _tileEpoch;
    mutable std::mutex _tileDirectoryMutex;
    // The directory getTileDirectory() names, held until the node's cache id or the project's
    // saved state changes, and deleted with the node unless others can reuse it.
    mutable std::shared_ptr<TexturePaintCache::TileDirectoryClaim> _tileClaim;
    mutable std::string _tileClaimId;
    mutable bool _tileClaimSaved = false;
    // The cache id and saved state no directory could be claimed for, and when to try again, so
    // that every tile read does not.
    mutable bool _tileClaimFailed = false;
    mutable std::string _tileClaimFailedId;
    mutable bool _tileClaimFailedSaved = false;
    mutable std::chrono::steady_clock::time_point _tileClaimRetry;
    mutable std::mutex _stampsMutex;
    mutable bool _stampsValid = false;
    mutable U64 _stampsSignature = 0;
    mutable U64 _stampsDigest = 0;

    PrimSelector _primPattern;
    KnobStringWPtr _uvPrimvar;
    KnobChoiceWPtr _tileResolution;
    KnobChoiceWPtr _bufferResolution;
    KnobChoiceWPtr _textureMode;
    KnobOutputFileWPtr _bakeInputsPath;
    KnobButtonWPtr _bakeInputs;
    // What the last Bake Inputs wrote, saved with the project, so the tiles keep painting over
    // those files and their cache key changes with each bake.
    KnobStringWPtr _bakedInputs;
    KnobOutputFileWPtr _bakeOutputPath;
    KnobButtonWPtr _bakeOutput;
    // What the last Bake Output wrote, as _bakedInputs is for Bake Inputs.
    KnobStringWPtr _bakedOutput;
    // Set to the baker's generation as each bake lands, so that the node's hash changes and the output refreshes.
    KnobIntWPtr _bakeGeneration;
    KnobIntWPtr _outputTile;
    RotoPaintInteractPtr _paintInteract;
    std::unique_ptr<TexturePaintInteract> _viewInteract;
    std::atomic<int> _sessionRenders;
    std::unique_ptr<TexturePaintBaker> _baker;
    // This node's identity as the owner of TexturePaintFileQueue claims.
    const std::uint64_t _bakeOwnerId;
    // Bake Outputs, and their undos and redos, queued, running or landing, whose files are not
    // yet owned by an undo step or put back.
    const std::shared_ptr<std::atomic<int>> _pendingBakeOutputs;
    std::function<void(const UndoCommandPtr&)> _undoSinkForTesting;
    // quitBackgroundWork clears this from a destroyNode watcher thread, and a queued
    // clear could outlive the node, so the map is guarded rather than main-thread only.
    mutable std::mutex _pendingSessionBakesMutex;
    // Session bakes queued and not landed, by session.
    std::map<const RotoLayer*, int> _pendingSessionBakes;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexturePaint_h
