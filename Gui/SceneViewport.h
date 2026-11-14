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

#ifndef NATRON_GUI_SCENEVIEWPORT_H
#define NATRON_GUI_SCENEVIEWPORT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Global/GLIncludes.h" // must come before qopengl.h

#include <QOpenGLWidget>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <OpenColorIO/OpenColorIO.h>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"

// Free of USD: the viewer tab includes it, and USD's headers need the full Python API. Everything
// that touches pxr lives in SceneViewport.cpp.

class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

NATRON_NAMESPACE_ENTER

class SceneStage;
struct SceneDrawDelta;
struct SceneDrawChange;
class SceneDrawStates;
class SceneDrawTiming;
class UndoHistory;
class SceneTransformTarget;
struct SceneTransformDrag;

/**
 * @brief Draws a scene with Storm (UsdImagingGLEngine on HgiGL) into a framebuffer of the GL
 * context current when its methods run, with a ground grid and an op's transform handles over it,
 * and picks prims at pixels.
 *
 * Another delegate, such as a path tracer, renders progressively in the background
 * (HydraRender::ProgressiveRender): each edit of the stage, the view or Max Samples restarts it, it
 * refines until it converges, reaches Max Samples or is paused, and render() draws its latest
 * frame, uploaded to a texture, under the grid and handles.
 *
 * Owns one long-lived stage (SceneStage) for its whole life and syncs each new scene into it, so
 * an edit upstream reaches the engine as the stage's change notices, which it applies to the
 * changed prims alone. The engine is made
 * once per GL context, at the first render() in it, and must be released with releaseGL() in
 * that context: when the context goes away or the renderer does, never because the scene changed.
 *
 * The stage and the engine are guarded by one lock, the viewer lock: syncScene() and the commit of
 * a payload load hold it exclusively while they edit the stage, as do render() and pick(), which
 * touch the engine. What only reads the stage shares it, with renderBackgroundFrame() among them,
 * so that a first render taking seconds keeps out edits and no reader.
 *
 * syncScene() and serviceStageLoads() may run on any thread, and the stage load requests are safe
 * from any. renderBackgroundFrame() runs in the GL context of render(), with no thread waiting for
 * it; it reads only what prepareBackgroundFrame() copied. Everything else is for one thread at a
 * time, normally the main thread; render() and pick() may run on another while that thread waits.
 **/
class SceneViewportRenderer {
public:
    enum DrawModeEnum {
        eDrawModeUnshaded = 0, // the prims' display colours, no lighting
        eDrawModeShaded, // lit by the lighting mode's lights
        eDrawModeWireframe
    };

    // What lights the shaded mode: the stage's own lights when it has any, else a light at the
    // camera (auto); always the stage's lights, dark when it has none (scene); or always the
    // light at the camera (headlight). Scene until set; the viewer sets it from the Viewer 3D
    // Lighting preference.
    enum LightingModeEnum {
        eLightingModeAuto = 0,
        eLightingModeScene,
        eLightingModeHeadlight
    };

    // How the free camera looks at its pivot: in perspective, or orthographic along an axis of
    // the stage. Front looks along -Z on a Y-up stage and along +Y on a Z-up one, Right along -X,
    // and Top down the up axis.
    enum ViewModeEnum {
        eViewModePerspective = 0,
        eViewModeFront,
        eViewModeBack,
        eViewModeRight,
        eViewModeLeft,
        eViewModeTop,
        eViewModeBottom
    };

    // The camera that is not a prim of the stage: it orbits pivot at distance, azimuth and
    // elevation in degrees about the stage's up axis, with a vertical field of view of fovY degrees.
    // An orthographic view shows the height the perspective one shows at the pivot.
    struct FreeCamera {
        double pivot[3] = { 0., 0., 0. };
        double distance = 10.;
        double azimuth = 0.;
        double elevation = 0.;
        double fovY = 45.;
        ViewModeEnum viewMode = eViewModePerspective;
    };

    // The free camera's near and far clip distances, from its eye. Automatic ones follow the
    // camera's distance to its pivot.
    struct ClipPlanes {
        bool automatic = true;
        double nearClip = 0.1;
        double farClip = 10000.;
    };

    // Where a camera is and what it sees, as Gf (row-vector) matrices in row-major order: its
    // camera-to-world and its parent's local-to-world transforms, the height of its view at unit
    // depth (perspective) or in scene units (orthographic), its width over its height, its clip
    // distances, and the depth ahead of it that navigation turns about.
    struct CameraPose {
        double cameraToWorld[16];
        double parentToWorld[16];
        double viewHeight = 1.;
        double aspect = 1.;
        bool orthographic = false;
        double nearClip = 0.1;
        double farClip = 10000.;
        double pivotDepth = 1.;
    };

    SceneViewportRenderer();

    ~SceneViewportRenderer();

    enum RenderStatusEnum {
        eRenderStatusDone = 0, // the image is final
        eRenderStatusRefining, // Storm has not converged; another render would refine the image
        eRenderStatusBusy, // a sync holds the viewer lock: nothing was drawn
        eRenderStatusWaiting // a background render refines the image and calls the refine listener when it has more
    };

    // The progressive render of a delegate other than Storm.
    struct RefineStatus {
        // A delegate other than Storm is chosen and its engine is rendering, or has rendered.
        bool active = false;
        bool stopped = false;
        bool converged = false;
        // The samples per pixel rendered, -1 if the delegate reports none, and the cap.
        int samples = -1;
        int maxSamples = 0;
        // The renders started and the frames published since the delegate was chosen.
        U64 restarts = 0;
        U64 frames = 0;
        // The samples per pixel of the frame drawn last, -1 if unknown or none was.
        int drawnSamples = -1;
    };

    /**
     * @brief Syncs the renderer's stage to payload, under the viewer lock, on the calling thread.
     * Shows nothing for an empty payload. Returns false with *error set if USD is unusable.
     * MT-safe; does not move the camera. serial, when not 0, is the request the payload answers:
     * a request later than clearTransformPreviewAfter()'s clears the drag preview in the same
     * step, so the preview gives way to the scene that has the drag written.
     **/
    bool syncScene(const ScenePayload& payload, std::string* error, U64 serial = 0);

    // syncScene(), and shows the stage at frame from then on: a paint never sees one without the
    // other.
    bool syncScene(const ScenePayload& payload, double frame, double framesPerSecond, std::string* error, U64 serial = 0);

    /**
     * @brief Shows payload at the project frame (a project running at framesPerSecond). The free
     * camera is framed on the scene the first time a scene is shown, and kept for later scenes.
     * Returns false with *error set if USD is unusable, and then shows nothing. Syncs on the
     * calling thread.
     **/
    bool setScene(const ScenePayload& payload, double frame, double framesPerSecond, std::string* error);

    void setFrame(double frame, double framesPerSecond);

    // Shows nothing. Keeps the stage and the engine for the next scene.
    void clearScene();

    bool hasScene() const;

    /**
     * @brief Calls read under the lock that keeps syncs and paints out of the stage, with the
     * stage shown, or null when no scene is. read must not call back into the renderer.
     **/
    void readStage(const std::function<void(const SceneStage* stage)>& read) const;

    // readStage() unless an edit holds the stage: then false, without waiting or calling read.
    bool tryReadStage(const std::function<void(const SceneStage* stage)>& read) const;

    /**
     * @brief Called, on the reader's thread, when a readStage() that kept out a try for the lock
     * to edit has ended. Readers announce nothing else, unlike syncs, loads and frames.
     **/
    void setReadEndedCallback(const std::function<void()>& callback);

    // The shown stage's edit count, without the lock: 0 before the first sync.
    U64 getStageEditCount() const;

    // The serial of the last sync the stage took, counts and summary included: 0 before the first.
    U64 getStageSyncSerial() const;

    // Whether the free camera has been framed on a scene, or placed, yet.
    bool isFramed() const;

    // How many engines this renderer has made: one per GL context it rendered in and delegate.
    U64 getEngineCount() const;

    // How many engines every renderer of the process has made.
    static U64 getEngineConstructionCount();

    // Whether the renderer holds an engine, which the next render() makes when it does not.
    bool hasEngine() const;

    /**
     * @brief Whether the shown stage, or a setting that decides Storm's shaders (draw mode, lighting,
     * purposes, display transform), changed since the engine last rendered, or there is no engine:
     * the render that follows does the Hydra work, however long that takes, rather than only
     * drawing. Main thread.
     **/
    bool needsBackgroundFrame() const;

    /**
     * @brief Queues a draw state change for this renderer's engine; needsBackgroundFrame() holds
     * until a frame has taken it in. revealBatch 0 shows everything in one go, and kRevealPaced in
     * batches sized from the frames' cost. startedAt is when the change was asked for, for
     * drawTiming(). Main thread; takes no lock a frame or a sync holds.
     **/
    void postDrawDelta(const SceneDrawDelta& delta,
                       std::size_t revealBatch,
                       std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::time_point());

    /** @brief Render thread. Sends what postDrawDelta() queued to the engine's filter. */
    void applyPendingDrawDeltas();

    /**
     * @brief Takes in one change of the viewer's draw states: posts its delta as postDrawDelta()
     * does, drops the stage's queued loads at the change's cancelled paths and asks the stage for
     * its loads, which serviceStageLoads() then takes. Never edits the stage. True if a delta was
     * posted, for applyPendingDrawDeltas() to send. Main thread.
     **/
    bool postDrawChange(const SceneDrawChange& change, std::size_t revealBatch);

    static constexpr std::size_t kRevealPaced = static_cast<std::size_t>(-1);

    bool isDrawRevealing() const;

    /** @brief The image of the last reveal step is on screen, so the next step may go. */
    void noteDrawRevealPresented();

    /**
     * @brief Whether renders leave the stage out, drawing the background and overlays only, so that
     * no frame shows prims before the policy that applies to them. Main thread.
     **/
    void setStageHeld(bool held);
    bool isStageHeld() const;

    /**
     * @brief How long a reveal still waits for the camera, in milliseconds: none goes out while the
     * user moves it, so the frames it asks for come at the scene's own cost. Main thread.
     **/
    int getRevealYieldMs() const;

    struct DrawFilterStats {
        bool filterBound = false;
        bool revealing = false;
        // The batch the next paced reveal step shows, and the frame cost the pacer has fitted.
        unsigned long long revealBatch = 0;
        double revealFixedMs = 0.;
        double revealPerPrimUs = 0.;
        unsigned long long revealSteps = 0;
        double lastApplyMs = 0.;
        std::size_t ruleCount = 0;
        unsigned long long posted = 0;
        unsigned long long absorbed = 0;
        unsigned long long inputAdded = 0;
        unsigned long long inputRemoved = 0;
        unsigned long long inputDirtied = 0;
        unsigned long long hiddenSent = 0;
        unsigned long long shownSent = 0;
        // Of hiddenSent and shownSent, those sent as a visibility change.
        unsigned long long visibilitySent = 0;
        unsigned long long heldInvisible = 0;
        // Native instances turned off in their instancer's mask.
        unsigned long long maskedInstances = 0;
        long long lastInputAddedEpochMs = 0;
        unsigned long long stageEdits = 0;
        unsigned long long stageLoadEdits = 0;
        unsigned long long stageNotices = 0;
        unsigned long long resyncedPaths = 0;
        unsigned long long infoOnlyPaths = 0;
    };
    DrawFilterStats getDrawFilterStats() const;

    // The boxes and the markers of boxed rows the last frame drew.
    int getDrawnBoxCount() const;
    int getDrawnMarkerCount() const;

    // A box or marker the last render drew: its row, kind (box, unloaded, marker or pending),
    // whether it holds a selected prim, its projected centre and the screen rectangle of its
    // corners, in pixels counted from the top left, for scripts to aim at.
    struct DrawnBox {
        std::string path;
        std::string kind;
        bool highlighted = false;
        double x = 0.;
        double y = 0.;
        double left = 0.;
        double top = 0.;
        double right = 0.;
        double bottom = 0.;
    };

    // Off by default: building the records costs nine projections per box.
    void setRecordDrawnBoxes(bool record);

    const std::vector<DrawnBox>& getDrawnBoxes() const;

    // Null unless NATRON_DRAW_STATE_TIMING is 1.
    SceneDrawTiming* drawTiming() const;

    /**
     * @brief Copies what renderBackgroundFrame() needs for a width x height frame. False when no
     * such frame is needed, or a sync holds the viewer lock; the sync's end is the time to retry.
     * Not to be called while a background frame runs.
     **/
    bool prepareBackgroundFrame(int width, int height);

    /**
     * @brief Makes the engine if needed and renders the prepared frame into fbo, without overlays.
     * If the next render() is of the same view into the same fbo with nothing changed, it keeps this
     * image and only draws the overlays. Needs the context of render() current. Renders up to a
     * few passes, and after the first ends early for stopBackgroundFrame() or a thread waiting to
     * edit the stage; then it keeps nothing for render() to reuse.
     **/
    void renderBackgroundFrame(unsigned int fbo);

    /**
     * @brief Ends the background frame running, or the one prepared, after its current pass, and
     * never before its first: what it drew is taken in as for a full frame. Any thread.
     **/
    void stopBackgroundFrame();

    /**
     * @brief Ends the background frame running, or the one prepared, before its next pass, the
     * first included, and then it takes nothing in. Any thread.
     **/
    void cancelBackgroundFrame();

    /**
     * @brief Passes load requests on to the stage, which takes them when serviceStageLoads() next
     * runs. Before the first sync has made the stage they wait, in order, for it to; the cancels
     * drop them alike. See SceneStage::requestLoad().
     **/
    void requestStageLoad(const std::vector<std::string>& paths,
                          bool withDescendants,
                          const std::vector<std::string>& excluded = std::vector<std::string>());

    void requestStageUnload(const std::vector<std::string>& paths);

    void cancelStageLoads();

    // Drops the load requests at paths that no batch has taken yet.
    void cancelStageLoads(const std::vector<std::string>& paths);

    bool hasPendingStageLoads() const;

    // Whether a load request waits, before the stage exists too, or a batch is being serviced.
    bool hasOutstandingStageLoads() const;

    /**
     * @brief Services the pending requests, opening their files with the viewer lock released.
     * Returns, committing nothing, once cancelled() turns true. See SceneStage::serviceLoads().
     **/
    bool serviceStageLoads(const std::function<bool()>& cancelled = std::function<bool()>());

    /**
     * @brief The scene as evaluated with a load set: evaluate sets *payload to the viewed node's
     * output with nodes' patterns matched on that set, and *reach to where those patterns reach,
     * returning false when it cannot. committed is told each load set the stage commits, under
     * the viewer lock in the same step, and whether the scene must be evaluated again for it.
     **/
    typedef std::function<bool(const SceneLoadSetPtr& loadSet, ScenePayload* payload, SceneLoadReachPtr* reach)> LoadSetEvaluator;
    typedef std::function<void(const SceneLoadSetPtr& loadSet, bool reevaluate)> LoadSetCommitted;

    /**
     * @brief Has serviceStageLoads() evaluate the scene for the load set a batch makes, once its
     * files are open, when the patterns of the scene on the stage reach what the batch loads or
     * unloads, and sync that scene in the same commit as the loads, so that a prim a node
     * deactivates by pattern never shows while its payload comes in. A batch no pattern reaches
     * evaluates nothing, as a load is no edit of the scene. committed is told to evaluate again
     * when the stage's scene still matched on another set once the batch commits: its evaluation
     * failed, it has live textures, which the worker renders first, or a sync came meanwhile. It
     * is also told when a sync is dropped as evaluated for a set its patterns no longer match.
     * Any thread.
     **/
    void setLoadSetEvaluator(const LoadSetEvaluator& evaluate,
                             const LoadSetCommitted& committed);

    // The hash of the load set the stage last committed (SceneLoadSet::getHash()).
    U64 getCommittedLoadSetHash() const;

    /**
     * @brief The load set the payload of the next syncScene() was evaluated for and the reach of
     * the patterns it matched, a null reach for none in particular. A payload whose patterns
     * reach what changed between that set and the stage's is stale, the committed callback is
     * told to evaluate it again, and that sync changes nothing.
     **/
    void setSyncEvaluatedFor(const SceneLoadSetPtr& loadSet,
                             const SceneLoadReachPtr& reach);

    // How many load commits edited the stage, and how long the last batch held the viewer lock.
    U64 getStageLoadEditCount() const;

    double getLastStageLoadHoldMs() const;

    // The prims whose last load failed, each with why.
    std::vector<std::pair<std::string, std::string>> getStageLoadFailures() const;

    /**
     * @brief The stage's payload load rules as its last load commit left them, kept while no scene
     * is shown, the root's left out when it loads nothing. False before the first sync has made
     * the stage. Any thread; never waits on the stage lock.
     **/
    bool getStageLoadRules(SceneLoadRules* rules) const;

    // The stage timecode shown, the project frame mapped by seconds.
    double getTimeCode() const;

    // Whether the shown stage's up axis is Z rather than Y.
    bool isUpAxisZ() const;

    void setDrawMode(DrawModeEnum mode);

    DrawModeEnum getDrawMode() const;

    // How what Storm draws reaches the screen: the display, view and look of config, from
    // colorSpace, as the 2D viewer shows images, with its gain multiplied in before them and its
    // gamma applied after. Gain and gamma act on what was rendered, not on the background.
    struct DisplayTransform {
        OCIO_NAMESPACE::ConstConfigRcPtr config;
        std::string colorSpace;
        std::string display;
        std::string view;
        // Empty for the view's own looks.
        std::string look;
        double gain = 1.;
        double gamma = 1.;
    };

    /**
     * @brief Applies transform from the next render() on. Without a config, or when the config
     * cannot build the transform, the sRGB encoding is applied instead, still with the gain and
     * gamma. A change of gain or gamma alone only redraws.
     **/
    void setDisplayTransform(const DisplayTransform& transform);

    // Whether render() goes through OpenColorIO rather than the sRGB encoding.
    bool isUsingOpenColorIO() const;

    // The ground grid, drawn over the scene by default.
    void setShowGrid(bool show);

    bool getShowGrid() const;

    // The camera and light guides, drawn over the scene by default. The camera looked through
    // has none.
    void setShowGuides(bool show);

    bool getShowGuides() const;

    // How long camera frustums reach and how big light icons are, in scene units: 1 until set.
    void setGuideSize(double size);

    double getGuideSize() const;

    // Whether prims of the proxy, render and guide purposes are drawn: proxy only by default.
    void setShowProxy(bool show);

    bool getShowProxy() const;

    void setShowRender(bool show);

    bool getShowRender() const;

    void setShowGuidePurpose(bool show);

    bool getShowGuidePurpose() const;

    void setLightingMode(LightingModeEnum mode);

    LightingModeEnum getLightingMode() const;

    // Whether the last render lit the shaded mode with the stage's lights rather than the headlight.
    bool isSceneLit() const;

    // A guide the last render drew, its kind's script name and a pixel on it, counted from the top
    // left, for scripts to aim at.
    struct DrawnGuide {
        std::string path;
        std::string kind;
        double x = 0.;
        double y = 0.;
    };

    // Off by default: building the records costs a projection per guide segment.
    void setRecordDrawnGuides(bool record);

    const std::vector<DrawnGuide>& getDrawnGuides() const;

    void setFreeCamera(const FreeCamera& camera);

    const FreeCamera& getFreeCamera() const;

    // Looks through the free camera in mode. An orthographic mode faces its axis; perspective
    // keeps the current angles.
    void setViewMode(ViewModeEnum mode);

    ViewModeEnum getViewMode() const;

    void setClipPlanes(const ClipPlanes& planes);

    const ClipPlanes& getClipPlanes() const;

    // The near and far distances the free camera draws with.
    void getFreeCameraClipRange(double* nearClip, double* farClip) const;

    /**
     * @brief What a width x height view shows: through the camera looked through, or through the
     * free camera, whose pivotDepth is its distance. For a stage camera, cameraToWorld is the
     * prim's own transform (with any scale), parentToWorld its parent's, and pivotDepth the depth
     * ahead of it of the scene's centre, as computeFrameBounds() last found it. False only for a
     * stage camera that cannot be read.
     **/
    bool getViewPose(int width, int height, CameraPose* pose, bool* busy = nullptr) const;

    // What a framing frames, worked out apart from moving the camera.
    struct FrameBounds {
        double min[3] = { 0., 0., 0. };
        double max[3] = { 0., 0., 0. };
        bool empty = true;
        bool selection = false;
    };

    /**
     * @brief The bounds of the prims at the paths in selection at timeCode, or of the scene when it
     * is empty. A stage walk, so any thread other than the one painting. Prims with no payload
     * loaded count by their extent hints, and no payload is read.
     **/
    FrameBounds computeFrameBounds(const std::vector<std::string>& selection, double timeCode) const;

    // Frames the free camera on bounds, and looks through it. Main thread.
    void applyFrameBounds(const FrameBounds& bounds);

    // Frames the free camera on the bounds of the scene at the shown time, and looks through it.
    void frameScene();

    /**
     * @brief Frames the free camera on the union of the selected prims' bounds at the shown time,
     * and looks through it. A prim without extent counts as its world origin, and a selection
     * without extent is framed with a radius of one unit. Frames the scene if nothing is selected.
     **/
    void frameSelection();

    // Turns the free camera by degrees, in perspective.
    void tumble(double azimuthDegrees, double elevationDegrees);

    // Slides the free camera's pivot in the view plane by a pixel delta of a view height pixels high.
    void pan(double dxPixels, double dyPixels, int viewHeight);

    // Moves the free camera towards (factor < 1) or away from (factor > 1) its pivot.
    void dolly(double factor);

    // The paths of the stage's UsdGeomCamera prims, in stage order, as of the last sync or load.
    std::vector<std::string> getStageCameraPaths() const;

    /**
     * @brief Looks through the UsdGeomCamera prim at path, or through the free camera if path is
     * empty or not a camera of the stage. Moving the free camera looks through it again.
     **/
    void setLookThroughCamera(const std::string& path);

    const std::string& getLookThroughCamera() const;

    /**
     * @brief Draws the camera looked through from cameraToWorld (a Gf row-major matrix) in place of
     * its prim's transform, until clearLookThroughTransform() or another camera is looked through.
     * Lets a move of the camera show at once, before its edit reaches the stage.
     **/
    void setLookThroughTransform(const double cameraToWorld[16]);

    void clearLookThroughTransform();

    // Whether the camera looked through is drawn from setLookThroughTransform().
    bool hasLookThroughTransform() const;

    // The highlighted prims, in selection order: the last is the current one.
    void setSelectedPrims(const std::vector<std::string>& paths);

    const std::vector<std::string>& getSelectedPrims() const;

    // The values of an op whose handles the viewer draws over the scene: a translate, rotate
    // (degrees, in rotateOrder, a SceneRotateOrderEnum), scale and pivot, authored as opName on
    // primPath and on each of the other targets, whose prims a drag previews too.
    struct TransformHandles {
        struct Target {
            std::string primPath;
            std::string opName;
        };

        std::string primPath;
        std::string opName;
        double translate[3] = { 0., 0., 0. };
        double rotate[3] = { 0., 0., 0. };
        double scale[3] = { 1., 1., 1. };
        double uniformScale = 1.;
        double pivot[3] = { 0., 0., 0. };
        int rotateOrder = 0;
        std::vector<Target> otherTargets;
    };

    /**
     * @brief Draws the handles of an op over the scene, at the shown time, until
     * clearTransformHandles(). Nothing is drawn while the shown stage has no such op on the prim.
     * The transform widget on the selection, when its tool shows handles, is drawn instead.
     **/
    void setTransformHandles(const TransformHandles& handles);

    void clearTransformHandles();

    /**
     * @brief The transform widget's tool, frame and origin (SceneTransform::ToolEnum, SpaceEnum
     * and PivotEnum): the handles of the tool on the selected prims.
     **/
    void setTransformTool(int tool);

    int getTransformTool() const;

    void setTransformSpace(int space);

    int getTransformSpace() const;

    void setTransformPivot(int pivot);

    int getTransformPivot() const;

    // Whether handles are drawn: the transform widget's on the selection or the op's.
    bool hasTransformHandles() const;

    // Whether the handles drawn are the transform widget's on the selection.
    bool hasSelectionHandles(bool* busy = nullptr) const;

    /*
     * The calls below taking busy are for the main thread, which must not wait out a load. They do
     * not wait for the lock: when an edit holds it they set busy and return the empty result.
     */

    /**
     * @brief The handles drawn, as pixels to aim at in a width x height view, and the frame they
     * point along: origin then the three axes, 12 values. False when none are drawn.
     **/
    bool getDrawnTransformHandles(int width, int height, std::vector<int>* handles, std::vector<double>* pixels, std::vector<double>* frame, bool* busy = nullptr) const;

    // The handle (a SceneGizmo handle) under the pixel (x, y) of a width x height view, or -1.
    int hitTestTransformHandles(double x, double y, int width, int height, bool* busy = nullptr) const;

    // The handle drawn highlighted, -1 for none.
    void setHighlightedTransformHandle(int handle);

    int getHighlightedTransformHandle() const;

    // A pixel on handle, to aim at. False if the handle is not drawn.
    bool getTransformHandlePixel(int handle, int width, int height, double* x, double* y) const;

    /**
     * @brief Starts dragging handle from the pixel (x, y): later changes are measured against the
     * handles as they are now, and a drag of the selection's widget moves the prims it has now.
     * Clears any drag preview first. False if the handle is not drawn.
     **/
    bool beginTransformDrag(int handle, double x, double y, int width, int height, bool* busy = nullptr);

    // Whether the drag under way moves the selection, rather than an op's values.
    bool isSelectionTransformDrag() const;

    /**
     * @brief The change a drag to the pixel (x, y) makes since beginTransformDrag(): the amount
     * added to the translate or rotate value of the handle's axis, or the factor its scale value
     * is multiplied by. False if the drag says nothing, as for a ring seen edge-on.
     **/
    bool getTransformDragChange(double x, double y, int width, int height, double* change, bool* busy = nullptr) const;

    /**
     * @brief Draws the prims the drag under way moves as a change of change would leave them,
     * without editing any node: a transient transform on the renderer's own layer. Returns
     * whether any prim is drawn moved.
     **/
    bool previewTransformDrag(double change, bool* busy = nullptr);

    /**
     * @brief The selection drag under way, for a change, as a target writes it. False if the drag
     * under way does not move the selection.
     **/
    bool getTransformDrag(double change, double frame, SceneTransformDrag* drag) const;

    void endTransformDrag();

    // Drops the drag preview now. False if an edit holds the stage: it is still there.
    bool clearTransformPreview();

    // Drops the drag preview when a scene requested after serial is synced.
    void clearTransformPreviewAfter(U64 serial);

    /**
     * @brief Draws into the framebuffer fbo of the current context, width x height pixels, and
     * leaves the GL state as it found it. Clears to the background when there is no scene.
     * Returns false if Storm has not converged yet and another render would refine the image.
     **/
    bool render(unsigned int fbo, int width, int height);

    /**
     * @brief render(), unless a sync holds the viewer lock, in which case nothing is drawn and the
     * framebuffer keeps what it had. For paints, which must not wait for a sync. It keeps the
     * renderer it has: when Scene Default's prim names another, it draws with the old one and
     * hasPendingRendererSwitch() turns true until followSceneRenderer() makes the switch.
     **/
    RenderStatusEnum renderIfIdle(unsigned int fbo, int width, int height);

    /**
     * @brief renderIfIdle() while the delegate is not Storm, in any context sharing the one
     * render() runs in: the refinement makes its frames on its own thread, and drawing one needs
     * no engine. Draws nothing and returns eRenderStatusBusy while the delegate is Storm.
     **/
    RenderStatusEnum renderRefinementIfIdle(unsigned int fbo, int width, int height);

    /**
     * @brief Under Scene Default, renders with the renderer the stage's prim names from the next
     * render on; true if that changed the renderer. A change destroys the engine, so it needs the
     * context of render() current.
     **/
    bool followSceneRenderer();

    // Whether the last renderIfIdle() drew with a renderer Scene Default no longer names.
    bool hasPendingRendererSwitch() const;

    /**
     * @brief The path of the prim drawn nearest to the pixel (x, y), counted from the top left of
     * a width x height view, or empty if none is: a guide within a few pixels, or a boxed row's box
     * or marker, wins over a prim behind it. Needs the context of render() current.
     **/
    std::string pick(int x, int y, int width, int height, bool* busy = nullptr);

    /**
     * @brief The paths of the prims and guides drawn in the rectangle with corners (x0, y0) and
     * (x1, y1), pixels of a width x height view counted from the top left, sorted. Needs the context of
     * render() current.
     **/
    std::vector<std::string> pickRect(int x0, int y0, int x1, int y1, int width, int height, bool* busy = nullptr);

    // Destroys the engine. Needs the context of render() current.
    void releaseGL();

    /**
     * @brief Renders with the Hydra render delegate id from the next render() on, Storm's for an
     * empty id. kRenderDelegateSceneDefaultId renders with the renderer the stage's active render
     * settings prim names (natron:renderer), Storm's without one, following it as the stage
     * changes. A change destroys the engine, so it needs the context of render() current.
     **/
    void setRendererId(const std::string& id);

    // The renderer chosen: a delegate's id, or kRenderDelegateSceneDefaultId.
    std::string getRendererId() const;

    // The delegate rendering: the one chosen, or for Scene Default the one the prim names as of
    // the last render.
    std::string getActiveRendererId() const;

    // Why the chosen delegate could not render, empty if it can or has not been tried yet.
    std::string getRendererError() const;

    // The error of a render that threw, which discarded the engine; cleared by reading it.
    std::string takeRenderError();

    // The delegate of the engine held, empty if none is.
    std::string getEngineRendererId() const;

    // The background colour as it shows: through the display transform, without the viewer's gain
    // and gamma, which act on rendered pixels only.
    void getBackgroundColor(float rgb[3]) const;

    /**
     * @brief The cap on the samples per pixel of a progressive render; <= 0 for the delegate's
     * default. A change restarts the render; it holds across a change of delegate.
     **/
    void setMaxSamples(int samples);

    // The cap in force: the one set, or the delegate's default once known, else 0.
    int getMaxSamples() const;

    /**
     * @brief Renders the whole scene past its draw states; turning it on also asks the stage to
     * load every payload. Off restores the draw states and unloads nothing.
     **/
    void setRenderEverything(bool on);

    // Pausing keeps a progressive render's samples; unpausing continues from them.
    void setRefinementPaused(bool paused);

    RefineStatus getRefineStatus() const;

    /**
     * @brief Called on the background render's thread whenever it has a new frame for render() to
     * draw. Empty for none.
     **/
    void setRefineListener(const std::function<void()>& listener);

    // Ends a progressive render and frees its engine and threads; the next render() starts another.
    void releaseRefinement();

    // How long the last upload of a progressive frame to its texture took, in milliseconds, and how
    // many pixels it had; -1 and 0 before the first.
    double getLastUploadMs() const;
    int getLastUploadPixels() const;

private:
    RenderStatusEnum renderLocked(unsigned int fbo, int width, int height, bool followScene);

    RenderStatusEnum renderProgressive(unsigned int fbo, int width, int height);

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

/**
 * @brief The viewer tab's view of a scene input: SceneViewportRenderer in a QOpenGLWidget, with
 * the camera driven from the mouse, prim picking, and a context menu for the draw mode and the
 * camera to look through, and transform handles that drag a node's knobs. Shows an error message
 * in place of the scene when there is one.
 *
 * setScene() hands the payload to a worker thread, which syncs the renderer's stage; a paint that
 * finds the worker syncing keeps the last frame, and the widget repaints once the sync is done.
 *
 * The engine lives on the widget's render thread, in a context of its own sharing textures with the
 * widget's, drawing into a texture that paints copy to the screen. A QOpenGLWidget's context cannot
 * leave the main thread, and an engine cannot change context. A frame that takes in a stage change
 * renders there with the main thread free, showing the old image or a composing status meanwhile;
 * any other frame is a paint waiting for the render thread to draw. Where Qt or the driver cannot
 * share a context across threads, the engine stays in the widget's context on the main thread.
 *
 * The stage opens with payloads unloaded. requestLoad() and requestUnload() go through a thread of
 * their own, so that slow file opens hold up neither syncs nor the main thread.
 **/
class SceneViewport
    : public QOpenGLWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit SceneViewport(QWidget* parent = nullptr);

    ~SceneViewport() override;

    /**
     * @brief Shows payload once the worker has synced it; a newer call before that replaces it.
     * The free camera is framed on the first scene shown.
     **/
    void setScene(const ScenePayload& payload, double frame, double framesPerSecond);

    // Whether every setScene(), setError() and clearScene() so far has been synced and, in a
    // visible view, taken in by the engine, so that the next paint shows it.
    bool isSceneSynced() const;

    // Whether the stage tryReadStage() reads is the one the last setScene(), setError() or
    // clearScene() asked for. Unlike isSceneSynced(), this does not wait for the engine's frame, so
    // readers of the stage are not left waiting on a frame that announces nothing when it ends.
    bool isStageSynced() const;

    void setFrame(double frame, double framesPerSecond);

    /**
     * @brief What the payload of the next setScene() was evaluated for. See
     * SceneViewportRenderer::setSyncEvaluatedFor().
     **/
    void setSceneEvaluatedFor(const SceneLoadSetPtr& loadSet,
                              const SceneLoadReachPtr& reach);

    // See SceneViewportRenderer::setLoadSetEvaluator().
    void setLoadSetEvaluator(const SceneViewportRenderer::LoadSetEvaluator& evaluate,
                             const SceneViewportRenderer::LoadSetCommitted& committed);

    // Shows message instead of a scene.
    void setError(const QString& message);

    void clearScene();

    const QString& getError() const;

    SceneViewportRenderer& getRenderer();

    void setDrawMode(SceneViewportRenderer::DrawModeEnum mode);

    void setDisplayTransform(const SceneViewportRenderer::DisplayTransform& transform);

    // Renders with the Hydra render delegate id, whatever any USDRender node renders with.
    void setRendererId(const QString& id);
    void setShowGrid(bool show);

    void setShowGuides(bool show);

    // Whether prims of the proxy, render and guide purposes are drawn.
    void setShowPurposes(bool proxy, bool render, bool guide);

    // Sets the Viewer 3D Lighting preference, which every 3D view follows.
    void setLightingMode(SceneViewportRenderer::LightingModeEnum mode);

    // The progressive render's cap on samples per pixel, <= 0 for the delegate's default. The
    // viewer's Max Samples and Refinement Paused knobs drive these.
    void setMaxSamples(int samples);

    void setRenderEverything(bool on);

    void setRefinementPaused(bool paused);

    void setLookThroughCamera(const QString& path);

    void frameScene();

    void frameSelection();

    enum PickwalkEnum {
        ePickwalkParent,
        ePickwalkChild,
        ePickwalkPrevSibling,
        ePickwalkNextSibling
    };

    /**
     * @brief Moves every selected prim one step through the stage by the children the scene graph
     * lists, and merges prims that land on one: the current prim stays last. A prim with nowhere
     * to go stays; siblings wrap. Child takes the child last left by a Parent walk of the same
     * parent, else the first. Returns false, changing nothing, when the stage is busy, as reading
     * it must not wait on the stage's writers.
     **/
    bool pickwalk(PickwalkEnum direction);

    // The prim drawn at pos (widget coordinates), empty for none. If the scene can't be picked
    // without waiting, sets busy instead: an empty result then says nothing about pos.
    QString pickPrimAt(const QPoint& pos, bool* busy = nullptr);

    // The prims drawn in rect (widget coordinates).
    QStringList pickPrimsIn(const QRect& rect, bool* busy = nullptr);

    // How a click or marquee combines its prims with the selection.
    enum SelectionModeEnum {
        eSelectionModeReplace,
        eSelectionModeAdd,
        eSelectionModeToggle
    };

    void select(const QStringList& paths, SelectionModeEnum mode);

    // The selected prims, the current one last.
    void setSelection(const QStringList& paths);

    QStringList getSelection() const;

    /**
     * @brief Sets the function that names the node whose transform handles are drawn while the
     * transform widget shows none on the selection: a USDEditTransform with its settings panel
     * open, or null for none. Dragging a handle sets the node's Translate, Rotate or Scale once,
     * at the release, as one undoable edit.
     **/
    void setTransformNodeProvider(const std::function<NodePtr()>& provider);

    /**
     * @brief Where drags of the transform widget on the selection are written. Without one, or
     * while the natronSceneTransformRecordOnly property is true, they are only recorded into the
     * natronSceneTransformDelta property.
     **/
    void setTransformTarget(const std::shared_ptr<SceneTransformTarget>& target);

    // The transform widget's tool, frame and origin (SceneTransform::ToolEnum, SpaceEnum, PivotEnum).
    void setTransformTool(int tool);

    int getTransformTool() const;

    // Translate, rotate, scale and back to translate; from the select tool, translate.
    void cycleTransformTool();

    void setTransformSpace(int space);

    void setTransformPivot(int pivot);

    // The node whose scene is shown: new cameras are placed near it, in its group.
    void setViewedNodeProvider(const std::function<NodePtr()>& provider);

    /**
     * @brief What the selection is used for. setOnNodeBlocker says why setOnNode can't take the
     * paths now, or nothing when it can.
     **/
    struct SelectionActions {
        std::function<QString(const QStringList& paths)> setOnNodeBlocker;
        std::function<void(const QStringList& paths)> setOnNode;
        std::function<void(const QStringList& paths)> createFromSelection;
    };

    void setSelectionActions(const SelectionActions& actions);

    // The Camera node that made the camera prim at a path of the shown scene, null for none.
    void setCameraNodeProvider(const std::function<NodePtr(const std::string&)>& provider);

    // Where moves of a locked camera's node are undone.
    void setUndoHistory(UndoHistory* history);

    void setViewMode(SceneViewportRenderer::ViewModeEnum mode);

    void setClipPlanes(const SceneViewportRenderer::ClipPlanes& planes);

    // While locked, navigating through a stage camera moves it, and the end of each move writes
    // its node's knobs.
    void setLockView(bool locked);

    bool isLockView() const;

    /**
     * @brief Locks the view to the camera looked through. From the free camera, it looks through
     * the stage's only camera and locks to it, or offers a menu of the stage's cameras to pick
     * from; with none, the HUD says so and the view stays unlocked.
     **/
    void lockViewToCamera();

    // Looks through the camera at path and locks the view to it.
    void lockViewToCamera(const QString& path);

    // Makes an unconnected Camera node, near the viewed node, that sees what the view shows.
    NodePtr createCameraFromView();

    // Reads the handles of the provided node's op from its knobs, at the shown frame.
    void refreshTransformHandles();

    /**
     * @brief Destroys the renderer's engine and the GPU memory it holds, in this widget's context,
     * and repaints, which makes another. The stage stays: it holds no GPU memory, and making it
     * again would mean syncing the whole scene again. A scene showing live textures is requested
     * again, as the store they live in is emptied alongside, and the repaint waits for that sync.
     * Main thread; waits for a sync under way.
     **/
    void releaseEngine();

    /**
     * @brief Calls read with this viewer's draw states, unless an edit of many rows holds them,
     * which returns false at once. Main thread.
     **/
    bool tryReadDrawStates(const std::function<void(const SceneDrawStates&)>& read) const;

    /**
     * @brief This viewer's set draw states and its stage's load set, as a project saves them. A
     * payload whose load failed is kept, so that a file found again loads on reopening, and so are
     * the loads a restore still waits on. Saves the states as the last edit left them and the load
     * rules as the last load commit did, so it waits on neither. Any thread.
     **/
    void saveSceneView(Serialization::SceneViewSerialization* serialization) const;

    /**
     * @brief Puts the saved states in place of this viewer's as one draw state edit, and asks for
     * the saved load set, before the first sync if need be. Both go the way an edit's and a twirl's
     * go: cancelPendingLoads() stops the loads, a later state change cancels the ones it no longer
     * wants, and a load that fails shows on its row. Main thread.
     **/
    void restoreSceneView(const Serialization::SceneViewSerialization& serialization);

    /**
     * @brief Every draw state this viewer sets, effective or remembered, in path order, as the last
     * edit left them. Any thread; never waits on an edit.
     **/
    std::vector<std::pair<std::string, SceneDrawStates::Stored>> getSetDrawStates() const;

    // Puts states in place of every draw state this viewer sets, as one edit. Main thread.
    void replaceSetDrawStates(const std::vector<std::pair<std::string, SceneDrawStates::Stored>>& states);

    // Puts states in place of this viewer's at their paths, as one edit; the others stay. Main thread.
    void mergeSetDrawStates(const std::vector<std::pair<std::string, SceneDrawStates::Stored>>& states);

    // Clears this viewer's states at paths, as one edit; the others stay. Main thread.
    void clearSetDrawStates(const std::vector<std::string>& paths);

    // Whether this viewer sets any draw state, as the last edit left them. Any thread; never waits on an edit.
    bool hasSetDrawStates() const;

    /**
     * @brief Runs edit, an edit of this viewer's draw states, once the worker has matched the graph
     * policy of a sync requested from now on or of rules published from now on, or after a short
     * while at most, so that a graph edit made now and edit show in one frame. Drawing goes on
     * meanwhile with the states and policy the view has. Main thread.
     **/
    void editWithGraphPolicy(const std::function<void()>& edit);

public Q_SLOTS:
    // Replaces the selection. A slot, so that scripts reach it through the meta object.
    void selectPaths(const QStringList& paths);

    // Prim paths. A slot, so that scripts reach it through the meta object.
    void requestLoad(const QStringList& paths, bool withDescendants);

    void requestUnload(const QStringList& paths);

    // Drops the load requests no batch has taken yet; a batch under way finishes.
    void cancelPendingLoads();

    /**
     * @brief This viewer's draw state on a prim path, by script name: bbox, children, full, loadAll
     * or hidden. False if the path or the name is not one. Never edits a stage or waits on Hydra.
     **/
    bool setDrawState(const QString& path, const QString& state);
    void clearDrawState(const QString& path);
    // Clears the rows below path; its own state stays.
    void clearDrawStatesBelow(const QString& path);
    // Clears path and the rows below it, which then follow the graph's policy or the default.
    void resetDrawStatesToGraph(const QString& path);
    /**
     * @brief The same edits on many rows, made as one edit with one change, so nothing is drawn or
     * loaded between rows. setDrawStateOnRows() is false, and edits nothing, if a path or the
     * name is not one.
     **/
    bool setDrawStateOnRows(const QStringList& paths, const QString& state);
    void clearDrawStatesBelowRows(const QStringList& paths);
    void resetDrawStatesToGraphOnRows(const QStringList& paths);
    void clearDrawStates();
    // The state a path resolves to, effective or inherited. Waits for an edit under way.
    QString resolvedDrawState(const QString& path) const;
    /**
     * @brief What a change shows, and what its loads bring in, goes out this many prims per frame; 0
     * shows it at once. Below 0, the default, each frame's batch is sized from what the frames before
     * it cost, so the viewer keeps its frame rate.
     **/
    void setDrawRevealBatch(int prims);
    /**
     * @brief With NATRON_DRAW_STATE_TIMING=1, traces the frames of the changes that follow, until
     * called with false. Every presented change publishes its record to natronSceneDrawTimings.
     **/
    void setDrawTimingTrace(bool trace);

Q_SIGNALS:

    void selectionChanged(const QStringList& paths);

    // From the context menu's Renderer submenu.
    void rendererChanged(const QString& id);

    // Whenever the natronSceneSamples family of properties is published.
    void refineStateChanged();

    // The renderer's stage holds the latest requested scene, or none (main thread).
    void sceneSynced();

    // Emitted on the main thread after a batch of payload requests is serviced or the waiting ones
    // are cancelled.
    void stageLoadsChanged();

    // A draw state edit has reached the renderer (main thread).
    void drawStatesChanged();

    // First thing in the destructor, with the renderer still whole (main thread).
    void closing();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    // Mirrors the error, whether a scene is shown, whether every request is synced, how many
    // engines the renderer made, whether it holds one, its delegate and that delegate's error into
    // the natronSceneError, natronSceneShown, natronSceneSynced, natronSceneEngineCount,
    // natronSceneEngineHeld, natronSceneRenderer and natronSceneRendererError
    // properties, the display settings into natronSceneDrawMode, natronSceneShowGrid,
    // natronSceneShowGuides, natronScenePurposes and natronSceneLighting, and the progressive
    // render's state into natronSceneSamples, natronSceneMaxSamples, natronSceneRefining,
    // natronSceneRefinementStopped, natronSceneRefineConverged, natronSceneRestartCount and
    // natronSceneUploadMs.
    void publishState();

    void publishRefineState();

    // Starts dragging the transform handle at pos (widget coordinates). False if none is there, or
    // with busy set if an edit holds the stage and it can't tell.
    bool beginHandleDrag(const QPoint& pos, bool* busy = nullptr);

    void dragHandle(const QPoint& pos);

    void endHandleDrag();

    void highlightHandleAt(const QPoint& pos);

    // Writes the change of the op handle drag under way to its node, as one undo step.
    bool commitOpHandleDrag(double change);

    /**
     * @brief Mirrors the handles drawn into natronSceneTransformHandles (handle, x, y in widget
     * coordinates) and natronSceneTransformFrame (origin and axes). publishState() mirrors the
     * widget's tool, frame and origin into natronSceneTransformTool, natronSceneTransformSpace and
     * natronSceneTransformPivot.
     **/
    void publishTransformState();

    // Forgets the mouse gesture under way.
    void clearGesture();

    // Whether navigation moves the camera looked through, not the free camera.
    bool navigatesLockedCamera() const;

    // Unlocks the view once it no longer looks through a camera, writing a move of the camera under
    // way, so a later look-through starts unlocked.
    void endLockOutsideLookThrough();

    // Starts moving the locked camera's node. False, with the HUD saying why, when it has none.
    bool beginCameraLock();

    // Applies a tumble or pan drag of delta pixels, or a dolly by dollyFactor, to the locked camera.
    void moveLockedCamera(int navigation, const QPoint& delta, double dollyFactor);

    // Writes the move to the camera's node as one undo step.
    void endCameraLock();

    void showContextMenu(const QPoint& globalPos);

    // A menu of the cameras at paths, under the pointer: the one picked is looked through, locked.
    void showLockCameraMenu(const std::vector<std::string>& paths);

    // Queues payload for the worker; the error shown once it is synced. keepsFrame lets a frame
    // rendering the stage go on, for a payload that likely leaves the stage as it is.
    void requestSync(const ScenePayload& payload, const QString& error, bool keepsFrame = false);

    /**
     * @brief Main thread: the worker synced request serial, with error set on failure. Drawing
     * waits for the worker's match awaitedMatch, if not 0, before it shows the stage.
     **/
    void onSynced(U64 serial, bool ok, const QString& error, const SceneViewportRenderer::FrameBounds& bounds, U64 awaitedMatch);

    // Puts a framing on the worker. selection is empty for the scene.
    void requestFraming(const std::vector<std::string>& selection);

    void onFramingComputed(U64 id, const SceneViewportRenderer::FrameBounds& bounds);

    // Main thread: starts the frame that takes in a stage change, if one is due and can start.
    void startBackgroundFrame();
    // rows: how many rows the edit brings in, which sends a large one to the worker.
    void editDrawStates(const std::function<SceneDrawDelta(SceneDrawStates*)>& edit, std::size_t rows = 0);
    void editDrawStateRows(const std::vector<SceneDrawRowEdit>& edits);
    void deliverDrawChange(const SceneDrawChange& change);
    // Main thread: the graph policy the scene's draw rules, of the generation given, matched; matchId
    // numbers the worker's matches and serial is the sync matched for, both 0 for one after loads.
    void onDrawRulesMatched(U64 generation, U64 matchId, U64 serial, const SceneDrawStates::PolicyStates& policy);
    // Main thread: frames leave the stage out until the worker's match matchId lands and its policy
    // is in the draw states, for kPolicyHoldMs at most.
    void holdStageUntilMatch(U64 matchId);
    void runPolicyEdits();
    void releasePolicyHoldIfDone();
    void releasePolicyHold();
    void wakeLoadsAskedBeforeStage();
    // A restore's load set stops standing in for loads the stage has yet to take.
    void forgetRestoredLoads();
    void publishDrawStats();

    void onBackgroundFrameDone();

    // Main thread: the render thread has switched to the renderer id for the switch numbered serial.
    void onRendererSwitched(const QString& id, U64 serial);

    // Main thread: a batch of payload loads was committed, leaving the stage at loadEditCount.
    void onStageLoadsServiced(const SceneViewportRenderer::FrameBounds& bounds, U64 loadEditCount);
    // Whether a pick has to wait for a background frame, which this starts if it is due.
    bool pickMustWait();

    // Main thread: something that held the stage has let go; carries out the input that found it held.
    void serviceDeferredInput();

    // Runs fn with the engine's context current, on whichever thread that context lives.
    void runWithEngineContext(const std::function<void()>& fn);

    // Draws the composing status over what the framebuffer has, or the background if it has no scene.
    void paintComposingStatus();

    bool isComposing() const;

    // Main thread, outside a paint: Scene Default's switch to the renderer the prim now names.
    void followSceneRenderer();

    /**
     * @brief Publishes the delegate rendering and emits rendererChanged() if it changed. Never from
     * paintGL(): the render row it shows or hides resizes this widget.
     **/
    void publishActiveRenderer();

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEVIEWPORT_H
