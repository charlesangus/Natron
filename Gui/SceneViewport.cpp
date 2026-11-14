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
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <QActionGroup>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QUndoCommand>
#include <QWheelEvent>
#include <QWidgetAction>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/StormGLSection.h"
#include "Engine/Nodes/Scene/StormPtexBudget.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/UndoGesture.h"
#include "Serialization/NodeSerialization.h"

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
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/pickTask.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hgi/texture.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageLoadRules.h>
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
#include "Gui/SceneDrawBoxes.h"
#include "Gui/SceneDrawFilter.h"
#include "Gui/SceneDrawStateQueue.h"
#include "Gui/SceneDrawTiming.h"
#include "Gui/SceneGizmo.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneGuides.h"
#include "Gui/SceneTransformTarget.h"
#include "Gui/SceneTransformWidget.h"
#include "Gui/UndoHistory.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// The background grey, in scene-linear; it is shown through the display transform.
const float kBackgroundLinear = 0.05f;
// The same grey once encoded to sRGB, for the clears and checks done outside Storm.
const float kBackgroundSrgb = 0.2478f;

// Storm may need more than one pass before an image is final.
const int kMaxRenderPasses = 8;

// A stage change usually reaches the screen sooner than this, and a status flashing over the old
// image on every edit would be worse than the short wait.
const int kComposingStatusDelayMs = 250;
// The longest a sync's first frame waits for the graph policy its prims take: past it, the view
// draws them as they are rather than seem stuck.
const int kPolicyHoldMs = 500;

// A paced reveal aims for 30 frames a second, the rate a viewer still tumbles smoothly at.
const double kRevealTargetMs = 33.;
// A step looking at prims it finds nothing to show in, such as those of a subtree shown already,
// stops here, so the frame it is part of still keeps to the target.
const int kRevealStepDeadlineMs = 16;
// Long enough to span the gaps between the moves of one drag.
const int kRevealCameraYieldMs = 250;

SdfPathVector
toSdfPaths(const std::vector<std::string>& paths)
{
    SdfPathVector result;

    for (const std::string& path : paths) {
        if (SdfPath::IsValidPathString(path)) {
            result.push_back(SdfPath(path));
        }
    }

    return result;
}

// Releases the Python GIL while the calling thread waits for the render thread, if it holds it: a
// script may have caused the paint, and an evaluation the render thread waits behind may need the GIL.
class PythonUnlockedWhileWaiting {
public:
    PythonUnlockedWhileWaiting()
        : _state(nullptr)
    {
        if (Py_IsInitialized() && PyGILState_Check()) {
            _state = PyEval_SaveThread();
        }
    }

    ~PythonUnlockedWhileWaiting()
    {
        if (_state) {
            PyEval_RestoreThread(_state);
        }
    }

    PythonUnlockedWhileWaiting(const PythonUnlockedWhileWaiting&) = delete;
    PythonUnlockedWhileWaiting& operator=(const PythonUnlockedWhileWaiting&) = delete;

private:
    PyThreadState* _state;
};

const double kMinElevation = -89.;
// The radius framed around a selection without extent, in scene units.
const double kDefaultFrameRadius = 1.;
const double kMaxElevation = 89.;

double
degreesToRadians(double d)
{
    return d * M_PI / 180.;
}

/**
 * @brief The 2D viewer's display transform, as ProjectColorManagement::getDisplayProcessor() makes
 * it: colorSpace through display and view, with look in place of the view's own looks when it is
 * not empty. Throws what OpenColorIO throws.
 **/
OCIO_NAMESPACE::ConstProcessorRcPtr
displayProcessor(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                 const std::string& colorSpace,
                 const std::string& display,
                 const std::string& view,
                 const std::string& look)
{
    OCIO_NAMESPACE::DisplayViewTransformRcPtr displayView = OCIO_NAMESPACE::DisplayViewTransform::Create();
    displayView->setSrc(colorSpace.c_str());
    displayView->setDisplay(display.c_str());
    displayView->setView(view.c_str());
    OCIO_NAMESPACE::LegacyViewingPipelineRcPtr pipeline = OCIO_NAMESPACE::LegacyViewingPipeline::Create();
    pipeline->setDisplayViewTransform(displayView);
    if (!look.empty()) {
        pipeline->setLooksOverrideEnabled(true);
        pipeline->setLooksOverride(look.c_str());
    }

    return pipeline->getProcessor(config);
}

// The encoding shown without OpenColorIO, as Hydra's colour correction applies it.
const char* const kSrgbDisplayFunction = "vec4 toDisplay(vec4 c) {\n"
                                         "    vec3 v = max(c.rgb, vec3(0.0));\n"
                                         "    return vec4(mix(v * 12.92, 1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, v)), c.a);\n"
                                         "}\n";

const char* const kGradeVertexShader = "#version 400 compatibility\n"
                                       "void main() {\n"
                                       "    gl_Position = vec4((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0, 0.0, 1.0);\n"
                                       "}\n";

/**
 * @brief The pass that draws Storm's colour AOV onto the framebuffer, around displayFunction, which
 * defines vec4 toDisplay(vec4). The AOV is premultiplied over a transparent clear, and a display
 * transform is not linear, so each pixel is graded unpremultiplied (gain, toDisplay, then gamma, as
 * the 2D viewer applies them) and premultiplied again for the blend over the ungraded background.
 * Pixels nothing was rendered to are left to the background. The AOV's depth goes with it, for
 * the grid and guides drawn after; without one, what was rendered stays in front of them.
 **/
std::string
gradeFragmentShader(const std::string& displayFunction)
{
    std::string source = "#version 400 compatibility\n"
                         "uniform sampler2D colorAov;\n"
                         "uniform sampler2D depthAov;\n"
                         "uniform bool hasDepth;\n"
                         "uniform float gain;\n"
                         "uniform float gamma;\n"
                         "\n";
    source += displayFunction;
    source += "\n"
              "void main() {\n"
              "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
              "    vec4 c = texelFetch(colorAov, p, 0);\n"
              "    float a = clamp(c.a, 0.0, 1.0);\n"
              "    if (a <= 0.0) {\n"
              "        discard;\n"
              "    }\n"
              "    vec3 shown = toDisplay(vec4(c.rgb / a * gain, 1.0)).rgb;\n"
              "    if (gamma <= 0.0) {\n"
              "        shown = vec3(greaterThanEqual(shown, vec3(1.0)));\n"
              "    } else {\n"
              "        shown = pow(max(shown, vec3(0.0)), vec3(1.0 / gamma));\n"
              "    }\n"
              "    gl_FragColor = vec4(shown * a, a);\n"
              "    gl_FragDepth = hasDepth ? texelFetch(depthAov, p, 0).r : 0.0;\n"
              "}\n";

    return source;
}

// Throws std::runtime_error with the driver's log.
GLuint
compileShader(GLenum type,
              const std::string& source)
{
    const GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(std::max(length, 1), '\0');
        glGetShaderInfoLog(shader, (GLsizei)log.size(), nullptr, &log[0]);
        glDeleteShader(shader);
        throw std::runtime_error("the shader does not compile: " + log);
    }

    return shader;
}

// Throws std::runtime_error with the driver's log.
GLuint
linkProgram(const std::string& vertexSource,
            const std::string& fragmentSource)
{
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = 0;
    try {
        fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    } catch (...) {
        glDeleteShader(vertex);
        throw;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(std::max(length, 1), '\0');
        glGetProgramInfoLog(program, (GLsizei)log.size(), nullptr, &log[0]);
        glDeleteProgram(program);
        throw std::runtime_error("the shader does not link: " + log);
    }

    return program;
}

void
setLutParameters(GLenum target,
                 OCIO_NAMESPACE::Interpolation interpolation)
{
    const GLint filter = (interpolation == OCIO_NAMESPACE::INTERP_NEAREST) ? GL_NEAREST : GL_LINEAR;

    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    if (target != GL_TEXTURE_1D) {
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (target == GL_TEXTURE_3D) {
        glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
}

// The colour AOV and its depth take the first two texture units, the LUTs the ones after.
const GLint kFirstLutUnit = 2;

struct GradeLut {
    GLuint id;
    GLenum target;
    std::string sampler;
};

double
radiansToDegrees(double r)
{
    return r * 180. / M_PI;
}

/**
 * @brief Saves on construction, and restores on destruction, the GL state Storm (HgiGL, and the
 * pass grading its AOV into the target framebuffer) changes and the drawing around it relies on: the
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

// Published so that a harness can hold the viewer to display states never editing the stage.
class StageNoticeCounter
    : public TfWeakBase {
public:
    void onObjectsChanged(const UsdNotice::ObjectsChanged& notice)
    {
        ++count;
        resyncedPaths += notice.GetResyncedPaths().size();
        infoOnlyPaths += notice.GetChangedInfoOnlyPaths().size();
    }

    std::atomic<U64> count { 0 };
    std::atomic<U64> resyncedPaths { 0 };
    std::atomic<U64> infoOnlyPaths { 0 };
};

/**
 * The threads a viewer's Cycles engine asks for, on its worker: the budget the thread preferences
 * give one effect, less a core for the interface. It is the budget with the pool idle, not
 * getNCPUsAvailableForEffect()'s, which follows the pool's busy threads from moment to moment:
 * each change of the count rebuilds every Cycles engine of the process.
 **/
int
refineThreads()
{
    AppManager* app = appPTR;
    if (!app) {
        return 1;
    }
    int toRender = 0;
    int perEffect = 0;
    app->getNThreadsSettings(&toRender, &perEffect);
    if (toRender == -1) {
        return 1;
    }
    const int poolMax = app->getMaxThreadCount();
    if (perEffect == 0) {
        perEffect = std::max(1, std::min(poolMax, app->getHardwareIdealThreadCount()));
    }

    return std::max(1, AppManager::computeNCPUsAvailable(poolMax, 0, 0, perEffect) - 1);
}
/**
 * @brief Collects, while it lives, whether the stage's change notices touched anything outside
 * /Render, where the render settings prims are. Anything that is not a prim's property or a prim
 * under /Render counts, but for info-only changes of the pseudo-root: a sublayer swap reports
 * there every metadata field the swapped layers author, changed or not, though only the root
 * layer's compose into the stage's, and those are compared by the caller (SceneMetadata).
 **/
class StageChangePaths
    : public TfWeakBase {
public:
    explicit StageChangePaths(const UsdStageRefPtr& stage)
    {
        if (stage) {
            _key = TfNotice::Register(TfCreateWeakPtr(this), &StageChangePaths::changed, UsdStageWeakPtr(stage));
        }
    }

    ~StageChangePaths()
    {
        TfNotice::Revoke(_key);
    }

    StageChangePaths(const StageChangePaths&) = delete;
    StageChangePaths& operator=(const StageChangePaths&) = delete;

    bool any = false;
    bool outsideRender = false;

private:
    void changed(const UsdNotice::ObjectsChanged& notice)
    {
        static const SdfPath render("/Render");
        const auto collect = [this](const UsdNotice::ObjectsChanged::PathRange& paths,
                                    bool infoOnly) {
            for (UsdNotice::ObjectsChanged::PathRange::const_iterator it = paths.begin(); it != paths.end(); ++it) {
                any = true;
                if (it->GetPrimPath().HasPrefix(render) || (infoOnly && it->IsAbsoluteRootPath())) {
                    continue;
                }
                outsideRender = true;
            }
        };
        collect(notice.GetResyncedPaths(), false);
        collect(notice.GetChangedInfoOnlyPaths(), true);
    }

    TfNotice::Key _key;
};

// One instance per box: corner picks between its min and max, a 0 or 1 per axis. A marker is a
// cross of fixed size on screen around boxMin, corner its arm in pixels over markerScale.
const char* const kBoxVertexShader = "#version 330 core\n"
                                     "uniform mat4 viewProjection;\n"
                                     "uniform int markers;\n"
                                     "uniform vec2 markerScale;\n"
                                     "layout(location = 0) in vec3 corner;\n"
                                     "layout(location = 1) in vec3 boxMin;\n"
                                     "layout(location = 2) in vec3 boxMax;\n"
                                     "layout(location = 3) in vec3 boxColour;\n"
                                     "out vec3 edgeColour;\n"
                                     "void main() {\n"
                                     "    edgeColour = boxColour;\n"
                                     "    if (markers != 0) {\n"
                                     "        vec4 centre = viewProjection * vec4(boxMin, 1.0);\n"
                                     "        gl_Position = centre + vec4(corner.xy * markerScale * centre.w, 0.0, 0.0);\n"
                                     "    } else {\n"
                                     "        gl_Position = viewProjection * vec4(mix(boxMin, boxMax, corner), 1.0);\n"
                                     "    }\n"
                                     "}\n";
const char* const kBoxFragmentShader = "#version 330 core\n"
                                       "in vec3 edgeColour;\n"
                                       "out vec4 colour;\n"
                                       "void main() { colour = vec4(edgeColour, 1.0); }\n";
const GLfloat kBoxEdges[] = {
    0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0,
    0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1,
    0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1
};
const GLfloat kMarkerArms[] = { -1, 0, 0, 1, 0, 0, 0, -1, 0, 0, 1, 0 };
const int kBoxFloats = 9;
const float kMarkerPixels = 6.f;
// Beyond a marker's arms, so a click need not land on its one-pixel lines.
const float kMarkerPickSlack = 2.f;
} // anon namespace

std::atomic<U64> engineConstructionCount(0);

struct SceneViewportRenderer::Implementation {
    // The viewer lock: held while sync edits the stage, and while anything reads the stage or
    // touches the engine (whose stage scene index queues the stage's change notices without a
    // lock of its own, OpenUSD v26.08 usdImaging/stageSceneIndex.cpp). Shared by readers: OpenUSD
    // allows many, and a background frame is one that may run for seconds.
    std::shared_mutex stageMutex;

    // A try for stageMutex to edit failed, and the holders may be readers that announce no end.
    std::atomic<bool> exclusiveOwed { false };
    std::function<void()> readEnded;

    // For the main thread, and for work it waits on. Whoever made this fail lets the viewport know
    // when it lets go: edits and frames by their signals, other readers by readEnded.
    bool tryLockExclusive(std::unique_lock<std::shared_mutex>* lock)
    {
        *lock = std::unique_lock<std::shared_mutex>(stageMutex, std::try_to_lock);
        if (lock->owns_lock()) {
            return true;
        }
        exclusiveOwed = true;
        // The last reader may have gone before it could see the flag.
        if (lock->try_lock()) {
            exclusiveOwed = false;

            return true;
        }
        ++backgroundStops;

        return false;
    }

    // Threads blocked taking stageMutex to edit. A background frame, which holds it shared for up
    // to kMaxRenderPasses passes, ends at its next pass for them. Not exclusiveOwed, which may
    // stay set after the try it answers has gone.
    std::atomic<int> exclusiveWaiting { 0 };
    // Counters a background frame compares with their values when it was prepared: a stop ends it
    // at its next pass after the first, keeping what it drew; a cancel before its next pass, the
    // first included, taking nothing in.
    std::atomic<U64> backgroundStops { 0 };
    std::atomic<U64> backgroundCancels { 0 };

    // Takes stageMutex to edit, waiting for it.
    /**
     * @brief Takes what the payload of a sync was evaluated for. False if its patterns did not
     * match on what the stage loads now, so it is evaluated again for that and is not synced.
     **/
    bool takeSyncLoadSet(const ScenePayload& payload)
    {
        SceneLoadSetPtr evaluatedFor;
        SceneLoadReachPtr reach;
        SceneLoadSetPtr committedLoadSet;
        SceneViewportRenderer::LoadSetCommitted committed;
        {
            std::lock_guard<std::mutex> loadSetLock(loadSetMutex);
            evaluatedFor.swap(syncLoadSet);
            reach.swap(syncReach);
            committedLoadSet = this->committedLoadSet;
            committed = loadSetCommitted;
        }
        if (reach && !payload.isEmpty() && SceneOps::loadChangeReaches(evaluatedFor.get(), committedLoadSet.get(), reach->getPrefixes())) {
            if (committed) {
                committed(committedLoadSet, true);
            }

            return false;
        }
        {
            // Matches made for evaluatedFor hold for the committed set, which the next load
            // change is measured from.
            std::lock_guard<std::mutex> loadSetLock(loadSetMutex);
            shownLoadSet = committedLoadSet;
            shownReach = payload.isEmpty() ? std::make_shared<SceneLoadReach>() : reach;
        }

        return true;
    }

    std::unique_lock<std::shared_mutex> lockExclusive()
    {
        struct Waiting {
            std::atomic<int>& count;

            explicit Waiting(std::atomic<int>& c)
                : count(c)
            {
                ++count;
            }
            ~Waiting() { --count; }
        } waiting(exclusiveWaiting);

        return std::unique_lock<std::shared_mutex>(stageMutex);
    }
    // Created by the first sync and kept for the renderer's life: the engine binds the first
    // stage it renders for good (v26.08 usdImagingGL/engine.cpp, PrepareBatch populates once).
    std::unique_ptr<SceneStage> sceneStage;
    // The same object, readable without the lock by those who only queue load requests.
    std::atomic<SceneStage*> loadStage { nullptr };
    // Load requests made before the first sync made the stage, handed to it then. The mutex also
    // orders them against loadStage being set, so that none falls between.
    std::mutex earlyLoadsMutex;
    std::vector<SceneDrawLoadRequest> earlyLoads;
    // The stage's load rules as its last load commit left them, for saving without the stage lock.
    std::mutex loadRulesMutex;
    SceneLoadRules loadRules;
    bool loadRulesMade = false;
    SceneWriteBarrier::ParticipantId participant = 0;

    // With the stage lock held exclusively, which also orders the writes of loadRulesMade.
    void publishLoadRules()
    {
        SceneLoadRules rules;
        for (const std::pair<SdfPath, UsdStageLoadRules::Rule>& rule : sceneStage->getStage()->GetLoadRules().GetRules()) {
            SceneLoadRuleEnum saved = eSceneLoadRuleNone;
            if (rule.second == UsdStageLoadRules::AllRule) {
                saved = eSceneLoadRuleAll;
            } else if (rule.second == UsdStageLoadRules::OnlyRule) {
                saved = eSceneLoadRuleOnly;
            } else if (rule.first.IsAbsoluteRootPath()) {
                continue;
            }
            rules[rule.first.GetString()] = saved;
        }
        std::lock_guard<std::mutex> lock(loadRulesMutex);
        loadRules.swap(rules);
        loadRulesMade = true;
    }

    // Written by sync on whichever thread runs it, read by the main thread.
    std::atomic<bool> shown;
    std::atomic<U64> stageEditCount { 0 };
    std::atomic<U64> stageLoadEditCount { 0 };
    // What serviceStageLoads() evaluates a batch's scene with. Under loadSetMutex too: the load
    // set the stage last committed, what the next sync's payload was evaluated for, and what the
    // payload on the stage was evaluated for. A null reach is for nothing in particular, and for
    // the stage's payload, one any load change may alter.
    std::mutex loadSetMutex;
    SceneViewportRenderer::LoadSetEvaluator loadSetEvaluator;
    SceneViewportRenderer::LoadSetCommitted loadSetCommitted;
    std::atomic<U64> committedLoadSetHash { SceneLoadSet::none().getHash() };
    SceneLoadSetPtr committedLoadSet = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
    SceneLoadSetPtr syncLoadSet;
    SceneLoadReachPtr syncReach;
    SceneLoadSetPtr shownLoadSet;
    SceneLoadReachPtr shownReach = std::make_shared<SceneLoadReach>();
    std::atomic<U64> stageSyncSerial { 0 };
    // The counts the engine last took in, by whichever render.
    std::atomic<U64> absorbedEditCount { 0 };
    std::atomic<U64> absorbedLoadEditCount { 0 };
    std::atomic<std::size_t> absorbedParamsKey { 0 };

    // Draw state deltas from the main thread, queued because applying one sends Hydra notices that
    // it must not wait for. drawRules is what a filter on a new engine starts from. drawMutex is
    // never held across a Hydra call, so the main thread's post cannot block on one.
    struct QueuedDrawDelta {
        SceneDrawDelta delta;
        std::size_t revealBatch;
        std::chrono::steady_clock::time_point startedAt;
    };
    mutable std::mutex drawMutex;
    std::vector<QueuedDrawDelta> drawQueue;
    DrawFilterStats drawStats;
    std::atomic<U64> drawPosted { 0 };
    std::atomic<U64> drawApplied { 0 };
    std::atomic<U64> drawAbsorbed { 0 };
    // A reveal shows one step per presented image; drawRevealStepDue says that image has been seen.
    std::atomic<bool> drawRevealing { false };
    std::atomic<bool> drawRevealStepDue { true };
    std::size_t drawRevealBatch = 0;
    // A paced step is timed from its start to the end of the render that sent it to Hydra, which is
    // where its cost lands.
    SceneRevealPacer drawRevealPacer { kRevealTargetMs };
    bool drawRevealStepTimed = false;
    std::size_t drawRevealStepPrims = 0;
    std::chrono::steady_clock::time_point drawRevealStepStart;
    unsigned long long drawRevealSteps = 0;
    // When the user last moved the camera, as steady clock ticks.
    std::atomic<long long> cameraMovedAt { 0 };
    // Background frames take the reveal steps then, so a render the main thread waits for takes none.
    std::atomic<bool> backgroundFramesUsed { false };
    SceneDrawFilter::Rules drawRules;
    SceneDrawFilter::WeakPtr drawFilter;
    // The list the box buffer holds, and how many of its boxes the last frame drew.
    SceneDrawBoxes::ListPtr uploadedBoxes;
    // What the last frame drew, which is what a click picks from.
    SceneDrawBoxes::ListPtr drawnBoxList;
    // Cleared by the main thread when the selection changes, as the buffer colours the boxes
    // holding a selected prim.
    std::atomic<bool> boxColoursValid { false };
    std::vector<bool> boxHighlighted;
    bool recordDrawnBoxes = false;
    std::vector<DrawnBox> drawnBoxes;
    // Bumped with each change of drawRules, so the guides follow them.
    U64 drawRulesVersion = 0;
    GLuint boxProgram = 0;
    GLuint boxVao = 0;
    GLuint boxEdgeBuffer = 0;
    GLuint boxInstanceBuffer = 0;
    GLsizei boxInstances = 0;
    GLuint markerVao = 0;
    GLuint markerArmBuffer = 0;
    GLuint markerInstanceBuffer = 0;
    GLsizei markerInstances = 0;
    std::atomic<int> boxesDrawn { 0 };
    std::atomic<int> markersDrawn { 0 };
    std::atomic<bool> everyLightHidden { false };
    StageNoticeCounter stageNotices;
    bool stageNoticesRegistered = false;
    const std::unique_ptr<SceneDrawTiming> drawTiming { SceneDrawTiming::enabledByEnvironment() ? new SceneDrawTiming : nullptr };

    std::atomic<bool> zUp;
    std::atomic<double> metersPerUnit;
    std::atomic<double> timeCodesPerSecond;
    std::atomic<double> frame;
    std::atomic<double> framesPerSecond;
    std::atomic<bool> stageHeld { false };

    // Main thread only.
    DrawModeEnum drawMode = eDrawModeShaded;
    bool showGrid = true;
    bool showGuides = true;
    bool showProxy = true;
    bool showRender = false;
    bool showGuidePurpose = false;
    LightingModeEnum lightingMode = eLightingModeScene;
    bool sceneLit = false;
    FreeCamera freeCamera;
    ClipPlanes clipPlanes;
    // Read by the threads that work out the first framing.
    std::atomic<bool> framedOnce { false };
    std::string lookThrough;
    // The transform the camera at lookThroughOverridePath is drawn with instead of its prim's.
    std::string lookThroughOverridePath;
    GfMatrix4d lookThroughOverride = GfMatrix4d(1.);
    // The selected prims, the current one last.
    std::vector<std::string> selected;
    std::unique_ptr<UsdImagingGLEngine> engine;
    std::atomic<bool> engineHeld { false };
    std::atomic<U64> engineCount { 0 };
    // What the engine has yet to be told is selected: the main thread sets it without waiting for
    // whichever thread holds the engine.
    std::mutex engineSelectionMutex;
    std::vector<std::string> engineSelection;
    std::atomic<bool> engineSelectionDirty { false };
    // The renderer chosen, a delegate's id or kRenderDelegateSceneDefaultId; the delegate the
    // next engine is made with, the chosen one or the one the stage's render settings prim names;
    // and why it could not be, if it could not.
    std::string rendererChoice = kRenderDelegateStormId;
    std::string rendererId = kRenderDelegateStormId;
    // Written under stageMutex and this, so that the main thread reads them with neither while the
    // render thread switches renderers.
    mutable std::mutex rendererIdMutex;
    mutable std::mutex rendererErrorMutex;
    std::string rendererError;
    // Why the refinement's delegate is not rendering now, from its status: unlike rendererError it
    // does not stop the refinement, which tries the delegate again at the next edit.
    std::string refineError;
    // Why the last render threw, until a paint takes it.
    std::string renderError;
    // Whether the last render that kept its renderer found Scene Default naming another.
    std::atomic<bool> rendererSwitchPending { false };
    // The stage's active render settings prim as of an edit count: its path, empty without one,
    // and the renderer it names, Storm's without one. Then the prim's settings for a renderer at
    // a time. All need stageMutex.
    bool scenePrimValid = false;
    U64 scenePrimEditCount = 0;
    SdfPath scenePrimPath;
    std::string sceneRenderer = kRenderDelegateStormId;
    bool sceneSettingsValid = false;
    U64 sceneSettingsEditCount = 0;
    std::string sceneSettingsRenderer;
    double sceneSettingsTime = 0.;
    RenderSettingsPrim::Values sceneSettings;
    // The prim's settings the Storm engine numbered stormSettingsEngine was last given.
    RenderSettingsPrim::Values stormSettings;
    U64 stormSettingsEngine = 0;

    // What a background frame draws, copied on the thread that owns these settings, and what it left.
    struct BackgroundFrame {
        int width = 0;
        int height = 0;
        GfFrustum frustum;
        UsdImagingGLRenderParams params;
        LightingModeEnum lightingMode = eLightingModeScene;
        OCIO_NAMESPACE::ConstProcessorRcPtr displayProcessor;
        double gain = 1.;
        double gamma = 1.;
        float background[3] = { 0.f, 0.f, 0.f };

        // True while the fbo holds this frame's image and nothing has drawn over it.
        bool imageKept = false;
        unsigned int fbo = 0;
        U64 editCount = 0;
        U64 loadEditCount = 0;
        U64 previewGeneration = 0;
        // backgroundStops and backgroundCancels when the frame was prepared.
        U64 stops = 0;
        U64 cancels = 0;
    };
    BackgroundFrame backgroundFrame;

    // What a traversal of the stage found, as of an edit count. The threads that change the stage
    // publish it before anyone is told of the change, and nothing else traverses, so no frame or
    // menu on the main thread walks a large stage.
    struct StageSummary {
        U64 editCount = 0;
        std::vector<SceneGuides::GuidePrim> guidePrims;
        bool hasLights = false;
        std::vector<std::string> cameraPaths;
    };
    mutable std::mutex summaryMutex;
    std::shared_ptr<const StageSummary> stageSummary;
    // Where a stage camera turns about. Kept from the last time the scene's bounds were worked
    // out: a heuristic, not worth a walk of the stage for each pose a drag asks for.
    GfRange3d pivotBounds;

    // The guides as of a summary and a time: need stageMutex.
    std::shared_ptr<const StageSummary> guidesSummary;
    std::vector<SceneGuides::Guide> guides;
    bool guidesValid = false;
    U64 guidesRulesVersion = 0;
    double guidesTimeCode = 0.;
    double guidesLength = 0.;
    std::atomic<double> guideSize { 1. };
    std::vector<DrawnGuide> drawnGuides;
    bool recordDrawnGuides = false;
    // Indices into guides, unselected cameras, unselected lights and selected ones, without the
    // look-through camera's.
    std::vector<int> guideBatches[3];
    // Cleared by the main thread when the selection changes, whichever thread holds the engine.
    std::atomic<bool> guideBatchesValid { false };

    DisplayTransform displayTransform;
    // displayTransform's display, view and look, null for the sRGB encoding.
    OCIO_NAMESPACE::ConstProcessorRcPtr displayProcessor;
    // The background as shown: the display transform of kBackgroundLinear, without the viewer's
    // gain and gamma, which act on rendered pixels only.
    float backgroundShown[3] = { kBackgroundSrgb, kBackgroundSrgb, kBackgroundSrgb };
    // The pass grading Storm's colour AOV onto the framebuffer (gradeFragmentShader()), GL objects
    // of the context of render(), and the processor it was made from. Only the thread holding the
    // engine touches them: a background frame grades with the processor it copied, not the one
    // the main thread may have set since.
    GLuint gradeProgram = 0;
    std::vector<GradeLut> gradeLuts;
    OCIO_NAMESPACE::GpuShaderDescRcPtr gradeShader;
    bool gradeBuilt = false;
    OCIO_NAMESPACE::ConstProcessorRcPtr gradeProcessor;

    // The transform handles, if any, and the drag of one under way.
    bool hasHandles = false;
    TransformHandles handles;
    SceneTransform::ToolEnum transformTool = SceneTransform::eToolSelect;
    SceneTransform::SpaceEnum transformSpace = SceneTransform::eSpaceWorld;
    SceneTransform::PivotEnum transformPivot = SceneTransform::ePivotSelectionCentre;
    int highlightedHandle = SceneGizmo::kNoHandle;
    int dragHandle = SceneGizmo::kNoHandle;
    SceneGizmo::Frame dragFrame;
    GfVec2d dragPressPixel;
    // A drag of the selection's widget moves the prims as they were at the press; a drag of an
    // op's handles changes the op's values, the op's result placed by dragOpSpace and each other
    // target's by its own space.
    bool dragSelection = false;
    std::vector<SceneTransformPrim> dragPrims;
    GfMatrix4d dragOpSpace = GfMatrix4d(1.);
    std::vector<std::pair<std::string, GfMatrix4d>> dragOpTargets;
    double dragChange = 0.;
    bool dragChanged = false;
    // The serial after which a synced scene has the drag written; kNoPreviewClear while none is
    // awaited. Set on the main thread, read by sync.
    static const U64 kNoPreviewClear = ~U64(0);
    std::atomic<U64> previewClearAfter;

    // The progressive render of a delegate other than Storm, and what it was last told.
    mutable std::mutex refineListenerMutex;
    std::function<void()> refineListener;
    std::atomic<int> maxSamples { 0 };
    // The delegate the refinement was last given; needs stageMutex held exclusively. A release
    // from the main thread, which holds no lock, asks the next render to forget it.
    std::string refineRenderer;
    std::atomic<bool> refineReleased { false };
    // The texture its frames are drawn from, the buffer holding their depth, and the frame they
    // hold: shared by every context sharing the one of render(), any of which may draw them. The
    // framebuffer reading the texture is made for each draw, as framebuffers are not shared.
    GLuint refineTexture = 0;
    GLuint refineDepthBuffer = 0;
    int refineWidth = 0;
    int refineHeight = 0;
    // The frame in the texture. Kept alive, so that a later frame cannot have its address, and not
    // told apart by its serial, which starts over with each delegate.
    std::shared_ptr<const HydraRender::ProgressiveRender::Frame> uploadedFrame;
    std::atomic<int> drawnSamples { -1 };
    std::atomic<double> uploadMs { -1. };
    std::atomic<int> uploadPixels { 0 };
    // Last, so that its worker, which reads the stage, is joined before the stage goes.
    std::unique_ptr<HydraRender::ProgressiveRender> progressive;

    Implementation()
        : shown(false)
        , zUp(false)
        , metersPerUnit(0.01)
        , timeCodesPerSecond(24.)
        , frame(0.)
        , framesPerSecond(24.)
        , previewClearAfter(kNoPreviewClear)
    {
        freeCamera.azimuth = 35.;
        freeCamera.elevation = 25.;
    }

    // The stage shown, null when none is. Needs stageMutex.
    UsdStageRefPtr currentStage() const
    {
        return (shown && sceneStage) ? sceneStage->getStage() : UsdStageRefPtr();
    }

    // Reads the stage's active render settings prim again after the stage is edited. Needs stageMutex.
    void refreshScenePrim()
    {
        const UsdStageRefPtr stage = currentStage();
        // A load can bring in a render settings prim as it does any other.
        const U64 editCount = sceneStage ? sceneStage->getEditCount() + sceneStage->getLoadEditCount() : 0;
        if (scenePrimValid && (scenePrimEditCount == editCount)) {
            return;
        }
        scenePrimValid = true;
        scenePrimEditCount = editCount;
        scenePrimPath = SdfPath();
        sceneRenderer = kRenderDelegateStormId;
        const UsdRenderSettings prim = stage ? RenderSettingsPrim::activePrim(stage, std::string(), nullptr) : UsdRenderSettings();
        if (!prim) {
            return;
        }
        scenePrimPath = prim.GetPath();
        std::string id;
        std::string error;
        if (!RenderSettingsPrim::rendererFor(prim.GetPrim(), &id, &error)) {
            // A renderer that is not installed is kept, so that the view says why it cannot render.
            const UsdAttribute attribute = prim.GetPrim().GetAttribute(TfToken(kRenderSettingsPrimRendererAttribute));
            TfToken token;
            if (attribute && attribute.Get(&token)) {
                id = token.GetString();
            }
        }
        if (!id.empty()) {
            sceneRenderer = id;
        }
    }

    // The delegate renders are made with: the one chosen, or for Scene Default the prim's. Needs stageMutex.
    std::string effectiveRenderer()
    {
        if (rendererChoice != kRenderDelegateSceneDefaultId) {
            return rendererChoice;
        }
        refreshScenePrim();

        return sceneRenderer;
    }

    // The active prim's settings for renderer at the time shown. Needs stageMutex.
    const RenderSettingsPrim::Values& sceneSettingsFor(const std::string& renderer)
    {
        refreshScenePrim();
        const double time = timeCode();
        if (sceneSettingsValid && (sceneSettingsEditCount == scenePrimEditCount) && (sceneSettingsRenderer == renderer) && (sceneSettingsTime == time)) {
            return sceneSettings;
        }
        sceneSettingsValid = true;
        sceneSettingsEditCount = scenePrimEditCount;
        sceneSettingsRenderer = renderer;
        sceneSettingsTime = time;
        sceneSettings.clear();
        const UsdStageRefPtr stage = currentStage();
        if (stage && !scenePrimPath.IsEmpty()) {
            sceneSettings = RenderSettingsPrim::settings(stage->GetPrimAtPath(scenePrimPath), renderer, UsdTimeCode(time));
        }

        return sceneSettings;
    }

    // Gives the Storm engine the active prim's settings for it, as a USDRender's engine has them.
    // Unlike that engine it is kept across a change: remaking it would resync the whole stage on
    // each edit, so a setting the prim stops authoring goes back to the delegate's default. Needs
    // stageMutex.
    void applyStormSettings()
    {
        const RenderSettingsPrim::Values& wanted = sceneSettingsFor(rendererId);
        const bool sameEngine = stormSettingsEngine == engineCount;

        if (sameEngine && (wanted == stormSettings)) {
            return;
        }
        if (sameEngine) {
            const UsdImagingGLRendererSettingsList declared = engine->GetRendererSettingsList();
            for (const std::pair<const std::string, VtValue>& setting : stormSettings) {
                if (wanted.count(setting.first)) {
                    continue;
                }
                const TfToken key(setting.first);
                VtValue value;
                for (const UsdImagingGLRendererSetting& d : declared) {
                    if (d.key == key) {
                        value = d.defValue;
                        break;
                    }
                }
                engine->SetRendererSetting(key, value);
            }
        }
        for (const std::pair<const std::string, VtValue>& setting : wanted) {
            engine->SetRendererSetting(TfToken(setting.first), setting.second);
        }
        stormSettings = wanted;
        stormSettingsEngine = engineCount;
    }

    /**
     * @brief Renders with renderer from the next render on: a change destroys the engine, so it
     * needs the context of render() current. Needs stageMutex.
     **/
    void switchRenderer(const std::string& renderer)
    {
        if (renderer == rendererId) {
            return;
        }
        {
            std::lock_guard<std::mutex> idLock(rendererIdMutex);
            rendererId = renderer;
        }
        setRendererErrorCopy(std::string());
        setRefineErrorCopy(std::string());
        uploadedFrame.reset();
        dropEngine();
        // The last delegate's refinement engine goes; another has its own default cap, and starts
        // refining.
        progressive->setRenderer(std::string());
        refineRenderer.clear();
    }

    std::shared_ptr<const StageSummary> getStageSummary() const
    {
        std::lock_guard<std::mutex> lock(summaryMutex);

        return stageSummary;
    }

    // Traverses the stage if it changed since the last summary. Needs stageMutex, shared being
    // enough: readers carry on meanwhile.
    void refreshStageSummary()
    {
        const UsdStageRefPtr stage = currentStage();
        std::shared_ptr<StageSummary> summary;

        if (stage) {
            // Loads compose prims too; both counts only grow, so their sum moves with either.
            const U64 editCount = sceneStage->getEditCount() + sceneStage->getLoadEditCount();
            const std::shared_ptr<const StageSummary> current = getStageSummary();
            if (current && (current->editCount == editCount)) {
                return;
            }
            summary = std::make_shared<StageSummary>();
            summary->editCount = editCount;
            summary->guidePrims = SceneGuides::findGuidePrims(stage, &summary->hasLights);
            for (const SceneGuides::GuidePrim& guidePrim : summary->guidePrims) {
                if (guidePrim.kind == SceneGuides::eGuideKindCamera) {
                    const UsdPrim prim = stage->GetPrimAtPath(guidePrim.path);
                    if (prim && !prim.IsInstanceProxy()) {
                        summary->cameraPaths.push_back(guidePrim.path.GetString());
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(summaryMutex);
        stageSummary = summary;
    }

    double guideLength() const
    {
        const double size = guideSize.load();

        return (size > 0.) ? size : 1.;
    }

    // The guides at the shown time, built again when the stage or the time changed. Needs
    // stageMutex.
    const std::vector<SceneGuides::Guide>& currentGuides()
    {
        const std::shared_ptr<const StageSummary> summary = getStageSummary();
        const double time = timeCode();
        const double length = guideLength();
        if (!guidesValid || (guidesSummary != summary) || (guidesTimeCode != time) || (guidesLength != length) || (guidesRulesVersion != drawRulesVersion)) {
            const UsdStageRefPtr stage = currentStage();
            guides.clear();
            if (stage && summary) {
                // A Hidden row takes its cameras' and lights' guides with it.
                std::vector<SceneGuides::GuidePrim> shown;
                shown.reserve(summary->guidePrims.size());
                for (const SceneGuides::GuidePrim& prim : summary->guidePrims) {
                    if (!SceneDrawFilter::isUnderHiddenRow(drawRules, prim.path)) {
                        shown.push_back(prim);
                    }
                }
                guides = SceneGuides::buildGuides(stage, shown, UsdTimeCode(time), length);
            }
            guidesRulesVersion = drawRulesVersion;
            guidesSummary = summary;
            guidesTimeCode = time;
            guidesLength = length;
            guidesValid = true;
            guideBatchesValid = false;
        }

        return guides;
    }

    const std::vector<int>* currentGuideBatches()
    {
        const std::vector<SceneGuides::Guide>& all = currentGuides();
        if (!guideBatchesValid) {
            for (std::vector<int>& batch : guideBatches) {
                batch.clear();
            }
            for (int i = 0; i < (int)all.size(); ++i) {
                const SceneGuides::Guide& guide = all[i];
                if (guide.path == lookThrough) {
                    continue;
                }
                const bool isCamera = (guide.kind == SceneGuides::eGuideKindCamera);
                guideBatches[isSelected(guide.path) ? 2 : (isCamera ? 0 : 1)].push_back(i);
            }
            guideBatchesValid = true;
        }

        return guideBatches;
    }

    bool isSelected(const std::string& path) const
    {
        return std::find(selected.begin(), selected.end(), path) != selected.end();
    }

    // Whether the shaded mode is lit by the stage's lights.
    bool usesSceneLights()
    {
        return usesSceneLights(lightingMode);
    }

    bool usesSceneLights(LightingModeEnum mode)
    {
        switch (mode) {
        case eLightingModeAuto: {
            const std::shared_ptr<const StageSummary> summary = getStageSummary();

            return summary && summary->hasLights && !everyLightHidden;
        }
        case eLightingModeScene:
            return !everyLightHidden;
        case eLightingModeHeadlight:
            return false;
        }

        return false;
    }

    // Hydra holds no light of the stage once every one is under a Hidden row, and nothing lights
    // the scene then. A stage with no light at all stays dark under Scene Lights, as hiding a mesh
    // must not light it. The headlight is a light in the filter's input too, so only stage prims
    // count, or adding it would reverse this on the next pass. Needs stageMutex and the engine.
    bool hidesEveryLight() const
    {
        const SceneDrawFilter* filter = get_pointer(drawFilter);
        if (!filter || !filter->hidesLights()) {
            return false;
        }
        const std::shared_ptr<const StageSummary> summary = getStageSummary();
        const UsdStageRefPtr stage = currentStage();
        if (!summary || !summary->hasLights || !stage) {
            return false;
        }

        return !filter->hasDrawnLights([&stage](const SdfPath& light) {
            return bool(stage->GetPrimAtPath(light));
        });
    }

    // Needs stageMutex and the engine. The main thread reads the result for its render params,
    // and may touch neither the filter nor the stage.
    void refreshLightsHidden()
    {
        everyLightHidden = hidesEveryLight();
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

    // Needs stageMutex when looking through a camera.
    UsdGeomCamera lookThroughCamera() const
    {
        if (lookThrough.empty() || !SdfPath::IsValidPathString(lookThrough)) {
            return UsdGeomCamera();
        }
        const UsdStageRefPtr stage = currentStage();
        if (!stage) {
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
        // Transparent and scene-linear: drawGrade() grades what was rendered and puts it over the
        // background. Hydra's colour correction would grade premultiplied values, which a
        // transform that is not linear gets wrong wherever alpha is between 0 and 1.
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = true;
        params.showProxy = showProxy;
        params.showRender = showRender;
        params.showGuides = showGuidePurpose;
        params.enableSceneLights = sceneLit;
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

    // Needs an engine.
    void applySelection()
    {
        // The engine highlights differently from now on, so a kept image is out of date.
        backgroundFrame.imageKept = false;
        engineSelectionDirty = false;
        SdfPathVector paths;
        {
            std::lock_guard<std::mutex> lock(engineSelectionMutex);
            for (const std::string& path : engineSelection) {
                if (SdfPath::IsValidPathString(path)) {
                    paths.push_back(SdfPath(path));
                }
            }
        }
        if (paths.empty()) {
            engine->ClearSelected();
        } else {
            engine->SetSelected(paths);
        }
    }

    // Needs the GL context current, stageMutex and a StormGLSection, as the render that threw has
    // them. Hydra's state after an exception is unknown, so the engine goes and the next render
    // makes another.
    void discardFailedEngine(const std::string& what)
    {
        engine.reset();
        engineHeld = false;
        backgroundFrame.imageKept = false;
        setRenderErrorCopy("The " + rendererId + " render failed: " + what);
    }

    // Nothing above a paint catches what a delegate throws.
    void engineFailedToStart(const std::string& what)
    {
        engine.reset();
        engineHeld = false;
        setRendererErrorCopy("The renderer " + rendererId + " could not be started: " + what + ".");
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
            if (engineSelectionDirty) {
                applySelection();
            }

            return true;
        }
        if (!getRendererErrorCopy().empty()) {
            return false;
        }
        setRendererErrorCopy(RenderDelegates::unavailableMessage(rendererId));
        if (!getRendererErrorCopy().empty()) {
            return false;
        }
        // Named even for Storm: with no id the engine takes the first delegate by priority that
        // loads, which need not be Storm.
        UsdImagingGLEngine::Parameters parameters;
        parameters.rendererPluginId = TfToken(rendererId);
        try {
            SceneDrawFilter::registerForAllRenderers();
            (void)SceneDrawFilter::takeAppendedInstanceId();
            CyclesAovs::registerSceneIndices();
            {
                TfErrorMark mark;
                engine.reset(new UsdImagingGLEngine(parameters));
                ++engineCount;
                ++engineConstructionCount;
                if (engine->GetCurrentRendererId() != parameters.rendererPluginId) {
                    std::string reasons;
                    for (TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
                        reasons += (reasons.empty() ? ": " : "; ") + it->GetCommentary();
                    }
                    mark.Clear();
                    engine.reset();
                    setRendererErrorCopy("The renderer " + rendererId + " could not be started" + reasons + ".");

                    return false;
                }
            }
            if (RenderDelegates::isStorm(rendererId)) {
                applyStormPtexBudget(engine->GetHgi());
            }
            // drawGrade() reads the colour AOV, and the depth AOV Storm keeps with it, in place of
            // Hydra's presentation.
            engine->SetRendererAov(HdAovTokens->color);
            engine->SetEnablePresentation(false);
            engine->SetSelectionColor(GfVec4f(1.f, 0.8f, 0.1f, 1.f));
            applySelection();
        } catch (const std::exception& e) {
            engineFailedToStart(e.what());

            return false;
        } catch (...) {
            engineFailedToStart("unknown exception");

            return false;
        }
        drawFilter = SceneDrawFilter::find(SceneDrawFilter::takeAppendedInstanceId());
        if (SceneDrawFilter* filter = get_pointer(drawFilter)) {
            filter->seedRules(drawRules);
        }
        engineHeld = true;

        return true;
    }

    std::string getRendererErrorCopy() const
    {
        std::lock_guard<std::mutex> lock(rendererErrorMutex);

        return rendererError;
    }

    void setRendererErrorCopy(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(rendererErrorMutex);

        rendererError = error;
    }

    void setRefineErrorCopy(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(rendererErrorMutex);

        refineError = error;
    }

    void setRenderErrorCopy(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(rendererErrorMutex);

        renderError = error;
    }

    // Needs stageMutex and a current GL context. Lets the engine's GL objects go; no Hgi to collect
    // with, as the engine's own goes with it, collecting as it does.
    void dropEngine()
    {
        StormGLSection section;

        engine.reset();
        drawFilter = SceneDrawFilter::WeakPtr();
        drawRevealing = false;
        everyLightHidden = false;
        engineHeld = false;
        backgroundFrame.imageKept = false;
    }

    // Needs stageMutex.
    void noteStageAbsorbed(const UsdImagingGLRenderParams& params)
    {
        if (sceneStage) {
            absorbedEditCount = sceneStage->getEditCount();
            absorbedLoadEditCount = sceneStage->getLoadEditCount();
        }
        absorbedParamsKey = paramsKey(params);
        drawAbsorbed = drawApplied.load();
        if (!stageNoticesRegistered && sceneStage && sceneStage->getStage()) {
            stageNoticesRegistered = true;
            TfNotice::Register(TfCreateWeakPtr(&stageNotices), &StageNoticeCounter::onObjectsChanged, UsdStageWeakPtr(sceneStage->getStage()));
        }
        refreshDrawStats();
    }

    // The render params a paint draws with, lit as the stage's summary says.
    UsdImagingGLRenderParams currentRenderParams()
    {
        UsdImagingGLRenderParams params = renderParams();

        params.enableSceneLights = usesSceneLights();

        return params;
    }

    // What of the params changes Storm's shaders or what it draws. Changing it costs a render
    // like a stage change does, the compiles taking seconds on a scene of many materials.
    static std::size_t paramsKey(const UsdImagingGLRenderParams& params)
    {
        std::size_t key = 0;
        const auto add = [&key](std::size_t value) { key ^= value + 0x9e3779b97f4a7c15ULL + (key << 6) + (key >> 2); };

        add(std::size_t(params.drawMode));
        add(params.enableLighting);
        add(params.enableSceneLights);
        add(params.showProxy);
        add(params.showRender);
        add(params.showGuides);
        add(params.highlight);
        add(params.colorCorrectionMode.Hash());
        add(params.ocioDisplay.Hash());
        add(params.ocioView.Hash());
        add(params.ocioColorSpace.Hash());

        return key;
    }

    // Needs stageMutex. Reads the filter, which only the thread holding the engine may touch.
    void refreshDrawStats()
    {
        SceneDrawFilter* filter = get_pointer(drawFilter);
        std::lock_guard<std::mutex> lock(drawMutex);
        drawStats.filterBound = (filter != nullptr);
        drawStats.revealing = drawRevealing;
        drawStats.revealBatch = drawRevealPacer.nextBatch();
        drawStats.revealFixedMs = drawRevealPacer.getFixedMs();
        drawStats.revealPerPrimUs = drawRevealPacer.getPerPrimMs() * 1000.;
        drawStats.revealSteps = drawRevealSteps;
        drawStats.ruleCount = drawRules.size();
        if (!filter) {
            return;
        }
        drawStats.inputAdded = filter->getInputAddedCount();
        drawStats.inputRemoved = filter->getInputRemovedCount();
        drawStats.inputDirtied = filter->getInputDirtiedCount();
        drawStats.hiddenSent = filter->getHiddenSentCount();
        drawStats.shownSent = filter->getShownSentCount();
        drawStats.visibilitySent = filter->getVisibilitySentCount();
        drawStats.heldInvisible = filter->getHeldInvisibleCount();
        drawStats.maskedInstances = filter->getMaskedInstanceCount();
        if (drawStats.inputAdded != 0) {
            // The harness compares this with its own wall clock, and the filter keeps a steady one.
            const auto sinceAdded = SceneDrawFilter::Clock::now() - filter->getLastInputAddedTime();
            const auto addedAt = std::chrono::system_clock::now() - std::chrono::duration_cast<std::chrono::system_clock::duration>(sinceAdded);
            drawStats.lastInputAddedEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(addedAt.time_since_epoch()).count();
        }
    }

    void noteCameraMoved()
    {
        cameraMovedAt = std::chrono::steady_clock::now().time_since_epoch().count();
    }

    bool yieldsToCamera() const
    {
        const long long movedAt = cameraMovedAt;

        return (movedAt != 0)
            && ((std::chrono::steady_clock::now().time_since_epoch().count() - movedAt)
                < std::chrono::steady_clock::duration(std::chrono::milliseconds(kRevealCameraYieldMs)).count());
    }

    // Needs stageMutex, the GL context current and an engine, between renders: the notices reach the
    // render index directly. A reveal step goes only into a frame, which is what shows it and what
    // its cost is timed by.
    void serviceDrawFilter(bool mayStep)
    {
        std::vector<QueuedDrawDelta> queued;
        U64 posted = 0;
        {
            std::lock_guard<std::mutex> lock(drawMutex);
            queued.swap(drawQueue);
            posted = drawPosted;
        }
        SceneDrawFilter* filter = get_pointer(drawFilter);
        SceneDrawTiming* timing = drawTiming.get();
        const U64 appliedBefore = drawApplied;
        unsigned long long hiddenBefore = 0;
        unsigned long long shownBefore = 0;
        if (timing && (!queued.empty() || (mayStep && filter && filter->isRevealing() && drawRevealStepDue))) {
            timing->applyStarting();
            if (filter) {
                hiddenBefore = filter->getHiddenSentCount();
                shownBefore = filter->getShownSentCount();
            }
        }
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        // One call for the lot, so a prim that several queued edits touch reaches Hydra once, or not
        // at all if it ends where it started.
        std::vector<const SceneDrawDelta*> deltas;
        deltas.reserve(queued.size());
        for (const QueuedDrawDelta& entry : queued) {
            SceneDrawFilter::applyToRules(entry.delta, &drawRules);
            drawRevealBatch = entry.revealBatch;
            deltas.push_back(&entry.delta);
        }
        if (!queued.empty()) {
            ++drawRulesVersion;
        }
        if (!queued.empty() && boxes) {
            boxes->setRules(drawRules);
        }
        if (!queued.empty()) {
            if (filter) {
                filter->applyDeltas(deltas, drawRevealBatch);
            }
            drawRevealStepDue = true;
        }
        std::size_t stepped = 0;
        drawRevealStepTimed = false;
        if (mayStep && filter && filter->isRevealing() && !yieldsToCamera() && drawRevealStepDue.exchange(false)) {
            const bool paced = (drawRevealBatch == SceneViewportRenderer::kRevealPaced);
            const std::chrono::steady_clock::time_point stepStart = std::chrono::steady_clock::now();
            stepped = filter->stepReveal(paced ? drawRevealPacer.nextBatch() : drawRevealBatch,
                                         stepStart + std::chrono::milliseconds(kRevealStepDeadlineMs));
            ++drawRevealSteps;
            // A step the deadline cut short says nothing of what its prims cost.
            drawRevealStepTimed = paced && (stepped != 0) && !filter->wasStepCut();
            drawRevealStepPrims = stepped;
            drawRevealStepStart = stepStart;
        }
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        drawRevealing = filter && filter->isRevealing();
        drawApplied = posted;
        if (!queued.empty() || (stepped != 0)) {
            std::lock_guard<std::mutex> lock(drawMutex);
            drawStats.lastApplyMs = ms;
        }
        if (timing && (!queued.empty() || (stepped != 0))) {
            std::chrono::steady_clock::time_point startedAt = start;
            for (const QueuedDrawDelta& entry : queued) {
                startedAt = (std::min)(startedAt, entry.startedAt);
            }
            timing->applied(appliedBefore + 1, posted, startedAt, start, end,
                            filter ? (filter->getHiddenSentCount() - hiddenBefore) : 0,
                            filter ? (filter->getShownSentCount() - shownBefore) : 0, stepped != 0);
        }
        refreshLightsHidden();
        refreshDrawStats();
    }

    // Needs stageMutex. After the renders of a frame: a load landing in one starts a reveal there.
    void noteDrawFilterRendered(bool rendered)
    {
        SceneDrawFilter* filter = get_pointer(drawFilter);
        if (!filter) {
            return;
        }
        filter->endRender();
        if (drawRevealStepTimed && rendered) {
            drawRevealPacer.observe(drawRevealStepPrims, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawRevealStepStart).count());
        }
        drawRevealStepTimed = false;
        drawRevealing = filter->isRevealing();
    }

    // The GL state both kinds of frame start from.
    static void bindFrameTarget(unsigned int fbo,
                                int width,
                                int height)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
    }

    static void clearFrameTarget(const float background[3])
    {
        glClearColor(background[0], background[1], background[2], 1.f);
        glClearDepth(1.);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // Takes the image a background frame left, if it is what this render would draw. It is good for
    // one render: the overlays go on top of it. Needs stageMutex.
    bool takeBackgroundImage(unsigned int fbo,
                             int width,
                             int height,
                             const GfFrustum& frustum,
                             const UsdImagingGLRenderParams& params)
    {
        BackgroundFrame& frame = backgroundFrame;
        const bool kept = frame.imageKept;

        frame.imageKept = false;

        return kept && sceneStage && (frame.fbo == fbo) && (frame.width == width) && (frame.height == height)
            && (frame.frustum.ComputeViewMatrix() == frustum.ComputeViewMatrix())
            && (frame.frustum.ComputeProjectionMatrix() == frustum.ComputeProjectionMatrix())
            && (frame.params == params)
            && std::equal(frame.background, frame.background + 3, backgroundShown)
            && (frame.displayProcessor == displayProcessor) && (frame.gain == displayTransform.gain) && (frame.gamma == displayTransform.gamma)
            && (frame.editCount == sceneStage->getEditCount())
            && (frame.loadEditCount == sceneStage->getLoadEditCount())
            && (frame.previewGeneration == sceneStage->getPreviewGeneration());
    }

    // Needs the GL context current.
    void releaseGrade()
    {
        if (gradeProgram) {
            glDeleteProgram(gradeProgram);
            gradeProgram = 0;
        }
        for (const GradeLut& lut : gradeLuts) {
            glDeleteTextures(1, &lut.id);
        }
        gradeLuts.clear();
        gradeShader.reset();
        gradeBuilt = false;
        gradeProcessor.reset();
    }

    // Needs the GL context current. Throws what OpenColorIO throws.
    void uploadLuts(const OCIO_NAMESPACE::GpuShaderDescRcPtr& desc)
    {
        // A bound unpack buffer would be read as the LUT's source.
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        for (unsigned idx = 0; idx < desc->getNum3DTextures(); ++idx) {
            const char* textureName = nullptr;
            const char* samplerName = nullptr;
            unsigned edgelen = 0;
            OCIO_NAMESPACE::Interpolation interpolation = OCIO_NAMESPACE::INTERP_LINEAR;
            desc->get3DTexture(idx, textureName, samplerName, edgelen, interpolation);
            const float* values = nullptr;
            desc->get3DTextureValues(idx, values);
            if (!samplerName || !values || (edgelen == 0)) {
                throw std::runtime_error("an OpenColorIO 3D LUT has no data");
            }
            GradeLut lut { 0, GL_TEXTURE_3D, samplerName };
            glGenTextures(1, &lut.id);
            gradeLuts.push_back(lut);
            glActiveTexture((GLenum)(GL_TEXTURE0 + kFirstLutUnit + gradeLuts.size() - 1));
            glBindTexture(GL_TEXTURE_3D, lut.id);
            setLutParameters(GL_TEXTURE_3D, interpolation);
            glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F, edgelen, edgelen, edgelen, 0, GL_RGB, GL_FLOAT, values);
        }
        for (unsigned idx = 0; idx < desc->getNumTextures(); ++idx) {
            const char* textureName = nullptr;
            const char* samplerName = nullptr;
            unsigned width = 0;
            unsigned height = 0;
            OCIO_NAMESPACE::GpuShaderDesc::TextureType channel = OCIO_NAMESPACE::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
            OCIO_NAMESPACE::Interpolation interpolation = OCIO_NAMESPACE::INTERP_LINEAR;
#if OCIO_VERSION_HEX >= 0x02030000
            OCIO_NAMESPACE::GpuShaderCreator::TextureDimensions dimensions = OCIO_NAMESPACE::GpuShaderCreator::TEXTURE_2D;
            desc->getTexture(idx, textureName, samplerName, width, height, channel, dimensions, interpolation);
            const bool is1D = (dimensions == OCIO_NAMESPACE::GpuShaderCreator::TEXTURE_1D);
#else
            desc->getTexture(idx, textureName, samplerName, width, height, channel, interpolation);
            const bool is1D = (height <= 1);
#endif
            const float* values = nullptr;
            desc->getTextureValues(idx, values);
            if (!samplerName || !values || (width == 0)) {
                throw std::runtime_error("an OpenColorIO LUT has no data");
            }
            const bool red = (channel == OCIO_NAMESPACE::GpuShaderCreator::TEXTURE_RED_CHANNEL);
            const GLint internalFormat = red ? GL_R32F : GL_RGB32F;
            const GLenum format = red ? GL_RED : GL_RGB;
            GradeLut lut { 0, (GLenum)(is1D ? GL_TEXTURE_1D : GL_TEXTURE_2D), samplerName };
            glGenTextures(1, &lut.id);
            gradeLuts.push_back(lut);
            glActiveTexture((GLenum)(GL_TEXTURE0 + kFirstLutUnit + gradeLuts.size() - 1));
            glBindTexture(lut.target, lut.id);
            setLutParameters(lut.target, interpolation);
            if (is1D) {
                glTexImage1D(GL_TEXTURE_1D, 0, internalFormat, width, 0, format, GL_FLOAT, values);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, std::max(height, 1u), 0, format, GL_FLOAT, values);
            }
        }
        glActiveTexture(GL_TEXTURE0);
    }

    // Needs the GL context current. False, with why in *error, when OpenColorIO or the driver
    // cannot make the pass of processor (null for the sRGB encoding); nothing is kept then.
    bool buildGrade(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                    std::string* error)
    {
        releaseGrade();
        try {
            std::string displayFunction = kSrgbDisplayFunction;
            OCIO_NAMESPACE::GpuShaderDescRcPtr desc;
            if (processor) {
                desc = OCIO_NAMESPACE::GpuShaderDesc::CreateShaderDesc();
                desc->setLanguage(OCIO_NAMESPACE::GPU_LANGUAGE_GLSL_4_0);
                desc->setFunctionName("toDisplay");
                desc->setResourcePrefix("ocio_");
                processor->getDefaultGPUProcessor()->extractGpuShaderInfo(desc);
                displayFunction = desc->getShaderText();
                uploadLuts(desc);
            }
            gradeProgram = linkProgram(kGradeVertexShader, gradeFragmentShader(displayFunction));
            gradeShader = desc;
        } catch (const std::exception& e) {
            *error = e.what();
            releaseGrade();

            return false;
        }

        return true;
    }

    // Needs the GL context current. A transform the pass cannot draw falls back to the sRGB
    // encoding, once per change of the transform.
    void ensureGrade(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor)
    {
        if (gradeBuilt && (gradeProcessor == processor)) {
            return;
        }
        std::string error;
        if (!buildGrade(processor, &error)) {
            qWarning() << "SceneViewport: the display transform cannot be drawn, showing sRGB instead:" << error.c_str();
            if (processor && !buildGrade(OCIO_NAMESPACE::ConstProcessorRcPtr(), &error)) {
                qWarning() << "SceneViewport: the sRGB pass cannot be drawn either:" << error.c_str();
            }
        }
        gradeBuilt = true;
        gradeProcessor = processor;
    }

    // Needs the GL context current and an engine that has rendered, whose program, vertex array
    // and texture bindings it changes.
    void drawGrade(unsigned int fbo,
                   int width,
                   int height,
                   const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                   double gain,
                   double gamma)
    {
        ensureGrade(processor);
        const HgiTextureHandle color = engine->GetAovTexture(HdAovTokens->color);
        if (!gradeProgram || !color) {
            return;
        }
        const HgiTextureHandle depth = engine->GetAovTexture(HdAovTokens->depth);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

        glUseProgram(gradeProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, 0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)color->GetRawResource());
        glUniform1i(glGetUniformLocation(gradeProgram, "colorAov"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindSampler(1, 0);
        glBindTexture(GL_TEXTURE_2D, depth ? (GLuint)depth->GetRawResource() : 0);
        glUniform1i(glGetUniformLocation(gradeProgram, "depthAov"), 1);
        glUniform1i(glGetUniformLocation(gradeProgram, "hasDepth"), depth ? 1 : 0);
        glUniform1f(glGetUniformLocation(gradeProgram, "gain"), (GLfloat)gain);
        glUniform1f(glGetUniformLocation(gradeProgram, "gamma"), (GLfloat)gamma);
        for (std::size_t i = 0; i < gradeLuts.size(); ++i) {
            const GLint unit = kFirstLutUnit + (GLint)i;
            glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
            glBindSampler((GLuint)unit, 0);
            glBindTexture(gradeLuts[i].target, gradeLuts[i].id);
            glUniform1i(glGetUniformLocation(gradeProgram, gradeLuts[i].sampler.c_str()), unit);
        }
        if (gradeShader) {
            for (unsigned i = 0; i < gradeShader->getNumUniforms(); ++i) {
                OCIO_NAMESPACE::GpuShaderDesc::UniformData data;
                const char* name = gradeShader->getUniform(i, data);
                const GLint location = name ? glGetUniformLocation(gradeProgram, name) : -1;
                if (location < 0) {
                    continue;
                }
                if (data.m_getDouble) {
                    glUniform1f(location, (GLfloat)data.m_getDouble());
                } else if (data.m_getBool) {
                    glUniform1i(location, data.m_getBool() ? 1 : 0);
                } else if (data.m_getFloat3) {
                    glUniform3f(location, data.m_getFloat3()[0], data.m_getFloat3()[1], data.m_getFloat3()[2]);
                } else if (data.m_vectorFloat.m_getSize && data.m_vectorFloat.m_getVector) {
                    glUniform1fv(location, (GLsizei)data.m_vectorFloat.m_getSize(), (const GLfloat*)data.m_vectorFloat.m_getVector());
                } else if (data.m_vectorInt.m_getSize && data.m_vectorInt.m_getVector) {
                    glUniform1iv(location, (GLsizei)data.m_vectorInt.m_getSize(), (const GLint*)data.m_vectorInt.m_getVector());
                }
            }
        }

        glDrawArrays(GL_TRIANGLES, 0, 3);

        for (std::size_t i = 0; i < gradeLuts.size(); ++i) {
            glActiveTexture((GLenum)(GL_TEXTURE0 + kFirstLutUnit + i));
            glBindTexture(gradeLuts[i].target, 0);
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_BLEND);
        glDepthFunc(GL_LESS);
        glDisable(GL_DEPTH_TEST);
    }

    // A light at the camera, as usdview's default camera light, or none when the stage's own
    // lights light the scene (Storm adds those itself with enableSceneLights). No ambient then
    // either, so a scene without lights shows its lit surfaces black.
    void setLights(bool lit)
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
        const float ambient = lit ? 0.f : 0.01f;
        engine->SetLightingState(lit ? GlfSimpleLightVector() : GlfSimpleLightVector(1, light), material, GfVec4f(ambient, ambient, ambient, 1.f));
    }

    void setCamera(const GfFrustum& frustum,
                   int width,
                   int height,
                   bool lit)
    {
        engine->SetRenderBufferSize(GfVec2i(width, height));
        engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
        engine->SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());
        setLights(lit);
    }

    // Needs stageMutex: a camera looked through is a prim of the stage.
    SceneGizmo::View makeView(int width,
                              int height) const
    {
        const GfFrustum frustum = computeFrustum(width, height);

        return SceneGizmo::View(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
    }

    // The selected prims the transform widget moves. Needs stageMutex.
    std::vector<UsdPrim> transformablePrims() const
    {
        return SceneTransformWidget::getTransformablePrims(currentStage(), selected);
    }

    // The widget on the selection, while its tool shows handles. Needs stageMutex.
    bool computeSelectionFrame(SceneGizmo::Frame* frame) const
    {
        if ((transformTool == SceneTransform::eToolSelect) || !currentStage()) {
            return false;
        }

        return SceneTransformWidget::computeFrame(transformablePrims(), timeCode(), transformSpace, transformPivot, frame);
    }

    // The op's values after a drag by change of handle.
    void changeOpValues(int handle,
                        double change,
                        GfVec3d* translate,
                        GfVec3d* rotate,
                        GfVec3d* scale) const
    {
        *translate = GfVec3d(handles.translate[0], handles.translate[1], handles.translate[2]);
        *rotate = GfVec3d(handles.rotate[0], handles.rotate[1], handles.rotate[2]);
        *scale = GfVec3d(handles.scale[0], handles.scale[1], handles.scale[2]);
        if ((handle < 0) || (handle >= SceneGizmo::kNumHandles)) {
            return;
        }
        const int axis = SceneGizmo::getHandleAxis(handle);
        switch (SceneGizmo::getHandleKind(handle)) {
        case SceneGizmo::eHandleKindTranslate:
            (*translate)[axis] += change;
            break;
        case SceneGizmo::eHandleKindRotate:
            (*rotate)[axis] += change;
            break;
        case SceneGizmo::eHandleKindScale:
            (*scale)[axis] *= change;
            break;
        }
    }

    GfMatrix4d composeOp(const GfVec3d& translate,
                         const GfVec3d& rotate,
                         const GfVec3d& scale) const
    {
        return SceneNodeAuthoring::composeTransformMatrix(translate, rotate, scale * handles.uniformScale,
                                                          GfVec3d(handles.pivot[0], handles.pivot[1], handles.pivot[2]),
                                                          (SceneRotateOrderEnum)handles.rotateOrder);
    }

    // The space placing the result of opName on primPath. Needs stageMutex.
    bool computeTargetSpace(const std::string& primPath,
                            const std::string& opName,
                            GfMatrix4d* space) const
    {
        const UsdStageRefPtr stage = currentStage();
        if (!stage || !SdfPath::IsValidPathString(primPath)) {
            return false;
        }
        const SdfPath path(primPath);
        if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
            return false;
        }

        return SceneGizmo::computeOpSpace(stage->GetPrimAtPath(path), opName, timeCode(), space);
    }

    // Where an op's handles sit at the shown time, moved by a drag of them under way. Needs
    // stageMutex.
    bool computeOpFrame(SceneGizmo::Frame* frame) const
    {
        if (!hasHandles) {
            return false;
        }
        const bool dragged = (dragHandle != SceneGizmo::kNoHandle) && !dragSelection;
        GfMatrix4d space = dragOpSpace;
        if (!dragged && !computeTargetSpace(handles.primPath, handles.opName, &space)) {
            return false;
        }
        GfVec3d translate, rotate, scale;
        changeOpValues(dragged ? dragHandle : SceneGizmo::kNoHandle, dragChange, &translate, &rotate, &scale);
        *frame = SceneGizmo::computeFrame(space, translate, rotate,
                                          GfVec3d(handles.pivot[0], handles.pivot[1], handles.pivot[2]),
                                          (SceneRotateOrderEnum)handles.rotateOrder);

        return true;
    }

    /**
     * @brief Where the handles drawn sit at the shown time: the selection's widget, else an op's.
     * *selection tells which. False if none are drawn. Needs stageMutex.
     **/
    bool computeHandleFrame(SceneGizmo::Frame* frame,
                            bool* selection) const
    {
        if (computeSelectionFrame(frame)) {
            *selection = true;

            return true;
        }
        *selection = false;

        return computeOpFrame(frame);
    }

    bool computeHandleFrame(SceneGizmo::Frame* frame) const
    {
        bool selection = false;

        return computeHandleFrame(frame, &selection);
    }

    // The handles drawn and picked, length world units long. Needs stageMutex.
    std::vector<SceneGizmo::HandleShape> buildDrawnHandles(const SceneGizmo::Frame& frame,
                                                           bool selection,
                                                           double length) const
    {
        std::vector<SceneGizmo::HandleShape> shapes = SceneGizmo::buildHandles(frame, length);

        return selection ? SceneTransformWidget::filterHandles(shapes, transformTool) : shapes;
    }

    bool isDrawnHandle(int handle,
                       bool selection) const
    {
        return selection ? SceneTransformWidget::isToolHandle(transformTool, handle) : ((handle >= 0) && (handle < SceneGizmo::kNumHandles));
    }

    // Drops the drag preview. Needs stageMutex.
    void clearPreviewLocked()
    {
        previewClearAfter = kNoPreviewClear;
        if (sceneStage && sceneStage->hasPreviewTransforms()) {
            sceneStage->clearPreviewTransforms();
            guidesValid = false;
            progressive->invalidate();
        }
    }

    // The handles in X red, Y green and Z blue, the highlighted or dragged one yellow, drawn over
    // everything. Needs stageMutex.
    void drawTransformHandles(const GfFrustum& frustum,
                              int width,
                              int height) const
    {
        SceneGizmo::Frame frame;
        bool selection = false;

        if (!computeHandleFrame(&frame, &selection)) {
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
        if (selection && (transformTool == SceneTransform::eToolScale)) {
            // Stems to the scale boxes, which are not handles themselves, as thick as the arrows.
            glLineWidth(3.f);
            for (const SceneGizmo::HandleShape& shape : SceneGizmo::buildHandles(frame, length)) {
                if (SceneGizmo::getHandleKind(shape.handle) != SceneGizmo::eHandleKindTranslate) {
                    continue;
                }
                const float* colour = axisColours[SceneGizmo::getHandleAxis(shape.handle)];
                glColor4f(colour[0], colour[1], colour[2], 0.85f);
                glBegin(GL_LINES);
                glVertex3d(frame.origin[0], frame.origin[1], frame.origin[2]);
                glVertex3d(shape.points[1][0], shape.points[1][1], shape.points[1][2]);
                glEnd();
            }
        }
        std::vector<SceneGizmo::HandleShape> shapes = buildDrawnHandles(frame, selection, length);
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

    void recordDrawnGuide(const SceneGizmo::View& view,
                          const SceneGuides::Guide& guide,
                          int width,
                          int height)
    {
        for (std::size_t k = 0; k + 1 < guide.segments.size(); k += 2) {
            GfVec2d a, b;
            if (!SceneGizmo::project(view, guide.segments[k], &a) || !SceneGizmo::project(view, guide.segments[k + 1], &b)) {
                continue;
            }
            const GfVec2d middle = (a + b) * 0.5;
            if ((middle[0] >= 0.) && (middle[0] < width) && (middle[1] >= 0.) && (middle[1] < height)) {
                DrawnGuide drawn;
                drawn.path = guide.path;
                drawn.kind = SceneGuides::getGuideKindName(guide.kind);
                drawn.x = middle[0];
                drawn.y = middle[1];
                drawnGuides.push_back(drawn);
                break;
            }
        }
    }

    // The camera and light guides, but the camera looked through's, as one batch of lines per
    // colour, depth-tested against Storm's presented depth; selected ones in the selection colour.
    // Needs stageMutex.
    void drawGuides(const GfFrustum& frustum,
                    int width,
                    int height)
    {
        drawnGuides.clear();
        const std::vector<SceneGuides::Guide>& all = currentGuides();
        if (all.empty()) {
            return;
        }
        const std::vector<int>* batches = currentGuideBatches();
        if (recordDrawnGuides) {
            const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
            for (int pass = 0; pass < 3; ++pass) {
                for (int index : batches[pass]) {
                    recordDrawnGuide(view, all[index], width, height);
                }
            }
        }

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

        // Unselected cameras, unselected lights, then the selection drawn thicker over them.
        for (int pass = 0; pass < 3; ++pass) {
            if (pass == 0) {
                glColor4f(0.45f, 0.85f, 0.85f, 1.f);
            } else if (pass == 1) {
                glColor4f(0.95f, 0.9f, 0.55f, 1.f);
            } else {
                glColor4f(1.f, 0.8f, 0.1f, 1.f);
            }
            glLineWidth((pass == 2) ? 2.f : 1.f);
            glBegin(GL_LINES);
            for (int index : batches[pass]) {
                for (const GfVec3d& p : all[index].segments) {
                    glVertex3d(p[0], p[1], p[2]);
                }
            }
            glEnd();
        }
        glLineWidth(1.f);

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
    } // drawGuides

    // Tells the box worker the frame's time and the stage's version. Needs stageMutex.
    void feedBoxes()
    {
        if (boxes) {
            boxes->setTime(timeCode());
            boxes->setStageVersion(stageEditCount + stageLoadEditCount);
        }
    }

    bool hasBoxes() const
    {
        const SceneDrawBoxes::ListPtr list = boxes ? boxes->getList() : SceneDrawBoxes::ListPtr();

        return list && !list->boxes.empty();
    }

    bool ensureBoxProgram()
    {
        if (boxProgram) {
            return true;
        }
        const GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
        const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(vertex, 1, &kBoxVertexShader, nullptr);
        glShaderSource(fragment, 1, &kBoxFragmentShader, nullptr);
        glCompileShader(vertex);
        glCompileShader(fragment);
        const GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            glDeleteProgram(program);

            return false;
        }
        boxProgram = program;
        const auto makeVao = [](GLuint* vao, GLuint* shape, GLuint* instances, const GLfloat* vertices, GLsizeiptr size) {
            glGenVertexArrays(1, vao);
            glGenBuffers(1, shape);
            glGenBuffers(1, instances);
            glBindVertexArray(*vao);
            glBindBuffer(GL_ARRAY_BUFFER, *shape);
            glBufferData(GL_ARRAY_BUFFER, size, vertices, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
            glBindBuffer(GL_ARRAY_BUFFER, *instances);
            for (GLuint attribute = 1; attribute <= 3; ++attribute) {
                glEnableVertexAttribArray(attribute);
                glVertexAttribPointer(attribute, 3, GL_FLOAT, GL_FALSE, kBoxFloats * sizeof(GLfloat), reinterpret_cast<const void*>((attribute - 1) * 3 * sizeof(GLfloat)));
                glVertexAttribDivisor(attribute, 1);
            }
        };
        makeVao(&boxVao, &boxEdgeBuffer, &boxInstanceBuffer, kBoxEdges, sizeof(kBoxEdges));
        makeVao(&markerVao, &markerArmBuffer, &markerInstanceBuffer, kMarkerArms, sizeof(kMarkerArms));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        uploadedBoxes.reset();

        return true;
    } // ensureBoxProgram

    // Which boxes hold a selected prim. Boxed rows never nest, so a selected prim deep in a boxed
    // branch lights the one box drawn for its nearest boxed ancestor.
    void findHighlightedBoxes(const SceneDrawBoxes::List& list)
    {
        boxHighlighted.assign(list.boxes.size(), false);
        if (selected.empty()) {
            return;
        }
        std::unordered_set<SdfPath, SdfPath::Hash> lit;
        for (const std::string& path : selected) {
            if (!SdfPath::IsValidPathString(path)) {
                continue;
            }
            for (SdfPath at(path); !at.IsEmpty() && !at.IsAbsoluteRootPath(); at = at.GetParentPath()) {
                if (!lit.insert(at).second) {
                    break;
                }
            }
        }
        for (std::size_t index = 0; index < list.boxes.size(); ++index) {
            boxHighlighted[index] = (lit.count(list.boxes[index].path) != 0);
        }
    }

    // The box's projected centre and screen rectangle, if its centre is in front of the eye.
    void recordDrawnBox(const SceneGizmo::View& view,
                        const SceneDrawBoxes::Box& box,
                        bool highlighted)
    {
        static const char* const kKindNames[] = { "box", "unloaded", "marker", "pending" };
        DrawnBox drawn;
        GfVec2d centre;
        if (!SceneGizmo::project(view, (GfVec3d(box.min) + GfVec3d(box.max)) * 0.5, &centre)) {
            return;
        }
        drawn.path = box.path.GetString();
        drawn.kind = kKindNames[box.kind];
        drawn.highlighted = highlighted;
        drawn.x = centre[0];
        drawn.y = centre[1];
        drawn.left = drawn.right = centre[0];
        drawn.top = drawn.bottom = centre[1];
        for (int corner = 0; corner < 8; ++corner) {
            const GfVec3d at((corner & 1) ? box.max[0] : box.min[0], (corner & 2) ? box.max[1] : box.min[1], (corner & 4) ? box.max[2] : box.min[2]);
            GfVec2d p;
            if (SceneGizmo::project(view, at, &p)) {
                drawn.left = std::min(drawn.left, p[0]);
                drawn.right = std::max(drawn.right, p[0]);
                drawn.top = std::min(drawn.top, p[1]);
                drawn.bottom = std::max(drawn.bottom, p[1]);
            }
        }
        drawnBoxes.push_back(drawn);
    }

    // One draw call for the boxes and one for the markers, however many, depth-tested against the
    // frame's depth like the guides. Unloaded rows are tinted; a pending row's marker is grey; a
    // box holding a selected prim takes the guides' selection colour.
    void drawBoxes(const GfFrustum& frustum,
                   int width,
                   int height)
    {
        const SceneDrawBoxes::ListPtr list = boxes ? boxes->getList() : SceneDrawBoxes::ListPtr();

        boxesDrawn = 0;
        markersDrawn = 0;
        drawnBoxList.reset();
        drawnBoxes.clear();
        if (!list || list->boxes.empty() || !ensureBoxProgram()) {
            return;
        }
        const bool coloursValid = boxColoursValid.exchange(true);
        if ((list != uploadedBoxes) || !coloursValid) {
            findHighlightedBoxes(*list);
            std::vector<GLfloat> boxData;
            std::vector<GLfloat> markerData;
            for (std::size_t index = 0; index < list->boxes.size(); ++index) {
                const SceneDrawBoxes::Box& box = list->boxes[index];
                GfVec3f colour(0.55f, 0.75f, 0.95f);
                if (boxHighlighted[index]) {
                    colour = GfVec3f(1.f, 0.8f, 0.1f);
                } else if ((box.kind == SceneDrawBoxes::eKindUnloaded) || (box.kind == SceneDrawBoxes::eKindMarker)) {
                    colour = GfVec3f(0.95f, 0.7f, 0.35f);
                } else if (box.kind == SceneDrawBoxes::eKindPending) {
                    colour = GfVec3f(0.6f, 0.6f, 0.6f);
                }
                std::vector<GLfloat>& data = ((box.kind == SceneDrawBoxes::eKindMarker) || (box.kind == SceneDrawBoxes::eKindPending)) ? markerData : boxData;
                data.insert(data.end(), { box.min[0], box.min[1], box.min[2], box.max[0], box.max[1], box.max[2], colour[0], colour[1], colour[2] });
            }
            glBindBuffer(GL_ARRAY_BUFFER, boxInstanceBuffer);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(boxData.size() * sizeof(GLfloat)), boxData.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, markerInstanceBuffer);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(markerData.size() * sizeof(GLfloat)), markerData.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            boxInstances = GLsizei(boxData.size() / kBoxFloats);
            markerInstances = GLsizei(markerData.size() / kBoxFloats);
            uploadedBoxes = list;
        }
        if ((boxInstances == 0) && (markerInstances == 0)) {
            return;
        }
        drawnBoxList = list;
        if (recordDrawnBoxes) {
            const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
            for (std::size_t index = 0; index < list->boxes.size(); ++index) {
                recordDrawnBox(view, list->boxes[index], boxHighlighted[index]);
            }
        }
        // GfMatrix4d is row-major with row vectors, so its memory is what GL reads as column-major
        // with column vectors, and view * projection here is projection * view there.
        const GfMatrix4f viewProjection(frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix());
        glUseProgram(boxProgram);
        glUniformMatrix4fv(glGetUniformLocation(boxProgram, "viewProjection"), 1, GL_FALSE, viewProjection.GetArray());
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glLineWidth(1.f);
        glUniform1i(glGetUniformLocation(boxProgram, "markers"), 0);
        glBindVertexArray(boxVao);
        glDrawArraysInstanced(GL_LINES, 0, 24, boxInstances);
        glUniform1i(glGetUniformLocation(boxProgram, "markers"), 1);
        glUniform2f(glGetUniformLocation(boxProgram, "markerScale"), 2.f * kMarkerPixels / float(width), 2.f * kMarkerPixels / float(height));
        glBindVertexArray(markerVao);
        glDrawArraysInstanced(GL_LINES, 0, 4, markerInstances);
        glBindVertexArray(0);
        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glDisable(GL_DEPTH_TEST);
        boxesDrawn = int(boxInstances);
        markersDrawn = int(markerInstances);
    } // drawBoxes

    void releaseBoxesGL()
    {
        if (boxProgram) {
            glDeleteProgram(boxProgram);
            glDeleteVertexArrays(1, &boxVao);
            glDeleteBuffers(1, &boxEdgeBuffer);
            glDeleteBuffers(1, &boxInstanceBuffer);
            glDeleteVertexArrays(1, &markerVao);
            glDeleteBuffers(1, &markerArmBuffer);
            glDeleteBuffers(1, &markerInstanceBuffer);
            boxProgram = 0;
            boxVao = 0;
            boxEdgeBuffer = 0;
            boxInstanceBuffer = 0;
            markerVao = 0;
            markerArmBuffer = 0;
            markerInstanceBuffer = 0;
        }
        uploadedBoxes.reset();
        drawnBoxList.reset();
        boxInstances = 0;
        markerInstances = 0;
    }

    // The drawn box or marker under the pixel (x, y) of the frustum's width x height view, -1 for
    // none. Needs stageMutex.
    int pickBox(const GfFrustum& frustum,
                int width,
                int height,
                int x,
                int y,
                GfVec3d* point) const
    {
        if (!drawnBoxList) {
            return -1;
        }

        return SceneDrawBoxes::pick(*drawnBoxList, frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix(), width, height,
                                    GfVec2d(x + 0.5, y + 0.5), kMarkerPixels + kMarkerPickSlack, point);
    }

    // Last, so that it goes first: its worker reads the members above.
    std::unique_ptr<SceneDrawBoxes> boxes;
};

SceneViewportRenderer::SceneViewportRenderer()
    : _imp(new Implementation)
{
    // A layer reload edits layers the stage composes: it waits until no paint, pick or sync is
    // reading the stage, and keeps them out until it is done.
    std::shared_mutex* stageMutex = &_imp->stageMutex;
    _imp->participant = SceneWriteBarrier::registerParticipant([stageMutex]() { stageMutex->lock(); },
                                                               [stageMutex]() { stageMutex->unlock(); });
    Implementation* imp = _imp.get();
    // The refinement reads the stage alongside paints, and like any other reader lets a failed
    // try to edit know when it lets go.
    HydraRender::ProgressiveRender::StageLock refineStageLock;
    refineStageLock.acquire = [stageMutex]() { stageMutex->lock_shared(); };
    refineStageLock.release = [imp, stageMutex]() {
        stageMutex->unlock_shared();
        if (imp->exclusiveOwed.exchange(false) && imp->readEnded) {
            imp->readEnded();
        }
    };
    _imp->progressive.reset(new HydraRender::ProgressiveRender(refineStageLock, [imp]() { return imp->currentStage(); }, [imp]() {
        std::function<void()> listener;
        {
            std::lock_guard<std::mutex> lock(imp->refineListenerMutex);
            listener = imp->refineListener;
        }
        if (listener) {
            listener();
        } }, kBackgroundLinear));
    _imp->progressive->setThreads(&refineThreads);
    // Like any other reader, the box worker lets a failed try to edit know when it lets go, and
    // ends its pass for a waiting edit.
    _imp->boxes.reset(new SceneDrawBoxes([imp](const SceneDrawBoxes::StageRead& read) {
        SceneWriteBarrier::ReadScope readScope;
        {
            std::shared_lock<std::shared_mutex> lock(imp->stageMutex);
            const bool shown = imp->shown && imp->sceneStage && imp->sceneStage->isValid();
            read(shown ? imp->sceneStage->getStage() : UsdStageRefPtr(), imp->stageEditCount + imp->stageLoadEditCount);
        }
        if (imp->exclusiveOwed.exchange(false) && imp->readEnded) {
            imp->readEnded();
        } }, [imp]() { return (imp->exclusiveWaiting > 0) || imp->exclusiveOwed; }, [imp]() {
        std::function<void()> listener;
        {
            std::lock_guard<std::mutex> lock(imp->refineListenerMutex);
            listener = imp->refineListener;
        }
        if (listener) {
            listener();
        } }));
}

SceneViewportRenderer::~SceneViewportRenderer()
{
    // Its worker reads the stage's layers under stageMutex: joined while the barrier still holds
    // writes back for it.
    _imp->progressive.reset();
    _imp->boxes.reset();
    SceneWriteBarrier::unregisterParticipant(_imp->participant);
    // The engine owns GL objects: whoever owns this must have called releaseGL() in its context.
    if (_imp->refineTexture || _imp->refineDepthBuffer) {
        qDebug() << "SceneViewportRenderer destroyed without releaseGL(); leaking its refinement texture";
    }
    if (_imp->gradeProgram || !_imp->gradeLuts.empty()) {
        qDebug() << "SceneViewportRenderer destroyed without releaseGL(); leaking its display transform pass";
    }
    if (_imp->engine) {
        qDebug() << "SceneViewportRenderer destroyed without releaseGL(); leaking its Storm engine";
        (void)_imp->engine.release();
    }
}

bool
SceneViewportRenderer::syncScene(const ScenePayload& payload,
                                 std::string* error,
                                 U64 serial)
{
    return syncScene(payload, -1., 0., error, serial);
}

bool
SceneViewportRenderer::syncScene(const ScenePayload& payload,
                                 double frame,
                                 double framesPerSecond,
                                 std::string* error,
                                 U64 serial)
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
    bool restart = false;
    bool unchanged = false;
    {
        // Playback and draw rule edits sync the stack the stage has: these leave the stage as it
        // is, so the paints that read it go on meanwhile.
        std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
        unchanged = _imp->sceneStage && _imp->sceneStage->isValid() && _imp->loadRulesMade && (_imp->shown == !payload.isEmpty())
            && !((serial != 0) && (serial > _imp->previewClearAfter.load())) && _imp->sceneStage->isSyncedTo(payload);
    }
    if (unchanged) {
        if (!_imp->takeSyncLoadSet(payload)) {
            return true;
        }
        if (framesPerSecond > 0.) {
            _imp->frame = frame;
            _imp->framesPerSecond = framesPerSecond;
        }
    } else {
        const std::unique_lock<std::shared_mutex> lock = _imp->lockExclusive();
        if (!_imp->sceneStage) {
            // The viewer shows what was asked for: nothing loads until a load is requested.
            _imp->sceneStage.reset(new SceneStage(UsdStage::LoadNone));
            std::lock_guard<std::mutex> earlyLock(_imp->earlyLoadsMutex);
            _imp->loadStage = _imp->sceneStage.get();
            for (const SceneDrawLoadRequest& load : _imp->earlyLoads) {
                _imp->sceneStage->requestLoad(toSdfPaths(std::vector<std::string>(1, load.path)), load.withDescendants, toSdfPaths(load.excluded));
            }
            _imp->earlyLoads.clear();
        }
        if (!_imp->sceneStage->isValid()) {
            _imp->shown = false;
            if (error) {
                *error = "OpenUSD could not open a stage for the scene.";
            }

            return false;
        }
        if (!_imp->loadRulesMade) {
            _imp->publishLoadRules();
        }
        if (!_imp->takeSyncLoadSet(payload)) {
            return true;
        }
        if ((serial != 0) && (serial > _imp->previewClearAfter.load())) {
            // In the same step as the sync, so no frame shows the scene without the drag.
            _imp->clearPreviewLocked();
        }
        // The active prim's path is left out: what the render takes of it reaches it through its
        // settings.
        SceneMetadata before = _imp->sceneStage->getPayload().getMetadata();
        SceneMetadata after = payload.getMetadata();
        before.renderSettingsPrimPath.clear();
        after.renderSettingsPrimPath.clear();
        StageChangePaths changes(_imp->sceneStage->getStage());
        if (_imp->sceneStage->sync(payload)) {
            // An edit of the render settings prims alone restarts nothing here: what of it the
            // render takes, the active prim's settings for its renderer, reaches it through
            // ProgressiveRender::setSettings(), which makes the engine again when they differ.
            restart = changes.outsideRender || !changes.any || (before != after);
        }
        if (framesPerSecond > 0.) {
            _imp->frame = frame;
            _imp->framesPerSecond = framesPerSecond;
        }
        const SceneMetadata& metadata = payload.getMetadata();
        _imp->zUp = (metadata.upAxis == SceneMetadata::eUpAxisZ);
        _imp->metersPerUnit = metadata.metersPerUnit;
        _imp->timeCodesPerSecond = metadata.timeCodesPerSecond;
        _imp->shown = !payload.isEmpty();
        _imp->stageEditCount = _imp->sceneStage->getEditCount();
    }
    if (restart) {
        _imp->progressive->invalidate();
    }
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
    _imp->refreshStageSummary();
    if (serial > _imp->stageSyncSerial) {
        _imp->stageSyncSerial = serial;
    }

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
    // Keeps the stage and the engine: the next scene is synced into them. The refinement engine
    // goes, as a view with no scene is not painted to let it go and its Cycles session would hold
    // the process' Cycles thread count.
    syncScene(ScenePayload(), nullptr);
    releaseRefinement();
}

bool
SceneViewportRenderer::hasScene() const
{
    return _imp->shown;
}

void
SceneViewportRenderer::readStage(const std::function<void(const SceneStage* stage)>& read) const
{
    SceneWriteBarrier::ReadScope readScope;
    {
        std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
        const bool shown = _imp->shown && _imp->sceneStage && _imp->sceneStage->isValid();
        read(shown ? _imp->sceneStage.get() : nullptr);
    }
    if (_imp->exclusiveOwed.exchange(false) && _imp->readEnded) {
        _imp->readEnded();
    }
}

bool
SceneViewportRenderer::tryReadStage(const std::function<void(const SceneStage* stage)>& read) const
{
    SceneWriteBarrier::ReadScope readScope;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        return false;
    }
    const bool shown = _imp->shown && _imp->sceneStage && _imp->sceneStage->isValid();
    read(shown ? _imp->sceneStage.get() : nullptr);

    return true;
}

void
SceneViewportRenderer::setReadEndedCallback(const std::function<void()>& callback)
{
    _imp->readEnded = callback;
}

U64
SceneViewportRenderer::getStageEditCount() const
{
    return _imp->stageEditCount;
}

U64
SceneViewportRenderer::getStageSyncSerial() const
{
    return _imp->stageSyncSerial;
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
    return _imp->engineCount.load();
}

bool
SceneViewportRenderer::hasEngine() const
{
    return _imp->engineHeld;
}

bool
SceneViewportRenderer::needsBackgroundFrame() const
{
    // Another delegate than Storm renders on the refinement's own thread.
    if (_imp->stageHeld || !_imp->shown || !_imp->loadStage.load() || !_imp->getRendererErrorCopy().empty() || (getActiveRendererId() != kRenderDelegateStormId)) {
        return false;
    }

    return !_imp->engineHeld || (_imp->absorbedEditCount != _imp->stageEditCount) || (_imp->absorbedLoadEditCount != _imp->stageLoadEditCount)
        || (_imp->absorbedParamsKey != Implementation::paramsKey(_imp->currentRenderParams()))
        || (_imp->drawAbsorbed != _imp->drawPosted) || (_imp->drawRevealing && _imp->drawRevealStepDue && !_imp->yieldsToCamera());
}

void
SceneViewportRenderer::postDrawDelta(const SceneDrawDelta& delta,
                                     std::size_t revealBatch,
                                     std::chrono::steady_clock::time_point startedAt)
{
    if (_imp->drawTiming && (startedAt == std::chrono::steady_clock::time_point())) {
        startedAt = std::chrono::steady_clock::now();
    }
    std::lock_guard<std::mutex> lock(_imp->drawMutex);
    _imp->drawQueue.push_back(Implementation::QueuedDrawDelta { delta, revealBatch, startedAt });
    ++_imp->drawPosted;
}

void
SceneViewportRenderer::applyPendingDrawDeltas()
{
    // Before the viewer lock, the order every reader takes them in.
    SceneWriteBarrier::ReadScope read;
    {
        std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
        // Without an engine the queue waits for the frame that makes one.
        if (_imp->engine) {
            StormGLStateGuard guard;
            StormGLSection section;
            section.collectWith(_imp->engine->GetHgi());
            _imp->serviceDrawFilter(false);
        }
    }
    if (_imp->exclusiveOwed.exchange(false) && _imp->readEnded) {
        _imp->readEnded();
    }
}

bool
SceneViewportRenderer::postDrawChange(const SceneDrawChange& change,
                                      std::size_t revealBatch)
{
    const SceneDrawDelta& delta = change.delta;
    // Loads alone go to the filter too when revealed, for it to know where what they bring in goes.
    const bool posted = !delta.drawn.empty() || !delta.expanded.empty() || !delta.boxed.empty() || !delta.hidden.empty() || !delta.cleared.empty()
        || ((revealBatch != 0) && !delta.loads.empty());

    if (posted) {
        postDrawDelta(delta, revealBatch, change.startedAt);
    }
    if (!change.cancelledLoads.empty()) {
        cancelStageLoads(change.cancelledLoads);
    }
    for (const SceneDrawLoadRequest& load : delta.loads) {
        requestStageLoad(std::vector<std::string>(1, load.path), load.withDescendants, load.excluded);
    }

    return posted;
}

bool
SceneViewportRenderer::isDrawRevealing() const
{
    return _imp->drawRevealing;
}

void
SceneViewportRenderer::noteDrawRevealPresented()
{
    _imp->drawRevealStepDue = true;
}

void
SceneViewportRenderer::setStageHeld(bool held)
{
    _imp->stageHeld = held;
}

bool
SceneViewportRenderer::isStageHeld() const
{
    return _imp->stageHeld;
}

int
SceneViewportRenderer::getRevealYieldMs() const
{
    const long long movedAt = _imp->cameraMovedAt;
    if (movedAt == 0) {
        return 0;
    }
    const std::chrono::steady_clock::duration since(std::chrono::steady_clock::now().time_since_epoch().count() - movedAt);
    const long long left = kRevealCameraYieldMs - std::chrono::duration_cast<std::chrono::milliseconds>(since).count();

    return (left > 0) ? int(left) : 0;
}

int
SceneViewportRenderer::getDrawnBoxCount() const
{
    return _imp->boxesDrawn;
}

int
SceneViewportRenderer::getDrawnMarkerCount() const
{
    return _imp->markersDrawn;
}

void
SceneViewportRenderer::setRecordDrawnBoxes(bool record)
{
    _imp->recordDrawnBoxes = record;
}

const std::vector<SceneViewportRenderer::DrawnBox>&
SceneViewportRenderer::getDrawnBoxes() const
{
    return _imp->drawnBoxes;
}

SceneViewportRenderer::DrawFilterStats
SceneViewportRenderer::getDrawFilterStats() const
{
    std::lock_guard<std::mutex> lock(_imp->drawMutex);
    DrawFilterStats stats = _imp->drawStats;
    stats.posted = _imp->drawPosted;
    stats.absorbed = _imp->drawAbsorbed;
    stats.stageEdits = _imp->stageEditCount;
    stats.stageLoadEdits = _imp->stageLoadEditCount;
    stats.stageNotices = _imp->stageNotices.count;
    stats.resyncedPaths = _imp->stageNotices.resyncedPaths;
    stats.infoOnlyPaths = _imp->stageNotices.infoOnlyPaths;

    return stats;
}

SceneDrawTiming*
SceneViewportRenderer::drawTiming() const
{
    return _imp->drawTiming.get();
}

bool
SceneViewportRenderer::prepareBackgroundFrame(int width,
                                              int height)
{
    _imp->backgroundFramesUsed = true;
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded() || !needsBackgroundFrame()) {
        return false;
    }
    // A camera looked through is a prim of the stage.
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    // A sync or a load refreshes the summary after its locked edit. Lit by a summary that is not
    // yet the stage's, the frame would not be the one the next paint draws, and that paint would
    // render the stage again, compiling the lit shaders.
    const std::shared_ptr<const Implementation::StageSummary> summary = _imp->getStageSummary();
    if (_imp->currentStage() && (!summary || (summary->editCount != _imp->sceneStage->getEditCount() + _imp->sceneStage->getLoadEditCount()))) {
        return false;
    }
    Implementation::BackgroundFrame& frame = _imp->backgroundFrame;
    frame.width = width;
    frame.height = height;
    frame.frustum = _imp->computeFrustum(width, height);
    frame.params = _imp->currentRenderParams();
    frame.lightingMode = _imp->lightingMode;
    frame.displayProcessor = _imp->displayProcessor;
    frame.gain = _imp->displayTransform.gain;
    frame.gamma = _imp->displayTransform.gamma;
    std::copy(_imp->backgroundShown, _imp->backgroundShown + 3, frame.background);
    frame.imageKept = false;
    frame.stops = _imp->backgroundStops;
    frame.cancels = _imp->backgroundCancels;

    return true;
}

void
SceneViewportRenderer::renderBackgroundFrame(unsigned int fbo)
{
    Implementation::BackgroundFrame& frame = _imp->backgroundFrame;
    // Before the viewer lock, the order every reader takes them in.
    SceneWriteBarrier::ReadScope read;

    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
    StormGLStateGuard guard;
    StormGLSection section;

    Implementation::bindFrameTarget(fbo, frame.width, frame.height);
    Implementation::clearFrameTarget(frame.background);
    // Another delegate renders on the refinement's own thread, and one the stage's render
    // settings prim names instead is switched to on the main thread before Storm compiles for it.
    if (_imp->rendererId != kRenderDelegateStormId) {
        return;
    }
    if (_imp->effectiveRenderer() != _imp->rendererId) {
        _imp->rendererSwitchPending = true;

        return;
    }
    // Storm, the only delegate rendering here, cannot be stopped inside a pass
    // (HdStRenderDelegate leaves IsStopSupported at false), so the frame ends between passes.
    Implementation* imp = _imp.get();
    const auto ended = [imp, &frame](int pass) {
        if (imp->backgroundCancels != frame.cancels) {
            return true;
        }

        return (pass > 0) && ((imp->backgroundStops != frame.stops) || (imp->exclusiveWaiting > 0));
    };
    if (ended(0) || !_imp->ensureEngine()) {
        return;
    }
    section.collectWith(_imp->engine->GetHgi());
    _imp->serviceDrawFilter(true);
    frame.params.enableSceneLights = _imp->usesSceneLights(frame.lightingMode);
    Implementation::bindFrameTarget(fbo, frame.width, frame.height);

    const UsdStageRefPtr stage = _imp->currentStage();
    SceneDrawTiming* timing = _imp->drawTiming.get();
    const bool timed = timing && timing->frameStarting(std::chrono::steady_clock::now());
    bool converged = false;
    int passes = 0;
    std::string what;
    bool failed = false;
    try {
        _imp->applyStormSettings();
        _imp->setCamera(frame.frustum, frame.width, frame.height, frame.params.enableSceneLights);
        for (; (passes < kMaxRenderPasses) && !converged && !ended(passes); ++passes) {
            RenderDelegates::runRenderFaultHook();
            _imp->engine->Render(stage->GetPseudoRoot(), frame.params);
            converged = _imp->engine->IsConverged();
            // The first render is what puts the stage's lights into Hydra, and so into the filter.
            _imp->refreshLightsHidden();
            const bool lit = _imp->usesSceneLights(frame.lightingMode);
            if (lit != frame.params.enableSceneLights) {
                frame.params.enableSceneLights = lit;
                _imp->setLights(lit);
                converged = false;
            }
        }
    } catch (const std::exception& e) {
        failed = true;
        what = e.what();
    } catch (...) {
        failed = true;
    }
    if (failed) {
        // The engine's Hgi goes with it, collecting as it does.
        section.collectWith(nullptr);
        _imp->discardFailedEngine(what.empty() ? std::string("unknown exception") : what);

        return;
    }
    _imp->noteDrawFilterRendered(passes != 0);
    if (passes == 0) {
        return;
    }
    if (timed) {
        // Only the render thread waits here, and the image could not be shown before the GPU is
        // done with it anyway; the wait separates GPU time left after submission from the CPU's.
        const std::chrono::steady_clock::time_point renderEnd = std::chrono::steady_clock::now();
        glFinish();
        const double gpuTailMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderEnd).count();
        timing->frameEnded(renderEnd, passes, gpuTailMs, false);
    }
    _imp->drawGrade(fbo, frame.width, frame.height, frame.displayProcessor, frame.gain, frame.gamma);
    _imp->noteStageAbsorbed(frame.params);
    frame.fbo = fbo;
    frame.editCount = _imp->sceneStage->getEditCount();
    frame.loadEditCount = _imp->sceneStage->getLoadEditCount();
    frame.previewGeneration = _imp->sceneStage->getPreviewGeneration();
    frame.imageKept = converged;
}

void
SceneViewportRenderer::requestStageLoad(const std::vector<std::string>& paths,
                                        bool withDescendants,
                                        const std::vector<std::string>& excluded)
{
    SceneStage* stage = _imp->loadStage;

    if (!stage) {
        std::lock_guard<std::mutex> lock(_imp->earlyLoadsMutex);
        stage = _imp->loadStage;
        if (!stage) {
            for (const std::string& path : paths) {
                _imp->earlyLoads.push_back(SceneDrawLoadRequest { path, withDescendants, excluded });
            }

            return;
        }
    }
    stage->requestLoad(toSdfPaths(paths), withDescendants, toSdfPaths(excluded));
}

void
SceneViewportRenderer::requestStageUnload(const std::vector<std::string>& paths)
{
    SceneStage* stage = _imp->loadStage;

    if (stage) {
        stage->requestUnload(toSdfPaths(paths));
    }
}

void
SceneViewportRenderer::cancelStageLoads()
{
    SceneStage* stage = _imp->loadStage;

    if (!stage) {
        std::lock_guard<std::mutex> lock(_imp->earlyLoadsMutex);
        stage = _imp->loadStage;
        if (!stage) {
            _imp->earlyLoads.clear();

            return;
        }
    }
    stage->cancelPendingLoads();
}

void
SceneViewportRenderer::cancelStageLoads(const std::vector<std::string>& paths)
{
    SceneStage* stage = _imp->loadStage;

    if (!stage) {
        std::lock_guard<std::mutex> lock(_imp->earlyLoadsMutex);
        stage = _imp->loadStage;
        if (!stage) {
            const std::set<std::string> cancelled(paths.begin(), paths.end());
            std::vector<SceneDrawLoadRequest>& early = _imp->earlyLoads;
            early.erase(std::remove_if(early.begin(), early.end(), [&cancelled](const SceneDrawLoadRequest& load) { return cancelled.count(load.path) != 0; }), early.end());

            return;
        }
    }
    stage->cancelPendingLoads(toSdfPaths(paths));
}

bool
SceneViewportRenderer::hasPendingStageLoads() const
{
    const SceneStage* stage = _imp->loadStage;

    return stage && stage->hasPendingLoads();
}

bool
SceneViewportRenderer::hasOutstandingStageLoads() const
{
    const SceneStage* stage = _imp->loadStage;

    if (!stage) {
        std::lock_guard<std::mutex> lock(_imp->earlyLoadsMutex);
        stage = _imp->loadStage;
        if (!stage) {
            return !_imp->earlyLoads.empty();
        }
    }

    return stage->hasOutstandingLoads();
}

void
SceneViewportRenderer::stopBackgroundFrame()
{
    ++_imp->backgroundStops;
}

void
SceneViewportRenderer::cancelBackgroundFrame()
{
    ++_imp->backgroundCancels;
}

bool
SceneViewportRenderer::serviceStageLoads(const std::function<bool()>& cancelled)
{
    SceneStage* stage = _imp->loadStage;

    if (!stage) {
        return false;
    }
    Implementation* imp = _imp.get();
    LoadSetEvaluator evaluate;
    LoadSetCommitted committed;
    {
        std::lock_guard<std::mutex> lock(imp->loadSetMutex);
        evaluate = imp->loadSetEvaluator;
        committed = imp->loadSetCommitted;
    }
    const std::shared_ptr<bool> restart = std::make_shared<bool>(false);
    SceneStage::LoadPreparer prepare;
    if (evaluate) {
        prepare = [imp, evaluate, restart](const SceneLoadSetPtr& loadSet) -> std::function<void()> {
            SceneLoadSetPtr shownLoadSet;
            SceneLoadReachPtr shownReach;
            {
                std::lock_guard<std::mutex> lock(imp->loadSetMutex);
                shownLoadSet = imp->shownLoadSet;
                shownReach = imp->shownReach;
            }
            if (shownReach && !SceneOps::loadChangeReaches(shownLoadSet.get(), loadSet.get(), shownReach->getPrefixes())) {
                return std::function<void()>();
            }
            ScenePayload payload;
            SceneLoadReachPtr reach;
            if (!evaluate(loadSet, &payload, &reach) || !SceneOps::getLiveTextures(payload).empty()) {
                return std::function<void()>();
            }

            return [imp, payload, reach, loadSet, restart]() {
                {
                    std::lock_guard<std::mutex> lock(imp->loadSetMutex);
                    imp->shownLoadSet = loadSet;
                    imp->shownReach = payload.isEmpty() ? std::make_shared<SceneLoadReach>() : reach;
                }
                SceneMetadata before = imp->sceneStage->getPayload().getMetadata();
                SceneMetadata after = payload.getMetadata();
                before.renderSettingsPrimPath.clear();
                after.renderSettingsPrimPath.clear();
                StageChangePaths changes(imp->sceneStage->getStage());
                if (imp->sceneStage->sync(payload)) {
                    *restart = changes.outsideRender || !changes.any || (before != after);
                }
                const SceneMetadata& metadata = payload.getMetadata();
                imp->zUp = (metadata.upAxis == SceneMetadata::eUpAxisZ);
                imp->metersPerUnit = metadata.metersPerUnit;
                imp->timeCodesPerSecond = metadata.timeCodesPerSecond;
                imp->shown = !payload.isEmpty();
                imp->stageEditCount = imp->sceneStage->getEditCount();
            };
        };
    }
    const bool serviced = stage->serviceLoads([imp](const std::function<void()>& section) {
        SceneWriteBarrier::ReadScope read;
        std::shared_lock<std::shared_mutex> lock(imp->stageMutex);
        section(); }, [imp, stage, committed](const std::function<void()>& section) {
        SceneWriteBarrier::ReadScope read;
        const std::unique_lock<std::shared_mutex> lock = imp->lockExclusive();
        section();
        imp->stageLoadEditCount = stage->getLoadEditCount();
        if (stage->isValid()) {
            imp->publishLoadRules();
        }
        const SceneLoadSetPtr committedLoadSet = stage->getLoadSet();
        if (!committedLoadSet) {
            return;
        }
        imp->committedLoadSetHash = committedLoadSet->getHash();
        bool reevaluate = false;
        {
            std::lock_guard<std::mutex> loadSetLock(imp->loadSetMutex);
            if (!imp->committedLoadSet || (*imp->committedLoadSet != *committedLoadSet)) {
                // Also catches a scene the batch could not evaluate, and one synced while it
                // was being prepared.
                reevaluate = !imp->shownReach || SceneOps::loadChangeReaches(imp->shownLoadSet.get(), committedLoadSet.get(), imp->shownReach->getPrefixes());
                if (!reevaluate) {
                    imp->shownLoadSet = committedLoadSet;
                }
            }
            imp->committedLoadSet = committedLoadSet;
        }
        // Under the lock, so that a scene synced after the commit was evaluated with this set or
        // is found stale against it.
        if (committed) {
            committed(committedLoadSet, reevaluate);
        } }, cancelled, prepare);
    if (*restart) {
        imp->progressive->invalidate();
    }
    if (cancelled && cancelled()) {
        return serviced;
    }
    SceneWriteBarrier::ReadScope read;
    std::shared_lock<std::shared_mutex> lock(imp->stageMutex);
    imp->refreshStageSummary();

    return serviced;
}

void
SceneViewportRenderer::setLoadSetEvaluator(const LoadSetEvaluator& evaluate,
                                           const LoadSetCommitted& committed)
{
    std::lock_guard<std::mutex> lock(_imp->loadSetMutex);

    _imp->loadSetEvaluator = evaluate;
    _imp->loadSetCommitted = committed;
}

U64
SceneViewportRenderer::getCommittedLoadSetHash() const
{
    return _imp->committedLoadSetHash;
}

void
SceneViewportRenderer::setSyncEvaluatedFor(const SceneLoadSetPtr& loadSet,
                                           const SceneLoadReachPtr& reach)
{
    std::lock_guard<std::mutex> lock(_imp->loadSetMutex);

    _imp->syncLoadSet = loadSet;
    _imp->syncReach = reach;
}

U64
SceneViewportRenderer::getStageLoadEditCount() const
{
    return _imp->stageLoadEditCount;
}

double
SceneViewportRenderer::getLastStageLoadHoldMs() const
{
    const SceneStage* stage = _imp->loadStage;

    return stage ? stage->getLastLoadHoldMs() : 0.;
}

std::vector<std::pair<std::string, std::string>>
SceneViewportRenderer::getStageLoadFailures() const
{
    std::vector<std::pair<std::string, std::string>> result;
    const SceneStage* stage = _imp->loadStage;

    if (stage) {
        for (const SceneStage::LoadFailure& failure : stage->getLoadFailures()) {
            result.emplace_back(failure.prim.GetString(), failure.error);
        }
    }

    return result;
}

bool
SceneViewportRenderer::getStageLoadRules(SceneLoadRules* rules) const
{
    std::lock_guard<std::mutex> lock(_imp->loadRulesMutex);
    *rules = _imp->loadRules;

    return _imp->loadRulesMade;
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
SceneViewportRenderer::setDisplayTransform(const DisplayTransform& transform)
{
    const DisplayTransform& current = _imp->displayTransform;
    const bool sameDisplay = (transform.config == current.config) && (transform.colorSpace == current.colorSpace)
        && (transform.display == current.display) && (transform.view == current.view) && (transform.look == current.look);
    _imp->displayTransform = transform;
    // Gain and gamma are uniforms of the pass: dragging them makes nothing again.
    if (sameDisplay) {
        return;
    }
    _imp->displayProcessor.reset();
    float background[3] = { kBackgroundSrgb, kBackgroundSrgb, kBackgroundSrgb };
    if (transform.config && !transform.colorSpace.empty() && !transform.display.empty() && !transform.view.empty()) {
        try {
            OCIO_NAMESPACE::ConstProcessorRcPtr processor = displayProcessor(transform.config, transform.colorSpace, transform.display, transform.view, transform.look);
            float rgb[3] = { kBackgroundLinear, kBackgroundLinear, kBackgroundLinear };
            processor->getDefaultCPUProcessor()->applyRGB(rgb);
            for (int c = 0; c < 3; ++c) {
                background[c] = std::min(std::max(rgb[c], 0.f), 1.f);
            }
            _imp->displayProcessor = processor;
        } catch (const std::exception& e) {
            qWarning() << "SceneViewport: OpenColorIO:" << e.what();
            background[0] = background[1] = background[2] = kBackgroundSrgb;
        }
    }
    for (int c = 0; c < 3; ++c) {
        _imp->backgroundShown[c] = background[c];
    }
}

bool
SceneViewportRenderer::isUsingOpenColorIO() const
{
    return (bool)_imp->displayProcessor;
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
SceneViewportRenderer::setShowGuides(bool show)
{
    _imp->showGuides = show;
}

bool
SceneViewportRenderer::getShowGuides() const
{
    return _imp->showGuides;
}

void
SceneViewportRenderer::setGuideSize(double size)
{
    _imp->guideSize = size;
}

double
SceneViewportRenderer::getGuideSize() const
{
    return _imp->guideSize.load();
}

void
SceneViewportRenderer::setShowProxy(bool show)
{
    _imp->showProxy = show;
}

bool
SceneViewportRenderer::getShowProxy() const
{
    return _imp->showProxy;
}

void
SceneViewportRenderer::setShowRender(bool show)
{
    _imp->showRender = show;
}

bool
SceneViewportRenderer::getShowRender() const
{
    return _imp->showRender;
}

void
SceneViewportRenderer::setShowGuidePurpose(bool show)
{
    _imp->showGuidePurpose = show;
}

bool
SceneViewportRenderer::getShowGuidePurpose() const
{
    return _imp->showGuidePurpose;
}

void
SceneViewportRenderer::setLightingMode(LightingModeEnum mode)
{
    _imp->lightingMode = mode;
}

SceneViewportRenderer::LightingModeEnum
SceneViewportRenderer::getLightingMode() const
{
    return _imp->lightingMode;
}

bool
SceneViewportRenderer::isSceneLit() const
{
    return _imp->sceneLit;
}

void
SceneViewportRenderer::setRecordDrawnGuides(bool record)
{
    _imp->recordDrawnGuides = record;
}

const std::vector<SceneViewportRenderer::DrawnGuide>&
SceneViewportRenderer::getDrawnGuides() const
{
    return _imp->drawnGuides;
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

void
setBusy(bool* busy)
{
    if (busy) {
        *busy = true;
    }
}
} // anon namespace

bool
SceneViewportRenderer::getViewPose(int width,
                                   int height,
                                   CameraPose* pose,
                                   bool* busy) const
{
    const double aspect = double(std::max(width, 1)) / double(std::max(height, 1));

    pose->aspect = aspect;
    storeMatrix(GfMatrix4d(1.), pose->parentToWorld);
    if (!_imp->lookThrough.empty()) {
        std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            setBusy(busy);

            return false;
        }
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
        {
            std::lock_guard<std::mutex> summaryLock(_imp->summaryMutex);
            if (!_imp->pivotBounds.IsEmpty()) {
                depth = GfDot(_imp->pivotBounds.GetMidpoint() - eye, forward);
                if (depth <= 1e-6) {
                    depth = _imp->pivotBounds.GetSize().GetLength() * 0.5;
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

SceneViewportRenderer::FrameBounds
SceneViewportRenderer::computeFrameBounds(const std::vector<std::string>& selection,
                                          double timeCode) const
{
    FrameBounds bounds;

    bounds.selection = !selection.empty();
    // Before the viewer lock, the order every reader takes them in.
    SceneWriteBarrier::ReadScope read;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
    const UsdStageRefPtr stage = _imp->currentStage();
    if (!stage) {
        return bounds;
    }
    const UsdTimeCode time(timeCode);
    // With useExtentsHint, an unloaded model counts by the extent hint authored over its payload
    // (usdGeom/bboxCache.h), so framing never reads a payload.
    UsdGeomBBoxCache bboxes(time, { UsdGeomTokens->default_, UsdGeomTokens->proxy }, true);
    GfRange3d range;
    if (selection.empty()) {
        range = bboxes.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
        std::lock_guard<std::mutex> summaryLock(_imp->summaryMutex);
        _imp->pivotBounds = range;
    } else {
        UsdGeomXformCache xforms(time);
        for (const std::string& path : selection) {
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
    if (!range.IsEmpty()) {
        for (int i = 0; i < 3; ++i) {
            bounds.min[i] = range.GetMin()[i];
            bounds.max[i] = range.GetMax()[i];
        }
        bounds.empty = false;
    }

    return bounds;
} // SceneViewportRenderer::computeFrameBounds

void
SceneViewportRenderer::applyFrameBounds(const FrameBounds& bounds)
{
    if (!bounds.selection) {
        _imp->lookThrough.clear();
    }
    if (bounds.empty) {
        return;
    }
    _imp->lookThrough.clear();
    const GfRange3d range(GfVec3d(bounds.min[0], bounds.min[1], bounds.min[2]), GfVec3d(bounds.max[0], bounds.max[1], bounds.max[2]));
    const GfVec3d centre = range.GetMidpoint();
    double radius = range.GetSize().GetLength() * 0.5;
    if (radius < 1e-6) {
        radius = bounds.selection ? kDefaultFrameRadius : 1e-6;
    }
    _imp->freeCamera.pivot[0] = centre[0];
    _imp->freeCamera.pivot[1] = centre[1];
    _imp->freeCamera.pivot[2] = centre[2];
    _imp->freeCamera.distance = radius / std::sin(degreesToRadians(_imp->freeCamera.fovY) * 0.5) * 1.05;
    _imp->framedOnce = true;
}

void
SceneViewportRenderer::frameScene()
{
    applyFrameBounds(computeFrameBounds(std::vector<std::string>(), _imp->timeCode()));
}

void
SceneViewportRenderer::frameSelection()
{
    applyFrameBounds(computeFrameBounds(_imp->selected, _imp->timeCode()));
}

void
SceneViewportRenderer::tumble(double azimuthDegrees,
                              double elevationDegrees)
{
    _imp->noteCameraMoved();
    _imp->lookThrough.clear();
    // Tumbling an orthographic view leaves its axis.
    _imp->freeCamera.viewMode = eViewModePerspective;
    _imp->freeCamera.azimuth = std::fmod(_imp->freeCamera.azimuth + azimuthDegrees, 360.);
    _imp->freeCamera.elevation = std::max(kMinElevation, std::min(kMaxElevation, _imp->freeCamera.elevation + elevationDegrees));
}

void
SceneViewportRenderer::pan(double dxPixels,
                           double dyPixels,
                           int viewHeight)
{
    _imp->noteCameraMoved();
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
    _imp->noteCameraMoved();
    _imp->lookThrough.clear();
    if (factor > 0.) {
        _imp->freeCamera.distance = std::max(_imp->freeCamera.distance * factor, 1e-9);
    }
}

std::vector<std::string>
SceneViewportRenderer::getStageCameraPaths() const
{
    const std::shared_ptr<const Implementation::StageSummary> summary = _imp->getStageSummary();

    return summary ? summary->cameraPaths : std::vector<std::string>();
}

void
SceneViewportRenderer::setLookThroughCamera(const std::string& path)
{
    _imp->lookThrough = path;
    _imp->guideBatchesValid = false;
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
    _imp->guideBatchesValid = false;
    _imp->boxColoursValid = false;
    std::lock_guard<std::mutex> lock(_imp->engineSelectionMutex);
    _imp->engineSelection = paths;
    _imp->engineSelectionDirty = true;
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
    if (!_imp->dragSelection) {
        _imp->highlightedHandle = SceneGizmo::kNoHandle;
        _imp->dragHandle = SceneGizmo::kNoHandle;
    }
}

void
SceneViewportRenderer::setTransformTool(int tool)
{
    _imp->transformTool = (SceneTransform::ToolEnum)std::max(0, std::min(tool, (int)SceneTransform::eToolScale));
}

int
SceneViewportRenderer::getTransformTool() const
{
    return _imp->transformTool;
}

void
SceneViewportRenderer::setTransformSpace(int space)
{
    _imp->transformSpace = (space == SceneTransform::eSpaceLocal) ? SceneTransform::eSpaceLocal : SceneTransform::eSpaceWorld;
}

int
SceneViewportRenderer::getTransformSpace() const
{
    return _imp->transformSpace;
}

void
SceneViewportRenderer::setTransformPivot(int pivot)
{
    _imp->transformPivot = (pivot == SceneTransform::ePivotCurrentPrim) ? SceneTransform::ePivotCurrentPrim : SceneTransform::ePivotSelectionCentre;
}

int
SceneViewportRenderer::getTransformPivot() const
{
    return _imp->transformPivot;
}

bool
SceneViewportRenderer::hasTransformHandles() const
{
    SceneGizmo::Frame frame;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);

    return _imp->computeHandleFrame(&frame);
}

bool
SceneViewportRenderer::hasSelectionHandles(bool* busy) const
{
    SceneGizmo::Frame frame;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        setBusy(busy);

        return false;
    }

    return _imp->computeSelectionFrame(&frame);
}

bool
SceneViewportRenderer::getDrawnTransformHandles(int width,
                                                int height,
                                                std::vector<int>* handles,
                                                std::vector<double>* pixels,
                                                std::vector<double>* frameValues,
                                                bool* busy) const
{
    SceneGizmo::Frame frame;
    bool selection = false;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        setBusy(busy);

        return false;
    }
    handles->clear();
    pixels->clear();
    frameValues->clear();
    if ((width <= 0) || (height <= 0) || !_imp->computeHandleFrame(&frame, &selection)) {
        return false;
    }
    const SceneGizmo::View view = _imp->makeView(width, height);
    for (int handle = 0; handle < SceneGizmo::kNumHandles; ++handle) {
        GfVec2d pixel;
        if (_imp->isDrawnHandle(handle, selection) && SceneGizmo::getHandlePixel(view, frame, handle, &pixel)) {
            handles->push_back(handle);
            pixels->push_back(pixel[0]);
            pixels->push_back(pixel[1]);
        }
    }
    const GfVec3d* vectors[4] = { &frame.origin, &frame.axes[0], &frame.axes[1], &frame.axes[2] };
    for (const GfVec3d* v : vectors) {
        for (int i = 0; i < 3; ++i) {
            frameValues->push_back((*v)[i]);
        }
    }

    return true;
} // SceneViewportRenderer::getDrawnTransformHandles

int
SceneViewportRenderer::hitTestTransformHandles(double x,
                                               double y,
                                               int width,
                                               int height,
                                               bool* busy) const
{
    SceneGizmo::Frame frame;
    bool selection = false;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        setBusy(busy);

        return SceneGizmo::kNoHandle;
    }
    if ((width <= 0) || (height <= 0) || !_imp->computeHandleFrame(&frame, &selection)) {
        return SceneGizmo::kNoHandle;
    }
    const SceneGizmo::View view = _imp->makeView(width, height);

    return SceneGizmo::hitTest(view, _imp->buildDrawnHandles(frame, selection, SceneGizmo::getHandleLength(view, frame)), GfVec2d(x, y));
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
    bool selection = false;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);

    if ((width <= 0) || (height <= 0) || !_imp->computeHandleFrame(&frame, &selection) || !_imp->isDrawnHandle(handle, selection)) {
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
                                          int /*height*/,
                                          bool* busy)
{
    SceneGizmo::Frame frame;
    bool selection = false;
    std::unique_lock<std::shared_mutex> lock;

    if (!_imp->tryLockExclusive(&lock)) {
        setBusy(busy);

        return false;
    }
    // A preview still waiting for its scene goes now: the new drag measures from the scene shown.
    _imp->clearPreviewLocked();
    _imp->dragHandle = SceneGizmo::kNoHandle;
    _imp->dragSelection = false;
    if (!_imp->computeHandleFrame(&frame, &selection) || !_imp->isDrawnHandle(handle, selection)) {
        return false;
    }
    _imp->dragPrims.clear();
    _imp->dragOpTargets.clear();
    if (selection) {
        const double time = _imp->timeCode();
        UsdGeomXformCache xforms((UsdTimeCode(time)));
        for (const UsdPrim& prim : _imp->transformablePrims()) {
            SceneTransformPrim described;
            SceneTransformWidget::describePrim(prim, &xforms, time, &described);
            _imp->dragPrims.push_back(described);
        }
    } else {
        if (!_imp->computeTargetSpace(_imp->handles.primPath, _imp->handles.opName, &_imp->dragOpSpace)) {
            return false;
        }
        _imp->dragOpTargets.push_back(std::make_pair(_imp->handles.primPath, _imp->dragOpSpace));
        for (const TransformHandles::Target& target : _imp->handles.otherTargets) {
            GfMatrix4d space;
            if ((target.primPath != _imp->handles.primPath) && _imp->computeTargetSpace(target.primPath, target.opName, &space)) {
                _imp->dragOpTargets.push_back(std::make_pair(target.primPath, space));
            }
        }
    }
    _imp->dragHandle = handle;
    _imp->dragSelection = selection;
    _imp->dragFrame = frame;
    _imp->dragPressPixel = GfVec2d(x, y);
    _imp->dragChange = 0.;
    _imp->dragChanged = false;

    return true;
} // SceneViewportRenderer::beginTransformDrag

bool
SceneViewportRenderer::isSelectionTransformDrag() const
{
    return (_imp->dragHandle != SceneGizmo::kNoHandle) && _imp->dragSelection;
}

bool
SceneViewportRenderer::getTransformDragChange(double x,
                                              double y,
                                              int width,
                                              int height,
                                              double* change,
                                              bool* busy) const
{
    if ((_imp->dragHandle == SceneGizmo::kNoHandle) || (width <= 0) || (height <= 0)) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::defer_lock);

    // The free camera's view reads no prim, so its drags follow the mouse through a load.
    if (!_imp->lookThrough.empty() && !lock.try_lock()) {
        setBusy(busy);

        return false;
    }

    return SceneGizmo::computeDragChange(_imp->makeView(width, height), _imp->dragFrame, _imp->dragHandle, _imp->dragPressPixel, GfVec2d(x, y), change);
}

bool
SceneViewportRenderer::previewTransformDrag(double change,
                                            bool* busy)
{
    if (_imp->dragHandle == SceneGizmo::kNoHandle) {
        return false;
    }
    std::vector<std::pair<std::string, GfMatrix4d>> deltas;
    if (_imp->dragSelection) {
        const GfMatrix4d delta = SceneTransformWidget::computeDelta(_imp->dragFrame, _imp->dragHandle, change);
        for (const SceneTransformPrim& prim : _imp->dragPrims) {
            deltas.push_back(std::make_pair(prim.path, delta));
        }
    } else {
        // W = inner * op * space, so W' = W * space^-1 op^-1 op' space.
        GfVec3d translate, rotate, scale;
        _imp->changeOpValues(SceneGizmo::kNoHandle, 0., &translate, &rotate, &scale);
        const GfMatrix4d opInverse = _imp->composeOp(translate, rotate, scale).GetInverse();
        _imp->changeOpValues(_imp->dragHandle, change, &translate, &rotate, &scale);
        const GfMatrix4d changed = _imp->composeOp(translate, rotate, scale);
        for (const std::pair<std::string, GfMatrix4d>& target : _imp->dragOpTargets) {
            deltas.push_back(std::make_pair(target.first, target.second.GetInverse() * opInverse * changed * target.second));
        }
    }
    std::unique_lock<std::shared_mutex> lock;

    if (!_imp->tryLockExclusive(&lock)) {
        setBusy(busy);

        return false;
    }
    _imp->dragChange = change;
    _imp->dragChanged = true;
    if (!_imp->sceneStage || deltas.empty()) {
        return false;
    }
    _imp->guidesValid = false;
    if (!_imp->sceneStage->setPreviewTransforms(deltas, _imp->timeCode())) {
        return false;
    }
    _imp->progressive->invalidate();

    return true;
} // SceneViewportRenderer::previewTransformDrag

bool
SceneViewportRenderer::getTransformDrag(double change,
                                        double frame,
                                        SceneTransformDrag* drag) const
{
    if ((_imp->dragHandle == SceneGizmo::kNoHandle) || !_imp->dragSelection || _imp->dragPrims.empty()) {
        return false;
    }
    drag->prims = _imp->dragPrims;
    SceneTransformWidget::storeMatrix(SceneTransformWidget::computeDelta(_imp->dragFrame, _imp->dragHandle, change), drag->delta);
    drag->tool = _imp->transformTool;
    drag->frame = frame;
    for (int i = 0; i < 3; ++i) {
        drag->pivot[i] = _imp->dragFrame.origin[i];
    }

    return true;
}

void
SceneViewportRenderer::endTransformDrag()
{
    _imp->dragHandle = SceneGizmo::kNoHandle;
    _imp->dragSelection = false;
    _imp->dragPrims.clear();
    _imp->dragOpTargets.clear();
    _imp->dragChange = 0.;
    _imp->dragChanged = false;
}

bool
SceneViewportRenderer::clearTransformPreview()
{
    std::unique_lock<std::shared_mutex> lock;

    if (!_imp->tryLockExclusive(&lock)) {
        return false;
    }
    _imp->clearPreviewLocked();

    return true;
}

void
SceneViewportRenderer::clearTransformPreviewAfter(U64 serial)
{
    _imp->previewClearAfter = serial;
}

bool
SceneViewportRenderer::render(unsigned int fbo,
                              int width,
                              int height)
{
    const std::unique_lock<std::shared_mutex> lock = _imp->lockExclusive();

    return renderLocked(fbo, width, height, true) == eRenderStatusDone;
}

SceneViewportRenderer::RenderStatusEnum
SceneViewportRenderer::renderIfIdle(unsigned int fbo,
                                    int width,
                                    int height)
{
    std::unique_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        return eRenderStatusBusy;
    }

    return renderLocked(fbo, width, height, false);
}

SceneViewportRenderer::RenderStatusEnum
SceneViewportRenderer::renderRefinementIfIdle(unsigned int fbo,
                                              int width,
                                              int height)
{
    std::unique_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock() || (_imp->rendererId == kRenderDelegateStormId)) {
        return eRenderStatusBusy;
    }

    return renderLocked(fbo, width, height, false);
}

SceneViewportRenderer::RenderStatusEnum
SceneViewportRenderer::renderLocked(unsigned int fbo,
                                    int width,
                                    int height,
                                    bool followScene)
{
    if ((width <= 0) || (height <= 0) || !appPTR->isOpenGLLoaded()) {
        return eRenderStatusDone;
    }
    // Scene Default follows the renderer the stage's render settings prim names. A paint keeps
    // the renderer it has and leaves the switch to followSceneRenderer(), outside Qt's paint.
    const std::string effective = _imp->effectiveRenderer();
    if (followScene) {
        StormGLStateGuard guard;
        _imp->switchRenderer(effective);
    }
    _imp->rendererSwitchPending = (effective != _imp->rendererId);
    if (_imp->rendererId != kRenderDelegateStormId) {
        return renderProgressive(fbo, width, height);
    }
    StormGLStateGuard guard;
    StormGLSection section;

    if (!_imp->ensureEngine()) {
        Implementation::bindFrameTarget(fbo, width, height);
        Implementation::clearFrameTarget(_imp->backgroundShown);

        return eRenderStatusDone;
    }
    section.collectWith(_imp->engine->GetHgi());
    _imp->serviceDrawFilter(!_imp->backgroundFramesUsed);
    {
        std::string what;
        bool failed = false;
        try {
            _imp->applyStormSettings();
        } catch (const std::exception& e) {
            failed = true;
            what = e.what();
        } catch (...) {
            failed = true;
        }
        if (failed) {
            // The engine's Hgi goes with it, collecting as it does.
            section.collectWith(nullptr);
            _imp->discardFailedEngine(what.empty() ? std::string("unknown exception") : what);

            return eRenderStatusDone;
        }
    }

    const UsdStageRefPtr stage = _imp->currentStage();
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    _imp->sceneLit = _imp->usesSceneLights();
    UsdImagingGLRenderParams params = _imp->renderParams();
    const bool held = _imp->stageHeld;
    const bool kept = !held && _imp->takeBackgroundImage(fbo, width, height, frustum, params);
    Implementation::bindFrameTarget(fbo, width, height);
    bool converged = kept || held;
    if (held) {
        // The engine is not handed the stage either: it would take its prims in at full.
        Implementation::clearFrameTarget(_imp->backgroundShown);
    } else if (!kept) {
        Implementation::clearFrameTarget(_imp->backgroundShown);
        _imp->setCamera(frustum, width, height, _imp->sceneLit);
        SceneDrawTiming* timing = _imp->drawTiming.get();
        const bool timed = timing && timing->frameStarting(std::chrono::steady_clock::now());
        int passes = 0;
        for (int i = 0; (i < kMaxRenderPasses) && !converged; ++i) {
            std::string what;
            bool failed = false;
            try {
                RenderDelegates::runRenderFaultHook();
                _imp->engine->Render(stage->GetPseudoRoot(), params);
            } catch (const std::exception& e) {
                failed = true;
                what = e.what();
            } catch (...) {
                failed = true;
            }
            if (failed) {
                // The engine's Hgi goes with it, collecting as it does.
                section.collectWith(nullptr);
                _imp->discardFailedEngine(what.empty() ? std::string("unknown exception") : what);

                return eRenderStatusDone;
            }
            ++passes;
            converged = _imp->engine->IsConverged();
            _imp->refreshLightsHidden();
            const bool lit = _imp->usesSceneLights();
            if (lit != _imp->sceneLit) {
                _imp->sceneLit = lit;
                params.enableSceneLights = lit;
                _imp->setLights(lit);
                converged = false;
            }
        }
        if (timed) {
            // The main thread waits for this render, so the GPU is left to finish on its own.
            timing->frameEnded(std::chrono::steady_clock::now(), passes, -1., true);
        }
        // A kept image was graded by the frame that left it.
        _imp->drawGrade(fbo, width, height, _imp->displayProcessor, _imp->displayTransform.gain, _imp->displayTransform.gamma);
    }
    if (!held) {
        _imp->noteDrawFilterRendered(!kept);
        _imp->noteStageAbsorbed(params);
    }

    _imp->drawnGuides.clear();
    _imp->feedBoxes();
    _imp->boxesDrawn = 0;
    _imp->markersDrawn = 0;
    _imp->drawnBoxList.reset();
    _imp->drawnBoxes.clear();
    if (_imp->showGrid || _imp->showGuides || _imp->hasHandles || _imp->hasBoxes()) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glUseProgram(0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_FRAMEBUFFER_SRGB);
        if (_imp->showGrid) {
            _imp->drawGrid(frustum);
        }
        if (!held) {
            _imp->drawBoxes(frustum, width, height);
        }
        if (_imp->showGuides) {
            _imp->drawGuides(frustum, width, height);
        }
        _imp->drawTransformHandles(frustum, width, height);
    }

    return converged ? eRenderStatusDone : eRenderStatusRefining;
} // renderLocked

SceneViewportRenderer::RenderStatusEnum
SceneViewportRenderer::renderProgressive(unsigned int fbo,
                                         int width,
                                         int height)
{
    StormGLStateGuard guard;
    HydraRender::ProgressiveRender& progressive = *_imp->progressive;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(_imp->backgroundShown[0], _imp->backgroundShown[1], _imp->backgroundShown[2], 1.f);
    glClearDepth(1.);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (_imp->refineReleased.exchange(false)) {
        _imp->refineRenderer.clear();
        _imp->uploadedFrame.reset();
    }
    if (_imp->getRendererErrorCopy().empty()) {
        _imp->setRendererErrorCopy(RenderDelegates::unavailableMessage(_imp->rendererId));
    }
    const UsdStageRefPtr stage = _imp->currentStage();
    if (!_imp->getRendererErrorCopy().empty() || !stage) {
        progressive.setRenderer(std::string());
        _imp->refineRenderer.clear();
        _imp->setRefineErrorCopy(std::string());
        _imp->uploadedFrame.reset();

        return eRenderStatusDone;
    }
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    const bool held = _imp->stageHeld;
    HydraRender::ProgressiveRender::Status status;
    if (!held) {
        // The prim's settings for the renderer first, so that its engine is made under them; the
        // view's own sample cap applies over its samples.
        progressive.setSettings(_imp->sceneSettingsFor(_imp->rendererId));
        if (_imp->refineRenderer != _imp->rendererId) {
            progressive.setRenderer(_imp->rendererId);
            _imp->refineRenderer = _imp->rendererId;
        }
        HydraRender::ProgressiveRender::View view;
        view.frustum = frustum;
        view.width = width;
        view.height = height;
        view.time = UsdTimeCode(_imp->timeCode());
        progressive.setMaxSamples(_imp->maxSamples);
        progressive.setView(view);

        status = progressive.getStatus();
        _imp->setRefineErrorCopy(status.error);
        if (!status.error.empty()) {
            return eRenderStatusDone;
        }
    }
    const std::shared_ptr<const HydraRender::ProgressiveRender::Frame> frame = held ? std::shared_ptr<const HydraRender::ProgressiveRender::Frame>() : progressive.getFrame();
    if (frame && (frame->width > 0) && (frame->height > 0)) {
        const GLsizei frameWidth = frame->width;
        const GLsizei frameHeight = frame->height;
        if (!_imp->refineTexture) {
            glGenTextures(1, &_imp->refineTexture);
            glGenBuffers(1, &_imp->refineDepthBuffer);
        }
        if (frame != _imp->uploadedFrame) {
            const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, _imp->refineTexture);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            if ((frameWidth != _imp->refineWidth) || (frameHeight != _imp->refineHeight)) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frameWidth, frameHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame->rgba.data());
                _imp->refineWidth = frameWidth;
                _imp->refineHeight = frameHeight;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frameWidth, frameHeight, GL_RGBA, GL_UNSIGNED_BYTE, frame->rgba.data());
            }
            // The depth stays in a buffer of the GPU's, for the paints that draw the grid over the
            // frame again before the next one.
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _imp->refineDepthBuffer);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)(frame->depth.size() * sizeof(float)), frame->depth.data(), GL_STREAM_DRAW);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            _imp->uploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            _imp->uploadPixels = frameWidth * frameHeight;
            _imp->uploadedFrame = frame;
        }
        // A frame of another size, while the one of this size renders, is stretched to the view.
        GLuint readFramebuffer = 0;
        glGenFramebuffers(1, &readFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _imp->refineTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, frameWidth, frameHeight, 0, 0, width, height, GL_COLOR_BUFFER_BIT, ((frameWidth == width) && (frameHeight == height)) ? GL_NEAREST : GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDeleteFramebuffers(1, &readFramebuffer);
        if ((_imp->showGrid || _imp->showGuides || _imp->hasHandles || _imp->hasBoxes()) && (frameWidth == width) && (frameHeight == height)) {
            // The frame's depth, so that the grid and guides hide behind what it shows.
            glUseProgram(0);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);
            glWindowPos2i(0, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _imp->refineDepthBuffer);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glDrawPixels(frameWidth, frameHeight, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glDepthFunc(GL_LESS);
            glDisable(GL_DEPTH_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        _imp->drawnSamples = frame->samples;
    } else {
        _imp->drawnSamples = -1;
    }

    _imp->drawnGuides.clear();
    _imp->feedBoxes();
    _imp->boxesDrawn = 0;
    _imp->markersDrawn = 0;
    _imp->drawnBoxList.reset();
    _imp->drawnBoxes.clear();
    if (_imp->showGrid || _imp->showGuides || _imp->hasHandles || _imp->hasBoxes()) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glUseProgram(0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        if (_imp->showGrid) {
            _imp->drawGrid(frustum);
        }
        if (!held) {
            _imp->drawBoxes(frustum, width, height);
        }
        if (_imp->showGuides) {
            _imp->drawGuides(frustum, width, height);
        }
        _imp->drawTransformHandles(frustum, width, height);
    }
    if (held) {
        return eRenderStatusDone;
    }

    const bool finished = status.converged && frame && frame->converged && (frame->restart == status.restarts);

    return finished ? eRenderStatusDone : eRenderStatusWaiting;
} // renderProgressive

std::string
SceneViewportRenderer::pick(int x,
                            int y,
                            int width,
                            int height,
                            bool* busy)
{
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded()) {
        return std::string();
    }
    if (_imp->rendererId != kRenderDelegateStormId) {
        // From the AOVs of the last pass, of the size of the view when it rendered; it takes the
        // viewer lock itself, so it is read before ours is taken.
        const std::shared_ptr<const HydraRender::ProgressiveRender::Frame> frame = _imp->progressive->getFrame();
        // What is under the pixel and its window depth; a prim without one is behind everything.
        std::string path;
        double nearest = std::numeric_limits<double>::infinity();
        if (frame && (frame->width > 0) && (frame->height > 0)) {
            const int fx = (int)((x + 0.5) * frame->width / width);
            const int fy = frame->height - 1 - (int)((y + 0.5) * frame->height / height);
            path = _imp->progressive->pick(*frame, fx, fy);
            const std::size_t pixel = (std::size_t)fy * (std::size_t)frame->width + (std::size_t)fx;
            if (!path.empty() && (pixel < frame->depth.size())) {
                nearest = frame->depth[pixel];
            }
        }
        std::unique_lock<std::shared_mutex> lock;
        if (!_imp->tryLockExclusive(&lock)) {
            setBusy(busy);

            return std::string();
        }
        const GfFrustum frustum = _imp->computeFrustum(width, height);
        const GfMatrix4d viewMatrix = frustum.ComputeViewMatrix();
        const GfMatrix4d projection = frustum.ComputeProjectionMatrix();
        // A box wins a tie with a prim, and a guide a tie with either.
        GfVec3d boxPoint;
        const int box = _imp->pickBox(frustum, width, height, x, y, &boxPoint);
        if (box >= 0) {
            const double boxDepth = ((viewMatrix * projection).Transform(boxPoint)[2] + 1.) * 0.5;
            if (path.empty() || (boxDepth <= nearest)) {
                path = _imp->drawnBoxList->boxes[box].path.GetString();
                nearest = boxDepth;
            }
        }
        if (_imp->showGuides) {
            const SceneGizmo::View view(viewMatrix, projection, width, height);
            const double maxDepth = path.empty() ? std::numeric_limits<double>::infinity() : 2. * nearest - 1.;
            GfVec3d guidePoint;
            const int guide = SceneGuides::hitTest(view, _imp->currentGuides(), GfVec2d(x + 0.5, y + 0.5), _imp->lookThrough, &guidePoint, maxDepth);
            if (guide >= 0) {
                path = _imp->guides[guide].path;
            }
        }

        return path;
    }
    std::unique_lock<std::shared_mutex> lock;
    if (!_imp->tryLockExclusive(&lock)) {
        setBusy(busy);

        return std::string();
    }
    StormGLStateGuard guard;
    StormGLSection section;

    if (!_imp->ensureEngine()) {
        return std::string();
    }
    section.collectWith(_imp->engine->GetHgi());
    const UsdStageRefPtr stage = _imp->currentStage();
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    // The pixel's centre in normalized device coordinates, y up, and a one-pixel window.
    const GfVec2d centre(2. * (x + 0.5) / width - 1., 1. - 2. * (y + 0.5) / height);
    const GfFrustum pickFrustum = frustum.ComputeNarrowedFrustum(centre, GfVec2d(1. / width, 1. / height));

    UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = HdxPickTokens->resolveNearestToCenter;
    UsdImagingGLEngine::IntersectionResultVector results;
    const bool hitPrim = _imp->engine->TestIntersection(pickParams, pickFrustum.ComputeViewMatrix(), pickFrustum.ComputeProjectionMatrix(),
                                                        stage->GetPseudoRoot(), _imp->renderParams(), &results)
        && !results.empty();

    // The nearer wins: depth is -z in the camera's space, for either projection. A guide wins a
    // tie, then a box.
    const GfMatrix4d viewMatrix = frustum.ComputeViewMatrix();
    const GfMatrix4d projection = frustum.ComputeProjectionMatrix();
    std::string path;
    double nearest = 0.;
    GfVec3d nearestPoint;
    if (hitPrim) {
        path = results.front().hitPrimPath.GetString();
        nearestPoint = results.front().hitPoint;
        nearest = viewMatrix.Transform(nearestPoint)[2];
    }
    GfVec3d boxPoint;
    const int box = _imp->pickBox(frustum, width, height, x, y, &boxPoint);
    if (box >= 0) {
        const double z = viewMatrix.Transform(boxPoint)[2];
        if (path.empty() || (z >= nearest)) {
            path = _imp->drawnBoxList->boxes[box].path.GetString();
            nearest = z;
            nearestPoint = boxPoint;
        }
    }
    if (_imp->showGuides) {
        const SceneGizmo::View view(viewMatrix, projection, width, height);
        const double maxDepth = path.empty() ? std::numeric_limits<double>::infinity() : view.viewProjection.Transform(nearestPoint)[2];
        GfVec3d guidePoint;
        const int guide = SceneGuides::hitTest(view, _imp->currentGuides(), GfVec2d(x + 0.5, y + 0.5), _imp->lookThrough, &guidePoint, maxDepth);
        if (guide >= 0) {
            path = _imp->guides[guide].path;
        }
    }

    return path;
}

std::vector<std::string>
SceneViewportRenderer::pickRect(int x0,
                                int y0,
                                int x1,
                                int y1,
                                int width,
                                int height,
                                bool* busy)
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
    std::unique_lock<std::shared_mutex> lock;
    if (!_imp->tryLockExclusive(&lock)) {
        setBusy(busy);

        return paths;
    }
    StormGLStateGuard guard;
    StormGLSection section;

    if (!_imp->ensureEngine()) {
        return paths;
    }
    section.collectWith(_imp->engine->GetHgi());
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
        results.clear();
    }
    for (const UsdImagingGLEngine::IntersectionResult& result : results) {
        const std::string path = result.hitPrimPath.GetString();
        if (!path.empty() && (std::find(paths.begin(), paths.end(), path) == paths.end())) {
            paths.push_back(path);
        }
    }
    if (_imp->showGuides) {
        const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
        const std::vector<SceneGuides::Guide>& guides = _imp->currentGuides();
        for (int index : SceneGuides::guidesInRect(view, guides, GfVec2d(left, top), GfVec2d(right + 1., bottom + 1.), _imp->lookThrough)) {
            if (std::find(paths.begin(), paths.end(), guides[index].path) == paths.end()) {
                paths.push_back(guides[index].path);
            }
        }
    }
    // Hits come in hash order; sorted, a marquee's selection does not depend on it.
    std::sort(paths.begin(), paths.end());

    return paths;
}

void
SceneViewportRenderer::releaseGL()
{
    const std::unique_lock<std::shared_mutex> lock = _imp->lockExclusive();

    _imp->dropEngine();
    _imp->releaseGrade();
    _imp->releaseBoxesGL();
    if (_imp->refineTexture) {
        glDeleteTextures(1, &_imp->refineTexture);
        glDeleteBuffers(1, &_imp->refineDepthBuffer);
        _imp->refineTexture = 0;
        _imp->refineDepthBuffer = 0;
    }
    _imp->refineWidth = 0;
    _imp->refineHeight = 0;
    _imp->uploadedFrame.reset();
}

void
SceneViewportRenderer::setRendererId(const std::string& id)
{
    const std::unique_lock<std::shared_mutex> lock = _imp->lockExclusive();

    {
        std::lock_guard<std::mutex> idLock(_imp->rendererIdMutex);
        _imp->rendererChoice = id.empty() ? std::string(kRenderDelegateStormId) : id;
    }
    // The error is cleared even for the same id, so that choosing it again retries it.
    _imp->setRendererErrorCopy(std::string());
    _imp->setRefineErrorCopy(std::string());
    _imp->switchRenderer(_imp->effectiveRenderer());
}

bool
SceneViewportRenderer::followSceneRenderer()
{
    const std::unique_lock<std::shared_mutex> lock = _imp->lockExclusive();
    const std::string before = _imp->rendererId;

    _imp->switchRenderer(_imp->effectiveRenderer());
    _imp->rendererSwitchPending = false;

    return _imp->rendererId != before;
}

bool
SceneViewportRenderer::hasPendingRendererSwitch() const
{
    return _imp->rendererSwitchPending;
}

std::string
SceneViewportRenderer::getRendererId() const
{
    std::lock_guard<std::mutex> lock(_imp->rendererIdMutex);

    return _imp->rendererChoice;
}

std::string
SceneViewportRenderer::getActiveRendererId() const
{
    std::lock_guard<std::mutex> lock(_imp->rendererIdMutex);

    return _imp->rendererId;
}

std::string
SceneViewportRenderer::takeRenderError()
{
    std::lock_guard<std::mutex> lock(_imp->rendererErrorMutex);
    std::string error;

    error.swap(_imp->renderError);

    return error;
}

std::string
SceneViewportRenderer::getRendererError() const
{
    std::lock_guard<std::mutex> lock(_imp->rendererErrorMutex);

    return _imp->rendererError.empty() ? _imp->refineError : _imp->rendererError;
}

std::string
SceneViewportRenderer::getEngineRendererId() const
{
    const std::unique_lock<std::shared_mutex> lock = _imp->lockExclusive();

    return _imp->engine ? _imp->engine->GetCurrentRendererId().GetString() : std::string();
}

void
SceneViewportRenderer::getBackgroundColor(float rgb[3]) const
{
    for (int c = 0; c < 3; ++c) {
        rgb[c] = _imp->backgroundShown[c];
    }
}

void
SceneViewportRenderer::setMaxSamples(int samples)
{
    _imp->maxSamples = std::max(0, samples);
    _imp->progressive->setMaxSamples(_imp->maxSamples);
}

int
SceneViewportRenderer::getMaxSamples() const
{
    return _imp->progressive->getMaxSamples();
}

void
SceneViewportRenderer::setRefinementPaused(bool paused)
{
    if (paused) {
        _imp->progressive->stop();
    } else {
        _imp->progressive->resume();
    }
}

SceneViewportRenderer::RefineStatus
SceneViewportRenderer::getRefineStatus() const
{
    const HydraRender::ProgressiveRender::Status status = _imp->progressive->getStatus();
    RefineStatus result;

    result.active = status.active && (getActiveRendererId() != kRenderDelegateStormId);
    result.stopped = status.stopped;
    result.converged = status.converged;
    result.samples = status.samples;
    result.maxSamples = status.maxSamples;
    result.restarts = status.restarts;
    result.frames = status.frames;
    result.drawnSamples = _imp->drawnSamples;

    return result;
}

void
SceneViewportRenderer::setRefineListener(const std::function<void()>& listener)
{
    std::lock_guard<std::mutex> lock(_imp->refineListenerMutex);

    _imp->refineListener = listener;
}

void
SceneViewportRenderer::releaseRefinement()
{
    _imp->progressive->setRenderer(std::string());
    _imp->setRefineErrorCopy(std::string());
    _imp->refineReleased = true;
}

double
SceneViewportRenderer::getLastUploadMs() const
{
    return _imp->uploadMs;
}

int
SceneViewportRenderer::getLastUploadPixels() const
{
    return _imp->uploadPixels;
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

// The 3D viewer's default key binding of actionID, for a process without the application.
const Viewer3DKeyBinding*
defaultKeyBinding(const char* actionID)
{
    for (const Viewer3DKeyBinding& binding : getViewer3DDefaultKeyBindings()) {
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
    const Qt::KeyboardModifiers relevant = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;
    const Viewer3DKeyBinding* binding = defaultKeyBinding(actionID);
    if (!binding) {
        return false;
    }

    return ((binding->key == key) && ((modifiers & relevant) == binding->modifiers)) || (binding->altKey && (binding->altKey == key) && ((modifiers & relevant) == binding->altModifiers));
}

// The transform widget's tools, their key actions, script names and menu labels.
struct TransformToolEntry {
    SceneTransform::ToolEnum tool;
    const char* actionID;
    const char* name;
    const char* label;
};

const TransformToolEntry kTransformTools[] = {
    { SceneTransform::eToolSelect, kShortcutIDActionViewer3DSelectTool, "select", kShortcutDescActionViewer3DSelectTool },
    { SceneTransform::eToolTranslate, kShortcutIDActionViewer3DTranslateTool, "translate", kShortcutDescActionViewer3DTranslateTool },
    { SceneTransform::eToolRotate, kShortcutIDActionViewer3DRotateTool, "rotate", kShortcutDescActionViewer3DRotateTool },
    { SceneTransform::eToolScale, kShortcutIDActionViewer3DScaleTool, "scale", kShortcutDescActionViewer3DScaleTool },
};

// The tool a key selects, -1 for none.
int
transformToolForKey(Qt::KeyboardModifiers modifiers,
                    int key)
{
    for (const TransformToolEntry& entry : kTransformTools) {
        if (matchesKeyBinding(entry.actionID, modifiers, key)) {
            return entry.tool;
        }
    }

    return -1;
}

// The script name of a tool, as the natronSceneTransformTool property reports it.
const char*
transformToolName(int tool)
{
    for (const TransformToolEntry& entry : kTransformTools) {
        if (entry.tool == tool) {
            return entry.name;
        }
    }

    return "";
}

// Whether a key is one of the transform widget's.
bool
isTransformKey(Qt::KeyboardModifiers modifiers,
               int key)
{
    return (transformToolForKey(modifiers, key) >= 0) || matchesKeyBinding(kShortcutIDActionViewer3DCycleTool, modifiers, key)
        || matchesKeyBinding(kShortcutIDActionViewer3DToggleSpace, modifiers, key) || matchesKeyBinding(kShortcutIDActionViewer3DTogglePivot, modifiers, key);
}

// The pickwalk step a key is bound to, -1 for none.
int
pickwalkForKey(Qt::KeyboardModifiers modifiers,
               int key)
{
    static const struct {
        const char* actionID;
        int direction;
    } kPickwalkKeys[] = {
        { kShortcutIDActionViewer3DPickwalkParent, SceneViewport::ePickwalkParent },
        { kShortcutIDActionViewer3DPickwalkChild, SceneViewport::ePickwalkChild },
        { kShortcutIDActionViewer3DPickwalkPrevSibling, SceneViewport::ePickwalkPrevSibling },
        { kShortcutIDActionViewer3DPickwalkNextSibling, SceneViewport::ePickwalkNextSibling },
    };
    for (const auto& entry : kPickwalkKeys) {
        if (matchesKeyBinding(entry.actionID, modifiers, key)) {
            return entry.direction;
        }
    }

    return -1;
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
    const Viewer3DKeyBinding* binding = defaultKeyBinding(actionID);

    return binding ? QKeySequence(int(binding->modifiers) | int(binding->key)) : QKeySequence();
}

// A shortcut action's description in the context its translations are extracted under.
QString
shortcutText(const char* description)
{
    return QCoreApplication::translate("ActionWithShortcut", description);
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

// The draw modes, their key actions and menu labels.
struct DrawModeEntry {
    SceneViewportRenderer::DrawModeEnum mode;
    const char* actionID;
    const char* label;
    const char* name;
};

const DrawModeEntry kDrawModes[] = {
    { SceneViewportRenderer::eDrawModeUnshaded, kShortcutIDActionViewer3DUnshaded, kShortcutDescActionViewer3DUnshaded, "unshaded" },
    { SceneViewportRenderer::eDrawModeShaded, kShortcutIDActionViewer3DShaded, kShortcutDescActionViewer3DShaded, "shaded" },
    { SceneViewportRenderer::eDrawModeWireframe, kShortcutIDActionViewer3DWireframe, kShortcutDescActionViewer3DWireframe, "wireframe" },
};

// The draw mode a key press asks for, or -1.
int
drawModeForKey(Qt::KeyboardModifiers modifiers,
               int key)
{
    for (const DrawModeEntry& entry : kDrawModes) {
        if (matchesKeyBinding(entry.actionID, modifiers, key)) {
            return (int)entry.mode;
        }
    }

    return -1;
}

const char*
drawModeName(SceneViewportRenderer::DrawModeEnum mode)
{
    for (const DrawModeEntry& entry : kDrawModes) {
        if (entry.mode == mode) {
            return entry.name;
        }
    }

    return "";
}

// The lighting modes, their menu labels and script names.
struct LightingModeEntry {
    SceneViewportRenderer::LightingModeEnum mode;
    const char* label;
    const char* name;
};

const LightingModeEntry kLightingModes[] = {
    { SceneViewportRenderer::eLightingModeAuto, QT_TRANSLATE_NOOP("SceneViewport", "Auto"), "auto" },
    { SceneViewportRenderer::eLightingModeScene, QT_TRANSLATE_NOOP("SceneViewport", "Scene Lights"), "scene" },
    { SceneViewportRenderer::eLightingModeHeadlight, QT_TRANSLATE_NOOP("SceneViewport", "Headlight"), "headlight" },
};

const char*
lightingModeName(SceneViewportRenderer::LightingModeEnum mode)
{
    for (const LightingModeEntry& entry : kLightingModes) {
        if (entry.mode == mode) {
            return entry.name;
        }
    }

    return "";
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
// As a GUI edit, not a user one: a knob's widget skips refreshing on a user edit, which it takes
// for one typed into it, so the camera's panel would go on showing the old values.
void
setCameraKnobValue(const KnobDoublePtr& knob,
                   int dimension,
                   double frame,
                   bool animated,
                   double value)
{
    if (animated) {
        knob->setValueAtTime(frame, value, ViewSpec::all(), dimension, eValueChangedReasonNatronGuiEdited, nullptr);
    } else {
        knob->setValue(value, ViewSpec::all(), dimension, eValueChangedReasonNatronGuiEdited, nullptr);
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
    // Made by the widget, which its deliveries run on.
    std::unique_ptr<SceneDrawStateQueue> drawStates;
    std::size_t drawRevealBatch = SceneViewportRenderer::kRevealPaced;
    // A restore's load set until the stage has taken its loads, which a save reads meanwhile.
    mutable std::mutex restoredLoadsMutex;
    SceneLoadRules restoredLoads;
    QString error;
    bool glInitialized = false;
    // The delegate rendering as of the last rendererChanged().
    std::string announcedRenderer = kRenderDelegateStormId;
    // A switch of renderer waits for the background frame: making it waits on the render thread.
    bool followSceneOwed = false;

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

    // What the view showed: a deferred pick means what was under the mouse then, or nothing.
    struct PressView {
        SceneViewportRenderer::FreeCamera camera;
        std::string lookThrough;
        int width = 0;
        int height = 0;

        bool operator==(const PressView& other) const
        {
            return std::equal(camera.pivot, camera.pivot + 3, other.camera.pivot) && (camera.distance == other.camera.distance)
                && (camera.azimuth == other.camera.azimuth) && (camera.elevation == other.camera.elevation) && (camera.fovY == other.camera.fovY)
                && (camera.viewMode == other.camera.viewMode) && (lookThrough == other.lookThrough) && (width == other.width) && (height == other.height);
        }
    };

    PressView currentView(const QWidget* widget) const
    {
        PressView view;

        view.camera = renderer.getFreeCamera();
        view.lookThrough = renderer.getLookThroughCamera();
        view.width = widget->width();
        view.height = widget->height();

        return view;
    }

    // The press under way found the stage held: it has yet to find out what it landed on.
    bool pressOwedHandle = false;
    bool pressOwedPick = false;
    PressView pressView;

    // A click or marquee released before the stage could say what it hit, oldest first.
    struct DeferredSelection {
        QPoint pos;
        QRect rect;
        bool marquee = false;
        // A handle at pos would have taken the press, so the click selects nothing there.
        bool checkHandle = false;
        // A drag that no binding gives a role leaves the selection alone if it hit nothing.
        bool emptySelects = true;
        SelectionModeEnum mode = eSelectionModeReplace;
        PressView view;
    };
    std::deque<DeferredSelection> deferredSelections;

    // The handle drag found the stage held: the last move has yet to be drawn, or if changeStale,
    // even measured, which the release needs before it can write the drag.
    bool handleMoveOwed = false;
    bool handleChangeStale = false;
    QPoint handleMovePos;
    bool handleEndOwed = false;

    bool previewClearOwed = false;
    bool cameraFromViewOwed = false;

    void clearPreview()
    {
        previewClearOwed = !renderer.clearTransformPreview();
    }

    // Space is down, while a binding of the 3D viewer uses it as a modifier.
    bool spaceHeld = false;

    // The project frame shown, and its rate once a scene or frame was set.
    double frame = 0.;
    double framesPerSecond = 0.;

    std::function<NodePtr()> transformNodeProvider;
    // The drag of a transform handle under way: the handle, the node whose op it moves (null
    // for the widget on the selection), and the last change it made. The renderer draws the
    // change as a preview; the release writes it once.
    int handle = SceneGizmo::kNoHandle;
    NodeWPtr handleNode;
    double handleChange = 0.;
    bool handleChanged = false;
    std::shared_ptr<SceneTransformTarget> transformTarget;
    SceneTransformRecorder transformRecorder;
    // Drops a preview whose written drag never reaches the scene shown, as when the node written
    // is not in the viewed branch. Long, since a heavy graph may take a while to evaluate before
    // it requests the scene with the drag.
    QTimer* previewTimer = nullptr;
    // While a written drag's preview is shown: the last request made before the write, so any
    // result for a later one ends the preview.
    bool previewAwaitsSync = false;
    U64 previewSerial = 0;

    std::function<NodePtr()> viewedNodeProvider;
    SceneViewport::SelectionActions selectionActions;
    std::function<NodePtr(const std::string&)> cameraNodeProvider;
    QPointer<UndoHistory> undoHistory;

    // Whether navigating through a stage camera moves its node, and why the last try could not.
    bool lockView = false;
    QString lockMessage;
    QString armedPick;

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

    // The child each parent's last Parent walk came from, which Child walks back to.
    QHash<QString, QString> pickwalkChildren;

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
    // The live textures the pending payload shows at pendingFrame, which the worker renders
    // before it syncs: Hydra only reads them.
    LiveTextureSourceList pendingLiveTextures;
    double pendingFrame = 0.;
    double pendingFramesPerSecond = 0.;
    U64 pendingSerial = 0;
    SceneLoadSetPtr pendingLoadSet;
    SceneLoadReachPtr pendingReach;
    // Main thread: what the next requestSync()'s payload was evaluated for.
    SceneLoadSetPtr requestedLoadSet;
    SceneLoadReachPtr requestedReach;

    // The draw rules of the scene the worker last matched, and how many times they changed; the
    // load thread matches them again after each load, so a pattern finds what the load brought.
    std::mutex drawRulesMutex;
    SceneDrawRules syncedDrawRules;
    U64 drawRulesGeneration = 0;
    // Under drawRulesMutex: what the rules of matchedRowsGeneration last matched, on the stage at
    // matchedRowsEditCount with matchedRowsLoadSet loaded. A match after loads walks only what
    // they changed.
    bool matchedRowsValid = false;
    SceneOps::SceneDrawRuleMatches matchedRows;
    U64 matchedRowsGeneration = 0;
    U64 matchedRowsEditCount = 0;
    SceneLoadSetPtr matchedRowsLoadSet;
    // The worker's matches, numbered, so that a sync knows which match its first frame waits on.
    std::atomic<U64> matchesIssued { 0 };
    // Main thread: the policy last put in the draw states, and the generation it was matched for.
    SceneDrawStates::PolicyStates appliedPolicy;
    U64 appliedDrawRulesGeneration = 0;
    // Main thread: the last of the worker's matches handed over, and the one frames leave the stage
    // out for, 0 for none. Only the first sync of a newly viewed node holds the stage, as it has no
    // policy before to draw with; policyHoldTimer bounds the hold.
    U64 appliedMatch = 0;
    U64 heldMatch = 0;
    QTimer* policyHoldTimer = nullptr;
    // Main thread: the last of the worker's matches cancelled before handing over a policy, and the
    // first request whose draw rules or stack may call for a match, 0 for none.
    U64 droppedMatch = 0;
    U64 policyRequestSerial = 0;
    // The load thread's rounds, each a batch and the match after it, taken and handed over to the
    // main thread: the stage counts no loads outstanding during that match, which may ask for more.
    std::atomic<U64> loadRoundsTaken { 0 };
    U64 loadRoundsServiced = 0;
    // Main thread: the node the last setScene() showed, and the first request made for a node newly
    // viewed while no sync of it has come back, 0 for none.
    NodeWPtr sceneNode;
    U64 newNodeSerial = 0;
    // Main thread: draw state edits that wait for the policy of a graph edit made with them, so that
    // one frame shows both, and the first sync serial and rules generation whose match runs them.
    std::vector<std::function<void()>> policyEdits;
    U64 policyEditsSerial = 0;
    U64 policyEditsGeneration = 0;
    QTimer* policyEditsTimer = nullptr;

    // Main thread: whether frames leave the stage out until the graph policy is in the draw states.
    bool policyHeld() const
    {
        return (heldMatch != 0) && ((appliedMatch < heldMatch) || (drawStates && (drawStates->getOutstandingCount() > 0)));
    }

    /**
     * @brief Any thread but the main one: matches rules on the stage shown and hands the policy to
     * view, with matchId, 0 for none, and the serial of the sync matched for, 0 for a match after
     * loads. False, handing nothing, once cancelled turns true. After loads, a match of the
     * generation last matched on the same stack walks only the subtrees whose loads changed since.
     **/
    bool matchDrawRules(SceneViewport* view,
                        const SceneDrawRules& rules,
                        U64 generation,
                        U64 matchId,
                        U64 serial,
                        const std::function<bool()>& cancelled,
                        bool afterLoads)
    {
        SceneOps::SceneDrawRuleMatches matches;
        bool cached = false;
        U64 cachedEditCount = 0;
        SceneLoadSetPtr cachedLoadSet;
        if (afterLoads) {
            std::lock_guard<std::mutex> lock(drawRulesMutex);
            if (matchedRowsValid && (matchedRowsGeneration == generation)) {
                matches = matchedRows;
                cachedEditCount = matchedRowsEditCount;
                cachedLoadSet = matchedRowsLoadSet;
                cached = true;
            }
        }
        bool matched = true;
        U64 editCount = 0;
        SceneLoadSetPtr loadSet;
        renderer.readStage([&](const SceneStage* stage) {
            const UsdStageRefPtr usdStage = stage ? stage->getStage() : UsdStageRefPtr();
            if (stage) {
                editCount = stage->getEditCount();
                loadSet = stage->getLoadSet();
            }
            if (cached && stage && (editCount == cachedEditCount)) {
                matched = SceneOps::rematchDrawRulesUnder(usdStage, rules, SceneLoadSet::getChangedPaths(cachedLoadSet.get(), loadSet.get()), &matches, cancelled);
            } else {
                matched = SceneOps::matchDrawRules(usdStage, rules, &matches, cancelled);
            }
        });
        if (!matched) {
            if (matchId != 0) {
                QMetaObject::invokeMethod(view, [view, matchId]() { view->onDrawRulesMatchDropped(matchId); }, Qt::QueuedConnection);
            }

            return false;
        }
        SceneDrawStates::PolicyStates policy;
        SceneOps::getDrawRulePolicy(rules, matches, &policy);
        {
            std::lock_guard<std::mutex> lock(drawRulesMutex);
            if (!matchedRowsValid || (generation > matchedRowsGeneration) || ((generation == matchedRowsGeneration) && (editCount >= matchedRowsEditCount))) {
                matchedRowsValid = true;
                matchedRows.swap(matches);
                matchedRowsGeneration = generation;
                matchedRowsEditCount = editCount;
                matchedRowsLoadSet = loadSet;
            }
        }
        // Dropped by Qt if the widget is gone by then.
        QMetaObject::invokeMethod(view, [view, generation, matchId, serial, policy]() { view->onDrawRulesMatched(generation, matchId, serial, policy); }, Qt::QueuedConnection);

        return true;
    }

    // Worker: publishes rules for the load thread, as a new generation when they changed.
    U64 publishDrawRules(const SceneDrawRules& rules)
    {
        std::lock_guard<std::mutex> lock(drawRulesMutex);
        if (rules != syncedDrawRules) {
            syncedDrawRules = rules;
            ++drawRulesGeneration;
        }

        return drawRulesGeneration;
    }

    // Main thread: the last request made, the last one synced, and the error the last request
    // shows once synced. A request whose payload shows live textures leaves the renderer at the
    // frame it had until the worker has rendered them, so Hydra never asks for one not stored.
    U64 requestedSerial = 0;
    U64 syncedSerial = 0;
    // A framing the worker has yet to take, newest only; the ids it was asked for and last answered.
    U64 pendingFramingId = 0;
    std::vector<std::string> pendingFramingSelection;
    U64 framingRequested = 0;
    U64 framingDone = 0;
    // The worker has yet to find where a newly looked-through stage camera turns about.
    bool pendingPivot = false;
    // A new engine reads each texture once and caches a fallback for one the store lacks under a
    // URI that does not change, so after the store was emptied it draws nothing until this sync.
    U64 paintHeldUntilSerial = 0;
    QString requestedError;
    ScenePayload requestedPayload;
    bool requestedLiveTextures = false;

    // The thread requests for payload loads are serviced on. It waits under this mutex's condition
    // for the stage to have requests; a request only needs to wake it.
    std::thread loadThread;
    std::mutex loadWakeMutex;
    std::condition_variable loadWake;
    // Also read by the load batch, which gives up on its files when it turns true.
    std::atomic<bool> loadQuit { false };

    // The thread the engine lives on, running jobs in order.
    std::thread renderThread;
    std::mutex renderJobsMutex;
    std::condition_variable renderJobsChanged;
    std::deque<std::function<void()>> renderJobs;
    bool renderQuit = false;
    // Whether the engine's context is the render thread's right now. Main thread.
    bool usesRenderThread = false;
    std::unique_ptr<QOffscreenSurface> renderSurface;
    // Render thread: its context, sharing the widget's, and the target it draws into.
    std::unique_ptr<QOpenGLContext> renderContext;
    GLuint renderFbo = 0;
    GLuint renderTexture = 0;
    GLuint renderDepth = 0;
    int renderWidth = 0;
    int renderHeight = 0;
    // The widget's context: framebuffer objects are not shared, so it has its own on the texture.
    GLuint presentFbo = 0;

    // Main thread.
    bool backgroundFrameRunning = false;
    // A renderer switch posted to the render thread, whose engine it drops, has not come back. The
    // switch numbered rendererSwitchLatest is the one asked for last: the render thread skips the
    // others, and the main thread waits for that one.
    bool rendererSwitchRunning = false;
    std::atomic<U64> rendererSwitchLatest { 0 };

    // Whether the render thread is on work the main thread does not wait for, and must not queue
    // behind.
    bool renderThreadBusy() const
    {
        return backgroundFrameRunning || rendererSwitchRunning;
    }
    // The load edit count the main thread has had onStageLoadsServiced() for.
    U64 takenLoadEditCount = 0;

    // Whether the worker or the load thread changed the stage and the main thread has not had its
    // onSynced() or onStageLoadsServiced() yet. These apply the first framing: a background frame
    // started before would draw from the unframed camera, and the paint after would render again.
    bool stageAwaitsTakeIn() const
    {
        return (renderer.getStageSyncSerial() > syncedSerial) || (renderer.getStageLoadEditCount() > takenLoadEditCount);
    }

    // Whether the framebuffer holds a frame of a scene, which a composing status can stay out of.
    bool presentedScene = false;
    QElapsedTimer composingClock;
    QTimer* composingTimer = nullptr;

    void postToRenderThread(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(renderJobsMutex);
            renderJobs.push_back(std::move(job));
        }
        renderJobsChanged.notify_one();
    }

    // Waits for job, after any job before it. The caller's own state is safe for job to read, as
    // the mutex hand-overs order them.
    void callOnRenderThread(const std::function<void()>& job)
    {
        std::mutex doneMutex;
        std::condition_variable doneChanged;
        bool done = false;

        postToRenderThread([&]() {
            job();
            std::lock_guard<std::mutex> lock(doneMutex);
            done = true;
            doneChanged.notify_one();
        });
        PythonUnlockedWhileWaiting unlocked;
        std::unique_lock<std::mutex> lock(doneMutex);
        doneChanged.wait(lock, [&done]() { return done; });
    }

    void startRenderThread()
    {
        if (renderThread.joinable()) {
            return;
        }
        renderThread = std::thread([this]() {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(renderJobsMutex);
                    renderJobsChanged.wait(lock, [this]() { return renderQuit || !renderJobs.empty(); });
                    if (renderJobs.empty()) {
                        return;
                    }
                    job = std::move(renderJobs.front());
                    renderJobs.pop_front();
                }
                job();
            }
        });
    }

    // Main thread, the widget's context current. False if this platform cannot, and the engine
    // then stays in the widget's context.
    bool startRenderContext(QOpenGLContext* share)
    {
        if (!QOpenGLContext::supportsThreadedOpenGL() || !AppManager::instance()->isOpenGLLoaded()) {
            return false;
        }
        // A surface is created on the main thread and may be used from any.
        renderSurface.reset(new QOffscreenSurface());
        renderSurface->setFormat(share->format());
        renderSurface->create();
        if (!renderSurface->isValid()) {
            renderSurface.reset();

            return false;
        }
        startRenderThread();
        bool started = false;
        callOnRenderThread([&]() {
            renderContext.reset(new QOpenGLContext());
            renderContext->setFormat(share->format());
            renderContext->setShareContext(share);
            started = renderContext->create() && QOpenGLContext::areSharing(renderContext.get(), share) && renderContext->makeCurrent(renderSurface.get());
            if (!started) {
                renderContext.reset();
            }
        });
        if (!started) {
            renderSurface.reset();
        }
        usesRenderThread = started;

        return started;
    }

    // Render thread. The target of a width x height frame, reallocated when the size changes.
    GLuint ensureRenderTarget(int width,
                              int height)
    {
        if (renderFbo && (renderWidth == width) && (renderHeight == height)) {
            return renderFbo;
        }
        if (!renderFbo) {
            glGenFramebuffers(1, &renderFbo);
            glGenTextures(1, &renderTexture);
            glGenRenderbuffers(1, &renderDepth);
        }
        glBindTexture(GL_TEXTURE_2D, renderTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, renderDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, renderFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderTexture, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, renderDepth);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        renderWidth = width;
        renderHeight = height;

        return renderFbo;
    }

    // Render thread: everything of the render context, the engine first.
    void releaseRenderContext()
    {
        if (!renderContext) {
            return;
        }
        renderer.releaseGL();
        glDeleteFramebuffers(1, &renderFbo);
        glDeleteTextures(1, &renderTexture);
        glDeleteRenderbuffers(1, &renderDepth);
        renderFbo = renderTexture = renderDepth = 0;
        renderWidth = renderHeight = 0;
        renderContext->doneCurrent();
        renderContext.reset();
    }

    // Main thread, the widget's context current, with the engine's context dying or the widget going.
    void releaseGLResources()
    {
        if (!usesRenderThread) {
            renderer.releaseGL();

            return;
        }
        callOnRenderThread([this]() { releaseRenderContext(); });
        if (presentFbo) {
            glDeleteFramebuffers(1, &presentFbo);
            presentFbo = 0;
        }
        renderSurface.reset();
        usesRenderThread = false;
        backgroundFrameRunning = false;
        presentedScene = false;
    }

    void stopRenderThread()
    {
        if (!renderThread.joinable()) {
            return;
        }
        postToRenderThread([this]() { releaseRenderContext(); });
        {
            std::lock_guard<std::mutex> lock(renderJobsMutex);
            renderQuit = true;
        }
        renderJobsChanged.notify_one();
        renderThread.join();
    }

    // Main thread, the widget's context current. The render thread has finished its work on the
    // texture when fence signals.
    void presentRenderTarget(GLuint target,
                             int width,
                             int height,
                             GLsync fence)
    {
        if (fence) {
            glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(fence);
        }
        if (!presentFbo) {
            glGenFramebuffers(1, &presentFbo);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, presentFbo);
        // Attached anew each time: a change made in another context is only seen after a bind, and
        // a resize has given the texture a new image.
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
    }
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
    setProperty("natronSceneArmedPick", QString());
    // A paint skipped while the worker syncs leaves the framebuffer as it was. Assumed from Qt's
    // documentation of QOpenGLWidget::UpdateBehavior, which says PartialUpdate keeps the
    // framebuffer's content between paints; not checked against Qt's source.
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);

    if (SceneDrawTiming* timing = _imp->renderer.drawTiming()) {
        // A trace is read after its frame has been shown, so its record changes once more.
        const auto republish = [this]() {
            setProperty("natronSceneDrawTimings", _imp->renderer.drawTiming()->records());
        };
        timing->setTraceReadCallback([this, republish]() { QMetaObject::invokeMethod(this, republish, Qt::QueuedConnection); });
    }
    _imp->drawStates.reset(new SceneDrawStateQueue([this](const std::function<void()>& run) { QMetaObject::invokeMethod(this, run, Qt::QueuedConnection); },
                                                   [this](const SceneDrawChange& change) { deliverDrawChange(change); }));

    _imp->wheelMoveTimer = new QTimer(this);
    _imp->wheelMoveTimer->setSingleShot(true);
    _imp->wheelMoveTimer->setInterval(300);
    QObject::connect(_imp->wheelMoveTimer, &QTimer::timeout, this, [this]() {
        if (_imp->button == Qt::NoButton) {
            endCameraLock();
        }
    });
    _imp->previewTimer = new QTimer(this);
    _imp->previewTimer->setSingleShot(true);
    _imp->previewTimer->setInterval(30000);
    QObject::connect(_imp->previewTimer, &QTimer::timeout, this, [this]() {
        if (!_imp->previewAwaitsSync || (_imp->handle != SceneGizmo::kNoHandle)) {
            return;
        }
        if ((_imp->requestedSerial > _imp->previewSerial) && (_imp->syncedSerial < _imp->requestedSerial)) {
            // The scene with the drag is on its way: its result ends the preview.
            _imp->previewTimer->start();

            return;
        }
        _imp->previewAwaitsSync = false;
        _imp->clearPreview();
        update();
    });
    // Queued: it runs on the reader's thread, and readers end before the viewport goes.
    _imp->renderer.setReadEndedCallback([this]() { QMetaObject::invokeMethod(this, [this]() { serviceDeferredInput(); }, Qt::QueuedConnection); });

    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();
    if (settings) {
        _imp->renderer.setGuideSize(settings->getViewer3DGuideSize());
        // The Viewer 3D Lighting preference is every 3D view's lighting mode.
        _imp->renderer.setLightingMode((SceneViewportRenderer::LightingModeEnum)settings->getViewer3DLighting());
        QObject::connect(settings.get(), &Settings::settingChanged, this, [this]() {
            SettingsPtr current = appPTR->getCurrentSettings();
            const double size = current ? current->getViewer3DGuideSize() : _imp->renderer.getGuideSize();
            const SceneViewportRenderer::LightingModeEnum lighting = current ? (SceneViewportRenderer::LightingModeEnum)current->getViewer3DLighting() : _imp->renderer.getLightingMode();
            if ((size != _imp->renderer.getGuideSize()) || (lighting != _imp->renderer.getLightingMode())) {
                _imp->renderer.setGuideSize(size);
                _imp->renderer.setLightingMode(lighting);
                publishState();
                update();
            }
        });
    }

    _imp->composingTimer = new QTimer(this);
    _imp->composingTimer->setSingleShot(true);
    _imp->composingTimer->setInterval(kComposingStatusDelayMs);
    QObject::connect(_imp->composingTimer, &QTimer::timeout, this, [this]() { update(); });

    _imp->policyHoldTimer = new QTimer(this);
    _imp->policyHoldTimer->setSingleShot(true);
    _imp->policyHoldTimer->setInterval(kPolicyHoldMs);
    QObject::connect(_imp->policyHoldTimer, &QTimer::timeout, this, [this]() { releasePolicyHold(); });
    _imp->policyEditsTimer = new QTimer(this);
    _imp->policyEditsTimer->setSingleShot(true);
    _imp->policyEditsTimer->setInterval(kPolicyHoldMs);
    QObject::connect(_imp->policyEditsTimer, &QTimer::timeout, this, [this]() { runPolicyEdits(); });

    Implementation* imp = _imp.get();
    _imp->loadThread = std::thread([this, imp]() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(imp->loadWakeMutex);
                imp->loadWake.wait(lock, [imp]() { return imp->loadQuit || imp->renderer.hasPendingStageLoads(); });
                if (imp->loadQuit) {
                    return;
                }
            }
            const U64 loadRound = ++imp->loadRoundsTaken;
            imp->renderer.serviceStageLoads([imp]() { return imp->loadQuit.load(); });
            if (imp->loadQuit) {
                return;
            }
            {
                SceneDrawRules rules;
                U64 generation = 0;
                {
                    std::lock_guard<std::mutex> lock(imp->drawRulesMutex);
                    rules = imp->syncedDrawRules;
                    generation = imp->drawRulesGeneration;
                }
                if (SceneOps::drawRulesNeedStage(rules)) {
                    const std::function<bool()> cancelled = [imp, generation]() {
                        if (imp->loadQuit) {
                            return true;
                        }
                        // Newer rules are matched by the worker, on the whole stage.
                        std::lock_guard<std::mutex> lock(imp->drawRulesMutex);

                        return imp->drawRulesGeneration != generation;
                    };
                    imp->matchDrawRules(this, rules, generation, 0, 0, cancelled, true);
                }
            }
            // Payloads without extent hints leave nothing to frame until one of them loads.
            const U64 loadEditCount = imp->renderer.getStageLoadEditCount();
            const SceneViewportRenderer::FrameBounds bounds = imp->renderer.isFramed() ? SceneViewportRenderer::FrameBounds() : imp->renderer.computeFrameBounds(std::vector<std::string>(), imp->renderer.getTimeCode());
            // Dropped by Qt if the widget is gone by then.
            QMetaObject::invokeMethod(this, [this, bounds, loadEditCount, loadRound]() { onStageLoadsServiced(bounds, loadEditCount, loadRound); }, Qt::QueuedConnection);
        }
    });
    _imp->worker = std::thread([this, imp]() {
        // What the draw rules were last matched for, completely, and the scene the stage last took.
        bool rulesMatched = false;
        SceneDrawRules matchedRules;
        ScenePayload matchedFor;
        const std::function<bool()> newerRequest = [imp]() {
            std::lock_guard<std::mutex> lock(imp->queueMutex);

            return imp->quit || imp->hasPending;
        };
        for (;;) {
            ScenePayload payload;
            U64 serial = 0;
            LiveTextureSourceList liveTextures;
            double frame = 0.;
            double framesPerSecond = 0.;
            bool sync = false;
            U64 framingId = 0;
            std::vector<std::string> framingSelection;
            bool pivot = false;
            SceneLoadSetPtr loadSet;
            SceneLoadReachPtr reach;
            {
                std::unique_lock<std::mutex> lock(imp->queueMutex);
                imp->queueChanged.wait(lock, [imp]() { return imp->quit || imp->hasPending || (imp->pendingFramingId != 0) || imp->pendingPivot; });
                if (imp->quit) {
                    return;
                }
                sync = imp->hasPending;
                if (sync) {
                    payload = imp->pending;
                    imp->pending = ScenePayload();
                    imp->hasPending = false;
                    serial = imp->pendingSerial;
                    loadSet.swap(imp->pendingLoadSet);
                    reach.swap(imp->pendingReach);
                    liveTextures.swap(imp->pendingLiveTextures);
                    frame = imp->pendingFrame;
                    framesPerSecond = imp->pendingFramesPerSecond;
                }
                framingId = imp->pendingFramingId;
                imp->pendingFramingId = 0;
                framingSelection.swap(imp->pendingFramingSelection);
                pivot = imp->pendingPivot;
                imp->pendingPivot = false;
            }
            if (sync) {
                std::string error;
                bool ok;
                imp->renderer.setSyncEvaluatedFor(loadSet, reach);
                if (liveTextures.empty()) {
                    ok = imp->renderer.syncScene(payload, &error, serial);
                } else {
                    LiveTextureStore::instance().prepare(liveTextures, frame);
                    liveTextures.clear();
                    ok = imp->renderer.syncScene(payload, frame, framesPerSecond, &error, serial);
                }
                const QString qerror = QString::fromStdString(error);
                // The first framing lands with the sync, so the first frame drawn is framed.
                const SceneViewportRenderer::FrameBounds bounds = (!ok || imp->renderer.isFramed()) ? SceneViewportRenderer::FrameBounds() : imp->renderer.computeFrameBounds(std::vector<std::string>(), imp->renderer.getTimeCode());
                // A sync the stage skipped, as evaluated for loads it no longer has, leaves the
                // stage on the scene before: the scene evaluated again comes as a new request.
                const bool taken = ok && (imp->renderer.getStageSyncSerial() >= serial);
                const SceneDrawRules& rules = payload.getDrawRules();
                bool matches = false;
                bool needsStage = false;
                U64 generation = 0;
                U64 matchId = 0;
                U64 awaitedMatch = 0;
                if (taken) {
                    const bool stackChanged = !payload.isSameStack(matchedFor);
                    needsStage = SceneOps::drawRulesNeedStage(rules);
                    matchedFor = payload;
                    if (!rulesMatched || (rules != matchedRules) || (stackChanged && needsStage)) {
                        matches = true;
                        generation = imp->publishDrawRules(rules);
                        matchId = ++imp->matchesIssued;
                        // A frame of the stage before still shows the policy matched on it.
                        awaitedMatch = stackChanged ? matchId : 0;
                    }
                }
                if (matches && !needsStage) {
                    // Plain paths match at once: the policy lands before the sync does.
                    rulesMatched = imp->matchDrawRules(this, rules, generation, matchId, serial, newerRequest, false);
                    matchedRules = rules;
                }
                // Dropped by Qt if the widget is gone by then.
                QMetaObject::invokeMethod(this, [this, serial, ok, qerror, bounds, awaitedMatch]() { onSynced(serial, ok, qerror, bounds, awaitedMatch); }, Qt::QueuedConnection);
                if (matches && needsStage) {
                    rulesMatched = imp->matchDrawRules(this, rules, generation, matchId, serial, newerRequest, false);
                    matchedRules = rules;
                }
            }
            if (framingId != 0) {
                const SceneViewportRenderer::FrameBounds bounds = imp->renderer.computeFrameBounds(framingSelection, imp->renderer.getTimeCode());
                QMetaObject::invokeMethod(this, [this, framingId, bounds]() { onFramingComputed(framingId, bounds); }, Qt::QueuedConnection);
            }
            if (pivot) {
                imp->renderer.computeFrameBounds(std::vector<std::string>(), imp->renderer.getTimeCode());
            }
        }
    });
    // A progressive render's new frame repaints.
    _imp->renderer.setRefineListener([this]() {
        QMetaObject::invokeMethod(this, [this]() { update(); }, Qt::QueuedConnection);
    });
    publishState();
}

SceneViewport::~SceneViewport()
{
    Q_EMIT closing();
    _imp->drawStates.reset();
    _imp->renderer.setRefineListener(std::function<void()>());
    // Every thread is told before any is waited for: the worker's sync may wait for a frame, and
    // the load batch gives up on the files it is opening.
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->quit = true;
    }
    {
        std::lock_guard<std::mutex> lock(_imp->loadWakeMutex);
        _imp->loadQuit = true;
    }
    ++_imp->rendererSwitchLatest;
    _imp->renderer.cancelBackgroundFrame();
    _imp->queueChanged.notify_all();
    _imp->loadWake.notify_all();
    if (_imp->worker.joinable()) {
        _imp->worker.join();
    }
    if (_imp->loadThread.joinable()) {
        _imp->loadThread.join();
    }
    GLContextLoss::releaseNow(this);
    _imp->stopRenderThread();
}

void
SceneViewport::requestSync(const ScenePayload& payload,
                           const QString& error,
                           bool keepsFrame)
{
    _imp->requestedError = error;
    const SceneDrawRules& rules = payload.getDrawRules();
    const bool mayMatch = (rules != _imp->requestedPayload.getDrawRules()) || (!payload.isSameStack(_imp->requestedPayload) && SceneOps::drawRulesNeedStage(rules));
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
        if (mayMatch) {
            _imp->policyRequestSerial = _imp->requestedSerial;
        }
        _imp->pendingLoadSet = payload.isEmpty() ? SceneLoadSetPtr() : _imp->requestedLoadSet;
        _imp->pendingReach = payload.isEmpty() ? SceneLoadReachPtr() : _imp->requestedReach;
    }
    _imp->requestedLoadSet.reset();
    _imp->requestedReach.reset();
    if (!keepsFrame) {
        // A frame of the stage this sync replaces is not worth the rest of its passes.
        _imp->renderer.stopBackgroundFrame();
    }
    _imp->queueChanged.notify_one();
    publishState();
    if (!_imp->renderer.hasScene()) {
        // The composing status goes up without waiting for the worker.
        update();
    }
}

void
SceneViewport::onSynced(U64 serial,
                        bool ok,
                        const QString& error,
                        const SceneViewportRenderer::FrameBounds& bounds,
                        U64 awaitedMatch)
{
    _imp->syncedSerial = std::max(_imp->syncedSerial, serial);
    const bool firstOfNode = (_imp->newNodeSerial != 0) && (serial >= _imp->newNodeSerial);
    if (firstOfNode) {
        _imp->newNodeSerial = 0;
    }
    // Any other sync draws with the policy the view has until the new one lands: playback and
    // scrubbing never stop drawing. An overtaken one is drawn at once too.
    if (firstOfNode && (serial == _imp->requestedSerial) && (awaitedMatch != 0) && ((awaitedMatch > _imp->appliedMatch) || (_imp->drawStates && (_imp->drawStates->getOutstandingCount() > 0)))) {
        holdStageUntilMatch(awaitedMatch);
    }
    wakeLoadsAskedBeforeStage();
    // The synced stage may name another renderer for Scene Default.
    followSceneRenderer();
    if (_imp->previewAwaitsSync && (serial > _imp->previewSerial)) {
        // A good sync normally drops the preview with the stage update; this covers a failed one
        // and one synced before the drag's write was known to the renderer.
        _imp->previewAwaitsSync = false;
        _imp->previewTimer->stop();
        _imp->clearPreview();
        update();
    }
    serviceDeferredInput();
    if (serial != _imp->requestedSerial) {
        // A newer request is on its way, but this one is drawn meanwhile: in playback each frame's
        // request is overtaken by the next before its sync ends, so drawing only caught-up syncs
        // would draw almost none. The renderer is consistent: the stage at the frame whose
        // textures were just rendered.
        publishState();
        if (ok) {
            update();
        }
        noteDrawPolicySettled();

        return;
    }
    _imp->error = ok ? _imp->requestedError : error;
    if (_imp->lockedMoveAwaitsSync && !_imp->cameraLock.active && (serial > _imp->lockedMoveSerial)) {
        // The stage has the written move: the camera's prim is where the view shows it.
        _imp->lockedMoveAwaitsSync = false;
        _imp->renderer.clearLookThroughTransform();
    }
    if (ok && _imp->renderer.hasScene() && !_imp->renderer.isFramed()) {
        _imp->renderer.applyFrameBounds(bounds);
        endLockOutsideLookThrough();
    }
    // With natronSceneLastPaintMs, lets a benchmark time an edit to the frame that shows it.
    setProperty("natronSceneSyncedAtMs", QVariant::fromValue<qlonglong>(QDateTime::currentMSecsSinceEpoch()));
    startBackgroundFrame();
    publishState();
    update();
    Q_EMIT sceneSynced();
    noteDrawPolicySettled();
}

void
SceneViewport::followSceneRenderer()
{
    // Dropping the old engine needs its context, which a view not yet shown has not made: it has
    // no engine then either.
    if (_imp->renderThreadBusy()) {
        _imp->followSceneOwed = true;

        return;
    }
    _imp->followSceneOwed = false;
    bool changed = false;
    if (_imp->glInitialized && context()) {
        runWithEngineContext([this, &changed]() { changed = _imp->renderer.followSceneRenderer(); });
    } else {
        changed = _imp->renderer.followSceneRenderer();
    }
    if (changed) {
        publishState();
        publishActiveRenderer();
        update();
    }
}

bool
SceneViewport::isSceneSynced() const
{
    if ((_imp->syncedSerial != _imp->requestedSerial) || (_imp->framingDone != _imp->framingRequested) || _imp->policyHeld() || !_imp->policyEdits.empty()) {
        return false;
    }
    // A view nobody sees takes nothing in until it is shown.
    const bool awaitsEngine = _imp->usesRenderThread && isVisible() && (width() > 0) && (height() > 0);

    return !awaitsEngine || (!_imp->renderThreadBusy() && !_imp->renderer.needsBackgroundFrame());
}

bool
SceneViewport::isStageSynced() const
{
    return _imp->syncedSerial == _imp->requestedSerial;
}

void
SceneViewport::startBackgroundFrame()
{
    if (!_imp->usesRenderThread || _imp->renderThreadBusy() || !isVisible() || (_imp->syncedSerial < _imp->paintHeldUntilSerial) || _imp->stageAwaitsTakeIn() || _imp->policyHeld()) {
        return;
    }
    const qreal dpr = devicePixelRatio();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    if (!_imp->renderer.prepareBackgroundFrame(w, h)) {
        return;
    }
    _imp->backgroundFrameRunning = true;
    _imp->composingClock.start();
    _imp->composingTimer->start();
    Implementation* imp = _imp.get();
    _imp->postToRenderThread([this, imp, w, h]() {
        imp->renderer.renderBackgroundFrame(imp->ensureRenderTarget(w, h));
        // The widget's context reads the image only after a later fence, but the commands must be
        // on their way before this thread goes idle.
        glFlush();
        // Dropped by Qt if the widget is gone by then.
        QMetaObject::invokeMethod(this, [this]() { onBackgroundFrameDone(); }, Qt::QueuedConnection);
    });
    publishState();
}

void
SceneViewport::onBackgroundFrameDone()
{
    if (SceneDrawTiming* timing = _imp->renderer.drawTiming()) {
        timing->frameDone(std::chrono::steady_clock::now());
    }
    _imp->backgroundFrameRunning = false;
    if (!_imp->rendererSwitchRunning) {
        _imp->composingTimer->stop();
    }
    if (_imp->followSceneOwed || _imp->renderer.hasPendingRendererSwitch()) {
        followSceneRenderer();
    }
    // The stage may have changed again while the frame ran.
    startBackgroundFrame();
    serviceDeferredInput();
    publishState();
    update();
}

void
SceneViewport::onStageLoadsServiced(const SceneViewportRenderer::FrameBounds& bounds,
                                    U64 loadEditCount,
                                    U64 loadRound)
{
    _imp->takenLoadEditCount = std::max(_imp->takenLoadEditCount, loadEditCount);
    _imp->loadRoundsServiced = std::max(_imp->loadRoundsServiced, loadRound);
    if (!_imp->renderer.hasPendingStageLoads()) {
        forgetRestoredLoads();
    }
    if (!bounds.empty && !_imp->renderer.isFramed()) {
        _imp->renderer.applyFrameBounds(bounds);
        endLockOutsideLookThrough();
    }
    startBackgroundFrame();
    serviceDeferredInput();
    publishState();
    update();
    Q_EMIT stageLoadsChanged();
    noteDrawPolicySettled();
}
void
SceneViewport::serviceDeferredInput()
{
    if (_imp->previewClearOwed) {
        _imp->clearPreview();
        if (!_imp->previewClearOwed) {
            update();
        }
    }
    if (_imp->cameraFromViewOwed) {
        _imp->cameraFromViewOwed = false;
        createCameraFromView();
    }
    if (_imp->handle != SceneGizmo::kNoHandle) {
        if (_imp->handleMoveOwed) {
            dragHandle(_imp->handleMovePos);
        }
        if (_imp->handleEndOwed && !_imp->handleChangeStale) {
            _imp->handleEndOwed = false;
            endHandleDrag();
        }
    }
    const Implementation::PressView view = _imp->currentView(this);
    const qreal dpr = devicePixelRatio();
    while (!_imp->deferredSelections.empty()) {
        const Implementation::DeferredSelection deferred = _imp->deferredSelections.front();
        bool busy = false;
        if (deferred.view == view) {
            if (deferred.checkHandle) {
                const GfVec2d pixel = Implementation::toPixel(this, deferred.pos);
                if (_imp->renderer.hitTestTransformHandles(pixel[0], pixel[1], int(width() * dpr), int(height() * dpr), &busy) != SceneGizmo::kNoHandle) {
                    _imp->deferredSelections.pop_front();
                    continue;
                }
                if (busy) {
                    return;
                }
            }
            QStringList paths;
            if (deferred.marquee) {
                paths = pickPrimsIn(deferred.rect, &busy);
            } else {
                const QString prim = pickPrimAt(deferred.pos, &busy);
                if (!prim.isEmpty()) {
                    paths.push_back(prim);
                }
            }
            if (busy) {
                return;
            }
            _imp->deferredSelections.pop_front();
            if (!paths.isEmpty() || deferred.emptySelects) {
                select(paths, deferred.mode);
            }
            continue;
        }
        // The view moved on: selecting what is there now is not what was clicked.
        _imp->deferredSelections.pop_front();
    }
    if (_imp->button == Qt::NoButton) {
        return;
    }
    const bool navigated = _imp->dragging && (_imp->dragRole != Implementation::eDragRoleMarquee) && (_imp->dragRole != Implementation::eDragRoleNone);
    if (_imp->pressOwedHandle) {
        if (navigated || !(_imp->pressView == view)) {
            _imp->pressOwedHandle = false;
        } else {
            bool busy = false;
            if (beginHandleDrag(_imp->pressPos, &busy)) {
                const QPoint pos = _imp->lastPos;
                clearGesture();
                dragHandle(pos);

                return;
            }
            if (busy) {
                return;
            }
            _imp->pressOwedHandle = false;
        }
    }
    // Left owed if the view moved: the release drops it with the view it remembers.
    if (_imp->pressOwedPick && (_imp->pressView == view)) {
        bool busy = false;
        const QString prim = pickPrimAt(_imp->pressPos, &busy);
        if (!busy) {
            _imp->pressPrim = prim;
            _imp->pressOwedPick = false;
        }
    }
} // SceneViewport::serviceDeferredInput

void
SceneViewport::runWithEngineContext(const std::function<void()>& fn)
{
    if (_imp->usesRenderThread) {
        _imp->callOnRenderThread(fn);
    } else {
        makeCurrent();
        fn();
        doneCurrent();
    }
}

bool
SceneViewport::isComposing() const
{
    return _imp->renderThreadBusy() || ((_imp->syncedSerial < _imp->requestedSerial) && !_imp->requestedPayload.isEmpty() && !_imp->renderer.hasScene());
}

void
SceneViewport::paintComposingStatus()
{
    if (!_imp->presentedScene) {
        float background[3];
        _imp->renderer.getBackgroundColor(background);
        glClearColor(background[0], background[1], background[2], 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    const QString text = tr("Composing scene...");
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QRect box = painter.fontMetrics().boundingRect(text).adjusted(-10, -6, 10, 6);
    box.moveCenter(rect().center());
    // Opaque: paints repeat over a framebuffer that keeps its content, and text alone would pile up.
    painter.fillRect(box, QColor(40, 40, 40));
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(box, Qt::AlignCenter, text);
    painter.end();
}

namespace {
std::vector<std::string>
toStdPaths(const QStringList& paths)
{
    std::vector<std::string> result;

    result.reserve(paths.size());
    for (const QString& path : paths) {
        result.push_back(path.toStdString());
    }

    return result;
}

// Read only while draw timing is on, so the change costs the main thread nothing otherwise.
std::chrono::steady_clock::time_point
drawChangeStart(const SceneViewportRenderer& renderer)
{
    return renderer.drawTiming() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
}
} // anonymous namespace

void
SceneViewport::requestLoad(const QStringList& paths,
                           bool withDescendants)
{
    _imp->renderer.requestStageLoad(toStdPaths(paths), withDescendants);
    // Taking the mutex keeps the wake-up from falling between the thread's check and its wait.
    {
        std::lock_guard<std::mutex> lock(_imp->loadWakeMutex);
    }
    _imp->loadWake.notify_one();
    publishState();
}

void
SceneViewport::publishDrawStats()
{
    const SceneViewportRenderer::DrawFilterStats stats = _imp->renderer.getDrawFilterStats();
    QVariantMap published;
    published[QString::fromUtf8("filterBound")] = stats.filterBound;
    published[QString::fromUtf8("revealing")] = stats.revealing;
    published[QString::fromUtf8("revealBatch")] = QVariant::fromValue<qulonglong>(stats.revealBatch);
    published[QString::fromUtf8("revealFixedMs")] = stats.revealFixedMs;
    published[QString::fromUtf8("revealPerPrimUs")] = stats.revealPerPrimUs;
    published[QString::fromUtf8("revealSteps")] = QVariant::fromValue<qulonglong>(stats.revealSteps);
    published[QString::fromUtf8("lastApplyMs")] = stats.lastApplyMs;
    published[QString::fromUtf8("ruleCount")] = QVariant::fromValue<qulonglong>(stats.ruleCount);
    published[QString::fromUtf8("posted")] = QVariant::fromValue<qulonglong>(stats.posted);
    published[QString::fromUtf8("absorbed")] = QVariant::fromValue<qulonglong>(stats.absorbed);
    published[QString::fromUtf8("inputAdded")] = QVariant::fromValue<qulonglong>(stats.inputAdded);
    published[QString::fromUtf8("inputRemoved")] = QVariant::fromValue<qulonglong>(stats.inputRemoved);
    published[QString::fromUtf8("inputDirtied")] = QVariant::fromValue<qulonglong>(stats.inputDirtied);
    published[QString::fromUtf8("hiddenSent")] = QVariant::fromValue<qulonglong>(stats.hiddenSent);
    published[QString::fromUtf8("shownSent")] = QVariant::fromValue<qulonglong>(stats.shownSent);
    published[QString::fromUtf8("visibilitySent")] = QVariant::fromValue<qulonglong>(stats.visibilitySent);
    published[QString::fromUtf8("heldInvisible")] = QVariant::fromValue<qulonglong>(stats.heldInvisible);
    published[QString::fromUtf8("maskedInstances")] = QVariant::fromValue<qulonglong>(stats.maskedInstances);
    published[QString::fromUtf8("lastInputAddedEpochMs")] = QVariant::fromValue<qlonglong>(stats.lastInputAddedEpochMs);
    published[QString::fromUtf8("stageEdits")] = QVariant::fromValue<qulonglong>(stats.stageEdits);
    published[QString::fromUtf8("stageLoadEdits")] = QVariant::fromValue<qulonglong>(stats.stageLoadEdits);
    published[QString::fromUtf8("stageNotices")] = QVariant::fromValue<qulonglong>(stats.stageNotices);
    published[QString::fromUtf8("resyncedPaths")] = QVariant::fromValue<qulonglong>(stats.resyncedPaths);
    published[QString::fromUtf8("infoOnlyPaths")] = QVariant::fromValue<qulonglong>(stats.infoOnlyPaths);
    setProperty("natronSceneDrawStats", published);
    const int boxes = _imp->renderer.getDrawnBoxCount();
    if ((boxes > 0) && (property("natronSceneDrawBoxes").toInt() == 0)) {
        setProperty("natronSceneFirstBoxesMs", QVariant::fromValue<qlonglong>(QDateTime::currentMSecsSinceEpoch()));
    }
    setProperty("natronSceneDrawBoxes", boxes);
    setProperty("natronSceneDrawMarkers", _imp->renderer.getDrawnMarkerCount());
}

void
SceneViewport::onDrawRulesMatched(U64 generation,
                                  U64 matchId,
                                  U64 serial,
                                  const SceneDrawStates::PolicyStates& policy)
{
    _imp->appliedMatch = std::max(_imp->appliedMatch, matchId);
    if ((generation > _imp->appliedDrawRulesGeneration) || ((generation == _imp->appliedDrawRulesGeneration) && (policy != _imp->appliedPolicy))) {
        _imp->appliedDrawRulesGeneration = generation;
        // The rows dropped from the policy are walked as well as the ones it now holds.
        const std::size_t rows = std::max(_imp->appliedPolicy.size(), policy.size());
        _imp->appliedPolicy = policy;
        editDrawStates([policy](SceneDrawStates* states) { return states->replacePolicyStates(policy); }, rows);
    }
    if (!_imp->policyEdits.empty() && (((serial != 0) && (serial >= _imp->policyEditsSerial)) || (generation >= _imp->policyEditsGeneration))) {
        runPolicyEdits();
    }
    releasePolicyHoldIfDone();
    noteDrawPolicySettled();
}

void
SceneViewport::onDrawRulesMatchDropped(U64 matchId)
{
    _imp->droppedMatch = std::max(_imp->droppedMatch, matchId);
    noteDrawPolicySettled();
}

bool
SceneViewport::isDrawPolicyPending() const
{
    const Implementation& imp = *_imp;

    return imp.policyHeld() || (imp.newNodeSerial != 0) || (imp.syncedSerial < imp.policyRequestSerial)
        || (std::max(imp.appliedMatch, imp.droppedMatch) < imp.matchesIssued.load())
        || (imp.loadRoundsServiced < imp.loadRoundsTaken.load())
        || (imp.drawStates && (imp.drawStates->getOutstandingCount() > 0)) || !imp.policyEdits.empty();
}

void
SceneViewport::noteDrawPolicySettled()
{
    if (!isDrawPolicyPending() && !_imp->renderer.hasOutstandingStageLoads()) {
        Q_EMIT drawPolicySettled();
    }
}

void
SceneViewport::holdStageUntilMatch(U64 matchId)
{
    _imp->heldMatch = std::max(_imp->heldMatch, matchId);
    _imp->renderer.setStageHeld(true);
    if (!_imp->policyHoldTimer->isActive()) {
        // Bounded from when the hold begins, so that a slow match never leaves the stage out long.
        _imp->policyHoldTimer->start();
    }
}

void
SceneViewport::editWithGraphPolicy(const std::function<void()>& edit)
{
    _imp->policyEdits.push_back(edit);
    // A request made from now on is evaluated with the graph edit, as are rules published after it.
    _imp->policyEditsSerial = _imp->requestedSerial + 1;
    {
        std::lock_guard<std::mutex> lock(_imp->drawRulesMutex);
        _imp->policyEditsGeneration = _imp->drawRulesGeneration + 1;
    }
    if (!_imp->policyEditsTimer->isActive()) {
        // Bounded from the first edit waiting, so that a graph edit leading to no match still shows.
        _imp->policyEditsTimer->start();
    }
}

void
SceneViewport::runPolicyEdits()
{
    _imp->policyEditsTimer->stop();
    std::vector<std::function<void()>> edits;
    edits.swap(_imp->policyEdits);
    for (const std::function<void()>& edit : edits) {
        edit();
    }
    noteDrawPolicySettled();
}

void
SceneViewport::releasePolicyHoldIfDone()
{
    if ((_imp->heldMatch != 0) && !_imp->policyHeld()) {
        releasePolicyHold();
    }
}

void
SceneViewport::releasePolicyHold()
{
    _imp->policyHoldTimer->stop();
    _imp->heldMatch = 0;
    _imp->renderer.setStageHeld(false);
    startBackgroundFrame();
    publishState();
    update();
    noteDrawPolicySettled();
}

void
SceneViewport::deliverDrawChange(const SceneDrawChange& change)
{
    if (_imp->renderer.postDrawChange(change, _imp->drawRevealBatch) && _imp->usesRenderThread) {
        Implementation* imp = _imp.get();
        _imp->postToRenderThread([imp]() { imp->renderer.applyPendingDrawDeltas(); });
    }
    if (!change.delta.loads.empty()) {
        // Taking the mutex keeps the wake-up from falling between the thread's check and its wait.
        {
            std::lock_guard<std::mutex> lock(_imp->loadWakeMutex);
        }
        _imp->loadWake.notify_one();
    }
    publishState();
    update();
    releasePolicyHoldIfDone();
    if (!change.cancelledLoads.empty()) {
        Q_EMIT stageLoadsChanged();
    }
    Q_EMIT drawStatesChanged();
    noteDrawPolicySettled();
}

namespace {
bool
isDrawStatePath(const std::string& path)
{
    return SdfPath::IsValidPathString(path) && SdfPath(path).IsAbsoluteRootOrPrimPath();
}

struct DrawKey {
    const char* actionID;
    SceneDrawStateEnum state;
    // Whether the state goes on the parents of the rows rather than on the rows.
    bool toParent;
};

const DrawKey kDrawKeys[] = {
    { kShortcutIDActionViewer3DDrawChildren, eSceneDrawStateChildren, false },
    { kShortcutIDActionViewer3DDrawFull, eSceneDrawStateFull, false },
    { kShortcutIDActionViewer3DDrawLoadAll, eSceneDrawStateLoadAll, false },
    { kShortcutIDActionViewer3DDrawParentBBox, eSceneDrawStateBBox, true },
};

const DrawKey*
drawKeyFor(Qt::KeyboardModifiers modifiers,
           int key)
{
    for (const DrawKey& drawKey : kDrawKeys) {
        if (matchesKeyBinding(drawKey.actionID, modifiers, key)) {
            return &drawKey;
        }
    }

    return nullptr;
}

/**
 * @brief The rows a draw state key acts on: for each selected prim, the row whose box stands for
 * it when the prim or an ancestor is boxed or hidden, the prim otherwise; with toParent, the
 * parents of those rows. So the down keys open the picked box, and the up key boxes the row above
 * it, again on each press.
 **/
std::vector<std::string>
drawKeyRows(const SceneDrawStates& states,
            const QStringList& selection,
            bool toParent)
{
    std::set<std::string> rows;

    for (const QString& selected : selection) {
        const std::string path = selected.toStdString();
        if (!SdfPath::IsValidPathString(path) || !SdfPath(path).IsPrimPath()) {
            continue;
        }
        SdfPath row(path);
        for (const SdfPath& prefix : row.GetPrefixes()) {
            const SceneDrawResolved resolved = states.resolve(prefix.GetString());
            if (!resolved.blocked && ((resolved.state == eSceneDrawStateBBox) || (resolved.state == eSceneDrawStateHidden))) {
                row = prefix;
                break;
            }
        }
        if (toParent) {
            row = row.GetParentPath();
            if (!row.IsPrimPath()) {
                continue;
            }
        }
        rows.insert(row.GetString());
    }

    return std::vector<std::string>(rows.begin(), rows.end());
}
} // anonymous namespace

void
SceneViewport::editDrawStates(const std::function<SceneDrawDelta(SceneDrawStates*)>& edit,
                              std::size_t rows)
{
    if (!_imp->drawStates) {
        return;
    }
    const std::chrono::steady_clock::time_point startedAt = drawChangeStart(_imp->renderer);
    // Nothing the stage still waits on or services came from an earlier change, so no change can
    // cancel it. A batch being serviced still takes cancels, for its follow-up loads.
    if (!_imp->renderer.hasOutstandingStageLoads()) {
        _imp->drawStates->forgetIssuedLoads();
    }
    _imp->drawStates->edit(edit, startedAt, rows);
}

void
SceneViewport::editDrawStateRows(const std::vector<SceneDrawRowEdit>& edits)
{
    if (!edits.empty()) {
        editDrawStates([edits](SceneDrawStates* states) { return states->editRows(edits); });
    }
}

namespace {
std::vector<std::string>
drawStatePaths(const QStringList& paths)
{
    std::vector<std::string> valid;
    for (const QString& path : paths) {
        const std::string pathString = path.toStdString();
        if (isDrawStatePath(pathString)) {
            valid.push_back(pathString);
        }
    }

    return valid;
}
} // anonymous namespace

bool
SceneViewport::setDrawStateOnRows(const QStringList& paths,
                                  const QString& state)
{
    SceneDrawStateEnum parsed = eSceneDrawStateFull;
    if (!sceneDrawStateFromName(state.toStdString(), &parsed)) {
        return false;
    }
    const std::vector<std::string> rows = drawStatePaths(paths);
    if (rows.size() != std::size_t(paths.size())) {
        return false;
    }
    std::vector<SceneDrawRowEdit> edits;
    for (const std::string& row : rows) {
        edits.push_back(SceneDrawRowEdit { SceneDrawRowEdit::eKindSet, row, parsed });
    }
    editDrawStateRows(edits);

    return true;
}

void
SceneViewport::clearDrawStatesBelowRows(const QStringList& paths)
{
    std::vector<SceneDrawRowEdit> edits;
    for (const std::string& row : drawStatePaths(paths)) {
        edits.push_back(SceneDrawRowEdit { SceneDrawRowEdit::eKindClearBelow, row, eSceneDrawStateFull });
    }
    editDrawStateRows(edits);
}

void
SceneViewport::resetDrawStatesToGraphOnRows(const QStringList& paths)
{
    std::vector<SceneDrawRowEdit> edits;
    for (const std::string& row : drawStatePaths(paths)) {
        edits.push_back(SceneDrawRowEdit { SceneDrawRowEdit::eKindClearBelow, row, eSceneDrawStateFull });
        edits.push_back(SceneDrawRowEdit { SceneDrawRowEdit::eKindClear, row, eSceneDrawStateFull });
    }
    editDrawStateRows(edits);
}

bool
SceneViewport::setDrawState(const QString& path,
                            const QString& state)
{
    const std::string pathString = path.toStdString();
    SceneDrawStateEnum parsed = eSceneDrawStateFull;
    if (!sceneDrawStateFromName(state.toStdString(), &parsed) || !isDrawStatePath(pathString)) {
        return false;
    }
    editDrawStates([pathString, parsed](SceneDrawStates* states) { return states->setState(pathString, parsed); });

    return true;
}

void
SceneViewport::clearDrawState(const QString& path)
{
    const std::string pathString = path.toStdString();
    if (isDrawStatePath(pathString)) {
        editDrawStates([pathString](SceneDrawStates* states) { return states->clearState(pathString); });
    }
}

void
SceneViewport::clearDrawStatesBelow(const QString& path)
{
    const std::string pathString = path.toStdString();
    if (isDrawStatePath(pathString)) {
        editDrawStates([pathString](SceneDrawStates* states) { return states->clearStatesBelow(pathString); });
    }
}

void
SceneViewport::resetDrawStatesToGraph(const QString& path)
{
    const std::string pathString = path.toStdString();
    if (!isDrawStatePath(pathString)) {
        return;
    }
    if (pathString == "/") {
        clearDrawStates();

        return;
    }
    resetDrawStatesToGraphOnRows(QStringList(path));
}

void
SceneViewport::clearDrawStates()
{
    editDrawStates([](SceneDrawStates* states) { return states->clearAllStates(); });
}

QString
SceneViewport::resolvedDrawState(const QString& path) const
{
    SceneDrawResolved resolved { eSceneDrawStateFull, eSceneDrawOriginDefault, false };
    if (_imp->drawStates) {
        const std::string pathString = path.toStdString();
        _imp->drawStates->read([&resolved, &pathString](const SceneDrawStates& states) { resolved = states.resolve(pathString); });
    }

    return QString::fromUtf8(sceneDrawStateName(resolved.state));
}

bool
SceneViewport::tryReadDrawStates(const std::function<void(const SceneDrawStates&)>& read) const
{
    return _imp->drawStates && _imp->drawStates->tryRead(read);
}

void
SceneViewport::setDrawRevealBatch(int prims)
{
    _imp->drawRevealBatch = (prims < 0) ? SceneViewportRenderer::kRevealPaced : std::size_t(prims);
}

void
SceneViewport::setDrawTimingTrace(bool trace)
{
    if (SceneDrawTiming* timing = _imp->renderer.drawTiming()) {
        timing->setTraceNext(trace);
    }
}

void
SceneViewport::requestUnload(const QStringList& paths)
{
    forgetRestoredLoads();
    _imp->renderer.requestStageUnload(toStdPaths(paths));
    {
        std::lock_guard<std::mutex> lock(_imp->loadWakeMutex);
    }
    _imp->loadWake.notify_one();
    publishState();
}

void
SceneViewport::wakeLoadsAskedBeforeStage()
{
    // The first sync's stage took the loads asked for before it, which woke no load thread.
    if (!_imp->renderer.hasPendingStageLoads()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_imp->loadWakeMutex);
    }
    _imp->loadWake.notify_one();
}

void
SceneViewport::forgetRestoredLoads()
{
    std::lock_guard<std::mutex> lock(_imp->restoredLoadsMutex);
    _imp->restoredLoads.clear();
}

void
SceneViewport::saveSceneView(Serialization::SceneViewSerialization* serialization) const
{
    std::shared_ptr<const SceneDrawStateQueue::SetStates> states;
    if (_imp->drawStates) {
        states = _imp->drawStates->getSetStates();
    }
    SceneLoadRules rules;
    _imp->renderer.getStageLoadRules(&rules);
    {
        std::lock_guard<std::mutex> lock(_imp->restoredLoadsMutex);
        rules.insert(_imp->restoredLoads.begin(), _imp->restoredLoads.end());
    }
    for (const std::pair<std::string, std::string>& failure : _imp->renderer.getStageLoadFailures()) {
        // Under an "all" rule the stage still asks for it; anywhere else the rules have lost it.
        SceneLoadRuleEnum governing = eSceneLoadRuleNone;
        for (SdfPath prefix(failure.first); !prefix.IsEmpty(); prefix = prefix.GetParentPath()) {
            const SceneLoadRules::const_iterator found = rules.find(prefix.GetString());
            if (found != rules.end()) {
                governing = (found->first == failure.first) ? eSceneLoadRuleOnly : found->second;
                break;
            }
        }
        if (governing == eSceneLoadRuleNone) {
            rules[failure.first] = eSceneLoadRuleOnly;
        }
    }
    sceneViewToSerialization(states ? *states : SceneDrawStateQueue::SetStates(), rules, serialization);
}

std::vector<std::pair<std::string, SceneDrawStates::Stored>>
SceneViewport::getSetDrawStates() const
{
    std::shared_ptr<const SceneDrawStateQueue::SetStates> states;
    if (_imp->drawStates) {
        states = _imp->drawStates->getSetStates();
    }

    return states ? *states : SceneDrawStateQueue::SetStates();
}

void
SceneViewport::replaceSetDrawStates(const std::vector<std::pair<std::string, SceneDrawStates::Stored>>& states)
{
    editDrawStates([states](SceneDrawStates* drawStates) { return drawStates->replaceSetStates(states); }, states.size());
}

void
SceneViewport::mergeSetDrawStates(const std::vector<std::pair<std::string, SceneDrawStates::Stored>>& states)
{
    editDrawStates([states](SceneDrawStates* drawStates) { return drawStates->mergeSetStates(states); }, states.size());
}

void
SceneViewport::clearSetDrawStates(const std::vector<std::string>& paths)
{
    editDrawStates([paths](SceneDrawStates* drawStates) { return drawStates->clearSetStates(paths); }, paths.size());
}

bool
SceneViewport::hasSetDrawStates() const
{
    const std::shared_ptr<const SceneDrawStateQueue::SetStates> states = _imp->drawStates ? _imp->drawStates->getSetStates() : std::shared_ptr<const SceneDrawStateQueue::SetStates>();

    return states && !states->empty();
}

void
SceneViewport::restoreSceneView(const Serialization::SceneViewSerialization& serialization)
{
    std::vector<std::pair<std::string, SceneDrawStates::Stored>> states;
    SceneLoadRules rules;
    sceneViewFromSerialization(serialization, &states, &rules);
    editDrawStates([states](SceneDrawStates* drawStates) { return drawStates->replaceSetStates(states); }, states.size());
    {
        std::lock_guard<std::mutex> lock(_imp->restoredLoadsMutex);
        _imp->restoredLoads = rules;
    }
    for (const SceneDrawLoadRequest& load : sceneLoadRequestsFor(rules)) {
        _imp->renderer.requestStageLoad(std::vector<std::string>(1, load.path), load.withDescendants, load.excluded);
    }
    wakeLoadsAskedBeforeStage();
    publishState();
}

void
SceneViewport::cancelPendingLoads()
{
    forgetRestoredLoads();
    _imp->renderer.cancelStageLoads();
    publishState();
    Q_EMIT stageLoadsChanged();
    noteDrawPolicySettled();
}

void
SceneViewport::setScene(const ScenePayload& payload,
                        double frame,
                        double framesPerSecond)
{
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
    // The same stack as asked for last most likely leaves the stage as it is, so a frame rendering it
    // goes on. It still goes through the worker, which syncs what the stage really holds and
    // passes on what the scene was evaluated for.
    const bool keepsFrame = !payload.isEmpty() && _imp->requestedError.isEmpty() && !_imp->requestedLiveTextures && payload.isSameStack(_imp->requestedPayload);
    const NodePtr viewed = _imp->viewedNodeProvider ? _imp->viewedNodeProvider() : NodePtr();
    const bool newNode = viewed != _imp->sceneNode.lock();
    _imp->sceneNode = viewed;
    requestSync(payload, QString(), keepsFrame);
    if (newNode && !payload.isEmpty()) {
        _imp->newNodeSerial = _imp->requestedSerial;
    }
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
    if ((frame != _imp->frame) && (_imp->handle == SceneGizmo::kNoHandle)) {
        _imp->previewAwaitsSync = false;
        _imp->previewTimer->stop();
        _imp->clearPreview();
    }
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
SceneViewport::setSceneEvaluatedFor(const SceneLoadSetPtr& loadSet,
                                    const SceneLoadReachPtr& reach)
{
    _imp->requestedLoadSet = loadSet;
    _imp->requestedReach = reach;
}

void
SceneViewport::setLoadSetEvaluator(const SceneViewportRenderer::LoadSetEvaluator& evaluate,
                                   const SceneViewportRenderer::LoadSetCommitted& committed)
{
    _imp->renderer.setLoadSetEvaluator(evaluate, committed);
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
    // A viewer showing images hides this, and a hidden view is not painted to let its refinement
    // engine go.
    _imp->renderer.releaseRefinement();
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
    setProperty("natronSceneComposing", isComposing());
    // The payload loads: whether requests wait, how many commits edited the stage, how long the last
    // batch held the viewer lock, and each failed prim's path and error.
    setProperty("natronSceneLoadsPending", _imp->renderer.hasPendingStageLoads());
    setProperty("natronSceneLoadEdits", QVariant::fromValue<qulonglong>(_imp->renderer.getStageLoadEditCount()));
    setProperty("natronSceneLoadHoldMs", _imp->renderer.getLastStageLoadHoldMs());
    publishDrawStats();
    {
        QVariantList failures;
        for (const std::pair<std::string, std::string>& failure : _imp->renderer.getStageLoadFailures()) {
            failures.push_back(QVariantList { QString::fromStdString(failure.first), QString::fromStdString(failure.second) });
        }
        setProperty("natronSceneLoadFailures", failures);
    }
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
    setProperty("natronSceneRenderer", QString::fromStdString(_imp->renderer.getRendererId()));
    // The delegate rendering: the chosen one, or for Scene Default the one the stage's prim names.
    setProperty("natronSceneActiveRenderer", QString::fromStdString(_imp->renderer.getActiveRendererId()));
    setProperty("natronSceneRendererError", QString::fromStdString(_imp->renderer.getRendererError()));
    // The display settings: the draw mode's and lighting mode's script names, and the proxy,
    // render and guide purposes shown.
    setProperty("natronSceneDrawMode", QString::fromUtf8(drawModeName(_imp->renderer.getDrawMode())));
    setProperty("natronSceneShowGrid", _imp->renderer.getShowGrid());
    setProperty("natronSceneShowGuides", _imp->renderer.getShowGuides());
    setProperty("natronSceneGuideSize", _imp->renderer.getGuideSize());
    setProperty("natronScenePurposes", QVariantList { _imp->renderer.getShowProxy(), _imp->renderer.getShowRender(), _imp->renderer.getShowGuidePurpose() });
    setProperty("natronSceneLighting", QString::fromUtf8(lightingModeName(_imp->renderer.getLightingMode())));
    // The transform widget's tool, frame and origin, by script name.
    setProperty("natronSceneTransformTool", QString::fromUtf8(transformToolName(_imp->renderer.getTransformTool())));
    setProperty("natronSceneTransformSpace", QString::fromUtf8((_imp->renderer.getTransformSpace() == SceneTransform::eSpaceLocal) ? "local" : "world"));
    setProperty("natronSceneTransformPivot", QString::fromUtf8((_imp->renderer.getTransformPivot() == SceneTransform::ePivotCurrentPrim) ? "currentPrim" : "selectionCentre"));
    publishRefineState();
}

void
SceneViewport::publishRefineState()
{
    const SceneViewportRenderer::RefineStatus status = _imp->renderer.getRefineStatus();

    setProperty("natronSceneRefining", status.active && !status.stopped && !status.converged);
    setProperty("natronSceneRefinementStopped", status.stopped);
    setProperty("natronSceneRefineConverged", status.converged);
    setProperty("natronSceneSamples", status.active ? status.samples : -1);
    setProperty("natronSceneDrawnSamples", status.active ? status.drawnSamples : -1);
    setProperty("natronSceneMaxSamples", _imp->renderer.getMaxSamples());
    setProperty("natronSceneRestartCount", QVariant::fromValue<qulonglong>(status.restarts));
    setProperty("natronSceneRefineFrames", QVariant::fromValue<qulonglong>(status.frames));
    setProperty("natronSceneUploadMs", _imp->renderer.getLastUploadMs());
    setProperty("natronSceneUploadPixels", _imp->renderer.getLastUploadPixels());
    Q_EMIT refineStateChanged();
}

// Keeps what it last published if an edit holds the stage; the paint that edit asks for publishes.
void
SceneViewport::publishTransformState()
{
    const qreal dpr = devicePixelRatio();
    std::vector<int> handles;
    std::vector<double> pixels, frame;
    bool busy = false;
    _imp->renderer.getDrawnTransformHandles(int(width() * dpr), int(height() * dpr), &handles, &pixels, &frame, &busy);
    if (busy) {
        return;
    }
    QVariantList drawn;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        drawn.push_back(QVariantList { handles[i], pixels[2 * i] / dpr, pixels[2 * i + 1] / dpr });
    }
    setProperty("natronSceneTransformHandles", drawn);
    QVariantList frameValues;
    for (double v : frame) {
        frameValues.push_back(v);
    }
    setProperty("natronSceneTransformFrame", frameValues);
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
    publishState();
    update();
}

void
SceneViewport::setShowGrid(bool show)
{
    _imp->renderer.setShowGrid(show);
    publishState();
    update();
}

void
SceneViewport::setShowGuides(bool show)
{
    _imp->renderer.setShowGuides(show);
    publishState();
    update();
}

void
SceneViewport::setShowPurposes(bool proxy,
                               bool render,
                               bool guide)
{
    _imp->renderer.setShowProxy(proxy);
    _imp->renderer.setShowRender(render);
    _imp->renderer.setShowGuidePurpose(guide);
    publishState();
    update();
}

void
SceneViewport::setLightingMode(SceneViewportRenderer::LightingModeEnum mode)
{
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();
    if (settings) {
        // Its settingChanged brings every other 3D view along.
        settings->setViewer3DLighting((Settings::Viewer3DLightingEnum)mode);
    }
    _imp->renderer.setLightingMode(mode);
    publishState();
    update();
}

void
SceneViewport::setRendererId(const QString& id)
{
    // Dropping the old engine needs its context.
    const std::string renderer = id.toStdString();
    if (!_imp->usesRenderThread) {
        ++_imp->rendererSwitchLatest;
        _imp->rendererSwitchRunning = false;
        runWithEngineContext([this, &renderer]() { _imp->renderer.setRendererId(renderer); });
        publishState();
        update();
        _imp->announcedRenderer = _imp->renderer.getActiveRendererId();
        Q_EMIT rendererChanged(id);

        return;
    }
    // The render thread may be seconds into a frame of the renderer being left: the frame ends
    // before its next pass, and the switch goes on the render thread with nobody waiting for it.
    _imp->renderer.cancelBackgroundFrame();
    const U64 serial = ++_imp->rendererSwitchLatest;
    _imp->rendererSwitchRunning = true;
    if (!_imp->composingTimer->isActive()) {
        _imp->composingClock.start();
        _imp->composingTimer->start();
    }
    Implementation* imp = _imp.get();
    _imp->postToRenderThread([this, imp, renderer, id, serial]() {
        if (imp->rendererSwitchLatest != serial) {
            return;
        }
        imp->renderer.setRendererId(renderer);
        // Dropped by Qt if the widget is gone by then.
        QMetaObject::invokeMethod(this, [this, id, serial]() { onRendererSwitched(id, serial); }, Qt::QueuedConnection);
    });
    publishState();
}

void
SceneViewport::onRendererSwitched(const QString& id,
                                  U64 serial)
{
    if (serial != _imp->rendererSwitchLatest) {
        return;
    }
    _imp->rendererSwitchRunning = false;
    if (!_imp->backgroundFrameRunning) {
        _imp->composingTimer->stop();
    }
    _imp->announcedRenderer = _imp->renderer.getActiveRendererId();
    Q_EMIT rendererChanged(id);
    if (_imp->followSceneOwed || _imp->renderer.hasPendingRendererSwitch()) {
        followSceneRenderer();
    }
    startBackgroundFrame();
    serviceDeferredInput();
    publishState();
    update();
}

void
SceneViewport::publishActiveRenderer()
{
    // Under Scene Default the delegate follows the stage's prim: the render row follows it.
    const std::string& active = _imp->renderer.getActiveRendererId();

    if (_imp->announcedRenderer != active) {
        _imp->announcedRenderer = active;
        setProperty("natronSceneActiveRenderer", QString::fromStdString(active));
        Q_EMIT rendererChanged(QString::fromStdString(_imp->renderer.getRendererId()));
    }
}

void
SceneViewport::setMaxSamples(int samples)
{
    _imp->renderer.setMaxSamples(samples);
    publishRefineState();
    update();
}

void
SceneViewport::setRefinementPaused(bool paused)
{
    _imp->renderer.setRefinementPaused(paused);
    publishRefineState();
    update();
}

void
SceneViewport::setDisplayTransform(const SceneViewportRenderer::DisplayTransform& transform)
{
    _imp->renderer.setDisplayTransform(transform);
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
    if (!path.isEmpty()) {
        {
            std::lock_guard<std::mutex> lock(_imp->queueMutex);
            _imp->pendingPivot = true;
        }
        _imp->queueChanged.notify_one();
    }
    endLockOutsideLookThrough();
    _imp->lockMessage.clear();
    publishState();
    update();
}

void
SceneViewport::requestFraming(const std::vector<std::string>& selection)
{
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->pendingFramingId = ++_imp->framingRequested;
        _imp->pendingFramingSelection = selection;
    }
    _imp->queueChanged.notify_one();
    publishState();
}

void
SceneViewport::onFramingComputed(U64 id,
                                 const SceneViewportRenderer::FrameBounds& bounds)
{
    serviceDeferredInput();
    // A later request was taken with this one or follows it, and its answer is on its way.
    if (id != _imp->framingRequested) {
        return;
    }
    _imp->framingDone = id;
    _imp->renderer.applyFrameBounds(bounds);
    endLockOutsideLookThrough();
    publishState();
    update();
}

void
SceneViewport::frameScene()
{
    requestFraming(std::vector<std::string>());
}

void
SceneViewport::frameSelection()
{
    requestFraming(_imp->renderer.getSelectedPrims());
}

bool
SceneViewport::pickwalk(PickwalkEnum direction)
{
    const QStringList selection = getSelection();
    if (selection.isEmpty() || !_imp->renderer.hasScene()) {
        return true;
    }
    QStringList walked;
    QHash<QString, QString> remembered = _imp->pickwalkChildren;
    const bool read = _imp->renderer.tryReadStage([&](const SceneStage* stage) {
        const UsdStageRefPtr usdStage = stage ? stage->getStage() : UsdStageRefPtr();
        if (!usdStage) {
            walked = selection;

            return;
        }
        const Usd_PrimFlagsPredicate predicate = childPredicate();
        struct SiblingList {
            std::vector<QString> paths;
            QHash<QString, std::size_t> index;
        };
        std::map<SdfPath, SiblingList> siblingLists;
        std::vector<QString> targets;
        targets.reserve(std::size_t(selection.size()));
        for (const QString& path : selection) {
            QString target = path;
            const std::string pathString = path.toStdString();
            const UsdPrim prim = SdfPath::IsValidPathString(pathString) ? usdStage->GetPrimAtPath(SdfPath(pathString)) : UsdPrim();
            if (prim) {
                const UsdPrim parent = prim.GetParent();
                switch (direction) {
                case ePickwalkParent:
                    if (parent && !parent.IsPseudoRoot()) {
                        target = QString::fromStdString(parent.GetPath().GetString());
                        remembered.insert(target, path);
                    }
                    break;
                case ePickwalkChild: {
                    const std::string last = remembered.value(path).toStdString();
                    UsdPrim child;
                    if (SdfPath::IsValidPathString(last)) {
                        const SdfPath lastPath(last);
                        if (lastPath.GetParentPath() == prim.GetPath()) {
                            child = usdStage->GetPrimAtPath(lastPath);
                        }
                    }
                    if (!child || !predicate(child)) {
                        const UsdPrimSiblingRange children = prim.GetFilteredChildren(predicate);
                        child = children.empty() ? UsdPrim() : *children.begin();
                    }
                    if (child) {
                        target = QString::fromStdString(child.GetPath().GetString());
                    }
                    break;
                }
                case ePickwalkPrevSibling:
                case ePickwalkNextSibling: {
                    const UsdPrim container = parent ? parent : usdStage->GetPseudoRoot();
                    auto found = siblingLists.find(container.GetPath());
                    if (found == siblingLists.end()) {
                        SiblingList list;
                        for (const UsdPrim& sibling : container.GetFilteredChildren(predicate)) {
                            list.index.insert(QString::fromStdString(sibling.GetPath().GetString()), list.paths.size());
                            list.paths.push_back(QString::fromStdString(sibling.GetPath().GetString()));
                        }
                        found = siblingLists.emplace(container.GetPath(), std::move(list)).first;
                    }
                    const SiblingList& siblings = found->second;
                    const auto it = siblings.index.constFind(path);
                    if (it != siblings.index.constEnd()) {
                        const std::ptrdiff_t count = std::ptrdiff_t(siblings.paths.size());
                        const std::ptrdiff_t step = (direction == ePickwalkNextSibling) ? 1 : -1;
                        target = siblings.paths[std::size_t((std::ptrdiff_t(it.value()) + step + count) % count)];
                    }
                    break;
                }
                }
            }
            targets.push_back(target);
        }
        // Prims that walk onto the same target keep its last place, so the prim walked from the
        // current one stays last and current.
        QSet<QString> placed;
        placed.reserve(int(targets.size()));
        std::vector<QString> ordered;
        ordered.reserve(targets.size());
        for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
            if (!placed.contains(*it)) {
                placed.insert(*it);
                ordered.push_back(*it);
            }
        }
        walked.reserve(int(ordered.size()));
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
            walked.push_back(*it);
        }
    });
    if (!read) {
        return false;
    }
    _imp->pickwalkChildren = remembered;
    select(walked, eSelectionModeReplace);

    return true;
}

bool
SceneViewport::pickMustWait()
{
    // The engine is busy for as long as a background frame runs, and a stage change not yet taken
    // in would have the pick do that frame's work on the render thread, with the main thread
    // waiting for it. Asking for the frame here brings the retry that follows it.
    startBackgroundFrame();

    return _imp->renderThreadBusy() || (_imp->usesRenderThread && _imp->renderer.needsBackgroundFrame());
}

QString
SceneViewport::pickPrimAt(const QPoint& pos,
                          bool* busy)
{
    std::string path;

    if (_imp->glInitialized && _imp->renderer.hasScene() && context()) {
        if (pickMustWait()) {
            if (busy) {
                *busy = true;
            }

            return QString();
        }
        const qreal dpr = devicePixelRatio();
        const int x = int(pos.x() * dpr);
        const int y = int(pos.y() * dpr);
        const int w = int(width() * dpr);
        const int h = int(height() * dpr);
        runWithEngineContext([&]() { path = _imp->renderer.pick(x, y, w, h, busy); });
    }

    return QString::fromStdString(path);
}

QStringList
SceneViewport::pickPrimsIn(const QRect& rect,
                           bool* busy)
{
    std::vector<std::string> paths;

    if (_imp->glInitialized && _imp->renderer.hasScene() && context()) {
        if (pickMustWait()) {
            if (busy) {
                *busy = true;
            }

            return QStringList();
        }
        const qreal dpr = devicePixelRatio();
        const QRect r = rect.normalized();
        const int left = int(r.left() * dpr);
        const int top = int(r.top() * dpr);
        const int right = int((r.right() + 1) * dpr) - 1;
        const int bottom = int((r.bottom() + 1) * dpr) - 1;
        const int w = int(width() * dpr);
        const int h = int(height() * dpr);
        runWithEngineContext([&]() { paths = _imp->renderer.pickRect(left, top, right, bottom, w, h, busy); });
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
SceneViewport::selectPaths(const QStringList& paths)
{
    select(paths, eSelectionModeReplace);
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
    _imp->renderer.setSelectedPrims(p);
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
SceneViewport::setTransformTarget(const std::shared_ptr<SceneTransformTarget>& target)
{
    _imp->transformTarget = target;
}

void
SceneViewport::setTransformTool(int tool)
{
    if (_imp->handle != SceneGizmo::kNoHandle) {
        return;
    }
    _imp->renderer.setTransformTool(tool);
    _imp->renderer.setHighlightedTransformHandle(SceneGizmo::kNoHandle);
    publishState();
    update();
}

int
SceneViewport::getTransformTool() const
{
    return _imp->renderer.getTransformTool();
}

void
SceneViewport::cycleTransformTool()
{
    const int tool = _imp->renderer.getTransformTool();

    setTransformTool(((tool == SceneTransform::eToolSelect) || (tool == SceneTransform::eToolScale)) ? SceneTransform::eToolTranslate : tool + 1);
}

void
SceneViewport::setTransformSpace(int space)
{
    if (_imp->handle != SceneGizmo::kNoHandle) {
        return;
    }
    _imp->renderer.setTransformSpace(space);
    publishState();
    update();
}

void
SceneViewport::setTransformPivot(int pivot)
{
    if (_imp->handle != SceneGizmo::kNoHandle) {
        return;
    }
    _imp->renderer.setTransformPivot(pivot);
    publishState();
    update();
}

void
SceneViewport::setViewedNodeProvider(const std::function<NodePtr()>& provider)
{
    _imp->viewedNodeProvider = provider;
}

void
SceneViewport::setSelectionActions(const SelectionActions& actions)
{
    _imp->selectionActions = actions;
}

void
SceneViewport::setArmedPick(const QString& label)
{
    _imp->armedPick = label;
    setProperty("natronSceneArmedPick", label);
    update();
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
    endLockOutsideLookThrough();
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

void
SceneViewport::endLockOutsideLookThrough()
{
    if (!_imp->renderer.getLookThroughCamera().empty()) {
        return;
    }
    if (_imp->cameraLock.active) {
        endCameraLock();
    }
    if (_imp->lockView) {
        _imp->lockView = false;
        _imp->lockMessage.clear();
        publishState();
    }
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
    bool poseBusy = false;
    const bool havePose = _imp->renderer.getViewPose(int(width() * dpr), int(height() * dpr), &pose, &poseBusy);
    // Not refused: the next move of the drag or the wheel tries again.
    if (poseBusy) {
        return false;
    }
    const double frame = _imp->frame;
    GfVec3d s(1.);
    if (scale && uniformScale) {
        const double uniform = uniformScale->getValueAtTime(frame, 0);
        s = GfVec3d(scale->getValueAtTime(frame, 0) * uniform, scale->getValueAtTime(frame, 1) * uniform, scale->getValueAtTime(frame, 2) * uniform);
    }
    const bool scaled = (std::abs(s[0]) > 1e-12) && (std::abs(s[1]) > 1e-12) && (std::abs(s[2]) > 1e-12);
    if (!translate || !rotate || !rotateOrder || !havePose || !scaled) {
        // The view stays locked to the camera and does not move for a camera read from a
        // file.
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
    bool poseBusy = false;

    if (!app || !_imp->renderer.getViewPose(int(width() * dpr), int(height() * dpr), &pose, &poseBusy)) {
        _imp->cameraFromViewOwed = poseBusy;

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
    KnobDoublePtr scale = knobs.scale.lock();
    KnobDoublePtr uniformScale = knobs.uniformScale.lock();
    KnobChoicePtr rotateOrder = knobs.rotateOrder.lock();
    KnobDoublePtr pivot = effect->getPivotKnob();
    if (!translate || !rotate || !scale || !uniformScale || !rotateOrder || !pivot) {
        _imp->renderer.clearTransformHandles();

        return;
    }
    SceneViewportRenderer::TransformHandles handles;
    handles.primPath = primPath;
    handles.opName = opName;
    for (int d = 0; d < 3; ++d) {
        handles.translate[d] = translate->getValueAtTime(_imp->frame, d);
        handles.rotate[d] = rotate->getValueAtTime(_imp->frame, d);
        handles.scale[d] = scale->getValueAtTime(_imp->frame, d);
        handles.pivot[d] = pivot->getValueAtTime(_imp->frame, d);
    }
    handles.uniformScale = uniformScale->getValueAtTime(_imp->frame, 0);
    handles.rotateOrder = rotateOrder->getValue();
    std::vector<std::string> paths, opNames;
    effect->getMatchedPrims(&paths, &opNames);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        SceneViewportRenderer::TransformHandles::Target target;
        target.primPath = paths[i];
        target.opName = opNames[i];
        handles.otherTargets.push_back(target);
    }
    _imp->renderer.setTransformHandles(handles);
} // SceneViewport::refreshTransformHandles

void
SceneViewport::releaseEngine()
{
    // The engine's GL objects belong to its context, and releaseGL() takes the viewer lock, so the
    // worker is not syncing into the stage the engine listens to while it goes.
    if (_imp->glInitialized && context()) {
        // A background frame would keep the main thread waiting for the rest of its passes.
        _imp->renderer.cancelBackgroundFrame();
        runWithEngineContext([this]() { _imp->renderer.releaseGL(); });
    }
    _imp->renderer.releaseRefinement();
    if (_imp->requestedLiveTextures) {
        // Free GPU Memory emptied the store too, and only the worker's sync renders textures.
        requestSync(_imp->requestedPayload, _imp->requestedError);
        _imp->paintHeldUntilSerial = _imp->requestedSerial;
    } else {
        publishState();
    }
    // A hidden view makes its engine when it is next shown.
    update();
}

void
SceneViewport::initializeGL()
{
    _imp->glInitialized = true;
    _imp->startRenderContext(context());
    // A new context (the pane was floated or docked) cannot use the old one's engine, and the engine
    // context shared with the old one has to go with it.
    GLContextLoss::watch(this, [this]() {
        _imp->releaseGLResources();
    });
}

void
SceneViewport::resizeGL(int /*w*/,
                        int /*h*/)
{
    // Qt gave the widget a new framebuffer, with nothing of the old image in it.
    _imp->presentedScene = false;
}

void
SceneViewport::paintGL()
{
    if (_imp->syncedSerial < _imp->paintHeldUntilSerial) {
        // onSynced asks for the repaint.
        return;
    }
    const qreal dpr = devicePixelRatio();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    if (_imp->usesRenderThread && (w > 0) && (h > 0)) {
        startBackgroundFrame();
        if (_imp->renderThreadBusy() || _imp->renderer.needsBackgroundFrame()) {
            if (!_imp->renderThreadBusy() && !_imp->stageAwaitsTakeIn()) {
                // A sync or a load holds the viewer lock and asks for no paint if it edits nothing.
                update();
            }
            if (!_imp->presentedScene || (_imp->composingClock.isValid() && (_imp->composingClock.elapsed() >= kComposingStatusDelayMs))) {
                paintComposingStatus();
            }

            return;
        }
    }
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
    _imp->renderer.setRecordDrawnGuides(property("natronSceneRecordGuides").toBool());
    _imp->renderer.setRecordDrawnBoxes(property("natronSceneRecordBoxes").toBool());
    SceneViewportRenderer::RenderStatusEnum status = SceneViewportRenderer::eRenderStatusBusy;
    if (_imp->usesRenderThread && (_imp->renderer.getActiveRendererId() != kRenderDelegateStormId)) {
        // The delegate's frames are made on the refinement's thread, and drawing one needs no
        // engine: waiting on the render thread for it would hold the event loop for two thread
        // wake-ups on processors the delegate keeps busy, at every frame it refines.
        status = _imp->renderer.renderRefinementIfIdle(defaultFramebufferObject(), w, h);
    } else if (_imp->usesRenderThread) {
        GLuint target = 0;
        GLsync fence = nullptr;
        Implementation* imp = _imp.get();
        imp->callOnRenderThread([imp, w, h, &status, &target, &fence]() {
            target = imp->ensureRenderTarget(w, h);
            status = imp->renderer.renderIfIdle(target, w, h);
            if (status != SceneViewportRenderer::eRenderStatusBusy) {
                fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                // A fence another context waits on has to reach the GPU first.
                glFlush();
            }
        });
        if (status != SceneViewportRenderer::eRenderStatusBusy) {
            imp->presentRenderTarget(defaultFramebufferObject(), w, h, fence);
        }
    } else {
        status = _imp->renderer.renderIfIdle(defaultFramebufferObject(), w, h);
    }
    if (status == SceneViewportRenderer::eRenderStatusBusy) {
        // The worker is syncing; it asks for a repaint when it is done. The refinement passing
        // holds the lock too, and asks for one only once it has a frame: this paint's camera or
        // size must still reach it.
        if (!_imp->presentedScene && isComposing()) {
            paintComposingStatus();
        }
        if (_imp->renderer.getActiveRendererId() != kRenderDelegateStormId) {
            QTimer::singleShot(10, this, [this]() { update(); });
        }

        return;
    }
    _imp->presentedScene = _imp->renderer.hasScene() && !_imp->renderer.isStageHeld();
    if (_imp->renderer.isDrawRevealing()) {
        _imp->renderer.noteDrawRevealPresented();
        const int yieldMs = _imp->renderer.getRevealYieldMs();
        if (yieldMs > 0) {
            // The camera's own moves ask for the paints meanwhile.
            QTimer::singleShot(yieldMs, this, [this]() { update(); });
        } else {
            update();
        }
    }
    if (_imp->renderer.hasPendingRendererSwitch()) {
        // A sync landed between the last one followed and this paint.
        QTimer::singleShot(0, this, [this]() { followSceneRenderer(); });
    }
    const bool converged = (status == SceneViewportRenderer::eRenderStatusDone);
    const SceneViewportRenderer::RefineStatus refine = _imp->renderer.getRefineStatus();
    publishRefineState();
    publishTransformState();
    publishDrawStats();
    if (SceneDrawTiming* timing = _imp->renderer.drawTiming()) {
        if (timing->presented(_imp->renderer.getDrawFilterStats().absorbed, std::chrono::steady_clock::now())) {
            setProperty("natronSceneDrawTimings", timing->records());
        }
    }
    // A paint gets here only once the frame has taken in every delta posted so far, unlike the stats,
    // which also change on the way to the screen.
    setProperty("natronSceneDrawPresented", QVariant::fromValue<qulonglong>(_imp->renderer.getDrawFilterStats().absorbed));
    // The engine is made at the first render in a context.
    setProperty("natronSceneEngineCount", QVariant::fromValue<qulonglong>(_imp->renderer.getEngineCount()));
    setProperty("natronSceneEngineHeld", _imp->renderer.hasEngine());
    const QString renderError = QString::fromStdString(_imp->renderer.takeRenderError());
    if (!renderError.isEmpty()) {
        _imp->error = renderError;
        publishState();
    }
    const QString rendererError = QString::fromStdString(_imp->renderer.getRendererError());
    setProperty("natronSceneRendererError", rendererError);
    if (_imp->announcedRenderer != _imp->renderer.getActiveRendererId()) {
        // The render row shown or hidden resizes this widget, which inside its paint frees the
        // texture Qt then composes the window from.
        QTimer::singleShot(0, this, [this]() { publishActiveRenderer(); });
    }
    // Whether the stage's own lights lit this paint, and the guides it drew: path, kind and a
    // point on each in widget pixels.
    setProperty("natronSceneSceneLit", _imp->renderer.isSceneLit());
    {
        QVariantList guides;
        for (const SceneViewportRenderer::DrawnGuide& guide : _imp->renderer.getDrawnGuides()) {
            guides.push_back(QVariantList { QString::fromStdString(guide.path), QString::fromStdString(guide.kind), guide.x / dpr, guide.y / dpr });
        }
        setProperty("natronSceneGuides", guides);
        QVariantList drawnBoxes;
        for (const SceneViewportRenderer::DrawnBox& box : _imp->renderer.getDrawnBoxes()) {
            drawnBoxes.push_back(QVariantList { QString::fromStdString(box.path), QString::fromStdString(box.kind), box.highlighted,
                                                box.x / dpr, box.y / dpr, box.left / dpr, box.top / dpr, box.right / dpr, box.bottom / dpr });
        }
        setProperty("natronSceneBoxes", drawnBoxes);
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (!_imp->error.isEmpty() || !rendererError.isEmpty()) {
        painter.setPen(QColor(230, 90, 80));
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, _imp->error.isEmpty() ? rendererError : _imp->error);
    } else if (isComposing()) {
        painter.end();
        paintComposingStatus();
        painter.begin(this);
    } else if (_imp->renderer.hasScene()) {
        QString mode;
        for (const DrawModeEntry& entry : kDrawModes) {
            if (entry.mode == _imp->renderer.getDrawMode()) {
                mode = shortcutText(entry.label);
            }
        }
        const std::string& camera = _imp->renderer.getLookThroughCamera();
        QString view = tr("Free camera");
        if (!camera.empty()) {
            view = QString::fromStdString(camera);
        } else if (_imp->renderer.getViewMode() != SceneViewportRenderer::eViewModePerspective) {
            for (const ViewModeEntry& entry : kViewModes) {
                if (entry.mode == _imp->renderer.getViewMode()) {
                    view = shortcutText(entry.label);
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
        if (refine.active) {
            // The samples of the frame shown, against the cap.
            const QString drawn = (refine.drawnSamples >= 0) ? QString::number(refine.drawnSamples) : QString::fromUtf8("-");
            if (refine.maxSamples > 0) {
                hud += tr(" | %1 / %2 samples").arg(drawn).arg(refine.maxSamples);
            } else {
                hud += tr(" | %1 samples").arg(drawn);
            }
            if (refine.stopped) {
                hud += tr(" (paused)");
            }
        }
        if (property("natronSceneHud").toString() != hud) {
            setProperty("natronSceneHud", hud);
        }
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignTop, hud);
        if (!_imp->lockMessage.isEmpty()) {
            painter.setPen(QColor(230, 160, 80));
            painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignBottom, _imp->lockMessage);
        }
    }
    if (!_imp->armedPick.isEmpty()) {
        painter.setPen(QColor(240, 170, 80));
        painter.drawText(rect().adjusted(8, 6, -8, -28), Qt::AlignHCenter | Qt::AlignBottom, tr("Pick for %1: select prims, Enter writes them, Esc cancels").arg(_imp->armedPick));
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
    } else if (status == SceneViewportRenderer::eRenderStatusRefining) {
        update();
    }
    // eRenderStatusWaiting: the background render's listener asks for the next paint.
} // paintGL

void
SceneViewport::mousePressEvent(QMouseEvent* e)
{
    // A handle drag is still to be written, and this press would start over it.
    if (_imp->handleEndOwed) {
        e->accept();

        return;
    }
    // Clicks still waiting go first, whichever way this one turns out.
    serviceDeferredInput();
    const bool queued = !_imp->deferredSelections.empty();
    bool handleOwed = false;
    if ((e->button() == Qt::LeftButton) && (e->modifiers() == Qt::NoModifier) && !_imp->spaceHeld) {
        if (queued) {
            handleOwed = true;
        } else if (beginHandleDrag(e->position().toPoint(), &handleOwed)) {
            e->accept();

            return;
        }
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
    if ((role == Implementation::eDragRoleNone) && !clickSelects && !handleOwed) {
        // A right press with no binding opens the context menu.
        QOpenGLWidget::mousePressEvent(e);

        return;
    }
    const QPoint pos = e->position().toPoint();
    // A marquee may start on a prim: a drag selects by rectangle wherever it starts,
    // and a release without a drag selects what is under the press.
    bool pickOwed = false;
    if (clickSelects) {
        if (queued) {
            pickOwed = true;
        } else {
            _imp->pressPrim = pickPrimAt(pos, &pickOwed);
        }
    }
    _imp->pressOwedHandle = handleOwed;
    _imp->pressOwedPick = pickOwed;
    _imp->pressView = _imp->currentView(this);
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
        if (!_imp->handleEndOwed) {
            dragHandle(pos);
        }
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
    if (navigates) {
        endLockOutsideLookThrough();
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
    Implementation::DeferredSelection deferred;
    deferred.pos = _imp->pressPos;
    deferred.rect = marquee;
    deferred.checkHandle = _imp->pressOwedHandle;
    deferred.mode = mode;
    deferred.view = _imp->pressView;
    const bool owed = _imp->pressOwedPick || _imp->pressOwedHandle;
    clearGesture();

    if (dragged && (role == Implementation::eDragRoleMarquee)) {
        bool busy = !_imp->deferredSelections.empty();
        QStringList paths;
        if (!busy) {
            paths = pickPrimsIn(marquee, &busy);
        }
        if (busy) {
            deferred.marquee = true;
            deferred.checkHandle = false;
            _imp->deferredSelections.push_back(deferred);
            serviceDeferredInput();
        } else {
            select(paths, mode);
        }
    } else if ((!dragged || (role == Implementation::eDragRoleNone)) && clickSelects && owed) {
        deferred.emptySelects = !dragged;
        _imp->deferredSelections.push_back(deferred);
        serviceDeferredInput();
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
    _imp->pressOwedHandle = false;
    _imp->pressOwedPick = false;
    if (marquee) {
        update();
    }
}

bool
SceneViewport::beginHandleDrag(const QPoint& pos,
                               bool* busy)
{
    refreshTransformHandles();
    const qreal dpr = devicePixelRatio();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    const int handle = _imp->renderer.hitTestTransformHandles(pixel[0], pixel[1], w, h, busy);
    if (handle == SceneGizmo::kNoHandle) {
        return false;
    }
    bool selectionBusy = false;
    const bool selection = _imp->renderer.hasSelectionHandles(&selectionBusy);
    if (selectionBusy) {
        if (busy) {
            *busy = true;
        }

        return false;
    }
    NodePtr node;
    if (!selection && !_imp->getTransformNode(&node)) {
        return false;
    }
    if (!_imp->renderer.beginTransformDrag(handle, pixel[0], pixel[1], w, h, busy)) {
        return false;
    }
    _imp->previewClearOwed = false;
    _imp->handleMoveOwed = false;
    _imp->handleChangeStale = false;
    _imp->handleEndOwed = false;
    _imp->previewTimer->stop();
    _imp->previewAwaitsSync = false;
    _imp->handle = handle;
    _imp->handleNode = node;
    _imp->handleChange = 0.;
    _imp->handleChanged = false;
    update();

    return true;
} // SceneViewport::beginHandleDrag

void
SceneViewport::dragHandle(const QPoint& pos)
{
    const qreal dpr = devicePixelRatio();
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    double change = 0.;
    bool busy = false;

    _imp->handleMoveOwed = false;
    _imp->handleChangeStale = false;
    if ((_imp->handle == SceneGizmo::kNoHandle) || !_imp->renderer.getTransformDragChange(pixel[0], pixel[1], int(width() * dpr), int(height() * dpr), &change, &busy)) {
        if (busy) {
            _imp->handleMoveOwed = true;
            _imp->handleChangeStale = true;
            _imp->handleMovePos = pos;
        }

        return;
    }
    // Only drawn: writing the nodes on every move would evaluate the graph and sync the scene
    // each time, and the prims would only move as fast as that round trip.
    _imp->handleChange = change;
    _imp->handleChanged = true;
    _imp->renderer.previewTransformDrag(change, &busy);
    if (busy) {
        _imp->handleMoveOwed = true;
        _imp->handleMovePos = pos;
    }
    update();
}

void
SceneViewport::endHandleDrag()
{
    if (_imp->handle == SceneGizmo::kNoHandle) {
        return;
    }
    if (_imp->handleChangeStale) {
        dragHandle(_imp->handleMovePos);
        // The drag can't be written until its last move is measured; it ends when that can be.
        if (_imp->handleChangeStale) {
            _imp->handleEndOwed = true;

            return;
        }
    }
    _imp->handleMoveOwed = false;
    _imp->handleEndOwed = false;
    const double change = _imp->handleChange;
    const bool changed = _imp->handleChanged;
    // Before any write, which may request the scene that has it before returning.
    const U64 serial = _imp->requestedSerial;
    bool written = false;

    if (changed) {
        if (_imp->renderer.isSelectionTransformDrag()) {
            SceneTransformDrag drag;
            if (_imp->renderer.getTransformDrag(change, _imp->frame, &drag)) {
                _imp->transformRecorder.commit(drag);
                QVariantList delta;
                for (int i = 0; i < 16; ++i) {
                    delta.push_back(drag.delta[i]);
                }
                QStringList paths;
                for (const SceneTransformPrim& prim : drag.prims) {
                    paths.push_back(QString::fromStdString(prim.path));
                }
                setProperty("natronSceneTransformDelta", QVariantList { QString::fromUtf8(transformToolName(drag.tool)), paths, delta });
                setProperty("natronSceneTransformDrags", _imp->transformRecorder.getCount());
                if (_imp->transformTarget && !property("natronSceneTransformRecordOnly").toBool()) {
                    written = _imp->transformTarget->commit(drag);
                }
            }
        } else {
            written = commitOpHandleDrag(change);
        }
    }
    _imp->renderer.endTransformDrag();
    _imp->handle = SceneGizmo::kNoHandle;
    _imp->handleNode.reset();
    _imp->handleChange = 0.;
    _imp->handleChanged = false;
    if (written && (_imp->syncedSerial <= serial)) {
        // The preview stays until the scene with the written drag is shown.
        _imp->renderer.clearTransformPreviewAfter(serial);
        _imp->previewAwaitsSync = true;
        _imp->previewSerial = serial;
        _imp->previewTimer->start();
    } else {
        _imp->clearPreview();
    }
    update();
} // SceneViewport::endHandleDrag

bool
SceneViewport::commitOpHandleDrag(double change)
{
    NodePtr node = _imp->handleNode.lock();
    USDEditTransform* effect = node ? dynamic_cast<USDEditTransform*>(node->getEffectInstance().get()) : nullptr;

    if (!effect) {
        return false;
    }
    const SceneTransformKnobs& knobs = effect->getTransformKnobs();
    KnobDoublePtr knob;
    switch (SceneGizmo::getHandleKind(_imp->handle)) {
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
    if (!knob) {
        return false;
    }
    const int axis = SceneGizmo::getHandleAxis(_imp->handle);
    const double old = knob->getValueAtTime(_imp->frame, axis);
    const double value = (SceneGizmo::getHandleKind(_imp->handle) == SceneGizmo::eHandleKindScale) ? old * change : old + change;
    if (value == old) {
        return false;
    }
    applySceneKnobEdits(node, _imp->frame, { makeSceneKnobEdit(knob, axis, _imp->frame, value) }, _imp->undoHistory.data(),
                        tr("Move %1").arg(QString::fromUtf8(node->getLabel().c_str())));

    return true;
} // SceneViewport::commitOpHandleDrag

void
SceneViewport::highlightHandleAt(const QPoint& pos)
{
    const qreal dpr = devicePixelRatio();
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    bool busy = false;
    const int handle = _imp->renderer.hitTestTransformHandles(pixel[0], pixel[1], int(width() * dpr), int(height() * dpr), &busy);

    // The highlight stays as it is: it can't tell what is under the pointer.
    if (!busy && (handle != _imp->renderer.getHighlightedTransformHandle())) {
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
    endLockOutsideLookThrough();
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
            || (drawModeForKey(k->modifiers(), k->key()) >= 0) || matchesKeyBinding(kShortcutIDActionViewer3DShowGrid, k->modifiers(), k->key())
            || matchesKeyBinding(kShortcutIDActionViewer3DShowGuides, k->modifiers(), k->key()) || isTransformKey(k->modifiers(), k->key())
            || matchesKeyBinding(kShortcutIDActionViewer3DSetSelectionOnNode, k->modifiers(), k->key())
            || matchesKeyBinding(kShortcutIDActionViewer3DCreateFromSelection, k->modifiers(), k->key()) || drawKeyFor(k->modifiers(), k->key())
            || (pickwalkForKey(k->modifiers(), k->key()) >= 0)
            || ((k->key() == Qt::Key_Escape) && (_imp->handle != SceneGizmo::kNoHandle)) || ((k->key() == Qt::Key_Space) && bindingsUseSpace())) {
            e->accept();

            return true;
        }
    }
    // QWidget::event() spends Tab on moving the focus before keyPressEvent() sees it.
    if (e->type() == QEvent::KeyPress) {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        if (matchesKeyBinding(kShortcutIDActionViewer3DCreateFromSelection, k->modifiers(), k->key())) {
            keyPressEvent(k);

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
    if ((e->key() == Qt::Key_Escape) && (_imp->handle != SceneGizmo::kNoHandle)) {
        // Drops the drag under way: nothing is written.
        _imp->handleChanged = false;
        endHandleDrag();
        clearGesture();
        e->accept();

        return;
    }
    const int tool = transformToolForKey(e->modifiers(), e->key());
    if (tool >= 0) {
        setTransformTool(tool);
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DCycleTool, e->modifiers(), e->key())) {
        cycleTransformTool();
        e->accept();

        return;
    }
    if (const DrawKey* drawKey = drawKeyFor(e->modifiers(), e->key())) {
        if (_imp->drawStates) {
            const QStringList selection = getSelection();
            const bool toParent = drawKey->toParent;
            const SceneDrawStateEnum state = drawKey->state;
            // The rows are found as the edit runs, so the key never waits on an edit under way and
            // acts on the states the edits before it left.
            editDrawStates([selection, toParent, state](SceneDrawStates* states) {
                std::vector<SceneDrawRowEdit> edits;
                for (const std::string& row : drawKeyRows(*states, selection, toParent)) {
                    edits.push_back(SceneDrawRowEdit { SceneDrawRowEdit::eKindSet, row, state });
                }

                return states->editRows(edits);
            });
        }
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DSetSelectionOnNode, e->modifiers(), e->key())) {
        if (_imp->selectionActions.setOnNode) {
            _imp->selectionActions.setOnNode(getSelection());
        }
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DCreateFromSelection, e->modifiers(), e->key())) {
        if (_imp->selectionActions.createFromSelection) {
            _imp->selectionActions.createFromSelection(getSelection());
        }
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DToggleSpace, e->modifiers(), e->key())) {
        setTransformSpace((_imp->renderer.getTransformSpace() == SceneTransform::eSpaceLocal) ? SceneTransform::eSpaceWorld : SceneTransform::eSpaceLocal);
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DTogglePivot, e->modifiers(), e->key())) {
        setTransformPivot((_imp->renderer.getTransformPivot() == SceneTransform::ePivotCurrentPrim) ? SceneTransform::ePivotSelectionCentre : SceneTransform::ePivotCurrentPrim);
        e->accept();

        return;
    }
    const int pickwalkStep = pickwalkForKey(e->modifiers(), e->key());
    if (pickwalkStep >= 0) {
        // A busy stage drops the press: the key repeats, and the user sees nothing to wait for.
        pickwalk(PickwalkEnum(pickwalkStep));
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
    const int drawMode = drawModeForKey(e->modifiers(), e->key());
    if (drawMode >= 0) {
        setDrawMode((SceneViewportRenderer::DrawModeEnum)drawMode);
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DShowGrid, e->modifiers(), e->key())) {
        setShowGrid(!_imp->renderer.getShowGrid());
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DShowGuides, e->modifiers(), e->key())) {
        setShowGuides(!_imp->renderer.getShowGuides());
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
    for (const DrawModeEntry& item : kDrawModes) {
        QAction* action = menu.addAction(shortcutText(item.label));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getDrawMode() == item.mode);
        action->setActionGroup(modes);
        action->setShortcut(keySequenceOf(item.actionID));
        const SceneViewportRenderer::DrawModeEnum mode = item.mode;
        QObject::connect(action, &QAction::triggered, this, [this, mode]() {
            setDrawMode(mode);
        });
    }

    QMenu* renderers = menu.addMenu(tr("Renderer"));
    QActionGroup* rendererGroup = new QActionGroup(renderers);
    const std::string& rendererId = _imp->renderer.getRendererId();
    {
        QAction* action = renderers->addAction(tr("Scene Default"));
        action->setCheckable(true);
        action->setChecked(rendererId == kRenderDelegateSceneDefaultId);
        action->setActionGroup(rendererGroup);
        action->setToolTip(tr("Renders with the renderer the scene's active render settings prim names, "
                              "Storm without one, and with that prim's settings for it."));
        QObject::connect(action, &QAction::triggered, this, [this]() {
            setRendererId(QString::fromUtf8(kRenderDelegateSceneDefaultId));
        });
        renderers->addSeparator();
    }
    for (const RenderDelegates::Delegate& delegate : RenderDelegates::list()) {
        QString label = QString::fromStdString(delegate.displayName);
        if (!delegate.available) {
            label = tr("%1 (unavailable)").arg(label);
        }
        QAction* action = renderers->addAction(label);
        action->setCheckable(true);
        action->setChecked(rendererId == delegate.id);
        action->setActionGroup(rendererGroup);
        if (!delegate.available) {
            action->setToolTip(QString::fromStdString(delegate.error));
        }
        const QString id = QString::fromStdString(delegate.id);
        QObject::connect(action, &QAction::triggered, this, [this, id]() {
            setRendererId(id);
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
        QAction* action = views->addAction(shortcutText(entry.label));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getLookThroughCamera().empty() && (_imp->renderer.getViewMode() == entry.mode));
        action->setActionGroup(viewModes);
        action->setShortcut(keySequenceOf(entry.actionID));
        const SceneViewportRenderer::ViewModeEnum mode = entry.mode;
        QObject::connect(action, &QAction::triggered, this, [this, mode]() {
            setViewMode(mode);
        });
    }

    QAction* lockView = menu.addAction(shortcutText(kShortcutDescActionViewer3DLockView));
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

    menu.addSeparator();
    {
        const QStringList selection = getSelection();
        QAction* setOnNode = menu.addAction(tr(kShortcutDescActionViewer3DSetSelectionOnNode));
        setOnNode->setObjectName(QString::fromUtf8("sceneSetSelectionOnNode"));
        setOnNode->setShortcut(keySequenceOf(kShortcutIDActionViewer3DSetSelectionOnNode));
        const QString blocker = _imp->selectionActions.setOnNodeBlocker ? _imp->selectionActions.setOnNodeBlocker(selection) : QString();
        setOnNode->setEnabled(bool(_imp->selectionActions.setOnNode) && blocker.isEmpty());
        setOnNode->setToolTip(blocker);
        setOnNode->setStatusTip(blocker);
        QObject::connect(setOnNode, &QAction::triggered, this, [this, selection]() {
            if (_imp->selectionActions.setOnNode) {
                _imp->selectionActions.setOnNode(selection);
            }
        });
        QAction* create = menu.addAction(tr(kShortcutDescActionViewer3DCreateFromSelection));
        create->setObjectName(QString::fromUtf8("sceneCreateFromSelection"));
        create->setShortcut(keySequenceOf(kShortcutIDActionViewer3DCreateFromSelection));
        create->setEnabled(bool(_imp->selectionActions.createFromSelection) && _imp->renderer.hasScene());
        QObject::connect(create, &QAction::triggered, this, [this, selection]() {
            if (_imp->selectionActions.createFromSelection) {
                _imp->selectionActions.createFromSelection(selection);
            }
        });
    }

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

    // The transform widget's tool, frame and origin.
    QMenu* transform = menu.addMenu(tr("Transform"));
    QActionGroup* tools = new QActionGroup(transform);
    for (const TransformToolEntry& entry : kTransformTools) {
        QAction* action = transform->addAction(tr(entry.label));
        action->setObjectName(QString::fromUtf8(entry.name));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getTransformTool() == entry.tool);
        action->setShortcut(keySequenceOf(entry.actionID));
        action->setActionGroup(tools);
        const int tool = entry.tool;
        QObject::connect(action, &QAction::triggered, this, [this, tool]() {
            setTransformTool(tool);
        });
    }
    transform->addSeparator();
    QActionGroup* spaces = new QActionGroup(transform);
    const struct
    {
        SceneTransform::SpaceEnum space;
        const char* name;
        QString label;
    } spaceItems[] = {
        { SceneTransform::eSpaceWorld, "world", tr("World Space") },
        { SceneTransform::eSpaceLocal, "local", tr("Local Space") },
    };
    for (const auto& item : spaceItems) {
        QAction* action = transform->addAction(item.label);
        action->setObjectName(QString::fromUtf8(item.name));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getTransformSpace() == item.space);
        action->setActionGroup(spaces);
        const int space = item.space;
        QObject::connect(action, &QAction::triggered, this, [this, space]() {
            setTransformSpace(space);
        });
    }
    transform->addSeparator();
    QActionGroup* pivots = new QActionGroup(transform);
    const struct
    {
        SceneTransform::PivotEnum pivot;
        const char* name;
        QString label;
    } pivotItems[] = {
        { SceneTransform::ePivotSelectionCentre, "selectionCentre", tr("Selection Centre Pivot") },
        { SceneTransform::ePivotCurrentPrim, "currentPrim", tr("Current Prim Pivot") },
    };
    for (const auto& item : pivotItems) {
        QAction* action = transform->addAction(item.label);
        action->setObjectName(QString::fromUtf8(item.name));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getTransformPivot() == item.pivot);
        action->setActionGroup(pivots);
        const int pivot = item.pivot;
        QObject::connect(action, &QAction::triggered, this, [this, pivot]() {
            setTransformPivot(pivot);
        });
    }

    // The purposes and lights Storm draws with, and the overlays drawn over it.
    QMenu* display = menu.addMenu(tr("Display"));
    QMenu* purposes = display->addMenu(tr("Purposes"));
    const struct
    {
        const char* name;
        QString label;
        bool shown;
    } purposeItems[] = {
        { "proxy", tr("Proxy"), _imp->renderer.getShowProxy() },
        { "render", tr("Render"), _imp->renderer.getShowRender() },
        { "guide", tr("Guide"), _imp->renderer.getShowGuidePurpose() },
    };
    QAction* purposeActions[3] = { nullptr, nullptr, nullptr };
    for (int i = 0; i < 3; ++i) {
        QAction* action = purposes->addAction(purposeItems[i].label);
        action->setObjectName(QString::fromUtf8(purposeItems[i].name));
        action->setCheckable(true);
        action->setChecked(purposeItems[i].shown);
        purposeActions[i] = action;
    }
    for (QAction* action : purposeActions) {
        QObject::connect(action, &QAction::toggled, this, [this, purposeActions]() {
            setShowPurposes(purposeActions[0]->isChecked(), purposeActions[1]->isChecked(), purposeActions[2]->isChecked());
        });
    }
    QMenu* lighting = display->addMenu(tr("Lighting"));
    QActionGroup* lightingModes = new QActionGroup(lighting);
    for (const LightingModeEntry& entry : kLightingModes) {
        QAction* action = lighting->addAction(tr(entry.label));
        action->setObjectName(QString::fromUtf8(entry.name));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getLightingMode() == entry.mode);
        action->setActionGroup(lightingModes);
        const SceneViewportRenderer::LightingModeEnum mode = entry.mode;
        QObject::connect(action, &QAction::triggered, this, [this, mode]() {
            setLightingMode(mode);
        });
    }
    display->addSeparator();
    QAction* grid = display->addAction(shortcutText(kShortcutDescActionViewer3DShowGrid));
    grid->setCheckable(true);
    grid->setChecked(_imp->renderer.getShowGrid());
    grid->setShortcut(keySequenceOf(kShortcutIDActionViewer3DShowGrid));
    QObject::connect(grid, &QAction::toggled, this, [this](bool show) {
        setShowGrid(show);
    });
    QAction* guides = display->addAction(shortcutText(kShortcutDescActionViewer3DShowGuides));
    guides->setCheckable(true);
    guides->setChecked(_imp->renderer.getShowGuides());
    guides->setShortcut(keySequenceOf(kShortcutIDActionViewer3DShowGuides));
    QObject::connect(guides, &QAction::toggled, this, [this](bool show) {
        setShowGuides(show);
    });

    menu.exec(globalPos);
    clearGesture();
} // showContextMenu

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SceneViewport.cpp"
