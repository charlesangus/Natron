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

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

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
} // anonymous namespace

USDCamera::USDCamera(NodePtr node)
    : NativeEffectBase(node)
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
    _transform.create(this, page);

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
    _near = createLengthKnob(this, page, tr("Near"), kUSDCameraParamNear,
                             tr("The near clipping distance, in scene units."), 0.1, 0.);
    _far = createLengthKnob(this, page, tr("Far"), kUSDCameraParamFar,
                            tr("The far clipping distance, in scene units."), 10000., 0.);
    _fStop = createLengthKnob(this, page, tr("F-Stop"), kUSDCameraParamFStop,
                              tr("The lens aperture for depth of field. 0 turns depth of field off."), 0., 0.);
    _focusDistance = createLengthKnob(this, page, tr("Focus Distance"), kUSDCameraParamFocusDistance,
                                      tr("The distance in focus, in scene units."), 0., 0.);
} // USDCamera::initializeKnobs

bool
USDCamera::authorLayer(const SceneLayerEdit& edit,
                       std::string* errorMessage) const
{
    PXR_NS::SdfPath path;

    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        return false;
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
    if (!SceneNodeAuthoring::authorTransform(_transform, camera, edit, errorMessage)) {
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

    // Millimetres to tenths of a scene unit: a tenth of a scene unit is 100 x metersPerUnit mm.
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
    const bool authored = camera.CreateProjectionAttr().Set((projection->getValue() == (int)eProjectionOrthographic) ? PXR_NS::UsdGeomTokens->orthographic : PXR_NS::UsdGeomTokens->perspective)
        && authorValue(camera.CreateFocalLengthAttr(), edit, isAnimated(focalLength), lengthAt(focalLength))
        && authorValue(camera.CreateHorizontalApertureAttr(), edit, isAnimated(horizontalAperture), lengthAt(horizontalAperture))
        && authorValue(camera.CreateVerticalApertureAttr(), edit, isAnimated(verticalAperture), lengthAt(verticalAperture))
        && authorValue(camera.CreateHorizontalApertureOffsetAttr(), edit, isAnimated(horizontalApertureOffset), lengthAt(horizontalApertureOffset))
        && authorValue(camera.CreateVerticalApertureOffsetAttr(), edit, isAnimated(verticalApertureOffset), lengthAt(verticalApertureOffset))
        && authorValue(camera.CreateClippingRangeAttr(), edit, isAnimated(nearKnob) || isAnimated(farKnob), [&nearKnob, &farKnob](double frame) {
                              return PXR_NS::GfVec2f((float)nearKnob->getValueAtTime(frame, 0), (float)farKnob->getValueAtTime(frame, 0));
                          })
        && authorValue(camera.CreateFStopAttr(), edit, isAnimated(fStop), valueAt(fStop)) && authorValue(camera.CreateFocusDistanceAttr(), edit, isAnimated(focusDistance), valueAt(focusDistance));
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
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadNothing;
    if (!emitSceneLayer(ScenePayload(), reads, [this](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
