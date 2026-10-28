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

#include "USDCamera.h"

#include <limits>
#include <string>
#include <vector>

#include <pxr/usd/usd/primRange.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneFileSource.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace {
enum ProjectionEnum {
    eProjectionPerspective = 0,
    eProjectionOrthographic
};

KnobDoublePtr
createLengthKnob(USDCamera* holder,
                 const KnobPagePtr& page,
                 const QString& label,
                 const char* name,
                 const QString& toolTip,
                 double defaultValue,
                 double minimum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
createEditKnob(USDCamera* holder,
               const KnobPagePtr& page,
               const QString& label,
               const char* name,
               const QString& toolTip)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    knob->setDefaultValue(false);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

// A project frame's timecode in the file: (frame + offset) x the file's time codes per second /
// the project frame rate, USDRead's mapping.
struct FileTime {
    double offset = 0.;
    double scale = 1.;

    PXR_NS::UsdTimeCode operator()(double frame) const
    {
        return PXR_NS::UsdTimeCode((frame + offset) * scale);
    }
};

FileTime
makeFileTime(const PXR_NS::UsdStageRefPtr& stage,
             double frameOffset,
             double framesPerSecond)
{
    const double timeCodesPerSecond = stage->GetTimeCodesPerSecond();
    FileTime time;

    time.offset = frameOffset;
    time.scale = timeCodesPerSecond / ((framesPerSecond > 0.) ? framesPerSecond : timeCodesPerSecond);

    return time;
}

// The cameras of stage, depth first.
std::vector<PXR_NS::SdfPath>
listCameras(const PXR_NS::UsdStageRefPtr& stage)
{
    std::vector<PXR_NS::SdfPath> paths;

    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<PXR_NS::UsdGeomCamera>()) {
            paths.push_back(prim.GetPath());
        }
    }

    return paths;
}

// The camera at cameraPath in the stage of file, or its first camera when cameraPath is empty.
PXR_NS::UsdGeomCamera
findFileCamera(const PXR_NS::UsdStageRefPtr& stage,
               const std::string& file,
               const std::string& cameraPath,
               std::string* errorMessage)
{
    const QString fileName = QString::fromStdString(file);

    if (cameraPath.empty()) {
        const std::vector<PXR_NS::SdfPath> cameras = listCameras(stage);
        if (cameras.empty()) {
            *errorMessage = EffectInstance::tr("%1 holds no camera.").arg(fileName).toStdString();

            return PXR_NS::UsdGeomCamera();
        }

        return PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(cameras.front()));
    }
    const PXR_NS::SdfPath path = PXR_NS::SdfPath::IsValidPathString(cameraPath) ? PXR_NS::SdfPath(cameraPath) : PXR_NS::SdfPath();
    const PXR_NS::UsdPrim prim = (path.IsAbsolutePath() && path.IsPrimPath()) ? stage->GetPrimAtPath(path) : PXR_NS::UsdPrim();
    if (!prim || !prim.IsA<PXR_NS::UsdGeomCamera>()) {
        *errorMessage = EffectInstance::tr("%1 has no camera at %2.").arg(fileName).arg(QString::fromStdString(cameraPath)).toStdString();

        return PXR_NS::UsdGeomCamera();
    }

    return PXR_NS::UsdGeomCamera(prim);
}

// Authors to from the file attribute from, as time samples over the project range only if from
// may vary.
template <typename T>
bool
copyAttribute(const PXR_NS::UsdAttribute& from,
              const PXR_NS::UsdAttribute& to,
              const SceneLayerEdit& edit,
              const FileTime& fileTime)
{
    const auto valueAt = [&from, &fileTime](double frame) {
        T value {};

        from.Get(&value, fileTime(frame));

        return value;
    };

    return SceneNodeAuthoring::authorValue(to, edit, from.ValueMightBeTimeVarying(), valueAt);
}

/**
 * @brief Authors the file camera's world transform, its ancestors' flattened in, as xformable's
 * xformOp:transform: one time sample per frame of the project range, or its default when the
 * transform does not move over it.
 **/
bool
authorWorldTransform(const PXR_NS::UsdGeomCamera& fileCamera,
                     const PXR_NS::UsdGeomXformable& xformable,
                     const SceneLayerEdit& edit,
                     const FileTime& fileTime)
{
    const PXR_NS::UsdGeomXformOp op = xformable.MakeMatrixXform();

    if (!op) {
        return false;
    }
    const PXR_NS::UsdPrim prim = fileCamera.GetPrim();
    PXR_NS::UsdGeomXformCache cache;
    std::vector<double> frames = SceneNodeAuthoring::getSampleFrames(edit);
    if (frames.empty()) {
        frames.push_back(edit.firstFrame);
    }
    std::vector<PXR_NS::GfMatrix4d> matrices;
    matrices.reserve(frames.size());
    bool moves = false;
    for (double frame : frames) {
        cache.SetTime(fileTime(frame));
        matrices.push_back(cache.GetLocalToWorldTransform(prim));
        moves = moves || (matrices.back() != matrices.front());
    }
    if (!moves) {
        return op.Set(matrices.front());
    }
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!op.Set(matrices[i], SceneNodeAuthoring::frameToTimeCode(edit, frames[i]))) {
            return false;
        }
    }

    return true;
}

// The translate, rotate (degrees, in rotateOrder) and scale knob values that compose matrix.
void
decomposeTransform(const PXR_NS::GfMatrix4d& matrix,
                   SceneRotateOrderEnum rotateOrder,
                   PXR_NS::GfVec3d* translate,
                   PXR_NS::GfVec3d* rotate,
                   PXR_NS::GfVec3d* scale)
{
    *translate = matrix.ExtractTranslation();
    PXR_NS::GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        const PXR_NS::GfVec3d row = matrix.GetRow3(i);
        (*scale)[i] = row.GetLength();
    }
    // A mirror is put on X.
    if (matrix.GetDeterminant3() < 0.) {
        (*scale)[0] = -(*scale)[0];
    }
    for (int i = 0; i < 3; ++i) {
        if ((*scale)[i] != 0.) {
            rotation.SetRow3(i, matrix.GetRow3(i) / (*scale)[i]);
        }
    }
    // Decompose() takes the axes last applied first.
    const PXR_NS::GfVec3d axes[3] = { PXR_NS::GfVec3d::XAxis(), PXR_NS::GfVec3d::YAxis(), PXR_NS::GfVec3d::ZAxis() };
    const int first = SceneNodeAuthoring::getRotateOrderAxis(rotateOrder, 0);
    const int second = SceneNodeAuthoring::getRotateOrderAxis(rotateOrder, 1);
    const int third = SceneNodeAuthoring::getRotateOrderAxis(rotateOrder, 2);
    const PXR_NS::GfVec3d angles = rotation.ExtractRotation().Decompose(axes[third], axes[second], axes[first]);
    (*rotate)[third] = angles[0];
    (*rotate)[second] = angles[1];
    (*rotate)[first] = angles[2];
}

// Shows value in dimension of knob, dropping any animation or expression it held.
void
setFileValue(const KnobDoublePtr& knob,
             int dimension,
             double value)
{
    if (knob->isAnimated(dimension)) {
        knob->removeAnimation(ViewSpec::all(), dimension);
    }
    if (!knob->getExpression(dimension).empty()) {
        knob->clearExpression(dimension, true);
    }
    knob->setValue(value, ViewSpec::all(), dimension, true);
}

// What showFileValues() writes, read from the file at one time.
struct FileCameraValues {
    PXR_NS::GfMatrix4d world { 1. };
    PXR_NS::TfToken projection;
    float focalLength = 0.f;
    float horizontalAperture = 0.f;
    float verticalAperture = 0.f;
    float horizontalApertureOffset = 0.f;
    float verticalApertureOffset = 0.f;
    PXR_NS::GfVec2f clippingRange { 0.f, 0.f };
    float fStop = 0.f;
    float focusDistance = 0.f;
};
} // anonymous namespace

// The file a file camera comes from.
struct USDCamera::FileCamera {
    SceneFileSource source;
};

// The knobs an authoring with Read From File reads, read once before it.
struct USDCamera::FileSettings {
    bool enabled = false;
    std::string path;
    // The selected camera's path, empty for the file's first.
    std::string cameraPath;
    double frameOffset = 0.;
    bool editTransform = false;
    bool editLens = false;
    bool editClipping = false;
};

USDCamera::USDCamera(NodePtr node)
    : NativeEffectBase(node)
    , _fileCamera(new FileCamera)
{
}

USDCamera::~USDCamera()
{
}

const NodeSpec*
USDCamera::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDCamera::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "Camera"));
}

void
USDCamera::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Camera"), tr("The path of the camera prim the node authors."));

    KnobBoolPtr readFromFile = createKnob<KnobBool>(tr("Read From File"));
    readFromFile->setName(kUSDCameraParamReadFromFile);
    readFromFile->setHintToolTip(tr("Takes the camera from a camera prim in a file, such as a tracked camera: its world "
                                    "transform, lens and clipping at every frame. The knobs show the file's values at the "
                                    "current frame; an Edit toggle lets a group's knobs override them."));
    readFromFile->setDefaultValue(false);
    readFromFile->setAnimationEnabled(false);
    page->addKnob(readFromFile);
    _readFromFile = readFromFile;

    KnobFilePtr file = createKnob<KnobFile>(tr("File"));
    file->setName(kUSDCameraParamFile);
    file->setHintToolTip(tr("The USD file (.usd, .usda, .usdc, .usdz) or Alembic file (.abc) holding the camera."));
    file->setFileExtensions({ "usd", "usda", "usdc", "usdz", "abc" });
    file->setAddNewLine(false);
    page->addKnob(file);
    _file = file;

    KnobButtonPtr reload = createKnob<KnobButton>(tr("Reload"));
    reload->setName(kUSDCameraParamReload);
    reload->setHintToolTip(tr("Reads the file again, and every file it brings in, so the camera follows files changed on "
                              "disk. Each changed file is read again in place: every scene using it shows the new content. "
                              "Renders reading scenes are stopped or waited for first."));
    page->addKnob(reload);
    _reload = reload;

    KnobChoicePtr cameraPrim = createKnob<KnobChoice>(tr("Camera Prim"));
    cameraPrim->setName(kUSDCameraParamCameraPrim);
    cameraPrim->setHintToolTip(tr("The file's camera to read, by prim path. The first camera in the file by default."));
    cameraPrim->setAnimationEnabled(false);
    page->addKnob(cameraPrim);
    _cameraPrim = cameraPrim;

    KnobDoublePtr frameOffset = createKnob<KnobDouble>(tr("Frame Offset"));
    frameOffset->setName(kUSDCameraParamFrameOffset);
    frameOffset->setHintToolTip(tr("Frames added to the project frame before it is converted to the file's timecode: "
                                   "frame f reads the file at (f + offset) x the file's time codes per second / the "
                                   "project frame rate."));
    frameOffset->setAnimationEnabled(false);
    frameOffset->setDefaultValue(0.);
    page->addKnob(frameOffset);
    _frameOffset = frameOffset;

    _editTransform = createEditKnob(this, page, tr("Edit Transform"), kUSDCameraParamEditTransform,
                                    tr("Enables the transform knobs, which then move the file camera in its own space: "
                                       "they start at no change and act after the file's transform. Off shows the file's "
                                       "transform again."));
    _transform.create(this, page);

    _editLens = createEditKnob(this, page, tr("Edit Lens"), kUSDCameraParamEditLens,
                               tr("Enables the lens knobs, from projection to focus distance, whose values then replace "
                                  "the file's. They start at the file's values at the current frame. Off shows the file's "
                                  "values again."));

    KnobChoicePtr projection = createKnob<KnobChoice>(tr("Projection"));
    projection->setName(kUSDCameraParamProjection);
    projection->setHintToolTip(tr("Perspective, or orthographic with the aperture as the width and height of the view."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("perspective", tr("Perspective").toStdString(), ""));
        options.push_back(ChoiceOption("orthographic", tr("Orthographic").toStdString(), ""));
        projection->populateChoices(options);
    }
    projection->setDefaultValue((int)eProjectionPerspective);
    projection->setAnimationEnabled(false);
    page->addKnob(projection);
    _projection = projection;

    const double unbounded = -std::numeric_limits<double>::max();
    _focalLength = createLengthKnob(this, page, tr("Focal Length"), kUSDCameraParamFocalLength,
                                    tr("The lens focal length, in millimetres."), 50., 0.);
    _horizontalAperture = createLengthKnob(this, page, tr("Horizontal Aperture"), kUSDCameraParamHorizontalAperture,
                                           tr("The width of the film back, in millimetres."), 36., 0.);
    _verticalAperture = createLengthKnob(this, page, tr("Vertical Aperture"), kUSDCameraParamVerticalAperture,
                                         tr("The height of the film back, in millimetres."), 24., 0.);
    _horizontalApertureOffset = createLengthKnob(this, page, tr("Horizontal Aperture Offset"), kUSDCameraParamHorizontalApertureOffset,
                                                 tr("Shifts the film back horizontally, in millimetres."), 0., unbounded);
    _verticalApertureOffset = createLengthKnob(this, page, tr("Vertical Aperture Offset"), kUSDCameraParamVerticalApertureOffset,
                                               tr("Shifts the film back vertically, in millimetres."), 0., unbounded);
    _fStop = createLengthKnob(this, page, tr("F-Stop"), kUSDCameraParamFStop,
                              tr("The lens aperture for depth of field. 0 turns depth of field off."), 0., 0.);
    _focusDistance = createLengthKnob(this, page, tr("Focus Distance"), kUSDCameraParamFocusDistance,
                                      tr("The distance in focus, in scene units."), 0., 0.);

    _editClipping = createEditKnob(this, page, tr("Edit Clipping"), kUSDCameraParamEditClipping,
                                   tr("Enables Near and Far, whose values then replace the file's clipping range. They "
                                      "start at the file's values at the current frame. Off shows the file's values again."));
    _near = createLengthKnob(this, page, tr("Near"), kUSDCameraParamNear,
                             tr("The near clipping distance, in scene units."), 0.1, 0.);
    _far = createLengthKnob(this, page, tr("Far"), kUSDCameraParamFar,
                            tr("The far clipping distance, in scene units."), 10000., 0.);

    refreshKnobStates();
} // USDCamera::initializeKnobs

std::vector<KnobIPtr>
USDCamera::getTransformKnobs() const
{
    return { _transform.translate.lock(), _transform.rotate.lock(), _transform.scale.lock(), _transform.uniformScale.lock(), _transform.rotateOrder.lock() };
}

std::vector<KnobIPtr>
USDCamera::getLensKnobs() const
{
    return { _projection.lock(), _focalLength.lock(), _horizontalAperture.lock(), _verticalAperture.lock(), _horizontalApertureOffset.lock(),
             _verticalApertureOffset.lock(), _fStop.lock(), _focusDistance.lock() };
}

std::vector<KnobIPtr>
USDCamera::getClippingKnobs() const
{
    return { _near.lock(), _far.lock() };
}

void
USDCamera::refreshKnobStates()
{
    KnobBoolPtr readFromFile = _readFromFile.lock();
    KnobBoolPtr editTransform = _editTransform.lock();
    KnobBoolPtr editLens = _editLens.lock();
    KnobBoolPtr editClipping = _editClipping.lock();

    if (!readFromFile || !editTransform || !editLens || !editClipping) {
        return;
    }
    const bool fromFile = readFromFile->getValue();
    const std::vector<KnobIPtr> fileKnobs = { _file.lock(), _reload.lock(), _cameraPrim.lock(), _frameOffset.lock(), editTransform, editLens, editClipping };
    for (const KnobIPtr& knob : fileKnobs) {
        if (knob) {
            knob->setSecret(!fromFile);
        }
    }
    // The file drives a group not being edited: its knobs only show the file's values, which
    // must neither key the node nor ask for a render as they follow the timeline.
    const auto drive = [](const std::vector<KnobIPtr>& knobs, bool driven) {
        for (const KnobIPtr& knob : knobs) {
            if (!knob) {
                continue;
            }
            knob->setAllDimensionsEnabled(!driven);
            if (knob->isHashExempt() != driven) {
                knob->setHashExempt(driven);
                knob->setEvaluateOnChange(!driven);
            }
        }
    };
    drive(getTransformKnobs(), fromFile && !editTransform->getValue());
    drive(getLensKnobs(), fromFile && !editLens->getValue());
    drive(getClippingKnobs(), fromFile && !editClipping->getValue());
} // USDCamera::refreshKnobStates

void
USDCamera::refreshCameraChoices(bool selectFirst)
{
    KnobBoolPtr readFromFile = _readFromFile.lock();
    KnobFilePtr file = _file.lock();
    KnobChoicePtr cameraPrim = _cameraPrim.lock();

    if (!readFromFile || !file || !cameraPrim || !readFromFile->getValue()) {
        return;
    }
    const std::string path = file->getSingleFilePath();
    if (path.empty()) {
        return;
    }
    std::vector<ChoiceOption> options;
    {
        SceneWriteBarrier::ReadScope read;
        std::string error;
        PXR_NS::SdfLayerRefPtr layer = SceneFileSource::openLayer(path, &error);
        PXR_NS::UsdStageRefPtr stage = layer ? _fileCamera->source.getStage(layer) : PXR_NS::UsdStageRefPtr();
        // An unreadable file keeps the menu, and the selection, for the authoring to report.
        if (!stage) {
            return;
        }
        for (const PXR_NS::SdfPath& camera : listCameras(stage)) {
            options.push_back(ChoiceOption(camera.GetString(), "", ""));
        }
    }
    cameraPrim->populateChoices(options);
    if (selectFirst && !options.empty() && (cameraPrim->getActiveEntry().id.empty() || !cameraPrim->isActiveEntryPresentInEntries())) {
        cameraPrim->setValueFromID(options.front().id, 0);
    }
} // USDCamera::refreshCameraChoices

void
USDCamera::showFileValues(double time)
{
    KnobBoolPtr editTransform = _editTransform.lock();
    KnobBoolPtr editLens = _editLens.lock();
    KnobBoolPtr editClipping = _editClipping.lock();

    if (!editTransform || !editLens || !editClipping) {
        return;
    }
    showFileValues(time, !editTransform->getValue(), !editLens->getValue(), !editClipping->getValue());
}

void
USDCamera::showFileValues(double time,
                          bool transform,
                          bool lens,
                          bool clipping)
{
    KnobBoolPtr readFromFile = _readFromFile.lock();
    KnobFilePtr file = _file.lock();
    KnobChoicePtr cameraPrim = _cameraPrim.lock();
    KnobDoublePtr frameOffset = _frameOffset.lock();

    if (!readFromFile || !file || !cameraPrim || !frameOffset || !readFromFile->getValue() || (!transform && !lens && !clipping)) {
        return;
    }
    const std::string path = file->getSingleFilePath();
    if (path.empty()) {
        return;
    }
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    const double framesPerSecond = project ? project->getProjectFrameRate() : 0.;
    const double metersPerUnit = project ? project->getProjectMetersPerUnit() : 1.;

    // Read under the barrier, written after it: writing knobs may wait on renders.
    FileCameraValues values;
    {
        SceneWriteBarrier::ReadScope read;
        std::string error;
        PXR_NS::SdfLayerRefPtr layer = SceneFileSource::openLayer(path, &error);
        PXR_NS::UsdStageRefPtr stage = layer ? _fileCamera->source.getStage(layer) : PXR_NS::UsdStageRefPtr();
        if (!stage) {
            return;
        }
        const PXR_NS::UsdGeomCamera camera = findFileCamera(stage, path, cameraPrim->getActiveEntry().id, &error);
        if (!camera) {
            return;
        }
        const PXR_NS::UsdTimeCode fileTime = makeFileTime(stage, frameOffset->getValue(), framesPerSecond)(time);
        values.world = camera.ComputeLocalToWorldTransform(fileTime);
        camera.GetProjectionAttr().Get(&values.projection, fileTime);
        camera.GetFocalLengthAttr().Get(&values.focalLength, fileTime);
        camera.GetHorizontalApertureAttr().Get(&values.horizontalAperture, fileTime);
        camera.GetVerticalApertureAttr().Get(&values.verticalAperture, fileTime);
        camera.GetHorizontalApertureOffsetAttr().Get(&values.horizontalApertureOffset, fileTime);
        camera.GetVerticalApertureOffsetAttr().Get(&values.verticalApertureOffset, fileTime);
        camera.GetClippingRangeAttr().Get(&values.clippingRange, fileTime);
        camera.GetFStopAttr().Get(&values.fStop, fileTime);
        camera.GetFocusDistanceAttr().Get(&values.focusDistance, fileTime);
    }

    beginChanges();
    if (transform) {
        KnobDoublePtr translate = _transform.translate.lock();
        KnobDoublePtr rotate = _transform.rotate.lock();
        KnobDoublePtr scale = _transform.scale.lock();
        KnobDoublePtr uniformScale = _transform.uniformScale.lock();
        KnobChoicePtr rotateOrder = _transform.rotateOrder.lock();
        if (translate && rotate && scale && uniformScale && rotateOrder) {
            PXR_NS::GfVec3d t, r, s;
            decomposeTransform(values.world, (SceneRotateOrderEnum)rotateOrder->getValue(), &t, &r, &s);
            for (int i = 0; i < 3; ++i) {
                setFileValue(translate, i, t[i]);
                setFileValue(rotate, i, r[i]);
                setFileValue(scale, i, s[i]);
            }
            setFileValue(uniformScale, 0, 1.);
        }
    }
    if (lens) {
        // The knobs' millimetres, at the project's units, as authoring them would write the file's values.
        const double cameraUnitsToMillimetres = 100. * metersPerUnit;
        KnobChoicePtr projection = _projection.lock();
        if (projection) {
            projection->setValue((values.projection == PXR_NS::UsdGeomTokens->orthographic) ? (int)eProjectionOrthographic : (int)eProjectionPerspective, ViewSpec::all(), 0, true);
        }
        const std::pair<KnobDoubleWPtr, double> lengths[] = {
            { _focalLength, values.focalLength * cameraUnitsToMillimetres },
            { _horizontalAperture, values.horizontalAperture * cameraUnitsToMillimetres },
            { _verticalAperture, values.verticalAperture * cameraUnitsToMillimetres },
            { _horizontalApertureOffset, values.horizontalApertureOffset * cameraUnitsToMillimetres },
            { _verticalApertureOffset, values.verticalApertureOffset * cameraUnitsToMillimetres },
            { _fStop, values.fStop },
            { _focusDistance, values.focusDistance },
        };
        for (const std::pair<KnobDoubleWPtr, double>& length : lengths) {
            KnobDoublePtr knob = length.first.lock();
            if (knob) {
                setFileValue(knob, 0, length.second);
            }
        }
    }
    if (clipping) {
        KnobDoublePtr nearKnob = _near.lock();
        KnobDoublePtr farKnob = _far.lock();
        if (nearKnob && farKnob) {
            setFileValue(nearKnob, 0, values.clippingRange[0]);
            setFileValue(farKnob, 0, values.clippingRange[1]);
        }
    }
    endChanges();
} // USDCamera::showFileValues

void
USDCamera::onKnobsLoaded()
{
    refreshKnobStates();
    refreshCameraChoices(false);
    showFileValues(getCurrentTime());
}

void
USDCamera::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                             double time)
{
    NativeEffectBase::refreshExtraStateAfterTimeChanged(isPlayback, time);
    // Writing knobs during playback would only slow it down; the knobs catch up when it stops.
    if (!isPlayback) {
        showFileValues(time);
    }
}

bool
USDCamera::knobChanged(KnobI* k,
                       ValueChangedReasonEnum reason,
                       ViewSpec /*view*/,
                       double time,
                       bool /*originatedFromMainThread*/)
{
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();

    if (!k || loading || (reason == eValueChangedReasonTimeChanged)) {
        return false;
    }
    KnobBoolPtr readFromFile = _readFromFile.lock();
    KnobFilePtr file = _file.lock();
    KnobButtonPtr reload = _reload.lock();
    KnobChoicePtr cameraPrim = _cameraPrim.lock();
    KnobDoublePtr frameOffset = _frameOffset.lock();
    KnobBoolPtr editTransform = _editTransform.lock();
    KnobBoolPtr editLens = _editLens.lock();
    KnobBoolPtr editClipping = _editClipping.lock();
    if (!readFromFile || !file || !reload || !cameraPrim || !frameOffset || !editTransform || !editLens || !editClipping) {
        return false;
    }

    if (k == reload.get()) {
        std::string error;
        if (!_fileCamera->source.reload(file->getSingleFilePath(), &error)) {
            setPersistentMessage(eMessageTypeError, tr("Reload failed: %1").arg(QString::fromStdString(error)).toStdString());
        } else {
            clearPersistentMessage(false);
        }
        purgeCaches();
        // Same as USDRead: the Reload bracket hashed this node before purgeCaches() moved its
        // hash inputs.
        NodePtr node = getNode();
        if (node) {
            node->markHashDirty();
        }
        file->reloadOtherNodesReadingTheseFiles();
        // A camera gone from the file stays selected, so the node reports it.
        refreshCameraChoices(false);
        showFileValues(time);

        return true;
    }
    if (k == readFromFile.get()) {
        refreshKnobStates();
        refreshCameraChoices(true);
        showFileValues(time);

        return true;
    }
    if (k == file.get()) {
        refreshCameraChoices(true);
        showFileValues(time);

        return true;
    }
    if ((k == cameraPrim.get()) || (k == frameOffset.get())) {
        showFileValues(time);

        return true;
    }
    if (k == editTransform.get()) {
        if (editTransform->getValue()) {
            // The knobs become an offset from the file's transform, starting at none.
            for (const KnobIPtr& knob : getTransformKnobs()) {
                if (knob && (knob != _transform.rotateOrder.lock())) {
                    for (int i = 0; i < knob->getDimension(); ++i) {
                        knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(i);
                    }
                }
            }
        } else {
            showFileValues(time, true, false, false);
        }
        refreshKnobStates();

        return true;
    }
    // Edit on starts from the file's values, off restores them.
    if (k == editLens.get()) {
        showFileValues(time, false, true, false);
        refreshKnobStates();

        return true;
    }
    if (k == editClipping.get()) {
        showFileValues(time, false, false, true);
        refreshKnobStates();

        return true;
    }

    return false;
} // USDCamera::knobChanged

void
USDCamera::purgeCaches()
{
    KnobFilePtr file = _file.lock();

    if (file) {
        file->invalidateFileStamp();
    }
    _fileCamera->source.purgeStamps();
    invalidateSceneLayer();
    clearActionsCache();
}

void
USDCamera::appendNodeContentHash(ContentHasher& h) const
{
    // The file's own stamp is in the File knob's hash; this covers every file it brings in.
    h.appendU64(_fileCamera->source.getDigest());
}

bool
USDCamera::authorLayer(const SceneLayerEdit& edit,
                       const FileSettings& file,
                       std::string* errorMessage) const
{
    PXR_NS::SdfPath path;

    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        return false;
    }

    // The file camera, opened before this layer's stage so its diagnostics come first.
    PXR_NS::UsdGeomCamera fileCamera;
    FileTime fileTime;
    if (file.enabled) {
        PXR_NS::SdfLayerRefPtr fileLayer = SceneFileSource::openLayer(file.path, errorMessage);
        if (!fileLayer) {
            return false;
        }
        _fileCamera->source.hold(fileLayer);
        PXR_NS::UsdStageRefPtr fileStage = _fileCamera->source.getStage(fileLayer);
        if (!fileStage) {
            *errorMessage = tr("USD could not compose %1.").arg(QString::fromStdString(file.path)).toStdString();

            return false;
        }
        fileCamera = findFileCamera(fileStage, file.path, file.cameraPath, errorMessage);
        if (!fileCamera) {
            return false;
        }
        fileTime = makeFileTime(fileStage, file.frameOffset, edit.framesPerSecond);
    }

    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, errorMessage);
    if (!stage) {
        return false;
    }
    PXR_NS::UsdGeomCamera camera = PXR_NS::UsdGeomCamera::Define(stage, path);
    if (!camera) {
        *errorMessage = tr("USD could not define a camera at %1.").arg(QString::fromStdString(path.GetString())).toStdString();

        return false;
    }
    if (file.enabled) {
        if (!authorWorldTransform(fileCamera, camera, edit, fileTime)) {
            *errorMessage = tr("USD could not author the transform of %1.").arg(QString::fromStdString(path.GetString())).toStdString();

            return false;
        }
        // The knobs' ops follow the file's matrix in the op order, so they act in the camera's space.
        if (file.editTransform && !SceneNodeAuthoring::authorTransform(_transform, camera, edit, errorMessage)) {
            return false;
        }
    } else if (!SceneNodeAuthoring::authorTransform(_transform, camera, edit, errorMessage)) {
        return false;
    }

    KnobChoicePtr projection = _projection.lock();
    KnobDoublePtr focalLength = _focalLength.lock();
    KnobDoublePtr horizontalAperture = _horizontalAperture.lock();
    KnobDoublePtr verticalAperture = _verticalAperture.lock();
    KnobDoublePtr horizontalApertureOffset = _horizontalApertureOffset.lock();
    KnobDoublePtr verticalApertureOffset = _verticalApertureOffset.lock();
    KnobDoublePtr nearKnob = _near.lock();
    KnobDoublePtr farKnob = _far.lock();
    KnobDoublePtr fStop = _fStop.lock();
    KnobDoublePtr focusDistance = _focusDistance.lock();
    if (!projection || !focalLength || !horizontalAperture || !verticalAperture || !horizontalApertureOffset || !verticalApertureOffset || !nearKnob || !farKnob || !fStop || !focusDistance) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return false;
    }

    // Millimetres to tenths of a scene unit: a tenth of a scene unit is 100 x the project's
    // metersPerUnit mm.
    const double millimetresToCameraUnits = 1. / (100. * SceneNodeAuthoring::getMetersPerUnit(edit));
    const auto lengthAt = [millimetresToCameraUnits](const KnobDoublePtr& knob) {
        return [knob, millimetresToCameraUnits](double frame) {
            return (float)(knob->getValueAtTime(frame, 0) * millimetresToCameraUnits);
        };
    };
    const auto valueAt = [](const KnobDoublePtr& knob) {
        return [knob](double frame) {
            return (float)knob->getValueAtTime(frame, 0);
        };
    };
    using SceneNodeAuthoring::authorValue;
    using SceneNodeAuthoring::isAnimated;
    bool authored;
    // The file's values are copied as they are, in its units: a units mismatch is only warned of.
    if (file.enabled && !file.editLens) {
        authored = copyAttribute<PXR_NS::TfToken>(fileCamera.GetProjectionAttr(), camera.CreateProjectionAttr(), edit, fileTime)
            && copyAttribute<float>(fileCamera.GetFocalLengthAttr(), camera.CreateFocalLengthAttr(), edit, fileTime)
            && copyAttribute<float>(fileCamera.GetHorizontalApertureAttr(), camera.CreateHorizontalApertureAttr(), edit, fileTime)
            && copyAttribute<float>(fileCamera.GetVerticalApertureAttr(), camera.CreateVerticalApertureAttr(), edit, fileTime)
            && copyAttribute<float>(fileCamera.GetHorizontalApertureOffsetAttr(), camera.CreateHorizontalApertureOffsetAttr(), edit, fileTime)
            && copyAttribute<float>(fileCamera.GetVerticalApertureOffsetAttr(), camera.CreateVerticalApertureOffsetAttr(), edit, fileTime)
            && copyAttribute<float>(fileCamera.GetFStopAttr(), camera.CreateFStopAttr(), edit, fileTime)
            && copyAttribute<float>(fileCamera.GetFocusDistanceAttr(), camera.CreateFocusDistanceAttr(), edit, fileTime);
    } else {
        authored = camera.CreateProjectionAttr().Set((projection->getValue() == (int)eProjectionOrthographic) ? PXR_NS::UsdGeomTokens->orthographic : PXR_NS::UsdGeomTokens->perspective)
            && authorValue(camera.CreateFocalLengthAttr(), edit, isAnimated(focalLength), lengthAt(focalLength))
            && authorValue(camera.CreateHorizontalApertureAttr(), edit, isAnimated(horizontalAperture), lengthAt(horizontalAperture))
            && authorValue(camera.CreateVerticalApertureAttr(), edit, isAnimated(verticalAperture), lengthAt(verticalAperture))
            && authorValue(camera.CreateHorizontalApertureOffsetAttr(), edit, isAnimated(horizontalApertureOffset), lengthAt(horizontalApertureOffset))
            && authorValue(camera.CreateVerticalApertureOffsetAttr(), edit, isAnimated(verticalApertureOffset), lengthAt(verticalApertureOffset))
            && authorValue(camera.CreateFStopAttr(), edit, isAnimated(fStop), valueAt(fStop))
            && authorValue(camera.CreateFocusDistanceAttr(), edit, isAnimated(focusDistance), valueAt(focusDistance));
    }
    if (authored) {
        if (file.enabled && !file.editClipping) {
            authored = copyAttribute<PXR_NS::GfVec2f>(fileCamera.GetClippingRangeAttr(), camera.CreateClippingRangeAttr(), edit, fileTime);
        } else {
            authored = authorValue(camera.CreateClippingRangeAttr(), edit, isAnimated(nearKnob) || isAnimated(farKnob), [&nearKnob, &farKnob](double frame) {
                return PXR_NS::GfVec2f((float)nearKnob->getValueAtTime(frame, 0), (float)farKnob->getValueAtTime(frame, 0));
            });
        }
    }
    if (!authored) {
        *errorMessage = tr("USD could not author the camera at %1.").arg(QString::fromStdString(path.GetString())).toStdString();

        return false;
    }

    return true;
} // USDCamera::authorLayer

StatusEnum
USDCamera::renderScene(const SceneRenderActionArgs& /*args*/,
                       ScenePayload* outputScene,
                       std::string* errorMessage)
{
    FileSettings file;
    {
        KnobBoolPtr readFromFile = _readFromFile.lock();
        file.enabled = readFromFile && readFromFile->getValue();
    }
    if (file.enabled) {
        KnobFilePtr fileKnob = _file.lock();
        KnobChoicePtr cameraPrim = _cameraPrim.lock();
        KnobDoublePtr frameOffset = _frameOffset.lock();
        KnobBoolPtr editTransform = _editTransform.lock();
        KnobBoolPtr editLens = _editLens.lock();
        KnobBoolPtr editClipping = _editClipping.lock();
        file.path = fileKnob ? fileKnob->getSingleFilePath() : std::string();
        if (file.path.empty()) {
            if (fileKnob && !fileKnob->getValue().empty()) {
                *errorMessage = tr("%1 names a frame sequence; USDCamera reads a single file.").arg(QString::fromStdString(fileKnob->getValue())).toStdString();
            } else {
                *errorMessage = tr("No camera file to read.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        file.cameraPath = cameraPrim ? cameraPrim->getActiveEntry().id : std::string();
        file.frameOffset = frameOffset ? frameOffset->getValue() : 0.;
        file.editTransform = editTransform && editTransform->getValue();
        file.editLens = editLens && editLens->getValue();
        file.editClipping = editClipping && editClipping->getValue();
    }

    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadNothing;
    if (!emitSceneLayer(ScenePayload(), reads, [this, file](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, file, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    if (file.enabled) {
        // The file's units as its stage reads them, USD's fallbacks where it authors none.
        PXR_NS::UsdStageRefPtr fileStage = _fileCamera->source.getLastStage();
        AppInstancePtr app = getApp();
        ProjectPtr project = app ? app->getProject() : ProjectPtr();
        if (fileStage) {
            SceneMetadata metadata;
            metadata.upAxis = (PXR_NS::UsdGeomGetStageUpAxis(fileStage) == PXR_NS::UsdGeomTokens->z) ? SceneMetadata::eUpAxisZ : SceneMetadata::eUpAxisY;
            metadata.metersPerUnit = PXR_NS::UsdGeomGetStageMetersPerUnit(fileStage);
            const std::string warning = getSceneUnitsWarning(metadata, project);
            if (!warning.empty()) {
                setPersistentMessage(eMessageTypeWarning, warning);

                return eStatusOK;
            }
        }
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDCamera::renderScene

NATRON_NAMESPACE_EXIT
