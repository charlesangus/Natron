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

#include "LensDistortion.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/Transform/LensModels3DE.h"

NATRON_NAMESPACE_ENTER

namespace {
// Which models a knob belongs to, one bit per LensModels::ModelType.
constexpr unsigned
modelBit(LensModels::ModelType type)
{
    return 1u << (unsigned)type;
}

constexpr unsigned kNuke = modelBit(LensModels::eModelNuke);
constexpr unsigned kPFBarrel = modelBit(LensModels::eModelPFBarrel);
constexpr unsigned kClassic = modelBit(LensModels::eModel3DEClassic);
constexpr unsigned kAnamorphic6 = modelBit(LensModels::eModel3DEAnamorphic6);
constexpr unsigned kFisheye8 = modelBit(LensModels::eModel3DEFisheye8);
constexpr unsigned kStandard = modelBit(LensModels::eModel3DEStandard);
constexpr unsigned kAnamorphic4 = modelBit(LensModels::eModel3DEAnamorphic4);
constexpr unsigned kPanoTools = modelBit(LensModels::eModelPanoTools);
constexpr unsigned k3DE = kClassic | kAnamorphic6 | kFisheye8 | kStandard | kAnamorphic4;
constexpr unsigned kAnamorphic = kAnamorphic4 | kAnamorphic6;

// One model knob as openfx-misc's LensDistortion declares it: name, label and hint, dimensions,
// default, display range (the same on both dimensions), and the models that use it.
struct ParamDef {
    LensDistortion::ModelParam param;
    const char* name;
    const char* label;
    const char* hint;
    int dimensions;
    double defaultValue[2];
    double displayMin;
    double displayMax;
    unsigned models;
};

const ParamDef kParamDefs[] = {
    { LensDistortion::eParamK1, kLensDistortionParamK1, "K1", "Nuke: First radial distortion coefficient (coefficient for r^2).", 1, { 0., 0. }, -0.3, 0.3, kNuke },
    { LensDistortion::eParamK2, kLensDistortionParamK2, "K2", "Nuke: Second radial distortion coefficient (coefficient for r^4).", 1, { 0., 0. }, -0.1, 0.1, kNuke },
    { LensDistortion::eParamCenter, kLensDistortionParamCenter, "Center", "Nuke: Offset of the distortion center from the image center.", 2, { 0., 0. }, -1., 1., kNuke },
    { LensDistortion::eParamSqueeze, kLensDistortionParamSqueeze, "Squeeze", "Nuke: Anamorphic squeeze (only for anamorphic lens). Values below 0.001 count as 0.001.", 1, { 1., 1. }, 0., 1., kNuke },
    { LensDistortion::eParamAsymmetric, kLensDistortionParamAsymmetric, "Asymmetric", "Nuke: Asymmetric distortion (only for anamorphic lens).", 2, { 0., 0. }, -0.5, 0.5, kNuke },
    { LensDistortion::eParamPFC3, kLensDistortionParamPFC3, "C3", "PFBarrel: Low order radial distortion coefficient.", 1, { 0., 0. }, -0.5, 0.5, kPFBarrel },
    { LensDistortion::eParamPFC5, kLensDistortionParamPFC5, "C5", "PFBarrel: High order radial distortion coefficient.", 1, { 0., 0. }, -0.5, 0.5, kPFBarrel },
    { LensDistortion::eParamPFP, kLensDistortionParamPFP, "Center", "PFBarrel: The distortion center of the lens (specified as a factor rather than a pixel value).", 2, { 0.5, 0.5 }, 0., 1., kPFBarrel },
    { LensDistortion::eParamPFSqueeze, kLensDistortionParamPFSqueeze, "Squeeze", "PFBarrel: Anamorphic squeeze (only for anamorphic lens).", 1, { 1., 1. }, 0.1, 3., kPFBarrel },
    { LensDistortion::eParam3DEFovXA, kLensDistortionParam3DEFovXA, "fov left [unit coord]", "3DE4: Field of view.", 1, { 0., 0. }, 0., 1., k3DE },
    { LensDistortion::eParam3DEFovYA, kLensDistortionParam3DEFovYA, "fov bottom [unit coord]", "3DE4: Field of view.", 1, { 0., 0. }, 0., 1., k3DE },
    { LensDistortion::eParam3DEFovXB, kLensDistortionParam3DEFovXB, "fov right [unit coord]", "3DE4: Field of view.", 1, { 1., 1. }, 0., 1., k3DE },
    { LensDistortion::eParam3DEFovYB, kLensDistortionParam3DEFovYB, "fov top [unit coord]", "3DE4: Field of view.", 1, { 1., 1. }, 0., 1., k3DE },
    { LensDistortion::eParam3DEFocalLength, kLensDistortionParam3DEFocalLength, "tde4 focal length [cm]", "3DE4: Focal length.", 1, { 1., 1. }, 0.5, 50., k3DE },
    { LensDistortion::eParam3DEFocusDistance, kLensDistortionParam3DEFocusDistance, "tde4 focus distance [cm]", "3DE4: Focus distance.", 1, { 100., 100. }, 10., 1000., k3DE },
    { LensDistortion::eParam3DEFilmbackWidth, kLensDistortionParam3DEFilmbackWidth, "tde4 filmback width [cm]", "3DE4: Filmback width.", 1, { 0.8, 0.8 }, 0.1, 10., k3DE },
    { LensDistortion::eParam3DEFilmbackHeight, kLensDistortionParam3DEFilmbackHeight, "tde4 filmback height [cm]", "3DE4: Filmback height.", 1, { 0.6, 0.6 }, 0.1, 10., k3DE },
    { LensDistortion::eParam3DELensCenterOffsetX, kLensDistortionParam3DELensCenterOffsetX, "tde4 lens center offset x [cm]", "3DE4: Lens center horizontal offset.", 1, { 0., 0. }, -5., 5., k3DE },
    { LensDistortion::eParam3DELensCenterOffsetY, kLensDistortionParam3DELensCenterOffsetY, "tde4 lens center offset y [cm]", "3DE4: Lens center vertical offset.", 1, { 0., 0. }, -5., 5., k3DE },
    { LensDistortion::eParam3DEPixelAspect, kLensDistortionParam3DEPixelAspect, "tde4 pixel aspect", "3DE4: Pixel aspect ratio.", 1, { 1., 1. }, 0.25, 4., k3DE },
    { LensDistortion::eParam3DEDistortion, kLensDistortionParam3DEDistortion, "Distortion", "3DE Classic: Distortion.", 1, { 0., 0. }, -0.5, 0.5, kClassic },
    { LensDistortion::eParam3DEAnamorphicSqueeze, kLensDistortionParam3DEAnamorphicSqueeze, "Anamorphic Squeeze", "3DE Classic: Anamorphic Squeeze.", 1, { 1., 1. }, 0.25, 4., kClassic },
    { LensDistortion::eParam3DECurvatureX, kLensDistortionParam3DECurvatureX, "Curvature X", "3DE Classic: Curvature X.", 1, { 0., 0. }, -0.5, 0.5, kClassic },
    { LensDistortion::eParam3DECurvatureY, kLensDistortionParam3DECurvatureY, "Curvature Y", "3DE Classic: Curvature Y.", 1, { 0., 0. }, -0.5, 0.5, kClassic },
    { LensDistortion::eParam3DEQuarticDistortion, kLensDistortionParam3DEQuarticDistortion, "Quartic Distortion", "3DE Classic: Quartic Distortion.", 1, { 0., 0. }, -0.5, 0.5, kClassic },
    { LensDistortion::eParam3DEDistortionDegree2, kLensDistortionParam3DEDistortionDegree2, "Distortion - Degree 2", "3DE Standard and Fisheye: Distortion.", 1, { 0., 0. }, -0.5, 0.5, kStandard | kFisheye8 },
    { LensDistortion::eParam3DEUDegree2, kLensDistortionParam3DEUDegree2, "U - Degree 2", "3DE Standard: U - Degree 2.", 1, { 0., 0. }, -0.5, 0.5, kStandard },
    { LensDistortion::eParam3DEVDegree2, kLensDistortionParam3DEVDegree2, "V - Degree 2", "3DE Standard: V - Degree 2.", 1, { 0., 0. }, -0.5, 0.5, kStandard },
    { LensDistortion::eParam3DEQuarticDistortionDegree4, kLensDistortionParam3DEQuarticDistortionDegree4, "Quartic Distortion - Degree 4", "3DE Standard and Fisheye: Quartic Distortion - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kStandard | kFisheye8 },
    { LensDistortion::eParam3DEUDegree4, kLensDistortionParam3DEUDegree4, "U - Degree 4", "3DE Standard: U - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kStandard },
    { LensDistortion::eParam3DEVDegree4, kLensDistortionParam3DEVDegree4, "V - Degree 4", "3DE Standard: V - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kStandard },
    { LensDistortion::eParam3DEPhiCylindricDirection, kLensDistortionParam3DEPhiCylindricDirection, "Phi - Cylindric Direction", "3DE Standard: Phi - Cylindric Direction.", 1, { 0., 0. }, -90., 90., kStandard },
    { LensDistortion::eParam3DEBCylindricBending, kLensDistortionParam3DEBCylindricBending, "B - Cylindric Bending", "3DE Standard: B - Cylindric Bending.", 1, { 0., 0. }, -0.1, 0.1, kStandard },
    { LensDistortion::eParam3DECx02, kLensDistortionParam3DECx02, "Cx02 - Degree 2", "3DE Anamorphic 4 and 6: Cx02 - Degree 2.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECy02, kLensDistortionParam3DECy02, "Cy02 - Degree 2", "3DE Anamorphic 4 and 6: Cy02 - Degree 2.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECx22, kLensDistortionParam3DECx22, "Cx22 - Degree 2", "3DE Anamorphic 4 and 6: Cx22 - Degree 2.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECy22, kLensDistortionParam3DECy22, "Cy22 - Degree 2", "3DE Anamorphic 4 and 6: Cy22 - Degree 2.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECx04, kLensDistortionParam3DECx04, "Cx04 - Degree 4", "3DE Anamorphic 4 and 6: Cx04 - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECy04, kLensDistortionParam3DECy04, "Cy04 - Degree 4", "3DE Anamorphic 4 and 6: Cy04 - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECx24, kLensDistortionParam3DECx24, "Cx24 - Degree 4", "3DE Anamorphic 4 and 6: Cx24 - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECy24, kLensDistortionParam3DECy24, "Cy24 - Degree 4", "3DE Anamorphic 4 and 6: Cy24 - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECx44, kLensDistortionParam3DECx44, "Cx44 - Degree 4", "3DE Anamorphic 4 and 6: Cx44 - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECy44, kLensDistortionParam3DECy44, "Cy44 - Degree 4", "3DE Anamorphic 4 and 6: Cy44 - Degree 4.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic },
    { LensDistortion::eParam3DECx06, kLensDistortionParam3DECx06, "Cx06 - Degree 6", "3DE Anamorphic 6: Cx06 - Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic6 },
    { LensDistortion::eParam3DECy06, kLensDistortionParam3DECy06, "Cy06 - Degree 6", "3DE Anamorphic 6: Cy06 - Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic6 },
    { LensDistortion::eParam3DECx26, kLensDistortionParam3DECx26, "Cx26 - Degree 6", "3DE Anamorphic 6: Cx26 - Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic6 },
    { LensDistortion::eParam3DECy26, kLensDistortionParam3DECy26, "Cy26 - Degree 6", "3DE Anamorphic 6: Cy26 - Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic6 },
    { LensDistortion::eParam3DECx46, kLensDistortionParam3DECx46, "Cx46 - Degree 6", "3DE Anamorphic 6: Cx46 - Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic6 },
    { LensDistortion::eParam3DECy46, kLensDistortionParam3DECy46, "Cy46 - Degree 6", "3DE Anamorphic 6: Cy46 - Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic6 },
    { LensDistortion::eParam3DECx66, kLensDistortionParam3DECx66, "Cx66 - Degree 6", "3DE Anamorphic 6: Cx66 - Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic6 },
    { LensDistortion::eParam3DECy66, kLensDistortionParam3DECy66, "Cy66 - Degree 6", "3DE Anamorphic 6: Cy66 - Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kAnamorphic6 },
    { LensDistortion::eParam3DELensRotation, kLensDistortionParam3DELensRotation, "Lens Rotation 4", "3DE Anamorphic 4: Lens Rotation 4.", 1, { 0., 0. }, -2., 2., kAnamorphic4 },
    { LensDistortion::eParam3DESqueezeX, kLensDistortionParam3DESqueezeX, "Squeeze-X", "3DE Anamorphic 4: Squeeze-X.", 1, { 1., 1. }, 0.9, 1.1, kAnamorphic4 },
    { LensDistortion::eParam3DESqueezeY, kLensDistortionParam3DESqueezeY, "Squeeze-Y", "3DE Anamorphic 4: Squeeze-Y.", 1, { 1., 1. }, 0.9, 1.1, kAnamorphic4 },
    { LensDistortion::eParam3DEDegree6, kLensDistortionParam3DEDegree6, "Degree 6", "3DE Fisheye: Degree 6.", 1, { 0., 0. }, -0.5, 0.5, kFisheye8 },
    { LensDistortion::eParam3DEDegree8, kLensDistortionParam3DEDegree8, "Degree 8", "3DE Fisheye: Degree 8.", 1, { 0., 0. }, -0.5, 0.5, kFisheye8 },
    { LensDistortion::eParamPanoToolsA, kLensDistortionParamPanoToolsA, "a", "PanoTools: Radial lens distortion 3rd degree coefficient a.", 1, { 0., 0. }, -0.2, 0.2, kPanoTools },
    { LensDistortion::eParamPanoToolsB, kLensDistortionParamPanoToolsB, "b", "PanoTools: Radial lens distortion 2nd degree coefficient b.", 1, { 0., 0. }, -0.2, 0.2, kPanoTools },
    { LensDistortion::eParamPanoToolsC, kLensDistortionParamPanoToolsC, "c", "PanoTools: Radial lens distortion 1st degree coefficient c.", 1, { 0., 0. }, -0.2, 0.2, kPanoTools },
    { LensDistortion::eParamPanoToolsD, kLensDistortionParamPanoToolsD, "d", "PanoTools: Horizontal lens shift (in pixels).", 1, { 0., 0. }, -1000., 1000., kPanoTools },
    { LensDistortion::eParamPanoToolsE, kLensDistortionParamPanoToolsE, "e", "PanoTools: Vertical lens shift (in pixels).", 1, { 0., 0. }, -1000., 1000., kPanoTools },
    { LensDistortion::eParamPanoToolsG, kLensDistortionParamPanoToolsG, "g", "PanoTools: Vertical lens shear (in pixels). Use to remove slight misalignment of the line scanner relative to the film transport.", 1, { 0., 0. }, -1000., 1000., kPanoTools },
    { LensDistortion::eParamPanoToolsT, kLensDistortionParamPanoToolsT, "t", "PanoTools: Horizontal lens shear (in pixels).", 1, { 0., 0. }, -1000., 1000., kPanoTools },
};

static_assert(sizeof(kParamDefs) / sizeof(kParamDefs[0]) == (std::size_t)LensDistortion::eParamCount, "one definition per model knob");

// Where each knob's models are looked up by the knob's index.
unsigned
paramModels(int param)
{
    for (const ParamDef& def : kParamDefs) {
        if ((int)def.param == param) {
            return def.models;
        }
    }

    return 0;
}

void
setSecretAndDisabled(const KnobIPtr& knob,
                     bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

// The region of definition of input in canonical coordinates at time, view and scale.
bool
inputRegion(const EffectInstancePtr& input,
            double time,
            ViewIdx view,
            const RenderScale& scale,
            RectD* rod)
{
    if (!input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) != eStatusFailed;
}

ExtentPolicy
lensDistortionExtentPolicy()
{
    ExtentPolicy policy;

    policy.defaultExtent = ExtentKnobs::eExtentDefault;
    policy.reformatToggle = false;
    policy.overlayFollowsExtent = true;
    policy.recenterOnSource = true;

    return policy;
}

// Points along the edges of rect, 11 per edge as openfx-misc samples them, mapped by model in
// direction; the bounding box of those the model maps. False when it maps none.
bool
mappedBoundingBox(const LensModels::Model& model,
                  LensModels::Direction direction,
                  const RectD& rect,
                  RectD* box)
{
    const int steps = 10;
    bool any = false;
    const auto add = [&](double x, double y) {
        LensModels::Sample s;
        if (!model.map(direction, x, y, &s) || !std::isfinite(s.x) || !std::isfinite(s.y)) {
            return;
        }
        if (!any) {
            *box = RectD(s.x, s.y, s.x, s.y);
            any = true;
        } else {
            box->x1 = std::min(box->x1, s.x);
            box->y1 = std::min(box->y1, s.y);
            box->x2 = std::max(box->x2, s.x);
            box->y2 = std::max(box->y2, s.y);
        }
    };
    for (int i = 0; i <= steps; ++i) {
        const double x = rect.x1 + (rect.x2 - rect.x1) * i / steps;
        add(x, rect.y1);
        add(x, rect.y2);
    }
    for (int i = 1; i < steps; ++i) {
        const double y = rect.y1 + (rect.y2 - rect.y1) * i / steps;
        add(rect.x1, y);
        add(rect.x2, y);
    }

    return any;
}
} // namespace

LensDistortion::LensDistortion(NodePtr node)
    : StagedImageEffect(node)
    , _extentKnobs(this, lensDistortionExtentPolicy())
{
}

LensDistortion::~LensDistortion()
{
}

const NodeSpec*
LensDistortion::nodeSpec() const
{
    return staticNodeSpec<kLensDistortionNodeSpec>();
}

const ImagePipelineSpec&
LensDistortion::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kLensDistortionPipeline, kLensDistortionNodeSpec>();
}

std::string
LensDistortion::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kLensDistortionInputSource:
        return tr("The image to distort or undistort. Its format is the default reference format.").toStdString();
    case kLensDistortionInputMask:
        return tr("The mask limiting where the distortion applies.").toStdString();
    default:
        break;
    }

    return std::string();
}

void
LensDistortion::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    // The frame range knob is kept for parity only: the node has no time domain of its own.
    _extentKnobs.createKnobs(page);

    KnobDoublePtr overscan = createKnob<KnobDouble>(tr("Overscan"));
    overscan->setName(kLensDistortionParamOverscan);
    overscan->setHintToolTip(tr("How much larger than the reference format the output format is, in percent of its width and "
                                "height, split evenly between opposite sides. The lens model stays defined over the reference "
                                "format; the overscan holds what distortion moves outside of it."));
    overscan->setDefaultValue(0.);
    overscan->setMinimum(0.);
    overscan->setDisplayMinimum(0.);
    overscan->setDisplayMaximum(50.);
    overscan->setAnimationEnabled(false);
    overscan->setIsMetadataSlave(true);
    page->addKnob(overscan);
    _overscan = overscan;

    KnobChoicePtr model = createKnob<KnobChoice>(tr("Model"));
    model->setName(kLensDistortionParamModel);
    model->setHintToolTip(tr("Choice of the distortion model, i.e. the function that goes from distorted to undistorted image "
                             "coordinates."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLensDistortionModelNuke, "Nuke", tr("The model used in Nuke's LensDistortion plugin.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionModelPFBarrel, "PFBarrel", tr("The PFBarrel model used in PFTrack by PixelFarm.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionModel3DEClassic, "3DE Classic", tr("Degree-2 anamorphic and degree-4 radial mixed model, used in 3DEqualizer by Science-D-Visions. Works, but it is recommended to use 3DE4 Radial Standard Degree 4 or 3DE4 Anamorphic Standard Degree 4 instead.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionModel3DEAnamorphic6, "3DE4 Anamorphic Degree 6", tr("Degree-6 anamorphic model, used in 3DEqualizer by Science-D-Visions.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionModel3DEFisheye8, "3DE4 Radial Fisheye Degree 8", tr("Radial lens distortion model with equisolid-angle fisheye projection, used in 3DEqualizer by Science-D-Visions. Rays at 90 degrees or more from the axis have no image.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionModel3DEStandard, "3DE4 Radial Standard Degree 4", tr("Radial lens distortion model, a.k.a. radial decentered cylindric degree 4, which compensates for decentered lenses (and beam splitter artefacts in stereo rigs), used in 3DEqualizer by Science-D-Visions.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionModel3DEAnamorphic4, "3DE4 Anamorphic Standard Degree 4", tr("Degree-4 anamorphic model with anamorphic lens rotation, which handles 'human-touched' mounted anamorphic lenses, used in 3DEqualizer by Science-D-Visions.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionModelPanoTools, "PanoTools", tr("The model used in PanoTools, PTGui, PTAssembler, Hugin. See http://wiki.panotools.org/Lens_correction_model").toStdString()));
        model->populateChoices(options);
    }
    model->setDefaultValue((int)LensModels::eModelNuke);
    model->setAnimationEnabled(false);
    page->addKnob(model);
    _model = model;

    KnobChoicePtr direction = createKnob<KnobChoice>(tr("Direction"));
    direction->setName(kLensDistortionParamDirection);
    direction->setHintToolTip(tr("Should the output correspond to applying or to removing distortion."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLensDistortionDirectionDistort, "Distort", tr("The output corresponds to applying distortion.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionDirectionUndistort, "Undistort", tr("The output corresponds to removing distortion.").toStdString()));
        direction->populateChoices(options);
    }
    direction->setDefaultValue((int)LensModels::eDirectionDistort);
    direction->setAnimationEnabled(false);
    page->addKnob(direction);
    _direction = direction;

    KnobChoicePtr outputMode = createKnob<KnobChoice>(tr("Output Mode"));
    outputMode->setName(kLensDistortionParamOutputMode);
    outputMode->setHintToolTip(tr("Choice of the output, which may be either a distorted/undistorted image, or a "
                                  "distortion/undistortion STMap."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLensDistortionOutputImage, "Image", tr("The output is the distorted/undistorted Source.").toStdString()));
        options.push_back(ChoiceOption(kLensDistortionOutputSTMap, "STMap", tr("The output is a distortion/undistortion STMap: red and green hold the normalized position in the Source format each pixel reads, alpha is 1 where the model has a solution and 0 elsewhere. It is recommended to insert a FrameHold node at the Source input so that the STMap is computed only once if the parameters are not animated.").toStdString()));
        outputMode->populateChoices(options);
    }
    outputMode->setDefaultValue(0);
    outputMode->setAnimationEnabled(false);
    outputMode->setIsMetadataSlave(true);
    page->addKnob(outputMode);
    _outputMode = outputMode;

    for (const ParamDef& def : kParamDefs) {
        if (def.param == eParamPFC3) {
            // The PFBarrel file comes before its coefficients, as in openfx-misc.
            KnobFilePtr pfFile = createKnob<KnobFile>(tr("File"));
            pfFile->setName(kLensDistortionParamPFFile);
            pfFile->setHintToolTip(tr("The location of the PFBarrel .pfb file to use. Choosing a file sets C3, C5, Center and "
                                      "Squeeze from it, keyframed when the file holds more than one key."));
            pfFile->setAnimationEnabled(false);
            page->addKnob(pfFile);
            _pfFile = pfFile;
        }
        KnobDoublePtr knob = createKnob<KnobDouble>(tr(def.label), def.dimensions);
        knob->setName(def.name);
        knob->setHintToolTip(tr(def.hint));
        for (int d = 0; d < def.dimensions; ++d) {
            knob->setDefaultValue(def.defaultValue[d], d);
            knob->setDisplayMinimum(def.displayMin, d);
            knob->setDisplayMaximum(def.displayMax, d);
        }
        if (def.param == eParamPFSqueeze) {
            knob->setMinimum(0.);
        }
        page->addKnob(knob);
        _params[def.param] = knob;
    }

    KnobChoicePtr filter = createKnob<KnobChoice>(tr(kResamplerParamFilterTypeLabel));
    filter->setName(kResamplerParamFilterType);
    filter->setHintToolTip(tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)Resampler::eFilterCubic);
    filter->setAddNewLine(false);
    filter->setSpacingBetweenItems(1);
    page->addKnob(filter);
    _filter = filter;

    KnobBoolPtr clamp = createKnob<KnobBool>(tr(kResamplerParamFilterClampLabel));
    clamp->setName(kResamplerParamFilterClamp);
    clamp->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clamp->setDefaultValue(false);
    clamp->setAddNewLine(false);
    clamp->setSpacingBetweenItems(1);
    page->addKnob(clamp);
    _clamp = clamp;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutside->setName(kResamplerParamFilterBlackOutside);
    blackOutside->setHintToolTip(tr(kResamplerParamFilterBlackOutsideHint));
    blackOutside->setDefaultValue(true);
    page->addKnob(blackOutside);
    _blackOutside = blackOutside;

    KnobBoolPtr cropToFormat = createKnob<KnobBool>(tr("Crop To Format"));
    cropToFormat->setName(kLensDistortionParamCropToFormat);
    cropToFormat->setHintToolTip(tr("If the source is inside its format and the distortion extends it outside of the output "
                                    "format, crop it there to avoid unnecessary calculations. To avoid unwanted crops, only the "
                                    "borders that were inside of the format in the Source are cropped."));
    cropToFormat->setDefaultValue(true);
    cropToFormat->setAddNewLine(false);
    cropToFormat->setSpacingBetweenItems(1);
    page->addKnob(cropToFormat);
    _cropToFormat = cropToFormat;

    KnobBoolPtr useRoD = createKnob<KnobBool>(tr("Use Source RoD"));
    useRoD->setName(kLensDistortionParamUseRoD);
    useRoD->setHintToolTip(tr("Use the region of definition of the Source as the Source format: the default reference format, "
                              "and the box STMap output normalizes against."));
    useRoD->setDefaultValue(false);
    useRoD->setAnimationEnabled(false);
    useRoD->setIsMetadataSlave(true);
    page->addKnob(useRoD);
    _useRoD = useRoD;

    addStageKnobs(page);
    _extentKnobs.finishKnobs();
    updateModelVisibility();
} // LensDistortion::initializeKnobs

void
LensDistortion::updateModelVisibility()
{
    KnobChoicePtr model = _model.lock();
    const int type = model ? model->getValue() : 0;
    const unsigned bit = 1u << (unsigned)std::max(0, type);

    for (int i = 0; i < eParamCount; ++i) {
        setSecretAndDisabled(_params[i].lock(), !(paramModels(i) & bit));
    }
    setSecretAndDisabled(_pfFile.lock(), !(kPFBarrel & bit));
}

void
LensDistortion::onKnobsLoaded()
{
    _extentKnobs.updateVisibility();
    updateModelVisibility();
}

bool
LensDistortion::knobChanged(KnobI* k,
                            ValueChangedReasonEnum reason,
                            ViewSpec /*view*/,
                            double time,
                            bool /*originatedFromMainThread*/)
{
    if (_extentKnobs.onKnobChanged(k, reason, time)) {
        return true;
    }
    KnobChoicePtr model = _model.lock();
    if (model && (k == model.get())) {
        updateModelVisibility();

        return true;
    }
    KnobFilePtr pfFile = _pfFile.lock();
    if (pfFile && (k == pfFile.get())) {
        // Only a user's choice of file loads it, so that loading a project keeps the keys it saved.
        if (reason == eValueChangedReasonUserEdited) {
            loadPFBarrelFile();
        }

        return true;
    }

    return false;
}

bool
LensDistortion::loadPFBarrelFile()
{
    KnobFilePtr pfFile = _pfFile.lock();
    if (!pfFile) {
        return false;
    }
    const std::string path = pfFile->getFileName(0, ViewIdx(0));
    if (path.empty()) {
        return false;
    }
    std::ifstream stream(path.c_str());
    if (!stream) {
        message(eMessageTypeError, tr("Error reading file \"%1\": cannot open it.").arg(QString::fromStdString(path)).toStdString());

        return false;
    }
    std::stringstream text;
    text << stream.rdbuf();
    LensModels::PFBarrelFile file;
    std::string error;
    if (!LensModels::parsePFBarrelFile(text.str(), &file, &error)) {
        message(eMessageTypeError, tr("Error reading file \"%1\": %2").arg(QString::fromStdString(path)).arg(QString::fromStdString(error)).toStdString());

        return false;
    }

    KnobDoublePtr c3 = _params[eParamPFC3].lock();
    KnobDoublePtr c5 = _params[eParamPFC5].lock();
    KnobDoublePtr center = _params[eParamPFP].lock();
    KnobDoublePtr squeeze = _params[eParamPFSqueeze].lock();
    if (!c3 || !c5 || !center || !squeeze) {
        return false;
    }
    beginChanges();
    static_cast<KnobI*>(c3.get())->removeAnimation(ViewSpec::all(), 0);
    static_cast<KnobI*>(c5.get())->removeAnimation(ViewSpec::all(), 0);
    static_cast<KnobI*>(center.get())->removeAnimation(ViewSpec::all(), 0);
    static_cast<KnobI*>(center.get())->removeAnimation(ViewSpec::all(), 1);
    // Unlike openfx-misc, which reads the squeeze but leaves the knob alone, and keys every frame
    // with the first key's values.
    squeeze->setValue(file.squeeze);
    if (file.keys.size() == 1) {
        const LensModels::PFBarrelKey& key = file.keys[0];
        c3->setValue(key.c3);
        c5->setValue(key.c5);
        center->setValue(key.centerX, ViewSpec::all(), 0);
        center->setValue(key.centerY, ViewSpec::all(), 1);
    } else {
        // A low-order file holds no C5; parsePFBarrelFile() gives 0 for it.
        for (const LensModels::PFBarrelKey& key : file.keys) {
            c3->setValueAtTime(key.frame, key.c3, ViewSpec::all(), 0);
            c5->setValueAtTime(key.frame, key.c5, ViewSpec::all(), 0);
            center->setValueAtTime(key.frame, key.centerX, ViewSpec::all(), 0);
            center->setValueAtTime(key.frame, key.centerY, ViewSpec::all(), 1);
        }
    }
    endChanges();

    return true;
} // LensDistortion::loadPFBarrelFile

void
LensDistortion::readSettings(double time,
                             ViewIdx view,
                             Settings* settings) const
{
    KnobChoicePtr direction = _direction.lock();
    if (direction) {
        settings->direction = (direction->getValueAtTime(time, 0, view) == 1) ? LensModels::eDirectionUndistort : LensModels::eDirectionDistort;
    }
    KnobChoicePtr outputMode = _outputMode.lock();
    if (outputMode) {
        settings->stmap = (outputMode->getValueAtTime(time, 0, view) == 1);
    }
    KnobChoicePtr filter = _filter.lock();
    if (filter) {
        settings->filter = (Resampler::FilterEnum)filter->getValueAtTime(time, 0, view);
    }
    KnobBoolPtr clamp = _clamp.lock();
    if (clamp) {
        settings->clamp = clamp->getValueAtTime(time, 0, view);
    }
    KnobBoolPtr blackOutside = _blackOutside.lock();
    if (blackOutside) {
        settings->blackOutside = blackOutside->getValueAtTime(time, 0, view);
    }
}

double
LensDistortion::paramValue(ModelParam param,
                           double time,
                           ViewIdx view,
                           int dimension) const
{
    KnobDoublePtr knob = _params[param].lock();
    if (!knob) {
        for (const ParamDef& def : kParamDefs) {
            if (def.param == param) {
                return def.defaultValue[dimension];
            }
        }

        return 0.;
    }

    return knob->getValueAtTime(time, dimension, view);
}

void
LensDistortion::getGeometry(double time,
                            ViewIdx view,
                            Geometry* geometry)
{
    *geometry = Geometry();

    EffectInstancePtr source = getInput(kLensDistortionInputSource);
    KnobBoolPtr useRoDKnob = _useRoD.lock();
    const bool useRoD = useRoDKnob && useRoDKnob->getValue();
    if (source) {
        geometry->sourcePar = source->getAspectRatio(-1);
        if (geometry->sourcePar <= 0.) {
            geometry->sourcePar = 1.;
        }
        const RectI format = source->getOutputFormat();
        RectD rod;
        if (!useRoD && !format.isNull()) {
            geometry->sourceFormat = RectD(format.x1, format.y1, format.x2, format.y2);
        } else if (inputRegion(source, time, view, RenderScale::identity, &rod) && !rod.isNull()) {
            geometry->sourceFormat = RectD(rod.x1 / geometry->sourcePar, rod.y1, rod.x2 / geometry->sourcePar, rod.y2);
        }
    }

    // The reference format in canonical coordinates, or directly in lens pixels for the Source format.
    RectD canonical;
    double par = 1.;
    bool inPixels = false;
    switch (_extentKnobs.getExtent()) {
    case ExtentKnobs::eExtentFormat: {
        KnobIntPtr formatSize = _extentKnobs.getFormatSizeKnob();
        KnobDoublePtr formatPar = _extentKnobs.getFormatParKnob();
        par = formatPar ? formatPar->getValue() : 1.;
        geometry->reference = RectD(0., 0., formatSize ? formatSize->getValue(0) : 0, formatSize ? formatSize->getValue(1) : 0);
        inPixels = true;
        break;
    }
    case ExtentKnobs::eExtentSize: {
        KnobDoublePtr size = _extentKnobs.getSizeKnob();
        KnobDoublePtr bottomLeft = _extentKnobs.getBottomLeftKnob();
        if (source) {
            par = geometry->sourcePar;
        } else {
            _extentKnobs.getProjectExtentRect(&par);
        }
        if (size && bottomLeft) {
            canonical.x1 = bottomLeft->getValueAtTime(time, 0, view);
            canonical.y1 = bottomLeft->getValueAtTime(time, 1, view);
            canonical.x2 = canonical.x1 + size->getValueAtTime(time, 0, view);
            canonical.y2 = canonical.y1 + size->getValueAtTime(time, 1, view);
        }
        break;
    }
    case ExtentKnobs::eExtentProject:
        canonical = _extentKnobs.getProjectExtentRect(&par);
        break;
    case ExtentKnobs::eExtentDefault:
        if (!geometry->sourceFormat.isNull()) {
            geometry->reference = geometry->sourceFormat;
            par = geometry->sourcePar;
            inPixels = true;
        } else {
            canonical = _extentKnobs.getProjectExtentRect(&par);
        }
        break;
    }
    if (!(par > 0.)) {
        par = 1.;
    }
    geometry->par = par;
    if (!inPixels) {
        geometry->reference = RectD(canonical.x1 / par, canonical.y1, canonical.x2 / par, canonical.y2);
    }

    KnobDoublePtr overscanKnob = _overscan.lock();
    const double overscan = overscanKnob ? std::max(0., overscanKnob->getValue()) : 0.;
    const RectD& reference = geometry->reference;
    const int padX = (int)std::floor(reference.width() * overscan / 200. + 0.5);
    const int padY = (int)std::floor(reference.height() * overscan / 200. + 0.5);
    geometry->output = RectI((int)std::floor(reference.x1 + 0.5) - padX,
                             (int)std::floor(reference.y1 + 0.5) - padY,
                             (int)std::floor(reference.x2 + 0.5) + padX,
                             (int)std::floor(reference.y2 + 0.5) + padY);
} // LensDistortion::getGeometry

LensModels::Model
LensDistortion::buildModel(double time,
                           ViewIdx view,
                           const Geometry& geometry) const
{
    const LensModels::Frame frame = { geometry.reference.x1, geometry.reference.y1, geometry.reference.x2, geometry.reference.y2, geometry.par };
    const auto value = [&](ModelParam param, int dimension = 0) {
        return paramValue(param, time, view, dimension);
    };
    const auto camera = [&]() {
        LensModels::Camera3DE c;
        c.xa_fov_unit = value(eParam3DEFovXA);
        c.ya_fov_unit = value(eParam3DEFovYA);
        c.xb_fov_unit = value(eParam3DEFovXB);
        c.yb_fov_unit = value(eParam3DEFovYB);
        c.fl_cm = value(eParam3DEFocalLength);
        c.fd_cm = value(eParam3DEFocusDistance);
        c.w_fb_cm = value(eParam3DEFilmbackWidth);
        c.h_fb_cm = value(eParam3DEFilmbackHeight);
        c.x_lco_cm = value(eParam3DELensCenterOffsetX);
        c.y_lco_cm = value(eParam3DELensCenterOffsetY);
        c.pa = value(eParam3DEPixelAspect);

        return c;
    };

    KnobChoicePtr modelKnob = _model.lock();
    const int type = modelKnob ? modelKnob->getValueAtTime(time, 0, view) : 0;
    switch ((LensModels::ModelType)type) {
    case LensModels::eModelPFBarrel: {
        LensModels::PFBarrelParams p;
        p.c3 = value(eParamPFC3);
        p.c5 = value(eParamPFC5);
        p.centerX = value(eParamPFP, 0);
        p.centerY = value(eParamPFP, 1);
        p.squeeze = value(eParamPFSqueeze);

        return LensModels::makePFBarrel(frame, p);
    }
    case LensModels::eModel3DEClassic: {
        LensModels::Classic3DEParams p;
        p.ld = value(eParam3DEDistortion);
        p.sq = value(eParam3DEAnamorphicSqueeze);
        p.cx = value(eParam3DECurvatureX);
        p.cy = value(eParam3DECurvatureY);
        p.qu = value(eParam3DEQuarticDistortion);

        return LensModels::make3DEClassic(frame, camera(), p);
    }
    case LensModels::eModel3DEAnamorphic6: {
        LensModels::Anamorphic6Params p;
        p.cx02 = value(eParam3DECx02);
        p.cy02 = value(eParam3DECy02);
        p.cx22 = value(eParam3DECx22);
        p.cy22 = value(eParam3DECy22);
        p.cx04 = value(eParam3DECx04);
        p.cy04 = value(eParam3DECy04);
        p.cx24 = value(eParam3DECx24);
        p.cy24 = value(eParam3DECy24);
        p.cx44 = value(eParam3DECx44);
        p.cy44 = value(eParam3DECy44);
        p.cx06 = value(eParam3DECx06);
        p.cy06 = value(eParam3DECy06);
        p.cx26 = value(eParam3DECx26);
        p.cy26 = value(eParam3DECy26);
        p.cx46 = value(eParam3DECx46);
        p.cy46 = value(eParam3DECy46);
        p.cx66 = value(eParam3DECx66);
        p.cy66 = value(eParam3DECy66);

        return LensModels::make3DEAnamorphic6(frame, camera(), p);
    }
    case LensModels::eModel3DEFisheye8: {
        LensModels::Fisheye8Params p;
        p.c2 = value(eParam3DEDistortionDegree2);
        p.c4 = value(eParam3DEQuarticDistortionDegree4);
        p.c6 = value(eParam3DEDegree6);
        p.c8 = value(eParam3DEDegree8);

        return LensModels::make3DEFisheye8(frame, camera(), p);
    }
    case LensModels::eModel3DEStandard: {
        LensModels::Standard3DEParams p;
        p.c2 = value(eParam3DEDistortionDegree2);
        p.u1 = value(eParam3DEUDegree2);
        p.v1 = value(eParam3DEVDegree2);
        p.c4 = value(eParam3DEQuarticDistortionDegree4);
        p.u3 = value(eParam3DEUDegree4);
        p.v3 = value(eParam3DEVDegree4);
        p.phi = value(eParam3DEPhiCylindricDirection);
        p.b = value(eParam3DEBCylindricBending);

        return LensModels::make3DEStandard(frame, camera(), p);
    }
    case LensModels::eModel3DEAnamorphic4: {
        LensModels::Anamorphic4Params p;
        p.cx02 = value(eParam3DECx02);
        p.cy02 = value(eParam3DECy02);
        p.cx22 = value(eParam3DECx22);
        p.cy22 = value(eParam3DECy22);
        p.cx04 = value(eParam3DECx04);
        p.cy04 = value(eParam3DECy04);
        p.cx24 = value(eParam3DECx24);
        p.cy24 = value(eParam3DECy24);
        p.cx44 = value(eParam3DECx44);
        p.cy44 = value(eParam3DECy44);
        p.phi = value(eParam3DELensRotation);
        p.sqx = value(eParam3DESqueezeX);
        p.sqy = value(eParam3DESqueezeY);

        return LensModels::make3DEAnamorphic4(frame, camera(), p);
    }
    case LensModels::eModelPanoTools: {
        LensModels::PanoToolsParams p;
        p.a = value(eParamPanoToolsA);
        p.b = value(eParamPanoToolsB);
        p.c = value(eParamPanoToolsC);
        p.d = value(eParamPanoToolsD);
        p.e = value(eParamPanoToolsE);
        p.g = value(eParamPanoToolsG);
        p.t = value(eParamPanoToolsT);

        return LensModels::makePanoTools(frame, p);
    }
    case LensModels::eModelNuke:
    default: {
        LensModels::NukeParams p;
        p.k1 = value(eParamK1);
        p.k2 = value(eParamK2);
        p.centerX = value(eParamCenter, 0);
        p.centerY = value(eParamCenter, 1);
        p.anamorphicSqueeze = std::max(0.001, value(eParamSqueeze));
        p.asymmetricX = value(eParamAsymmetric, 0);
        p.asymmetricY = value(eParamAsymmetric, 1);

        return LensModels::makeNuke(frame, p);
    }
    }
} // LensDistortion::buildModel

Route
LensDistortion::route(double time,
                      ViewIdx view)
{
    Q_UNUSED(time);
    Q_UNUSED(view);
    if (!getInput(kLensDistortionInputSource)) {
        return Route::black();
    }

    return Route::none();
}

StatusEnum
LensDistortion::getPreferredMetadata(NodeMetadata& metadata)
{
    Geometry geometry;
    getGeometry(getCurrentTime(), ViewIdx(0), &geometry);
    if (!geometry.output.isNull()) {
        metadata.setOutputFormat(geometry.output);
    }
    metadata.setPixelAspectRatio(-1, geometry.par);

    KnobChoicePtr outputMode = _outputMode.lock();
    if (outputMode && (outputMode->getValue() == 1)) {
        metadata.setNComps(-1, 4);
    } else if (getInput(kLensDistortionInputSource)) {
        metadata.setNComps(-1, metadata.getNComps(kLensDistortionInputSource));
    }

    return eStatusOK;
}

StatusEnum
LensDistortion::getRegionOfDefinition(U64 /*hash*/,
                                      double time,
                                      const RenderScale& scale,
                                      ViewIdx view,
                                      RectD* rod)
{
    EffectInstancePtr source = getInput(kLensDistortionInputSource);
    if (!source) {
        rod->clear();

        return eStatusOK;
    }

    Geometry geometry;
    getGeometry(time, view, &geometry);
    Settings settings;
    readSettings(time, view, &settings);
    const double par = geometry.par;
    const RectD outputFormat(geometry.output.x1 * par, geometry.output.y1, geometry.output.x2 * par, geometry.output.y2);

    RectD sourceRoD;
    const bool hasSourceRoD = inputRegion(source, time, view, scale, &sourceRoD) && !sourceRoD.isNull();

    if (settings.stmap) {
        // Every output pixel holds a coordinate, or alpha 0.
        *rod = outputFormat;
    } else if (!hasSourceRoD) {
        rod->clear();
    } else {
        // Where the edges of the Source land, in lens pixels, as openfx-misc computes it.
        const LensModels::Model model = buildModel(time, view, geometry);
        const RectD sourceLens(sourceRoD.x1 / par, sourceRoD.y1, sourceRoD.x2 / par, sourceRoD.y2);
        RectD box;
        if (!mappedBoundingBox(model, settings.direction, sourceLens, &box)) {
            box = RectD(geometry.output.x1, geometry.output.y1, geometry.output.x2, geometry.output.y2);
        }
        const double margin = (settings.blackOutside ? 1. : 0.) + 2.;
        *rod = RectD((box.x1 - margin) * par, box.y1 - margin, (box.x2 + margin) * par, box.y2 + margin);

        KnobBoolPtr cropToFormat = _cropToFormat.lock();
        if (cropToFormat && cropToFormat->getValueAtTime(time, 0, view) && !geometry.sourceFormat.isNull()) {
            // Only the sides where the Source stayed inside its format are cropped.
            const double sourcePar = geometry.sourcePar;
            const RectD sourceFormat(geometry.sourceFormat.x1 * sourcePar, geometry.sourceFormat.y1, geometry.sourceFormat.x2 * sourcePar, geometry.sourceFormat.y2);
            if (sourceRoD.x1 >= sourceFormat.x1) {
                rod->x1 = std::max(rod->x1, outputFormat.x1);
            }
            if (sourceRoD.y1 >= sourceFormat.y1) {
                rod->y1 = std::max(rod->y1, outputFormat.y1);
            }
            if (sourceRoD.x2 <= sourceFormat.x2) {
                rod->x2 = std::min(rod->x2, outputFormat.x2);
            }
            if (sourceRoD.y2 <= sourceFormat.y2) {
                rod->y2 = std::min(rod->y2, outputFormat.y2);
            }
            if ((rod->x2 <= rod->x1) || (rod->y2 <= rod->y1)) {
                rod->clear();
            }
        }
    }

    // Under a mask or a partial mix the Source shows through outside the distorted region.
    if (hasSourceRoD && (isMaskApplied() || (getMixValue(time, view) != 1.))) {
        if (rod->isNull()) {
            *rod = sourceRoD;
        } else {
            rod->merge(sourceRoD);
        }
    }

    return eStatusOK;
} // LensDistortion::getRegionOfDefinition

void
LensDistortion::getRegionsOfInterest(double time,
                                     const RenderScale& scale,
                                     const RectD& outputRoD,
                                     const RectD& renderWindow,
                                     ViewIdx view,
                                     RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    Settings settings;
    readSettings(time, view, &settings);
    EffectInstancePtr source = getInput(kLensDistortionInputSource);
    if (!source || settings.stmap) {
        return;
    }
    // The Source is read wherever the model sends a pixel, and at the window for the blend.
    RectD sourceRoI = renderWindow;
    RectD sourceRoD;
    if (inputRegion(source, time, view, scale, &sourceRoD) && !sourceRoD.isNull()) {
        sourceRoI.merge(sourceRoD);
    }
    (*ret)[source] = sourceRoI;
}

bool
LensDistortion::isOpIdentity(double /*time*/,
                             const RenderScale& /*scale*/,
                             const RectI& /*roi*/,
                             ViewIdx /*view*/)
{
    return false;
}

StatusEnum
LensDistortion::renderOp(const RenderActionArgs& args,
                         const StagePlan& plan)
{
    Settings settings;
    readSettings(args.time, args.view, &settings);
    if (args.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    const bool jacobians = (settings.filter != Resampler::eFilterImpulse);

    Geometry geometry;
    getGeometry(args.time, args.view, &geometry);
    // Built here, on the calling thread; the band threads only read it.
    const LensModels::Model model = buildModel(args.time, args.view, geometry);
    // Each output pixel reads the Source where the opposite direction sends it.
    const LensModels::Direction inverse = (settings.direction == LensModels::eDirectionDistort) ? LensModels::eDirectionUndistort : LensModels::eDirectionDistort;

    const double scaleX = args.mappedScale.toOfxPointD().x;
    const double scaleY = args.mappedScale.toOfxPointD().y;
    // Lens pixels to Source pixels along x: both are canonical x over a pixel aspect ratio.
    const double toSourceX = geometry.par / geometry.sourcePar;
    const RectD sourceFormat = geometry.sourceFormat;
    const bool stmap = settings.stmap;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        ImagePtr src;
        ImageLayerDesc sourceLayer;
        if (!stmap && model.valid && resolveInputPlaneForRender(kLensDistortionInputSource, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            src = getImage(kLensDistortionInputSource, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (src && !PassExec::isFloatImage(src)) {
            return eStatusFailed;
        }

        // The whole Source in the output plane's layout, locked here, on the calling thread.
        RectI srcBounds;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::vector<float> converted;
        Resampler::SourceImage source((const float*)0, RectI(), nComps);
        if (src) {
            srcBounds = src->getBounds();
            const int srcNComps = (int)src->getComponentsCount();
            srcAccess = std::make_shared<Image::ReadAccess>(src.get());
            if (srcBounds.isNull()) {
                src.reset();
            } else if (srcNComps == nComps) {
                source = Resampler::SourceImage((const float*)srcAccess->pixelAt(srcBounds.x1, srcBounds.y1), srcBounds, nComps);
            } else {
                const int width = srcBounds.width();
                converted.resize((std::size_t)width * srcBounds.height() * nComps);
                for (int y = srcBounds.y1; y < srcBounds.y2; ++y) {
                    PassExec::readSourceRow(src.get(), srcAccess.get(), srcBounds, srcNComps, srcBounds.x1, y, width, nComps, &converted[(std::size_t)(y - srcBounds.y1) * width * nComps]);
                }
                source = Resampler::SourceImage(&converted[0], srcBounds, nComps);
            }
        }
        const bool sampled = (bool)src;

        const auto row = [&](const RowIO& io) {
            std::vector<float> out((std::size_t)io.width * io.nComps, 0.f);
            if (stmap && model.valid && !sourceFormat.isNull()) {
                // (u, v, 0, converged), the first io.nComps of them; a single channel is alpha.
                const double y = (io.y + 0.5) / scaleY;
                for (int i = 0; i < io.width; ++i) {
                    LensModels::Sample s;
                    const double x = (io.x0 + i + 0.5) / scaleX;
                    if (!model.map(inverse, x, y, &s) || !std::isfinite(s.x) || !std::isfinite(s.y)) {
                        continue;
                    }
                    const float values[4] = {
                        (float)((s.x * toSourceX - sourceFormat.x1) / sourceFormat.width()),
                        (float)((s.y - sourceFormat.y1) / sourceFormat.height()),
                        0.f,
                        1.f,
                    };
                    float* pixel = &out[(std::size_t)i * io.nComps];
                    if (io.nComps == 1) {
                        pixel[0] = values[3];
                    } else {
                        for (int c = 0; c < io.nComps; ++c) {
                            pixel[c] = values[c];
                        }
                    }
                }
            } else if (sampled) {
                // Source pixel coordinates at the render scale, NaN where the model has no
                // solution, which the resampler reads as black.
                const float nan = std::numeric_limits<float>::quiet_NaN();
                std::vector<float> sx(io.width);
                std::vector<float> sy(io.width);
                std::vector<float> jacobian;
                if (jacobians) {
                    jacobian.resize((std::size_t)4 * io.width);
                }
                const double y = (io.y + 0.5) / scaleY;
                for (int i = 0; i < io.width; ++i) {
                    LensModels::Sample s;
                    const double x = (io.x0 + i + 0.5) / scaleX;
                    if (!model.map(inverse, x, y, &s) || !std::isfinite(s.x) || !std::isfinite(s.y)) {
                        sx[i] = nan;
                        sy[i] = nan;
                        if (jacobians) {
                            std::fill(&jacobian[(std::size_t)4 * i], &jacobian[(std::size_t)4 * i + 4], 0.f);
                        }
                        continue;
                    }
                    sx[i] = (float)(s.x * toSourceX * scaleX);
                    sy[i] = (float)(s.y * scaleY);
                    if (jacobians) {
                        // d(source at scale) / d(output at scale), from the model's full-resolution one.
                        float* J = &jacobian[(std::size_t)4 * i];
                        J[0] = (float)(s.J[0] * toSourceX);
                        J[1] = (float)(s.J[1] * toSourceX * scaleX / scaleY);
                        J[2] = (float)(s.J[2] * scaleY / scaleX);
                        J[3] = (float)s.J[3];
                    }
                }
                Resampler::resampleRowFromCoords(source, &sx[0], &sy[0], jacobians ? &jacobian[0] : 0, io.width,
                                                 settings.filter, settings.clamp, settings.blackOutside, Resampler::eWrapNone, Resampler::eWrapNone,
                                                 &out[0], io.nComps);
            }
            for (int c = 0; c < io.nComps; ++c) {
                if (!io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                    continue;
                }
                for (int i = 0; i < io.width; ++i) {
                    io.dst[(std::size_t)i * io.nComps + c] = out[(std::size_t)i * io.nComps + c];
                }
            }
        };

        EffectInstance::RenderActionArgs planeArgs = args;
        planeArgs.outputLayers.clear();
        planeArgs.outputLayers.push_back(*it);
        const StatusEnum status = runStagesCpu(*this, plan, planeArgs, StageRowFn(row));
        if (status != eStatusOK) {
            return status;
        }
    }

    return eStatusOK;
} // LensDistortion::renderOp

NATRON_NAMESPACE_EXIT
