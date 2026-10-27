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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Native/RetouchTestSupport.h"
#include "NativeParity.h"
#include "RawReferenceSupport.h"
#include "RenderBothWays.h"
#include "Serialization/ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"
#include "Engine/Nodes/Filter/EdgeAware/PoissonHeal.h"
#include "Engine/Nodes/Filter/Retouch.h"
#include "Engine/Nodes/Filter/RetouchRender.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace RC = RetouchCore;
using RetouchTest::ShapePoint;
using RetouchTest::ShapeSpec;

namespace {

const double kTime = 1.;

const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

const char* const kReferenceDir = NATRON_TESTS_FIXTURES_DIR "/raw/reference/";

// Against darktable's export, relative to the reference's mean level as RawReference compares,
// beyond the half float reference's rounding: the mean error and the largest one. Roto's feather
// falloff is not darktable's quadratic one and the ellipses are Beziers within 0.25 px, so the
// edges of the shapes carry most of it. Engine/Nodes/Filter/docs/Retouch.md quotes them.
const RawReference::Tolerance kDarktableClass = { 2e-3, 5e-2, 0 };

bool
renderRGBA(const NodePtr& node,
           RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, kWindow, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// Whether path holds a reference rather than nothing or a Git LFS pointer.
bool
referenceAvailable(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);

    if (!in) {
        return false;
    }
    char head[24] = { 0 };
    in.read(head, sizeof(head) - 1);

    return std::string(head).compare(0, 23, "version https://git-lfs") != 0;
}

// |got - expected| beyond the rounding of expected to half float.
double
halfFloatError(double got,
               double expected)
{
    return std::max(0., std::fabs(got - expected) - std::fabs(expected) * std::ldexp(1., -11) - std::ldexp(1., -25));
}

// tools/raw/make-reference.py's RETOUCH_SHAPES, in canonical pixels, y up.
enum ShapeKindEnum {
    eShapeCircle,
    eShapeEllipse,
    eShapePath,
};

ShapeSpec
geometrySpec(ShapeKindEnum kind)
{
    ShapeSpec spec;

    switch (kind) {
    case eShapeCircle:
        spec.feather = 3.;
        spec.opacity = 1.;
        spec.sourceX = 22.;
        spec.sourceY = -8.;
        break;
    case eShapeEllipse:
        spec.feather = 2.;
        spec.opacity = 0.6;
        spec.sourceX = -22.;
        spec.sourceY = 8.;
        break;
    case eShapePath:
        spec.feather = 2.;
        spec.opacity = 1.;
        spec.sourceX = 16.;
        spec.sourceY = 20.;
        break;
    }

    return spec;
}

std::vector<ShapePoint>
geometryPoints(ShapeKindEnum kind)
{
    switch (kind) {
    case eShapeCircle:
        return RetouchTest::ellipsePoints(20., 30., 6., 6., 0.);
    case eShapeEllipse:
        // darktable's 30 degrees turn clockwise on screen, y down.
        return RetouchTest::ellipsePoints(42., 24., 9., 5., -30.);
    case eShapePath:
        break;
    }

    return {
        { 26., 8., 24., 9., 28., 7. },
        { 38., 10., 37., 8., 39., 12. },
        { 36., 20., 38., 19., 34., 21. },
        { 24., 18., 25., 20., 23., 16. },
    };
}

struct ReferenceShape {
    ShapeKindEnum kind;
    RC::AlgorithmEnum algorithm;
    int scale;
    RC::BlurTypeEnum blurType;
    double blurRadius;
    RC::FillModeEnum fillMode;
    double fillColor[3];
    double fillBrightness;
};

struct ReferenceJob {
    std::string name;
    int scales;
    int mergeFrom;
    std::vector<ReferenceShape> shapes;
};

ReferenceShape
referenceShape(ShapeKindEnum kind,
               RC::AlgorithmEnum algorithm,
               int scale)
{
    ReferenceShape s = { kind, algorithm, scale, RC::eBlurTypeGaussian, 10., RC::eFillModeErase, { 0., 0., 0. }, 0. };

    if (algorithm == RC::eAlgorithmBlur) {
        s.blurRadius = 4.;
    } else if (algorithm == RC::eAlgorithmFill) {
        s.fillBrightness = 0.05;
    }

    return s;
}

// make-reference.py's retouch_jobs().
std::vector<ReferenceJob>
referenceJobs()
{
    const char* const algorithmNames[] = { "clone", "heal", "blur", "fill" };
    const char* const kindNames[] = { "circle", "ellipse", "path" };
    const int kindScales[] = { 1, 2, 3 };
    std::vector<ReferenceJob> jobs;

    for (int a = 0; a < 4; ++a) {
        for (int k = 0; k < 3; ++k) {
            ReferenceJob job;
            job.name = std::string("retouch-") + algorithmNames[a] + "-" + kindNames[k];
            job.scales = 2;
            job.mergeFrom = 0;
            job.shapes.push_back(referenceShape((ShapeKindEnum)k, (RC::AlgorithmEnum)a, kindScales[k]));
            jobs.push_back(job);
        }
    }
    jobs.push_back({ "retouch-heal-circle-image", 2, 0, { referenceShape(eShapeCircle, RC::eAlgorithmHeal, 0) } });

    ReferenceShape bilateral = referenceShape(eShapeCircle, RC::eAlgorithmBlur, 1);
    bilateral.blurType = RC::eBlurTypeBilateral;
    bilateral.blurRadius = 6.;
    jobs.push_back({ "retouch-blur-bilateral-circle", 2, 0, { bilateral } });

    ReferenceShape colour = referenceShape(eShapeEllipse, RC::eAlgorithmFill, 0);
    colour.fillMode = RC::eFillModeColor;
    colour.fillColor[0] = 0.8;
    colour.fillColor[1] = 0.2;
    colour.fillColor[2] = 0.1;
    colour.fillBrightness = 0.1;
    jobs.push_back({ "retouch-fill-color-ellipse", 2, 0, { colour } });

    jobs.push_back({ "retouch-mixed-merge", 3, 2, {
                                                      referenceShape(eShapeCircle, RC::eAlgorithmHeal, 1),
                                                      referenceShape(eShapeEllipse, RC::eAlgorithmClone, 2),
                                                      referenceShape(eShapePath, RC::eAlgorithmFill, 4),
                                                  } });

    return jobs;
}
} // namespace

class NativeRetouchTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        resetProject();
    }

    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeRetouch64x48", 1.));
    }

    NodePtr makeSource(bool noise)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
            setParitySourceNoise(source, noise);
        }

        return source;
    }

    NodePtr makeRetouch(const NodePtr& source,
                        int scales)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RETOUCH), PLUGIN_MAJOR_NATRON_RETOUCH);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, 0, true);
        }
        EXPECT_TRUE(setKnobValues(node, kRetouchParamScales, { (double)scales }));

        return node;
    }

    static Retouch* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<Retouch*>(node->getEffectInstance().get()) : NULL;
    }

    static void expectDouble(const KnobIPtr& knob,
                             const char* label,
                             double def,
                             double min,
                             double max)
    {
        KnobDoublePtr k = std::dynamic_pointer_cast<KnobDouble>(knob);

        ASSERT_TRUE(bool(k)) << label;
        EXPECT_EQ(std::string(label), k->getLabel());
        EXPECT_DOUBLE_EQ(def, k->getDefaultValue(0)) << label;
        EXPECT_DOUBLE_EQ(min, k->getMinimum(0)) << label;
        EXPECT_DOUBLE_EQ(max, k->getMaximum(0)) << label;
    }

    static void expectInt(const KnobIPtr& knob,
                          const char* label,
                          int def,
                          int min,
                          int max)
    {
        KnobIntPtr k = std::dynamic_pointer_cast<KnobInt>(knob);

        ASSERT_TRUE(bool(k)) << label;
        EXPECT_EQ(std::string(label), k->getLabel());
        EXPECT_EQ(def, k->getDefaultValue(0)) << label;
        EXPECT_EQ(min, k->getMinimum(0)) << label;
        EXPECT_EQ(max, k->getMaximum(0)) << label;
    }

    static void expectChoice(const KnobIPtr& knob,
                             const char* label,
                             int def,
                             const std::vector<std::string>& ids)
    {
        KnobChoicePtr k = std::dynamic_pointer_cast<KnobChoice>(knob);

        ASSERT_TRUE(bool(k)) << label;
        EXPECT_EQ(std::string(label), k->getLabel());
        EXPECT_EQ(def, k->getDefaultValue(0)) << label;
        const std::vector<ChoiceOption> options = k->getEntries_mt_safe();
        ASSERT_EQ(ids.size(), options.size()) << label;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            EXPECT_EQ(ids[i], options[i].id) << label;
        }
    }
};

// Every field of dt_iop_retouch_params_t (version 3) and dt_iop_retouch_form_data_t is a knob
// with darktable's default and range: the globals on the node, a form's settings on each shape.
// darktable's NONE algorithm has no knob option: a shape always does something.
TEST_F(NativeRetouchTest, KnobsMatchDarktableParams)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(effectOf(node) != NULL);
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));

    const std::vector<std::string> algorithms = { kRetouchAlgorithmClone, kRetouchAlgorithmHeal, kRetouchAlgorithmBlur, kRetouchAlgorithmFill };
    const std::vector<std::string> blurTypes = { kRetouchBlurTypeGaussian, kRetouchBlurTypeBilateral };
    const std::vector<std::string> fillModes = { kRetouchFillModeErase, kRetouchFillModeColor };

    expectChoice(node->getKnobByName(kRetouchParamAlgorithm), "Algorithm", (int)RC::eAlgorithmHeal, algorithms);
    expectInt(node->getKnobByName(kRetouchParamScales), "Scales", 0, 0, RC::kMaxScales);
    expectInt(node->getKnobByName(kRetouchParamCurrentScale), "Current Scale", 0, 0, RC::kMaxScales + 1);
    expectInt(node->getKnobByName(kRetouchParamMergeFrom), "Merge From", 0, 0, RC::kMaxScales);
    expectChoice(node->getKnobByName(kRetouchParamOutput), "Output", (int)RC::eOutputResult,
                 { kRetouchOutputResult, kRetouchOutputCurrentScale, kRetouchOutputResidual });
    KnobDoublePtr levels = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kRetouchParamPreviewLevels));
    ASSERT_TRUE(bool(levels));
    EXPECT_EQ(std::string("Preview Levels"), levels->getLabel());
    ASSERT_EQ(3, levels->getDimension());
    EXPECT_DOUBLE_EQ(-3., levels->getDefaultValue(0));
    EXPECT_DOUBLE_EQ(0., levels->getDefaultValue(1));
    EXPECT_DOUBLE_EQ(3., levels->getDefaultValue(2));
    expectChoice(node->getKnobByName(kRetouchParamBlurType), "Blur Type", (int)RC::eBlurTypeGaussian, blurTypes);
    expectDouble(node->getKnobByName(kRetouchParamBlurRadius), "Blur Radius", 10., 0.1, 200.);
    expectChoice(node->getKnobByName(kRetouchParamFillMode), "Fill Mode", (int)RC::eFillModeErase, fillModes);
    KnobColorPtr fillColor = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kRetouchParamFillColor));
    ASSERT_TRUE(bool(fillColor));
    EXPECT_EQ(std::string("Fill Color"), fillColor->getLabel());
    for (int d = 0; d < 3; ++d) {
        EXPECT_DOUBLE_EQ(0., fillColor->getDefaultValue(d));
    }
    expectDouble(node->getKnobByName(kRetouchParamFillBrightness), "Fill Brightness", 0., -1., 1.);
    expectInt(node->getKnobByName(kRetouchParamMaxHealIterations), "Max Heal Iterations", 2000, 1, 100000);
    EXPECT_EQ(2000, EdgeAware::kPoissonHealDefaultIterations);

    // A form's settings, on a new shape: darktable's form defaults.
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    BezierPtr shape = context->makeEllipse(24., 24., 16., true, kTime);
    ASSERT_TRUE(bool(shape));
    expectChoice(shape->getKnobByName(kRetouchShapeParamAlgorithm), "Shape Algorithm", (int)RC::eAlgorithmHeal, algorithms);
    expectInt(shape->getKnobByName(kRetouchShapeParamScale), "Shape Scale", 0, 0, RC::kMaxScales + 1);
    KnobDoublePtr source = std::dynamic_pointer_cast<KnobDouble>(shape->getKnobByName(kRetouchShapeParamSourceOffset));
    ASSERT_TRUE(bool(source));
    EXPECT_EQ(std::string("Source Offset"), source->getLabel());
    ASSERT_EQ(2, source->getDimension());
    expectChoice(shape->getKnobByName(kRetouchShapeParamBlurType), "Shape Blur Type", (int)RC::eBlurTypeGaussian, blurTypes);
    expectDouble(shape->getKnobByName(kRetouchShapeParamBlurRadius), "Shape Blur Radius", 10., 0.1, 200.);
    expectChoice(shape->getKnobByName(kRetouchShapeParamFillMode), "Shape Fill Mode", (int)RC::eFillModeErase, fillModes);
    KnobColorPtr shapeColor = std::dynamic_pointer_cast<KnobColor>(shape->getKnobByName(kRetouchShapeParamFillColor));
    ASSERT_TRUE(bool(shapeColor));
    EXPECT_EQ(std::string("Shape Fill Color"), shapeColor->getLabel());
    expectDouble(shape->getKnobByName(kRetouchShapeParamFillBrightness), "Shape Fill Brightness", 0., -1., 1.);

    const RC::ShapeSettings defaults;
    EXPECT_EQ(RC::eAlgorithmHeal, defaults.algorithm);
    EXPECT_EQ(0, defaults.scale);
    EXPECT_EQ(10.f, defaults.blurRadius);
    EXPECT_EQ(RC::eFillModeErase, defaults.fillMode);
    const RC::Params params;
    EXPECT_EQ(2000, params.maxHealIterations);
    EXPECT_EQ(0, params.mergeFromScale);
}

// Every Bezier the node's context makes carries the shape knobs, copies included, and its settings
// read back as made.
TEST_F(NativeRetouchTest, ShapesCarryTheirKnobs)
{
    NodePtr node = makeRetouch(makeSource(false), 2);
    ASSERT_TRUE(bool(node));
    ShapeSpec spec;
    spec.algorithm = RC::eAlgorithmBlur;
    spec.scale = 2;
    spec.blurRadius = 7.;
    spec.sourceX = 3.;
    spec.sourceY = -4.;
    BezierPtr ellipse = RetouchTest::makeShape(node, spec, kTime);
    ASSERT_TRUE(bool(ellipse));
    BezierPtr path = RetouchTest::makeBezierShape(node, geometryPoints(eShapePath), spec, kTime);
    ASSERT_TRUE(bool(path));

    for (const BezierPtr& shape : { ellipse, path }) {
        const RC::ShapeSettings s = Retouch::readShapeSettings(shape, kTime);
        EXPECT_EQ(RC::eAlgorithmBlur, s.algorithm);
        EXPECT_EQ(2, s.scale);
        EXPECT_FLOAT_EQ(7.f, s.blurRadius);
        double dx = 0., dy = 0.;
        Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
        EXPECT_DOUBLE_EQ(3., dx);
        EXPECT_DOUBLE_EQ(-4., dy);
    }
}

// The "Scale / Algorithm" column: the image, a detail scale, the residual, and a scale the node
// does not process.
TEST_F(NativeRetouchTest, ScaleColumnText)
{
    EXPECT_EQ(std::string("Image / Heal"), Retouch::shapeColumnText(0, (int)RC::eAlgorithmHeal, 2));
    EXPECT_EQ(std::string("S1 / Clone"), Retouch::shapeColumnText(1, (int)RC::eAlgorithmClone, 2));
    EXPECT_EQ(std::string("Residual / Fill"), Retouch::shapeColumnText(3, (int)RC::eAlgorithmFill, 2));
    EXPECT_EQ(std::string("S5 (off) / Blur"), Retouch::shapeColumnText(5, (int)RC::eAlgorithmBlur, 2));

    NodePtr node = makeRetouch(makeSource(false), 2);
    ASSERT_TRUE(bool(node));
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ShapeSpec spec;
    spec.scale = 2;
    BezierPtr shape = RetouchTest::makeShape(node, spec, kTime);
    ASSERT_TRUE(bool(shape));
    ASSERT_EQ(1u, effect->getColumns().size());
    EXPECT_EQ(std::string("Scale / Algorithm"), effect->getColumns()[0].header);
    EXPECT_EQ(std::string("S2 / Heal"), effect->getItemText(shape, 0));
}

// Copy to Scale keeps the shape and adds one on the current scale with its settings; Move to
// Scale changes the shape's scale.
TEST_F(NativeRetouchTest, CopyAndMoveToScale)
{
    NodePtr node = makeRetouch(makeSource(false), 3);
    ASSERT_TRUE(bool(node));
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    ShapeSpec spec;
    spec.algorithm = RC::eAlgorithmClone;
    spec.scale = 1;
    spec.sourceX = 6.;
    spec.sourceY = 2.;
    BezierPtr shape = RetouchTest::makeShape(node, spec, kTime);
    ASSERT_TRUE(bool(shape));
    ASSERT_TRUE(setKnobValues(node, kRetouchParamCurrentScale, { 3. }));

    const std::list<RotoItemPtr> items(1, shape);
    const std::vector<RotoCurvePanelHook::MenuAction> actions = effect->getItemMenuActions(items);
    ASSERT_EQ(2u, actions.size());
    EXPECT_EQ(std::string(kRetouchActionCopyToScale), actions[0].id);
    EXPECT_EQ(std::string("Copy to Scale 3"), actions[0].label);
    EXPECT_EQ(std::string(kRetouchActionMoveToScale), actions[1].id);
    EXPECT_EQ(std::string("Move to Scale 3"), actions[1].label);

    effect->onItemMenuActionTriggered(kRetouchActionCopyToScale, items);
    std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder(false);
    ASSERT_EQ(2u, curves.size());
    int onScale1 = 0;
    int onScale3 = 0;
    for (const RotoDrawableItemPtr& curve : curves) {
        const RC::ShapeSettings s = Retouch::readShapeSettings(curve, kTime);
        EXPECT_EQ(RC::eAlgorithmClone, s.algorithm);
        double dx = 0., dy = 0.;
        Retouch::readShapeSourceOffset(curve, kTime, &dx, &dy);
        EXPECT_DOUBLE_EQ(6., dx);
        EXPECT_DOUBLE_EQ(2., dy);
        onScale1 += (s.scale == 1);
        onScale3 += (s.scale == 3);
    }
    EXPECT_EQ(1, onScale1);
    EXPECT_EQ(1, onScale3);

    ASSERT_TRUE(setKnobValues(node, kRetouchParamCurrentScale, { 2. }));
    effect->onItemMenuActionTriggered(kRetouchActionMoveToScale, items);
    EXPECT_EQ(2u, context->getCurvesByRenderOrder(false).size());
    EXPECT_EQ(2, Retouch::readShapeSettings(shape, kTime).scale);
}

// With no shapes, or shapes only on scales the node skips, the output is the input.
TEST_F(NativeRetouchTest, NoShapesIsIdentity)
{
    NodePtr source = makeSource(true);
    ASSERT_TRUE(bool(source));
    RenderedPlane in;
    ASSERT_TRUE(renderRGBA(source, &in));

    for (int scales : { 0, 2, 5 }) {
        resetProject();
        source = makeSource(true);
        NodePtr node = makeRetouch(source, scales);
        ASSERT_TRUE(bool(node));
        if (scales == 2) {
            ShapeSpec spec;
            spec.scale = 4;
            ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec, kTime)));
        }
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(node, &out));
        ASSERT_EQ(in.pixels.size(), out.pixels.size());
        double worst = 0.;
        for (std::size_t i = 0; i < in.pixels.size(); ++i) {
            worst = std::max(worst, (double)std::fabs(in.pixels[i] - out.pixels[i]));
        }
        EXPECT_LE(worst, 1e-6) << scales << " scales";
    }
}

// The decomposition and recomposition alone give the image back within 1e-6.
TEST_F(NativeRetouchTest, DecomposeRecomposeIsIdentity)
{
    const int width = kParitySourceWidth;
    const int height = kParitySourceHeight;
    std::vector<float> image((std::size_t)width * height * 4);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 4; ++c) {
                image[((std::size_t)y * width + x) * 4 + c] = paritySourceColorValue(c, x, y, kTime) + 0.1f * paritySourceNoise(c, x, y);
            }
        }
    }
    for (int scales : { 1, 3, 5 }) {
        std::vector<float> out = image;
        RC::Params params;
        params.numScales = scales;
        RC::process(out.data(), width, height, params, std::vector<RC::Shape>(), EdgeAware::serialFor);
        double worst = 0.;
        for (std::size_t i = 0; i < image.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                worst = std::max(worst, (double)std::fabs(image[i + c] - out[i + c]));
            }
        }
        EXPECT_LE(worst, 1e-6) << scales << " scales";
    }
}

// The node's settings and every shape knob survive a save and a load.
TEST_F(NativeRetouchTest, SaveLoadRoundTrip)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr node = makeRetouch(makeSource(false), 3);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kRetouchParamMergeFrom, { 2. }));
    ASSERT_TRUE(setKnobValues(node, kRetouchParamMaxHealIterations, { 700. }));
    ShapeSpec spec;
    spec.algorithm = RC::eAlgorithmFill;
    spec.scale = 2;
    spec.sourceX = -5.5;
    spec.sourceY = 3.25;
    spec.opacity = 0.4;
    spec.feather = 2.5;
    spec.blurType = RC::eBlurTypeBilateral;
    spec.blurRadius = 12.;
    spec.fillMode = RC::eFillModeColor;
    spec.fillColor[0] = 0.25;
    spec.fillColor[1] = 0.5;
    spec.fillColor[2] = 0.75;
    spec.fillBrightness = -0.2;
    BezierPtr shape = RetouchTest::makeShape(node, spec, kTime);
    ASSERT_TRUE(bool(shape));
    const std::string nodeName = node->getScriptName();
    const std::string shapeName = shape->getScriptName();

    std::string yaml;
    ProjectPtr project = getApp()->getProject();
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("retouch.ntpz"), &yaml));
    node.reset();
    shape.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("retouch.ntpz")));

    NodePtr loaded = project->getNodeByName(nodeName);
    ASSERT_TRUE(bool(loaded));
    KnobIntPtr mergeFrom = std::dynamic_pointer_cast<KnobInt>(loaded->getKnobByName(kRetouchParamMergeFrom));
    ASSERT_TRUE(bool(mergeFrom));
    EXPECT_EQ(2, mergeFrom->getValue());
    KnobIntPtr maxHeal = std::dynamic_pointer_cast<KnobInt>(loaded->getKnobByName(kRetouchParamMaxHealIterations));
    ASSERT_TRUE(bool(maxHeal));
    EXPECT_EQ(700, maxHeal->getValue());
    RotoCurveContextPtr context = loaded->getCurveContext();
    ASSERT_TRUE(bool(context));
    RotoDrawableItemPtr loadedShape = std::dynamic_pointer_cast<RotoDrawableItem>(context->getItemByName(shapeName));
    ASSERT_TRUE(bool(loadedShape));

    const RC::ShapeSettings s = Retouch::readShapeSettings(loadedShape, kTime);
    EXPECT_EQ(RC::eAlgorithmFill, s.algorithm);
    EXPECT_EQ(2, s.scale);
    EXPECT_EQ(RC::eBlurTypeBilateral, s.blurType);
    EXPECT_FLOAT_EQ(12.f, s.blurRadius);
    EXPECT_EQ(RC::eFillModeColor, s.fillMode);
    EXPECT_FLOAT_EQ(0.25f, s.fillColor[0]);
    EXPECT_FLOAT_EQ(0.5f, s.fillColor[1]);
    EXPECT_FLOAT_EQ(0.75f, s.fillColor[2]);
    EXPECT_FLOAT_EQ(-0.2f, s.fillBrightness);
    EXPECT_FLOAT_EQ(0.4f, s.opacity);
    double dx = 0., dy = 0.;
    Retouch::readShapeSourceOffset(loadedShape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(-5.5, dx);
    EXPECT_DOUBLE_EQ(3.25, dy);
    KnobDoublePtr feather = RetouchTest::shapeKnob<KnobDouble>(loadedShape, "feather");
    ASSERT_TRUE(bool(feather));
    EXPECT_DOUBLE_EQ(2.5, feather->getValue());
}

// The node against darktable's retouch on make-reference.py's plate, the ParitySource with its
// noise, for every job of retouch_jobs(). Skips when the references are not checked out.
TEST_F(NativeRetouchTest, MatchesDarktableReferences)
{
    const std::vector<ReferenceJob> jobs = referenceJobs();
    const std::string first = std::string(kReferenceDir) + jobs.front().name + ".tif";

    if (!referenceAvailable(first)) {
        std::printf("NativeRetouch: %s is missing; run tools/raw/make-reference.sh --only retouch. Skipped.\n", first.c_str());

        return;
    }
    for (const ReferenceJob& job : jobs) {
        const std::string path = std::string(kReferenceDir) + job.name + ".tif";
        ASSERT_TRUE(referenceAvailable(path)) << path << " is missing while " << first << " is not";
        std::vector<float> expected;
        int width = 0;
        int height = 0;
        std::string error;
        ASSERT_TRUE(RawReference::readReference(path, &expected, &width, &height, &error)) << error;
        ASSERT_EQ(kParitySourceWidth, width) << job.name;
        ASSERT_EQ(kParitySourceHeight, height) << job.name;

        resetProject();
        NodePtr node = makeRetouch(makeSource(true), job.scales);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kRetouchParamMergeFrom, { (double)job.mergeFrom }));
        for (const ReferenceShape& r : job.shapes) {
            ShapeSpec spec = geometrySpec(r.kind);
            spec.algorithm = r.algorithm;
            spec.scale = r.scale;
            spec.blurType = r.blurType;
            spec.blurRadius = r.blurRadius;
            spec.fillMode = r.fillMode;
            for (int c = 0; c < 3; ++c) {
                spec.fillColor[c] = r.fillColor[c];
            }
            spec.fillBrightness = r.fillBrightness;
            ASSERT_TRUE(bool(RetouchTest::makeBezierShape(node, geometryPoints(r.kind), spec, kTime))) << job.name;
        }
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(node, &out)) << job.name;
        ASSERT_EQ((std::size_t)width * height * 4, out.pixels.size()) << job.name;

        double sum = 0.;
        double level = 0.;
        double worst = 0.;
        int worstX = 0;
        int worstY = 0;
        for (int row = 0; row < height; ++row) {
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const double want = expected[((std::size_t)row * width + x) * 3 + c];
                    const double got = out.pixels[((std::size_t)(height - 1 - row) * width + x) * 4 + c];
                    const double diff = halfFloatError(got, want);
                    sum += diff;
                    level += std::fabs(want);
                    if (diff > worst) {
                        worst = diff;
                        worstX = x;
                        worstY = row;
                    }
                }
            }
        }
        ASSERT_GT(level, 0.) << job.name;
        const double samples = 3. * width * height;
        const double relMean = sum / level;
        const double relMax = worst * samples / level;
        std::printf("%s against darktable: mean error %.4f%% of the mean level, max %.3f%% at %d,%d from the top-left\n",
                    job.name.c_str(), 100. * relMean, 100. * relMax, worstX, worstY);
        EXPECT_LE(relMean, kDarktableClass.mean) << job.name;
        EXPECT_LE(relMax, kDarktableClass.max) << job.name;
    }
}
