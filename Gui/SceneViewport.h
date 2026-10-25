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
#include <QtCore/QString>

#include "Engine/EngineFwd.h"
#include "Engine/ScenePayload.h"

// Free of USD: the viewer tab includes it, and USD's headers need the full Python API. Everything
// that touches pxr lives in SceneViewport.cpp.

class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

NATRON_NAMESPACE_ENTER

/**
 * @brief Draws a scene with Storm (UsdImagingGLEngine on HgiGL) into a framebuffer of the GL
 * context current when its methods run, with a ground grid and an op's transform handles over it,
 * and picks prims at pixels.
 *
 * Holds the composed stage it shows, fetched from StageCache, for as long as it shows it: the
 * stage is drawn at every repaint, not once per evaluation. The engine lives in the context that
 * was current at the first render() and must be released with releaseGL() in that context.
 *
 * Main thread only, except prepareStage().
 **/
class SceneViewportRenderer {
public:
    enum DrawModeEnum {
        eDrawModeUnshaded = 0, // the prims' display colours, no lighting
        eDrawModeShaded, // lit by a light at the camera
        eDrawModeWireframe
    };

    // The camera that is not a prim of the stage: it orbits pivot at distance, azimuth and
    // elevation in degrees about the stage's up axis, with a vertical field of view of fovY degrees.
    struct FreeCamera {
        double pivot[3] = { 0., 0., 0. };
        double distance = 10.;
        double azimuth = 0.;
        double elevation = 0.;
        double fovY = 45.;
    };

    SceneViewportRenderer();

    ~SceneViewportRenderer();

    /**
     * @brief Composes payload's stage, through StageCache, on the calling thread, so the main
     * thread's setScene() finds it composed. MT-safe.
     **/
    static void prepareStage(const ScenePayload& payload);

    /**
     * @brief Shows payload at the project frame (a project running at framesPerSecond). The free
     * camera is framed on the scene the first time a scene is shown, and kept for later scenes.
     * Returns false with *error set if USD is unusable or could not compose the stage, and then
     * shows nothing.
     **/
    bool setScene(const ScenePayload& payload, double frame, double framesPerSecond, std::string* error);

    void setFrame(double frame, double framesPerSecond);

    void clearScene();

    bool hasScene() const;

    // The stage timecode shown, the project frame mapped by seconds.
    double getTimeCode() const;

    void setDrawMode(DrawModeEnum mode);

    DrawModeEnum getDrawMode() const;

    // The ground grid, drawn over the scene by default.
    void setShowGrid(bool show);

    bool getShowGrid() const;

    void setFreeCamera(const FreeCamera& camera);

    const FreeCamera& getFreeCamera() const;

    // Frames the free camera on the bounds of the scene at the shown time, and looks through it.
    void frameScene();

    // Turns the free camera by degrees.
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

    // The highlighted prim, empty for none.
    void setSelectedPrim(const std::string& path);

    const std::string& getSelectedPrim() const;

    // The values of an op whose handles the viewer draws over the scene: a translate, rotate
    // (degrees, in rotateOrder, a SceneRotateOrderEnum) and pivot, authored as opName on primPath.
    struct TransformHandles {
        std::string primPath;
        std::string opName;
        double translate[3] = { 0., 0., 0. };
        double rotate[3] = { 0., 0., 0. };
        double pivot[3] = { 0., 0., 0. };
        int rotateOrder = 0;
    };

    /**
     * @brief Draws the handles of an op over the scene, at the shown time, until
     * clearTransformHandles(). Nothing is drawn while the shown stage has no such op on the prim.
     **/
    void setTransformHandles(const TransformHandles& handles);

    void clearTransformHandles();

    // Whether the shown stage has the op of the handles set, so that they are drawn.
    bool hasTransformHandles() const;

    // The handle (a SceneGizmo handle) under the pixel (x, y) of a width x height view, or -1.
    int hitTestTransformHandles(double x, double y, int width, int height) const;

    // The handle drawn highlighted, -1 for none.
    void setHighlightedTransformHandle(int handle);

    int getHighlightedTransformHandle() const;

    // A pixel on handle, to aim at. False if the handle is not drawn.
    bool getTransformHandlePixel(int handle, int width, int height, double* x, double* y) const;

    /**
     * @brief Starts dragging handle from the pixel (x, y): later changes are measured against the
     * handles as they are now. False if the handle is not drawn.
     **/
    bool beginTransformDrag(int handle, double x, double y, int width, int height);

    /**
     * @brief The change a drag to the pixel (x, y) makes since beginTransformDrag(): the amount
     * added to the translate or rotate value of the handle's axis, or the factor its scale value
     * is multiplied by. False if the drag says nothing, as for a ring seen edge-on.
     **/
    bool getTransformDragChange(double x, double y, int width, int height, double* change) const;

    void endTransformDrag();

    /**
     * @brief Draws into the framebuffer fbo of the current context, width x height pixels, and
     * leaves the GL state as it found it. Clears to the background when there is no scene.
     * Returns false if Storm has not converged yet and another render would refine the image.
     **/
    bool render(unsigned int fbo, int width, int height);

    /**
     * @brief The path of the prim drawn nearest to the pixel (x, y), counted from the top left of
     * a width x height view, or empty if none is. Needs the context of render() current.
     **/
    std::string pick(int x, int y, int width, int height);

    // Destroys the engine. Needs the context of render() current.
    void releaseGL();

    // The background colour render() clears to.
    static void getBackgroundColor(float rgb[3]);

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

/**
 * @brief The viewer tab's view of a scene input: SceneViewportRenderer in a QOpenGLWidget, with
 * the camera driven from the mouse, prim picking, and a context menu for the draw mode and the
 * camera to look through, and transform handles that drag a node's knobs. Shows an error message
 * in place of the scene when there is one.
 **/
class SceneViewport
    : public QOpenGLWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit SceneViewport(QWidget* parent = nullptr);

    ~SceneViewport() override;

    void setScene(const ScenePayload& payload, double frame, double framesPerSecond);

    void setFrame(double frame, double framesPerSecond);

    // Shows message instead of a scene.
    void setError(const QString& message);

    void clearScene();

    const QString& getError() const;

    SceneViewportRenderer& getRenderer();

    void setDrawMode(SceneViewportRenderer::DrawModeEnum mode);

    void setLookThroughCamera(const QString& path);

    void frameScene();

    // Picks the prim drawn at pos (widget coordinates) and selects it, or clears the selection.
    QString pickPrimAt(const QPoint& pos);

    void setSelectedPrim(const QString& path);

    QString getSelectedPrim() const;

    /**
     * @brief Sets the function that names the node whose transform handles are drawn: a
     * USDTransform with its settings panel open, or null for none. Dragging a handle sets the
     * node's Translate, Rotate or Scale, one undoable edit per drag.
     **/
    void setTransformNodeProvider(const std::function<NodePtr()>& provider);

    // Reads the handles of the provided node's op from its knobs, at the shown frame.
    void refreshTransformHandles();

Q_SIGNALS:

    void selectedPrimChanged(const QString& path);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    // Mirrors the error and whether a scene is shown into the natronSceneError and
    // natronSceneShown properties.
    void publishState();

    // Starts dragging the transform handle at pos (widget coordinates). False if none is there.
    bool beginHandleDrag(const QPoint& pos);

    void dragHandle(const QPoint& pos);

    void endHandleDrag();

    void highlightHandleAt(const QPoint& pos);

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEVIEWPORT_H
