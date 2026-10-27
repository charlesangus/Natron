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

#include "CornerPin.h"

#include <cfloat>
#include <list>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/UndoCommand.h"

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

namespace {
const char* const kToNames[4] = { kCornerPinNodeParamTo1, kCornerPinNodeParamTo2, kCornerPinNodeParamTo3, kCornerPinNodeParamTo4 };
const char* const kFromNames[4] = { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom4 };
const char* const kEnableNames[4] = { kCornerPinNodeParamEnable1, kCornerPinNodeParamEnable2, kCornerPinNodeParamEnable3, kCornerPinNodeParamEnable4 };

// The defaults of both point sets, as fractions of the project format: its four corners.
const double kDefaultPoints[4][2] = { { 0., 0. }, { 1., 0. }, { 1., 1. }, { 0., 1. } };

KnobDoublePtr
createPointKnob(KnobHolder* holder,
                const KnobGroupPtr& group,
                const char* name,
                const QString& label,
                const QString& hint,
                int index)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label, 2);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setSpatial(true);
    knob->setDefaultValuesAreNormalized(true);
    knob->disableSlider();
    for (int d = 0; d < 2; ++d) {
        knob->setMinimum(-DBL_MAX, d);
        knob->setMaximum(DBL_MAX, d);
        knob->setDisplayMinimum(-10000., d);
        knob->setDisplayMaximum(10000., d);
        knob->setIncrement(1., d);
        knob->setDefaultValue(kDefaultPoints[index][d], d);
    }
    group->addKnob(knob);

    return knob;
}

KnobButtonPtr
createButton(KnobHolder* holder,
             const KnobGroupPtr& group,
             const char* name,
             const QString& label,
             const QString& hint,
             bool newLine)
{
    KnobButtonPtr knob = AppManager::createKnob<KnobButton>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    if (!newLine) {
        knob->setAddNewLine(false);
        knob->setSpacingBetweenItems(1);
    }
    group->addKnob(knob);

    return knob;
}

Transform::Point3D
point(const double p[2])
{
    return Transform::Point3D(p[0], p[1], 1.);
}

/*
 * The solver for n points mapping p[idx[j]] to q[idx[j]]: a homography for 4, affine for 3,
 * similarity for 2, translation for 1. False for a degenerate set.
 */
bool
solvePoints(const Transform::Point3D* p,
            const Transform::Point3D* q,
            const int* idx,
            int n,
            Transform::Matrix3x3* H)
{
    switch (n) {
    case 4:
        return H->setHomographyFromFourPoints(p[idx[0]], p[idx[1]], p[idx[2]], p[idx[3]], q[idx[0]], q[idx[1]], q[idx[2]], q[idx[3]]);
    case 3:
        return H->setAffineFromThreePoints(p[idx[0]], p[idx[1]], p[idx[2]], q[idx[0]], q[idx[1]], q[idx[2]]);
    case 2:
        return H->setSimilarityFromTwoPoints(p[idx[0]], p[idx[1]], q[idx[0]], q[idx[1]]);
    case 1:
        return H->setTranslationFromOnePoint(p[idx[0]], q[idx[0]]);
    default:
        return false;
    }
}

/*
 * The matrix mapping the k points p onto q, from the first subset that solves: all k points, then
 * the subsets of one point fewer in lexicographic order (so later points are dropped first), and
 * so on down to one point. Negated when it maps its subset's first point to z < 0. False when no
 * subset solves.
 */
bool
solveWithFallback(const Transform::Point3D* p,
                  const Transform::Point3D* q,
                  int k,
                  Mat3* matrix)
{
    for (int n = k; n >= 1; --n) {
        int idx[4] = { 0, 1, 2, 3 };
        for (;;) {
            Transform::Matrix3x3 H;
            if (solvePoints(p, q, idx, n, &H)) {
                *matrix = TransformMath::fromEngineMatrix(H);
                if (n < 4) {
                    // The affine solvers compose Hq * inverse(Hp), which leaves rounding noise in
                    // the bottom row; left there it would send the pin down the projective path.
                    (*matrix)(2, 0) = 0.;
                    (*matrix)(2, 1) = 0.;
                    (*matrix)(2, 2) = 1.;
                }
                const Transform::Point3D& first = p[idx[0]];
                const TransformMath::Point3 mapped = *matrix * TransformMath::Point3(first.x, first.y, first.z);
                if (mapped.z < 0.) {
                    for (int c = 0; c < 9; ++c) {
                        matrix->m[c] = -matrix->m[c];
                    }
                }

                return true;
            }
            int i = n - 1;
            while ((i >= 0) && (idx[i] == k - n + i)) {
                --i;
            }
            if (i < 0) {
                break;
            }
            ++idx[i];
            for (int j = i + 1; j < n; ++j) {
                idx[j] = idx[j - 1] + 1;
            }
        }
    }

    return false;
}
} // namespace

void
CornerPinOp::createKnobs(KnobHolder* holder,
                         const KnobPagePtr& page)
{
    KnobGroupPtr toGroup = AppManager::createKnob<KnobGroup>(holder, EffectInstance::tr("To"));
    toGroup->setName(kCornerPinNodeParamToGroup);
    toGroup->setAsTab();
    toGroup->setDefaultValue(true);
    page->addKnob(toGroup);

    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr toKnob = createPointKnob(holder, toGroup, kToNames[i], EffectInstance::tr("To %1").arg(i + 1), EffectInstance::tr("Where \"From %1\" lands in the output.").arg(i + 1), i);
        toKnob->setAddNewLine(false);
        toKnob->setSpacingBetweenItems(1);
        to[i] = toKnob;

        KnobBoolPtr enableKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Enable"));
        enableKnob->setName(kEnableNames[i]);
        enableKnob->setHintToolTip(EffectInstance::tr("Whether point %1 takes part in the transform: a translation for 1 enabled point, a similarity for 2, an affine transform for 3 and a homography for 4.").arg(i + 1));
        enableKnob->setDefaultValue(true);
        toGroup->addKnob(enableKnob);
        enable[i] = enableKnob;
    }

    copyFrom = createButton(holder, toGroup, kCornerPinNodeParamCopyFrom, EffectInstance::tr("Copy \"From\""), EffectInstance::tr("Copy the content (including animation) of the \"From\" points to the \"To\" points."), false);
    copyFromSingle = createButton(holder, toGroup, kCornerPinNodeParamCopyFromSingle, EffectInstance::tr("Copy \"From\" (Single)"), EffectInstance::tr("Copy the current values of the \"From\" points to the \"To\" points."), true);

    KnobGroupPtr fromGroup = AppManager::createKnob<KnobGroup>(holder, EffectInstance::tr("From"));
    fromGroup->setName(kCornerPinNodeParamFromGroup);
    fromGroup->setAsTab();
    fromGroup->setDefaultValue(true);
    page->addKnob(fromGroup);

    for (int i = 0; i < 4; ++i) {
        from[i] = createPointKnob(holder, fromGroup, kFromNames[i], EffectInstance::tr("From %1").arg(i + 1), EffectInstance::tr("Source point %1, which lands on \"To %1\".").arg(i + 1), i);
    }

    setToInput = createButton(holder, fromGroup, kCornerPinNodeParamSetToInput, EffectInstance::tr("Set to Input"), EffectInstance::tr("Set the \"From\" points to the corners of the input's region of definition."), false);
    copyTo = createButton(holder, fromGroup, kCornerPinNodeParamCopyTo, EffectInstance::tr("Copy \"To\""), EffectInstance::tr("Copy the content (including animation) of the \"To\" points to the \"From\" points."), false);
    copyToSingle = createButton(holder, fromGroup, kCornerPinNodeParamCopyToSingle, EffectInstance::tr("Copy \"To\" (Single)"), EffectInstance::tr("Copy the current values of the \"To\" points to the \"From\" points."), true);

    KnobDoublePtr amountKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Amount"));
    amountKnob->setName(kCornerPinNodeParamAmount);
    amountKnob->setHintToolTip(EffectInstance::tr("Amount of transform to apply, moving each \"To\" point from its \"From\" point: 0 leaves the points where they are, 1 applies the full pin. The extra matrix always applies in full."));
    amountKnob->setMinimum(-DBL_MAX);
    amountKnob->setMaximum(DBL_MAX);
    amountKnob->setDisplayMinimum(0.);
    amountKnob->setDisplayMaximum(1.);
    amountKnob->setIncrement(0.01);
    amountKnob->setDefaultValue(1.);
    page->addKnob(amountKnob);
    amount = amountKnob;

    KnobGroupPtr extraGroup = AppManager::createKnob<KnobGroup>(holder, EffectInstance::tr("Extra Matrix"));
    extraGroup->setName(kCornerPinNodeParamExtraMatrix "Group");
    extraGroup->setDefaultValue(false);
    page->addKnob(extraGroup);

    KnobDoublePtr extraKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Matrix"), 9);
    extraKnob->setName(kCornerPinNodeParamExtraMatrix);
    extraKnob->setHintToolTip(EffectInstance::tr("A 3x3 matrix, row by row, composed with the pin: it maps output coordinates before the pin does, so the output is the pin's image transformed by its inverse."));
    for (int d = 0; d < 9; ++d) {
        extraKnob->setMinimum(-DBL_MAX, d);
        extraKnob->setMaximum(DBL_MAX, d);
        extraKnob->setDisplayMinimum(-10., d);
        extraKnob->setDisplayMaximum(10., d);
        extraKnob->setIncrement(0.01, d);
        extraKnob->setDefaultValue(((d % 4) == 0) ? 1. : 0., d);
    }
    extraGroup->addKnob(extraKnob);
    extraMatrix = extraKnob;

    KnobChoicePtr overlayKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Overlay Points"));
    overlayKnob->setName(kCornerPinNodeParamOverlayPoints);
    overlayKnob->setHintToolTip(EffectInstance::tr("Whether the viewer shows and edits the \"To\" or the \"From\" points."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("to", "To", ""));
        options.push_back(ChoiceOption("from", "From", ""));
        overlayKnob->populateChoices(options);
    }
    overlayKnob->setDefaultValue(0);
    overlayKnob->setAnimationEnabled(false);
    overlayKnob->setEvaluateOnChange(false);
    page->addKnob(overlayKnob);
    overlayPoints = overlayKnob;

    KnobBoolPtr interactiveKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Interactive Update"));
    interactiveKnob->setName(kCornerPinNodeParamInteractive);
    interactiveKnob->setHintToolTip(EffectInstance::tr("If checked, update the parameter values during interaction with the image viewer, else update the values when pen is released."));
    interactiveKnob->setDefaultValue(true);
    interactiveKnob->setAnimationEnabled(false);
    interactiveKnob->setEvaluateOnChange(false);
    page->addKnob(interactiveKnob);
    interactive = interactiveKnob;

    KnobBoolPtr invertKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Invert"));
    invertKnob->setName(kCornerPinNodeParamInvert);
    invertKnob->setHintToolTip(EffectInstance::tr("Invert the transform, extra matrix included."));
    invertKnob->setDefaultValue(false);
    page->addKnob(invertKnob);
    invert = invertKnob;

    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr(kResamplerParamFilterTypeLabel));
    filterKnob->setName(kCornerPinNodeParamFilter);
    filterKnob->setHintToolTip(EffectInstance::tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)Resampler::eFilterCubic);
    filterKnob->setAddNewLine(false);
    filterKnob->setSpacingBetweenItems(1);
    page->addKnob(filterKnob);
    filter = filterKnob;

    KnobBoolPtr clampKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr(kResamplerParamFilterClampLabel));
    clampKnob->setName(kCornerPinNodeParamClamp);
    clampKnob->setHintToolTip(EffectInstance::tr(kResamplerParamFilterClampHint));
    clampKnob->setDefaultValue(false);
    clampKnob->setAddNewLine(false);
    clampKnob->setSpacingBetweenItems(1);
    page->addKnob(clampKnob);
    clamp = clampKnob;

    KnobBoolPtr blackOutsideKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutsideKnob->setName(kCornerPinNodeParamBlackOutside);
    blackOutsideKnob->setHintToolTip(EffectInstance::tr(kResamplerParamFilterBlackOutsideHint));
    blackOutsideKnob->setDefaultValue(true);
    page->addKnob(blackOutsideKnob);
    blackOutside = blackOutsideKnob;

    KnobDoublePtr motionBlurKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Motion Blur"));
    motionBlurKnob->setName(kCornerPinNodeParamMotionBlur);
    motionBlurKnob->setHintToolTip(EffectInstance::tr("Quality of motion blur rendering. 0 disables motion blur, 1 is a good value. Increasing this slows down rendering."));
    motionBlurKnob->setMinimum(0.);
    motionBlurKnob->setMaximum(100.);
    motionBlurKnob->setDisplayMinimum(0.);
    motionBlurKnob->setDisplayMaximum(4.);
    motionBlurKnob->setIncrement(0.01);
    motionBlurKnob->setDefaultValue(0.);
    page->addKnob(motionBlurKnob);
    motionBlur = motionBlurKnob;

    KnobDoublePtr shutterKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Shutter"));
    shutterKnob->setName(kCornerPinNodeParamShutter);
    shutterKnob->setHintToolTip(EffectInstance::tr("Controls how long (in frames) the shutter should remain open."));
    shutterKnob->setMinimum(0.);
    shutterKnob->setMaximum(2.);
    shutterKnob->setDisplayMinimum(0.);
    shutterKnob->setDisplayMaximum(2.);
    shutterKnob->setIncrement(0.01);
    shutterKnob->setDefaultValue(0.5);
    page->addKnob(shutterKnob);
    shutter = shutterKnob;

    KnobChoicePtr shutterOffsetKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr(kResamplerParamShutterOffsetLabel));
    shutterOffsetKnob->setName(kCornerPinNodeParamShutterOffset);
    shutterOffsetKnob->setHintToolTip(EffectInstance::tr(kResamplerParamShutterOffsetHint));
    {
        const std::vector<Resampler::ChoiceOption>& offsets = Resampler::shutterOffsetOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            options.push_back(ChoiceOption(offsets[i].id, offsets[i].label, offsets[i].hint));
        }
        shutterOffsetKnob->populateChoices(options);
    }
    shutterOffsetKnob->setDefaultValue((int)Resampler::eShutterOffsetStart);
    page->addKnob(shutterOffsetKnob);
    shutterOffset = shutterOffsetKnob;

    KnobDoublePtr shutterCustomOffsetKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr(kResamplerParamShutterCustomOffsetLabel));
    shutterCustomOffsetKnob->setName(kCornerPinNodeParamShutterCustomOffset);
    shutterCustomOffsetKnob->setHintToolTip(EffectInstance::tr(kResamplerParamShutterCustomOffsetHint));
    shutterCustomOffsetKnob->setMinimum(-1.);
    shutterCustomOffsetKnob->setMaximum(1.);
    shutterCustomOffsetKnob->setDisplayMinimum(-1.);
    shutterCustomOffsetKnob->setDisplayMaximum(1.);
    shutterCustomOffsetKnob->setIncrement(0.1);
    shutterCustomOffsetKnob->setDefaultValue(0.);
    page->addKnob(shutterCustomOffsetKnob);
    shutterCustomOffset = shutterCustomOffsetKnob;
} // CornerPinOp::createKnobs

CornerPinOp::Params
CornerPinOp::bake(const KnobReader& reader) const
{
    Params params;
    CornerPinPoints& pin = params.pin;

    for (int i = 0; i < 4; ++i) {
        for (int d = 0; d < 2; ++d) {
            pin.to[i][d] = reader.value(to[i], d);
            pin.from[i][d] = reader.value(from[i], d);
        }
        pin.enable[i] = reader.value(enable[i]);
    }
    pin.amount = reader.value(amount);
    for (int d = 0; d < 9; ++d) {
        pin.extraMatrix.m[d] = reader.value(extraMatrix, d);
    }

    params.invert = reader.value(invert);
    params.filter = (Resampler::FilterEnum)reader.value(filter);
    params.clamp = reader.value(clamp);
    params.blackOutside = reader.value(blackOutside);

    Resampler::BlurSettings& blur = params.blur;
    blur.motionBlur = reader.value(motionBlur);
    blur.directionalBlur = false;
    blur.shutter = reader.value(shutter);
    blur.shutterOffset = (Resampler::ShutterOffsetEnum)reader.value(shutterOffset);
    blur.shutterCustomOffset = reader.value(shutterCustomOffset);

    return params;
} // CornerPinOp::bake

bool
CornerPinOp::isIdentityPin(const CornerPinPoints& pin)
{
    if (!pin.extraMatrix.isIdentity()) {
        return false;
    }
    if (pin.amount == 0.) {
        return true;
    }
    for (int i = 0; i < 4; ++i) {
        if (pin.enable[i] && ((pin.to[i][0] != pin.from[i][0]) || (pin.to[i][1] != pin.from[i][1]))) {
            return false;
        }
    }

    return true;
}

bool
CornerPinOp::isIdentity(const Params& params,
                        const PassContext& /*context*/) const
{
    if ((params.blur.shutter != 0.) && (params.blur.motionBlur != 0.)) {
        return false;
    }

    return isIdentityPin(params.pin);
}

ResampleSettings
CornerPinOp::settings(const Params& params,
                      const PassContext& /*context*/) const
{
    ResampleSettings settings;

    settings.filter = params.filter;
    settings.clamp = params.clamp;
    settings.blackOutside = params.blackOutside;
    settings.invert = params.invert;
    settings.blur = params.blur;
    settings.transformIsIdentity = isIdentityPin(params.pin);

    return settings;
}

bool
CornerPinOp::inverseMatrix(const CornerPinPoints& pin,
                           double amount,
                           bool invert,
                           Mat3* matrix)
{
    const double blend = amount * pin.amount;
    Transform::Point3D from[4];
    Transform::Point3D to[4];
    int k = 0;

    for (int i = 0; i < 4; ++i) {
        if (!pin.enable[i]) {
            continue;
        }
        from[k] = point(pin.from[i]);
        to[k] = point(pin.to[i]);
        if (blend != 1.) {
            to[k].x = from[k].x + blend * (to[k].x - from[k].x);
            to[k].y = from[k].y + blend * (to[k].y - from[k].y);
        }
        ++k;
    }

    // The sampling matrix maps output ("to") points to source ("from") points; the forward one
    // the other way, so that each is the other's inverse.
    Mat3 H = Mat3::identity();
    if (k > 0) {
        if (invert) {
            solveWithFallback(from, to, k, &H);
        } else {
            solveWithFallback(to, from, k, &H);
        }
    }
    if (!invert) {
        *matrix = H * pin.extraMatrix;

        return true;
    }
    Mat3 extraInverse;
    if (!pin.extraMatrix.inverse(&extraInverse)) {
        return false;
    }
    *matrix = extraInverse * H;

    return true;
} // CornerPinOp::inverseMatrix

bool
CornerPinOp::transform(const Params& params,
                       const PassContext& /*context*/,
                       double amount,
                       bool invert,
                       Mat3* matrix) const
{
    return inverseMatrix(params.pin, amount, invert, matrix);
}

bool
CornerPinOp::rendersWithMotionBlur(const Params& params)
{
    return (params.blur.motionBlur != 0.) && (params.blur.shutter != 0.);
}

std::optional<GpuCostHint>
CornerPinOp::costHint(const Params& params,
                      const PassContext& /*context*/) const
{
    GpuCostHint hint;

    hint.motionBlur = rendersWithMotionBlur(params);

    return hint;
}

CornerPinNode::CornerPinNode(NodePtr node)
    : Base(node)
{
}

CornerPinNode::~CornerPinNode()
{
}

void
CornerPinNode::addAcceptedComponents(int inputNb,
                                     std::list<ImageLayerDesc>* comps)
{
    Base::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

bool
CornerPinNode::getCanTransform() const
{
    // The MaskMix stage keeps the base from offering a matrix; without a mask, mix alone decides,
    // per time, in getTransform().
    return !isMaskApplied();
}

StatusEnum
CornerPinNode::getTransform(double time,
                            const RenderScale& renderScale,
                            bool draftRender,
                            ViewIdx view,
                            EffectInstancePtr* inputToTransform,
                            Transform::Matrix3x3* transform)
{
    if (isMaskApplied() || (getMixValue(time, view) != 1.)) {
        return eStatusReplyDefault;
    }

    return Base::getTransform(time, renderScale, draftRender, view, inputToTransform, transform);
}

bool
CornerPinNode::getInverseMatrix(double time,
                                ViewIdx view,
                                Mat3* matrix) const
{
    const CornerPinNodeParams params = bakeParams(op(), time, view);

    return CornerPinOp::inverseMatrix(params.pin, 1., params.invert, matrix);
}

void
CornerPinNode::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    op().createKnobs(static_cast<KnobHolder*>(this), page);
    addStageKnobs(page);

    KnobBoolPtr srcClipChanged = createKnob<KnobBool>(std::string(kCornerPinNodeParamSrcClipChanged));
    srcClipChanged->setName(kCornerPinNodeParamSrcClipChanged);
    srcClipChanged->setDefaultValue(false);
    srcClipChanged->setSecretByDefault(true);
    srcClipChanged->setDefaultAllDimensionsEnabled(false);
    srcClipChanged->setAnimationEnabled(false);
    srcClipChanged->setEvaluateOnChange(false);
    page->addKnob(srcClipChanged);
    _srcClipChanged = srcClipChanged;

    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));

    NodePtr node = getNode();
    if (node) {
        const CornerPinOp& o = op();
        node->addCornerPinInteract(o.from[0].lock(), o.from[1].lock(), o.from[2].lock(), o.from[3].lock(),
                                   o.to[0].lock(), o.to[1].lock(), o.to[2].lock(), o.to[3].lock(),
                                   o.enable[0].lock(), o.enable[1].lock(), o.enable[2].lock(), o.enable[3].lock(),
                                   o.overlayPoints.lock(), o.invert.lock(), o.interactive.lock());
    }
} // CornerPinNode::initializeKnobs

void
CornerPinNode::updateClampEnabled(double time,
                                  ViewSpec view)
{
    KnobChoicePtr filter = op().filter.lock();
    KnobBoolPtr clamp = op().clamp.lock();

    if (!filter || !clamp) {
        return;
    }
    clamp->setAllDimensionsEnabled(Resampler::filterHonoursClamp((Resampler::FilterEnum)filter->getValueAtTime(time, 0, view)));
}

void
CornerPinNode::onKnobsLoaded()
{
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

void
CornerPinNode::onInputChanged(int inputNo)
{
    KnobBoolPtr srcClipChanged = _srcClipChanged.lock();
    const int source = getSourceInput();

    if ((inputNo != source) || !getInput(source) || !srcClipChanged || srcClipChanged->getValue()) {
        return;
    }
    setFromToInput(getApp()->getTimeLine()->currentFrame(), true);
    if (!srcClipChanged->getValue()) {
        srcClipChanged->setValue(true);
    }
}

bool
CornerPinNode::getSourceRoD(double time,
                            RectD* rod) const
{
    EffectInstancePtr input = getInput(getSourceInput());

    if (!input) {
        return false;
    }
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, ViewIdx(0), rod, &isProjectFormat) != eStatusFailed;
}

void
CornerPinNode::setFromToInput(double time,
                              bool alsoTo)
{
    RectD rod;

    if (!getSourceRoD(time, &rod) || rod.isInfinite() || rod.isNull()) {
        return;
    }
    const double corners[4][2] = { { rod.x1, rod.y1 }, { rod.x2, rod.y1 }, { rod.x2, rod.y2 }, { rod.x1, rod.y2 } };
    const CornerPinOp& o = op();

    beginChanges();
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr fromKnob = o.from[i].lock();
        if (fromKnob) {
            fromKnob->setValues(corners[i][0], corners[i][1], ViewSpec::all(), eValueChangedReasonPluginEdited);
        }
        KnobDoublePtr toKnob = o.to[i].lock();
        if (alsoTo && toKnob) {
            toKnob->setValues(corners[i][0], corners[i][1], ViewSpec::all(), eValueChangedReasonPluginEdited);
        }
    }
    endChanges();
}

// The destination points of a copy button and srcClipChanged, kept as detached knobs so undo
// restores their values, expressions and curves.
class CornerPinNode::CopyPointsCommand
    : public UndoCommand {
public:
    CopyPointsCommand(const CornerPinNode& node,
                      bool fromToTo,
                      bool withAnimation,
                      double time,
                      bool markEdited)
        : UndoCommand(fromToTo ? "Copy From to To" : "Copy To to From")
        , _withAnimation(withAnimation)
        , _time(time)
        , _markEdited(markEdited)
        , _srcClipChanged(node._srcClipChanged)
        , _srcClipChangedBefore(false)
    {
        const CornerPinOp& o = node.op();

        for (int i = 0; i < 4; ++i) {
            _src[i] = fromToTo ? o.from[i] : o.to[i];
            _dst[i] = fromToTo ? o.to[i] : o.from[i];
            KnobDoublePtr dst = _dst[i].lock();
            if (dst) {
                _before[i] = std::make_shared<KnobDouble>((KnobHolder*)NULL, std::string(), dst->getDimension(), false);
                _before[i]->populate();
                _before[i]->clone(dst.get());
            }
        }
        KnobBoolPtr srcClipChanged = _srcClipChanged.lock();
        _srcClipChangedBefore = srcClipChanged && srcClipChanged->getValue();
    }

    virtual void redo() OVERRIDE FINAL
    {
        KnobHolder* holder = this->holder();

        if (holder) {
            holder->beginChanges();
        }
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr src = _src[i].lock();
            KnobDoublePtr dst = _dst[i].lock();
            if (!src || !dst) {
                continue;
            }
            if (_withAnimation) {
                dst->cloneAndUpdateGui(src.get());
                dst->evaluateValueChange(0, _time, ViewSpec::all(), eValueChangedReasonPluginEdited);
            } else {
                dst->setValues(src->getValueAtTime(_time, 0), src->getValueAtTime(_time, 1), ViewSpec::all(), eValueChangedReasonPluginEdited);
            }
        }
        KnobBoolPtr srcClipChanged = _srcClipChanged.lock();
        if (_markEdited && srcClipChanged && !srcClipChanged->getValue()) {
            srcClipChanged->setValue(true);
        }
        if (holder) {
            holder->endChanges();
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        KnobHolder* holder = this->holder();

        if (holder) {
            holder->beginChanges();
        }
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr dst = _dst[i].lock();
            if (!dst || !_before[i]) {
                continue;
            }
            dst->cloneAndUpdateGui(_before[i].get());
            dst->evaluateValueChange(0, _time, ViewSpec::all(), eValueChangedReasonPluginEdited);
        }
        // Restoring the points marks them edited; srcClipChanged goes back last.
        KnobBoolPtr srcClipChanged = _srcClipChanged.lock();
        if (srcClipChanged) {
            srcClipChanged->setValue(_srcClipChangedBefore);
        }
        if (holder) {
            holder->endChanges();
        }
    }

private:
    KnobHolder* holder() const
    {
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr dst = _dst[i].lock();
            if (dst) {
                return dst->getHolder();
            }
        }

        return NULL;
    }

    bool _withAnimation;
    double _time;
    bool _markEdited;
    KnobDoubleWPtr _src[4];
    KnobDoubleWPtr _dst[4];
    KnobDoublePtr _before[4];
    KnobBoolWPtr _srcClipChanged;
    bool _srcClipChangedBefore;
};

UndoCommandPtr
CornerPinNode::makeCopyPointsCommand(bool fromToTo,
                                     bool withAnimation,
                                     double time,
                                     bool markEdited)
{
    return std::make_shared<CopyPointsCommand>(*this, fromToTo, withAnimation, time, markEdited);
}

bool
CornerPinNode::isPointsEdit(ValueChangedReasonEnum reason)
{
    return (reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonPluginEdited) || (reason == eValueChangedReasonNatronGuiEdited) || (reason == eValueChangedReasonNatronInternalEdited);
}

void
CornerPinNode::markPointsEdited(ValueChangedReasonEnum reason)
{
    KnobBoolPtr srcClipChanged = _srcClipChanged.lock();

    if (isPointsEdit(reason) && srcClipChanged && !srcClipChanged->getValue()) {
        srcClipChanged->setValue(true);
    }
}

bool
CornerPinNode::knobChanged(KnobI* k,
                           ValueChangedReasonEnum reason,
                           ViewSpec view,
                           double time,
                           bool /*originatedFromMainThread*/)
{
    const CornerPinOp& o = op();

    if (k == o.setToInput.lock().get()) {
        setFromToInput(time, false);
        markPointsEdited(reason);

        return true;
    }
    if ((k == o.copyFrom.lock().get()) || (k == o.copyFromSingle.lock().get()) || (k == o.copyTo.lock().get()) || (k == o.copyToSingle.lock().get())) {
        const bool fromToTo = (k == o.copyFrom.lock().get()) || (k == o.copyFromSingle.lock().get());
        const bool withAnimation = (k == o.copyFrom.lock().get()) || (k == o.copyTo.lock().get());
        pushUndoCommand(makeCopyPointsCommand(fromToTo, withAnimation, time, isPointsEdit(reason)));

        return true;
    }
    for (int i = 0; i < 4; ++i) {
        if ((k == o.to[i].lock().get()) || (k == o.from[i].lock().get()) || (k == o.enable[i].lock().get())) {
            markPointsEdited(reason);

            return true;
        }
    }
    if (k == o.extraMatrix.lock().get()) {
        markPointsEdited(reason);

        return true;
    }
    KnobChoicePtr filterKnob = o.filter.lock();
    if (filterKnob && (k == filterKnob.get())) {
        updateClampEnabled(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }

    return false;
} // CornerPinNode::knobChanged

NATRON_NAMESPACE_EXIT
