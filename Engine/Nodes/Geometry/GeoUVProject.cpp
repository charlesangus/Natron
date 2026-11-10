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

#include "GeoUVProject.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Format.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/GeoCameraProjection.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
typedef std::array<double, 3> Vec3;

// Rotates v by -angle about one axis: the inverse of the knob's rotation.
Vec3
unrotate(const Vec3& v,
         int axis,
         double degrees)
{
    const double a = -degrees * M_PI / 180.;
    const double c = std::cos(a);
    const double s = std::sin(a);
    const int i = (axis + 1) % 3;
    const int j = (axis + 2) % 3;
    Vec3 out = v;
    out[i] = c * v[i] - s * v[j];
    out[j] = s * v[i] + c * v[j];

    return out;
}

// The point in the plane's own frame, where the projected square spans -0.5 to 0.5.
Vec3
planeLocal(const Vec3& p,
           const GeoUVProjectParams& params)
{
    Vec3 q = { p[0] - params.translate[0], p[1] - params.translate[1], p[2] - params.translate[2] };
    q = unrotate(q, 2, params.rotate[2]);
    q = unrotate(q, 1, params.rotate[1]);
    q = unrotate(q, 0, params.rotate[0]);
    for (int k = 0; k < 3; ++k) {
        const double s = params.scale[(std::size_t)k];
        q[(std::size_t)k] = (std::abs(s) > 1e-12) ? q[(std::size_t)k] / s : 0.;
    }

    return q;
}

std::array<double, 2>
planarUV(const Vec3& p,
         const GeoUVProjectParams& params)
{
    const Vec3 q = planeLocal(p, params);
    switch (params.axis) {
    case eGeoUVAxisX:
        return { 0.5 - q[2], 0.5 + q[1] };
    case eGeoUVAxisY:
        return { 0.5 + q[0], 0.5 - q[2] };
    case eGeoUVAxisZ:
    default:
        return { 0.5 + q[0], 0.5 + q[1] };
    }
}

std::array<double, 2>
cameraUV(const Vec3& p,
         const std::array<double, 16>& m)
{
    double r[4];
    for (int c = 0; c < 4; ++c) {
        r[c] = p[0] * m[(std::size_t)c] + p[1] * m[(std::size_t)(4 + c)] + p[2] * m[(std::size_t)(8 + c)] + m[(std::size_t)(12 + c)];
    }
    // A point in the camera's plane has no image; it is pinned to the frame's centre rather
    // than dividing by zero.
    if (std::abs(r[3]) < 1e-12) {
        return { 0.5, 0.5 };
    }

    return { r[0] / r[3], r[1] / r[3] };
}

// The vertex attribute to write uv into. An existing vertex attribute of two or more numeric
// components keeps its storage and its other components; anything else makes a float32 x3
// one, seeded from a point attribute of that name, which it would otherwise shadow on the
// vertices outside the group.
GeoAttribute*
uvAttributeForWriting(GeoDetailBuilder& detail,
                      const std::string& name)
{
    if (const GeoAttribute* existing = detail.detail().findAttribute(eGeoAttribClassVertex, name)) {
        if (geoIsNumeric(*existing) && !existing->desc().isArray && (existing->desc().tupleSize >= 2)) {
            return detail.attributeForWriting(eGeoAttribClassVertex, name);
        }
    }
    const GeoAttribute* pointUV = detail.detail().findAttribute(eGeoAttribClassPoint, name);
    std::vector<std::pair<std::size_t, std::array<double, 3>>> seeded;
    if (pointUV && geoIsNumeric(*pointUV) && !pointUV->desc().isArray) {
        const GeoDetail& d = detail.detail();
        const int components = std::min(3, (int)pointUV->desc().tupleSize);
        std::vector<double> value((std::size_t)pointUV->desc().tupleSize);
        d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
            geoReadNumeric(*pointUV, (std::size_t)d.vertexPoint(vertex), std::span<double>(value));
            std::array<double, 3> uvw = { 0., 0., 0. };
            for (int k = 0; k < components; ++k) {
                uvw[(std::size_t)k] = value[(std::size_t)k];
            }
            seeded.emplace_back((std::size_t)vertex, uvw);
        });
    }
    if (pointUV) {
        detail.removeAttribute(eGeoAttribClassPoint, name);
    }
    GeoAttribute& uv = detail.addAttribute(eGeoAttribClassVertex, GeoAttributeDesc { name, eGeoStorageFloat32, 3, eGeoTypeInfoNone, false });
    for (const std::pair<std::size_t, std::array<double, 3>>& s : seeded) {
        geoWriteNumeric(uv, s.first, std::span<const double>(s.second));
    }

    return &uv;
}
} // anonymous namespace

bool
projectGeoUVs(GeoDetailBuilder& detail,
              const GeoUVProjectParams& params,
              std::string* errorMessage)
{
    if (params.uvAttribute.empty()) {
        *errorMessage = "The UV Attribute name is empty.";

        return false;
    }
    GeoElementMask primitives;
    if (!geoSelectElements(detail.detail(), eGeoAttribClassPrimitive, params.group, &primitives, errorMessage)) {
        return false;
    }

    std::vector<std::pair<GeoOffset, std::array<double, 2>>> uvs;
    {
        const GeoDetail& d = detail.detail();
        d.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
            if (!primitives[(std::size_t)primitive]) {
                return;
            }
            const GeoVertexRange range = d.primitiveVertices(primitive);
            for (GeoOffset i = 0; i < range.count; ++i) {
                const GeoOffset vertex = range.start + i;
                const Vec3 p = d.pointPosition(d.vertexPoint(vertex));
                uvs.emplace_back(vertex, params.projection == eGeoUVProjectionCamera ? cameraUV(p, params.cameraWorldToFrame) : planarUV(p, params));
            }
        });
    }
    if (uvs.empty()) {
        return true;
    }

    if (params.fitToBounds) {
        double lo[2] = { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity() };
        double hi[2] = { -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };
        for (const std::pair<GeoOffset, std::array<double, 2>>& uv : uvs) {
            for (int k = 0; k < 2; ++k) {
                lo[k] = std::min(lo[k], uv.second[(std::size_t)k]);
                hi[k] = std::max(hi[k], uv.second[(std::size_t)k]);
            }
        }
        for (std::pair<GeoOffset, std::array<double, 2>>& uv : uvs) {
            for (int k = 0; k < 2; ++k) {
                const double extent = hi[k] - lo[k];
                uv.second[(std::size_t)k] = (extent > 0.) ? (uv.second[(std::size_t)k] - lo[k]) / extent : 0.5;
            }
        }
    }

    GeoAttribute* attribute = uvAttributeForWriting(detail, params.uvAttribute);
    std::vector<double> value(attribute->desc().tupleSize);
    for (const std::pair<GeoOffset, std::array<double, 2>>& uv : uvs) {
        const std::size_t offset = (std::size_t)uv.first;
        geoReadNumeric(*attribute, offset, std::span<double>(value));
        value[0] = params.invertU ? 1. - uv.second[0] : uv.second[0];
        value[1] = params.invertV ? 1. - uv.second[1] : uv.second[1];
        geoWriteNumeric(*attribute, offset, std::span<const double>(value));
    }

    return true;
} // projectGeoUVs

GeoUVProject::GeoUVProject(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoUVProject::~GeoUVProject()
{
}

const NodeSpec*
GeoUVProject::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoUVProject::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    auto createString = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      const std::string& defaultValue) {
        KnobStringPtr knob = createKnob<KnobString>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createToggle = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      bool defaultValue) {
        KnobBoolPtr knob = createKnob<KnobBool>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createVector = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      double defaultValue) {
        KnobDoublePtr knob = createKnob<KnobDouble>(label.toStdString(), 3);
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        for (int i = 0; i < 3; ++i) {
            knob->setDefaultValue(defaultValue, i);
        }
        page->addKnob(knob);

        return knob;
    };

    _group = createString(tr("Group"), kGeoUVProjectParamGroup,
                          tr("The primitives whose vertices get texture coordinates, as a group pattern: group names, "
                             "element numbers, @attribute comparisons, ! and ^. Empty projects onto every primitive."),
                          std::string());

    KnobChoicePtr projection = createKnob<KnobChoice>(tr("Projection").toStdString());
    projection->setName(kGeoUVProjectParamProjection);
    projection->setHintToolTip(tr("How points are mapped to texture coordinates.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("planar", tr("Planar").toStdString(),
                                       tr("Straight down the Axis onto the plane placed by Translate, Rotate and Scale.").toStdString()));
        options.push_back(ChoiceOption("camera", tr("Camera").toStdString(),
                                       tr("Through the camera at Camera Path on the Camera input: its frame spans 0 to 1.").toStdString()));
        projection->populateChoices(options);
    }
    projection->setDefaultValue((int)eGeoUVProjectionPlanar);
    projection->setAnimationEnabled(false);
    page->addKnob(projection);
    _projection = projection;

    KnobChoicePtr axis = createKnob<KnobChoice>(tr("Axis").toStdString());
    axis->setName(kGeoUVProjectParamAxis);
    axis->setHintToolTip(tr("For Planar: the axis the plane is seen down, from its positive side, before Rotate.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("x", "X", tr("u runs along -Z and v along Y.").toStdString()));
        options.push_back(ChoiceOption("y", "Y", tr("u runs along X and v along -Z.").toStdString()));
        options.push_back(ChoiceOption("z", "Z", tr("u runs along X and v along Y.").toStdString()));
        axis->populateChoices(options);
    }
    axis->setDefaultValue((int)eGeoUVAxisZ);
    axis->setAnimationEnabled(false);
    page->addKnob(axis);
    _axis = axis;

    _translate = createVector(tr("Translate"), kGeoUVProjectParamTranslate,
                              tr("For Planar: the centre of the plane, which gets uv (0.5, 0.5)."), 0.);
    _rotate = createVector(tr("Rotate"), kGeoUVProjectParamRotate,
                           tr("For Planar: the plane's rotation in degrees, about X, then Y, then Z."), 0.);
    _scale = createVector(tr("Scale"), kGeoUVProjectParamScale,
                          tr("For Planar: the plane's size. A distance of Scale along the u or v direction spans 0 to 1."), 1.);
    _fitToBounds = createToggle(tr("Fit To Bounds"), kGeoUVProjectParamFitToBounds,
                                tr("Stretches the projected texture coordinates so the projected vertices span 0 to 1 "
                                   "in u and in v."),
                                false);

    _cameraPath = createString(tr("Camera Path"), kGeoUVProjectParamCameraPath,
                               tr("For Camera: the path of the camera prim in the scene on the Camera input. Empty uses "
                                  "the scene's first camera."),
                               std::string());

    const ExtentKnobs::FormatKnobSet formatKnobs = ExtentKnobs::createFormatKnobs(this, page);
    formatKnobs.choice->setHintToolTip(tr("For Camera: the format of the image the texture coordinates will map, which the "
                                          "camera's aperture is conformed to by Fit Mode.")
                                           .toStdString());
    _format = formatKnobs.choice;
    _formatSize = formatKnobs.size;
    _formatPar = formatKnobs.par;

    KnobChoicePtr fitMode = createKnob<KnobChoice>(tr("Fit Mode").toStdString());
    fitMode->setName(kGeoUVProjectParamFitMode);
    fitMode->setHintToolTip(tr("For Camera: how the camera's aperture is conformed to the Format when their shapes differ.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("fill", tr("Fill").toStdString(),
                                       tr("The aperture fills the format: the side that does not fit is cropped.").toStdString()));
        options.push_back(ChoiceOption("horizontal", tr("Horizontal").toStdString(),
                                       tr("The aperture's width fills the format's; its height is cropped or extended.").toStdString()));
        options.push_back(ChoiceOption("vertical", tr("Vertical").toStdString(),
                                       tr("The aperture's height fills the format's; its width is cropped or extended.").toStdString()));
        fitMode->populateChoices(options);
    }
    fitMode->setDefaultValue(0);
    fitMode->setAnimationEnabled(false);
    page->addKnob(fitMode);
    _fitMode = fitMode;

    _useFrameHold = createToggle(tr("Use Frame Hold"), kGeoUVProjectParamUseFrameHold,
                                 tr("For Camera: projects through the camera as it is at Frame Hold on every frame, so a "
                                    "moving camera's texture stays stuck to the geometry."),
                                 false);
    KnobDoublePtr frameHold = createKnob<KnobDouble>(tr("Frame Hold").toStdString());
    frameHold->setName(kGeoUVProjectParamFrameHold);
    frameHold->setHintToolTip(tr("The frame the camera is held at when Use Frame Hold is on.").toStdString());
    frameHold->setDefaultValue(1.);
    frameHold->setAnimationEnabled(false);
    page->addKnob(frameHold);
    _frameHold = frameHold;

    _invertU = createToggle(tr("Invert U"), kGeoUVProjectParamInvertU, tr("Writes 1 - u instead of u."), false);
    _invertV = createToggle(tr("Invert V"), kGeoUVProjectParamInvertV, tr("Writes 1 - v instead of v."), false);
    _uvAttribute = createString(tr("UV Attribute"), kGeoUVProjectParamUVAttribute,
                                tr("The vertex attribute the texture coordinates are written to. An existing one keeps "
                                   "its other components; a point attribute of that name is replaced by it."),
                                "uv");

    updateEnabled();
} // GeoUVProject::initializeKnobs

void
GeoUVProject::updateEnabled()
{
    const KnobChoicePtr projection = _projection.lock();
    const bool camera = projection && (projection->getValue() == (int)eGeoUVProjectionCamera);
    auto enable = [](const KnobIPtr& knob,
                     bool enabled) {
        if (knob) {
            knob->setAllDimensionsEnabled(enabled);
        }
    };
    enable(_axis.lock(), !camera);
    enable(_translate.lock(), !camera);
    enable(_rotate.lock(), !camera);
    enable(_scale.lock(), !camera);
    enable(_cameraPath.lock(), camera);
    enable(_format.lock(), camera);
    enable(_formatSize.lock(), camera);
    enable(_formatPar.lock(), camera);
    enable(_fitMode.lock(), camera);
    enable(_useFrameHold.lock(), camera);
    const KnobBoolPtr useFrameHold = _useFrameHold.lock();
    enable(_frameHold.lock(), camera && useFrameHold && useFrameHold->getValue());
}

void
GeoUVProject::onKnobsLoaded()
{
    updateEnabled();
}

bool
GeoUVProject::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr projection = _projection.lock();
    const KnobBoolPtr useFrameHold = _useFrameHold.lock();
    if ((projection && (k == projection.get())) || (useFrameHold && (k == useFrameHold.get()))) {
        updateEnabled();

        return true;
    }

    return false;
}

StatusEnum
GeoUVProject::getPreferredMetadataAt(double /*time*/,
                                     ViewIdx /*view*/,
                                     NodeMetadata& metadata)
{
    // The scene payload is one per layer stack, not per frame, so nothing upstream tells the
    // cache that an animated camera changes the result from frame to frame.
    const KnobChoicePtr projection = _projection.lock();
    const KnobBoolPtr useFrameHold = _useFrameHold.lock();
    if (projection && (projection->getValue() == (int)eGeoUVProjectionCamera) && getInput(1)
        && !(useFrameHold && useFrameHold->getValue())) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

double
GeoUVProject::getFormatAspect() const
{
    const KnobIntPtr size = _formatSize.lock();
    const KnobDoublePtr par = _formatPar.lock();

    if (size && par) {
        const int width = size->getValue(0);
        const int height = size->getValue(1);
        const double pixelAspect = par->getValue();
        if ((width > 0) && (height > 0) && (pixelAspect > 0.)) {
            return width * pixelAspect / height;
        }
    }
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);

    return (format.height() > 0) ? format.width() * format.getPixelAspectRatio() / format.height() : 1.;
}

GeoUVProjectParams
GeoUVProject::readParams(double time,
                         ViewSpec view) const
{
    GeoUVProjectParams p;
    auto text = [time, view](const KnobStringWPtr& weak,
                             const std::string& fallback) {
        const KnobStringPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto toggle = [time, view](const KnobBoolWPtr& weak,
                               bool fallback) {
        const KnobBoolPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto vector = [time, view](const KnobDoubleWPtr& weak,
                               std::array<double, 3>* out) {
        if (const KnobDoublePtr knob = weak.lock()) {
            for (int i = 0; i < 3; ++i) {
                (*out)[(std::size_t)i] = knob->getValueAtTime(time, i, view);
            }
        }
    };
    p.group = text(_group, p.group);
    if (const KnobChoicePtr projection = _projection.lock()) {
        p.projection = projection->getValueAtTime(time, 0, view) == (int)eGeoUVProjectionCamera ? eGeoUVProjectionCamera : eGeoUVProjectionPlanar;
    }
    if (const KnobChoicePtr axis = _axis.lock()) {
        const int value = axis->getValueAtTime(time, 0, view);
        if ((value >= (int)eGeoUVAxisX) && (value <= (int)eGeoUVAxisZ)) {
            p.axis = (GeoUVAxisEnum)value;
        }
    }
    vector(_translate, &p.translate);
    vector(_rotate, &p.rotate);
    vector(_scale, &p.scale);
    p.fitToBounds = toggle(_fitToBounds, p.fitToBounds);
    p.invertU = toggle(_invertU, p.invertU);
    p.invertV = toggle(_invertV, p.invertV);
    p.uvAttribute = text(_uvAttribute, p.uvAttribute);

    return p;
}

StatusEnum
GeoUVProject::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    GeoUVProjectParams params = readParams(args.time, args.view);

    if (params.projection == eGeoUVProjectionCamera) {
        const EffectInstancePtr cameraInput = getInput(1);
        if (!cameraInput) {
            if (errorMessage) {
                *errorMessage = tr("The Camera projection needs a scene with a camera on the Camera input.").toStdString();
            }

            return eStatusFailed;
        }
        std::string error;
        ScenePayload scene;
        if (cameraInput->evaluateScene(EvaluateSceneArgs(args.time, args.view), &scene, &error) != eRenderRoIRetCodeOk) {
            if (errorMessage) {
                *errorMessage = error;
            }

            return eStatusFailed;
        }
        const KnobBoolPtr useFrameHold = _useFrameHold.lock();
        const KnobDoublePtr frameHold = _frameHold.lock();
        const double frame = (useFrameHold && frameHold && useFrameHold->getValue()) ? frameHold->getValue() : args.time;
        const KnobStringPtr cameraPath = _cameraPath.lock();
        const KnobChoicePtr fitMode = _fitMode.lock();
        std::string usedPath;
        if (!GeoCameraProjection::worldToFrame(scene, cameraPath ? cameraPath->getValue() : std::string(), frame,
                                               getApp()->getProjectFrameRate(), getFormatAspect(),
                                               fitMode ? fitMode->getValue() : 0, &params.cameraWorldToFrame, &usedPath, &error)) {
            if (errorMessage) {
                *errorMessage = error;
            }

            return eStatusFailed;
        }
    }

    return renderGeometryFromInput(args, 0, [&params](GeoDetailBuilder& detail, std::string* error) {
        std::string message;
        if (!projectGeoUVs(detail, params, &message)) {
            if (error) {
                *error = message;
            }

            return eStatusFailed;
        }

        return eStatusOK; }, outputDetail, errorMessage);
} // GeoUVProject::renderGeometry

NATRON_NAMESPACE_EXIT
