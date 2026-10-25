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

#include "Despill.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/ColorMath.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

#ifdef NATRON_HAVE_GPU
// The parameter block of DespillOp in despill.slang, which has no padding between members.
struct DespillGpuParams {
    uint32_t flags;
    uint32_t limitKind;
    float rot[9];
    float weights[2];
    float expandScale;
    float spillScale;
    float strength;
    float move[3];
};

static_assert(sizeof(DespillGpuParams) == 19 * 4, "must match DespillOp in despill.slang");

const uint32_t kGpuFlagHasAxis = 1u;
const uint32_t kGpuFlagSpillMapAlpha = 2u;
#endif

KnobColorPtr
addColor(KnobHolder* holder,
         const KnobPagePtr& page,
         const char* name,
         const char* label,
         const char* hint,
         const double* defaults)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, std::string(label), 3);

    knob->setName(name);
    knob->setHintToolTip(std::string(hint));
    for (int i = 0; i < 3; ++i) {
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
        knob->setDefaultValue(defaults[i], i);
    }
    page->addKnob(knob);

    return knob;
}

KnobDoublePtr
addDouble(KnobHolder* holder,
          const KnobPagePtr& page,
          const char* name,
          const char* label,
          const char* hint,
          double defaultValue,
          double displayMin,
          double displayMax)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, std::string(label));

    knob->setName(name);
    knob->setHintToolTip(std::string(hint));
    knob->setMinimum(-DBL_MAX);
    knob->setMaximum(DBL_MAX);
    knob->setDisplayMinimum(displayMin);
    knob->setDisplayMaximum(displayMax);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

// Row r of the 3x3 row-major m applied to rgb, summed in order with one rounding per operation,
// as despill.slang does.
float
rotatedChannel(const float* m,
               const float* rgb)
{
    float s = m[0] * rgb[0];
    const float g = m[1] * rgb[1];

    s = s + g;
    const float b = m[2] * rgb[2];
    s = s + b;

    return s;
}

// The limit of a colour already in the screen's hue frame, in doubles for the host.
double
limitOf(int kind,
        const double* weights,
        double r,
        double b)
{
    switch (kind) {
    case DespillOp::eLimitKindMax:
        return (r > b) ? r : b;
    case DespillOp::eLimitKindMin:
        return (r < b) ? r : b;
    default:
        return weights[0] * r + weights[1] * b;
    }
}

// Rounds the near-integer entries of the rotation away, so that a red, green or blue screen
// rotates by a plain permutation.
double
snap(double v)
{
    if (std::abs(v) < 1e-12) {
        return 0.;
    }
    if (std::abs(v - 1.) < 1e-12) {
        return 1.;
    }
    if (std::abs(v + 1.) < 1e-12) {
        return -1.;
    }

    return v;
}
} // anonymous namespace

void
DespillOp::createKnobs(KnobHolder* h,
                       const KnobPagePtr& page)
{
    holder = h;

    const double green[3] = { 0., 1., 0. };
    screenColor = addColor(h, page, kDespillParamScreenColor, "Screen Color",
                           "The colour of the screen whose spill is removed. Only its hue matters to the spill axis, and how "
                           "far it is from grey sets the scale of the spill map: a grey screen colour has no axis and leaves "
                           "the image as it is.",
                           green);

    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(h, std::string("Limit"));
        knob->setName(kDespillParamLimit);
        knob->setHintToolTip(std::string("What the screen channel is limited to, from the two other channels of the screen's "
                                         "hue frame: for a green screen red and blue, for a blue screen green and red, for a "
                                         "red screen blue and green."));
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("average", "Average", "The mean of the two other channels."));
        options.push_back(ChoiceOption("max", "Max", "The larger of the two other channels; removes the least spill."));
        options.push_back(ChoiceOption("min", "Min", "The smaller of the two other channels; removes the most spill."));
        options.push_back(ChoiceOption("custom", "Custom", "The two other channels weighted by Limit Weights."));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eLimitAverage);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
        limit = knob;
    }
    {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(h, std::string("Limit Weights"), 2);
        knob->setName(kDespillParamLimitWeights);
        knob->setHintToolTip(std::string("With the Custom limit, the weights of the channel a third of the hue circle below the "
                                         "screen's hue (red for a green screen) and of the one a third above it (blue for a "
                                         "green screen)."));
        for (int i = 0; i < 2; ++i) {
            knob->setMinimum(-DBL_MAX, i);
            knob->setMaximum(DBL_MAX, i);
            knob->setDisplayMinimum(0., i);
            knob->setDisplayMaximum(1., i);
            knob->setDefaultValue(0.5, i);
        }
        page->addKnob(knob);
        limitWeights = knob;
    }
    expand = addDouble(h, page, kDespillParamExpand, "Expand",
                       "Scales the limit by one minus this: above 0 more of the screen channel counts as spill, below 0 "
                       "(contract) less. At 1 all of it does.",
                       0., -1., 1.);
    strength = addDouble(h, page, kDespillParamStrength, "Strength",
                         "How much of the spill is removed. The spill map does not depend on it.",
                         1., 0., 1.);

    const double grey[3] = { 0.5, 0.5, 0.5 };
    replaceColor = addColor(h, page, kDespillParamReplaceColor, "Replace Color",
                            "The colour whose chroma replaces the removed spill, in proportion to it. Only its difference from "
                            "the grey of its own luminance is added, so a grey adds nothing and the replacement never changes "
                            "the luminance.",
                            grey);
    lumaRestore = addDouble(h, page, kDespillParamLumaRestore, "Luma Restore",
                            "How much of the luminance the despill took out is added back, as grey, in the project's working "
                            "space. At 1 the luminance is kept.",
                            0., 0., 1.);
    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(h, std::string("Spill Map"));
        knob->setName(kDespillParamSpillMap);
        knob->setHintToolTip(std::string("Where the spill map goes: the spill of each pixel, 1 for the screen colour less its "
                                         "smallest channel, before Strength. Alpha makes the output RGBA, and is written only "
                                         "while alpha is among the processed channels."));
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("none", "None", "Alpha passes through."));
        options.push_back(ChoiceOption("alpha", "Alpha", "The spill map replaces alpha."));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eSpillMapNone);
        knob->setAnimationEnabled(false);
        knob->setIsMetadataSlave(true);
        page->addKnob(knob);
        spillMap = knob;
    }
} // DespillOp::createKnobs

DespillOp::Params
DespillOp::bake(const KnobReader& reader) const
{
    Params p;
    double screen[3];
    double replace[3];

    for (int i = 0; i < 3; ++i) {
        screen[i] = reader.value(screenColor, i);
        replace[i] = reader.value(replaceColor, i);
    }
    const int limitIndex = reader.value(limit);
    double weights[2] = { 0.5, 0.5 };
    int kind = eLimitKindWeighted;
    if (limitIndex == eLimitMax) {
        kind = eLimitKindMax;
    } else if (limitIndex == eLimitMin) {
        kind = eLimitKindMin;
    } else if (limitIndex == eLimitCustom) {
        weights[0] = reader.value(limitWeights, 0);
        weights[1] = reader.value(limitWeights, 1);
    }
    const double expandScale = 1. - reader.value(expand);
    const double restore = reader.value(lumaRestore);
    p.strength = (float)reader.value(strength);
    p.spillMapAlpha = (reader.value(spillMap) == eSpillMapAlpha);
    p.limitKind = kind;
    p.weights[0] = (float)weights[0];
    p.weights[1] = (float)weights[1];
    p.expandScale = (float)expandScale;

    const double maxS = (std::max)(screen[0], (std::max)(screen[1], screen[2]));
    const double minS = (std::min)(screen[0], (std::min)(screen[1], screen[2]));
    const double chroma = maxS - minS;
    if (!(chroma > 1e-6 * (std::max)(1., (std::max)(std::abs(maxS), std::abs(minS))))) {
        // Grey: no spill axis.
        return p;
    }

    // The screen's hue angle, red at 0 and green at 2pi/3, and the rotation about the grey axis
    // that takes it to green's.
    const double hx = (2. * screen[0] - screen[1] - screen[2]) / std::sqrt(6.);
    const double hy = (screen[1] - screen[2]) / std::sqrt(2.);
    const double theta = 2. * M_PI / 3. - std::atan2(hy, hx);
    const double c = std::cos(theta);
    const double s = std::sin(theta) / std::sqrt(3.);
    const double t = (1. - c) / 3.;
    const double rot[9] = {
        c + t, t - s, t + s,
        t + s, c + t, t - s,
        t - s, t + s, c + t
    };
    for (int i = 0; i < 9; ++i) {
        p.rot[i] = (float)snap(rot[i]);
    }

    // The screen's chroma direction, from 0 in its smallest channel to 1 in its largest, and
    // its spill.
    double dir[3];
    for (int i = 0; i < 3; ++i) {
        dir[i] = (screen[i] - minS) / chroma;
    }
    double rotated[3];
    for (int r = 0; r < 3; ++r) {
        rotated[r] = rot[r * 3] * dir[0] + rot[r * 3 + 1] * dir[1] + rot[r * 3 + 2] * dir[2];
    }
    const double dirSpill = rotated[1] - expandScale * limitOf(kind, weights, rotated[0], rotated[2]);
    if (!(dirSpill > 1e-9)) {
        // The limit is at or above the screen channel even for the screen's own hue.
        return p;
    }
    p.spillScale = (float)(1. / dirSpill);

    double toXYZ[9];
    ColorMath::workingToXYZ(holder, toXYZ);
    double luma[3] = { toXYZ[3], toXYZ[4], toXYZ[5] };
    const double lumaSum = luma[0] + luma[1] + luma[2];
    if (lumaSum > 0.) {
        for (int i = 0; i < 3; ++i) {
            luma[i] /= lumaSum;
        }
    }
    const double dirLuma = luma[0] * dir[0] + luma[1] * dir[1] + luma[2] * dir[2];
    const double replaceLuma = luma[0] * replace[0] + luma[1] * replace[1] + luma[2] * replace[2];
    for (int i = 0; i < 3; ++i) {
        p.move[i] = (float)((replace[i] - replaceLuma) + restore * dirLuma - dir[i]);
    }
    p.hasAxis = true;

    return p;
} // DespillOp::bake

bool
DespillOp::isIdentity(const Params& params) const
{
    return (!params.hasAxis || (params.strength == 0.f)) && !params.spillMapAlpha;
}

float
DespillOp::spill(const Params& params,
                 const float* rgb)
{
    if (!params.hasAxis) {
        return 0.f;
    }
    const float r = rotatedChannel(params.rot, rgb);
    const float g = rotatedChannel(params.rot + 3, rgb);
    const float b = rotatedChannel(params.rot + 6, rgb);
    float lim;
    if (params.limitKind == eLimitKindMax) {
        lim = (r > b) ? r : b;
    } else if (params.limitKind == eLimitKindMin) {
        lim = (r < b) ? r : b;
    } else {
        lim = params.weights[0] * r;
        const float lb = params.weights[1] * b;
        lim = lim + lb;
    }
    const float scaled = params.expandScale * lim;
    const float m = g - scaled;
    // A NaN spill is none.
    const float positive = (m > 0.f) ? m : 0.f;

    return positive * params.spillScale;
}

float
DespillOp::apply(const Params& params,
                 float* rgb)
{
    const float map = spill(params, rgb);

    if (!params.hasAxis) {
        return map;
    }
    const float amount = map * params.strength;
    for (int c = 0; c < 3; ++c) {
        const float d = amount * params.move[c];
        rgb[c] = rgb[c] + d;
    }

    return map;
}

void
DespillOp::row(const Params& params,
               const RowIO& io) const
{
    const int nComps = io.nComps;

    if (nComps < 3) {
        std::copy(io.src[0], io.src[0] + io.width * nComps, io.dst);

        return;
    }
    bool process[4] = { false, false, false, false };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        float rgb[3] = { src[0], src[1], src[2] };
        const float map = apply(params, rgb);
        for (int c = 0; c < 3; ++c) {
            if (process[c]) {
                dst[c] = rgb[c];
            }
        }
        if ((nComps == 4) && process[3]) {
            dst[3] = params.spillMapAlpha ? map : src[3];
        }
    }
}

GpuOpRef
DespillOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    DespillGpuParams p = {};
    p.flags = (params.hasAxis ? kGpuFlagHasAxis : 0u) | (params.spillMapAlpha ? kGpuFlagSpillMapAlpha : 0u);
    p.limitKind = (uint32_t)params.limitKind;
    for (int i = 0; i < 9; ++i) {
        p.rot[i] = params.rot[i];
    }
    p.weights[0] = params.weights[0];
    p.weights[1] = params.weights[1];
    p.expandScale = params.expandScale;
    p.spillScale = params.spillScale;
    p.strength = params.strength;
    for (int i = 0; i < 3; ++i) {
        p.move[i] = params.move[i];
    }
    GpuOpRef ref = GpuOpRef::compiled((int)gpu::GpuKernelId::despill_wrapped, p);
    ref.acceptedComponents = (unsigned char)(GpuOpRef::componentsBit(3) | GpuOpRef::componentsBit(4));

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Despill::Despill(NodePtr node)
    : ImageNode<DespillOp>(node)
{
}

Despill::~Despill()
{
}

DespillOp::Params
Despill::bakeDespill(double time,
                     ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
Despill::despillRow(const DespillOp::Params& params,
                    const RowIO& io) const
{
    op().row(params, io);
}

void
Despill::addAcceptedComponents(int inputNb,
                               std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

StatusEnum
Despill::getPreferredMetadata(NodeMetadata& metadata)
{
    KnobChoicePtr map = op().spillMap.lock();

    if (map && (map->getValue() == DespillOp::eSpillMapAlpha)) {
        metadata.setNComps(-1, 4);
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
