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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "SceneViewport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <QActionGroup>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QUndoCommand>
#include <QWheelEvent>
#include <QWidgetAction>
#include <QtCore/QDebug>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/UndoGesture.h"

// See UsdIncludes.h: oneTBB, which these include too, declares members named emit().
#ifdef emit
#define NATRON_SCENEVIEWPORT_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hdx/pickTask.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_SCENEVIEWPORT_RESTORE_EMIT
#undef NATRON_SCENEVIEWPORT_RESTORE_EMIT
#define emit
#endif

#include "Gui/ActionShortcuts.h"
#include "Gui/GLContextLoss.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/Menu.h"
#include "Gui/SceneGizmo.h"
#include "Gui/UndoHistory.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// Storm clears to this, in scene-linear; it reaches the framebuffer through the sRGB transform.
const float kBackgroundLinear = 0.05f;
// The same grey once encoded to sRGB, for the clears and checks done outside Storm.
const float kBackgroundDisplay = 0.2478f;

// Storm may need more than one pass before an image is final.
const int kMaxRenderPasses = 8;

const double kMinElevation = -89.;
// The radius framed around a selection without extent, in scene units.
const double kDefaultFrameRadius = 1.;
const double kMaxElevation = 89.;

double
degreesToRadians(double d)
{
    return d * M_PI / 180.;
}

double
radiansToDegrees(double r)
{
    return r * 180. / M_PI;
}

/**
 * @brief Saves on construction, and restores on destruction, the GL state Storm (HgiGL and its
 * presentation into the target framebuffer) changes and the drawing around it relies on: the
 * attribute stacks, the bindings Storm leaves behind, and the capabilities the attribute stacks of
 * a compatibility context do not cover.
 **/
class StormGLStateGuard {
public:
    StormGLStateGuard()
        : _attribs(GL_ALL_ATTRIB_BITS)
    {
        glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &_drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &_readFramebuffer);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &_renderbuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &_program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &_vertexArray);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &_arrayBuffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &_pixelPackBuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &_pixelUnpackBuffer);
        glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &_uniformBuffer);
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &_storageBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &_activeTexture);
        for (int i = 0; i < kNumUnits; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &_texture2D[i]);
            glGetIntegerv(GL_SAMPLER_BINDING, &_sampler[i]);
        }
        glActiveTexture(_activeTexture);
        for (int i = 0; i < kNumCaps; ++i) {
            _capEnabled[i] = glIsEnabled(kCaps[i]);
        }
        if (GLAD_GL_VERSION_4_5) {
            glGetIntegerv(GL_CLIP_ORIGIN, &_clipOrigin);
            glGetIntegerv(GL_CLIP_DEPTH_MODE, &_clipDepthMode);
        }
    }

    ~StormGLStateGuard()
    {
        if (GLAD_GL_VERSION_4_5) {
            glClipControl(_clipOrigin, _clipDepthMode);
        }
        for (int i = 0; i < kNumCaps; ++i) {
            if (_capEnabled[i]) {
                glEnable(kCaps[i]);
            } else {
                glDisable(kCaps[i]);
            }
        }
        for (int i = 0; i < kNumUnits; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, _texture2D[i]);
            glBindSampler(i, _sampler[i]);
        }
        glActiveTexture(_activeTexture);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, _storageBuffer);
        glBindBuffer(GL_UNIFORM_BUFFER, _uniformBuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _pixelUnpackBuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, _pixelPackBuffer);
        glBindVertexArray(_vertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, _arrayBuffer);
        glUseProgram(_program);
        glBindRenderbuffer(GL_RENDERBUFFER, _renderbuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _readFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _drawFramebuffer);
        glPopClientAttrib();
        // _attribs pops the server attributes last.
    }

private:
    static const int kNumUnits = 8;
    static const int kNumCaps = 8;
    static const GLenum kCaps[kNumCaps];

    GLProtectAttrib _attribs;
    GLint _drawFramebuffer = 0;
    GLint _readFramebuffer = 0;
    GLint _renderbuffer = 0;
    GLint _program = 0;
    GLint _vertexArray = 0;
    GLint _arrayBuffer = 0;
    GLint _pixelPackBuffer = 0;
    GLint _pixelUnpackBuffer = 0;
    GLint _uniformBuffer = 0;
    GLint _storageBuffer = 0;
    GLint _activeTexture = GL_TEXTURE0;
    GLint _texture2D[kNumUnits] = {};
    GLint _sampler[kNumUnits] = {};
    GLboolean _capEnabled[kNumCaps] = {};
    GLint _clipOrigin = GL_LOWER_LEFT;
    GLint _clipDepthMode = GL_NEGATIVE_ONE_TO_ONE;
};

const GLenum StormGLStateGuard::kCaps[StormGLStateGuard::kNumCaps] = {
    GL_FRAMEBUFFER_SRGB, GL_PRIMITIVE_RESTART, GL_PRIMITIVE_RESTART_FIXED_INDEX, GL_PROGRAM_POINT_SIZE,
    GL_DEPTH_CLAMP, GL_TEXTURE_CUBE_MAP_SEAMLESS, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_RASTERIZER_DISCARD
};
} // anon namespace

std::atomic<U64> engineConstructionCount(0);

struct SceneViewportRenderer::Implementation {
    // The viewer lock: held while sync edits the stage, and while anything reads the stage or
    // touches the engine (whose stage scene index queues the stage's change notices without a
    // lock of its own, OpenUSD v26.08 usdImaging/stageSceneIndex.cpp).
    std::mutex stageMutex;
    // Created by the first sync and kept for the renderer's life: the engine binds the first
    // stage it renders for good (v26.08 usdImagingGL/engine.cpp, PrepareBatch populates once).
    std::unique_ptr<SceneStage> sceneStage;
    SceneWriteBarrier::ParticipantId participant = 0;

    // Written by sync on whichever thread runs it, read by the main thread.
    std::atomic<bool> shown;
    std::atomic<bool> zUp;
    std::atomic<double> metersPerUnit;
    std::atomic<double> timeCodesPerSecond;
    std::atomic<double> frame;
    std::atomic<double> framesPerSecond;

    // Main thread only.
    DrawModeEnum drawMode = eDrawModeShaded;
    bool showGrid = true;
    FreeCamera freeCamera;
    ClipPlanes clipPlanes;
    bool framedOnce = false;
    std::string lookThrough;
    // The transform the camera at lookThroughOverridePath is drawn with instead of its prim's.
    std::string lookThroughOverridePath;
    GfMatrix4d lookThroughOverride = GfMatrix4d(1.);
    // The selected prims, the current one last.
    std::vector<std::string> selected;
    std::unique_ptr<UsdImagingGLEngine> engine;
    U64 engineCount = 0;

    // The transform handles, if any, and the drag of one under way.
    bool hasHandles = false;
    TransformHandles handles;
    int highlightedHandle = SceneGizmo::kNoHandle;
    int dragHandle = SceneGizmo::kNoHandle;
    SceneGizmo::Frame dragFrame;
    GfVec2d dragPressPixel;

    Implementation()
        : shown(false)
        , zUp(false)
        , metersPerUnit(0.01)
        , timeCodesPerSecond(24.)
        , frame(0.)
        , framesPerSecond(24.)
    {
        freeCamera.azimuth = 35.;
        freeCamera.elevation = 25.;
    }

    // The stage shown, null when none is. Needs stageMutex.
    UsdStageRefPtr currentStage() const
    {
        return (shown && sceneStage) ? sceneStage->getStage() : UsdStageRefPtr();
    }

    // The project frame mapped by seconds, as SceneOps::frameToTimeCode.
    double timeCode() const
    {
        const double fps = framesPerSecond.load();

        return (fps > 0.) ? (frame.load() * timeCodesPerSecond.load() / fps) : frame.load();
    }

    GfVec3d worldUp() const
    {
        return zUp ? GfVec3d(0., 0., 1.) : GfVec3d(0., 1., 0.);
    }

    GfVec3d pivot() const
    {
        return GfVec3d(freeCamera.pivot[0], freeCamera.pivot[1], freeCamera.pivot[2]);
    }

    // From the pivot towards the free camera's eye, unit length.
    GfVec3d eyeDirection() const
    {
        const double az = degreesToRadians(freeCamera.azimuth);
        const double el = degreesToRadians(freeCamera.elevation);
        const double ce = std::cos(el);

        if (zUp) {
            return GfVec3d(ce * std::sin(az), -ce * std::cos(az), std::sin(el));
        }

        return GfVec3d(ce * std::sin(az), std::sin(el), ce * std::cos(az));
    }

    bool isOrthographic() const
    {
        return freeCamera.viewMode != eViewModePerspective;
    }

    // The free camera's frame: back points from the pivot to the eye, up is the top of the view.
    // An orthographic view looks straight along its axis, with Top and Bottom upright as seen
    // from the front.
    void viewBasis(GfVec3d* back,
                   GfVec3d* up) const
    {
        const GfVec3d u = worldUp();
        // The axis the front view looks back along: +Z when Y is up, -Y when Z is.
        const GfVec3d front = zUp ? GfVec3d(0., -1., 0.) : GfVec3d(0., 0., 1.);
        const GfVec3d x(1., 0., 0.);

        *up = u;
        switch (freeCamera.viewMode) {
        case eViewModePerspective:
            *back = eyeDirection();
            break;
        case eViewModeFront:
            *back = front;
            break;
        case eViewModeBack:
            *back = -front;
            break;
        case eViewModeRight:
            *back = x;
            break;
        case eViewModeLeft:
            *back = -x;
            break;
        case eViewModeTop:
            *back = u;
            *up = -front;
            break;
        case eViewModeBottom:
            *back = -u;
            *up = front;
            break;
        }
    }

    // The free camera's eye, before an orthographic view backs it off.
    GfVec3d freeEye() const
    {
        GfVec3d back, up;

        viewBasis(&back, &up);

        return pivot() + back * std::max(freeCamera.distance, 1e-9);
    }

    void freeClipRange(double* nearClip,
                       double* farClip) const
    {
        const double distance = std::max(freeCamera.distance, 1e-9);

        if (clipPlanes.automatic) {
            *nearClip = distance * 0.01;
            *farClip = distance * 1000.;
        } else {
            *nearClip = clipPlanes.nearClip;
            *farClip = clipPlanes.farClip;
        }
    }

    // How far an orthographic view backs its eye off, so that what lies behind the eye of the
    // perspective view is still drawn; its clip distances are measured from the eye before that.
    double orthographicBackOff() const
    {
        return std::max(freeCamera.distance, 1e-9) * 500.;
    }

    // Needs stageMutex.
    UsdGeomCamera lookThroughCamera() const
    {
        const UsdStageRefPtr stage = currentStage();
        if (!stage || lookThrough.empty() || !SdfPath::IsValidPathString(lookThrough)) {
            return UsdGeomCamera();
        }
        UsdGeomCamera camera(stage->GetPrimAtPath(SdfPath(lookThrough)));

        return camera;
    }

    bool hasLookThroughOverride() const
    {
        return !lookThrough.empty() && (lookThroughOverridePath == lookThrough);
    }

    // The camera looked through at the shown time, drawn from the override when there is one.
    // Needs stageMutex.
    GfCamera lookThroughGfCamera(const UsdGeomCamera& camera) const
    {
        GfCamera gfCamera = camera.GetCamera(UsdTimeCode(timeCode()));

        if (hasLookThroughOverride()) {
            gfCamera.SetTransform(lookThroughOverride);
        }

        return gfCamera;
    }

    // Needs stageMutex.
    GfFrustum computeFrustum(int width,
                             int height) const
    {
        const double aspect = double(std::max(width, 1)) / double(std::max(height, 1));

        if (UsdGeomCamera camera = lookThroughCamera()) {
            GfCamera gfCamera = lookThroughGfCamera(camera);
            // Keeps the camera's vertical aperture, as usdview does without a camera mask.
            CameraUtilConformWindow(&gfCamera, CameraUtilMatchVertically, aspect);

            return gfCamera.GetFrustum();
        }

        const double distance = std::max(freeCamera.distance, 1e-9);
        GfVec3d back, up;
        viewBasis(&back, &up);
        double nearClip, farClip;
        freeClipRange(&nearClip, &farClip);
        GfFrustum frustum;
        if (!isOrthographic()) {
            GfMatrix4d view;
            view.SetLookAt(pivot() + back * distance, pivot(), up);
            frustum.SetPositionAndRotationFromMatrix(view.GetInverse());
            frustum.SetPerspective(freeCamera.fovY, aspect, nearClip, farClip);

            return frustum;
        }
        const double backOff = orthographicBackOff();
        GfMatrix4d view;
        view.SetLookAt(pivot() + back * (distance + backOff), pivot(), up);
        frustum.SetPositionAndRotationFromMatrix(view.GetInverse());
        const double halfHeight = distance * std::tan(degreesToRadians(freeCamera.fovY) * 0.5);
        const double nearDepth = clipPlanes.automatic ? backOff * 1e-3 : backOff + nearClip;
        frustum.SetOrthographic(-halfHeight * aspect, halfHeight * aspect, -halfHeight, halfHeight, nearDepth, backOff + farClip);

        return frustum;
    }

    UsdImagingGLRenderParams renderParams() const
    {
        UsdImagingGLRenderParams params;

        params.frame = UsdTimeCode(timeCode());
        params.clearColor = GfVec4f(kBackgroundLinear, kBackgroundLinear, kBackgroundLinear, 1.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->sRGB;
        params.highlight = true;
        switch (drawMode) {
        case eDrawModeUnshaded:
            params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
            params.enableLighting = false;
            break;
        case eDrawModeShaded:
            params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
            params.enableLighting = true;
            break;
        case eDrawModeWireframe:
            params.drawMode = UsdImagingGLDrawMode::DRAW_WIREFRAME;
            params.enableLighting = false;
            break;
        }

        return params;
    }

    void applySelection()
    {
        if (!engine) {
            return;
        }
        SdfPathVector paths;
        for (const std::string& path : selected) {
            if (SdfPath::IsValidPathString(path)) {
                paths.push_back(SdfPath(path));
            }
        }
        if (paths.empty()) {
            engine->ClearSelected();
        } else {
            engine->SetSelected(paths);
        }
    }

    // Needs the GL context current and stageMutex. False if there is nothing to draw. The engine
    // is made once per GL context and kept through every edit of the stage: it hears of them
    // through the stage's notices and updates on its next render.
    bool ensureEngine()
    {
        if (!currentStage()) {
            return false;
        }
        if (engine) {
            return true;
        }
        UsdImagingGLEngine::Parameters parameters;
        engine.reset(new UsdImagingGLEngine(parameters));
        ++engineCount;
        ++engineConstructionCount;
        engine->SetSelectionColor(GfVec4f(1.f, 0.8f, 0.1f, 1.f));
        applySelection();

        return true;
    }

    // A light at the camera, as usdview's default camera light.
    void setHeadlight()
    {
        GlfSimpleLight light;

        light.SetIsCameraSpaceLight(true);
        light.SetPosition(GfVec4f(0.f, 0.f, 0.f, 1.f));
        light.SetAmbient(GfVec4f(0.f, 0.f, 0.f, 1.f));
        light.SetDiffuse(GfVec4f(1.f, 1.f, 1.f, 1.f));
        light.SetSpecular(GfVec4f(0.2f, 0.2f, 0.2f, 1.f));
        GlfSimpleMaterial material;
        material.SetAmbient(GfVec4f(0.2f, 0.2f, 0.2f, 1.f));
        material.SetSpecular(GfVec4f(0.1f, 0.1f, 0.1f, 1.f));
        material.SetShininess(32.);
        engine->SetLightingState(GlfSimpleLightVector(1, light), material, GfVec4f(0.01f, 0.01f, 0.01f, 1.f));
    }

    void setCamera(const GfFrustum& frustum,
                   int width,
                   int height,
                   unsigned int fbo)
    {
        engine->SetRenderBufferSize(GfVec2i(width, height));
        engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
        engine->SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());
        engine->SetPresentationOutput(HgiTokens->OpenGL, VtValue(uint32_t(fbo)));
        setHeadlight();
    }

    // Needs stageMutex: a camera looked through is a prim of the stage.
    SceneGizmo::View makeView(int width,
                              int height) const
    {
        const GfFrustum frustum = computeFrustum(width, height);

        return SceneGizmo::View(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
    }

    // Where the handles sit at the shown time. False if they are not drawn. Needs stageMutex.
    bool computeHandleFrame(SceneGizmo::Frame* frame) const
    {
        const UsdStageRefPtr stage = currentStage();
        if (!hasHandles || !stage || !SdfPath::IsValidPathString(handles.primPath)) {
            return false;
        }
        const SdfPath path(handles.primPath);
        if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
            return false;
        }
        GfMatrix4d space;
        if (!SceneGizmo::computeOpSpace(stage->GetPrimAtPath(path), handles.opName, timeCode(), &space)) {
            return false;
        }
        *frame = SceneGizmo::computeFrame(space,
                                          GfVec3d(handles.translate[0], handles.translate[1], handles.translate[2]),
                                          GfVec3d(handles.rotate[0], handles.rotate[1], handles.rotate[2]),
                                          GfVec3d(handles.pivot[0], handles.pivot[1], handles.pivot[2]),
                                          (SceneRotateOrderEnum)handles.rotateOrder);

        return true;
    }

    // The handles in X red, Y green and Z blue, the highlighted or dragged one yellow, drawn over
    // everything. Needs stageMutex.
    void drawTransformHandles(const GfFrustum& frustum,
                              int width,
                              int height) const
    {
        SceneGizmo::Frame frame;

        if (!computeHandleFrame(&frame)) {
            return;
        }
        const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
        const double length = SceneGizmo::getHandleLength(view, frame);
        if (!(length > 0.) || !std::isfinite(length)) {
            return;
        }
        const int active = (dragHandle != SceneGizmo::kNoHandle) ? dragHandle : highlightedHandle;

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeProjectionMatrix().GetArray());
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeViewMatrix().GetArray());

        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);

        static const float axisColours[3][3] = { { 0.9f, 0.25f, 0.25f }, { 0.3f, 0.8f, 0.3f }, { 0.3f, 0.45f, 0.95f } };
        std::vector<SceneGizmo::HandleShape> shapes = SceneGizmo::buildHandles(frame, length);
        // Rings under arrows under boxes, as hitTest() favours the boxes.
        std::stable_sort(shapes.begin(), shapes.end(), [](const SceneGizmo::HandleShape& a, const SceneGizmo::HandleShape& b) {
            static const int layer[3] = { 1, 0, 2 }; // translate, rotate, scale
            return layer[SceneGizmo::getHandleKind(a.handle)] < layer[SceneGizmo::getHandleKind(b.handle)];
        });
        for (const SceneGizmo::HandleShape& shape : shapes) {
            const float* colour = axisColours[SceneGizmo::getHandleAxis(shape.handle)];
            if (shape.handle == active) {
                glColor4f(1.f, 0.85f, 0.1f, 1.f);
            } else {
                glColor4f(colour[0], colour[1], colour[2], 1.f);
            }
            const SceneGizmo::HandleKindEnum kind = SceneGizmo::getHandleKind(shape.handle);
            if (kind == SceneGizmo::eHandleKindScale) {
                glBegin(GL_QUADS);
            } else {
                glLineWidth((kind == SceneGizmo::eHandleKindTranslate) ? 3.f : 2.f);
                glBegin(shape.closed ? GL_LINE_LOOP : GL_LINE_STRIP);
            }
            for (const GfVec3d& p : shape.points) {
                glVertex3d(p[0], p[1], p[2]);
            }
            glEnd();
        }
        glLineWidth(1.f);

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
    } // drawTransformHandles

    // A grid on the ground plane through the origin, with lines a power of ten metres apart, about
    // a tenth to half the camera's distance, and the two ground axes coloured. Depth-tested against
    // Storm's presented depth.
    void drawGrid(const GfFrustum& frustum) const
    {
        const double mpu = (metersPerUnit.load() > 0.) ? metersPerUnit.load() : 0.01;
        // An orthographic view's eye is backed far off: measure from where it would be.
        const GfVec3d eye = lookThroughCamera() ? frustum.GetPosition() : freeEye();
        const double distance = std::max(eye.GetLength(), freeCamera.distance);
        const double distanceMeters = std::max(distance * mpu, 1e-6);
        const double spacing = std::pow(10., std::floor(std::log10(distanceMeters * 0.5))) / mpu;
        const int numLines = 25;
        const double halfExtent = spacing * numLines;

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeProjectionMatrix().GetArray());
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeViewMatrix().GetArray());

        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(1.f);

        // Ground coordinates (a, b) to world: XZ for a Y-up stage, XY for a Z-up one.
        auto vertex = [this](double a, double b) {
            if (zUp) {
                glVertex3d(a, b, 0.);
            } else {
                glVertex3d(a, 0., b);
            }
        };

        glBegin(GL_LINES);
        for (int i = -numLines; i <= numLines; ++i) {
            const double t = i * spacing;
            if (i == 0) {
                continue;
            }
            glColor4f(0.5f, 0.5f, 0.5f, 0.5f);
            vertex(t, -halfExtent);
            vertex(t, halfExtent);
            vertex(-halfExtent, t);
            vertex(halfExtent, t);
        }
        // X in red; the other ground axis (Z for Y-up, Y for Z-up) in its usual colour.
        glColor4f(0.8f, 0.25f, 0.25f, 0.9f);
        vertex(-halfExtent, 0.);
        vertex(halfExtent, 0.);
        if (zUp) {
            glColor4f(0.3f, 0.75f, 0.3f, 0.9f);
        } else {
            glColor4f(0.3f, 0.4f, 0.9f, 0.9f);
        }
        vertex(0., -halfExtent);
        vertex(0., halfExtent);
        glEnd();

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
    }
};

SceneViewportRenderer::SceneViewportRenderer()
    : _imp(new Implementation)
{
    // A layer reload edits layers the stage composes: it waits until no paint, pick or sync is
    // reading the stage, and keeps them out until it is done.
    std::mutex* stageMutex = &_imp->stageMutex;
    _imp->participant = SceneWriteBarrier::registerParticipant([stageMutex]() { stageMutex->lock(); },
                                                               [stageMutex]() { stageMutex->unlock(); });
}

SceneViewportRenderer::~SceneViewportRenderer()
{
    SceneWriteBarrier::unregisterParticipant(_imp->participant);
    // The engine owns GL objects: whoever owns this must have called releaseGL() in its context.
    if (_imp->engine) {
        qDebug() << "SceneViewportRenderer destroyed without releaseGL(); leaking its Storm engine";
        (void)_imp->engine.release();
    }
}

bool
SceneViewportRenderer::syncScene(const ScenePayload& payload,
                                 std::string* error)
{
    std::string runtimeError;

    if (!SceneOps::checkUsdRuntime(&runtimeError)) {
        _imp->shown = false;
        if (error) {
            *error = runtimeError;
        }

        return false;
    }

    // Before the viewer lock, the order every reader takes them in.
    SceneWriteBarrier::ReadScope read;
    std::lock_guard<std::mutex> lock(_imp->stageMutex);
    if (!_imp->sceneStage) {
        _imp->sceneStage.reset(new SceneStage);
    }
    if (!_imp->sceneStage->isValid()) {
        _imp->shown = false;
        if (error) {
            *error = "OpenUSD could not open a stage for the scene.";
        }

        return false;
    }
    _imp->sceneStage->sync(payload);
    const SceneMetadata& metadata = payload.getMetadata();
    _imp->zUp = (metadata.upAxis == SceneMetadata::eUpAxisZ);
    _imp->metersPerUnit = metadata.metersPerUnit;
    _imp->timeCodesPerSecond = metadata.timeCodesPerSecond;
    _imp->shown = !payload.isEmpty();

    return true;
}

bool
SceneViewportRenderer::setScene(const ScenePayload& payload,
                                double frame,
                                double framesPerSecond,
                                std::string* error)
{
    setFrame(frame, framesPerSecond);
    if (!syncScene(payload, error)) {
        return false;
    }
    if (!_imp->framedOnce) {
        frameScene();
    }

    return true;
}

void
SceneViewportRenderer::setFrame(double frame,
                                double framesPerSecond)
{
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
}

void
SceneViewportRenderer::clearScene()
{
    // Keeps the stage and the engine: the next scene is synced into them.
    syncScene(ScenePayload(), nullptr);
}

bool
SceneViewportRenderer::hasScene() const
{
    return _imp->shown;
}

double
SceneViewportRenderer::getTimeCode() const
{
    return _imp->timeCode();
}

bool
SceneViewportRenderer::isFramed() const
{
    return _imp->framedOnce;
}

U64
SceneViewportRenderer::getEngineCount() const
{
    return _imp->engineCount;
}

bool
SceneViewportRenderer::hasEngine() const
{
    return bool(_imp->engine);
}

U64
SceneViewportRenderer::getEngineConstructionCount()
{
    return engineConstructionCount.load();
}

void
SceneViewportRenderer::setDrawMode(DrawModeEnum mode)
{
    _imp->drawMode = mode;
}

SceneViewportRenderer::DrawModeEnum
SceneViewportRenderer::getDrawMode() const
{
    return _imp->drawMode;
}

void
SceneViewportRenderer::setShowGrid(bool show)
{
    _imp->showGrid = show;
}

bool
SceneViewportRenderer::getShowGrid() const
{
    return _imp->showGrid;
}

void
SceneViewportRenderer::setFreeCamera(const FreeCamera& camera)
{
    _imp->freeCamera = camera;
    _imp->freeCamera.elevation = std::max(kMinElevation, std::min(kMaxElevation, camera.elevation));
    _imp->freeCamera.distance = std::max(camera.distance, 1e-9);
    _imp->framedOnce = true;
    _imp->lookThrough.clear();
}

const SceneViewportRenderer::FreeCamera&
SceneViewportRenderer::getFreeCamera() const
{
    return _imp->freeCamera;
}

void
SceneViewportRenderer::setViewMode(ViewModeEnum mode)
{
    _imp->lookThrough.clear();
    _imp->freeCamera.viewMode = mode;
    // Perspective (V) after an orthographic view starts from its angle.
    switch (mode) {
    case eViewModePerspective:
        break;
    case eViewModeFront:
        _imp->freeCamera.azimuth = 0.;
        _imp->freeCamera.elevation = 0.;
        break;
    case eViewModeBack:
        _imp->freeCamera.azimuth = 180.;
        _imp->freeCamera.elevation = 0.;
        break;
    case eViewModeRight:
        _imp->freeCamera.azimuth = 90.;
        _imp->freeCamera.elevation = 0.;
        break;
    case eViewModeLeft:
        _imp->freeCamera.azimuth = -90.;
        _imp->freeCamera.elevation = 0.;
        break;
    case eViewModeTop:
        _imp->freeCamera.azimuth = 0.;
        _imp->freeCamera.elevation = kMaxElevation;
        break;
    case eViewModeBottom:
        _imp->freeCamera.azimuth = 0.;
        _imp->freeCamera.elevation = kMinElevation;
        break;
    }
}

bool
SceneViewportRenderer::isUpAxisZ() const
{
    return _imp->zUp;
}

SceneViewportRenderer::ViewModeEnum
SceneViewportRenderer::getViewMode() const
{
    return _imp->freeCamera.viewMode;
}

void
SceneViewportRenderer::setClipPlanes(const ClipPlanes& planes)
{
    _imp->clipPlanes = planes;
    _imp->clipPlanes.nearClip = std::max(planes.nearClip, 1e-6);
    _imp->clipPlanes.farClip = std::max(planes.farClip, _imp->clipPlanes.nearClip * 1.0001);
}

const SceneViewportRenderer::ClipPlanes&
SceneViewportRenderer::getClipPlanes() const
{
    return _imp->clipPlanes;
}

void
SceneViewportRenderer::getFreeCameraClipRange(double* nearClip,
                                              double* farClip) const
{
    _imp->freeClipRange(nearClip, farClip);
}

namespace {
void
storeMatrix(const GfMatrix4d& matrix,
            double out[16])
{
    const double* values = matrix.GetArray();

    std::copy(values, values + 16, out);
}
} // anon namespace

bool
SceneViewportRenderer::getViewPose(int width,
                                   int height,
                                   CameraPose* pose) const
{
    const double aspect = double(std::max(width, 1)) / double(std::max(height, 1));

    pose->aspect = aspect;
    storeMatrix(GfMatrix4d(1.), pose->parentToWorld);
    if (!_imp->lookThrough.empty()) {
        std::lock_guard<std::mutex> lock(_imp->stageMutex);
        const UsdGeomCamera camera = _imp->lookThroughCamera();
        if (!camera) {
            return false;
        }
        const UsdTimeCode time(_imp->timeCode());
        GfCamera gfCamera = _imp->lookThroughGfCamera(camera);
        CameraUtilConformWindow(&gfCamera, CameraUtilMatchVertically, aspect);
        UsdGeomXformCache xforms(time);
        storeMatrix(_imp->hasLookThroughOverride() ? _imp->lookThroughOverride : xforms.GetLocalToWorldTransform(camera.GetPrim()), pose->cameraToWorld);
        storeMatrix(xforms.GetParentToWorldTransform(camera.GetPrim()), pose->parentToWorld);
        pose->orthographic = (gfCamera.GetProjection() == GfCamera::Orthographic);
        // Apertures are in tenths of a scene unit, as GfCamera::APERTURE_UNIT says.
        pose->viewHeight = pose->orthographic ? gfCamera.GetVerticalAperture() * GfCamera::APERTURE_UNIT
                                              : gfCamera.GetVerticalAperture() / std::max(double(gfCamera.GetFocalLength()), 1e-9);
        const GfRange1f clipping = gfCamera.GetClippingRange();
        pose->nearClip = clipping.GetMin();
        pose->farClip = clipping.GetMax();
        // Turn about the scene's centre, at its depth ahead of the camera.
        const GfMatrix4d cameraToWorld = gfCamera.GetTransform();
        const GfVec3d eye = cameraToWorld.ExtractTranslation();
        const GfVec3d forward = -cameraToWorld.TransformDir(GfVec3d(0., 0., 1.)).GetNormalized();
        double depth = 0.;
        if (const UsdStageRefPtr stage = _imp->currentStage()) {
            UsdGeomBBoxCache bboxes(time, { UsdGeomTokens->default_, UsdGeomTokens->proxy }, true);
            const GfRange3d range = bboxes.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
            if (!range.IsEmpty()) {
                depth = GfDot(range.GetMidpoint() - eye, forward);
                if (depth <= 1e-6) {
                    depth = range.GetSize().GetLength() * 0.5;
                }
            }
        }
        pose->pivotDepth = (depth > 1e-6) ? depth : 1.;

        return true;
    }
    const FreeCamera& freeCamera = _imp->freeCamera;
    const double distance = std::max(freeCamera.distance, 1e-9);
    GfVec3d back, up;
    _imp->viewBasis(&back, &up);
    GfMatrix4d view;
    view.SetLookAt(_imp->pivot() + back * distance, _imp->pivot(), up);
    storeMatrix(view.GetInverse(), pose->cameraToWorld);
    pose->orthographic = _imp->isOrthographic();
    const double tanHalf = std::tan(degreesToRadians(freeCamera.fovY) * 0.5);
    pose->viewHeight = pose->orthographic ? 2. * distance * tanHalf : 2. * tanHalf;
    _imp->freeClipRange(&pose->nearClip, &pose->farClip);
    pose->pivotDepth = distance;

    return true;
}

void
SceneViewportRenderer::frameScene()
{
    _imp->lookThrough.clear();
    GfRange3d range;
    {
        std::lock_guard<std::mutex> lock(_imp->stageMutex);
        const UsdStageRefPtr stage = _imp->currentStage();
        if (!stage) {
            return;
        }
        UsdGeomBBoxCache bboxes(UsdTimeCode(_imp->timeCode()), { UsdGeomTokens->default_, UsdGeomTokens->proxy }, true);
        range = bboxes.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
    }
    if (range.IsEmpty()) {
        return;
    }
    const GfVec3d centre = range.GetMidpoint();
    const double radius = std::max(range.GetSize().GetLength() * 0.5, 1e-6);
    _imp->freeCamera.pivot[0] = centre[0];
    _imp->freeCamera.pivot[1] = centre[1];
    _imp->freeCamera.pivot[2] = centre[2];
    _imp->freeCamera.distance = radius / std::sin(degreesToRadians(_imp->freeCamera.fovY) * 0.5) * 1.05;
    _imp->framedOnce = true;
}

void
SceneViewportRenderer::frameSelection()
{
    if (_imp->selected.empty()) {
        frameScene();

        return;
    }
    GfRange3d range;
    {
        std::lock_guard<std::mutex> lock(_imp->stageMutex);
        const UsdStageRefPtr stage = _imp->currentStage();
        if (!stage) {
            return;
        }
        const UsdTimeCode time(_imp->timeCode());
        UsdGeomBBoxCache bboxes(time, { UsdGeomTokens->default_, UsdGeomTokens->proxy }, true);
        UsdGeomXformCache xforms(time);
        for (const std::string& path : _imp->selected) {
            if (!SdfPath::IsValidPathString(path)) {
                continue;
            }
            const UsdPrim prim = stage->GetPrimAtPath(SdfPath(path));
            if (!prim) {
                continue;
            }
            const GfRange3d bound = bboxes.ComputeWorldBound(prim).ComputeAlignedRange();
            if (bound.IsEmpty()) {
                // A camera, light or empty transform has no extent: frame where it is.
                range.UnionWith(xforms.GetLocalToWorldTransform(prim).Transform(GfVec3d(0.)));
            } else {
                range.UnionWith(bound);
            }
        }
    }
    if (range.IsEmpty()) {
        return;
    }
    _imp->lookThrough.clear();
    const GfVec3d centre = range.GetMidpoint();
    double radius = range.GetSize().GetLength() * 0.5;
    if (radius < 1e-6) {
        radius = kDefaultFrameRadius;
    }
    _imp->freeCamera.pivot[0] = centre[0];
    _imp->freeCamera.pivot[1] = centre[1];
    _imp->freeCamera.pivot[2] = centre[2];
    _imp->freeCamera.distance = radius / std::sin(degreesToRadians(_imp->freeCamera.fovY) * 0.5) * 1.05;
    _imp->framedOnce = true;
}

void
SceneViewportRenderer::tumble(double azimuthDegrees,
                              double elevationDegrees)
{
    _imp->lookThrough.clear();
    // Tumbling an orthographic view leaves its axis, as in Nuke.
    _imp->freeCamera.viewMode = eViewModePerspective;
    _imp->freeCamera.azimuth = std::fmod(_imp->freeCamera.azimuth + azimuthDegrees, 360.);
    _imp->freeCamera.elevation = std::max(kMinElevation, std::min(kMaxElevation, _imp->freeCamera.elevation + elevationDegrees));
}

void
SceneViewportRenderer::pan(double dxPixels,
                           double dyPixels,
                           int viewHeight)
{
    _imp->lookThrough.clear();
    GfVec3d back, viewUp;
    _imp->viewBasis(&back, &viewUp);
    const GfVec3d right = GfCross(viewUp, back).GetNormalized();
    const GfVec3d up = GfCross(back, right).GetNormalized();
    const double unitsPerPixel = 2. * _imp->freeCamera.distance * std::tan(degreesToRadians(_imp->freeCamera.fovY) * 0.5) / std::max(viewHeight, 1);
    // The scene follows the mouse, so the pivot moves the other way.
    const GfVec3d pivot = _imp->pivot() - right * (dxPixels * unitsPerPixel) + up * (dyPixels * unitsPerPixel);

    _imp->freeCamera.pivot[0] = pivot[0];
    _imp->freeCamera.pivot[1] = pivot[1];
    _imp->freeCamera.pivot[2] = pivot[2];
}

void
SceneViewportRenderer::dolly(double factor)
{
    _imp->lookThrough.clear();
    if (factor > 0.) {
        _imp->freeCamera.distance = std::max(_imp->freeCamera.distance * factor, 1e-9);
    }
}

std::vector<std::string>
SceneViewportRenderer::getStageCameraPaths() const
{
    std::vector<std::string> paths;

    std::lock_guard<std::mutex> lock(_imp->stageMutex);
    const UsdStageRefPtr stage = _imp->currentStage();
    if (!stage) {
        return paths;
    }
    for (const UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<UsdGeomCamera>()) {
            paths.push_back(prim.GetPath().GetString());
        }
    }

    return paths;
}

void
SceneViewportRenderer::setLookThroughCamera(const std::string& path)
{
    _imp->lookThrough = path;
    _imp->lookThroughOverridePath.clear();
}

const std::string&
SceneViewportRenderer::getLookThroughCamera() const
{
    return _imp->lookThrough;
}

void
SceneViewportRenderer::setLookThroughTransform(const double cameraToWorld[16])
{
    _imp->lookThroughOverridePath = _imp->lookThrough;
    std::copy(cameraToWorld, cameraToWorld + 16, _imp->lookThroughOverride.data());
}

void
SceneViewportRenderer::clearLookThroughTransform()
{
    _imp->lookThroughOverridePath.clear();
}

bool
SceneViewportRenderer::hasLookThroughTransform() const
{
    return _imp->hasLookThroughOverride();
}

void
SceneViewportRenderer::setSelectedPrims(const std::vector<std::string>& paths)
{
    _imp->selected = paths;
    std::lock_guard<std::mutex> lock(_imp->stageMutex);
    _imp->applySelection();
}

const std::vector<std::string>&
SceneViewportRenderer::getSelectedPrims() const
{
    return _imp->selected;
}

void
SceneViewportRenderer::setTransformHandles(const TransformHandles& handles)
{
    _imp->hasHandles = true;
    _imp->handles = handles;
}

void
SceneViewportRenderer::clearTransformHandles()
{
    _imp->hasHandles = false;
    _imp->highlightedHandle = SceneGizmo::kNoHandle;
    _imp->dragHandle = SceneGizmo::kNoHandle;
}

bool
SceneViewportRenderer::hasTransformHandles() const
{
    SceneGizmo::Frame frame;
    std::lock_guard<std::mutex> lock(_imp->stageMutex);

    return _imp->computeHandleFrame(&frame);
}

int
SceneViewportRenderer::hitTestTransformHandles(double x,
                                               double y,
                                               int width,
                                               int height) const
{
    SceneGizmo::Frame frame;
    std::lock_guard<std::mutex> lock(_imp->stageMutex);

    if ((width <= 0) || (height <= 0) || !_imp->computeHandleFrame(&frame)) {
        return SceneGizmo::kNoHandle;
    }
    const SceneGizmo::View view = _imp->makeView(width, height);

    return SceneGizmo::hitTest(view, SceneGizmo::buildHandles(frame, SceneGizmo::getHandleLength(view, frame)), GfVec2d(x, y));
}

void
SceneViewportRenderer::setHighlightedTransformHandle(int handle)
{
    _imp->highlightedHandle = handle;
}

int
SceneViewportRenderer::getHighlightedTransformHandle() const
{
    return _imp->highlightedHandle;
}

bool
SceneViewportRenderer::getTransformHandlePixel(int handle,
                                               int width,
                                               int height,
                                               double* x,
                                               double* y) const
{
    SceneGizmo::Frame frame;
    std::lock_guard<std::mutex> lock(_imp->stageMutex);

    if ((width <= 0) || (height <= 0) || !_imp->computeHandleFrame(&frame)) {
        return false;
    }
    GfVec2d pixel;
    if (!SceneGizmo::getHandlePixel(_imp->makeView(width, height), frame, handle, &pixel)) {
        return false;
    }
    *x = pixel[0];
    *y = pixel[1];

    return true;
}

bool
SceneViewportRenderer::beginTransformDrag(int handle,
                                          double x,
                                          double y,
                                          int /*width*/,
                                          int /*height*/)
{
    SceneGizmo::Frame frame;
    std::lock_guard<std::mutex> lock(_imp->stageMutex);

    if ((handle < 0) || (handle >= SceneGizmo::kNumHandles) || !_imp->computeHandleFrame(&frame)) {
        return false;
    }
    _imp->dragHandle = handle;
    _imp->dragFrame = frame;
    _imp->dragPressPixel = GfVec2d(x, y);

    return true;
}

bool
SceneViewportRenderer::getTransformDragChange(double x,
                                              double y,
                                              int width,
                                              int height,
                                              double* change) const
{
    if ((_imp->dragHandle == SceneGizmo::kNoHandle) || (width <= 0) || (height <= 0)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_imp->stageMutex);

    return SceneGizmo::computeDragChange(_imp->makeView(width, height), _imp->dragFrame, _imp->dragHandle, _imp->dragPressPixel, GfVec2d(x, y), change);
}

void
SceneViewportRenderer::endTransformDrag()
{
    _imp->dragHandle = SceneGizmo::kNoHandle;
}

bool
SceneViewportRenderer::render(unsigned int fbo,
                              int width,
                              int height)
{
    std::lock_guard<std::mutex> lock(_imp->stageMutex);

    return renderLocked(fbo, width, height);
}

SceneViewportRenderer::RenderStatusEnum
SceneViewportRenderer::renderIfIdle(unsigned int fbo,
                                    int width,
                                    int height)
{
    std::unique_lock<std::mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        return eRenderStatusBusy;
    }

    return renderLocked(fbo, width, height) ? eRenderStatusDone : eRenderStatusRefining;
}

bool
SceneViewportRenderer::renderLocked(unsigned int fbo,
                                    int width,
                                    int height)
{
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded()) {
        return true;
    }
    StormGLStateGuard guard;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(kBackgroundDisplay, kBackgroundDisplay, kBackgroundDisplay, 1.f);
    glClearDepth(1.);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!_imp->ensureEngine()) {
        return true;
    }

    const UsdStageRefPtr stage = _imp->currentStage();
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    _imp->setCamera(frustum, width, height, fbo);
    const UsdImagingGLRenderParams params = _imp->renderParams();
    bool converged = false;
    for (int i = 0; (i < kMaxRenderPasses) && !converged; ++i) {
        _imp->engine->Render(stage->GetPseudoRoot(), params);
        converged = _imp->engine->IsConverged();
    }

    if (_imp->showGrid || _imp->hasHandles) {
        // Storm leaves its own program, vertex array and framebuffer bound.
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glUseProgram(0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_FRAMEBUFFER_SRGB);
        if (_imp->showGrid) {
            _imp->drawGrid(frustum);
        }
        _imp->drawTransformHandles(frustum, width, height);
    }

    return converged;
} // renderLocked

std::string
SceneViewportRenderer::pick(int x,
                            int y,
                            int width,
                            int height)
{
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded()) {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(_imp->stageMutex);
    StormGLStateGuard guard;

    if (!_imp->ensureEngine()) {
        return std::string();
    }
    const UsdStageRefPtr stage = _imp->currentStage();
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    // The pixel's centre in normalized device coordinates, y up, and a one-pixel window.
    const GfVec2d centre(2. * (x + 0.5) / width - 1., 1. - 2. * (y + 0.5) / height);
    const GfFrustum pickFrustum = frustum.ComputeNarrowedFrustum(centre, GfVec2d(1. / width, 1. / height));

    UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = HdxPickTokens->resolveNearestToCenter;
    UsdImagingGLEngine::IntersectionResultVector results;
    if (!_imp->engine->TestIntersection(pickParams, pickFrustum.ComputeViewMatrix(), pickFrustum.ComputeProjectionMatrix(),
                                        stage->GetPseudoRoot(), _imp->renderParams(), &results)
        || results.empty()) {
        return std::string();
    }

    return results.front().hitPrimPath.GetString();
}

std::vector<std::string>
SceneViewportRenderer::pickRect(int x0,
                                int y0,
                                int x1,
                                int y1,
                                int width,
                                int height)
{
    std::vector<std::string> paths;
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded()) {
        return paths;
    }
    const int left = std::max(0, std::min(x0, x1));
    const int right = std::min(width - 1, std::max(x0, x1));
    const int top = std::max(0, std::min(y0, y1));
    const int bottom = std::min(height - 1, std::max(y0, y1));
    if ((left > right) || (top > bottom)) {
        return paths;
    }
    std::lock_guard<std::mutex> lock(_imp->stageMutex);
    StormGLStateGuard guard;

    if (!_imp->ensureEngine()) {
        return paths;
    }
    const UsdStageRefPtr stage = _imp->currentStage();
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    // The rectangle's centre in normalized device coordinates, y up, and its size as a fraction
    // of the view, whole pixels included.
    const GfVec2d centre((left + right + 1.) / width - 1., 1. - (top + bottom + 1.) / height);
    const GfVec2d size(double(right - left + 1) / width, double(bottom - top + 1) / height);
    const GfFrustum pickFrustum = frustum.ComputeNarrowedFrustum(centre, size);

    UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = HdxPickTokens->resolveUnique;
    UsdImagingGLEngine::IntersectionResultVector results;
    if (!_imp->engine->TestIntersection(pickParams, pickFrustum.ComputeViewMatrix(), pickFrustum.ComputeProjectionMatrix(),
                                        stage->GetPseudoRoot(), _imp->renderParams(), &results)) {
        return paths;
    }
    for (const UsdImagingGLEngine::IntersectionResult& result : results) {
        const std::string path = result.hitPrimPath.GetString();
        if (!path.empty() && (std::find(paths.begin(), paths.end(), path) == paths.end())) {
            paths.push_back(path);
        }
    }
    // Hits come in hash order; sorted, a marquee's selection does not depend on it.
    std::sort(paths.begin(), paths.end());

    return paths;
}

void
SceneViewportRenderer::releaseGL()
{
    std::lock_guard<std::mutex> lock(_imp->stageMutex);

    _imp->engine.reset();
}

void
SceneViewportRenderer::getBackgroundColor(float rgb[3])
{
    rgb[0] = rgb[1] = rgb[2] = kBackgroundDisplay;
}

// ---------------------------------------------------------------------------

NATRON_NAMESPACE_ANONYMOUS_ENTER

// The application's bindings, or null in a process without the GUI application manager.
GuiApplicationManager*
guiManager()
{
    return dynamic_cast<GuiApplicationManager*>(AppManager::instance());
}

bool
matchesMouseBinding(const char* actionID,
                    Qt::KeyboardModifiers modifiers,
                    Qt::MouseButton button)
{
    if (GuiApplicationManager* manager = guiManager()) {
        return manager->matchesMouseShortcut(kShortcutGroupViewer3D, actionID, modifiers, button);
    }
    // Without the application, the default preset's bindings.
    MouseAction action;
    if (std::string(actionID) == kShortcutIDMouseViewer3DDollyWheel) {
        action.gesture = eMouseGestureWheel;
    }
    std::vector<MouseBinding> bindings;
    getViewer3DNavigationPresetBindings((int)Settings::eViewer3DNavigationNuke, QString::fromUtf8(actionID), &bindings);
    action.setBindings(bindings);

    return action.matches(modifiers, button);
}

// The 3D viewer's default key bindings, for a process without the application.
struct DefaultKeyBinding {
    const char* actionID;
    Qt::KeyboardModifiers modifiers;
    Qt::Key key;
};

const DefaultKeyBinding kDefaultKeyBindings[] = {
    { kShortcutIDActionViewer3DFrameSelection, Qt::NoModifier, Qt::Key_F },
    { kShortcutIDActionViewer3DFrameAll, Qt::NoModifier, Qt::Key_A },
    { kShortcutIDActionViewer3DPerspective, Qt::NoModifier, Qt::Key_V },
    { kShortcutIDActionViewer3DFront, Qt::NoModifier, Qt::Key_Z },
    { kShortcutIDActionViewer3DBack, Qt::ShiftModifier, Qt::Key_Z },
    { kShortcutIDActionViewer3DRight, Qt::NoModifier, Qt::Key_X },
    { kShortcutIDActionViewer3DLeft, Qt::ShiftModifier, Qt::Key_X },
    { kShortcutIDActionViewer3DTop, Qt::NoModifier, Qt::Key_C },
    { kShortcutIDActionViewer3DBottom, Qt::ShiftModifier, Qt::Key_C },
    { kShortcutIDActionViewer3DLockView, Qt::ControlModifier, Qt::Key_L },
};

const DefaultKeyBinding*
defaultKeyBinding(const char* actionID)
{
    for (const DefaultKeyBinding& binding : kDefaultKeyBindings) {
        if (std::string(binding.actionID) == actionID) {
            return &binding;
        }
    }

    return nullptr;
}

bool
matchesKeyBinding(const char* actionID,
                  Qt::KeyboardModifiers modifiers,
                  int key)
{
    if (GuiApplicationManager* manager = guiManager()) {
        return manager->matchesKeybind(kShortcutGroupViewer3D, actionID, modifiers, key);
    }
    const DefaultKeyBinding* binding = defaultKeyBinding(actionID);
    const Qt::KeyboardModifiers relevant = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;

    return binding && (binding->key == key) && ((modifiers & relevant) == binding->modifiers);
}

// Whether a mouse binding of the 3D viewer needs Space held.
bool
bindingsUseSpace()
{
    GuiApplicationManager* manager = guiManager();
    if (!manager) {
        return false;
    }
    const AppShortcuts& shortcuts = manager->getAllShortcuts();
    AppShortcuts::const_iterator group = shortcuts.find(QString::fromUtf8(kShortcutGroupViewer3D));
    if (group == shortcuts.end()) {
        return false;
    }
    for (GroupShortcuts::const_iterator it = group->second.begin(); it != group->second.end(); ++it) {
        if (!dynamic_cast<const MouseAction*>(it->second)) {
            continue;
        }
        for (std::list<Qt::KeyboardModifiers>::const_iterator m = it->second->modifiers.begin(); m != it->second->modifiers.end(); ++m) {
            if (*m & kShortcutModifierSpace) {
                return true;
            }
        }
    }

    return false;
}

QKeySequence
keySequenceOf(const char* actionID)
{
    if (GuiApplicationManager* manager = guiManager()) {
        std::list<QKeySequence> sequences = manager->getKeySequenceForAction(QString::fromUtf8(kShortcutGroupViewer3D), QString::fromUtf8(actionID));

        return sequences.empty() ? QKeySequence() : sequences.front();
    }
    const DefaultKeyBinding* binding = defaultKeyBinding(actionID);

    return binding ? QKeySequence(int(binding->modifiers) | int(binding->key)) : QKeySequence();
}

// The orthographic and perspective views, their key actions and menu labels.
struct ViewModeEntry {
    SceneViewportRenderer::ViewModeEnum mode;
    const char* actionID;
    const char* label;
};

const ViewModeEntry kViewModes[] = {
    { SceneViewportRenderer::eViewModePerspective, kShortcutIDActionViewer3DPerspective, kShortcutDescActionViewer3DPerspective },
    { SceneViewportRenderer::eViewModeFront, kShortcutIDActionViewer3DFront, kShortcutDescActionViewer3DFront },
    { SceneViewportRenderer::eViewModeBack, kShortcutIDActionViewer3DBack, kShortcutDescActionViewer3DBack },
    { SceneViewportRenderer::eViewModeRight, kShortcutIDActionViewer3DRight, kShortcutDescActionViewer3DRight },
    { SceneViewportRenderer::eViewModeLeft, kShortcutIDActionViewer3DLeft, kShortcutDescActionViewer3DLeft },
    { SceneViewportRenderer::eViewModeTop, kShortcutIDActionViewer3DTop, kShortcutDescActionViewer3DTop },
    { SceneViewportRenderer::eViewModeBottom, kShortcutIDActionViewer3DBottom, kShortcutDescActionViewer3DBottom },
};

// The view mode a key press asks for, or -1.
int
viewModeForKey(Qt::KeyboardModifiers modifiers,
               int key)
{
    for (const ViewModeEntry& entry : kViewModes) {
        if (matchesKeyBinding(entry.actionID, modifiers, key)) {
            return (int)entry.mode;
        }
    }

    return -1;
}

// The script name of a view mode, as the natronSceneViewMode property reports it.
const char*
viewModeName(SceneViewportRenderer::ViewModeEnum mode)
{
    switch (mode) {
    case SceneViewportRenderer::eViewModePerspective:
        return "perspective";
    case SceneViewportRenderer::eViewModeFront:
        return "front";
    case SceneViewportRenderer::eViewModeBack:
        return "back";
    case SceneViewportRenderer::eViewModeRight:
        return "right";
    case SceneViewportRenderer::eViewModeLeft:
        return "left";
    case SceneViewportRenderer::eViewModeTop:
        return "top";
    case SceneViewportRenderer::eViewModeBottom:
        return "bottom";
    }

    return "perspective";
}

QStringList
toStringList(const std::vector<std::string>& paths)
{
    QStringList ret;
    for (const std::string& path : paths) {
        ret.push_back(QString::fromStdString(path));
    }

    return ret;
}

GfMatrix4d
loadMatrix(const double values[16])
{
    GfMatrix4d matrix;

    std::copy(values, values + 16, matrix.data());

    return matrix;
}

// Sets a dimension of a locked camera's knob: at frame when it is animated, which keys it there.
void
setCameraKnobValue(const KnobDoublePtr& knob,
                   int dimension,
                   double frame,
                   bool animated,
                   double value)
{
    if (animated) {
        knob->setValueAtTime(frame, value, ViewSpec::all(), dimension, eValueChangedReasonUserEdited, nullptr);
    } else {
        knob->setValue(value, ViewSpec::all(), dimension, eValueChangedReasonUserEdited, nullptr);
    }
}

/**
 * @brief A move of a locked camera: the translate and rotate values its node had before and after
 * it, at a frame. The end of the move wrote the knobs before pushing it, so the first redo does nothing.
 * Undoing a dimension that was animated without a key at the frame removes the key it added.
 **/
class LockedCameraMoveCommand
    : public QUndoCommand {
public:
    struct Dimension {
        KnobDoubleWPtr knob;
        int dimension = 0;
        double oldValue = 0.;
        double newValue = 0.;
        bool animated = false;
        bool hadKey = false;
    };

    LockedCameraMoveCommand(const NodePtr& node,
                            double frame,
                            const std::vector<Dimension>& dimensions,
                            const QString& text)
        : QUndoCommand(text)
        , _node(node)
        , _frame(frame)
        , _dimensions(dimensions)
    {
    }

    void undo() override
    {
        apply(true);
    }

    void redo() override
    {
        if (_firstRedo) {
            _firstRedo = false;

            return;
        }
        apply(false);
    }

private:
    void apply(bool old)
    {
        NodePtr node = _node.lock();
        EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

        if (!effect) {
            return;
        }
        effect->beginChanges();
        for (const Dimension& d : _dimensions) {
            KnobDoublePtr knob = d.knob.lock();
            if (!knob) {
                continue;
            }
            if (old && d.animated && !d.hadKey) {
                // Public on the knob interface only.
                const KnobIPtr base = knob;
                base->deleteValueAtTime(eCurveChangeReasonInternal, _frame, ViewSpec::all(), d.dimension, false);
            } else {
                setCameraKnobValue(knob, d.dimension, _frame, d.animated, old ? d.oldValue : d.newValue);
            }
        }
        effect->endChanges();
    }

    NodeWPtr _node;
    double _frame;
    std::vector<Dimension> _dimensions;
    bool _firstRedo = true;
};
NATRON_NAMESPACE_ANONYMOUS_EXIT

struct SceneViewport::Implementation {
    SceneViewportRenderer renderer;
    QString error;
    bool glInitialized = false;

    // What a drag of the pressed button does.
    enum DragRoleEnum {
        eDragRoleNone,
        eDragRoleTumble,
        eDragRolePan,
        eDragRoleDolly,
        eDragRoleMarquee
    };

    // The mouse gesture under way: the button, where it was pressed, whether it has moved past
    // the drag threshold, what the drag does, and whether a release without a drag selects the
    // prim under the press (in selectionMode, which a marquee uses too) or opens the context menu.
    Qt::MouseButton button = Qt::NoButton;
    QPoint pressPos;
    QPoint lastPos;
    bool dragging = false;
    DragRoleEnum dragRole = eDragRoleNone;
    bool clickSelects = false;
    SelectionModeEnum selectionMode = eSelectionModeReplace;
    QString pressPrim;
    bool menuOnRelease = false;

    // Space is down, while a binding of the 3D viewer uses it as a modifier.
    bool spaceHeld = false;

    // The project frame shown.
    double frame = 0.;

    std::function<NodePtr()> transformNodeProvider;
    // The drag of a transform handle under way: its knob, the handle, the value the knob had at
    // the press, and the gesture that folds the drag's edits into one undo step.
    KnobDoubleWPtr handleKnob;
    int handle = SceneGizmo::kNoHandle;
    double handlePressValue = 0.;
    UndoGesture handleGesture;

    std::function<NodePtr()> viewedNodeProvider;
    std::function<NodePtr(const std::string&)> cameraNodeProvider;
    QPointer<UndoHistory> undoHistory;

    // Whether navigating through a stage camera moves its node, and why the last try could not.
    bool lockView = false;
    QString lockMessage;

    // A move of the locked camera under way. Its node's ops are taken to be the outermost of the
    // prim's, with the camera at world = inner * local * parentToWorld (row vectors), local being
    // the ops the knobs make; world is moved and local solved back from it. As the mouse moves,
    // the renderer draws the camera at world; the end of the move writes the knobs once, as one
    // undo step. Editing the knobs on every event would evaluate the graph and resync the stage
    // each time, and the view would only move as fast as that round trip.
    struct CameraLock {
        bool active = false;
        // The move found no node to edit: the rest of the gesture does nothing.
        bool refused = false;
        NodeWPtr node;
        KnobDoubleWPtr translate;
        KnobDoubleWPtr rotate;
        double frame = 0.;
        // Translate x, y, z then rotate x, y, z: before the move, now, whether the dimension
        // is animated and whether it had a key at frame.
        double oldValues[6] = { 0., 0., 0., 0., 0., 0. };
        double newValues[6] = { 0., 0., 0., 0., 0., 0. };
        bool animated[6] = { false, false, false, false, false, false };
        bool hadKey[6] = { false, false, false, false, false, false };
        GfVec3d scale = GfVec3d(1.);
        SceneRotateOrderEnum rotateOrder = eSceneRotateOrderXYZ;
        GfMatrix4d innerInverse = GfMatrix4d(1.);
        GfMatrix4d parentInverse = GfMatrix4d(1.);
        GfMatrix4d world = GfMatrix4d(1.);
        double pivotDepth = 1.;
        double viewHeight = 1.;
        bool orthographic = false;
    };
    CameraLock cameraLock;

    // After a move is written, the renderer keeps drawing the camera where the move left it until
    // a scene requested after the write is synced: the last request made before it.
    bool lockedMoveAwaitsSync = false;
    U64 lockedMoveSerial = 0;

    // Ends a run of wheel steps on the locked camera, once the wheel rests, as one move.
    QTimer* wheelMoveTimer = nullptr;

    USDEditTransform* getTransformNode(NodePtr* node) const
    {
        *node = transformNodeProvider ? transformNodeProvider() : NodePtr();
        if (!*node) {
            return nullptr;
        }

        return dynamic_cast<USDEditTransform*>((*node)->getEffectInstance().get());
    }

    // The device pixel of a widget position.
    static GfVec2d toPixel(const QWidget* widget,
                           const QPoint& pos)
    {
        const qreal dpr = widget->devicePixelRatio();

        return GfVec2d((pos.x() + 0.5) * dpr, (pos.y() + 0.5) * dpr);
    }

    // The worker syncing the renderer's stage, and the latest payload waiting for it. A newer
    // request replaces one the worker has not taken yet.
    std::thread worker;
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    bool quit = false;
    bool hasPending = false;
    ScenePayload pending;
    U64 pendingSerial = 0;

    // Main thread: the last request made, the last one synced, and the error the last request
    // shows once synced.
    U64 requestedSerial = 0;
    U64 syncedSerial = 0;
    QString requestedError;
};

SceneViewport::SceneViewport(QWidget* parent)
    : QOpenGLWidget(parent)
    , _imp(new Implementation)
{
    setFocusPolicy(Qt::ClickFocus);
    // Hovering highlights the transform handle under the pointer.
    setMouseTracking(true);
    setObjectName(QString::fromUtf8("SceneViewport"));
    // The selected prims, the current one last, for scripts.
    setProperty("natronSceneSelection", QStringList());
    // A paint skipped while the worker syncs leaves the framebuffer as it was. Assumed from Qt's
    // documentation of QOpenGLWidget::UpdateBehavior, which says PartialUpdate keeps the
    // framebuffer's content between paints; not checked against Qt's source.
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);

    _imp->wheelMoveTimer = new QTimer(this);
    _imp->wheelMoveTimer->setSingleShot(true);
    _imp->wheelMoveTimer->setInterval(300);
    QObject::connect(_imp->wheelMoveTimer, &QTimer::timeout, this, [this]() {
        if (_imp->button == Qt::NoButton) {
            endCameraLock();
        }
    });

    Implementation* imp = _imp.get();
    _imp->worker = std::thread([this, imp]() {
        for (;;) {
            ScenePayload payload;
            U64 serial = 0;
            {
                std::unique_lock<std::mutex> lock(imp->queueMutex);
                imp->queueChanged.wait(lock, [imp]() { return imp->quit || imp->hasPending; });
                if (imp->quit) {
                    return;
                }
                payload = imp->pending;
                imp->pending = ScenePayload();
                imp->hasPending = false;
                serial = imp->pendingSerial;
            }
            std::string error;
            const bool ok = imp->renderer.syncScene(payload, &error);
            const QString qerror = QString::fromStdString(error);
            // Dropped by Qt if the widget is gone by then.
            QMetaObject::invokeMethod(this, [this, serial, ok, qerror]() { onSynced(serial, ok, qerror); }, Qt::QueuedConnection);
        }
    });
    publishState();
}

SceneViewport::~SceneViewport()
{
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->quit = true;
    }
    _imp->queueChanged.notify_all();
    if (_imp->worker.joinable()) {
        _imp->worker.join();
    }
    GLContextLoss::releaseNow(this);
}

void
SceneViewport::requestSync(const ScenePayload& payload,
                           const QString& error)
{
    _imp->requestedError = error;
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->pending = payload;
        _imp->hasPending = true;
        _imp->pendingSerial = ++_imp->requestedSerial;
    }
    _imp->queueChanged.notify_one();
    publishState();
}

void
SceneViewport::onSynced(U64 serial,
                        bool ok,
                        const QString& error)
{
    _imp->syncedSerial = std::max(_imp->syncedSerial, serial);
    if (serial != _imp->requestedSerial) {
        // A newer request is on its way.
        publishState();

        return;
    }
    _imp->error = ok ? _imp->requestedError : error;
    if (_imp->lockedMoveAwaitsSync && !_imp->cameraLock.active && (serial > _imp->lockedMoveSerial)) {
        // The stage has the written move: the camera's prim is where the view shows it.
        _imp->lockedMoveAwaitsSync = false;
        _imp->renderer.clearLookThroughTransform();
    }
    if (ok && _imp->renderer.hasScene() && !_imp->renderer.isFramed()) {
        _imp->renderer.frameScene();
    }
    // With natronSceneLastPaintMs, lets a benchmark time an edit to the frame that shows it.
    setProperty("natronSceneSyncedAtMs", QVariant::fromValue<qlonglong>(QDateTime::currentMSecsSinceEpoch()));
    publishState();
    update();
}

bool
SceneViewport::isSceneSynced() const
{
    return _imp->syncedSerial == _imp->requestedSerial;
}

void
SceneViewport::setScene(const ScenePayload& payload,
                        double frame,
                        double framesPerSecond)
{
    _imp->frame = frame;
    _imp->renderer.setFrame(frame, framesPerSecond);
    requestSync(payload, QString());
}

void
SceneViewport::setFrame(double frame,
                        double framesPerSecond)
{
    if ((frame != _imp->frame) && !_imp->cameraLock.active) {
        // A written move holds at its own frame only; an animated camera is elsewhere at others.
        _imp->lockedMoveAwaitsSync = false;
        _imp->renderer.clearLookThroughTransform();
    }
    _imp->frame = frame;
    _imp->renderer.setFrame(frame, framesPerSecond);
    update();
}

void
SceneViewport::setError(const QString& message)
{
    // Shown at once; the stage lets go of the scene's layers once the worker syncs it empty. The
    // engine stays.
    _imp->error = message;
    requestSync(ScenePayload(), message);
    update();
}

void
SceneViewport::clearScene()
{
    _imp->error.clear();
    requestSync(ScenePayload(), QString());
    update();
}

// Lets GUI tests and scripts read the state through the widget.
void
SceneViewport::publishState()
{
    setProperty("natronSceneError", _imp->error);
    setProperty("natronSceneShown", _imp->renderer.hasScene());
    setProperty("natronSceneSynced", isSceneSynced());
    // How many scenes were handed to the worker: navigation alone must not add to it.
    setProperty("natronSceneSyncRequests", QVariant::fromValue<qulonglong>(_imp->requestedSerial));
    setProperty("natronSceneLookThrough", QString::fromStdString(_imp->renderer.getLookThroughCamera()));
    setProperty("natronSceneEngineCount", QVariant::fromValue<qulonglong>(_imp->renderer.getEngineCount()));
    setProperty("natronSceneEngineHeld", _imp->renderer.hasEngine());
    // The free camera's view (perspective, front, back, right, left, top or bottom), its clip
    // planes (automatic, near, far), whether the view is locked to the camera looked through, and
    // why the last move of a locked camera could not be made.
    setProperty("natronSceneViewMode", QString::fromUtf8(viewModeName(_imp->renderer.getViewMode())));
    const SceneViewportRenderer::ClipPlanes& clip = _imp->renderer.getClipPlanes();
    setProperty("natronSceneClip", QVariantList { clip.automatic, clip.nearClip, clip.farClip });
    setProperty("natronSceneLockView", _imp->lockView);
    setProperty("natronSceneLockMessage", _imp->lockMessage);
}

const QString&
SceneViewport::getError() const
{
    return _imp->error;
}

SceneViewportRenderer&
SceneViewport::getRenderer()
{
    return _imp->renderer;
}

void
SceneViewport::setDrawMode(SceneViewportRenderer::DrawModeEnum mode)
{
    _imp->renderer.setDrawMode(mode);
    update();
}

void
SceneViewport::setLookThroughCamera(const QString& path)
{
    if (_imp->cameraLock.active) {
        endCameraLock();
    }
    _imp->lockedMoveAwaitsSync = false;
    _imp->renderer.setLookThroughCamera(path.toStdString());
    if (path.isEmpty()) {
        // The free camera has nothing to lock to.
        _imp->lockView = false;
    }
    _imp->lockMessage.clear();
    publishState();
    update();
}

void
SceneViewport::frameScene()
{
    _imp->renderer.frameScene();
    update();
}

void
SceneViewport::frameSelection()
{
    _imp->renderer.frameSelection();
    update();
}

QString
SceneViewport::pickPrimAt(const QPoint& pos)
{
    std::string path;

    if (_imp->glInitialized && _imp->renderer.hasScene() && context()) {
        const qreal dpr = devicePixelRatio();
        makeCurrent();
        path = _imp->renderer.pick(int(pos.x() * dpr), int(pos.y() * dpr), int(width() * dpr), int(height() * dpr));
        doneCurrent();
    }

    return QString::fromStdString(path);
}

QStringList
SceneViewport::pickPrimsIn(const QRect& rect)
{
    std::vector<std::string> paths;

    if (_imp->glInitialized && _imp->renderer.hasScene() && context()) {
        const qreal dpr = devicePixelRatio();
        const QRect r = rect.normalized();
        makeCurrent();
        paths = _imp->renderer.pickRect(int(r.left() * dpr), int(r.top() * dpr), int((r.right() + 1) * dpr) - 1, int((r.bottom() + 1) * dpr) - 1,
                                        int(width() * dpr), int(height() * dpr));
        doneCurrent();
    }

    return toStringList(paths);
}

void
SceneViewport::select(const QStringList& paths,
                      SelectionModeEnum mode)
{
    QStringList selection = getSelection();

    switch (mode) {
    case eSelectionModeReplace:
        selection = paths;
        break;
    case eSelectionModeAdd:
        // An added prim already selected becomes the current one.
        for (const QString& path : paths) {
            selection.removeAll(path);
            selection.push_back(path);
        }
        break;
    case eSelectionModeToggle:
        for (const QString& path : paths) {
            if (selection.contains(path)) {
                selection.removeAll(path);
            } else {
                selection.push_back(path);
            }
        }
        break;
    }
    setSelection(selection);
}

void
SceneViewport::setSelection(const QStringList& paths)
{
    if (paths == getSelection()) {
        return;
    }
    std::vector<std::string> p;
    for (const QString& path : paths) {
        p.push_back(path.toStdString());
    }
    if (_imp->glInitialized && context()) {
        // The engine may exist, and selection changes reach its GL resources.
        makeCurrent();
        _imp->renderer.setSelectedPrims(p);
        doneCurrent();
    } else {
        _imp->renderer.setSelectedPrims(p);
    }
    setProperty("natronSceneSelection", paths);
    update();
    Q_EMIT selectionChanged(paths);
}

QStringList
SceneViewport::getSelection() const
{
    return toStringList(_imp->renderer.getSelectedPrims());
}

void
SceneViewport::setTransformNodeProvider(const std::function<NodePtr()>& provider)
{
    _imp->transformNodeProvider = provider;
    update();
}

void
SceneViewport::setViewedNodeProvider(const std::function<NodePtr()>& provider)
{
    _imp->viewedNodeProvider = provider;
}

void
SceneViewport::setCameraNodeProvider(const std::function<NodePtr(const std::string&)>& provider)
{
    _imp->cameraNodeProvider = provider;
}

void
SceneViewport::setUndoHistory(UndoHistory* history)
{
    _imp->undoHistory = history;
}

void
SceneViewport::setViewMode(SceneViewportRenderer::ViewModeEnum mode)
{
    _imp->renderer.setViewMode(mode);
    publishState();
    update();
}

void
SceneViewport::setClipPlanes(const SceneViewportRenderer::ClipPlanes& planes)
{
    _imp->renderer.setClipPlanes(planes);
    publishState();
    update();
}

void
SceneViewport::setLockView(bool locked)
{
    if (_imp->cameraLock.active) {
        endCameraLock();
    }
    _imp->lockView = locked;
    _imp->lockMessage.clear();
    publishState();
    update();
}

bool
SceneViewport::isLockView() const
{
    return _imp->lockView;
}

void
SceneViewport::lockViewToCamera()
{
    const std::string current = _imp->renderer.getLookThroughCamera();

    if (!current.empty()) {
        setLockView(true);

        return;
    }
    const std::vector<std::string> paths = _imp->renderer.getStageCameraPaths();
    if (paths.empty()) {
        _imp->lockView = false;
        _imp->lockMessage = tr("The scene has no camera to lock the view to");
        publishState();
        update();

        return;
    }
    if (paths.size() == 1) {
        lockViewToCamera(QString::fromStdString(paths.front()));

        return;
    }
    // Later: a key press or the context menu that asked may still be under way.
    QMetaObject::invokeMethod(this, [this, paths]() { showLockCameraMenu(paths); }, Qt::QueuedConnection);
}

void
SceneViewport::lockViewToCamera(const QString& path)
{
    setLookThroughCamera(path);
    setLockView(!path.isEmpty());
}

void
SceneViewport::showLockCameraMenu(const std::vector<std::string>& paths)
{
    clearGesture();
    Menu menu(this);
    menu.setObjectName(QString::fromUtf8("SceneLockCameraMenu"));
    for (const std::string& path : paths) {
        const QString qpath = QString::fromStdString(path);
        QAction* action = menu.addAction(qpath);
        QObject::connect(action, &QAction::triggered, this, [this, qpath]() {
            lockViewToCamera(qpath);
        });
    }
    QPoint pos = mapFromGlobal(QCursor::pos());
    if (!rect().contains(pos)) {
        pos = rect().center();
    }
    menu.exec(mapToGlobal(pos));
    clearGesture();
}

bool
SceneViewport::navigatesLockedCamera() const
{
    return _imp->lockView && !_imp->renderer.getLookThroughCamera().empty();
}

bool
SceneViewport::beginCameraLock()
{
    Implementation::CameraLock& lock = _imp->cameraLock;

    lock = Implementation::CameraLock();
    const std::string path = _imp->renderer.getLookThroughCamera();
    NodePtr node = _imp->cameraNodeProvider ? _imp->cameraNodeProvider(path) : NodePtr();
    KnobDoublePtr translate, rotate, scale, uniformScale;
    KnobChoicePtr rotateOrder;
    if (node) {
        translate = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamTranslate));
        rotate = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamRotate));
        scale = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamScale));
        uniformScale = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamUniformScale));
        rotateOrder = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kSceneParamRotateOrder));
    }
    const qreal dpr = devicePixelRatio();
    SceneViewportRenderer::CameraPose pose;
    const bool havePose = _imp->renderer.getViewPose(int(width() * dpr), int(height() * dpr), &pose);
    const double frame = _imp->frame;
    GfVec3d s(1.);
    if (scale && uniformScale) {
        const double uniform = uniformScale->getValueAtTime(frame, 0);
        s = GfVec3d(scale->getValueAtTime(frame, 0) * uniform, scale->getValueAtTime(frame, 1) * uniform, scale->getValueAtTime(frame, 2) * uniform);
    }
    const bool scaled = (std::abs(s[0]) > 1e-12) && (std::abs(s[1]) > 1e-12) && (std::abs(s[2]) > 1e-12);
    if (!translate || !rotate || !rotateOrder || !havePose || !scaled) {
        // The view stays locked to the camera and does not move, as Nuke's does for a camera
        // read from a file.
        lock.refused = true;
        _imp->lockMessage = tr("%1 has no Camera node to move: the view is locked to it").arg(QString::fromStdString(path));
        publishState();
        update();

        return false;
    }
    lock.active = true;
    lock.node = node;
    lock.translate = translate;
    lock.rotate = rotate;
    lock.frame = frame;
    for (int i = 0; i < 6; ++i) {
        const KnobDoublePtr& knob = (i < 3) ? translate : rotate;
        const int dimension = i % 3;
        lock.oldValues[i] = knob->getValueAtTime(frame, dimension);
        lock.newValues[i] = lock.oldValues[i];
        lock.animated[i] = knob->isAnimated(dimension);
        lock.hadKey[i] = lock.animated[i] && (knob->getKeyFrameIndex(ViewSpec::current(), dimension, frame) >= 0);
    }
    lock.scale = s;
    lock.rotateOrder = (SceneRotateOrderEnum)rotateOrder->getValue();
    const GfMatrix4d local = SceneNodeAuthoring::composeTransformMatrix(GfVec3d(lock.oldValues[0], lock.oldValues[1], lock.oldValues[2]),
                                                                        GfVec3d(lock.oldValues[3], lock.oldValues[4], lock.oldValues[5]),
                                                                        s, GfVec3d(0.), lock.rotateOrder);
    lock.world = loadMatrix(pose.cameraToWorld);
    lock.parentInverse = loadMatrix(pose.parentToWorld).GetInverse();
    // world = inner * local * parentToWorld: what the ops after the node's add.
    const GfMatrix4d inner = lock.world * lock.parentInverse * local.GetInverse();
    lock.innerInverse = inner.GetInverse();
    lock.pivotDepth = pose.pivotDepth;
    lock.viewHeight = pose.viewHeight;
    lock.orthographic = pose.orthographic;
    _imp->lockMessage.clear();
    publishState();

    return true;
} // SceneViewport::beginCameraLock

void
SceneViewport::moveLockedCamera(int navigation,
                                const QPoint& delta,
                                double dollyFactor)
{
    Implementation::CameraLock& lock = _imp->cameraLock;
    KnobDoublePtr translate = lock.translate.lock();
    KnobDoublePtr rotate = lock.rotate.lock();
    NodePtr node = lock.node.lock();

    if (!lock.active || !translate || !rotate || !node) {
        return;
    }
    const GfMatrix4d& world = lock.world;
    const GfVec3d right = world.TransformDir(GfVec3d(1., 0., 0.)).GetNormalized();
    const GfVec3d up = world.TransformDir(GfVec3d(0., 1., 0.)).GetNormalized();
    const GfVec3d back = world.TransformDir(GfVec3d(0., 0., 1.)).GetNormalized();
    const GfVec3d eye = world.ExtractTranslation();
    const double depth = lock.pivotDepth;
    GfMatrix4d move(1.);

    // The free camera's gestures, turned into a move of the camera in the world.
    switch (navigation) {
    case Implementation::eDragRoleTumble: {
        const GfVec3d worldUp = _imp->renderer.isUpAxisZ() ? GfVec3d(0., 0., 1.) : GfVec3d(0., 1., 0.);
        const double azimuth = -delta.x() * 0.4;
        // The eye's elevation above the pivot stays within the free camera's.
        const double elevation = radiansToDegrees(std::asin(std::max(-1., std::min(1., GfDot(back, worldUp)))));
        const double target = std::max(kMinElevation, std::min(kMaxElevation, elevation + delta.y() * 0.4));
        const GfVec3d centre = eye - back * depth;
        GfMatrix4d turn;
        turn.SetRotate(GfRotation(worldUp, azimuth));
        const GfVec3d turnedRight = turn.TransformDir(right).GetNormalized();
        GfMatrix4d tilt;
        tilt.SetRotate(GfRotation(turnedRight, -(target - elevation)));
        move = GfMatrix4d().SetTranslate(-centre) * turn * tilt * GfMatrix4d().SetTranslate(centre);
        break;
    }
    case Implementation::eDragRolePan: {
        const double unitsPerPixel = (lock.orthographic ? lock.viewHeight : lock.viewHeight * depth) / std::max(height(), 1);
        move.SetTranslate(-right * (delta.x() * unitsPerPixel) + up * (delta.y() * unitsPerPixel));
        break;
    }
    case Implementation::eDragRoleDolly:
        if (dollyFactor > 0.) {
            // An orthographic camera only moves: its aperture, not its distance, sets its zoom.
            move.SetTranslate(back * (depth * (dollyFactor - 1.)));
            lock.pivotDepth = std::max(depth * dollyFactor, 1e-9);
        }
        break;
    default:
        return;
    }
    lock.world = world * move;

    const GfMatrix4d local = lock.innerInverse * lock.world * lock.parentInverse;
    GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = local[i][j] / lock.scale[i];
        }
    }
    const GfVec3d t = local.ExtractTranslation();
    const GfVec3d r = SceneNodeAuthoring::decomposeRotationMatrix(rotation, lock.rotateOrder, GfVec3d(lock.newValues[3], lock.newValues[4], lock.newValues[5]));
    for (int i = 0; i < 6; ++i) {
        lock.newValues[i] = (i < 3) ? t[i] : r[i - 3];
    }
    double cameraToWorld[16];
    std::copy(lock.world.data(), lock.world.data() + 16, cameraToWorld);
    _imp->renderer.setLookThroughTransform(cameraToWorld);
} // SceneViewport::moveLockedCamera

void
SceneViewport::endCameraLock()
{
    Implementation::CameraLock lock = _imp->cameraLock;

    _imp->cameraLock = Implementation::CameraLock();
    _imp->wheelMoveTimer->stop();
    NodePtr node = lock.node.lock();
    if (!lock.active) {
        return;
    }
    if (!node) {
        if (!_imp->lockedMoveAwaitsSync) {
            _imp->renderer.clearLookThroughTransform();
        }

        return;
    }
    std::vector<LockedCameraMoveCommand::Dimension> dimensions;
    for (int i = 0; i < 6; ++i) {
        if (lock.newValues[i] == lock.oldValues[i]) {
            continue;
        }
        LockedCameraMoveCommand::Dimension d;
        d.knob = (i < 3) ? lock.translate : lock.rotate;
        d.dimension = i % 3;
        d.oldValue = lock.oldValues[i];
        d.newValue = lock.newValues[i];
        d.animated = lock.animated[i];
        d.hadKey = lock.hadKey[i];
        dimensions.push_back(d);
    }
    if (dimensions.empty()) {
        // Unless an earlier move still waits for its scene, the prim is where the view shows it.
        if (!_imp->lockedMoveAwaitsSync) {
            _imp->renderer.clearLookThroughTransform();
        }

        return;
    }
    // Before the write, which may request the scene that has it before returning.
    _imp->lockedMoveSerial = _imp->requestedSerial;
    _imp->lockedMoveAwaitsSync = true;
    EffectInstancePtr effect = node->getEffectInstance();
    if (effect) {
        effect->beginChanges();
    }
    for (const LockedCameraMoveCommand::Dimension& d : dimensions) {
        if (KnobDoublePtr knob = d.knob.lock()) {
            setCameraKnobValue(knob, d.dimension, lock.frame, d.animated, d.newValue);
        }
    }
    if (effect) {
        effect->endChanges();
    }
    LockedCameraMoveCommand* command = new LockedCameraMoveCommand(node, lock.frame, dimensions, tr("Move %1").arg(QString::fromUtf8(node->getLabel().c_str())));
    if (_imp->undoHistory) {
        _imp->undoHistory->push(command, UndoScope(UndoScope::holderForNode(node)));
    } else {
        // The edits are made: without a history there is nothing to record them in.
        delete command;
    }
} // SceneViewport::endCameraLock

NodePtr
SceneViewport::createCameraFromView()
{
    NodePtr viewed = _imp->viewedNodeProvider ? _imp->viewedNodeProvider() : NodePtr();
    AppInstancePtr app = viewed ? viewed->getApp() : AppInstancePtr();
    const qreal dpr = devicePixelRatio();
    SceneViewportRenderer::CameraPose pose;

    if (!app || !_imp->renderer.getViewPose(int(width() * dpr), int(height() * dpr), &pose)) {
        return NodePtr();
    }
    const GfMatrix4d cameraToWorld = loadMatrix(pose.cameraToWorld);
    // A stage camera may be scaled: the new one keeps its orientation alone.
    GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        const GfVec3d axis = GfVec3d(cameraToWorld[i][0], cameraToWorld[i][1], cameraToWorld[i][2]).GetNormalized();
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = axis[j];
        }
    }
    GfVec3d eye = cameraToWorld.ExtractTranslation();
    double nearClip = pose.nearClip;
    double farClip = pose.farClip;
    if (pose.orthographic && _imp->renderer.getLookThroughCamera().empty()) {
        // Backed off as the orthographic view is, so that it sees what lies behind its eye too.
        const double backOff = pose.pivotDepth * 500.;
        eye += GfVec3d(rotation[2][0], rotation[2][1], rotation[2][2]) * backOff;
        nearClip = _imp->renderer.getClipPlanes().automatic ? backOff * 1e-3 : backOff + nearClip;
        farClip += backOff;
    }
    const GfVec3d rotate = SceneNodeAuthoring::decomposeRotationMatrix(rotation, eSceneRotateOrderXYZ, GfVec3d(0.));

    CreateNodeArgs args(PLUGINID_NATRON_USDCAMERA, viewed->getGroup());
    double x = 0., y = 0.;
    viewed->getPosition(&x, &y);
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, x + 150., 0);
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, y, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.addParamDefaultValueN<double>(kSceneParamTranslate, std::vector<double> { eye[0], eye[1], eye[2] });
    args.addParamDefaultValueN<double>(kSceneParamRotate, std::vector<double> { rotate[0], rotate[1], rotate[2] });
    args.addParamDefaultValue<int>(kSceneParamRotateOrder, (int)eSceneRotateOrderXYZ);
    // The knobs are in millimetres; a film back 24 mm high, with the lens that gives the view's
    // field of view, or for an orthographic camera an aperture of the view's height: USD reads
    // apertures in tenths of a scene unit, which the camera node makes 100 x metersPerUnit mm.
    const double verticalAperture = pose.orthographic ? pose.viewHeight * 1000. * app->getProject()->getProjectMetersPerUnit() : 24.;
    args.addParamDefaultValue<int>(kUSDCameraParamProjection, pose.orthographic ? 1 : 0);
    args.addParamDefaultValue<double>(kUSDCameraParamVerticalAperture, verticalAperture);
    args.addParamDefaultValue<double>(kUSDCameraParamHorizontalAperture, verticalAperture * pose.aspect);
    if (!pose.orthographic) {
        args.addParamDefaultValue<double>(kUSDCameraParamFocalLength, verticalAperture / std::max(pose.viewHeight, 1e-9));
    }
    args.addParamDefaultValue<double>(kUSDCameraParamNear, nearClip);
    args.addParamDefaultValue<double>(kUSDCameraParamFar, farClip);

    return app->createNode(args);
} // SceneViewport::createCameraFromView

void
SceneViewport::refreshTransformHandles()
{
    NodePtr node;
    USDEditTransform* effect = _imp->getTransformNode(&node);
    std::string primPath, opName;

    if (!effect || !effect->getHandleTarget(&primPath, &opName)) {
        _imp->renderer.clearTransformHandles();

        return;
    }
    const SceneTransformKnobs& knobs = effect->getTransformKnobs();
    KnobDoublePtr translate = knobs.translate.lock();
    KnobDoublePtr rotate = knobs.rotate.lock();
    KnobChoicePtr rotateOrder = knobs.rotateOrder.lock();
    KnobDoublePtr pivot = effect->getPivotKnob();
    if (!translate || !rotate || !rotateOrder || !pivot) {
        _imp->renderer.clearTransformHandles();

        return;
    }
    SceneViewportRenderer::TransformHandles handles;
    handles.primPath = primPath;
    handles.opName = opName;
    for (int d = 0; d < 3; ++d) {
        handles.translate[d] = translate->getValueAtTime(_imp->frame, d);
        handles.rotate[d] = rotate->getValueAtTime(_imp->frame, d);
        handles.pivot[d] = pivot->getValueAtTime(_imp->frame, d);
    }
    handles.rotateOrder = rotateOrder->getValue();
    _imp->renderer.setTransformHandles(handles);
} // SceneViewport::refreshTransformHandles

void
SceneViewport::releaseEngine()
{
    // The engine's GL objects belong to this widget's context, and releaseGL() takes the viewer
    // lock, so the worker is not syncing into the stage the engine listens to while it goes.
    if (_imp->glInitialized && context()) {
        makeCurrent();
        _imp->renderer.releaseGL();
        doneCurrent();
    }
    publishState();
    // A hidden view makes its engine when it is next shown.
    update();
}

void
SceneViewport::initializeGL()
{
    _imp->glInitialized = true;
    // A new context (the pane was floated or docked) cannot use the old one's engine.
    GLContextLoss::watch(this, [this]() {
        _imp->renderer.releaseGL();
    });
}

void
SceneViewport::resizeGL(int /*w*/,
                        int /*h*/)
{
}

void
SceneViewport::paintGL()
{
    const qreal dpr = devicePixelRatio();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    refreshTransformHandles();
    {
        // The free camera this paint draws with, for scripts: pivot x, y, z, distance, azimuth,
        // elevation and vertical field of view.
        const SceneViewportRenderer::FreeCamera& camera = _imp->renderer.getFreeCamera();
        setProperty("natronSceneFreeCamera", QVariantList { camera.pivot[0], camera.pivot[1], camera.pivot[2], camera.distance, camera.azimuth, camera.elevation, camera.fovY });
        // A tumble turns an orthographic view to perspective.
        setProperty("natronSceneViewMode", QString::fromUtf8(viewModeName(camera.viewMode)));
        // Free navigation stops looking through a camera.
        setProperty("natronSceneLookThrough", QString::fromStdString(_imp->renderer.getLookThroughCamera()));
    }
    const SceneViewportRenderer::RenderStatusEnum status = _imp->renderer.renderIfIdle(defaultFramebufferObject(), w, h);
    if (status == SceneViewportRenderer::eRenderStatusBusy) {
        // The worker is syncing; it asks for a repaint when it is done.
        return;
    }
    const bool converged = (status == SceneViewportRenderer::eRenderStatusDone);
    // The engine is made at the first render in a context.
    setProperty("natronSceneEngineCount", QVariant::fromValue<qulonglong>(_imp->renderer.getEngineCount()));
    setProperty("natronSceneEngineHeld", _imp->renderer.hasEngine());

    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (!_imp->error.isEmpty()) {
        painter.setPen(QColor(230, 90, 80));
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, _imp->error);
    } else if (_imp->renderer.hasScene()) {
        QString mode;
        switch (_imp->renderer.getDrawMode()) {
        case SceneViewportRenderer::eDrawModeUnshaded:
            mode = tr("Unshaded");
            break;
        case SceneViewportRenderer::eDrawModeShaded:
            mode = tr("Shaded");
            break;
        case SceneViewportRenderer::eDrawModeWireframe:
            mode = tr("Wireframe");
            break;
        }
        const std::string& camera = _imp->renderer.getLookThroughCamera();
        QString view = tr("Free camera");
        if (!camera.empty()) {
            view = QString::fromStdString(camera);
        } else if (_imp->renderer.getViewMode() != SceneViewportRenderer::eViewModePerspective) {
            for (const ViewModeEntry& entry : kViewModes) {
                if (entry.mode == _imp->renderer.getViewMode()) {
                    view = tr(entry.label);
                }
            }
        }
        if (navigatesLockedCamera()) {
            view += tr(" (locked)");
        }
        QString hud = tr("%1 | %2 | time %3")
                          .arg(mode)
                          .arg(view)
                          .arg(_imp->renderer.getTimeCode(), 0, 'g', 6);
        const std::vector<std::string>& selected = _imp->renderer.getSelectedPrims();
        if (selected.size() == 1) {
            hud += tr(" | %1").arg(QString::fromStdString(selected.back()));
        } else if (selected.size() > 1) {
            hud += tr(" | %1 (+%2)").arg(QString::fromStdString(selected.back())).arg(selected.size() - 1);
        }
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignTop, hud);
        if (!_imp->lockMessage.isEmpty()) {
            painter.setPen(QColor(230, 160, 80));
            painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignBottom, _imp->lockMessage);
        }
    }
    if (_imp->dragging && (_imp->dragRole == Implementation::eDragRoleMarquee)) {
        painter.setPen(QPen(QColor(230, 230, 230), 1., Qt::DashLine));
        painter.setBrush(QColor(255, 255, 255, 30));
        painter.drawRect(QRect(_imp->pressPos, _imp->lastPos).normalized());
    }
    painter.end();
    if (converged) {
        // When the last finished frame was drawn, for benchmarks and scripts.
        setProperty("natronSceneLastPaintMs", QVariant::fromValue<qlonglong>(QDateTime::currentMSecsSinceEpoch()));
    } else {
        update();
    }
} // paintGL

void
SceneViewport::mousePressEvent(QMouseEvent* e)
{
    if ((e->button() == Qt::LeftButton) && (e->modifiers() == Qt::NoModifier) && !_imp->spaceHeld && beginHandleDrag(e->position().toPoint())) {
        e->accept();

        return;
    }
    clearGesture();
    const Qt::KeyboardModifiers modifiers = mouseBindingModifiers(e->modifiers(), _imp->spaceHeld);
    const Qt::MouseButton button = e->button();
    Implementation::DragRoleEnum role = Implementation::eDragRoleNone;
    if (matchesMouseBinding(kShortcutIDMouseViewer3DTumble, modifiers, button)) {
        role = Implementation::eDragRoleTumble;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DPan, modifiers, button)) {
        role = Implementation::eDragRolePan;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DDolly, modifiers, button)) {
        role = Implementation::eDragRoleDolly;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DMarquee, modifiers, button)) {
        role = Implementation::eDragRoleMarquee;
    }
    bool clickSelects = true;
    SelectionModeEnum mode = eSelectionModeReplace;
    if (matchesMouseBinding(kShortcutIDMouseViewer3DSelect, modifiers, button)) {
        mode = eSelectionModeReplace;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DAddToSelection, modifiers, button)) {
        mode = eSelectionModeAdd;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DToggleSelection, modifiers, button)) {
        mode = eSelectionModeToggle;
    } else {
        clickSelects = false;
    }
    // Adding or toggling by drag is a marquee of that mode, unless the drag is bound to another role.
    if (clickSelects && (mode != eSelectionModeReplace) && (role == Implementation::eDragRoleNone)) {
        role = Implementation::eDragRoleMarquee;
    }
    if ((role == Implementation::eDragRoleNone) && !clickSelects) {
        // A right press with no binding opens the context menu.
        QOpenGLWidget::mousePressEvent(e);

        return;
    }
    const QPoint pos = e->position().toPoint();
    // A marquee may start on a prim, as in Nuke: a drag selects by rectangle wherever it starts,
    // and a release without a drag selects what is under the press.
    if (clickSelects) {
        _imp->pressPrim = pickPrimAt(pos);
    }
    _imp->button = button;
    _imp->pressPos = pos;
    _imp->lastPos = pos;
    _imp->dragging = false;
    _imp->dragRole = role;
    _imp->clickSelects = clickSelects;
    _imp->selectionMode = mode;
    // A plain right press bound to a gesture defers the context menu to a release without a drag.
    _imp->menuOnRelease = (button == Qt::RightButton) && (modifiers == Qt::NoModifier);
    e->accept();
}

void
SceneViewport::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();

    if (_imp->handle != SceneGizmo::kNoHandle) {
        dragHandle(pos);
        e->accept();

        return;
    }
    // A release that went to a popup or another window never reaches this widget.
    if ((_imp->button != Qt::NoButton) && !(e->buttons() & _imp->button)) {
        clearGesture();
    }
    if (_imp->button == Qt::NoButton) {
        highlightHandleAt(pos);
        QOpenGLWidget::mouseMoveEvent(e);

        return;
    }
    if (!_imp->dragging && ((pos - _imp->pressPos).manhattanLength() < 4)) {
        return;
    }
    _imp->dragging = true;
    const QPoint delta = pos - _imp->lastPos;
    _imp->lastPos = pos;

    const bool navigates = (_imp->dragRole == Implementation::eDragRoleTumble) || (_imp->dragRole == Implementation::eDragRolePan) || (_imp->dragRole == Implementation::eDragRoleDolly);
    if (navigates && navigatesLockedCamera()) {
        if (!_imp->cameraLock.active && !_imp->cameraLock.refused) {
            beginCameraLock();
        }
        if (_imp->cameraLock.active) {
            moveLockedCamera(_imp->dragRole, delta, std::exp(-(delta.x() + delta.y()) * 0.01));
        }
        update();
        e->accept();

        return;
    }

    switch (_imp->dragRole) {
    case Implementation::eDragRoleTumble:
        _imp->renderer.tumble(-delta.x() * 0.4, delta.y() * 0.4);
        break;
    case Implementation::eDragRolePan:
        _imp->renderer.pan(delta.x(), delta.y(), height());
        break;
    case Implementation::eDragRoleDolly:
        _imp->renderer.dolly(std::exp(-(delta.x() + delta.y()) * 0.01));
        break;
    case Implementation::eDragRoleMarquee:
    case Implementation::eDragRoleNone:
        break;
    }
    update();
    e->accept();
}

void
SceneViewport::mouseReleaseEvent(QMouseEvent* e)
{
    if (_imp->handle != SceneGizmo::kNoHandle) {
        endHandleDrag();
        e->accept();

        return;
    }
    if ((_imp->button == Qt::NoButton) || (e->button() != _imp->button)) {
        e->accept();

        return;
    }
    const bool dragged = _imp->dragging;
    const Implementation::DragRoleEnum role = _imp->dragRole;
    const bool clickSelects = _imp->clickSelects;
    const SelectionModeEnum mode = _imp->selectionMode;
    const QString pressPrim = _imp->pressPrim;
    const bool menu = _imp->menuOnRelease;
    const QRect marquee = QRect(_imp->pressPos, e->position().toPoint()).normalized();
    clearGesture();

    if (dragged && (role == Implementation::eDragRoleMarquee)) {
        select(pickPrimsIn(marquee), mode);
    } else if (!dragged && clickSelects) {
        select(pressPrim.isEmpty() ? QStringList() : QStringList(pressPrim), mode);
    } else if (dragged && clickSelects && (role == Implementation::eDragRoleNone) && !pressPrim.isEmpty()) {
        // A drag that no binding gives a role is a click on the prim it started from.
        select(QStringList(pressPrim), mode);
    } else if (!dragged && menu) {
        showContextMenu(e->globalPosition().toPoint());
    }
    e->accept();
}

void
SceneViewport::clearGesture()
{
    const bool marquee = _imp->dragging && (_imp->dragRole == Implementation::eDragRoleMarquee);

    // The end of a drag, wherever its release went.
    endCameraLock();
    _imp->button = Qt::NoButton;
    _imp->dragging = false;
    _imp->dragRole = Implementation::eDragRoleNone;
    _imp->clickSelects = false;
    _imp->pressPrim.clear();
    _imp->menuOnRelease = false;
    if (marquee) {
        update();
    }
}

bool
SceneViewport::beginHandleDrag(const QPoint& pos)
{
    refreshTransformHandles();
    const qreal dpr = devicePixelRatio();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    const int handle = _imp->renderer.hitTestTransformHandles(pixel[0], pixel[1], w, h);
    if (handle == SceneGizmo::kNoHandle) {
        return false;
    }
    NodePtr node;
    USDEditTransform* effect = _imp->getTransformNode(&node);
    if (!effect) {
        return false;
    }
    const SceneTransformKnobs& knobs = effect->getTransformKnobs();
    KnobDoublePtr knob;
    switch (SceneGizmo::getHandleKind(handle)) {
    case SceneGizmo::eHandleKindTranslate:
        knob = knobs.translate.lock();
        break;
    case SceneGizmo::eHandleKindRotate:
        knob = knobs.rotate.lock();
        break;
    case SceneGizmo::eHandleKindScale:
        knob = knobs.scale.lock();
        break;
    }
    if (!knob || !_imp->renderer.beginTransformDrag(handle, pixel[0], pixel[1], w, h)) {
        return false;
    }
    _imp->handle = handle;
    _imp->handleKnob = knob;
    _imp->handlePressValue = knob->getValueAtTime(_imp->frame, SceneGizmo::getHandleAxis(handle));
    _imp->handleGesture.begin();
    update();

    return true;
} // SceneViewport::beginHandleDrag

void
SceneViewport::dragHandle(const QPoint& pos)
{
    KnobDoublePtr knob = _imp->handleKnob.lock();
    const qreal dpr = devicePixelRatio();
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    double change = 0.;

    if (!knob || !_imp->renderer.getTransformDragChange(pixel[0], pixel[1], int(width() * dpr), int(height() * dpr), &change)) {
        return;
    }
    const int axis = SceneGizmo::getHandleAxis(_imp->handle);
    const double value = (SceneGizmo::getHandleKind(_imp->handle) == SceneGizmo::eHandleKindScale) ? _imp->handlePressValue * change : _imp->handlePressValue + change;
    // Through the knob's GUI, as a slider drag is: the edits of one drag fold into one undo step,
    // and a later drag is a step of its own.
    KnobGuiPtr knobGui = std::dynamic_pointer_cast<KnobGui>(knob->getKnobGuiPointer());
    if (knobGui) {
        KnobUndoCommand<double>* command = new KnobUndoCommand<double>(knobGui, knob->getValueAtTime(_imp->frame, axis), value, axis, true);
        command->setGestureKey(_imp->handleGesture.key());
        knobGui->pushUndoCommand(command);
    } else {
        knob->setValue(value, ViewSpec::all(), axis, eValueChangedReasonUserEdited);
    }
    update();
}

void
SceneViewport::endHandleDrag()
{
    _imp->renderer.endTransformDrag();
    _imp->handle = SceneGizmo::kNoHandle;
    _imp->handleKnob.reset();
    _imp->handleGesture.end();
    update();
}

void
SceneViewport::highlightHandleAt(const QPoint& pos)
{
    const qreal dpr = devicePixelRatio();
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    const int handle = _imp->renderer.hitTestTransformHandles(pixel[0], pixel[1], int(width() * dpr), int(height() * dpr));

    if (handle != _imp->renderer.getHighlightedTransformHandle()) {
        _imp->renderer.setHighlightedTransformHandle(handle);
        update();
    }
}

void
SceneViewport::wheelEvent(QWheelEvent* e)
{
    // X11 turns a vertical wheel horizontal while Alt is held.
    const QPoint angle = e->angleDelta();
    const double steps = ((angle.y() != 0) ? angle.y() : angle.x()) / 120.;

    if ((steps == 0.) || !matchesMouseBinding(kShortcutIDMouseViewer3DDollyWheel, mouseBindingModifiers(e->modifiers(), _imp->spaceHeld), Qt::NoButton)) {
        e->ignore();

        return;
    }
    if (navigatesLockedCamera()) {
        // A run of wheel steps is one move, written once the wheel rests; a drag under way owns
        // the move and ends it itself.
        if (_imp->cameraLock.active || beginCameraLock()) {
            moveLockedCamera(Implementation::eDragRoleDolly, QPoint(), std::pow(0.85, steps));
            if (_imp->button == Qt::NoButton) {
                _imp->wheelMoveTimer->start();
            }
        }
        update();
        e->accept();

        return;
    }
    _imp->renderer.dolly(std::pow(0.85, steps));
    update();
    e->accept();
}

bool
SceneViewport::event(QEvent* e)
{
    // The 3D viewer's keys win over the window shortcuts of the viewer tab around it.
    if (e->type() == QEvent::ShortcutOverride) {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        if (matchesKeyBinding(kShortcutIDActionViewer3DFrameSelection, k->modifiers(), k->key()) || matchesKeyBinding(kShortcutIDActionViewer3DFrameAll, k->modifiers(), k->key())
            || matchesKeyBinding(kShortcutIDActionViewer3DLockView, k->modifiers(), k->key()) || (viewModeForKey(k->modifiers(), k->key()) >= 0)
            || ((k->key() == Qt::Key_Space) && bindingsUseSpace())) {
            e->accept();

            return true;
        }
    }

    return QOpenGLWidget::event(e);
}

void
SceneViewport::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Space) && bindingsUseSpace()) {
        _imp->spaceHeld = true;
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DFrameSelection, e->modifiers(), e->key())) {
        frameSelection();
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DFrameAll, e->modifiers(), e->key())) {
        frameScene();
        e->accept();

        return;
    }
    const int mode = viewModeForKey(e->modifiers(), e->key());
    if (mode >= 0) {
        setViewMode((SceneViewportRenderer::ViewModeEnum)mode);
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DLockView, e->modifiers(), e->key())) {
        if (navigatesLockedCamera()) {
            setLockView(false);
        } else {
            lockViewToCamera();
        }
        e->accept();

        return;
    }
    // Everything else is the viewer tab's.
    e->ignore();
}

void
SceneViewport::keyReleaseEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Space) && _imp->spaceHeld) {
        if (!e->isAutoRepeat()) {
            _imp->spaceHeld = false;
        }
        e->accept();

        return;
    }
    e->ignore();
}

void
SceneViewport::focusOutEvent(QFocusEvent* e)
{
    // A release or key release that goes elsewhere never comes back.
    if (_imp->handle != SceneGizmo::kNoHandle) {
        endHandleDrag();
    }
    clearGesture();
    _imp->spaceHeld = false;
    QOpenGLWidget::focusOutEvent(e);
}

void
SceneViewport::contextMenuEvent(QContextMenuEvent* e)
{
    // A right press bound to a gesture is that gesture's.
    if ((e->reason() == QContextMenuEvent::Mouse) && (_imp->button != Qt::NoButton)) {
        e->accept();

        return;
    }
    showContextMenu(e->globalPos());
    e->accept();
}

void
SceneViewport::showContextMenu(const QPoint& globalPos)
{
    // The menu takes the release, so no drag may outlive it.
    clearGesture();
    Menu menu(this);

    QActionGroup* modes = new QActionGroup(&menu);
    const struct
    {
        SceneViewportRenderer::DrawModeEnum mode;
        QString label;
    } modeItems[] = {
        { SceneViewportRenderer::eDrawModeUnshaded, tr("Unshaded") },
        { SceneViewportRenderer::eDrawModeShaded, tr("Shaded") },
        { SceneViewportRenderer::eDrawModeWireframe, tr("Wireframe") },
    };
    for (const auto& item : modeItems) {
        QAction* action = menu.addAction(item.label);
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getDrawMode() == item.mode);
        action->setActionGroup(modes);
        const SceneViewportRenderer::DrawModeEnum mode = item.mode;
        QObject::connect(action, &QAction::triggered, this, [this, mode]() {
            setDrawMode(mode);
        });
    }

    menu.addSeparator();
    QMenu* lookThrough = menu.addMenu(tr("Look Through"));
    QActionGroup* cameras = new QActionGroup(lookThrough);
    std::vector<std::string> paths = _imp->renderer.getStageCameraPaths();
    paths.insert(paths.begin(), std::string());
    for (const std::string& path : paths) {
        QAction* action = lookThrough->addAction(path.empty() ? tr("Free Camera") : QString::fromStdString(path));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getLookThroughCamera() == path);
        action->setActionGroup(cameras);
        const QString qpath = QString::fromStdString(path);
        QObject::connect(action, &QAction::triggered, this, [this, qpath]() {
            setLookThroughCamera(qpath);
        });
    }
    // Looking through a camera and locking to it in one pick.
    if (paths.size() > 1) {
        lookThrough->addSeparator();
    }
    for (std::size_t i = 1; i < paths.size(); ++i) {
        const QString qpath = QString::fromStdString(paths[i]);
        QAction* action = lookThrough->addAction(tr("Lock to %1").arg(qpath));
        action->setCheckable(true);
        action->setChecked(navigatesLockedCamera() && (_imp->renderer.getLookThroughCamera() == paths[i]));
        QObject::connect(action, &QAction::triggered, this, [this, qpath]() {
            lockViewToCamera(qpath);
        });
    }

    QMenu* views = menu.addMenu(tr("View"));
    QActionGroup* viewModes = new QActionGroup(views);
    for (const ViewModeEntry& entry : kViewModes) {
        QAction* action = views->addAction(tr(entry.label));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getLookThroughCamera().empty() && (_imp->renderer.getViewMode() == entry.mode));
        action->setActionGroup(viewModes);
        action->setShortcut(keySequenceOf(entry.actionID));
        const SceneViewportRenderer::ViewModeEnum mode = entry.mode;
        QObject::connect(action, &QAction::triggered, this, [this, mode]() {
            setViewMode(mode);
        });
    }

    QAction* lockView = menu.addAction(tr(kShortcutDescActionViewer3DLockView));
    lockView->setCheckable(true);
    lockView->setChecked(navigatesLockedCamera());
    lockView->setShortcut(keySequenceOf(kShortcutIDActionViewer3DLockView));
    QObject::connect(lockView, &QAction::toggled, this, [this](bool locked) {
        if (locked) {
            lockViewToCamera();
        } else {
            setLockView(false);
        }
    });
    QAction* createCamera = menu.addAction(tr("Create Camera from View"));
    createCamera->setEnabled(bool(_imp->viewedNodeProvider) && _imp->renderer.hasScene());
    QObject::connect(createCamera, &QAction::triggered, this, [this]() {
        createCameraFromView();
    });

    // The free camera's clip planes: typing a distance makes them absolute.
    QMenu* clipping = menu.addMenu(tr("Clipping"));
    QAction* automaticClip = clipping->addAction(tr("Automatic Clipping"));
    automaticClip->setCheckable(true);
    automaticClip->setChecked(_imp->renderer.getClipPlanes().automatic);
    double nearClip = 0., farClip = 0.;
    _imp->renderer.getFreeCameraClipRange(&nearClip, &farClip);
    QDoubleSpinBox* clipBoxes[2] = { nullptr, nullptr };
    const struct
    {
        const char* name;
        QString label;
        double value;
    } clipItems[] = {
        { "nearClip", tr("Near Clip"), nearClip },
        { "farClip", tr("Far Clip"), farClip },
    };
    for (int i = 0; i < 2; ++i) {
        QWidget* row = new QWidget(clipping);
        QHBoxLayout* layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 2, 8, 2);
        layout->addWidget(new QLabel(clipItems[i].label, row));
        QDoubleSpinBox* box = new QDoubleSpinBox(row);
        box->setObjectName(QString::fromUtf8(clipItems[i].name));
        box->setDecimals(4);
        box->setRange(1e-4, 1e12);
        box->setValue(clipItems[i].value);
        box->setKeyboardTracking(false);
        layout->addWidget(box);
        QWidgetAction* action = new QWidgetAction(clipping);
        action->setDefaultWidget(row);
        clipping->addAction(action);
        clipBoxes[i] = box;
    }
    QDoubleSpinBox* nearBox = clipBoxes[0];
    QDoubleSpinBox* farBox = clipBoxes[1];
    auto applyClip = [this, nearBox, farBox, automaticClip]() {
        SceneViewportRenderer::ClipPlanes planes;
        planes.automatic = false;
        planes.nearClip = nearBox->value();
        planes.farClip = farBox->value();
        {
            const QSignalBlocker blocker(automaticClip);
            automaticClip->setChecked(false);
        }
        setClipPlanes(planes);
    };
    QObject::connect(nearBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, applyClip);
    QObject::connect(farBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, applyClip);
    QObject::connect(automaticClip, &QAction::toggled, this, [this, nearBox, farBox](bool automatic) {
        SceneViewportRenderer::ClipPlanes planes = _imp->renderer.getClipPlanes();
        planes.automatic = automatic;
        if (!automatic) {
            // Absolute planes start where the automatic ones are.
            _imp->renderer.getFreeCameraClipRange(&planes.nearClip, &planes.farClip);
        }
        setClipPlanes(planes);
        double n = 0., f = 0.;
        _imp->renderer.getFreeCameraClipRange(&n, &f);
        const QSignalBlocker nearBlocker(nearBox);
        const QSignalBlocker farBlocker(farBox);
        nearBox->setValue(n);
        farBox->setValue(f);
    });

    menu.addSeparator();
    QAction* frameSelected = menu.addAction(tr("Frame Selection"));
    frameSelected->setShortcut(keySequenceOf(kShortcutIDActionViewer3DFrameSelection));
    QObject::connect(frameSelected, &QAction::triggered, this, [this]() {
        frameSelection();
    });
    QAction* frame = menu.addAction(tr("Frame All"));
    frame->setShortcut(keySequenceOf(kShortcutIDActionViewer3DFrameAll));
    QObject::connect(frame, &QAction::triggered, this, [this]() {
        frameScene();
    });

    QAction* grid = menu.addAction(tr("Show Grid"));
    grid->setCheckable(true);
    grid->setChecked(_imp->renderer.getShowGrid());
    QObject::connect(grid, &QAction::toggled, this, [this](bool show) {
        _imp->renderer.setShowGrid(show);
        update();
    });

    menu.exec(globalPos);
    clearGesture();
} // showContextMenu

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SceneViewport.cpp"
