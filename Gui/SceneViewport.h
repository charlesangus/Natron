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

#include "Engine/EngineFwd.h"
#include "Engine/ScenePayload.h"

// Free of USD: the viewer tab includes it, and USD's headers need the full Python API. Everything
// that touches pxr lives in SceneViewport.cpp.

class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

NATRON_NAMESPACE_ENTER

class UndoHistory;
class SceneTransformTarget;
struct SceneTransformDrag;

/**
 * @brief Draws a scene with Storm (UsdImagingGLEngine on HgiGL) into a framebuffer of the GL
 * context current when its methods run, with a ground grid and an op's transform handles over it,
 * and picks prims at pixels.
 *
 * Owns one long-lived stage (SceneStage) for its whole life and syncs each new scene into it, so
 * an edit upstream reaches the engine as the stage's change notices, which it applies to the
 * changed prims alone. The engine is made
 * once per GL context, at the first render() in it, and must be released with releaseGL() in
 * that context: when the context goes away or the renderer does, never because the scene changed.
 *
 * The stage and the engine are guarded by one lock, the viewer lock: syncScene() holds it while
 * it edits the stage, and render(), pick(), frameScene() and getStageCameraPaths() while they read.
 * It also takes part in SceneWriteBarrier, so a layer reload keeps them all out.
 *
 * syncScene() may run on any thread; everything else is for the main thread.
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
    // light at the camera (headlight).
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
        eRenderStatusBusy // a sync holds the viewer lock: nothing was drawn
    };

    /**
     * @brief Syncs the renderer's stage to payload, under the viewer lock, on the calling thread.
     * Shows nothing for an empty payload. Returns false with *error set if USD is unusable.
     * MT-safe; does not move the camera. serial, when not 0, is the request the payload answers:
     * a request later than clearTransformPreviewAfter()'s clears the drag preview in the same
     * step, so the preview gives way to the scene that has the drag written.
     **/
    bool syncScene(const ScenePayload& payload, std::string* error, U64 serial = 0);

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

    // Whether the free camera has been framed on a scene, or placed, yet.
    bool isFramed() const;

    // How many engines this renderer has made: one per GL context it rendered in.
    U64 getEngineCount() const;

    // How many engines every renderer of the process has made.
    static U64 getEngineConstructionCount();

    // Whether the renderer holds an engine, which the next render() makes when it does not.
    bool hasEngine() const;

    // The stage timecode shown, the project frame mapped by seconds.
    double getTimeCode() const;

    // Whether the shown stage's up axis is Z rather than Y.
    bool isUpAxisZ() const;

    void setDrawMode(DrawModeEnum mode);

    DrawModeEnum getDrawMode() const;

    // The ground grid, drawn over the scene by default.
    void setShowGrid(bool show);

    bool getShowGrid() const;

    // The camera and light guides, drawn over the scene by default. The camera looked through
    // has none.
    void setShowGuides(bool show);

    bool getShowGuides() const;

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
     * of the scene's centre ahead of it. False only for a stage camera that cannot be read.
     **/
    bool getViewPose(int width, int height, CameraPose* pose) const;

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

    // The paths of the stage's UsdGeomCamera prims, in stage order.
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
    bool hasSelectionHandles() const;

    /**
     * @brief The handles drawn, as pixels to aim at in a width x height view, and the frame they
     * point along: origin then the three axes, 12 values. False when none are drawn.
     **/
    bool getDrawnTransformHandles(int width, int height, std::vector<int>* handles, std::vector<double>* pixels, std::vector<double>* frame) const;

    // The handle (a SceneGizmo handle) under the pixel (x, y) of a width x height view, or -1.
    int hitTestTransformHandles(double x, double y, int width, int height) const;

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
    bool beginTransformDrag(int handle, double x, double y, int width, int height);

    // Whether the drag under way moves the selection, rather than an op's values.
    bool isSelectionTransformDrag() const;

    /**
     * @brief The change a drag to the pixel (x, y) makes since beginTransformDrag(): the amount
     * added to the translate or rotate value of the handle's axis, or the factor its scale value
     * is multiplied by. False if the drag says nothing, as for a ring seen edge-on.
     **/
    bool getTransformDragChange(double x, double y, int width, int height, double* change) const;

    /**
     * @brief Draws the prims the drag under way moves as a change of change would leave them,
     * without editing any node: a transient transform on the renderer's own layer. Returns
     * whether any prim is drawn moved.
     **/
    bool previewTransformDrag(double change);

    /**
     * @brief The selection drag under way, for a change, as a target writes it. False if the drag
     * under way does not move the selection.
     **/
    bool getTransformDrag(double change, double frame, SceneTransformDrag* drag) const;

    void endTransformDrag();

    // Drops the drag preview now.
    void clearTransformPreview();

    // Drops the drag preview when a scene requested after serial is synced.
    void clearTransformPreviewAfter(U64 serial);

    bool hasTransformPreview() const;

    /**
     * @brief Draws into the framebuffer fbo of the current context, width x height pixels, and
     * leaves the GL state as it found it. Clears to the background when there is no scene.
     * Returns false if Storm has not converged yet and another render would refine the image.
     **/
    bool render(unsigned int fbo, int width, int height);

    /**
     * @brief render(), unless a sync holds the viewer lock, in which case nothing is drawn and the
     * framebuffer keeps what it had. For paints, which must not wait for a sync.
     **/
    RenderStatusEnum renderIfIdle(unsigned int fbo, int width, int height);

    /**
     * @brief The path of the prim drawn nearest to the pixel (x, y), counted from the top left of
     * a width x height view, or empty if none is: a guide within a few pixels wins over a prim
     * behind it. Needs the context of render() current.
     **/
    std::string pick(int x, int y, int width, int height);

    /**
     * @brief The paths of the prims and guides drawn in the rectangle with corners (x0, y0) and
     * (x1, y1), pixels of a width x height view counted from the top left, sorted. Needs the context of
     * render() current.
     **/
    std::vector<std::string> pickRect(int x0, int y0, int x1, int y1, int width, int height);

    // Destroys the engine. Needs the context of render() current.
    void releaseGL();

    // The background colour render() clears to.
    static void getBackgroundColor(float rgb[3]);

private:
    bool renderLocked(unsigned int fbo, int width, int height);

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

    // Whether every setScene(), setError() and clearScene() so far has been synced.
    bool isSceneSynced() const;

    void setFrame(double frame, double framesPerSecond);

    // Shows message instead of a scene.
    void setError(const QString& message);

    void clearScene();

    const QString& getError() const;

    SceneViewportRenderer& getRenderer();

    void setDrawMode(SceneViewportRenderer::DrawModeEnum mode);

    void setShowGrid(bool show);

    void setShowGuides(bool show);

    // Whether prims of the proxy, render and guide purposes are drawn.
    void setShowPurposes(bool proxy, bool render, bool guide);

    void setLightingMode(SceneViewportRenderer::LightingModeEnum mode);

    void setLookThroughCamera(const QString& path);

    void frameScene();

    void frameSelection();

    // The prim drawn at pos (widget coordinates), empty for none.
    QString pickPrimAt(const QPoint& pos);

    // The prims drawn in rect (widget coordinates).
    QStringList pickPrimsIn(const QRect& rect);

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
     * again would mean syncing the whole scene again. Main thread; waits for a sync under way.
     **/
    void releaseEngine();

Q_SIGNALS:

    void selectionChanged(const QStringList& paths);

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
    // engines the renderer made and whether it holds one into the natronSceneError,
    // natronSceneShown, natronSceneSynced, natronSceneEngineCount and natronSceneEngineHeld
    // properties, and the display settings into natronSceneDrawMode, natronSceneShowGrid,
    // natronSceneShowGuides, natronScenePurposes and natronSceneLighting.
    void publishState();

    // Starts dragging the transform handle at pos (widget coordinates). False if none is there.
    bool beginHandleDrag(const QPoint& pos);

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

    // Starts moving the locked camera's node. False, with the HUD saying why, when it has none.
    bool beginCameraLock();

    // Applies a tumble or pan drag of delta pixels, or a dolly by dollyFactor, to the locked camera.
    void moveLockedCamera(int navigation, const QPoint& delta, double dollyFactor);

    // Writes the move to the camera's node as one undo step.
    void endCameraLock();

    void showContextMenu(const QPoint& globalPos);

    // A menu of the cameras at paths, under the pointer: the one picked is looked through, locked.
    void showLockCameraMenu(const std::vector<std::string>& paths);

    // Queues payload for the worker; the error shown once it is synced.
    void requestSync(const ScenePayload& payload, const QString& error);

    // Main thread: the worker synced request serial, with error set on failure.
    void onSynced(U64 serial, bool ok, const QString& error);

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEVIEWPORT_H
