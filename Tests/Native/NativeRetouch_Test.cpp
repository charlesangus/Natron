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
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QPointF>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Native/RetouchTestSupport.h"
#include "NativeParity.h"
#include "RawReferenceSupport.h"
#include "RegionIndependenceSupport.h"
#include "RenderBothWays.h"
#include "Serialization/ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"
#include "Engine/Nodes/Filter/EdgeAware/PoissonHeal.h"
#include "Engine/Nodes/Filter/EdgeAware/Wavelets.h"
#include "Engine/Nodes/Filter/Retouch.h"
#include "Engine/Nodes/Filter/RetouchRender.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoContextPrivate.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoCommand.h"
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

// darktable draws a path's feather as integer-snapped lines from each border point
// (_path_falloff_roi in src/develop/masks/path.c), which leaves a band about a pixel wide at
// full strength, biased up and left, inside and along the outline. The comparison leaves out the
// pixels this close inside a path's outline. Each line is int(feather) + 1 steps long from a
// truncated start and also writes its neighbouring pixel, so outside the outline darktable's
// falloff reaches about a pixel past the feather: the band there runs to feather + 1.
const double kPathEdgeBand = 2.;

// The closed outline through points, y up, as a polygon of points on its cubic segments.
std::vector<Point>
sampleOutline(const std::vector<ShapePoint>& points)
{
    const int perSegment = 256;
    std::vector<Point> outline;

    for (std::size_t k = 0; k < points.size(); ++k) {
        const ShapePoint& a = points[k];
        const ShapePoint& b = points[(k + 1) % points.size()];
        for (int i = 0; i < perSegment; ++i) {
            const double t = (double)i / perSegment;
            const double u = 1. - t;
            const double w0 = u * u * u;
            const double w1 = 3. * t * u * u;
            const double w2 = 3. * t * t * u;
            const double w3 = t * t * t;
            Point p;
            p.x = w0 * a.x + w1 * a.rx + w2 * b.lx + w3 * b.x;
            p.y = w0 * a.y + w1 * a.ry + w2 * b.ly + w3 * b.y;
            outline.push_back(p);
        }
    }

    return outline;
}

// The distance from (x, y) to outline, negative inside it.
double
signedOutlineDistance(const std::vector<Point>& outline,
                      double x,
                      double y)
{
    double nearest = std::numeric_limits<double>::max();
    bool inside = false;

    for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
        const Point& p = outline[i];
        const Point& q = outline[j];
        const double ex = q.x - p.x;
        const double ey = q.y - p.y;
        const double len2 = ex * ex + ey * ey;
        const double t = (len2 > 0.) ? std::max(0., std::min(1., ((x - p.x) * ex + (y - p.y) * ey) / len2)) : 0.;
        nearest = std::min(nearest, std::hypot(x - p.x - t * ex, y - p.y - t * ey));
        if (((p.y > y) != (q.y > y)) && (x < p.x + (y - p.y) * ex / ey)) {
            inside = !inside;
        }
    }

    return inside ? -nearest : nearest;
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
    // darktable's brush form: hardness and density as its defaults, size a diameter in pixels.
    const std::vector<std::string> pressures = { "off", "hardnessRelative", "hardnessAbsolute", "opacityRelative", "opacityAbsolute", "sizeRelative" };
    expectDouble(node->getKnobByName(kRetouchParamBrushSize), "Brush Size", 100., 1., 4000.);
    expectDouble(node->getKnobByName(kRetouchParamBrushHardness), "Brush Hardness", 0.66, RC::kBrushHardnessMin, 1.);
    expectDouble(node->getKnobByName(kRetouchParamBrushDensity), "Brush Density", 1., 0., 1.);
    expectChoice(node->getKnobByName(kRetouchParamBrushPressure), "Brush Pressure", (int)RC::eBrushPressureOff, pressures);

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
    expectDouble(node->getKnobByName(kRetouchShapeParamBrushSize), "Shape Brush Size", 100., 1., 4000.);
    expectDouble(node->getKnobByName(kRetouchShapeParamBrushHardness), "Shape Brush Hardness", 0.66, RC::kBrushHardnessMin, 1.);
    expectDouble(node->getKnobByName(kRetouchShapeParamBrushDensity), "Shape Brush Density", 1., 0., 1.);
    // The node's Shape Settings carry a brush's knobs; a Bezier has none of its own.
    expectChoice(node->getKnobByName(kRetouchShapeParamBrushPressure), "Shape Brush Pressure", (int)RC::eBrushPressureOff, pressures);
    EXPECT_FALSE(bool(shape->getKnobByName(kRetouchShapeParamBrushSize)));

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

// Dragging a shape's dashed source outline in the viewer, with auto-keying on (the default),
// keys the source offset where the pen moves it, and its undo step puts the offset back.
TEST_F(NativeRetouchTest, SourceDragKeysTheOffsetAndUndoes)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    ASSERT_TRUE(bool(node));
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);
    ShapeSpec spec;
    spec.algorithm = RC::eAlgorithmClone;
    spec.sourceX = 40.;
    spec.sourceY = 0.;
    BezierPtr shape = RetouchTest::makeShape(node, spec, kTime);
    ASSERT_TRUE(bool(shape));
    KnobDoublePtr offset = RetouchTest::shapeKnob<KnobDouble>(shape, kRetouchShapeParamSourceOffset);
    ASSERT_TRUE(bool(offset));

    // The source outline is the ellipse at (24, 24) moved by the offset: centred on (64, 24).
    const RenderScale scale = RenderScale::identity;
    const QPointF start(64., 24.);
    const QPointF end(70., 27.);
    EXPECT_TRUE(effect->onOverlayPenDown_public(kTime, scale, ViewIdx(0), start, start, 1., 0., ePenTypeLMB));
    for (int i = 1; i <= 4; ++i) {
        const double f = i / 4.;
        const QPointF p(start.x() + f * (end.x() - start.x()), start.y() + f * (end.y() - start.y()));
        EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), p, p, 1., i));
    }
    EXPECT_TRUE(effect->onOverlayPenUp_public(kTime, scale, ViewIdx(0), end, end, 1., 5.));

    double dx = 0., dy = 0.;
    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(46., dx);
    EXPECT_DOUBLE_EQ(3., dy);
    EXPECT_TRUE(offset->isAnimated(0, ViewSpec::all()));
    EXPECT_TRUE(offset->isAnimated(1, ViewSpec::all()));

    const double before[2] = { 40., 0. };
    const double after[2] = { 46., 3. };
    UndoCommandPtr command = effect->makeSourceDragCommand(shape, kTime, before, after);
    ASSERT_NE(nullptr, command.get());
    command->undo();
    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(40., dx);
    EXPECT_DOUBLE_EQ(0., dy);
    command->redo();
    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(46., dx);
    EXPECT_DOUBLE_EQ(3., dy);
}

// Dragging the source of a selected shape, whose offset was keyed through the node's knob, starts
// from that offset, moves it by the pen's motion, keeps the keys at other times, shows in the node's
// knob, and its undo step puts the offset back. Deselecting the shape keeps what the drag made.
TEST_F(NativeRetouchTest, SourceDragOfASelectedShapeMovesTheNodeOffset)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    ASSERT_TRUE(bool(node));
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);
    ShapeSpec spec;
    spec.algorithm = RC::eAlgorithmClone;
    spec.sourceX = 40.;
    spec.sourceY = 0.;
    BezierPtr shape = RetouchTest::makeShape(node, spec, kTime);
    ASSERT_TRUE(bool(shape));
    context->select(shape, RotoItem::eSelectionReasonOther);
    KnobDoublePtr nodeOffset = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kRetouchShapeParamSourceOffset));
    ASSERT_TRUE(bool(nodeOffset));

    // Keys set on the node's knob reach the selected shape's own knob.
    const double later = kTime + 10.;
    nodeOffset->setValueAtTime(kTime, 40., ViewSpec::all(), 0);
    nodeOffset->setValueAtTime(kTime, 10., ViewSpec::all(), 1);
    nodeOffset->setValueAtTime(later, 50., ViewSpec::all(), 0);
    nodeOffset->setValueAtTime(later, 20., ViewSpec::all(), 1);
    double dx = 0., dy = 0.;
    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    ASSERT_DOUBLE_EQ(40., dx);
    ASSERT_DOUBLE_EQ(10., dy);

    // The source outline is the ellipse at (24, 24) moved by the offset: centred on (64, 34).
    const RenderScale scale = RenderScale::identity;
    const QPointF start(64., 34.);
    const QPointF end(70., 37.);
    EXPECT_TRUE(effect->onOverlayPenDown_public(kTime, scale, ViewIdx(0), start, start, 1., 0., ePenTypeLMB));
    for (int i = 1; i <= 4; ++i) {
        const double f = i / 4.;
        const QPointF p(start.x() + f * (end.x() - start.x()), start.y() + f * (end.y() - start.y()));
        EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), p, p, 1., i));
    }
    EXPECT_TRUE(effect->onOverlayPenUp_public(kTime, scale, ViewIdx(0), end, end, 1., 5.));

    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(46., dx);
    EXPECT_DOUBLE_EQ(13., dy);
    EXPECT_DOUBLE_EQ(46., nodeOffset->getValueAtTime(kTime, 0));
    EXPECT_DOUBLE_EQ(13., nodeOffset->getValueAtTime(kTime, 1));
    Retouch::readShapeSourceOffset(shape, later, &dx, &dy);
    EXPECT_DOUBLE_EQ(50., dx);
    EXPECT_DOUBLE_EQ(20., dy);

    const double before[2] = { 40., 10. };
    const double after[2] = { 46., 13. };
    UndoCommandPtr command = effect->makeSourceDragCommand(shape, kTime, before, after);
    ASSERT_NE(nullptr, command.get());
    command->undo();
    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(40., dx);
    EXPECT_DOUBLE_EQ(10., dy);
    Retouch::readShapeSourceOffset(shape, later, &dx, &dy);
    EXPECT_DOUBLE_EQ(50., dx);
    EXPECT_DOUBLE_EQ(20., dy);
    command->redo();
    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(46., dx);
    EXPECT_DOUBLE_EQ(13., dy);

    context->clearSelection(RotoItem::eSelectionReasonOther);
    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(46., dx);
    EXPECT_DOUBLE_EQ(13., dy);
    Retouch::readShapeSourceOffset(shape, later, &dx, &dy);
    EXPECT_DOUBLE_EQ(50., dx);
    EXPECT_DOUBLE_EQ(20., dy);
}

// Each motion of a source drag asks for one render at most, of a draft heal, and the release
// renders the heal a fresh render of the final offset gives.
TEST_F(NativeRetouchTest, SourceDragRendersOncePerMotionAndHealsFullyOnRelease)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    ASSERT_TRUE(bool(node));
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    ShapeSpec spec;
    spec.algorithm = RC::eAlgorithmHeal;
    spec.sourceX = 30.;
    spec.sourceY = 0.;
    BezierPtr shape = RetouchTest::makeShape(node, spec, kTime);
    ASSERT_TRUE(bool(shape));
    context->select(shape, RotoItem::eSelectionReasonOther);

    // The source outline is the ellipse at (24, 24) moved by the offset: centred on (54, 24).
    const RenderScale scale = RenderScale::identity;
    const QPointF start(54., 24.);
    const QPointF end(46., 30.);
    EXPECT_TRUE(effect->onOverlayPenDown_public(kTime, scale, ViewIdx(0), start, start, 1., 0., ePenTypeLMB));
    for (int i = 1; i <= 4; ++i) {
        const double f = i / 4.;
        const QPointF p(start.x() + f * (end.x() - start.x()), start.y() + f * (end.y() - start.y()));
        const int before = effect->getRenderRequestCount();
        EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), p, p, 1., i));
        EXPECT_LE(effect->getRenderRequestCount() - before, 1) << "motion " << i;
        EXPECT_TRUE(effect->isInteractiveDraft()) << "motion " << i;
        RenderedPlane draft;
        ASSERT_TRUE(renderRGBA(node, &draft));
    }
    EXPECT_TRUE(effect->onOverlayPenUp_public(kTime, scale, ViewIdx(0), end, end, 1., 5.));
    EXPECT_FALSE(effect->isInteractiveDraft());
    double dx = 0., dy = 0.;
    Retouch::readShapeSourceOffset(shape, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(22., dx);
    EXPECT_DOUBLE_EQ(6., dy);
    RenderedPlane released;
    ASSERT_TRUE(renderRGBA(node, &released));

    resetProject();
    NodePtr fresh = makeRetouch(makeSource(false), 0);
    ASSERT_TRUE(bool(fresh));
    spec.sourceX = dx;
    spec.sourceY = dy;
    ASSERT_TRUE(bool(RetouchTest::makeShape(fresh, spec, kTime)));
    RenderedPlane expected;
    ASSERT_TRUE(renderRGBA(fresh, &expected));
    ASSERT_EQ(expected.pixels.size(), released.pixels.size());
    double worst = 0.;
    for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
        worst = std::max(worst, (double)std::fabs(expected.pixels[i] - released.pixels[i]));
    }
    EXPECT_LE(worst, 1e-6);
}

// Painting a brush asks for one render per pen motion, drafts while the pen is down, and stops
// drafting when it is lifted.
TEST_F(NativeRetouchTest, BrushPaintRendersOncePerMotion)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    ASSERT_TRUE(bool(node));
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    const RetouchInteractPtr& ui = effect->getInteract();
    ASSERT_TRUE(bool(ui));
    ui->setCurrentTool(ui->drawBrushAction.lock());

    const RenderScale scale = RenderScale::identity;
    const QPointF start(10., 20.);
    EXPECT_TRUE(effect->onOverlayPenDown_public(kTime, scale, ViewIdx(0), start, start, 1., 0., ePenTypeLMB));
    EXPECT_TRUE(effect->isInteractiveDraft());
    const int first = effect->getRenderRequestCount();
    for (int i = 1; i <= 4; ++i) {
        const QPointF p(start.x() + 5. * i, start.y() + i);
        const int before = effect->getRenderRequestCount();
        EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), p, p, 1., 0.));
        EXPECT_LE(effect->getRenderRequestCount() - before, 1) << "motion " << i;
        EXPECT_TRUE(effect->isInteractiveDraft()) << "motion " << i;
    }
    EXPECT_GE(effect->getRenderRequestCount() - first, 1);
    const QPointF end(start.x() + 20., start.y() + 4.);
    EXPECT_TRUE(effect->onOverlayPenUp_public(kTime, scale, ViewIdx(0), end, end, 1., 0.));
    EXPECT_FALSE(effect->isInteractiveDraft());
}

// Smooth (Z) on the cusp corners of a square: each point's handles become collinear and equal, along
// the bisector of its chords and as long as a circle's through the four corners, so the square becomes
// that circle. Smoothing again changes nothing, and one undo puts the cusps back.
TEST_F(NativeRetouchTest, SmoothMakesSymmetricCircleTangentsAndUndoes)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    ASSERT_TRUE(bool(node));
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);

    const double cx = 32., cy = 24., h = 10.;
    const std::vector<ShapePoint> square = {
        { cx + h, cy - h, cx + h, cy - h, cx + h, cy - h },
        { cx + h, cy + h, cx + h, cy + h, cx + h, cy + h },
        { cx - h, cy + h, cx - h, cy + h, cx - h, cy + h },
        { cx - h, cy - h, cx - h, cy - h, cx - h, cy - h },
    };
    BezierPtr shape = RetouchTest::makeBezierShape(node, square, ShapeSpec(), kTime);
    ASSERT_TRUE(bool(shape));

    SmoothCuspUndoCommand::SmoothCuspCurveData data;
    data.curve = shape;
    for (int i = 0; i < (int)square.size(); ++i) {
        data.newPoints.push_back(std::make_pair(shape->getControlPointAtIndex(i), shape->getFeatherPointAtIndex(i)));
    }
    const std::pair<double, double> pixelScale(1., 1.);
    std::shared_ptr<SmoothCuspUndoCommand> command = std::make_shared<SmoothCuspUndoCommand>(effect->getInteract(), std::list<SmoothCuspUndoCommand::SmoothCuspCurveData>(1, data), kTime, false, pixelScale);

    const double radius = h * std::sqrt(2.);
    const double handle = 4. / 3. * (std::sqrt(2.) - 1.) * radius;
    const auto expectSmooth = [&](const char* when) {
        for (int i = 0; i < (int)square.size(); ++i) {
            for (const BezierCPPtr& p : { shape->getControlPointAtIndex(i), shape->getFeatherPointAtIndex(i) }) {
                double x, y, lx, ly, rx, ry;
                p->getPositionAtTime(false, kTime, ViewIdx(0), &x, &y);
                p->getLeftBezierPointAtTime(false, kTime, ViewIdx(0), &lx, &ly);
                p->getRightBezierPointAtTime(false, kTime, ViewIdx(0), &rx, &ry);
                EXPECT_NEAR(0., (lx - x) + (rx - x), 1e-9) << when << " point " << i;
                EXPECT_NEAR(0., (ly - y) + (ry - y), 1e-9) << when << " point " << i;
                EXPECT_NEAR(handle, std::hypot(rx - x, ry - y), 1e-9) << when << " point " << i;
                // Tangent to the circle: at right angles to the radius, turning counter-clockwise.
                EXPECT_NEAR(0., (rx - x) * (x - cx) + (ry - y) * (y - cy), 1e-9) << when << " point " << i;
                EXPECT_GT((x - cx) * (ry - y) - (y - cy) * (rx - x), 0.) << when << " point " << i;
                EXPECT_FALSE(p->getBroken()) << when << " point " << i;
            }
        }
    };

    command->redo();
    expectSmooth("smoothed");
    for (int i = 0; i < (int)square.size(); ++i) {
        shape->smoothPointAtIndex(i, kTime, pixelScale);
    }
    expectSmooth("smoothed twice");

    command->undo();
    for (int i = 0; i < (int)square.size(); ++i) {
        double x, y, lx, ly, rx, ry;
        BezierCPPtr p = shape->getControlPointAtIndex(i);
        p->getPositionAtTime(false, kTime, ViewIdx(0), &x, &y);
        p->getLeftBezierPointAtTime(false, kTime, ViewIdx(0), &lx, &ly);
        p->getRightBezierPointAtTime(false, kTime, ViewIdx(0), &rx, &ry);
        EXPECT_DOUBLE_EQ(square[i].lx, lx) << "undone point " << i;
        EXPECT_DOUBLE_EQ(square[i].ly, ly) << "undone point " << i;
        EXPECT_DOUBLE_EQ(square[i].rx, rx) << "undone point " << i;
        EXPECT_DOUBLE_EQ(square[i].ry, ry) << "undone point " << i;
    }
}

// Smooth on a corner whose chords differ in length: the handles stay collinear and equal, capped at half
// the shorter chord so the short segment cannot loop. On the ends of an open path a handle runs along
// its only chord, a third of its length.
TEST_F(NativeRetouchTest, SmoothCapsUnevenChordsAndFollowsOpenEnds)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    ASSERT_TRUE(bool(node));
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    context->setAutoKeyingEnabled(true);
    const std::pair<double, double> pixelScale(1., 1.);

    BezierPtr closed = RetouchTest::makeBezierShape(node, {
                                                              { 0., 0., 0., 0., 0., 0. },
                                                              { 4., 0., 4., 0., 4., 0. },
                                                              { 4., 40., 4., 40., 4., 40. },
                                                          },
                                                    ShapeSpec(), kTime);
    ASSERT_TRUE(bool(closed));
    closed->smoothPointAtIndex(1, kTime, pixelScale);
    {
        double x, y, lx, ly, rx, ry;
        BezierCPPtr p = closed->getControlPointAtIndex(1);
        p->getPositionAtTime(false, kTime, ViewIdx(0), &x, &y);
        p->getLeftBezierPointAtTime(false, kTime, ViewIdx(0), &lx, &ly);
        p->getRightBezierPointAtTime(false, kTime, ViewIdx(0), &rx, &ry);
        EXPECT_NEAR(2., std::hypot(rx - x, ry - y), 1e-9);
        EXPECT_NEAR(2., std::hypot(lx - x, ly - y), 1e-9);
        EXPECT_NEAR(0., (lx - x) + (rx - x), 1e-9);
        EXPECT_NEAR(0., (ly - y) + (ry - y), 1e-9);
        // Along the bisector of (1, 0) and (0, 1).
        EXPECT_NEAR(rx - x, ry - y, 1e-9);
        EXPECT_GT(rx - x, 0.);
    }

    BezierPtr open = context->makeBezier(0., 0., kRotoBezierBaseName, kTime, true);
    ASSERT_TRUE(bool(open));
    open->addControlPoint(30., 0., kTime);
    open->addControlPoint(30., 30., kTime);
    open->setCurveFinished(true);
    open->smoothPointAtIndex(0, kTime, pixelScale);
    open->smoothPointAtIndex(2, kTime, pixelScale);
    double x, y, rx, ry, lx, ly;
    BezierCPPtr first = open->getControlPointAtIndex(0);
    first->getPositionAtTime(false, kTime, ViewIdx(0), &x, &y);
    first->getRightBezierPointAtTime(false, kTime, ViewIdx(0), &rx, &ry);
    EXPECT_NEAR(10., rx - x, 1e-9);
    EXPECT_NEAR(0., ry - y, 1e-9);
    BezierCPPtr last = open->getControlPointAtIndex(2);
    last->getPositionAtTime(false, kTime, ViewIdx(0), &x, &y);
    last->getLeftBezierPointAtTime(false, kTime, ViewIdx(0), &lx, &ly);
    EXPECT_NEAR(0., lx - x, 1e-9);
    EXPECT_NEAR(-10., ly - y, 1e-9);
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

namespace {
// A brush shape from (x0, y0) to (x1, y1) of the given border, in full-image pixels, top-down.
RC::Shape
brushShape(RC::AlgorithmEnum algorithm,
           int scale,
           float x0,
           float y0,
           float x1,
           float y1,
           float radius,
           float dx,
           float dy)
{
    RC::BrushPoint a, b;
    a.x = x0;
    a.y = y0;
    a.radius = radius;
    a.hardness = 0.6f;
    b = a;
    b.x = x1;
    b.y = y1;
    RC::Shape shape;
    shape.settings.algorithm = algorithm;
    shape.settings.scale = scale;
    shape.settings.blurRadius = 3.f;
    shape.settings.fillMode = RC::eFillModeColor;
    shape.settings.fillColor[0] = 0.8f;
    shape.maskRoi = RC::brushBounds({ a, b });
    RC::rasterizeBrush({ a, b }, shape.maskRoi, &shape.mask);
    shape.dx = dx;
    shape.dy = dy;

    return shape;
}

// A textured test image, width x height x 4.
std::vector<float>
texturedImage(int width,
              int height)
{
    std::vector<float> image((std::size_t)width * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 4; ++c) {
                image[((std::size_t)y * width + x) * 4 + c] = 0.3f + 0.2f * std::sin(0.11f * x + 0.7f * c) * std::cos(0.07f * y) + 0.05f * std::sin(1.3f * x * y + c);
            }
        }
    }

    return image;
}
} // anon namespace

// Processing the crop processRegion() asks for gives the full image's result inside the window,
// for shapes of every kind on the image, a detail scale and the residual. A window holding only
// the third shape's source leaves that shape's destination out of the crop.
TEST_F(NativeRetouchTest, CroppedProcessMatchesTheFullImage)
{
    const int width = 200;
    const int height = 150;
    const std::vector<float> image = texturedImage(width, height);
    std::vector<RC::Shape> shapes;
    shapes.push_back(brushShape(RC::eAlgorithmHeal, 0, 30.f, 40.f, 70.f, 50.f, 8.f, -50.f, 60.f));
    shapes.push_back(brushShape(RC::eAlgorithmClone, 2, 120.f, 20.f, 150.f, 60.f, 6.f, 40.f, 70.f));
    shapes.push_back(brushShape(RC::eAlgorithmHeal, 1, 150.f, 110.f, 185.f, 120.f, 7.f, 120.f, 10.f));
    shapes.push_back(brushShape(RC::eAlgorithmBlur, 4, 60.f, 120.f, 90.f, 135.f, 5.f, 0.f, 0.f));
    shapes.push_back(brushShape(RC::eAlgorithmFill, 0, 10.f, 140.f, 20.f, 145.f, 4.f, 0.f, 0.f));
    const RC::Region windows[] = {
        { 0, 0, 40, 30 },
        { 100, 60, 30, 20 },
        { 160, 100, 40, 50 },
        { 55, 115, 20, 10 },
        { -10, -10, 300, 300 },
        { 25, 95, 15, 8 },
    };
    const RC::Region& sourceOnly = windows[5];
    const RC::Region& healed = shapes[2].maskRoi;

    for (int scales : { 0, 3 }) {
        RC::Params params;
        params.numScales = scales;
        params.maxHealIterations = 100;
        std::vector<float> full = image;
        RC::process(full.data(), width, height, params, shapes, EdgeAware::serialFor);
        for (const RC::Region& window : windows) {
            const RC::Region crop = RC::processRegion(window, shapes, params, width, height);
            ASSERT_GT(crop.width, 0);
            ASSERT_GT(crop.height, 0);
            ASSERT_GE(crop.x, 0);
            ASSERT_GE(crop.y, 0);
            ASSERT_LE(crop.x + crop.width, width);
            ASSERT_LE(crop.y + crop.height, height);
            if (&window == &sourceOnly) {
                const bool apart = (crop.x + crop.width <= healed.x) || (healed.x + healed.width <= crop.x) || (crop.y + crop.height <= healed.y)
                    || (healed.y + healed.height <= crop.y);
                EXPECT_TRUE(apart) << scales << " scales: crop " << crop.x << "," << crop.y << " " << crop.width << "x" << crop.height;
            }
            std::vector<float> part((std::size_t)crop.width * crop.height * 4);
            for (int y = 0; y < crop.height; ++y) {
                std::copy(&image[((std::size_t)(y + crop.y) * width + crop.x) * 4], &image[((std::size_t)(y + crop.y) * width + crop.x + crop.width) * 4],
                          &part[(std::size_t)y * crop.width * 4]);
            }
            RC::Params cropParams = params;
            cropParams.originX = crop.x;
            cropParams.originY = crop.y;
            cropParams.fullWidth = width;
            cropParams.fullHeight = height;
            RC::process(part.data(), crop.width, crop.height, cropParams, shapes, EdgeAware::serialFor);
            double worst = 0.;
            const int x1 = std::max(window.x, 0), x2 = std::min(window.x + window.width, width);
            const int y1 = std::max(window.y, 0), y2 = std::min(window.y + window.height, height);
            for (int y = y1; y < y2; ++y) {
                for (int x = x1; x < x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        const float a = full[((std::size_t)y * width + x) * 4 + c];
                        const float b = part[((std::size_t)(y - crop.y) * crop.width + (x - crop.x)) * 4 + c];
                        worst = std::max(worst, (double)std::fabs(a - b));
                    }
                }
            }
            EXPECT_LE(worst, 1e-6) << scales << " scales, window " << window.x << "," << window.y << " " << window.width << "x" << window.height;
        }
    }
}

// With shapes on the image and detail scales only, the result decomposes just the shapes' areas
// and matches the full decomposition, on the whole image, its crops and with non-finite samples.
TEST_F(NativeRetouchTest, DetailScalesMatchTheFullDecomposition)
{
    const int width = 220;
    const int height = 170;
    std::vector<float> image = texturedImage(width, height);
    image[((std::size_t)50 * width + 130) * 4 + 1] = std::numeric_limits<float>::quiet_NaN();
    image[((std::size_t)12 * width + 3) * 4 + 2] = std::numeric_limits<float>::infinity();
    std::vector<RC::Shape> shapes;
    shapes.push_back(brushShape(RC::eAlgorithmHeal, 0, 30.f, 40.f, 70.f, 50.f, 8.f, -25.f, 60.f));
    shapes.push_back(brushShape(RC::eAlgorithmClone, 2, 120.f, 20.f, 150.f, 60.f, 6.f, 40.f, 70.f));
    shapes.push_back(brushShape(RC::eAlgorithmHeal, 1, 150.f, 110.f, 185.f, 120.f, 7.f, 120.f, 10.f));
    shapes.push_back(brushShape(RC::eAlgorithmHeal, 4, 100.f, 60.f, 140.f, 70.f, 9.f, -60.f, 30.f));
    shapes.push_back(brushShape(RC::eAlgorithmBlur, 3, 60.f, 120.f, 90.f, 135.f, 5.f, 0.f, 0.f));
    shapes.push_back(brushShape(RC::eAlgorithmFill, 2, 2.f, 3.f, 12.f, 8.f, 4.f, 0.f, 0.f));
    shapes.push_back(brushShape(RC::eAlgorithmClone, 4, 200.f, 150.f, 215.f, 165.f, 5.f, -150.f, -120.f));
    const RC::Region windows[] = {
        { 0, 0, 40, 30 },
        { 100, 60, 30, 20 },
        { 160, 100, 40, 50 },
        { 195, 145, 25, 25 },
    };

    RC::Params params;
    params.numScales = 4;
    params.maxHealIterations = 100;
    std::vector<float> reference = image;
    EdgeAware::DwtParams dwt;
    dwt.width = width;
    dwt.height = height;
    dwt.scales = params.numScales;
    EdgeAware::dwtDecompose(reference.data(), dwt, [&](float* layer, const EdgeAware::DwtParams& p, int scale) { RC::processLayer(layer, p.width, p.height, scale, p.scales, p.returnLayer, p.mergeFromScale, params, shapes, EdgeAware::serialFor); }, EdgeAware::serialFor);
    ASSERT_EQ(dwt.scales, params.numScales);

    std::vector<float> full = image;
    RC::process(full.data(), width, height, params, shapes, EdgeAware::serialFor);
    double worst = 0.;
    for (std::size_t i = 0; i < full.size(); ++i) {
        if (!std::isfinite(image[i])) {
            EXPECT_EQ(std::isnan(full[i]), std::isnan(image[i])) << i;
            continue;
        }
        worst = std::max(worst, (double)std::fabs(full[i] - reference[i]));
    }
    EXPECT_LE(worst, 1e-6);

    for (const RC::Region& window : windows) {
        const RC::Region crop = RC::processRegion(window, shapes, params, width, height);
        std::vector<float> part((std::size_t)crop.width * crop.height * 4);
        for (int y = 0; y < crop.height; ++y) {
            std::copy(&image[((std::size_t)(y + crop.y) * width + crop.x) * 4], &image[((std::size_t)(y + crop.y) * width + crop.x + crop.width) * 4],
                      &part[(std::size_t)y * crop.width * 4]);
        }
        RC::Params cropParams = params;
        cropParams.originX = crop.x;
        cropParams.originY = crop.y;
        cropParams.fullWidth = width;
        cropParams.fullHeight = height;
        RC::process(part.data(), crop.width, crop.height, cropParams, shapes, EdgeAware::serialFor);
        double cropWorst = 0.;
        for (int y = window.y; y < window.y + window.height; ++y) {
            for (int x = window.x; x < window.x + window.width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const float a = reference[((std::size_t)y * width + x) * 4 + c];
                    const float b = part[((std::size_t)(y - crop.y) * crop.width + (x - crop.x)) * 4 + c];
                    if (std::isfinite(a)) {
                        cropWorst = std::max(cropWorst, (double)std::fabs(a - b));
                    }
                }
            }
        }
        EXPECT_LE(cropWorst, 1e-6) << "window " << window.x << "," << window.y;
    }
}

// With a patch cache, a render solves only the shapes whose input, settings or reach changed:
// editing one heal leaves another's result cached, while a shape it reaches through the wavelet
// support is solved again. Cached renders equal uncached ones, in crops too, and drafts stay out.
TEST_F(NativeRetouchTest, EditingAShapeKeepsTheOthersCached)
{
    const int width = 200;
    const int height = 150;
    const std::vector<float> image = texturedImage(width, height);
    std::vector<RC::Shape> shapes;
    shapes.push_back(brushShape(RC::eAlgorithmHeal, 0, 30.f, 40.f, 70.f, 50.f, 8.f, -25.f, -60.f));
    shapes.push_back(brushShape(RC::eAlgorithmHeal, 0, 140.f, 100.f, 180.f, 110.f, 7.f, 20.f, 40.f));
    shapes.push_back(brushShape(RC::eAlgorithmHeal, 2, 40.f, 62.f, 80.f, 66.f, 5.f, -60.f, 40.f));
    RC::PatchCache cache((std::size_t)64 << 20);
    RC::Params plain;
    plain.numScales = 3;
    plain.maxHealIterations = 100;
    RC::Params params = plain;
    params.cache = &cache;
    params.inputKey = 7;

    const auto worstDifference = [&](const std::vector<RC::Shape>& set, const RC::Params& cachedParams) {
        std::vector<float> cached = image;
        RC::process(cached.data(), width, height, cachedParams, set, EdgeAware::serialFor);
        std::vector<float> fresh = image;
        RC::process(fresh.data(), width, height, plain, set, EdgeAware::serialFor);
        double worst = 0.;
        for (std::size_t i = 0; i < cached.size(); ++i) {
            worst = std::max(worst, (double)std::fabs(cached[i] - fresh[i]));
        }

        return worst;
    };

    EXPECT_EQ(worstDifference(shapes, params), 0.);
    EXPECT_EQ(cache.misses(), 3u);
    EXPECT_EQ(worstDifference(shapes, params), 0.);
    EXPECT_EQ(cache.misses(), 3u);

    // The second heal is out of the first's reach.
    shapes[1].dx = 25.f;
    EXPECT_EQ(worstDifference(shapes, params), 0.);
    EXPECT_EQ(cache.misses(), 4u);

    // The scale-2 heal reads within the first's wavelet support.
    shapes[0].dx = -35.f;
    EXPECT_EQ(worstDifference(shapes, params), 0.);
    EXPECT_EQ(cache.misses(), 6u);

    // A crop around the second heal finds it cached and matches the full render there.
    const RC::Region window = { 130, 90, 60, 30 };
    const RC::Region crop = RC::processRegion(window, shapes, params, width, height);
    std::vector<float> part((std::size_t)crop.width * crop.height * 4);
    for (int y = 0; y < crop.height; ++y) {
        std::copy(&image[((std::size_t)(y + crop.y) * width + crop.x) * 4], &image[((std::size_t)(y + crop.y) * width + crop.x + crop.width) * 4],
                  &part[(std::size_t)y * crop.width * 4]);
    }
    RC::Params cropParams = params;
    cropParams.originX = crop.x;
    cropParams.originY = crop.y;
    cropParams.fullWidth = width;
    cropParams.fullHeight = height;
    RC::process(part.data(), crop.width, crop.height, cropParams, shapes, EdgeAware::serialFor);
    EXPECT_EQ(cache.misses(), 6u);
    std::vector<float> full = image;
    RC::process(full.data(), width, height, plain, shapes, EdgeAware::serialFor);
    double cropWorst = 0.;
    for (int y = window.y; y < window.y + window.height; ++y) {
        for (int x = window.x; x < window.x + window.width; ++x) {
            for (int c = 0; c < 4; ++c) {
                cropWorst = std::max(cropWorst, (double)std::fabs(full[((std::size_t)y * width + x) * 4 + c] - part[((std::size_t)(y - crop.y) * crop.width + (x - crop.x)) * 4 + c]));
            }
        }
    }
    EXPECT_LE(cropWorst, 1e-6);

    // Another input solves everything again.
    params.inputKey = 8;
    EXPECT_EQ(worstDifference(shapes, params), 0.);
    EXPECT_EQ(cache.misses(), 9u);

    // A draft without a warm start solves in full and is kept; one refining a warm start is not.
    std::vector<RC::Shape> draftShapes(1, shapes[1]);
    draftShapes[0].dx = 30.f;
    draftShapes[0].warmStart = std::make_shared<RC::HealWarmStart>();
    RC::Params draft = params;
    draft.draftHealCycles = 2;
    std::vector<float> out = image;
    RC::process(out.data(), width, height, draft, draftShapes, EdgeAware::serialFor);
    EXPECT_EQ(cache.misses(), 10u);
    draftShapes[0].dx = 31.f;
    for (std::size_t expected : { 11u, 12u }) {
        out = image;
        RC::process(out.data(), width, height, draft, draftShapes, EdgeAware::serialFor);
        EXPECT_EQ(cache.misses(), expected);
    }
}

// The node renders a part of its output as it renders that part of the whole.
TEST_F(NativeRetouchTest, RenderingAWindowMatchesTheFullRender)
{
    NodePtr node = makeRetouch(makeSource(true), 2);
    ASSERT_TRUE(bool(node));
    ShapeSpec spec;
    spec.algorithm = RC::eAlgorithmHeal;
    spec.cx = 16.;
    spec.cy = 14.;
    spec.diameter = 10.;
    spec.sourceX = 28.;
    spec.sourceY = 20.;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec, kTime)));
    spec.algorithm = RC::eAlgorithmClone;
    spec.cx = 50.;
    spec.cy = 36.;
    spec.scale = 1;
    spec.sourceX = -30.;
    spec.sourceY = 0.;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec, kTime)));

    RenderedPlane full;
    ASSERT_TRUE(renderRGBA(node, &full));
    const RectI windows[] = { RectI(0, 0, 24, 20), RectI(40, 28, 64, 48), RectI(20, 10, 36, 30) };
    for (const RectI& window : windows) {
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error, true)) << error;
        ASSERT_EQ(1u, planes.size());
        double worst = 0.;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const float a = full.pixels[((std::size_t)(y - full.window.y1) * full.window.width() + (x - full.window.x1)) * 4 + c];
                    const float b = planes[0].pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4 + c];
                    worst = std::max(worst, (double)std::fabs(a - b));
                }
            }
        }
        EXPECT_LE(worst, 1e-6) << window.x1 << "," << window.y1;
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

        // Roto's feather falloff is not darktable's, so each shape's feather band, outside its
        // outline, is left out; around a path, kPathEdgeBand inside and feather + 1 outside.
        std::vector<char> excluded((std::size_t)width * height, 0);
        int excludedCount = 0;
        for (const ReferenceShape& r : job.shapes) {
            const std::vector<Point> outline = sampleOutline(geometryPoints(r.kind));
            const double inner = (r.kind == eShapePath) ? -kPathEdgeBand : 0.;
            const double outer = geometrySpec(r.kind).feather + ((r.kind == eShapePath) ? 1. : 0.);
            for (int row = 0; row < height; ++row) {
                for (int x = 0; x < width; ++x) {
                    const double d = signedOutlineDistance(outline, x + 0.5, height - row - 0.5);
                    char& e = excluded[(std::size_t)row * width + x];
                    if (!e && (d >= inner) && (d <= outer)) {
                        e = 1;
                        ++excludedCount;
                    }
                }
            }
        }

        double sum = 0.;
        double level = 0.;
        double worst = 0.;
        int worstX = 0;
        int worstY = 0;
        for (int row = 0; row < height; ++row) {
            for (int x = 0; x < width; ++x) {
                if (excluded[(std::size_t)row * width + x]) {
                    continue;
                }
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
        const double samples = 3. * (width * height - excludedCount);
        const double relMean = sum / level;
        const double relMax = worst * samples / level;
        std::printf("%s against darktable: mean error %.4f%% of the mean level, max %.3f%% at %d,%d from the top-left, %d pixels of the edge bands left out\n",
                    job.name.c_str(), 100. * relMean, 100. * relMax, worstX, worstY, excludedCount);
        EXPECT_LE(relMean, kDarktableClass.mean) << job.name;
        EXPECT_LE(relMax, kDarktableClass.max) << job.name;
    }
}

namespace {
// A finished brush shape through points, made the way the brush tool makes one.
RotoStrokeItemPtr
makeBrushShape(const NodePtr& node,
               const std::vector<QPointF>& points)
{
    RotoCurveContextPtr context = node->getCurveContext();
    if (!context) {
        return RotoStrokeItemPtr();
    }
    RotoStrokeItemPtr stroke = std::make_shared<RotoStrokeItem>(eRotoStrokeTypeSolid, context, context->generateUniqueName(kRetouchBrushBaseName), RotoLayerPtr());
    Retouch* effect = dynamic_cast<Retouch*>(node->getEffectInstance().get());
    if (effect) {
        effect->getInteract()->startLifeTimeAtCurrentFrame(stroke);
    }
    context->onItemCreated(stroke, false);
    context->addItem(context->getOrCreateBaseLayer(), 0, stroke, RotoItem::eSelectionReasonOther);
    for (std::size_t i = 0; i < points.size(); ++i) {
        stroke->appendPoint(i == 0, RotoPoint(points[i].x(), points[i].y(), 1., 0.));
    }
    stroke->setStrokeFinished();

    return stroke;
}

float
expectedFalloff(float d,
                float radius,
                float hardness,
                float density)
{
    if (d >= radius) {
        return 0.f;
    }
    const float u = d / radius;

    return (u <= hardness) ? density : density * (1.f - u) / (1.f - hardness);
}
} // anon namespace

// darktable's brush falloff: full density inside the hardness fraction of the radius, then linear
// to zero at the border; a lone point is a round stamp and a segment has round caps.
TEST(NativeRetouchBrush, RasterizeFollowsDarktablesFalloff)
{
    RC::Region roi;
    roi.width = 20;
    roi.height = 20;
    std::vector<float> mask;

    RC::BrushPoint stamp;
    stamp.x = 10.f;
    stamp.y = 10.f;
    stamp.radius = 4.f;
    stamp.hardness = 0.5f;
    stamp.density = 0.9f;
    RC::rasterizeBrush({ stamp }, roi, &mask);
    ASSERT_EQ(400u, mask.size());
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            const float d = std::hypot(x + 0.5f - 10.f, y + 0.5f - 10.f);
            EXPECT_NEAR(expectedFalloff(d, 4.f, 0.5f, 0.9f), mask[y * 20 + x], 1e-5f) << x << "," << y;
        }
    }

    RC::BrushPoint a, b;
    a.x = 5.f;
    a.y = 10.f;
    a.radius = 2.f;
    b = a;
    b.x = 15.f;
    RC::rasterizeBrush({ a, b }, roi, &mask);
    EXPECT_FLOAT_EQ(1.f, mask[10 * 20 + 9]);
    EXPECT_FLOAT_EQ(1.f, mask[11 * 20 + 9]);
    EXPECT_FLOAT_EQ(0.f, mask[12 * 20 + 9]);
    EXPECT_FLOAT_EQ(1.f, mask[10 * 20 + 16]);
    EXPECT_FLOAT_EQ(0.f, mask[10 * 20 + 17]);
    EXPECT_FLOAT_EQ(1.f, mask[10 * 20 + 3]);
    EXPECT_FLOAT_EQ(0.f, mask[10 * 20 + 2]);

    const RC::Region bounds = RC::brushBounds({ a, b });
    EXPECT_EQ(3, bounds.x);
    EXPECT_EQ(8, bounds.y);
    EXPECT_EQ(14, bounds.width);
    EXPECT_EQ(4, bounds.height);
}

// The pressure modes of darktable's brush, with its floors.
TEST(NativeRetouchBrush, PressureModesUseDarktablesMinimums)
{
    RC::BrushPoint p;
    p.radius = 10.f;
    p.hardness = 0.6f;
    p.density = 0.5f;

    RC::BrushPoint q = p;
    RC::applyBrushPressure(RC::eBrushPressureOff, 0.25f, 1.f, &q);
    EXPECT_FLOAT_EQ(10.f, q.radius);
    EXPECT_FLOAT_EQ(0.6f, q.hardness);
    EXPECT_FLOAT_EQ(0.5f, q.density);

    q = p;
    RC::applyBrushPressure(RC::eBrushPressureSizeRelative, 0.5f, 1.f, &q);
    EXPECT_FLOAT_EQ(5.f, q.radius);
    q = p;
    RC::applyBrushPressure(RC::eBrushPressureSizeRelative, 0.f, 1.f, &q);
    EXPECT_FLOAT_EQ(1.f, q.radius);

    q = p;
    RC::applyBrushPressure(RC::eBrushPressureHardnessRelative, 0.5f, 1.f, &q);
    EXPECT_FLOAT_EQ(0.3f, q.hardness);
    q = p;
    RC::applyBrushPressure(RC::eBrushPressureHardnessAbsolute, 0.f, 1.f, &q);
    EXPECT_FLOAT_EQ(RC::kBrushHardnessMin, q.hardness);

    q = p;
    RC::applyBrushPressure(RC::eBrushPressureOpacityRelative, 0.5f, 1.f, &q);
    EXPECT_FLOAT_EQ(0.25f, q.density);
    q = p;
    RC::applyBrushPressure(RC::eBrushPressureOpacityAbsolute, 0.f, 1.f, &q);
    EXPECT_FLOAT_EQ(RC::kBrushDensityPressureMin, q.density);
    EXPECT_EQ(10.f, q.radius);
}

// A brush shape takes the node's brush settings when made, and its mask is its stroke in the
// render frame: canonical y up, rows top-down, scaled with the render.
TEST_F(NativeRetouchTest, BrushShapeTakesTheNodeSettingsAndRasterizes)
{
    NodePtr node = makeRetouch(makeSource(false), 2);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kRetouchParamBrushSize, { 10. }));
    ASSERT_TRUE(setKnobValues(node, kRetouchParamBrushHardness, { 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kRetouchParamBrushDensity, { 0.8 }));
    RotoStrokeItemPtr brush = makeBrushShape(node, { QPointF(10., 24.), QPointF(25., 24.), QPointF(40., 24.) });
    ASSERT_TRUE(bool(brush));

    KnobDoublePtr size = RetouchTest::shapeKnob<KnobDouble>(brush, kRetouchShapeParamBrushSize);
    KnobDoublePtr hardness = RetouchTest::shapeKnob<KnobDouble>(brush, kRetouchShapeParamBrushHardness);
    KnobDoublePtr density = RetouchTest::shapeKnob<KnobDouble>(brush, kRetouchShapeParamBrushDensity);
    KnobChoicePtr pressure = RetouchTest::shapeKnob<KnobChoice>(brush, kRetouchShapeParamBrushPressure);
    ASSERT_TRUE(size && hardness && density && pressure);
    EXPECT_EQ(std::string("Shape Brush Size"), size->getLabel());
    EXPECT_DOUBLE_EQ(10., size->getValue());
    EXPECT_DOUBLE_EQ(0.5, hardness->getValue());
    EXPECT_DOUBLE_EQ(0.8, density->getValue());
    EXPECT_EQ((int)RC::eBrushPressureOff, pressure->getValue());
    EXPECT_TRUE(bool(RetouchTest::shapeKnob<KnobChoice>(brush, kRetouchShapeParamAlgorithm)));

    const RectI full(0, 0, kParitySourceWidth, kParitySourceHeight);
    RC::Region roi;
    std::vector<float> mask;
    ASSERT_TRUE(Retouch::rasterizeBrushShape(brush, kTime, 1., full, &roi, &mask));
    ASSERT_EQ((std::size_t)roi.width * roi.height, mask.size());
    const float y = (float)kParitySourceHeight - 24.f;
    EXPECT_LE(roi.x, 5);
    EXPECT_GE(roi.x + roi.width, 45);
    EXPECT_LE(roi.y, (int)y - 5);
    EXPECT_GE(roi.y + roi.height, (int)y + 5);
    auto at = [&](int px, int py) {
        return mask[(std::size_t)(py - roi.y) * roi.width + (px - roi.x)];
    };
    EXPECT_FLOAT_EQ(0.8f, at(25, (int)y));
    EXPECT_NEAR(expectedFalloff(3.5f, 5.f, 0.5f, 0.8f), at(25, (int)y + 3), 1e-4f);
    EXPECT_NEAR(expectedFalloff(4.5f, 5.f, 0.5f, 0.8f), at(25, (int)y + 4), 1e-4f);

    // Half resolution halves the radius around the halved stroke.
    const RectI half(0, 0, kParitySourceWidth / 2, kParitySourceHeight / 2);
    ASSERT_TRUE(Retouch::rasterizeBrushShape(brush, kTime, 0.5, half, &roi, &mask));
    EXPECT_FLOAT_EQ(0.8f, at(12, (int)(y / 2)));
    EXPECT_FLOAT_EQ(0.f, at(12, (int)(y / 2) + 2));
}

// The node has one set of shape knobs: with nothing selected they set what the next shape gets,
// with a shape selected they show and edit that shape alone, and deselecting brings the next
// shape's settings back. The General tab's Roto Color, which a retouch mask ignores, is hidden.
TEST_F(NativeRetouchTest, ShapeSettingsEditTheSelectionOrTheNextShape)
{
    NodePtr node = makeRetouch(makeSource(false), 2);
    ASSERT_TRUE(bool(node));
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    ASSERT_TRUE(bool(context->getColorKnob()));
    EXPECT_TRUE(context->getColorKnob()->getIsSecret());
    const char* const nextShapeKnobs[] = {
        kRetouchParamAlgorithm,
        kRetouchParamBlurType,
        kRetouchParamBlurRadius,
        kRetouchParamFillMode,
        kRetouchParamFillColor,
        kRetouchParamFillBrightness,
        kRetouchParamBrushSize,
        kRetouchParamBrushHardness,
        kRetouchParamBrushDensity,
        kRetouchParamBrushPressure,
    };
    for (const char* name : nextShapeKnobs) {
        KnobIPtr knob = node->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_TRUE(knob->getIsSecret()) << name;
    }

    KnobChoicePtr shownAlgorithm = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kRetouchShapeParamAlgorithm));
    KnobDoublePtr shownSize = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kRetouchShapeParamBrushSize));
    KnobChoicePtr nextAlgorithm = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kRetouchParamAlgorithm));
    KnobDoublePtr nextSize = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kRetouchParamBrushSize));
    KnobIPtr shownScale = node->getKnobByName(kRetouchShapeParamScale);
    ASSERT_TRUE(shownAlgorithm && shownSize && nextAlgorithm && nextSize && shownScale);
    EXPECT_FALSE(shownAlgorithm->getIsSecret());
    EXPECT_TRUE(shownAlgorithm->isEnabled(0));
    EXPECT_TRUE(shownSize->isEnabled(0));
    EXPECT_FALSE(shownScale->isEnabled(0));

    shownAlgorithm->setValue((int)RC::eAlgorithmBlur);
    shownSize->setValue(12.);
    EXPECT_EQ((int)RC::eAlgorithmBlur, nextAlgorithm->getValue());
    EXPECT_DOUBLE_EQ(12., nextSize->getValue());
    RotoStrokeItemPtr first = makeBrushShape(node, { QPointF(10., 24.), QPointF(25., 24.), QPointF(40., 24.) });
    ASSERT_TRUE(bool(first));
    KnobChoicePtr firstAlgorithm = RetouchTest::shapeKnob<KnobChoice>(first, kRetouchShapeParamAlgorithm);
    KnobDoublePtr firstSize = RetouchTest::shapeKnob<KnobDouble>(first, kRetouchShapeParamBrushSize);
    ASSERT_TRUE(firstAlgorithm && firstSize);
    EXPECT_EQ((int)RC::eAlgorithmBlur, firstAlgorithm->getValue());
    EXPECT_DOUBLE_EQ(12., firstSize->getValue());

    shownAlgorithm->setValue((int)RC::eAlgorithmFill);
    shownSize->setValue(30.);
    RotoStrokeItemPtr second = makeBrushShape(node, { QPointF(10., 60.), QPointF(40., 60.) });
    ASSERT_TRUE(bool(second));
    KnobDoublePtr secondSize = RetouchTest::shapeKnob<KnobDouble>(second, kRetouchShapeParamBrushSize);
    ASSERT_TRUE(bool(secondSize));
    EXPECT_DOUBLE_EQ(30., secondSize->getValue());

    // A brush size gesture writes the next shape's size, which the shown knob follows.
    nextSize->setValue(35.);
    EXPECT_DOUBLE_EQ(35., shownSize->getValue());

    context->select(first, RotoItem::eSelectionReasonOther);
    EXPECT_EQ((int)RC::eAlgorithmBlur, shownAlgorithm->getValue());
    EXPECT_DOUBLE_EQ(12., shownSize->getValue());
    EXPECT_TRUE(shownSize->isEnabled(0));
    EXPECT_TRUE(shownScale->isEnabled(0));
    shownSize->setValue(20.);
    EXPECT_DOUBLE_EQ(20., firstSize->getValue());
    EXPECT_DOUBLE_EQ(30., secondSize->getValue());
    EXPECT_DOUBLE_EQ(35., nextSize->getValue());

    context->clearSelection(RotoItem::eSelectionReasonOther);
    EXPECT_EQ((int)RC::eAlgorithmFill, shownAlgorithm->getValue());
    EXPECT_DOUBLE_EQ(35., shownSize->getValue());
    EXPECT_TRUE(shownSize->isEnabled(0));
    EXPECT_FALSE(shownScale->isEnabled(0));
    EXPECT_DOUBLE_EQ(20., firstSize->getValue());
}

// A click inside a painted brush's footprint with the select tool selects the brush, which a
// Shift+click adds to; a click on empty space past every shape selects nothing new.
TEST_F(NativeRetouchTest, ClickInsideABrushSelectsIt)
{
    NodePtr node = makeRetouch(makeSource(false), 2);
    ASSERT_TRUE(bool(node));
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    ASSERT_TRUE(setKnobValues(node, kRetouchParamBrushSize, { 10. }));
    RotoStrokeItemPtr heal = makeBrushShape(node, { QPointF(10., 24.), QPointF(40., 24.) });
    ASSERT_TRUE(setKnobValues(node, kRetouchParamAlgorithm, { (double)RC::eAlgorithmBlur }));
    RotoStrokeItemPtr blur = makeBrushShape(node, { QPointF(10., 60.), QPointF(40., 60.) });
    ASSERT_TRUE(heal && blur);
    EXPECT_TRUE(context->getSelectedCurves().empty());

    const RenderScale scale = RenderScale::identity;
    auto click = [&](const QPointF& p) {
        const bool ret = effect->onOverlayPenDown_public(kTime, scale, ViewIdx(0), p, p, 1., 0., ePenTypeLMB);
        effect->onOverlayPenUp_public(kTime, scale, ViewIdx(0), p, p, 1., 1.);

        return ret;
    };
    EXPECT_TRUE(click(QPointF(25., 61.)));
    std::list<RotoDrawableItemPtr> selected = context->getSelectedCurves();
    ASSERT_EQ(1u, selected.size());
    EXPECT_EQ(blur.get(), selected.front().get());

    // The Heal brush's source sits on it: the click selects the brush and leaves its source put.
    EXPECT_TRUE(click(QPointF(25., 23.)));
    selected = context->getSelectedCurves();
    ASSERT_EQ(1u, selected.size());
    EXPECT_EQ(heal.get(), selected.front().get());
    double dx = 1., dy = 1.;
    Retouch::readShapeSourceOffset(heal, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(0., dx);
    EXPECT_DOUBLE_EQ(0., dy);

    effect->getInteract()->shiftDown = true;
    EXPECT_TRUE(click(QPointF(30., 60.)));
    effect->getInteract()->shiftDown = false;
    EXPECT_EQ(2u, context->getSelectedCurves().size());
}

// A brush shape's points and settings survive a save and load.
TEST_F(NativeRetouchTest, BrushSaveLoadRoundTrip)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr node = makeRetouch(makeSource(false), 2);
    ASSERT_TRUE(bool(node));
    RotoStrokeItemPtr brush = makeBrushShape(node, { QPointF(8., 10.), QPointF(20., 30.), QPointF(44., 12.) });
    ASSERT_TRUE(bool(brush));
    RetouchTest::shapeKnob<KnobDouble>(brush, kRetouchShapeParamBrushSize)->setValue(17.);
    RetouchTest::shapeKnob<KnobDouble>(brush, kRetouchShapeParamBrushHardness)->setValue(0.3);
    RetouchTest::shapeKnob<KnobDouble>(brush, kRetouchShapeParamBrushDensity)->setValue(0.7);
    RetouchTest::shapeKnob<KnobChoice>(brush, kRetouchShapeParamBrushPressure)->setValue((int)RC::eBrushPressureOpacityAbsolute);
    RetouchTest::shapeKnob<KnobChoice>(brush, kRetouchShapeParamAlgorithm)->setValue((int)RC::eAlgorithmClone);
    RetouchTest::shapeKnob<KnobDouble>(brush, kRetouchShapeParamSourceOffset)->setValue(-6., ViewSpec::all(), 0);
    RetouchTest::shapeKnob<KnobDouble>(brush, kRetouchShapeParamSourceOffset)->setValue(2.5, ViewSpec::all(), 1);
    const RectI full(0, 0, kParitySourceWidth, kParitySourceHeight);
    const std::vector<std::vector<RC::BrushPoint>> before = Retouch::readBrushStrokes(brush, kTime, 1., full);
    ASSERT_EQ(1u, before.size());
    const std::string nodeName = node->getScriptName();
    const std::string brushName = brush->getScriptName();

    std::string yaml;
    ProjectPtr project = getApp()->getProject();
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("retouchbrush.ntpz"), &yaml));
    node.reset();
    brush.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("retouchbrush.ntpz")));

    NodePtr loaded = project->getNodeByName(nodeName);
    ASSERT_TRUE(bool(loaded));
    RotoCurveContextPtr context = loaded->getCurveContext();
    ASSERT_TRUE(bool(context));
    RotoStrokeItemPtr loadedBrush = std::dynamic_pointer_cast<RotoStrokeItem>(context->getItemByName(brushName));
    ASSERT_TRUE(bool(loadedBrush));

    EXPECT_DOUBLE_EQ(17., RetouchTest::shapeKnob<KnobDouble>(loadedBrush, kRetouchShapeParamBrushSize)->getValue());
    EXPECT_DOUBLE_EQ(0.3, RetouchTest::shapeKnob<KnobDouble>(loadedBrush, kRetouchShapeParamBrushHardness)->getValue());
    EXPECT_DOUBLE_EQ(0.7, RetouchTest::shapeKnob<KnobDouble>(loadedBrush, kRetouchShapeParamBrushDensity)->getValue());
    EXPECT_EQ((int)RC::eBrushPressureOpacityAbsolute, RetouchTest::shapeKnob<KnobChoice>(loadedBrush, kRetouchShapeParamBrushPressure)->getValue());
    const RC::ShapeSettings s = Retouch::readShapeSettings(loadedBrush, kTime);
    EXPECT_EQ(RC::eAlgorithmClone, s.algorithm);
    double dx = 0., dy = 0.;
    Retouch::readShapeSourceOffset(loadedBrush, kTime, &dx, &dy);
    EXPECT_DOUBLE_EQ(-6., dx);
    EXPECT_DOUBLE_EQ(2.5, dy);

    const std::vector<std::vector<RC::BrushPoint>> after = Retouch::readBrushStrokes(loadedBrush, kTime, 1., full);
    ASSERT_EQ(before.size(), after.size());
    ASSERT_EQ(before[0].size(), after[0].size());
    for (std::size_t i = 0; i < before[0].size(); ++i) {
        EXPECT_NEAR(before[0][i].x, after[0][i].x, 1e-4f) << i;
        EXPECT_NEAR(before[0][i].y, after[0][i].y, 1e-4f) << i;
        EXPECT_FLOAT_EQ(before[0][i].radius, after[0][i].radius) << i;
        EXPECT_FLOAT_EQ(before[0][i].density, after[0][i].density) << i;
    }
}

namespace {
// A brush shape painted through the overlay, the way the viewer paints one.
RotoStrokeItemPtr
paintBrush(Retouch* effect,
           const QPointF& start)
{
    const RenderScale scale = RenderScale::identity;
    EXPECT_TRUE(effect->onOverlayPenDown_public(kTime, scale, ViewIdx(0), start, start, 1., 0., ePenTypeLMB));
    RotoStrokeItemPtr stroke = effect->getInteract()->getStrokeBeingPainted();
    for (int i = 1; i <= 4; ++i) {
        const QPointF p(start.x() + 5. * i, start.y() + i);
        EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), p, p, 1., 0.));
    }
    const QPointF end(start.x() + 20., start.y() + 4.);
    EXPECT_TRUE(effect->onOverlayPenUp_public(kTime, scale, ViewIdx(0), end, end, 1., 0.));
    EXPECT_FALSE(bool(effect->getInteract()->getStrokeBeingPainted()));

    return stroke;
}

// RotoPaint's clone gesture: Ctrl held at pen down, the pen dragged from `from` to `to`.
void
ctrlDragSource(Retouch* effect,
               const QPointF& from,
               const QPointF& to)
{
    const RenderScale scale = RenderScale::identity;
    const RetouchInteractPtr& ui = effect->getInteract();
    ui->ctrlDown = 1;
    EXPECT_TRUE(effect->onOverlayPenDown_public(kTime, scale, ViewIdx(0), from, from, 1., 0., ePenTypeLMB));
    EXPECT_EQ(eEventStateDraggingCloneOffset, ui->state);
    const QPointF mid((from.x() + to.x()) / 2., (from.y() + to.y()) / 2.);
    EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), mid, mid, 1., 0.));
    EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), to, to, 1., 0.));
    EXPECT_TRUE(effect->onOverlayPenUp_public(kTime, scale, ViewIdx(0), to, to, 1., 0.));
    EXPECT_EQ(eEventStateNone, ui->state);
    ui->ctrlDown = 0;
}
} // anon namespace

// RotoPaint's clone offset on Retouch's shapes: Ctrl+drag keeps the drawn source where it is while
// the pen moves, makes no shape, and every later shape takes the offset until Reset Clone Offset.
TEST_F(NativeRetouchTest, CtrlDragSetsTheCarriedSourceOffset)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    const RetouchInteractPtr& ui = effect->getInteract();
    ASSERT_TRUE(bool(ui));
    ui->setCurrentTool(ui->drawBrushAction.lock());
    ASSERT_EQ(eRotoToolSolidBrush, ui->getSelectedTool());

    EXPECT_DOUBLE_EQ(0., ui->cloneOffset.first);
    EXPECT_DOUBLE_EQ(0., ui->cloneOffset.second);

    const QPointF press(40., 30.);
    const QPointF release(10., 12.);
    ctrlDragSource(effect, press, release);
    EXPECT_TRUE(context->getCurvesByRenderOrder(false).empty());
    EXPECT_DOUBLE_EQ(press.x() - release.x(), ui->cloneOffset.first);
    EXPECT_DOUBLE_EQ(press.y() - release.y(), ui->cloneOffset.second);
    const std::pair<double, double> carried = ui->cloneOffset;

    RotoStrokeItemPtr first = paintBrush(effect, QPointF(10., 12.));
    RotoStrokeItemPtr second = paintBrush(effect, QPointF(60., 40.));
    ASSERT_TRUE(first && second);
    for (const RotoStrokeItemPtr& brush : { first, second }) {
        double dx = 0., dy = 0.;
        Retouch::readShapeSourceOffset(brush, kTime, &dx, &dy);
        EXPECT_DOUBLE_EQ(carried.first, dx);
        EXPECT_DOUBLE_EQ(carried.second, dy);
    }
    const RectI full(0, 0, kParitySourceWidth, kParitySourceHeight);
    const std::vector<std::vector<RC::BrushPoint>> strokes = Retouch::readBrushStrokes(first, kTime, 1., full);
    ASSERT_EQ(1u, strokes.size());
    EXPECT_GE(strokes[0].size(), 5u);

    // Reset Clone Offset returns to zero, from where the source lands where the Ctrl+drag started,
    // as on a RotoPaint clone stroke.
    KnobButtonPtr reset = ui->resetCloneOffsetButton.lock();
    ASSERT_TRUE(bool(reset));
    EXPECT_EQ(std::string(kRotoUIParamResetCloneOffset), reset->getName());
    EXPECT_EQ(std::string("Reset Clone Offset"), reset->getLabel());
    reset->trigger();
    EXPECT_DOUBLE_EQ(0., ui->cloneOffset.first);
    EXPECT_DOUBLE_EQ(0., ui->cloneOffset.second);
    ui->setCurrentTool(ui->drawEllipseAction.lock());
    ASSERT_EQ(eRotoToolDrawEllipse, ui->getSelectedTool());
    ctrlDragSource(effect, QPointF(70., 50.), QPointF(30., 20.));
    EXPECT_DOUBLE_EQ(40., ui->cloneOffset.first);
    EXPECT_DOUBLE_EQ(30., ui->cloneOffset.second);
}

// RotoPaint's brush size gesture: Shift+drag with the brush adds half the pen's move right plus up
// to the next brush's size, rounded to a hundredth and at least 1, and paints nothing.
TEST_F(NativeRetouchTest, ShiftDragSizesTheBrush)
{
    NodePtr node = makeRetouch(makeSource(false), 0);
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));
    const RetouchInteractPtr& ui = effect->getInteract();
    ASSERT_TRUE(bool(ui));
    ui->setCurrentTool(ui->drawBrushAction.lock());
    ASSERT_EQ(eRotoToolSolidBrush, ui->getSelectedTool());
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kRetouchParamBrushSize));
    KnobDoublePtr shownSize = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kRetouchShapeParamBrushSize));
    ASSERT_TRUE(size && shownSize);
    ASSERT_DOUBLE_EQ(100., size->getValue());

    const RenderScale scale = RenderScale::identity;
    auto shiftDrag = [&](const QPointF& from, const QPointF& to) {
        ui->shiftDown = 1;
        EXPECT_TRUE(effect->onOverlayPenDown_public(kTime, scale, ViewIdx(0), from, from, 1., 0., ePenTypeLMB));
        EXPECT_EQ(eEventStateDraggingBrushSize, ui->state);
        const QPointF mid((from.x() + to.x()) / 2., (from.y() + to.y()) / 2.);
        EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), mid, mid, 1., 1.));
        EXPECT_TRUE(effect->onOverlayPenMotion_public(kTime, scale, ViewIdx(0), to, to, 1., 2.));
        effect->onOverlayPenUp_public(kTime, scale, ViewIdx(0), to, to, 1., 3.);
        EXPECT_EQ(eEventStateNone, ui->state);
        ui->shiftDown = 0;
    };
    shiftDrag(QPointF(40., 30.), QPointF(60., 40.));
    EXPECT_DOUBLE_EQ(115., size->getValue());
    EXPECT_DOUBLE_EQ(115., shownSize->getValue());
    EXPECT_TRUE(context->getCurvesByRenderOrder(false).empty());

    shiftDrag(QPointF(60., 40.), QPointF(-400., -40.));
    EXPECT_DOUBLE_EQ(1., size->getValue());
    EXPECT_TRUE(context->getCurvesByRenderOrder(false).empty());
}

// A Retouch brush gets a new RotoPaint stroke's lifetime knobs, defaults and visibility.
TEST_F(NativeRetouchTest, BrushLifetimeMatchesARotoPaintStroke)
{
    const int paintFrame = 5;
    struct TimelineAt {
        AppInstancePtr app;
        int restore;
        TimelineAt(const AppInstancePtr& a, int frame)
            : app(a)
            , restore(a->getTimeLine()->currentFrame())
        {
            app->getTimeLine()->seekFrame(frame, false, NULL, eTimelineChangeReasonOtherSeek);
        }

        ~TimelineAt()
        {
            app->getTimeLine()->seekFrame(restore, false, NULL, eTimelineChangeReasonOtherSeek);
        }
    };
    const TimelineAt timeline(getApp(), paintFrame);

    NodePtr paintNode = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(paintNode));
    RotoPaint* paint = dynamic_cast<RotoPaint*>(paintNode->getEffectInstance().get());
    ASSERT_TRUE(paint != NULL);
    RotoPaintInteractPtr paintUi = paint->getInteract();
    ASSERT_TRUE(bool(paintUi));
    paintUi->setCurrentTool(paintUi->brushAction.lock());
    ASSERT_EQ(eRotoToolSolidBrush, paintUi->getSelectedTool());
    paintUi->makeStroke(false, RotoPoint(10., 10., 1., 0.));
    RotoStrokeItemPtr paintStroke = paintUi->strokeBeingPaint;
    ASSERT_TRUE(bool(paintStroke));

    NodePtr node = makeRetouch(makeSource(false), 0);
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    const RetouchInteractPtr& ui = effect->getInteract();
    ui->setCurrentTool(ui->drawBrushAction.lock());
    RotoStrokeItemPtr brush = paintBrush(effect, QPointF(10., 12.));
    ASSERT_TRUE(bool(brush));

    KnobChoicePtr paintLifeTime = std::dynamic_pointer_cast<KnobChoice>(paintStroke->getKnobByName(kRotoDrawableItemLifeTimeParam));
    KnobChoicePtr lifeTime = std::dynamic_pointer_cast<KnobChoice>(brush->getKnobByName(kRotoDrawableItemLifeTimeParam));
    ASSERT_TRUE(paintLifeTime && lifeTime);
    EXPECT_EQ(paintLifeTime->getLabel(), lifeTime->getLabel());
    EXPECT_EQ(paintLifeTime->getDefaultValue(0), lifeTime->getDefaultValue(0));
    EXPECT_EQ(paintLifeTime->getValue(), lifeTime->getValue());
    EXPECT_EQ((int)eRotoPaintItemLifeTimeTypeSingle, lifeTime->getValue());
    const std::vector<ChoiceOption> paintModes = paintLifeTime->getEntries_mt_safe();
    const std::vector<ChoiceOption> modes = lifeTime->getEntries_mt_safe();
    ASSERT_EQ(paintModes.size(), modes.size());
    for (std::size_t i = 0; i < modes.size(); ++i) {
        EXPECT_EQ(paintModes[i].id, modes[i].id) << i;
    }
    KnobIntPtr paintFrameKnob = paintStroke->getLifeTimeFrameKnob();
    KnobIntPtr frameKnob = brush->getLifeTimeFrameKnob();
    ASSERT_TRUE(paintFrameKnob && frameKnob);
    EXPECT_EQ(paintFrameKnob->getDefaultValue(0), frameKnob->getDefaultValue(0));
    EXPECT_EQ(paintFrame, paintFrameKnob->getValue());
    EXPECT_EQ(paintFrame, frameKnob->getValue());
    KnobBoolPtr paintActivated = paintStroke->getActivatedKnob();
    KnobBoolPtr activated = brush->getActivatedKnob();
    ASSERT_TRUE(paintActivated && activated);
    EXPECT_EQ(paintActivated->getDefaultValue(0), activated->getDefaultValue(0));

    struct Visibility {
        RotoPaintItemLifeTimeTypeEnum mode;
        bool before, at, after;
    };
    const Visibility cases[] = {
        { eRotoPaintItemLifeTimeTypeAll, true, true, true },
        { eRotoPaintItemLifeTimeTypeSingle, false, true, false },
        { eRotoPaintItemLifeTimeTypeFromStart, true, true, false },
        { eRotoPaintItemLifeTimeTypeToEnd, false, true, true },
        { eRotoPaintItemLifeTimeTypeCustom, true, true, false },
    };
    for (const RotoStrokeItemPtr& item : { paintStroke, brush }) {
        item->getActivatedKnob()->setValueAtTime(paintFrame, true, ViewSpec::all(), 0);
        item->getActivatedKnob()->setValueAtTime(paintFrame + 1, false, ViewSpec::all(), 0);
    }
    for (const Visibility& c : cases) {
        paintLifeTime->setValue((int)c.mode);
        lifeTime->setValue((int)c.mode);
        for (const RotoStrokeItemPtr& item : { paintStroke, brush }) {
            EXPECT_EQ(c.before, item->isActivated(paintFrame - 1)) << c.mode;
            EXPECT_EQ(c.at, item->isActivated(paintFrame)) << c.mode;
            EXPECT_EQ(c.after, item->isActivated(paintFrame + 1)) << c.mode;
        }
    }
}

namespace {
class RetouchRegionTest
    : public RegionIndependenceTest {
protected:
    // A Retouch of source with 3 scales and the two region shapes of algorithm.
    NodePtr makeRetouch(const NodePtr& source,
                        RC::AlgorithmEnum algorithm)
    {
        NodePtr node = createAfter(PLUGINID_NATRON_RETOUCH, source);

        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValues(node, kRetouchParamScales, { 3. }));
        for (const RetouchTest::ShapeSpec& spec : RetouchTest::regionShapeSpecs(algorithm)) {
            EXPECT_TRUE(bool(RetouchTest::makeShape(node, spec, kTime)));
        }

        return node;
    }

    static std::vector<RC::AlgorithmEnum> algorithms()
    {
        return { RC::eAlgorithmClone, RC::eAlgorithmHeal, RC::eAlgorithmBlur, RC::eAlgorithmFill };
    }
};

TEST_F(RetouchRegionTest, RegionIndependentCpu)
{
    for (RC::AlgorithmEnum algorithm : algorithms()) {
        SCOPED_TRACE((int)algorithm);
        expectRegionIndependent([this, algorithm](const NodePtr& source) {
            return makeRetouch(source, algorithm);
        });
    }
}

TEST_F(RetouchRegionTest, InfiniteSourceFails)
{
    for (RC::AlgorithmEnum algorithm : algorithms()) {
        SCOPED_TRACE((int)algorithm);
        expectInfiniteRoDFails([this, algorithm](const NodePtr& source) {
            return makeRetouch(source, algorithm);
        });
    }
}

} // namespace
