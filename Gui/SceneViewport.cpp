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
#include <QContextMenuEvent>
#include <QDateTime>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtCore/QDebug>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/UndoGesture.h"

// See UsdIncludes.h: oneTBB, which these include too, declares members named emit().
#ifdef emit
#define NATRON_SCENEVIEWPORT_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/rect2i.h>
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
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_SCENEVIEWPORT_RESTORE_EMIT
#undef NATRON_SCENEVIEWPORT_RESTORE_EMIT
#define emit
#endif

#include "Gui/GLContextLoss.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/Menu.h"
#include "Gui/SceneGizmo.h"

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
const double kMaxElevation = 89.;

double
degreesToRadians(double d)
{
    return d * M_PI / 180.;
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
    bool framedOnce = false;
    std::string lookThrough;
    std::string selected;
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

    // Needs stageMutex.
    GfFrustum computeFrustum(int width,
                             int height) const
    {
        const double aspect = double(std::max(width, 1)) / double(std::max(height, 1));

        if (UsdGeomCamera camera = lookThroughCamera()) {
            GfCamera gfCamera = camera.GetCamera(UsdTimeCode(timeCode()));
            // Keeps the camera's vertical aperture, as usdview does without a camera mask.
            CameraUtilConformWindow(&gfCamera, CameraUtilMatchVertically, aspect);

            return gfCamera.GetFrustum();
        }

        const double distance = std::max(freeCamera.distance, 1e-9);
        const GfVec3d eye = pivot() + eyeDirection() * distance;
        GfMatrix4d view;
        view.SetLookAt(eye, pivot(), worldUp());
        GfFrustum frustum;
        frustum.SetPositionAndRotationFromMatrix(view.GetInverse());
        frustum.SetPerspective(freeCamera.fovY, aspect, distance * 0.01, distance * 1000.);

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
        if (selected.empty() || !SdfPath::IsValidPathString(selected)) {
            engine->ClearSelected();
        } else {
            engine->SetSelected(SdfPathVector(1, SdfPath(selected)));
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
        const double distance = std::max(frustum.GetPosition().GetLength(), freeCamera.distance);
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
    return syncScene(payload, -1., 0., error);
}

bool
SceneViewportRenderer::syncScene(const ScenePayload& payload,
                                 double frame,
                                 double framesPerSecond,
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
    if (framesPerSecond > 0.) {
        _imp->frame = frame;
        _imp->framesPerSecond = framesPerSecond;
    }
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
SceneViewportRenderer::tumble(double azimuthDegrees,
                              double elevationDegrees)
{
    _imp->lookThrough.clear();
    _imp->freeCamera.azimuth = std::fmod(_imp->freeCamera.azimuth + azimuthDegrees, 360.);
    _imp->freeCamera.elevation = std::max(kMinElevation, std::min(kMaxElevation, _imp->freeCamera.elevation + elevationDegrees));
}

void
SceneViewportRenderer::pan(double dxPixels,
                           double dyPixels,
                           int viewHeight)
{
    _imp->lookThrough.clear();
    const GfVec3d forward = -_imp->eyeDirection();
    const GfVec3d right = GfCross(forward, _imp->worldUp()).GetNormalized();
    const GfVec3d up = GfCross(right, forward).GetNormalized();
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
}

const std::string&
SceneViewportRenderer::getLookThroughCamera() const
{
    return _imp->lookThrough;
}

void
SceneViewportRenderer::setSelectedPrim(const std::string& path)
{
    _imp->selected = path;
    std::lock_guard<std::mutex> lock(_imp->stageMutex);
    _imp->applySelection();
}

const std::string&
SceneViewportRenderer::getSelectedPrim() const
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
    if ((width <= 0) || (height <= 0) || !appPTR->isOpenGLLoaded()) {
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
    if ((width <= 0) || (height <= 0) || !appPTR->isOpenGLLoaded()) {
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

struct SceneViewport::Implementation {
    SceneViewportRenderer renderer;
    QString error;
    bool glInitialized = false;

    // The mouse gesture under way.
    Qt::MouseButton button = Qt::NoButton;
    QPoint pressPos;
    QPoint lastPos;
    bool dragging = false;

    // The project frame shown, and its rate once a scene or frame was set.
    double frame = 0.;
    double framesPerSecond = 0.;

    std::function<NodePtr()> transformNodeProvider;
    // The drag of a transform handle under way: its knob, the handle, the value the knob had at
    // the press, and the gesture that folds the drag's edits into one undo step.
    KnobDoubleWPtr handleKnob;
    int handle = SceneGizmo::kNoHandle;
    double handlePressValue = 0.;
    UndoGesture handleGesture;

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
    // The live textures the pending payload shows at pendingFrame, which the worker renders
    // before it syncs: Hydra only reads them.
    LiveTextureSourceList pendingLiveTextures;
    double pendingFrame = 0.;
    double pendingFramesPerSecond = 0.;
    U64 pendingSerial = 0;

    // Main thread: the last request made, the last one synced, and the error the last request
    // shows once synced. A request whose payload shows live textures leaves the renderer at the
    // frame it had until the worker has rendered them, so Hydra never asks for one not stored.
    U64 requestedSerial = 0;
    U64 syncedSerial = 0;
    QString requestedError;
    ScenePayload requestedPayload;
    bool requestedLiveTextures = false;
};

SceneViewport::SceneViewport(QWidget* parent)
    : QOpenGLWidget(parent)
    , _imp(new Implementation)
{
    setFocusPolicy(Qt::ClickFocus);
    // Hovering highlights the transform handle under the pointer.
    setMouseTracking(true);
    setObjectName(QString::fromUtf8("SceneViewport"));
    // A paint skipped while the worker syncs leaves the framebuffer as it was. Assumed from Qt's
    // documentation of QOpenGLWidget::UpdateBehavior, which says PartialUpdate keeps the
    // framebuffer's content between paints; not checked against Qt's source.
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);

    Implementation* imp = _imp.get();
    _imp->worker = std::thread([this, imp]() {
        for (;;) {
            ScenePayload payload;
            U64 serial = 0;
            LiveTextureSourceList liveTextures;
            double frame = 0.;
            double framesPerSecond = 0.;
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
                liveTextures.swap(imp->pendingLiveTextures);
                frame = imp->pendingFrame;
                framesPerSecond = imp->pendingFramesPerSecond;
            }
            std::string error;
            bool ok;
            if (liveTextures.empty()) {
                ok = imp->renderer.syncScene(payload, &error);
            } else {
                LiveTextureStore::instance().prepare(liveTextures, frame, ViewIdx(0));
                liveTextures.clear();
                ok = imp->renderer.syncScene(payload, frame, framesPerSecond, &error);
            }
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
    _imp->requestedPayload = payload;
    LiveTextureSourceList liveTextures = SceneOps::getLiveTextures(payload);
    _imp->requestedLiveTextures = !liveTextures.empty();
    if (!_imp->requestedLiveTextures && (_imp->framesPerSecond > 0.)) {
        _imp->renderer.setFrame(_imp->frame, _imp->framesPerSecond);
    }
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->pending = payload;
        _imp->hasPending = true;
        _imp->pendingLiveTextures.swap(liveTextures);
        _imp->pendingFrame = _imp->frame;
        _imp->pendingFramesPerSecond = _imp->framesPerSecond;
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
    _imp->framesPerSecond = framesPerSecond;
    requestSync(payload, QString());
}

void
SceneViewport::setFrame(double frame,
                        double framesPerSecond)
{
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
    if (_imp->requestedLiveTextures) {
        // The same scene, through the worker, which renders the frame's textures first.
        requestSync(_imp->requestedPayload, _imp->requestedError);

        return;
    }
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
    setProperty("natronSceneEngineCount", QVariant::fromValue<qulonglong>(_imp->renderer.getEngineCount()));
    setProperty("natronSceneEngineHeld", _imp->renderer.hasEngine());
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
    _imp->renderer.setLookThroughCamera(path.toStdString());
    update();
}

void
SceneViewport::frameScene()
{
    _imp->renderer.frameScene();
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
    setSelectedPrim(QString::fromStdString(path));

    return QString::fromStdString(path);
}

void
SceneViewport::setSelectedPrim(const QString& path)
{
    const std::string p = path.toStdString();

    if (p == _imp->renderer.getSelectedPrim()) {
        return;
    }
    if (_imp->glInitialized && context()) {
        // The engine may exist, and selection changes reach its GL resources.
        makeCurrent();
        _imp->renderer.setSelectedPrim(p);
        doneCurrent();
    } else {
        _imp->renderer.setSelectedPrim(p);
    }
    update();
    Q_EMIT selectedPrimChanged(path);
}

QString
SceneViewport::getSelectedPrim() const
{
    return QString::fromStdString(_imp->renderer.getSelectedPrim());
}

void
SceneViewport::setTransformNodeProvider(const std::function<NodePtr()>& provider)
{
    _imp->transformNodeProvider = provider;
    update();
}

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
        QString hud = tr("%1 | %2 | time %3")
                          .arg(mode)
                          .arg(camera.empty() ? tr("Free camera") : QString::fromStdString(camera))
                          .arg(_imp->renderer.getTimeCode(), 0, 'g', 6);
        const std::string& selected = _imp->renderer.getSelectedPrim();
        if (!selected.empty()) {
            hud += tr(" | %1").arg(QString::fromStdString(selected));
        }
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignTop, hud);
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
    if ((e->button() == Qt::LeftButton) && (e->modifiers() == Qt::NoModifier) && beginHandleDrag(e->position().toPoint())) {
        e->accept();

        return;
    }
    // Only a press that starts a camera drag records its button: a plain right press opens the
    // context menu, which takes the release, so a recorded button would never be cleared.
    if ((e->button() == Qt::LeftButton) || (e->button() == Qt::MiddleButton) || ((e->button() == Qt::RightButton) && (e->modifiers() & Qt::AltModifier))) {
        _imp->button = e->button();
        _imp->pressPos = e->position().toPoint();
        _imp->lastPos = _imp->pressPos;
        _imp->dragging = false;
        e->accept();
    } else {
        QOpenGLWidget::mousePressEvent(e);
    }
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
        _imp->button = Qt::NoButton;
        _imp->dragging = false;
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

    const bool panning = (_imp->button == Qt::MiddleButton) || ((_imp->button == Qt::LeftButton) && (e->modifiers() & Qt::ShiftModifier));
    if (panning) {
        _imp->renderer.pan(delta.x(), delta.y(), height());
    } else if (_imp->button == Qt::LeftButton) {
        _imp->renderer.tumble(-delta.x() * 0.4, delta.y() * 0.4);
    } else if (_imp->button == Qt::RightButton) {
        _imp->renderer.dolly(std::exp(-(delta.x() + delta.y()) * 0.01));
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
    const bool click = (_imp->button == Qt::LeftButton) && !_imp->dragging && (e->button() == Qt::LeftButton);

    _imp->button = Qt::NoButton;
    _imp->dragging = false;
    if (click) {
        pickPrimAt(e->position().toPoint());
    }
    e->accept();
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
    const double steps = e->angleDelta().y() / 120.;

    if (steps == 0.) {
        e->ignore();

        return;
    }
    _imp->renderer.dolly(std::pow(0.85, steps));
    update();
    e->accept();
}

void
SceneViewport::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_F) && (e->modifiers() == Qt::NoModifier)) {
        frameScene();
        e->accept();

        return;
    }
    // Everything else is the viewer tab's.
    e->ignore();
}

void
SceneViewport::contextMenuEvent(QContextMenuEvent* e)
{
    // Alt+right drag dollies.
    if (e->modifiers() & Qt::AltModifier) {
        e->accept();

        return;
    }
    // The menu takes the release, so no drag may outlive it.
    _imp->button = Qt::NoButton;
    _imp->dragging = false;
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

    QAction* frame = menu.addAction(tr("Frame All"));
    frame->setShortcut(QKeySequence(Qt::Key_F));
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

    menu.exec(e->globalPos());
    e->accept();
} // contextMenuEvent

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SceneViewport.cpp"
