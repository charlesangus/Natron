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

#include "USDProjectUV.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include <QMetaObject>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isBlank(const std::string& s)
{
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

// Whether the transform of prim or of any ancestor may change over time.
bool
transformMightVary(const PXR_NS::UsdPrim& prim)
{
    for (PXR_NS::UsdPrim p = prim; p && !p.IsPseudoRoot(); p = p.GetParent()) {
        const PXR_NS::UsdGeomXformable xformable(p);
        if (xformable && xformable.TransformMightBeTimeVarying()) {
            return true;
        }
    }

    return false;
}

bool
attributeMightVary(const PXR_NS::UsdAttribute& attribute)
{
    return attribute && attribute.ValueMightBeTimeVarying();
}

bool
cameraMightVary(const PXR_NS::UsdGeomCamera& camera)
{
    return transformMightVary(camera.GetPrim())
        || attributeMightVary(camera.GetProjectionAttr())
        || attributeMightVary(camera.GetFocalLengthAttr())
        || attributeMightVary(camera.GetHorizontalApertureAttr())
        || attributeMightVary(camera.GetVerticalApertureAttr())
        || attributeMightVary(camera.GetHorizontalApertureOffsetAttr())
        || attributeMightVary(camera.GetVerticalApertureOffsetAttr())
        || attributeMightVary(camera.GetClippingRangeAttr());
}

/**
 * @brief values, a primvar of interpolation interp on a mesh of the given topology, as one value
 * per face vertex. False when its size does not fit the interpolation.
 **/
template <typename T>
bool
expandToFaceVertices(const PXR_NS::VtArray<T>& values,
                     const PXR_NS::TfToken& interp,
                     const PXR_NS::VtIntArray& counts,
                     const PXR_NS::VtIntArray& faceVertexIndices,
                     std::vector<T>* out)
{
    out->clear();
    out->reserve(faceVertexIndices.size());
    if (interp == PXR_NS::UsdGeomTokens->constant) {
        if (values.empty()) {
            return false;
        }
        out->assign(faceVertexIndices.size(), values[0]);

        return true;
    }
    if (interp == PXR_NS::UsdGeomTokens->uniform) {
        if (values.size() != counts.size()) {
            return false;
        }
        for (std::size_t f = 0; f < counts.size(); ++f) {
            for (int v = 0; v < counts[f]; ++v) {
                out->push_back(values[f]);
            }
        }

        return true;
    }
    if ((interp == PXR_NS::UsdGeomTokens->vertex) || (interp == PXR_NS::UsdGeomTokens->varying)) {
        for (int index : faceVertexIndices) {
            if ((index < 0) || ((std::size_t)index >= values.size())) {
                return false;
            }
            out->push_back(values[index]);
        }

        return true;
    }
    if (interp == PXR_NS::UsdGeomTokens->faceVarying) {
        if (values.size() != faceVertexIndices.size()) {
            return false;
        }
        out->assign(values.begin(), values.end());

        return true;
    }

    return false;
} // expandToFaceVertices

// Outside the clip volume on one side, one bit per side, for the frustum culling test.
unsigned int
getOutcode(const PXR_NS::GfVec4d& clip)
{
    const double w = clip[3];
    unsigned int code = 0;

    for (int axis = 0; axis < 3; ++axis) {
        if (clip[axis] < -w) {
            code |= 1u << (2 * axis);
        }
        if (clip[axis] > w) {
            code |= 1u << (2 * axis + 1);
        }
    }

    return code;
}

// A face's normal from its points by Newell's method, unnormalized.
PXR_NS::GfVec3d
getFaceNormal(const PXR_NS::VtVec3fArray& points,
              const PXR_NS::VtIntArray& faceVertexIndices,
              std::size_t first,
              int count)
{
    PXR_NS::GfVec3d normal(0.);

    for (int v = 0; v < count; ++v) {
        const PXR_NS::GfVec3d a(points[faceVertexIndices[first + v]]);
        const PXR_NS::GfVec3d b(points[faceVertexIndices[first + ((v + 1) % count)]]);
        normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
        normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
        normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }

    return normal;
}

std::string
joinPaths(const std::vector<std::string>& paths)
{
    std::string joined;

    for (const std::string& path : paths) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += path;
    }

    return joined;
}

const double kTwoPi = 2. * M_PI;
} // anonymous namespace

struct USDProjectUV::Settings {
    std::string pattern;
    std::string cameraPath;
    ProjectionEnum projection = eProjectionPerspective;
    PlaneEnum plane = ePlaneXY;
    ProjectOnEnum projectOn = eProjectOnBoth;
    bool frustumCulling = false;
    bool invertU = false;
    bool invertV = false;
    double uScale = 1.;
    double vScale = 1.;
    std::string primvarName;
    double aspect = 1.;
    HydraRender::FitModeEnum fit = HydraRender::eFitModeFill;
    bool useFrameHold = false;
    double frameHold = 1.;
};

namespace {
// Where the camera is at one frame: what the projection of a point needs of it.
struct CameraSample {
    // World to clip space, for Perspective (which also covers orthographic cameras).
    PXR_NS::GfMatrix4d viewProjection = PXR_NS::GfMatrix4d(1.);
    // World to the camera's space, and back, for the other projections.
    PXR_NS::GfMatrix4d worldToCamera = PXR_NS::GfMatrix4d(1.);
    PXR_NS::GfMatrix4d cameraToWorld = PXR_NS::GfMatrix4d(1.);
    PXR_NS::GfVec3d position = PXR_NS::GfVec3d(0.);
    bool orthographic = false;
    PXR_NS::GfVec3d viewDirection = PXR_NS::GfVec3d(0., 0., -1.);
};

// The UVs of one mesh: one sample, the default, or one per frame of the project range.
struct MeshUVs {
    PXR_NS::SdfPath path;
    std::vector<double> frames;
    std::vector<PXR_NS::VtVec2fArray> values;
    std::vector<PXR_NS::VtIntArray> indices;
};
} // anonymous namespace

struct USDProjectUV::Result {
    std::vector<MeshUVs> meshes;
    bool animated = false;
    bool matchedAny = false;
    std::vector<std::string> nonMeshGprims;
    std::vector<std::string> malformedMeshes;
    std::size_t pointsBehind = 0;
    U64 digest = 0;
};

struct USDProjectUV::Implementation {
    std::mutex lock;
    // The last result and what it was computed from, so an evaluation that changes nothing it
    // reads does not read the stages again.
    bool resultValid = false;
    U64 resultKey = 0;
    std::shared_ptr<const Result> result;
};

namespace {
/**
 * @brief The projection of the point at world position p (a mesh point) to (u, v), before the
 * inversion. behind is set for a Perspective point behind the camera, whose UV is clamped.
 **/
PXR_NS::GfVec2d
projectPoint(const PXR_NS::GfVec3d& p,
             const CameraSample& camera,
             USDProjectUV::ProjectionEnum projection,
             USDProjectUV::PlaneEnum plane,
             double uScale,
             double vScale,
             bool* behind,
             unsigned int* outcode)
{
    *behind = false;
    *outcode = 0;
    if (projection == USDProjectUV::eProjectionPerspective) {
        const PXR_NS::GfVec4d clip = PXR_NS::GfVec4d(p[0], p[1], p[2], 1.) * camera.viewProjection;
        *outcode = getOutcode(clip);
        const double w = clip[3];
        *behind = !(w > 0.);
        // A point behind the camera is divided by |w|, so it stays on its own side of the view
        // and clamps to the edge it is beyond, not to the opposite one.
        const double divisor = std::max(std::abs(w), 1e-12);
        PXR_NS::GfVec2d uv((clip[0] / divisor + 1.) * 0.5, (clip[1] / divisor + 1.) * 0.5);
        if (*behind) {
            uv[0] = std::clamp(uv[0], 0., 1.);
            uv[1] = std::clamp(uv[1], 0., 1.);
        }

        return uv;
    }
    const PXR_NS::GfVec3d q = camera.worldToCamera.Transform(p);
    switch (projection) {
    case USDProjectUV::eProjectionPlanar: {
        double a = q[0], b = q[1];
        if (plane == USDProjectUV::ePlaneYZ) {
            a = q[2];
            b = q[1];
        } else if (plane == USDProjectUV::ePlaneZX) {
            a = q[0];
            b = q[2];
        }

        return PXR_NS::GfVec2d(a / uScale + 0.5, b / vScale + 0.5);
    }
    case USDProjectUV::eProjectionSpherical: {
        const double length = q.GetLength();
        if (length <= 0.) {
            return PXR_NS::GfVec2d(0.5, 0.5);
        }
        const PXR_NS::GfVec3d n = q / length;
        const double phi = std::acos(std::clamp(-n[1], -1., 1.));
        double theta = -std::atan2(-n[0], n[2]);
        if (theta <= 0.) {
            theta += kTwoPi;
        }

        return PXR_NS::GfVec2d((theta / kTwoPi - 0.5) / uScale + 0.5, (phi / M_PI - 0.5) / vScale + 0.5);
    }
    case USDProjectUV::eProjectionCylindrical: {
        double theta = -std::atan2(-q[0], q[2]);
        if (theta <= 0.) {
            theta += kTwoPi;
        }

        return PXR_NS::GfVec2d((theta / kTwoPi - 0.5) / uScale + 0.5, q[1] * 0.5 / vScale + 0.5);
    }
    default:
        break;
    }

    return PXR_NS::GfVec2d(0., 0.);
} // projectPoint

// The direction from the world point p toward what projects onto it, for Project On.
PXR_NS::GfVec3d
getProjectorDirection(const PXR_NS::GfVec3d& p,
                      const CameraSample& camera,
                      USDProjectUV::ProjectionEnum projection,
                      USDProjectUV::PlaneEnum plane)
{
    switch (projection) {
    case USDProjectUV::eProjectionPerspective:
        return camera.orthographic ? -camera.viewDirection : camera.position - p;
    case USDProjectUV::eProjectionPlanar: {
        const PXR_NS::GfVec3d axis = (plane == USDProjectUV::ePlaneYZ) ? PXR_NS::GfVec3d(1., 0., 0.) : (plane == USDProjectUV::ePlaneZX) ? PXR_NS::GfVec3d(0., 1., 0.)
                                                                                                                                         : PXR_NS::GfVec3d(0., 0., 1.);

        return camera.cameraToWorld.TransformDir(axis);
    }
    case USDProjectUV::eProjectionCylindrical: {
        const PXR_NS::GfVec3d q = camera.worldToCamera.Transform(p);

        return camera.cameraToWorld.TransformDir(PXR_NS::GfVec3d(-q[0], 0., -q[2]));
    }
    default:
        return camera.position - p;
    }
}

/**
 * @brief The local normal of every face vertex of mesh at time: its authored normals
 * (primvars:normals before normals), else each face's geometric normal.
 **/
std::vector<PXR_NS::GfVec3f>
getFaceVertexNormals(const PXR_NS::UsdGeomMesh& mesh,
                     PXR_NS::UsdTimeCode time,
                     const PXR_NS::VtVec3fArray& points,
                     const PXR_NS::VtIntArray& counts,
                     const PXR_NS::VtIntArray& faceVertexIndices)
{
    std::vector<PXR_NS::GfVec3f> normals;
    const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(PXR_NS::TfToken("normals"));
    PXR_NS::VtVec3fArray authored;
    if (primvar && primvar.HasValue() && primvar.ComputeFlattened(&authored, time)
        && expandToFaceVertices(authored, primvar.GetInterpolation(), counts, faceVertexIndices, &normals)) {
        return normals;
    }
    const PXR_NS::UsdAttribute normalsAttr = mesh.GetNormalsAttr();
    if (normalsAttr && normalsAttr.HasValue() && normalsAttr.Get(&authored, time)
        && expandToFaceVertices(authored, mesh.GetNormalsInterpolation(), counts, faceVertexIndices, &normals)) {
        return normals;
    }
    PXR_NS::TfToken orientation;
    const bool leftHanded = mesh.GetOrientationAttr().Get(&orientation, time) && (orientation == PXR_NS::UsdGeomTokens->leftHanded);
    normals.clear();
    normals.reserve(faceVertexIndices.size());
    std::size_t first = 0;
    for (int count : counts) {
        PXR_NS::GfVec3d normal = getFaceNormal(points, faceVertexIndices, first, count);
        if (leftHanded) {
            normal = -normal;
        }
        for (int v = 0; v < count; ++v) {
            normals.push_back(PXR_NS::GfVec3f(normal));
        }
        first += (std::size_t)count;
    }

    return normals;
} // getFaceVertexNormals

/**
 * @brief The UVs the primvar named name already has on every face vertex of mesh at time, kept
 * where the projection does not reach. (0, 0) everywhere when it has none to keep.
 **/
std::vector<PXR_NS::GfVec2f>
getOriginalUVs(const PXR_NS::UsdGeomMesh& mesh,
               const PXR_NS::TfToken& name,
               PXR_NS::UsdTimeCode time,
               const PXR_NS::VtIntArray& counts,
               const PXR_NS::VtIntArray& faceVertexIndices)
{
    std::vector<PXR_NS::GfVec2f> uvs;
    const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(name);
    PXR_NS::VtVec2fArray values;
    if (primvar && primvar.HasValue() && (primvar.GetElementSize() == 1) && primvar.ComputeFlattened(&values, time)
        && expandToFaceVertices(values, primvar.GetInterpolation(), counts, faceVertexIndices, &uvs)) {
        return uvs;
    }
    uvs.assign(faceVertexIndices.size(), PXR_NS::GfVec2f(0.f));

    return uvs;
}
} // anonymous namespace

USDProjectUV::USDProjectUV(NodePtr node)
    : NativeEffectBase(node)
    , _imp(new Implementation())
    , _cameraMenuRefreshPending(false)
{
}

USDProjectUV::~USDProjectUV()
{
}

const NodeSpec*
USDProjectUV::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDProjectUV::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr camera = createKnob<KnobChoice>(tr("Camera"));
    camera->setName(kUSDProjectUVParamCamera);
    camera->setHintToolTip(tr("The camera to project through: one of the cameras of the Camera input's scene, or of the "
                              "Scene input's when the Camera input is not connected, by path. Until one is chosen the "
                              "first camera is used. A chosen camera the scene no longer has is an error naming its path."));
    camera->setAnimationEnabled(false);
    page->addKnob(camera);
    _camera = camera;

    KnobStringPtr primPattern = createKnob<KnobString>(tr("Prim Pattern"));
    primPattern->setName(kUSDProjectUVParamPrimPattern);
    primPattern->setHintToolTip(tr("The meshes to project onto: absolute prim paths separated by spaces, whose names may use * "
                                   "(any run of characters) and ? (any one character), such as /World/Geo/*. Matched prims "
                                   "that are not geometry are skipped; matched geometry that is not a mesh is named in a "
                                   "warning."));
    primPattern->setAnimationEnabled(false);
    page->addKnob(primPattern);
    _primPattern = primPattern;

    KnobChoicePtr projection = createKnob<KnobChoice>(tr("Projection"));
    projection->setName(kUSDProjectUVParamProjection);
    projection->setHintToolTip(tr("How points map to UVs. Pick the shape closest to the surface's."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("off", tr("Off").toStdString(),
                                       tr("The scene passes through unchanged.").toStdString()));
        options.push_back(ChoiceOption("perspective", tr("Perspective").toStdString(),
                                       tr("Where the point lands in the camera's aperture, conformed to the Format by Fit Mode: "
                                          "(0, 0) at the bottom left, (1, 1) at the top right.")
                                           .toStdString()));
        options.push_back(ChoiceOption("planar", tr("Planar").toStdString(),
                                       tr("The point's position on a plane of the camera's space, one scene unit across "
                                          "per U Scale and V Scale, centred on the camera.")
                                           .toStdString()));
        options.push_back(ChoiceOption("spherical", tr("Spherical").toStdString(),
                                       tr("The point's longitude and latitude about the camera: U turns once around the "
                                          "camera's Y axis, with 0.5 straight ahead, and V runs from bottom to top.")
                                           .toStdString()));
        options.push_back(ChoiceOption("cylindrical", tr("Cylindrical").toStdString(),
                                       tr("The point's longitude about the camera's Y axis, as Spherical, and its height "
                                          "along it: V runs from 0 to 1 over two scene units.")
                                           .toStdString()));
        projection->populateChoices(options);
    }
    projection->setDefaultValue((int)eProjectionPerspective);
    projection->setAnimationEnabled(false);
    page->addKnob(projection);
    _projection = projection;

    KnobChoicePtr plane = createKnob<KnobChoice>(tr("Plane"));
    plane->setName(kUSDProjectUVParamPlane);
    plane->setHintToolTip(tr("For Planar: the plane of the camera's space the points are projected onto, U along its first "
                             "axis and V along its second."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("xy", tr("XY").toStdString(), ""));
        options.push_back(ChoiceOption("yz", tr("YZ").toStdString(), ""));
        options.push_back(ChoiceOption("zx", tr("ZX").toStdString(), ""));
        plane->populateChoices(options);
    }
    plane->setDefaultValue((int)ePlaneXY);
    plane->setAnimationEnabled(false);
    page->addKnob(plane);
    _plane = plane;

    KnobChoicePtr projectOn = createKnob<KnobChoice>(tr("Project On"));
    projectOn->setName(kUSDProjectUVParamProjectOn);
    projectOn->setHintToolTip(tr("Which faces the projection falls on, by whether their normal faces the camera. A face vertex "
                                 "the projection does not fall on keeps the UV the mesh already has in the primvar, or "
                                 "(0, 0) if it has none."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("both", tr("Both").toStdString(), tr("Every face.").toStdString()));
        options.push_back(ChoiceOption("front", tr("Front").toStdString(), tr("Faces facing the camera.").toStdString()));
        options.push_back(ChoiceOption("back", tr("Back").toStdString(), tr("Faces facing away from the camera.").toStdString()));
        projectOn->populateChoices(options);
    }
    projectOn->setDefaultValue((int)eProjectOnBoth);
    projectOn->setAnimationEnabled(false);
    page->addKnob(projectOn);
    _projectOn = projectOn;

    KnobBoolPtr frustumCulling = createKnob<KnobBool>(tr("Frustum Culling"));
    frustumCulling->setName(kUSDProjectUVParamFrustumCulling);
    frustumCulling->setHintToolTip(tr("For Perspective: faces wholly outside the camera's view, its near and far clipping planes "
                                      "included, keep the UVs the mesh already has, as Project On does."));
    frustumCulling->setDefaultValue(false);
    frustumCulling->setAnimationEnabled(false);
    page->addKnob(frustumCulling);
    _frustumCulling = frustumCulling;

    KnobBoolPtr invertU = createKnob<KnobBool>(tr("Invert U"));
    invertU->setName(kUSDProjectUVParamInvertU);
    invertU->setHintToolTip(tr("Flips U: 1 - U."));
    invertU->setDefaultValue(false);
    invertU->setAnimationEnabled(false);
    page->addKnob(invertU);
    _invertU = invertU;

    KnobBoolPtr invertV = createKnob<KnobBool>(tr("Invert V"));
    invertV->setName(kUSDProjectUVParamInvertV);
    invertV->setHintToolTip(tr("Flips V: 1 - V."));
    invertV->setDefaultValue(false);
    invertV->setAnimationEnabled(false);
    page->addKnob(invertV);
    _invertV = invertV;

    KnobDoublePtr uScale = createKnob<KnobDouble>(tr("U Scale"));
    uScale->setName(kUSDProjectUVParamUScale);
    uScale->setHintToolTip(tr("For Planar, Spherical and Cylindrical: stretches the texture along U about its centre; 2 makes "
                              "it twice as wide."));
    uScale->setDefaultValue(1.);
    uScale->setMinimum(1e-6);
    uScale->setDisplayMinimum(0.);
    uScale->setDisplayMaximum(2.);
    uScale->setAnimationEnabled(false);
    page->addKnob(uScale);
    _uScale = uScale;

    KnobDoublePtr vScale = createKnob<KnobDouble>(tr("V Scale"));
    vScale->setName(kUSDProjectUVParamVScale);
    vScale->setHintToolTip(tr("For Planar, Spherical and Cylindrical: stretches the texture along V about its centre; 2 makes "
                              "it twice as tall."));
    vScale->setDefaultValue(1.);
    vScale->setMinimum(1e-6);
    vScale->setDisplayMinimum(0.);
    vScale->setDisplayMaximum(2.);
    vScale->setAnimationEnabled(false);
    page->addKnob(vScale);
    _vScale = vScale;

    KnobStringPtr primvarName = createKnob<KnobString>(tr("Primvar Name"));
    primvarName->setName(kUSDProjectUVParamPrimvarName);
    primvarName->setHintToolTip(tr("The texture coordinate primvar to write, without its primvars: prefix. st is the one "
                                   "UsdPreviewSurface textures read by default."));
    primvarName->setDefaultValue("st");
    primvarName->setAnimationEnabled(false);
    page->addKnob(primvarName);
    _primvarName = primvarName;

    const ExtentKnobs::FormatKnobSet formatKnobs = ExtentKnobs::createFormatKnobs(this, page);
    formatKnobs.choice->setHintToolTip(tr("For Perspective: the format of the image the UVs will texture, which the camera's "
                                          "aperture is conformed to by Fit Mode."));
    _format = formatKnobs.choice;
    _formatSize = formatKnobs.size;
    _formatPar = formatKnobs.par;

    KnobChoicePtr fitMode = createKnob<KnobChoice>(tr("Fit Mode"));
    fitMode->setName(kUSDProjectUVParamFitMode);
    fitMode->setHintToolTip(tr("For Perspective: how the camera's aperture is conformed to the Format when their shapes "
                               "differ, as in USDRender."));
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
    fitMode->setDefaultValue((int)HydraRender::eFitModeFill);
    fitMode->setAnimationEnabled(false);
    page->addKnob(fitMode);
    _fitMode = fitMode;

    KnobBoolPtr useFrameHold = createKnob<KnobBool>(tr("Use Frame Hold"));
    useFrameHold->setName(kUSDProjectUVParamUseFrameHold);
    useFrameHold->setHintToolTip(tr("Projects through the camera as it is at Frame Hold on every frame, so a moving camera "
                                    "leaves the projection where it was then. Meshes still move."));
    useFrameHold->setDefaultValue(false);
    useFrameHold->setAnimationEnabled(false);
    page->addKnob(useFrameHold);
    _useFrameHold = useFrameHold;

    KnobDoublePtr frameHold = createKnob<KnobDouble>(tr("Frame Hold"));
    frameHold->setName(kUSDProjectUVParamFrameHold);
    frameHold->setHintToolTip(tr("The frame the camera is held at when Use Frame Hold is on."));
    frameHold->setDefaultValue(1.);
    frameHold->setAnimationEnabled(false);
    page->addKnob(frameHold);
    _frameHold = frameHold;

    refreshProjectionKnobs();
} // USDProjectUV::initializeKnobs

void
USDProjectUV::refreshProjectionKnobs()
{
    KnobChoicePtr projectionKnob = _projection.lock();
    KnobBoolPtr useFrameHold = _useFrameHold.lock();
    if (!projectionKnob || !useFrameHold) {
        return;
    }
    const int projection = projectionKnob->getValue();
    const bool perspective = projection == (int)eProjectionPerspective;
    const bool mapped = projection > (int)eProjectionPerspective;
    auto enable = [](const KnobIPtr& knob, bool enabled) {
        if (knob) {
            knob->setAllDimensionsEnabled(enabled);
        }
    };
    enable(_plane.lock(), projection == (int)eProjectionPlanar);
    enable(_uScale.lock(), mapped);
    enable(_vScale.lock(), mapped);
    enable(_frustumCulling.lock(), perspective);
    enable(_format.lock(), perspective);
    enable(_fitMode.lock(), perspective);
    enable(_frameHold.lock(), useFrameHold->getValue());
}

void
USDProjectUV::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    refreshProjectionKnobs();
    scheduleCameraMenuRefresh();
}

void
USDProjectUV::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    scheduleCameraMenuRefresh();
}

void
USDProjectUV::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleCameraMenuRefresh();
}

bool
USDProjectUV::knobChanged(KnobI* k,
                          ValueChangedReasonEnum reason,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    KnobChoicePtr projection = _projection.lock();
    KnobBoolPtr useFrameHold = _useFrameHold.lock();
    if ((projection && (k == projection.get())) || (useFrameHold && (k == useFrameHold.get()))) {
        refreshProjectionKnobs();

        return true;
    }

    return false;
}

void
USDProjectUV::scheduleCameraMenuRefresh()
{
    if (_cameraMenuRefreshPending.exchange(true)) {
        return;
    }
    // The hooks that call this fire mid-recompute of a hash, before the upstream scene settled.
    QMetaObject::invokeMethod(this, [this]() {
        _cameraMenuRefreshPending = false;
        refreshCameraMenu(); }, Qt::QueuedConnection);
}

void
USDProjectUV::refreshCameraMenu()
{
    KnobChoicePtr camera = _camera.lock();

    if (!camera) {
        return;
    }
    std::vector<ChoiceOption> options;
    EffectInstancePtr input = getInput(1);
    if (!input) {
        input = getInput(0);
    }
    ScenePayload scene;
    std::string error;
    if (input && (input->evaluateScene(EvaluateSceneArgs(getCurrentTime(), ViewIdx(0)), &scene, &error) == eRenderRoIRetCodeOk) && !scene.isEmpty()) {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, &error);
        if (lease.isValid()) {
            for (const PXR_NS::SdfPath& path : HydraRender::cameraPaths(lease.getStage())) {
                options.push_back(ChoiceOption(path.GetString(), path.GetString(), ""));
            }
        }
    }
    // Repopulating an unchanged menu would move the hash and come back here.
    const std::vector<ChoiceOption> current = camera->getEntries_mt_safe();
    bool same = current.size() == options.size();
    for (std::size_t i = 0; same && (i < options.size()); ++i) {
        same = current[i].id == options[i].id;
    }
    if (!same) {
        camera->populateChoices(options);
    }
}

double
USDProjectUV::getFormatAspect() const
{
    KnobIntPtr size = _formatSize.lock();
    KnobDoublePtr par = _formatPar.lock();

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

StatusEnum
USDProjectUV::renderScene(const SceneRenderActionArgs& args,
                          ScenePayload* outputScene,
                          std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    const bool cameraInput = bool(getInput(1));
    const ScenePayload cameraScene = cameraInput ? args.getInputScene(1) : upstream;

    KnobStringPtr patternKnob = _primPattern.lock();
    KnobChoicePtr cameraKnob = _camera.lock();
    KnobChoicePtr projectionKnob = _projection.lock();
    KnobChoicePtr planeKnob = _plane.lock();
    KnobChoicePtr projectOnKnob = _projectOn.lock();
    KnobBoolPtr frustumCullingKnob = _frustumCulling.lock();
    KnobBoolPtr invertUKnob = _invertU.lock();
    KnobBoolPtr invertVKnob = _invertV.lock();
    KnobDoublePtr uScaleKnob = _uScale.lock();
    KnobDoublePtr vScaleKnob = _vScale.lock();
    KnobStringPtr primvarKnob = _primvarName.lock();
    KnobChoicePtr fitKnob = _fitMode.lock();
    KnobBoolPtr useFrameHoldKnob = _useFrameHold.lock();
    KnobDoublePtr frameHoldKnob = _frameHold.lock();
    if (!patternKnob || !cameraKnob || !projectionKnob || !planeKnob || !projectOnKnob || !frustumCullingKnob || !invertUKnob || !invertVKnob
        || !uScaleKnob || !vScaleKnob || !primvarKnob || !fitKnob || !useFrameHoldKnob || !frameHoldKnob) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return eStatusFailed;
    }

    Settings settings;
    settings.pattern = patternKnob->getValue();
    settings.cameraPath = cameraKnob->getActiveEntry().id;
    settings.projection = (ProjectionEnum)std::clamp(projectionKnob->getValue(), (int)eProjectionOff, (int)eProjectionCylindrical);
    settings.plane = (PlaneEnum)std::clamp(planeKnob->getValue(), (int)ePlaneXY, (int)ePlaneZX);
    settings.projectOn = (ProjectOnEnum)std::clamp(projectOnKnob->getValue(), (int)eProjectOnBoth, (int)eProjectOnBack);
    settings.frustumCulling = frustumCullingKnob->getValue();
    settings.invertU = invertUKnob->getValue();
    settings.invertV = invertVKnob->getValue();
    settings.uScale = std::max(uScaleKnob->getValue(), 1e-6);
    settings.vScale = std::max(vScaleKnob->getValue(), 1e-6);
    settings.primvarName = primvarKnob->getValue();
    settings.aspect = getFormatAspect();
    const int fitValue = fitKnob->getValue();
    settings.fit = (fitValue == (int)HydraRender::eFitModeHorizontal) ? HydraRender::eFitModeHorizontal : (fitValue == (int)HydraRender::eFitModeVertical) ? HydraRender::eFitModeVertical
                                                                                                                                                           : HydraRender::eFitModeFill;
    settings.useFrameHold = useFrameHoldKnob->getValue();
    settings.frameHold = frameHoldKnob->getValue();

    if ((settings.projection == eProjectionOff) || upstream.isEmpty() || isBlank(settings.pattern)) {
        *outputScene = upstream;
        clearPersistentMessage(false);

        return eStatusOK;
    }
    if (!PXR_NS::SdfPath::IsValidNamespacedIdentifier(settings.primvarName)) {
        *errorMessage = tr("\"%1\" is not a valid primvar name.").arg(QString::fromStdString(settings.primvarName)).toStdString();
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    SceneLayerMemo::Key layerKey;
    if (!getSceneLayerKey(SceneLayerReads(), &layerKey, errorMessage)) {
        return eStatusFailed;
    }
    ContentHasher keyHasher;
    keyHasher.appendU64(layerKey.nodeDigest);
    keyHasher.appendDouble(layerKey.firstFrame);
    keyHasher.appendDouble(layerKey.lastFrame);
    keyHasher.appendDouble(layerKey.framesPerSecond);
    keyHasher.appendU64(upstream.getStackHash());
    keyHasher.appendBool(cameraInput);
    keyHasher.appendU64(cameraScene.getStackHash());
    keyHasher.appendString(settings.cameraPath);
    keyHasher.appendDouble(settings.aspect);
    const U64 resultKey = keyHasher.finalize();

    std::shared_ptr<const Result> result;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        if (_imp->resultValid && (_imp->resultKey == resultKey)) {
            result = _imp->result;
        }
    }

    if (!result) {
        std::shared_ptr<Result> computed = std::make_shared<Result>();
        const double fps = layerKey.framesPerSecond;
        SceneLayerEdit range;
        range.firstFrame = layerKey.firstFrame;
        range.lastFrame = layerKey.lastFrame;
        const std::vector<double> frames = SceneNodeAuthoring::getSampleFrames(range);

        // The camera at every frame, or once when it holds still. Read first, and given back
        // before the scene is borrowed.
        std::vector<CameraSample> cameraSamples(1);
        bool cameraVaries = false;
        {
            SceneQueryPool::Lease lease;
            PXR_NS::UsdStageRefPtr stage;
            if (!cameraScene.isEmpty()) {
                lease = SceneQueryPool::instance().borrow(cameraScene, errorMessage);
                stage = lease.getStage();
                if (!stage) {
                    if (errorMessage->empty()) {
                        *errorMessage = tr("USD could not compose the camera scene.").toStdString();
                    }
                    setPersistentMessage(eMessageTypeError, *errorMessage);

                    return eStatusFailed;
                }
            }
            std::string cameraPath = settings.cameraPath;
            if (cameraPath.empty() && stage) {
                const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
                if (!cameras.empty()) {
                    cameraPath = cameras.front().GetString();
                }
            }
            if (cameraPath.empty()) {
                // Without a camera the other projections map in world space, as Nuke's do.
                if (settings.projection == eProjectionPerspective) {
                    *errorMessage = (cameraInput ? tr("The Camera input has no camera to project through.")
                                                 : tr("The scene has no camera to project through: connect one to the Camera input."))
                                        .toStdString();
                    setPersistentMessage(eMessageTypeError, *errorMessage);

                    return eStatusFailed;
                }
            } else {
                if (!PXR_NS::SdfPath::IsValidPathString(cameraPath) || !stage) {
                    *errorMessage = tr("No camera at %1 on the camera scene.").arg(QString::fromStdString(cameraPath)).toStdString();
                    setPersistentMessage(eMessageTypeError, *errorMessage);

                    return eStatusFailed;
                }
                const PXR_NS::SdfPath path(cameraPath);
                const PXR_NS::UsdGeomCamera camera(stage->GetPrimAtPath(path));
                cameraVaries = !settings.useFrameHold && camera && cameraMightVary(camera);
                std::vector<double> cameraFrames;
                if (settings.useFrameHold) {
                    cameraFrames.push_back(settings.frameHold);
                } else if (cameraVaries) {
                    cameraFrames = frames;
                } else {
                    cameraFrames.push_back(frames.front());
                }
                cameraSamples.clear();
                for (double frame : cameraFrames) {
                    const PXR_NS::UsdTimeCode time(SceneOps::frameToTimeCode(cameraScene, frame, fps));
                    PXR_NS::GfFrustum frustum;
                    if (!HydraRender::cameraFrustum(stage, path, time, settings.aspect, settings.fit, &frustum, errorMessage)) {
                        setPersistentMessage(eMessageTypeError, *errorMessage);

                        return eStatusFailed;
                    }
                    CameraSample sample;
                    sample.worldToCamera = frustum.ComputeViewMatrix();
                    sample.cameraToWorld = sample.worldToCamera.GetInverse();
                    sample.viewProjection = sample.worldToCamera * frustum.ComputeProjectionMatrix();
                    sample.position = frustum.GetPosition();
                    sample.orthographic = frustum.GetProjectionType() == PXR_NS::GfFrustum::Orthographic;
                    sample.viewDirection = frustum.ComputeViewDirection();
                    cameraSamples.push_back(sample);
                }
            }
        }

        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(upstream, errorMessage);
        PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        std::vector<PXR_NS::SdfPath> found;
        if (!SceneNodeAuthoring::matchPrims(stage, settings.pattern, &found, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        computed->matchedAny = !found.empty();
        const PXR_NS::TfToken primvarName(settings.primvarName);
        const bool filterFaces = (settings.projectOn != eProjectOnBoth) || (settings.frustumCulling && (settings.projection == eProjectionPerspective));

        for (const PXR_NS::SdfPath& path : found) {
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            const PXR_NS::UsdGeomMesh mesh(prim);
            if (!mesh) {
                if (prim.IsA<PXR_NS::UsdGeomGprim>()) {
                    computed->nonMeshGprims.push_back(path.GetString());
                }
                continue;
            }
            bool meshVaries = transformMightVary(prim)
                || attributeMightVary(mesh.GetPointsAttr())
                || attributeMightVary(mesh.GetFaceVertexCountsAttr())
                || attributeMightVary(mesh.GetFaceVertexIndicesAttr());
            if (!meshVaries && (settings.projectOn != eProjectOnBoth)) {
                const PXR_NS::UsdGeomPrimvar normals = PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(PXR_NS::TfToken("normals"));
                meshVaries = attributeMightVary(mesh.GetNormalsAttr()) || (normals && (attributeMightVary(normals.GetAttr()) || attributeMightVary(normals.GetIndicesAttr())));
            }
            if (!meshVaries && filterFaces) {
                const PXR_NS::UsdGeomPrimvar original = PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(primvarName);
                meshVaries = original && (attributeMightVary(original.GetAttr()) || attributeMightVary(original.GetIndicesAttr()));
            }

            MeshUVs uvs;
            uvs.path = path;
            const bool animated = meshVaries || cameraVaries;
            const std::vector<double> meshFrames = animated ? frames : std::vector<double>(1, frames.front());
            std::vector<bool> behind;
            bool malformed = false;
            for (std::size_t f = 0; f < meshFrames.size(); ++f) {
                const double frame = meshFrames[f];
                const PXR_NS::UsdTimeCode time(SceneOps::frameToTimeCode(upstream, frame, fps));
                const CameraSample& camera = cameraSamples[(cameraSamples.size() == 1) ? 0 : std::min(f, cameraSamples.size() - 1)];
                PXR_NS::VtVec3fArray points;
                PXR_NS::VtIntArray counts;
                PXR_NS::VtIntArray faceVertexIndices;
                mesh.GetPointsAttr().Get(&points, time);
                mesh.GetFaceVertexCountsAttr().Get(&counts, time);
                mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, time);
                std::size_t total = 0;
                for (int count : counts) {
                    malformed = malformed || (count < 3);
                    total += (std::size_t)std::max(count, 0);
                }
                malformed = malformed || (total != faceVertexIndices.size());
                for (int index : faceVertexIndices) {
                    malformed = malformed || (index < 0) || ((std::size_t)index >= points.size());
                }
                if (malformed) {
                    break;
                }

                const PXR_NS::GfMatrix4d toWorld = PXR_NS::UsdGeomImageable(prim).ComputeLocalToWorldTransform(time);
                std::vector<PXR_NS::GfVec3d> world(points.size());
                PXR_NS::VtVec2fArray values(points.size());
                std::vector<bool> pointBehind(points.size(), false);
                std::vector<unsigned int> outcodes(points.size(), 0);
                for (std::size_t i = 0; i < points.size(); ++i) {
                    world[i] = toWorld.Transform(PXR_NS::GfVec3d(points[i]));
                    bool isBehind = false;
                    unsigned int outcode = 0;
                    PXR_NS::GfVec2d uv = projectPoint(world[i], camera, settings.projection, settings.plane, settings.uScale, settings.vScale, &isBehind, &outcode);
                    if (settings.invertU) {
                        uv[0] = 1. - uv[0];
                    }
                    if (settings.invertV) {
                        uv[1] = 1. - uv[1];
                    }
                    values[i] = PXR_NS::GfVec2f(uv);
                    pointBehind[i] = isBehind;
                    outcodes[i] = outcode;
                }

                // Which face vertices the projection falls on; the others keep the mesh's UVs.
                std::vector<bool> keep(faceVertexIndices.size(), false);
                if (filterFaces) {
                    std::vector<PXR_NS::GfVec3f> normals;
                    PXR_NS::GfMatrix4d normalToWorld;
                    if (settings.projectOn != eProjectOnBoth) {
                        normals = getFaceVertexNormals(mesh, time, points, counts, faceVertexIndices);
                        normalToWorld = toWorld.GetInverse().GetTranspose();
                    }
                    std::size_t first = 0;
                    for (int count : counts) {
                        bool culled = false;
                        if (settings.frustumCulling && (settings.projection == eProjectionPerspective)) {
                            unsigned int common = ~0u;
                            for (int v = 0; v < count; ++v) {
                                common &= outcodes[faceVertexIndices[first + v]];
                            }
                            culled = common != 0;
                        }
                        for (int v = 0; v < count; ++v) {
                            const std::size_t fv = first + v;
                            bool kept = culled;
                            if (!kept && (settings.projectOn != eProjectOnBoth)) {
                                const int point = faceVertexIndices[fv];
                                const PXR_NS::GfVec3d n = normalToWorld.TransformDir(PXR_NS::GfVec3d(normals[fv]));
                                const double facing = PXR_NS::GfDot(n, getProjectorDirection(world[point], camera, settings.projection, settings.plane));
                                kept = (settings.projectOn == eProjectOnFront) ? (facing < 0.) : (facing > 0.);
                            }
                            keep[fv] = kept;
                        }
                        first += (std::size_t)count;
                    }
                }

                PXR_NS::VtIntArray indices(faceVertexIndices.size());
                std::vector<PXR_NS::GfVec2f> originals;
                if (behind.size() != points.size()) {
                    behind.assign(points.size(), false);
                }
                for (std::size_t fv = 0; fv < faceVertexIndices.size(); ++fv) {
                    const int point = faceVertexIndices[fv];
                    if (!keep[fv]) {
                        indices[fv] = point;
                        if (pointBehind[point]) {
                            behind[point] = true;
                        }
                        continue;
                    }
                    if (originals.empty()) {
                        originals = getOriginalUVs(mesh, primvarName, time, counts, faceVertexIndices);
                    }
                    indices[fv] = (int)values.size();
                    values.push_back(originals[fv]);
                }
                if (animated) {
                    uvs.frames.push_back(frame);
                }
                uvs.values.push_back(values);
                uvs.indices.push_back(indices);
            }
            if (malformed) {
                computed->malformedMeshes.push_back(path.GetString());
                continue;
            }
            computed->pointsBehind += (std::size_t)std::count(behind.begin(), behind.end(), true);
            computed->animated = computed->animated || animated;
            computed->meshes.push_back(std::move(uvs));
        }
        lease.release();

        ContentHasher digest;
        SceneNodeAuthoring::appendUpstreamMetadata(upstream, &digest);
        digest.appendString(settings.primvarName);
        digest.appendU64(computed->meshes.size());
        for (const MeshUVs& uvs : computed->meshes) {
            digest.appendString(uvs.path.GetString());
            digest.appendU64(uvs.frames.size());
            for (double frame : uvs.frames) {
                digest.appendDouble(frame);
            }
            for (std::size_t s = 0; s < uvs.values.size(); ++s) {
                digest.appendU64(uvs.values[s].size());
                digest.appendBytes(uvs.values[s].cdata(), uvs.values[s].size() * sizeof(PXR_NS::GfVec2f));
                digest.appendU64(uvs.indices[s].size());
                digest.appendBytes(uvs.indices[s].cdata(), uvs.indices[s].size() * sizeof(int));
            }
        }
        computed->digest = digest.finalize();

        result = computed;
        std::lock_guard<std::mutex> l(_imp->lock);
        _imp->resultValid = true;
        _imp->resultKey = resultKey;
        _imp->result = result;
    }

    QStringList warnings;
    if (!result->matchedAny) {
        warnings.push_back(tr("The prim pattern \"%1\" matches no prim; the scene is passed through unchanged.")
                               .arg(QString::fromStdString(settings.pattern)));
    }
    if (!result->nonMeshGprims.empty()) {
        warnings.push_back(tr("These matched prims are not meshes and get no UVs: %1")
                               .arg(QString::fromStdString(joinPaths(result->nonMeshGprims))));
    }
    if (!result->malformedMeshes.empty()) {
        warnings.push_back(tr("These meshes have invalid topology and get no UVs: %1")
                               .arg(QString::fromStdString(joinPaths(result->malformedMeshes))));
    }
    if (result->pointsBehind > 0) {
        warnings.push_back(tr("%n point(s) lie behind the camera; their UVs are clamped to the edge of the frame.", "", (int)result->pointsBehind));
    }

    if (result->meshes.empty()) {
        *outputScene = upstream;
    } else {
        SceneLayerReads reads;
        reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
        reads.factsDigest = result->digest;
        reads.frameRange = result->animated;
        const std::string primvarName = settings.primvarName;
        SceneLayerMemo::AuthorFunc author = [result, primvarName](const SceneLayerEdit& edit,
                                                                  std::string* error) {
            PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
            if (!stage) {
                if (error->empty()) {
                    *error = tr("USD could not open the node's layer.").toStdString();
                }

                return false;
            }
            const PXR_NS::TfToken name(primvarName);
            for (const MeshUVs& uvs : result->meshes) {
                const PXR_NS::UsdPrim prim = stage->OverridePrim(uvs.path);
                const PXR_NS::UsdGeomPrimvar primvar = prim ? PXR_NS::UsdGeomPrimvarsAPI(prim).CreatePrimvar(name, PXR_NS::SdfValueTypeNames->TexCoord2fArray, PXR_NS::UsdGeomTokens->faceVarying, 1) : PXR_NS::UsdGeomPrimvar();
                bool authored = bool(primvar);
                if (authored && uvs.frames.empty()) {
                    authored = primvar.Set(uvs.values.front()) && primvar.SetIndices(uvs.indices.front());
                } else {
                    for (std::size_t s = 0; authored && (s < uvs.frames.size()); ++s) {
                        const PXR_NS::UsdTimeCode time = SceneNodeAuthoring::frameToTimeCode(edit, uvs.frames[s]);
                        authored = primvar.Set(uvs.values[s], time) && primvar.SetIndices(uvs.indices[s], time);
                    }
                }
                if (!authored) {
                    *error = tr("USD could not author the UVs of %1.").arg(QString::fromStdString(uvs.path.GetString())).toStdString();

                    return false;
                }
            }

            return true;
        };
        if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
    }

    if (warnings.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());
    }

    return eStatusOK;
} // USDProjectUV::renderScene

NATRON_NAMESPACE_EXIT
