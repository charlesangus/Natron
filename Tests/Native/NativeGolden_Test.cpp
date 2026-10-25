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

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeGolden.h"
#include "NativeParity.h"

#include "Engine/Node.h"
#include "Engine/Nodes/Color/Clamp.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Color/ColorMathNode.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Color/Saturation.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Filter/EdgeExtend.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Filter/Sharpen.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Keyer/ChromaKeyer.h"
#include "Engine/Nodes/Keyer/Keyer.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Position.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Nodes/Transform/Transform.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::function<bool(const GoldenGraph&)> Apply;

bool
num(const GoldenGraph& g,
    const std::string& name,
    const std::vector<double>& values)
{
    return setKnobValues(g.node, name, values);
}

bool
choice(const GoldenGraph& g,
       const std::string& name,
       const std::string& id)
{
    return setKnobValue(g.node, name, id);
}

GoldenVariant
variant(const std::string& name,
        const Apply& apply = Apply())
{
    GoldenVariant v;

    v.name = name;
    v.apply = apply;

    return v;
}

GoldenVariant
maskVariant(const std::string& name,
            const Apply& apply)
{
    GoldenVariant v = variant(name, apply);

    v.mask = true;

    return v;
}

// The colour point-ops' shared tail: the given setting under "(Un)premult by" = A, and under a
// mask with mix.
void
addPremultAndMaskMix(std::vector<GoldenVariant>* variants,
                     const Apply& setting)
{
    variants->push_back(variant("premultA", [setting](const GoldenGraph& g) {
        return (!setting || setting(g)) && setGoldenChannelSelect(g.node, kUnPremultByKnobName, "rgba.A");
    }));
    variants->push_back(maskVariant("maskmix", [setting](const GoldenGraph& g) {
        return (!setting || setting(g)) && num(g, kOfxMixParamName, { 0.6 });
    }));
}

// A filter's mask-and-mix case, with the mask inverted so that the inversion is covered too.
GoldenVariant
filterMaskMix(const Apply& setting)
{
    return maskVariant("maskmix", [setting](const GoldenGraph& g) {
        return setting(g) && num(g, kOfxMixParamName, { 0.5 }) && num(g, kOfxMaskInvertParamName, { 1. });
    });
}

GoldenNodeSpec
filterSpec(const std::string& name,
           const std::string& id,
           int major)
{
    GoldenNodeSpec spec;

    spec.name = name;
    spec.pluginID = id;
    spec.major = major;
    spec.maskInput = "Mask";

    return spec;
}

GoldenNodeSpec
plainSpec(const std::string& name,
          const std::string& id,
          int major)
{
    GoldenNodeSpec spec;

    spec.name = name;
    spec.pluginID = id;
    spec.major = major;

    return spec;
}

GoldenNodeSpec
generatorSpec(const std::string& name,
              const std::string& id,
              int major)
{
    GoldenNodeSpec spec = plainSpec(name, id, major);

    spec.generator = true;

    return spec;
}

bool
editColorLookupCurves(const GoldenGraph& g)
{
    return num(g, kColorLookupParamSource, { 0.2, 0.3, 0.4, 0.5 }) && num(g, kColorLookupParamTarget, { 0.6, 0.25, 0.8, 0.9 }) && pressGoldenButton(g.node, kColorLookupParamSetRGB) && pressGoldenButton(g.node, kColorLookupParamSetA) && num(g, kColorLookupParamSource, { 0.7, 0.7, 0.7, 0.7 }) && num(g, kColorLookupParamTarget, { 0.55, 0.55, 0.55, 0.55 }) && pressGoldenButton(g.node, kColorLookupParamSetMaster);
}

bool
centerTransform(const GoldenGraph& g)
{
    return num(g, kTransformNodeParamCenter, { 32., 24. });
}

bool
rotateAndScale(const GoldenGraph& g)
{
    return centerTransform(g) && num(g, kTransformNodeParamRotate, { 30. }) && num(g, kTransformNodeParamScale, { 0.5, 0.75 }) && choice(g, kResamplerParamFilterType, "cubic");
}

bool
keyerColorMode(const GoldenGraph& g)
{
    return choice(g, kKeyerParamMode, "color") && num(g, kKeyerParamKeyColor, { 0.1, 0.8, 0.2 });
}

bool
chromaKey(const GoldenGraph& g)
{
    return num(g, kChromaKeyerParamKeyColor, { 0.1, 0.8, 0.2 });
}

GoldenNodeSpec
mergeSpec(const std::string& name,
          const std::string& id,
          bool secondA)
{
    GoldenNodeSpec spec = plainSpec(name, id, PLUGIN_MAJOR_NATRON_MERGE);

    spec.maskInput = "Mask";
    spec.extraInputs.push_back("A");
    if (secondA) {
        spec.extraInputs.push_back("A2");
    }

    return spec;
}

Apply
mergeOperation(const std::string& operation)
{
    return [operation](const GoldenGraph& g) { return choice(g, kMergeParamOperation, operation); };
}

// The cases every Merge registration shares: B and the A inputs sit at different origins, so each
// A reaches outside the render window and the bounding box modes differ.
void
addMergeCases(std::vector<GoldenVariant>* variants,
              const std::string& operation)
{
    const Apply op = mergeOperation(operation);

    variants->push_back(variant("default", op));
    variants->push_back(variant("bboxa", [operation](const GoldenGraph& g) { return choice(g, kMergeParamOperation, operation) && choice(g, kMergeParamBBox, kMergeBBoxA); }));
    variants->push_back(variant("bboxintersection", [operation](const GoldenGraph& g) { return choice(g, kMergeParamOperation, operation) && choice(g, kMergeParamBBox, kMergeBBoxIntersection); }));
    variants->push_back(variant("channels", [operation](const GoldenGraph& g) {
        return choice(g, kMergeParamOperation, operation) && num(g, std::string(kMergeParamAChannels) + "R", { 0. }) && num(g, std::string(kMergeParamOutputChannels) + "A", { 0. });
    }));
    variants->push_back(maskVariant("maskmix", [operation](const GoldenGraph& g) {
        return choice(g, kMergeParamOperation, operation) && num(g, kOfxMixParamName, { 0.5 }) && num(g, kOfxMaskInvertParamName, { 1. });
    }));
    variants->push_back(maskVariant("maskplain", op));
}

} // namespace

class NativeGoldenTest
    : public BaseTest {
protected:
    void run(const GoldenNodeSpec& spec,
             const std::vector<GoldenVariant>& variants)
    {
        runGoldenCases(getApp(), spec, variants);
    }
};

TEST_F(NativeGoldenTest, Invert)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    addPremultAndMaskMix(&v, Apply());
    run(filterSpec("Invert", PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT), v);
}

TEST_F(NativeGoldenTest, Clamp)
{
    const Apply range = [](const GoldenGraph& g) { return num(g, kClampParamMinimum, { 0.2 }) && num(g, kClampParamMaximum, { 0.8 }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("range", range));
    v.push_back(variant("clampto", [](const GoldenGraph& g) {
        return num(g, kClampParamMinimum, { 0.3 }) && num(g, kClampParamMaximum, { 0.6 }) && num(g, kClampParamMinClampTo, { -1., 2., -3., 0.25 }) && num(g, kClampParamMinClampToEnable, { 1. }) && num(g, kClampParamMaxClampTo, { 5., -5., 0.5, 0.75 }) && num(g, kClampParamMaxClampToEnable, { 1. });
    }));
    addPremultAndMaskMix(&v, range);
    run(filterSpec("Clamp", PLUGINID_NATRON_CLAMP, PLUGIN_MAJOR_NATRON_CLAMP), v);
}

TEST_F(NativeGoldenTest, Saturation)
{
    const Apply boost = [](const GoldenGraph& g) { return num(g, kSaturationParamSaturation, { 1.8 }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("boost", [](const GoldenGraph& g) { return num(g, kSaturationParamSaturation, { 2.5 }); }));
    v.push_back(variant("rec2020", [](const GoldenGraph& g) {
        return num(g, kSaturationParamSaturation, { 0.4 }) && num(g, kSaturationParamClampBlack, { 1. }) && choice(g, "luminanceMath", "rec2020");
    }));
    addPremultAndMaskMix(&v, boost);
    run(filterSpec("Saturation", PLUGINID_NATRON_SATURATION, PLUGIN_MAJOR_NATRON_SATURATION), v);
}

TEST_F(NativeGoldenTest, ColorCorrect)
{
    const Apply master = [](const GoldenGraph& g) {
        const std::string group = kColorCorrectGroupMaster;
        return num(g, group + kColorCorrectParamSaturation, { 1.4, 0.7, 1.1, 1. }) && num(g, group + kColorCorrectParamContrast, { 1.3, 0.8, 1.15, 1.2 }) && num(g, group + kColorCorrectParamGamma, { 0.8, 1.25, 0.6, 0.9 }) && num(g, group + kColorCorrectParamGain, { 1.2, 0.9, 1.5, 0.8 }) && num(g, group + kColorCorrectParamOffset, { 0.05, -0.03, 0.1, 0.02 });
    };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("master", master));
    v.push_back(variant("ranges", [](const GoldenGraph& g) {
        return num(g, std::string(kColorCorrectGroupShadows) + kColorCorrectParamGain, { 1.5 }) && num(g, std::string(kColorCorrectGroupMidtones) + kColorCorrectParamGamma, { 0.7 }) && num(g, std::string(kColorCorrectGroupHighlights) + kColorCorrectParamContrast, { 1.3 }) && num(g, kColorCorrectParamClampWhite, { 1. });
    }));
    addPremultAndMaskMix(&v, master);
    run(filterSpec("ColorCorrect", PLUGINID_NATRON_COLORCORRECT, PLUGIN_MAJOR_NATRON_COLORCORRECT), v);
}

TEST_F(NativeGoldenTest, ColorLookup)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("curves", editColorLookupCurves));
    v.push_back(variant("filmlike", [](const GoldenGraph& g) { return editColorLookupCurves(g) && choice(g, kColorLookupParamMasterCurveMode, "filmlike"); }));
    addPremultAndMaskMix(&v, editColorLookupCurves);
    run(filterSpec("ColorLookup", PLUGINID_NATRON_COLORLOOKUP, PLUGIN_MAJOR_NATRON_COLORLOOKUP), v);
}

TEST_F(NativeGoldenTest, Add)
{
    const Apply perChannel = [](const GoldenGraph& g) { return num(g, kColorMathParamValue, { 0.2, -0.1, 0.3, 0.05 }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("perchannel", perChannel));
    v.push_back(variant("uniform", [](const GoldenGraph& g) { return num(g, kColorMathParamValue, { -0.15 }); }));
    addPremultAndMaskMix(&v, perChannel);
    run(filterSpec("Add", PLUGINID_NATRON_ADD, PLUGIN_MAJOR_NATRON_COLORMATH), v);
}

TEST_F(NativeGoldenTest, Multiply)
{
    const Apply perChannel = [](const GoldenGraph& g) { return num(g, kColorMathParamValue, { 1.5, 0.5, 2., 0.8 }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("perchannel", perChannel));
    v.push_back(variant("negative", [](const GoldenGraph& g) { return num(g, kColorMathParamValue, { -0.75 }); }));
    addPremultAndMaskMix(&v, perChannel);
    run(filterSpec("Multiply", PLUGINID_NATRON_MULTIPLY, PLUGIN_MAJOR_NATRON_COLORMATH), v);
}

TEST_F(NativeGoldenTest, Gamma)
{
    const Apply perChannel = [](const GoldenGraph& g) { return num(g, kColorMathParamValue, { 0.8, 1.25, 2.2, 1. }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("perchannel", perChannel));
    v.push_back(variant("invert", [perChannel](const GoldenGraph& g) { return perChannel(g) && num(g, kColorMathParamInvert, { 1. }); }));
    addPremultAndMaskMix(&v, perChannel);
    run(filterSpec("Gamma", PLUGINID_NATRON_GAMMA, PLUGIN_MAJOR_NATRON_COLORMATH), v);
}

TEST_F(NativeGoldenTest, Grade)
{
    const Apply multiplyOffset = [](const GoldenGraph& g) {
        return num(g, kGradeParamMultiply, { 1.5, 0.75, 2., 1.25 }) && num(g, kGradeParamOffset, { 0.1, -0.05, 0.2, 0. }) && num(g, kGradeParamBlackPoint, { 0.05 }) && num(g, kGradeParamWhite, { 1.1 }) && num(g, kGradeParamGamma, { 0.8 }) && num(g, kGradeParamClampBlack, { 1. });
    };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("multiplyoffset", multiplyOffset));
    v.push_back(variant("reverse", [](const GoldenGraph& g) {
        return num(g, kGradeParamReverse, { 1. }) && num(g, kGradeParamMultiply, { 1.5 }) && num(g, kGradeParamBlack, { 0.1 }) && num(g, kGradeParamGamma, { 0.8 }) && num(g, kGradeParamClampBlack, { 0. });
    }));
    addPremultAndMaskMix(&v, multiplyOffset);
    run(filterSpec("Grade", PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE), v);
}

TEST_F(NativeGoldenTest, Constant)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("color", [](const GoldenGraph& g) { return num(g, kConstantParamColor, { 0.25, -0.5, 1.75, 0.6 }); }));
    v.push_back(variant("extent", [](const GoldenGraph& g) {
        return choice(g, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize) && num(g, kNativeGeneratorParamBottomLeft, { 11., 7. }) && num(g, kNativeGeneratorParamSize, { 37., 23. }) && num(g, kConstantParamColor, { 0.1, 0.2, 0.3, 0.4 });
    }));
    run(generatorSpec("Constant", PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT), v);
}

TEST_F(NativeGoldenTest, Solid)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("color", [](const GoldenGraph& g) { return num(g, kConstantParamColor, { 0.3, 0.6, 0.9 }); }));
    v.push_back(variant("extent", [](const GoldenGraph& g) {
        return choice(g, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize) && num(g, kNativeGeneratorParamBottomLeft, { 5., 9. }) && num(g, kNativeGeneratorParamSize, { 30., 20. }) && num(g, kConstantParamColor, { 0.7, -0.2, 1.4 });
    }));
    run(generatorSpec("Solid", PLUGINID_NATRON_SOLID, PLUGIN_MAJOR_NATRON_CONSTANT), v);
}

TEST_F(NativeGoldenTest, CheckerBoard)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("lines", [](const GoldenGraph& g) {
        return num(g, kCheckerBoardParamBoxSize, { 10., 7. }) && num(g, kCheckerBoardParamLineWidth, { 2. }) && num(g, kCheckerBoardParamCenterLineWidth, { 3. }) && num(g, kCheckerBoardParamLineColor, { 0.25, 0.5, 0.75, 1. });
    }));
    v.push_back(variant("boxes", [](const GoldenGraph& g) { return num(g, kCheckerBoardParamBoxSize, { 12., 9. }) && num(g, kCheckerBoardParamLineWidth, { 2. }); }));
    run(generatorSpec("CheckerBoard", PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD), v);
}

TEST_F(NativeGoldenTest, Blur)
{
    const Apply gaussian = [](const GoldenGraph& g) { return choice(g, kBlurParamFilter, kBlurParamFilterGaussian) && num(g, kBlurParamSize, { 6. }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("firgaussian", [](const GoldenGraph& g) { return num(g, kBlurParamSize, { 5., 3. }); }));
    v.push_back(variant("gaussian", gaussian));
    v.push_back(variant("box", [](const GoldenGraph& g) {
        return choice(g, kBlurParamFilter, kBlurParamFilterBox) && num(g, kBlurParamSize, { 7., 2. }) && num(g, kBlurParamCropToFormat, { 0. });
    }));
    v.push_back(filterMaskMix(gaussian));
    run(filterSpec("Blur", PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR), v);
}

TEST_F(NativeGoldenTest, EdgeDetect)
{
    const Apply sobel = [](const GoldenGraph& g) {
        return choice(g, kEdgeDetectParamFilter, kEdgeDetectParamFilterSobel) && choice(g, kEdgeDetectParamMultiChannel, kEdgeDetectParamMultiChannelRMS);
    };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("sobel", sobel));
    v.push_back(variant("blurnms", [](const GoldenGraph& g) {
        return num(g, kEdgeDetectParamBlurSize, { 3. }) && num(g, kEdgeDetectParamErodeSize, { 1. }) && num(g, kEdgeDetectParamNMS, { 1. }) && choice(g, kEdgeDetectParamMultiChannel, kEdgeDetectParamMultiChannelSeparate);
    }));
    v.push_back(filterMaskMix(sobel));
    run(filterSpec("EdgeDetect", PLUGINID_NATRON_EDGEDETECT, PLUGIN_MAJOR_NATRON_EDGEDETECT), v);
}

TEST_F(NativeGoldenTest, EdgeExtend)
{
    const Apply wide = [](const GoldenGraph& g) { return num(g, kEdgeExtendParamSize, { 20. }) && num(g, kEdgeExtendParamSlices, { 8. }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("wide", wide));
    v.push_back(variant("oneslice", [](const GoldenGraph& g) { return num(g, kEdgeExtendParamSize, { 6. }) && num(g, kEdgeExtendParamSlices, { 1. }); }));
    v.push_back(variant("premultiplied", [](const GoldenGraph& g) {
        return num(g, kEdgeExtendParamPremultiplyInput, { 0. }) && num(g, kEdgeExtendParamUnpremultiplyOutput, { 0. });
    }));
    v.push_back(filterMaskMix(wide));
    run(filterSpec("EdgeExtend", PLUGINID_NATRON_EDGEEXTEND, PLUGIN_MAJOR_NATRON_EDGEEXTEND), v);
}

TEST_F(NativeGoldenTest, Sharpen)
{
    const Apply soften = [](const GoldenGraph& g) {
        return choice(g, kSharpenParamMode, kSharpenParamModeSoften) && num(g, kSharpenParamAmount, { 0.7 });
    };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("soften", soften));
    v.push_back(variant("laplacian", [](const GoldenGraph& g) {
        return choice(g, kSharpenParamMode, kSharpenParamModeLaplacian) && num(g, kSharpenParamSize, { 5., 2. }) && num(g, kSharpenParamAmount, { 2. });
    }));
    v.push_back(variant("threshold", [](const GoldenGraph& g) {
        return num(g, kSharpenParamThreshold, { 0.2 }) && num(g, kSharpenParamAmount, { 1.5 });
    }));
    v.push_back(variant("gaussian", [](const GoldenGraph& g) {
        return choice(g, kSharpenParamFilter, kBlurParamFilterGaussian) && num(g, kSharpenParamSize, { 6. });
    }));
    v.push_back(variant("box", [](const GoldenGraph& g) { return choice(g, kSharpenParamFilter, kBlurParamFilterBox) && num(g, kSharpenParamSize, { 7., 2. }); }));
    v.push_back(filterMaskMix(soften));
    run(filterSpec("Sharpen", PLUGINID_NATRON_SHARPEN, PLUGIN_MAJOR_NATRON_SHARPEN), v);
}

TEST_F(NativeGoldenTest, Erode)
{
    const Apply wide = [](const GoldenGraph& g) { return num(g, kErodeDilateParamSize, { 3., 1. }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("wide", wide));
    v.push_back(variant("negative", [](const GoldenGraph& g) { return num(g, kErodeDilateParamSize, { -2., -2. }) && num(g, kErodeDilateParamExpandRoD, { 0. }); }));
    v.push_back(filterMaskMix(wide));
    run(filterSpec("Erode", PLUGINID_NATRON_ERODE, PLUGIN_MAJOR_NATRON_ERODE), v);
}

TEST_F(NativeGoldenTest, Dilate)
{
    const Apply wide = [](const GoldenGraph& g) { return num(g, kErodeDilateParamSize, { 3., 1. }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("wide", wide));
    v.push_back(variant("negative", [](const GoldenGraph& g) { return num(g, kErodeDilateParamSize, { -2., -2. }) && num(g, kErodeDilateParamExpandRoD, { 0. }); }));
    v.push_back(filterMaskMix(wide));
    run(filterSpec("Dilate", PLUGINID_NATRON_DILATE, PLUGIN_MAJOR_NATRON_DILATE), v);
}

TEST_F(NativeGoldenTest, Transform)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default", centerTransform));
    v.push_back(variant("rotatescale", rotateAndScale));
    v.push_back(variant("skew", [](const GoldenGraph& g) {
        return centerTransform(g) && num(g, kTransformNodeParamSkewX, { 0.3 }) && num(g, kTransformNodeParamSkewY, { -0.2 }) && num(g, kTransformNodeParamTranslate, { 3.25, -2.5 }) && choice(g, kTransformNodeParamSkewOrder, "YX") && choice(g, kResamplerParamFilterType, "keys");
    }));
    v.push_back(variant("invert", [](const GoldenGraph& g) {
        return centerTransform(g) && num(g, kTransformNodeParamRotate, { 12. }) && num(g, kTransformNodeParamTranslate, { -4.5, 2. }) && num(g, kTransformNodeParamInvert, { 1. }) && choice(g, kResamplerParamFilterType, "bilinear");
    }));
    run(plainSpec("Transform", PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM), v);
}

TEST_F(NativeGoldenTest, TransformMasked)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default", centerTransform));
    v.push_back(variant("rotatescale", rotateAndScale));
    v.push_back(maskVariant("maskmix", [](const GoldenGraph& g) {
        return centerTransform(g) && num(g, kTransformNodeParamTranslate, { 4.5, -2.25 }) && num(g, kTransformNodeParamRotate, { 20. }) && num(g, kOfxMixParamName, { 0.5 });
    }));
    run(filterSpec("TransformMasked", PLUGINID_NATRON_TRANSFORMMASKED, PLUGIN_MAJOR_NATRON_TRANSFORMMASKED), v);
}

TEST_F(NativeGoldenTest, Crop)
{
    const Apply rect = [](const GoldenGraph& g) { return num(g, kNativeGeneratorParamBottomLeft, { 6., 4. }) && num(g, kNativeGeneratorParamSize, { 40., 30. }); };
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("rect", rect));
    v.push_back(variant("soft", [rect](const GoldenGraph& g) { return rect(g) && num(g, kCropParamSoftness, { 7. }) && num(g, kCropParamBlackOutside, { 1. }); }));
    v.push_back(variant("reformat", [rect](const GoldenGraph& g) { return rect(g) && num(g, kCropParamReformat, { 1. }); }));
    run(plainSpec("Crop", PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP), v);
}

TEST_F(NativeGoldenTest, Reformat)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("box", [](const GoldenGraph& g) {
        return choice(g, kReformatParamType, kReformatParamTypeOptionToBox) && num(g, kReformatParamBoxSize, { 50., 20. }) && num(g, kReformatParamBoxFixed, { 1. }) && choice(g, kReformatParamResize, kReformatParamResizeOptionFit);
    }));
    v.push_back(variant("scale", [](const GoldenGraph& g) {
        return choice(g, kReformatParamType, kReformatParamTypeOptionScale) && num(g, kReformatParamScale, { 0.5, 0.75 }) && choice(g, kResamplerParamFilterType, "keys");
    }));
    v.push_back(variant("flipturn", [](const GoldenGraph& g) {
        return choice(g, kReformatParamType, kReformatParamTypeOptionToBox) && num(g, kReformatParamBoxSize, { 40., 30. }) && num(g, kReformatParamBoxFixed, { 1. }) && choice(g, kReformatParamResize, kReformatParamResizeOptionDistort) && num(g, kReformatParamFlip, { 1. }) && num(g, kReformatParamTurn, { 1. }) && num(g, kResamplerParamFilterClamp, { 1. });
    }));
    run(plainSpec("Reformat", PLUGINID_NATRON_REFORMAT, PLUGIN_MAJOR_NATRON_REFORMAT), v);
}

TEST_F(NativeGoldenTest, Position)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("integer", [](const GoldenGraph& g) { return num(g, kPositionParamTranslate, { 10., -3. }); }));
    v.push_back(variant("fractional", [](const GoldenGraph& g) { return num(g, kPositionParamTranslate, { 10.4, -3.6 }); }));
    run(plainSpec("Position", PLUGINID_NATRON_POSITION, PLUGIN_MAJOR_NATRON_POSITION), v);
}

TEST_F(NativeGoldenTest, Dissolve)
{
    GoldenNodeSpec spec = filterSpec("Dissolve", PLUGINID_NATRON_DISSOLVE, PLUGIN_MAJOR_NATRON_DISSOLVE);
    spec.extraInputs.push_back("1");
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("which03", [](const GoldenGraph& g) { return num(g, kDissolveParamWhich, { 0.3 }); }));
    v.push_back(variant("which1", [](const GoldenGraph& g) { return num(g, kDissolveParamWhich, { 1. }); }));
    v.push_back(maskVariant("mask", [](const GoldenGraph& g) { return num(g, kDissolveParamWhich, { 0.4 }); }));
    run(spec, v);
}

TEST_F(NativeGoldenTest, MergeOver)
{
    std::vector<GoldenVariant> v;
    addMergeCases(&v, "over");
    v.push_back(variant("alphamasking", [](const GoldenGraph& g) { return choice(g, kMergeParamOperation, "over") && num(g, kMergeParamAlphaMasking, { 1. }); }));
    run(mergeSpec("MergeOver", PLUGINID_NATRON_MERGE, false), v);
}

TEST_F(NativeGoldenTest, MergePlusOperation)
{
    std::vector<GoldenVariant> v;
    addMergeCases(&v, "plus");
    v.push_back(variant("alphamasking", [](const GoldenGraph& g) { return choice(g, kMergeParamOperation, "plus") && num(g, kMergeParamAlphaMasking, { 1. }); }));
    run(mergeSpec("MergePlusOperation", PLUGINID_NATRON_MERGE, false), v);
}

TEST_F(NativeGoldenTest, MergeA2)
{
    std::vector<GoldenVariant> v;
    addMergeCases(&v, "over");
    run(mergeSpec("MergeA2", PLUGINID_NATRON_MERGE, true), v);
}

TEST_F(NativeGoldenTest, MergeA2Plus)
{
    std::vector<GoldenVariant> v;
    addMergeCases(&v, "plus");
    run(mergeSpec("MergeA2Plus", PLUGINID_NATRON_MERGE, true), v);
}

TEST_F(NativeGoldenTest, MergePlus)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(maskVariant("maskmix", [](const GoldenGraph& g) { return num(g, kOfxMixParamName, { 0.5 }) && num(g, kOfxMaskInvertParamName, { 1. }); }));
    run(mergeSpec("MergePlus", MergeNode::presetPluginID(eMergePresetPlus), true), v);
}

TEST_F(NativeGoldenTest, MergeMultiply)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(maskVariant("maskmix", [](const GoldenGraph& g) { return num(g, kOfxMixParamName, { 0.5 }) && num(g, kOfxMaskInvertParamName, { 1. }); }));
    run(mergeSpec("MergeMultiply", MergeNode::presetPluginID(eMergePresetMultiply), false), v);
}

TEST_F(NativeGoldenTest, Keyer)
{
    GoldenNodeSpec spec = plainSpec("Keyer", PLUGINID_NATRON_KEYER, PLUGIN_MAJOR_NATRON_KEYER);
    spec.maskInput = "InM";
    spec.extraInputs.push_back("Bg");
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("color", keyerColorMode));
    v.push_back(variant("screencomposite", [](const GoldenGraph& g) {
        return choice(g, kKeyerParamMode, "screen") && num(g, kKeyerParamKeyColor, { 0.1, 0.8, 0.2 }) && choice(g, kKeyerParamShow, "composite") && choice(g, kKeyerParamSourceAlpha, "normal");
    }));
    v.push_back(variant("despill", [](const GoldenGraph& g) {
        return keyerColorMode(g) && num(g, kKeyerParamDespill, { 1. }) && num(g, kKeyerParamDespillAngle, { 180. }) && choice(g, kKeyerParamShow, "unpremultiplied");
    }));
    v.push_back(maskVariant("insidemask", [](const GoldenGraph& g) { return keyerColorMode(g) && choice(g, kKeyerParamShow, "premultiplied"); }));
    run(spec, v);
}

TEST_F(NativeGoldenTest, ChromaKeyer)
{
    GoldenNodeSpec spec = plainSpec("ChromaKeyer", PLUGINID_NATRON_CHROMAKEYER, PLUGIN_MAJOR_NATRON_CHROMAKEYER);
    spec.maskInput = "InM";
    spec.extraInputs.push_back("Bg");
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("green", chromaKey));
    v.push_back(variant("premultiplied", [](const GoldenGraph& g) {
        return chromaKey(g) && num(g, kChromaKeyerParamAcceptanceAngle, { 60. }) && num(g, kChromaKeyerParamKeyGain, { 1.5 }) && choice(g, kChromaKeyerParamShow, "premultiplied") && choice(g, kChromaKeyerParamSourceAlpha, "normal");
    }));
    v.push_back(variant("composite", [](const GoldenGraph& g) {
        return chromaKey(g) && num(g, kChromaKeyerParamLinear, { 1. }) && choice(g, kChromaKeyerParamColorspace, "rec2020") && choice(g, kChromaKeyerParamShow, "composite");
    }));
    v.push_back(maskVariant("insidemask", [](const GoldenGraph& g) { return chromaKey(g) && choice(g, kChromaKeyerParamShow, "unpremultiplied"); }));
    run(spec, v);
}
