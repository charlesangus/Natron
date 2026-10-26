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

#include "SceneNodeAuthoring.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"

#include <pxr/usd/usd/primRange.h>

#include <QDir>
#include <QString>

NATRON_NAMESPACE_ENTER

std::string
SceneNodeAuthoring::getAnchoredFilePath(const KnobFilePtr& knob)
{
    if (!knob) {
        return std::string();
    }
    const std::string path = knob->getResolvedPath();
    const QString qpath = QString::fromStdString(path);
    // A URI an asset resolver handles is kept as it is.
    const int colon = qpath.indexOf(QLatin1Char(':'));
    const int slash = qpath.indexOf(QLatin1Char('/'));
    const bool uri = (colon > 1) && ((slash < 0) || (colon < slash));
    if (path.empty() || uri || !QDir::isRelativePath(qpath)) {
        return path;
    }

    return QDir::cleanPath(QDir::current().absoluteFilePath(qpath)).toStdString();
}

void
setSceneKnobSecret(const KnobIPtr& knob,
                   bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

void
SceneTransformKnobs::create(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    KnobDoublePtr translateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Translate"), 3);
    translateKnob->setName(kSceneParamTranslate);
    translateKnob->setHintToolTip(EffectInstance::tr("The prim's position in its parent's space, in scene units."));
    for (int d = 0; d < 3; ++d) {
        translateKnob->setDefaultValue(0., d);
    }
    page->addKnob(translateKnob);
    translate = translateKnob;

    KnobDoublePtr rotateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Rotate"), 3);
    rotateKnob->setName(kSceneParamRotate);
    rotateKnob->setHintToolTip(EffectInstance::tr("Rotations about the X, Y and Z axes, in degrees, applied in the rotate order."));
    for (int d = 0; d < 3; ++d) {
        rotateKnob->setDefaultValue(0., d);
    }
    page->addKnob(rotateKnob);
    rotate = rotateKnob;

    KnobDoublePtr scaleKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Scale"), 3);
    scaleKnob->setName(kSceneParamScale);
    scaleKnob->setHintToolTip(EffectInstance::tr("Scale along the X, Y and Z axes, multiplied by the uniform scale."));
    for (int d = 0; d < 3; ++d) {
        scaleKnob->setDefaultValue(1., d);
    }
    page->addKnob(scaleKnob);
    scale = scaleKnob;

    KnobDoublePtr uniformScaleKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Uniform Scale"));
    uniformScaleKnob->setName(kSceneParamUniformScale);
    uniformScaleKnob->setHintToolTip(EffectInstance::tr("Scale along every axis, multiplied into the scale."));
    uniformScaleKnob->setDefaultValue(1.);
    page->addKnob(uniformScaleKnob);
    uniformScale = uniformScaleKnob;

    KnobChoicePtr rotateOrderKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Rotate Order"));
    rotateOrderKnob->setName(kSceneParamRotateOrder);
    rotateOrderKnob->setHintToolTip(EffectInstance::tr("The order the three rotations are applied in, as USD names its rotate ops: "
                                                       "XYZ turns about X first, then Y, then Z."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("XYZ", "XYZ", ""));
        options.push_back(ChoiceOption("XZY", "XZY", ""));
        options.push_back(ChoiceOption("YXZ", "YXZ", ""));
        options.push_back(ChoiceOption("YZX", "YZX", ""));
        options.push_back(ChoiceOption("ZXY", "ZXY", ""));
        options.push_back(ChoiceOption("ZYX", "ZYX", ""));
        rotateOrderKnob->populateChoices(options);
    }
    rotateOrderKnob->setDefaultValue((int)eSceneRotateOrderXYZ);
    rotateOrderKnob->setAnimationEnabled(false);
    page->addKnob(rotateOrderKnob);
    rotateOrder = rotateOrderKnob;
} // SceneTransformKnobs::create

void
SceneTransformKnobs::setSecret(bool secret) const
{
    setSceneKnobSecret(translate.lock(), secret);
    setSceneKnobSecret(rotate.lock(), secret);
    setSceneKnobSecret(scale.lock(), secret);
    setSceneKnobSecret(uniformScale.lock(), secret);
    setSceneKnobSecret(rotateOrder.lock(), secret);
}

KnobStringPtr
createScenePrimPathKnob(KnobHolder* holder,
                        const KnobPagePtr& page,
                        const std::string& defaultPath,
                        const QString& toolTip)
{
    KnobStringPtr knob = AppManager::createKnob<KnobString>(holder, EffectInstance::tr("Prim Path"));

    knob->setName(kSceneParamPrimPath);
    knob->setHintToolTip(toolTip);
    knob->setAnimationEnabled(false);
    knob->setDefaultValue(defaultPath);
    page->addKnob(knob);

    return knob;
}

std::string
getSceneNodeDefaultPrimPath(const EffectInstance* effect,
                            const std::string& fallbackName)
{
    const std::string scriptName = effect ? effect->getScriptName_mt_safe() : std::string();

    return "/" + (scriptName.empty() ? fallbackName : scriptName);
}

void
followSceneNodeRename(const KnobStringPtr& knob,
                      const std::string& newDefaultPath)
{
    if (!knob) {
        return;
    }
    const std::string oldDefault = knob->getDefaultValue(0);
    if (oldDefault == newDefaultPath) {
        return;
    }
    if (knob->getValue() == oldDefault) {
        knob->setDefaultValue(newDefaultPath);
    } else {
        knob->setDefaultValueWithoutApplying(newDefaultPath);
    }
}

namespace SceneNodeAuthoring {
double
getTimeCodesPerSecond(const SceneLayerEdit& edit)
{
    if (edit.upstream.isEmpty()) {
        return (edit.framesPerSecond > 0.) ? edit.framesPerSecond : edit.upstream.getMetadata().timeCodesPerSecond;
    }

    return edit.upstream.getMetadata().timeCodesPerSecond;
}

double
getMetersPerUnit(const SceneLayerEdit& edit)
{
    return (edit.metersPerUnit > 0.) ? edit.metersPerUnit : SceneMetadata().metersPerUnit;
}

void
appendUpstreamMetadata(const ScenePayload& upstream,
                       ContentHasher* h)
{
    const SceneMetadata& metadata = upstream.getMetadata();

    h->appendBool(upstream.isEmpty());
    h->appendI64((std::int64_t)metadata.upAxis);
    h->appendDouble(metadata.metersPerUnit);
    h->appendString(metadata.defaultPrim);
    h->appendDouble(metadata.timeCodesPerSecond);
}

void
authorLayerMetadata(const SceneLayerEdit& edit)
{
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();

    edit.layer->SetTimeCodesPerSecond(getTimeCodesPerSecond(edit));
    edit.layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis,
                         (edit.upAxis == SceneMetadata::eUpAxisZ) ? PXR_NS::UsdGeomTokens->z : PXR_NS::UsdGeomTokens->y);
    edit.layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, getMetersPerUnit(edit));
}

PXR_NS::UsdStageRefPtr
openLayerStage(const SceneLayerEdit& edit,
               std::string* errorMessage)
{
    if (!edit.layer) {
        *errorMessage = EffectInstance::tr("No layer to author.").toStdString();

        return PXR_NS::UsdStageRefPtr();
    }
    authorLayerMetadata(edit);
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(edit.layer, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadNone);
    if (!stage) {
        *errorMessage = EffectInstance::tr("USD could not open a stage on the node's layer.").toStdString();
    }

    return stage;
}

bool
getPrimPath(const KnobStringPtr& knob,
            PXR_NS::SdfPath* path,
            std::string* errorMessage)
{
    const std::string value = knob ? knob->getValue() : std::string();

    if (PXR_NS::SdfPath::IsValidPathString(value)) {
        const PXR_NS::SdfPath candidate(value);
        if (candidate.IsAbsolutePath() && candidate.IsPrimPath()) {
            *path = candidate;

            return true;
        }
    }
    *errorMessage = EffectInstance::tr("\"%1\" is not an absolute prim path such as /World/Prim.").arg(QString::fromStdString(value)).toStdString();

    return false;
}

bool
isAnimated(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    for (int d = 0; d < knob->getDimension(); ++d) {
        if (knob->isAnimated(d) || !knob->getExpression(d).empty()) {
            return true;
        }
    }

    return false;
}

std::vector<double>
getSampleFrames(const SceneLayerEdit& edit)
{
    std::vector<double> frames;
    const double first = std::ceil(edit.firstFrame);
    const double last = std::max(first, std::floor(edit.lastFrame));

    frames.reserve((std::size_t)(last - first) + 1);
    for (double frame = first; frame <= last; frame += 1.) {
        frames.push_back(frame);
    }

    return frames;
}

PXR_NS::UsdTimeCode
frameToTimeCode(const SceneLayerEdit& edit,
                double frame)
{
    if (edit.framesPerSecond <= 0.) {
        return PXR_NS::UsdTimeCode(frame);
    }

    return PXR_NS::UsdTimeCode(frame * getTimeCodesPerSecond(edit) / edit.framesPerSecond);
}

bool
authorTransform(const SceneTransformKnobs& knobs,
                const PXR_NS::UsdGeomXformable& xformable,
                const SceneLayerEdit& edit,
                std::string* errorMessage)
{
    static const PXR_NS::UsdGeomXformOp::Type kRotateTypes[] = {
        PXR_NS::UsdGeomXformOp::TypeRotateXYZ,
        PXR_NS::UsdGeomXformOp::TypeRotateXZY,
        PXR_NS::UsdGeomXformOp::TypeRotateYXZ,
        PXR_NS::UsdGeomXformOp::TypeRotateYZX,
        PXR_NS::UsdGeomXformOp::TypeRotateZXY,
        PXR_NS::UsdGeomXformOp::TypeRotateZYX,
    };
    KnobDoublePtr translate = knobs.translate.lock();
    KnobDoublePtr rotate = knobs.rotate.lock();
    KnobDoublePtr scale = knobs.scale.lock();
    KnobDoublePtr uniformScale = knobs.uniformScale.lock();
    KnobChoicePtr rotateOrder = knobs.rotateOrder.lock();

    if (!xformable || !translate || !rotate || !scale || !uniformScale || !rotateOrder) {
        *errorMessage = EffectInstance::tr("The prim cannot hold a transform.").toStdString();

        return false;
    }
    int order = rotateOrder->getValue();
    if ((order < 0) || (order >= (int)(sizeof(kRotateTypes) / sizeof(kRotateTypes[0])))) {
        order = (int)eSceneRotateOrderXYZ;
    }

    const PXR_NS::UsdGeomXformOp translateOp = xformable.AddTranslateOp(PXR_NS::UsdGeomXformOp::PrecisionDouble);
    const PXR_NS::UsdGeomXformOp rotateOp = xformable.AddXformOp(kRotateTypes[order], PXR_NS::UsdGeomXformOp::PrecisionFloat);
    const PXR_NS::UsdGeomXformOp scaleOp = xformable.AddScaleOp(PXR_NS::UsdGeomXformOp::PrecisionFloat);
    const bool authored = translateOp && rotateOp && scaleOp
        && authorValue(translateOp.GetAttr(), edit, isAnimated(translate), [&translate](double frame) {
                              return PXR_NS::GfVec3d(translate->getValueAtTime(frame, 0), translate->getValueAtTime(frame, 1), translate->getValueAtTime(frame, 2));
                          })
        && authorValue(rotateOp.GetAttr(), edit, isAnimated(rotate), [&rotate](double frame) {
                              return PXR_NS::GfVec3f((float)rotate->getValueAtTime(frame, 0), (float)rotate->getValueAtTime(frame, 1), (float)rotate->getValueAtTime(frame, 2));
                          })
        && authorValue(scaleOp.GetAttr(), edit, isAnimated(scale) || isAnimated(uniformScale), [&scale, &uniformScale](double frame) {
                              const double uniform = uniformScale->getValueAtTime(frame, 0);

                              return PXR_NS::GfVec3f((float)(scale->getValueAtTime(frame, 0) * uniform), (float)(scale->getValueAtTime(frame, 1) * uniform), (float)(scale->getValueAtTime(frame, 2) * uniform));
                          });
    if (!authored) {
        *errorMessage = EffectInstance::tr("USD could not author the transform of %1.").arg(QString::fromStdString(xformable.GetPath().GetString())).toStdString();

        return false;
    }

    return true;
} // authorTransform

bool
matchesGlob(const std::string& name,
            const std::string& glob)
{
    // Greedy with backtracking to the last star: linear for patterns with one star, and never
    // worse than quadratic.
    std::size_t n = 0;
    std::size_t g = 0;
    std::size_t starG = std::string::npos;
    std::size_t starN = 0;

    while (n < name.size()) {
        if ((g < glob.size()) && ((glob[g] == '?') || (glob[g] == name[n]))) {
            ++n;
            ++g;
        } else if ((g < glob.size()) && (glob[g] == '*')) {
            starG = g++;
            starN = n;
        } else if (starG != std::string::npos) {
            g = starG + 1;
            n = ++starN;
        } else {
            return false;
        }
    }
    while ((g < glob.size()) && (glob[g] == '*')) {
        ++g;
    }

    return g == glob.size();
}

namespace {
    bool
    isMatchablePrim(const PXR_NS::UsdPrim& prim)
    {
        return prim && prim.IsActive() && prim.IsDefined() && !prim.IsAbstract();
    }

    void
    collectMatches(const PXR_NS::UsdPrim& parent,
                   const std::vector<std::string>& names,
                   std::size_t depth,
                   std::vector<PXR_NS::SdfPath>* paths)
    {
        if (depth == names.size()) {
            if (isMatchablePrim(parent) && (std::find(paths->begin(), paths->end(), parent.GetPath()) == paths->end())) {
                paths->push_back(parent.GetPath());
            }

            return;
        }
        const std::string& name = names[depth];
        if (name.find_first_of("*?") == std::string::npos) {
            const PXR_NS::UsdPrim child = parent.GetChild(PXR_NS::TfToken(name));
            if (isMatchablePrim(child)) {
                collectMatches(child, names, depth + 1, paths);
            }

            return;
        }
        for (const PXR_NS::UsdPrim& child : parent.GetChildren()) {
            if (isMatchablePrim(child) && matchesGlob(child.GetName().GetString(), name)) {
                collectMatches(child, names, depth + 1, paths);
            }
        }
    }
} // anonymous namespace

bool
matchPrims(const PXR_NS::UsdStageRefPtr& stage,
           const std::string& pattern,
           std::vector<PXR_NS::SdfPath>* paths,
           std::string* errorMessage)
{
    paths->clear();
    std::vector<std::vector<std::string>> entries;
    std::size_t pos = 0;
    while (pos < pattern.size()) {
        const std::size_t begin = pattern.find_first_not_of(" \t\r\n", pos);
        if (begin == std::string::npos) {
            break;
        }
        std::size_t end = pattern.find_first_of(" \t\r\n", begin);
        if (end == std::string::npos) {
            end = pattern.size();
        }
        const std::string entry = pattern.substr(begin, end - begin);
        pos = end;
        if ((entry.size() < 2) || (entry[0] != '/')) {
            *errorMessage = EffectInstance::tr("\"%1\" is not an absolute prim path such as /World/Geo or /World/*.").arg(QString::fromStdString(entry)).toStdString();

            return false;
        }
        std::vector<std::string> names;
        std::size_t namePos = 1;
        while (namePos <= entry.size()) {
            std::size_t slash = entry.find('/', namePos);
            if (slash == std::string::npos) {
                slash = entry.size();
            }
            if (slash == namePos) {
                *errorMessage = EffectInstance::tr("\"%1\" has an empty prim name.").arg(QString::fromStdString(entry)).toStdString();

                return false;
            }
            names.push_back(entry.substr(namePos, slash - namePos));
            namePos = slash + 1;
        }
        entries.push_back(names);
    }
    if (!stage) {
        return true;
    }
    for (const std::vector<std::string>& names : entries) {
        collectMatches(stage->GetPseudoRoot(), names, 0, paths);
    }

    return true;
} // matchPrims

bool
graftMaterialInput(const ScenePayload& material,
                   const std::string& primPath,
                   const std::string& layerTag,
                   SceneGraftMemo* memo,
                   SceneMaterialGraft* graft,
                   std::string* errorMessage)
{
    *graft = SceneMaterialGraft();
    if (!material.isEmpty()) {
        SceneReparentArgs args;
        args.inputs.push_back(material);
        args.destinationPath = primPath;
        args.layerTag = layerTag;
        std::vector<SceneRootGraft> grafts;
        if (!SceneOps::reparentRoots(args, memo, &graft->payload, &grafts, &graft->unmappedLinks, errorMessage)) {
            return false;
        }

        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(material, errorMessage);
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = EffectInstance::tr("USD could not compose the material input.").toStdString();
            }

            return false;
        }
        for (const SceneRootGraft& root : grafts) {
            const PXR_NS::UsdPrim rootPrim = stage->GetPrimAtPath(PXR_NS::SdfPath(root.sourcePath));
            for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange(rootPrim)) {
                if (prim.IsA<PXR_NS::UsdShadeMaterial>()) {
                    graft->materialPath = prim.GetPath().ReplacePrefix(PXR_NS::SdfPath(root.sourcePath), PXR_NS::SdfPath(root.graftedPath)).GetString();
                    break;
                }
            }
            if (!graft->materialPath.empty()) {
                break;
            }
        }
    }

    ContentHasher facts;
    appendUpstreamMetadata(graft->payload, &facts);
    facts.appendString(graft->materialPath);
    graft->factsDigest = facts.finalize();

    return true;
} // graftMaterialInput

bool
authorMaterialBinding(const PXR_NS::UsdPrim& prim,
                      const std::string& materialPath,
                      std::string* errorMessage)
{
    if (materialPath.empty()) {
        return true;
    }
    const PXR_NS::UsdShadeMaterialBindingAPI binding = prim ? PXR_NS::UsdShadeMaterialBindingAPI::Apply(prim) : PXR_NS::UsdShadeMaterialBindingAPI();
    const PXR_NS::UsdRelationship rel = binding ? prim.CreateRelationship(PXR_NS::UsdShadeTokens->materialBinding, false) : PXR_NS::UsdRelationship();
    if (!rel || !rel.SetTargets({ PXR_NS::SdfPath(materialPath) })) {
        *errorMessage = EffectInstance::tr("USD could not bind %1 to the material at %2.").arg(QString::fromStdString(prim ? prim.GetPath().GetString() : std::string())).arg(QString::fromStdString(materialPath)).toStdString();

        return false;
    }

    return true;
}

int
getRotateOrderAxis(SceneRotateOrderEnum rotateOrder,
                   int position)
{
    static const char* const kOrders[] = { "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX" };
    const int order = ((int)rotateOrder >= 0 && (int)rotateOrder < 6) ? (int)rotateOrder : 0;

    return kOrders[order][std::min(std::max(position, 0), 2)] - 'X';
}

PXR_NS::GfMatrix4d
composeRotationMatrix(const PXR_NS::GfVec3d& rotate,
                      SceneRotateOrderEnum rotateOrder)
{
    // With row vectors the leftmost factor applies first, as the order's first axis does.
    PXR_NS::GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        const int axis = getRotateOrderAxis(rotateOrder, i);
        PXR_NS::GfVec3d direction(0.);
        direction[axis] = 1.;
        PXR_NS::GfMatrix4d factor(1.);
        factor.SetRotate(PXR_NS::GfRotation(direction, rotate[axis]));
        rotation = rotation * factor;
    }

    return rotation;
}

PXR_NS::GfMatrix4d
composeTransformMatrix(const PXR_NS::GfVec3d& translate,
                       const PXR_NS::GfVec3d& rotate,
                       const PXR_NS::GfVec3d& scale,
                       const PXR_NS::GfVec3d& pivot,
                       SceneRotateOrderEnum rotateOrder)
{
    PXR_NS::GfMatrix4d toPivot(1.);
    toPivot.SetTranslate(-pivot);
    PXR_NS::GfMatrix4d scaling(1.);
    scaling.SetScale(scale);
    PXR_NS::GfMatrix4d fromPivot(1.);
    fromPivot.SetTranslate(pivot + translate);

    return toPivot * scaling * composeRotationMatrix(rotate, rotateOrder) * fromPivot;
}

bool
isTransformAnimated(const SceneTransformKnobs& knobs,
                    const KnobDoublePtr& pivot)
{
    return isAnimated(knobs.translate.lock()) || isAnimated(knobs.rotate.lock()) || isAnimated(knobs.scale.lock())
        || isAnimated(knobs.uniformScale.lock()) || isAnimated(pivot);
}

PXR_NS::GfMatrix4d
getTransformMatrixAt(const SceneTransformKnobs& knobs,
                     const KnobDoublePtr& pivot,
                     double frame)
{
    KnobDoublePtr translate = knobs.translate.lock();
    KnobDoublePtr rotate = knobs.rotate.lock();
    KnobDoublePtr scale = knobs.scale.lock();
    KnobDoublePtr uniformScale = knobs.uniformScale.lock();
    KnobChoicePtr rotateOrder = knobs.rotateOrder.lock();

    if (!translate || !rotate || !scale || !uniformScale || !rotateOrder || !pivot) {
        return PXR_NS::GfMatrix4d(1.);
    }
    const double uniform = uniformScale->getValueAtTime(frame, 0);

    return composeTransformMatrix(PXR_NS::GfVec3d(translate->getValueAtTime(frame, 0), translate->getValueAtTime(frame, 1), translate->getValueAtTime(frame, 2)),
                                  PXR_NS::GfVec3d(rotate->getValueAtTime(frame, 0), rotate->getValueAtTime(frame, 1), rotate->getValueAtTime(frame, 2)),
                                  PXR_NS::GfVec3d(scale->getValueAtTime(frame, 0) * uniform, scale->getValueAtTime(frame, 1) * uniform, scale->getValueAtTime(frame, 2) * uniform),
                                  PXR_NS::GfVec3d(pivot->getValueAtTime(frame, 0), pivot->getValueAtTime(frame, 1), pivot->getValueAtTime(frame, 2)),
                                  (SceneRotateOrderEnum)rotateOrder->getValue());
}

} // namespace SceneNodeAuthoring

NATRON_NAMESPACE_EXIT
