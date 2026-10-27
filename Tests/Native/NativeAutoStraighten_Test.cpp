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
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RawReferenceSupport.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Transform/AshiftFit.h"
#include "Engine/Nodes/Transform/AutoStraighten.h"
#include "Engine/Project.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const double kTime = 1.;

// tools/raw/make-reference.py's ASHIFT_CASES: the knob values and the crop box each darktable
// reference was rendered with.
struct ReferenceCase {
    const char* name;
    double rotation;
    double lensShiftV;
    double lensShiftH;
    double shear;
    bool specific;
    double focalLength;
    double cropFactor;
    double lensDependence;
    double aspectAdjust;
    Ashift::CropBox crop;
};

const ReferenceCase kReferenceCases[] = {
    { "rotation", 3., 0., 0., 0., false, 28., 1., 100., 1., { 0., 1., 0., 1. } },
    { "keystone", 0., 0.3, 0., 0., false, 28., 1., 100., 1., { 0.05, 0.95, 0.04, 0.96 } },
    { "all", -2., 0.2, -0.15, 0.05, false, 28., 1., 100., 1., { 0.08, 0.9, 0.06, 0.93 } },
    { "specific", 1.5, -0.25, 0.1, 0., true, 50., 1.5, 60., 1.2, { 0., 1., 0., 1. } },
};

// Half-float references and float maths on both sides: the difference is the reference's
// rounding plus the interpolation's float error.
const double kReferenceMaxError = 2e-3;
const double kReferenceMeanError = 5e-4;
// Output pixels whose input sample lies closer than this to the input's edge are skipped:
// darktable and Natron treat the samples outside the image differently.
const double kEdgeMargin = 2.;

void
mapPoint(const TransformMath::Mat3& m,
         double x,
         double y,
         double* ox,
         double* oy)
{
    const double w = m.m[6] * x + m.m[7] * y + m.m[8];

    *ox = (m.m[0] * x + m.m[1] * y + m.m[2]) / w;
    *oy = (m.m[3] * x + m.m[4] * y + m.m[5]) / w;
}

std::string
persistentMessage(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return message.toStdString();
}

KnobDoublePtr
doubleKnob(const NodePtr& node,
           const std::string& name)
{
    return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
}

float
pattern(int x,
        int y,
        int c)
{
    if (c == 3) {
        return 1.f;
    }

    return (float)(0.2 + 0.1 * c + 0.3 * std::sin(x * 0.21 + c) * std::cos(y * 0.17));
}
} // namespace

class NativeAutoStraightenTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    void resetProject(int width,
                      int height)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, width, height, "nativeAutoStraighten", 1.));
    }

    // A source of width x height pixels of value, under an AutoStraighten.
    NodePtr createChain(int width,
                        int height,
                        const RawTestSource::ValueFn& value)
    {
        resetProject(width, height);
        NodePtr source = createNode(QString::fromUtf8(kImageTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_AUTOSTRAIGHTEN), PLUGIN_MAJOR_NATRON_AUTOSTRAIGHTEN);

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, width, height), 4, value, ImageMetadata());
        connectNodes(source, node, 0, true);

        return node;
    }

    static AutoStraighten* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<AutoStraighten*>(node->getEffectInstance().get()) : NULL;
    }

    static bool setReferenceCase(const NodePtr& node,
                                 const ReferenceCase& c)
    {
        return setKnobValues(node, kAutoStraightenParamRotation, { c.rotation }) && setKnobValues(node, kAutoStraightenParamLensShiftV, { c.lensShiftV })
            && setKnobValues(node, kAutoStraightenParamLensShiftH, { c.lensShiftH }) && setKnobValues(node, kAutoStraightenParamShear, { c.shear })
            && setKnobValue(node, kAutoStraightenParamLensModel, std::string(c.specific ? "specific" : "generic"))
            && setKnobValues(node, kAutoStraightenParamFocalLength, { c.focalLength }) && setKnobValues(node, kAutoStraightenParamCropFactor, { c.cropFactor })
            && setKnobValues(node, kAutoStraightenParamLensDependence, { c.lensDependence })
            && setKnobValues(node, kAutoStraightenParamAspectAdjust, { c.aspectAdjust })
            && setKnobValue(node, kResamplerParamFilterType, std::string("bilinear")) && setKnobValues(node, kResamplerParamFilterClamp, { 0. })
            && setKnobValues(node, kResamplerParamFilterBlackOutside, { 1. });
    }

    static RenderedPlane render(const NodePtr& node,
                                const RectI& window)
    {
        RenderedPlane plane;
        std::string error;

        EXPECT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;

        return plane;
    }
};

// At its defaults the node changes nothing: no geometry, the source's region and its pixels.
TEST_F(NativeAutoStraightenTest, DefaultsAreAnIdentity)
{
    NodePtr node = createChain(64, 48, pattern);
    ASSERT_TRUE(bool(node));
    AutoStraighten* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const AutoStraighten::Geometry g = effect->getGeometry(kTime, ViewIdx(0));
    EXPECT_TRUE(g.bypass);
    EXPECT_EQ(RectI(0, 0, 64, 48), g.output);
    const RenderedPlane plane = render(node, RectI(0, 0, 64, 48));
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 64; ++x) {
            for (int c = 0; c < 4; ++c) {
                ASSERT_EQ(pattern(x, y, c), rawTestPlaneValue(plane, x, y, c)) << x << "," << y << " channel " << c;
            }
        }
    }
}

// Each automatic crop mode sizes the output from its own crop box: off keeps the whole corrected
// image, largest and original crop inside it, original at the source's aspect.
TEST_F(NativeAutoStraightenTest, AutoCropModesSizeTheOutput)
{
    NodePtr node = createChain(160, 120, pattern);
    ASSERT_TRUE(bool(node));
    AutoStraighten* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValues(node, kAutoStraightenParamRotation, { 4. }));
    ASSERT_TRUE(setKnobValues(node, kAutoStraightenParamLensShiftV, { 0.25 }));

    int fullWidth = 0, fullHeight = 0;
    Ashift::outputSize(effect->getModel(kTime, ViewIdx(0)), 160, 120, 0., 1., 0., 1., &fullWidth, &fullHeight);
    const struct {
        const char* id;
        Ashift::CropModeEnum mode;
    } modes[] = { { "off", Ashift::eCropOff }, { "largest", Ashift::eCropLargest }, { "original", Ashift::eCropAspect } };
    for (const auto& m : modes) {
        ASSERT_TRUE(setKnobValue(node, kAutoStraightenParamAutoCrop, std::string(m.id)));
        const AutoStraighten::Geometry g = effect->getGeometry(kTime, ViewIdx(0));
        ASSERT_FALSE(g.bypass) << m.id;
        Ashift::CropBox box;
        ASSERT_TRUE(Ashift::computeCrop(effect->getModel(kTime, ViewIdx(0)), 160, 120, m.mode, &box)) << m.id;
        EXPECT_DOUBLE_EQ(box.cl, g.crop.cl) << m.id;
        EXPECT_DOUBLE_EQ(box.cr, g.crop.cr) << m.id;
        EXPECT_DOUBLE_EQ(box.ct, g.crop.ct) << m.id;
        EXPECT_DOUBLE_EQ(box.cb, g.crop.cb) << m.id;
        EXPECT_EQ(g.outWidth, g.output.width()) << m.id;
        EXPECT_EQ(g.outHeight, g.output.height()) << m.id;
        if (m.mode == Ashift::eCropOff) {
            EXPECT_EQ(fullWidth, g.outWidth);
            EXPECT_EQ(fullHeight, g.outHeight);
        } else {
            EXPECT_LT(g.outWidth, fullWidth) << m.id;
            EXPECT_LT(g.outHeight, fullHeight) << m.id;
        }
        if (m.mode == Ashift::eCropAspect) {
            EXPECT_NEAR(160. / 120., (double)g.outWidth / g.outHeight, 0.03);
        }
        const RenderedPlane plane = render(node, g.output);
        EXPECT_EQ(4u, plane.channels.size()) << m.id;
    }
}

// The crop box of either cropping mode lies inside the corrected image: its corners map back
// inside the source.
TEST_F(NativeAutoStraightenTest, CropBoxCornersMapInsideTheSource)
{
    const int width = 300, height = 200;
    Ashift::Model models[3];
    models[0].rotation = 5.;
    models[1].lensShiftV = 0.4;
    models[1].lensShiftH = -0.2;
    models[2].rotation = -3.;
    models[2].lensShiftV = -0.3;
    models[2].shear = 0.1;
    for (int i = 0; i < 3; ++i) {
        for (Ashift::CropModeEnum mode : { Ashift::eCropLargest, Ashift::eCropAspect }) {
            Ashift::CropBox box;
            ASSERT_TRUE(Ashift::computeCrop(models[i], width, height, mode, &box)) << i;
            EXPECT_LE(0., box.cl);
            EXPECT_LT(box.cl, box.cr);
            EXPECT_LE(box.cr, 1.);
            EXPECT_LE(0., box.ct);
            EXPECT_LT(box.ct, box.cb);
            EXPECT_LE(box.cb, 1.);
            EXPECT_GT((box.cr - box.cl) * (box.cb - box.ct), 0.3) << "model " << i << " mode " << mode;
            int fullWidth = 0, fullHeight = 0;
            Ashift::outputSize(models[i], width, height, 0., 1., 0., 1., &fullWidth, &fullHeight);
            const TransformMath::Mat3 inverse = Ashift::homography(models[i], width, height, true);
            const double xs[2] = { box.cl * fullWidth, box.cr * fullWidth };
            const double ys[2] = { box.ct * fullHeight, box.cb * fullHeight };
            for (double x : xs) {
                for (double y : ys) {
                    double ix = 0., iy = 0.;
                    mapPoint(inverse, x, y, &ix, &iy);
                    EXPECT_GE(ix, -1.5) << "model " << i << " mode " << mode;
                    EXPECT_LE(ix, width + 1.5) << "model " << i << " mode " << mode;
                    EXPECT_GE(iy, -1.5) << "model " << i << " mode " << mode;
                    EXPECT_LE(iy, height + 1.5) << "model " << i << " mode " << mode;
                }
            }
            if (mode == Ashift::eCropAspect) {
                EXPECT_NEAR((double)width / height, (box.cr - box.cl) * fullWidth / ((box.cb - box.ct) * fullHeight), 0.02) << i;
            }
        }
    }
    Ashift::CropBox off;
    ASSERT_TRUE(Ashift::computeCrop(models[0], width, height, Ashift::eCropOff, &off));
    EXPECT_EQ(0., off.cl);
    EXPECT_EQ(1., off.cr);
    EXPECT_EQ(0., off.ct);
    EXPECT_EQ(1., off.cb);
}

// darktable's ashift on tools/raw/make-reference.py's synthetic source, the crop box injected as
// darktable-cli applies it, compared away from the source's edges.
TEST_F(NativeAutoStraightenTest, MatchesDarktableReferences)
{
    const std::string dir = std::string(NATRON_TESTS_FIXTURES_DIR "/raw/reference/");
    std::vector<float> source;
    int sourceWidth = 0, sourceHeight = 0;
    std::string error;
    ASSERT_TRUE(RawReference::readReference(dir + "ashift-source.tif", &source, &sourceWidth, &sourceHeight, &error)) << error;
    const RawTestSource::ValueFn value = [&source, sourceWidth, sourceHeight](int x, int y, int c) {
        if (c == 3) {
            return 1.f;
        }
        // The reference's top row is the source's last.
        return source[((std::size_t)(sourceHeight - 1 - y) * sourceWidth + x) * 3 + c];
    };

    for (const ReferenceCase& rc : kReferenceCases) {
        std::vector<float> expected;
        int refWidth = 0, refHeight = 0;
        ASSERT_TRUE(RawReference::readReference(dir + "ashift-" + rc.name + ".tif", &expected, &refWidth, &refHeight, &error)) << error;

        NodePtr node = createChain(sourceWidth, sourceHeight, value);
        ASSERT_TRUE(bool(node));
        AutoStraighten* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        const Ashift::CropBox crop = rc.crop;
        effect->setCropOverride([crop](const Ashift::Model&, int, int, Ashift::CropModeEnum, Ashift::CropBox* box) {
            *box = crop;

            return true;
        });
        ASSERT_TRUE(setReferenceCase(node, rc)) << rc.name;

        const AutoStraighten::Geometry g = effect->getGeometry(kTime, ViewIdx(0));
        ASSERT_FALSE(g.bypass) << rc.name;
        ASSERT_EQ(refWidth, g.output.width()) << rc.name;
        ASSERT_EQ(refHeight, g.output.height()) << rc.name;
        const RenderedPlane plane = render(node, g.output);
        ASSERT_EQ(4u, plane.channels.size()) << rc.name;

        const Ashift::Model model = effect->getModel(kTime, ViewIdx(0));
        const TransformMath::Mat3 inverse = Ashift::homography(model, g.width, g.height, true);
        const double cx = g.outWidth / (g.crop.cr - g.crop.cl) * g.crop.cl;
        const double cy = g.outHeight / (g.crop.cb - g.crop.ct) * g.crop.ct;
        double sum = 0., worst = 0.;
        int count = 0, worstX = 0, worstY = 0;
        for (int j = 0; j < refHeight; ++j) {
            for (int i = 0; i < refWidth; ++i) {
                double ix = 0., iy = 0.;
                mapPoint(inverse, i + cx, j + cy, &ix, &iy);
                if ((ix < kEdgeMargin) || (ix > sourceWidth - 1 - kEdgeMargin) || (iy < kEdgeMargin) || (iy > sourceHeight - 1 - kEdgeMargin)) {
                    continue;
                }
                const int x = g.output.x1 + i;
                const int y = g.output.y2 - 1 - j;
                for (int c = 0; c < 3; ++c) {
                    const double d = std::fabs(rawTestPlaneValue(plane, x, y, c) - expected[((std::size_t)j * refWidth + i) * 3 + c]);
                    sum += d;
                    ++count;
                    if (d > worst) {
                        worst = d;
                        worstX = i;
                        worstY = j;
                    }
                }
            }
        }
        ASSERT_GT(count, refWidth * refHeight) << rc.name << ": too few pixels compared";
        EXPECT_LE(worst, kReferenceMaxError) << rc.name << " at " << worstX << "," << worstY << " (top row 0)";
        EXPECT_LE(sum / count, kReferenceMeanError) << rc.name;
    }
}

// Rectangle mode: corners that a known model makes rectangular fit back to that model's knob
// values, all four set in one go.
TEST_F(NativeAutoStraightenTest, RectangleFitRecoversTheModel)
{
    const int width = 320, height = 240;
    NodePtr node = createChain(width, height, pattern);
    ASSERT_TRUE(bool(node));
    AutoStraighten* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const double rotationKnob = 2.5, lensShiftV = 0.2, lensShiftH = -0.1;
    Ashift::Model truth;
    truth.rotation = -rotationKnob;
    truth.lensShiftV = lensShiftV;
    truth.lensShiftH = lensShiftH;
    const TransformMath::Mat3 forward = Ashift::homography(truth, width, height, false);
    const TransformMath::Mat3 inverse = Ashift::homography(truth, width, height, true);
    double cx = 0., cy = 0.;
    mapPoint(forward, width / 2., height / 2., &cx, &cy);
    // Top left, top right, bottom left, bottom right, in the corrected image (y down).
    const double corners[4][2] = { { cx - 90., cy - 70. }, { cx + 90., cy - 70. }, { cx - 90., cy + 70. }, { cx + 90., cy + 70. } };
    const char* const names[4] = { kAutoStraightenParamTopLeft, kAutoStraightenParamTopRight, kAutoStraightenParamBottomLeft, kAutoStraightenParamBottomRight };
    for (int i = 0; i < 4; ++i) {
        double ix = 0., iy = 0.;
        mapPoint(inverse, corners[i][0], corners[i][1], &ix, &iy);
        // Pixel (i, j) of darktable's input is canonical (i + 0.5, height - 0.5 - j).
        ASSERT_TRUE(setKnobValues(node, names[i], { ix + 0.5, height - 0.5 - iy })) << names[i];
    }
    ASSERT_TRUE(setKnobValue(node, kAutoStraightenParamStructure, std::string("rectangle")));
    ASSERT_TRUE(setKnobValue(node, kAutoStraightenParamFitParameters, std::string("rotationAndLensShift")));

    ASSERT_TRUE(effect->fit(AutoStraighten::eFitButtonBoth, kTime, ViewIdx(0))) << persistentMessage(node);
    EXPECT_NEAR(rotationKnob, doubleKnob(node, kAutoStraightenParamRotation)->getValue(), 0.05);
    EXPECT_NEAR(lensShiftV, doubleKnob(node, kAutoStraightenParamLensShiftV)->getValue(), 0.01);
    EXPECT_NEAR(lensShiftH, doubleKnob(node, kAutoStraightenParamLensShiftH)->getValue(), 0.01);
    EXPECT_EQ(0., doubleKnob(node, kAutoStraightenParamShear)->getValue());
    EXPECT_EQ(4u, effect->lines(kTime).lines.size());
}

namespace {
// A guide line in the corrected image (darktable pixels, y down), mapped back through the model
// into the source: what a user straightening the plate traces.
struct GuideLine {
    double x1, y1, x2, y2;
};

BezierPtr
drawLine(const NodePtr& node,
         const TransformMath::Mat3& inverse,
         int height,
         const GuideLine& line)
{
    RotoCurveContextPtr context = node->getCurveContext();
    if (!context) {
        return BezierPtr();
    }
    double ax = 0., ay = 0., bx = 0., by = 0.;
    mapPoint(inverse, line.x1, line.y1, &ax, &ay);
    mapPoint(inverse, line.x2, line.y2, &bx, &by);
    // Pixel (i, j) of darktable's input is canonical (i + 0.5, height - 0.5 - j).
    BezierPtr bezier = context->makeBezier(ax + 0.5, height - 0.5 - ay, "Line", kTime, true /*isOpenBezier*/);
    if (bezier) {
        bezier->addControlPoint(bx + 0.5, height - 0.5 - by, kTime);
        bezier->setCurveFinished(true);
    }

    return bezier;
}
} // namespace

// Drawn Lines mode: guide lines that a known model makes vertical and horizontal fit back to
// that model, verticals alone for Fit Vertical and both sets for Fit Both.
TEST_F(NativeAutoStraightenTest, DrawnLinesFitRecoversTheModel)
{
    const int width = 320, height = 240;
    NodePtr node = createChain(width, height, pattern);
    ASSERT_TRUE(bool(node));
    AutoStraighten* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    RotoCurveContextPtr context = node->getCurveContext();
    ASSERT_TRUE(bool(context));

    // Fit Vertical, every parameter: rotation and vertical lens shift from verticals alone.
    {
        const double rotationKnob = 2., lensShiftV = 0.25;
        Ashift::Model truth;
        truth.rotation = -rotationKnob;
        truth.lensShiftV = lensShiftV;
        const TransformMath::Mat3 forward = Ashift::homography(truth, width, height, false);
        const TransformMath::Mat3 inverse = Ashift::homography(truth, width, height, true);
        double cx = 0., cy = 0.;
        mapPoint(forward, width / 2., height / 2., &cx, &cy);

        // One is drawn bottom to top; each is tagged by its direction.
        std::list<BezierPtr> lines;
        for (const GuideLine& g : { GuideLine { cx - 100., cy - 80., cx - 100., cy + 80. }, GuideLine { cx, cy - 70., cx, cy + 70. },
                                    GuideLine { cx + 100., cy + 80., cx + 100., cy - 80. } }) {
            BezierPtr line = drawLine(node, inverse, height, g);
            ASSERT_TRUE(bool(line));
            effect->setLineOrientationFromDirection(line, kTime);
            AutoStraighten::LineOrientationEnum orientation;
            ASSERT_TRUE(AutoStraighten::getLineOrientation(line, &orientation));
            EXPECT_EQ(AutoStraighten::eLineOrientationVertical, orientation);
            EXPECT_EQ(std::string("Vertical"), effect->getItemText(line, 0));
            lines.push_back(line);
        }

        ASSERT_TRUE(effect->fit(AutoStraighten::eFitButtonVertical, kTime, ViewIdx(0))) << persistentMessage(node);
        EXPECT_EQ(3u, effect->lines(kTime).lines.size());
        EXPECT_NEAR(rotationKnob, doubleKnob(node, kAutoStraightenParamRotation)->getValue(), 0.05);
        EXPECT_NEAR(lensShiftV, doubleKnob(node, kAutoStraightenParamLensShiftV)->getValue(), 0.01);
        EXPECT_EQ(0., doubleKnob(node, kAutoStraightenParamLensShiftH)->getValue());

        for (const BezierPtr& line : lines) {
            context->removeItem(line);
        }
        for (const char* name : { kAutoStraightenParamRotation, kAutoStraightenParamLensShiftV }) {
            ASSERT_TRUE(setKnobValues(node, name, { 0. })) << name;
        }
    }

    // Fit Both, rotation and lens shift: verticals and horizontals, the horizontals tagged
    // explicitly.
    const double rotationKnob = -1.5, lensShiftV = 0.2, lensShiftH = -0.1;
    Ashift::Model truth;
    truth.rotation = -rotationKnob;
    truth.lensShiftV = lensShiftV;
    truth.lensShiftH = lensShiftH;
    const TransformMath::Mat3 forward = Ashift::homography(truth, width, height, false);
    const TransformMath::Mat3 inverse = Ashift::homography(truth, width, height, true);
    double cx = 0., cy = 0.;
    mapPoint(forward, width / 2., height / 2., &cx, &cy);
    for (const GuideLine& g : { GuideLine { cx - 100., cy - 80., cx - 100., cy + 80. }, GuideLine { cx + 100., cy - 80., cx + 100., cy + 80. },
                                GuideLine { cx, cy - 70., cx, cy + 70. } }) {
        BezierPtr line = drawLine(node, inverse, height, g);
        ASSERT_TRUE(bool(line));
        effect->setLineOrientation(line, AutoStraighten::eLineOrientationVertical);
    }
    for (const GuideLine& g : { GuideLine { cx - 110., cy - 60., cx + 110., cy - 60. }, GuideLine { cx - 90., cy + 75., cx + 90., cy + 75. },
                                GuideLine { cx - 100., cy, cx + 100., cy } }) {
        BezierPtr line = drawLine(node, inverse, height, g);
        ASSERT_TRUE(bool(line));
        effect->setLineOrientation(line, AutoStraighten::eLineOrientationHorizontal);
        EXPECT_EQ(std::string("Horizontal"), effect->getItemText(line, 0));
    }
    ASSERT_TRUE(setKnobValue(node, kAutoStraightenParamFitParameters, std::string("rotationAndLensShift")));
    ASSERT_TRUE(effect->fit(AutoStraighten::eFitButtonBoth, kTime, ViewIdx(0))) << persistentMessage(node);
    EXPECT_EQ(6u, effect->lines(kTime).lines.size());
    EXPECT_NEAR(rotationKnob, doubleKnob(node, kAutoStraightenParamRotation)->getValue(), 0.05);
    EXPECT_NEAR(lensShiftV, doubleKnob(node, kAutoStraightenParamLensShiftV)->getValue(), 0.01);
    EXPECT_NEAR(lensShiftH, doubleKnob(node, kAutoStraightenParamLensShiftH)->getValue(), 0.01);
    EXPECT_EQ(0., doubleKnob(node, kAutoStraightenParamShear)->getValue());
}

// Drawn Lines mode with too few lines in the fitted direction fails and leaves the knobs alone;
// a line drawn mostly sideways is tagged horizontal and does not count as a vertical.
TEST_F(NativeAutoStraightenTest, TooFewDrawnLinesFail)
{
    const int width = 160, height = 120;
    NodePtr node = createChain(width, height, pattern);
    ASSERT_TRUE(bool(node));
    AutoStraighten* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const TransformMath::Mat3 identity = Ashift::homography(Ashift::Model(), width, height, true);
    EXPECT_FALSE(effect->fit(AutoStraighten::eFitButtonVertical, kTime, ViewIdx(0)));
    EXPECT_EQ("not enough guide lines for automatic correction\nminimum 2 lines in each relevant direction", persistentMessage(node));

    BezierPtr vertical = drawLine(node, identity, height, GuideLine { 40., 10., 42., 100. });
    BezierPtr sideways = drawLine(node, identity, height, GuideLine { 20., 50., 140., 60. });
    ASSERT_TRUE(vertical && sideways);
    effect->setLineOrientationFromDirection(vertical, kTime);
    effect->setLineOrientationFromDirection(sideways, kTime);
    AutoStraighten::LineOrientationEnum orientation;
    ASSERT_TRUE(AutoStraighten::getLineOrientation(sideways, &orientation));
    EXPECT_EQ(AutoStraighten::eLineOrientationHorizontal, orientation);

    EXPECT_FALSE(effect->fit(AutoStraighten::eFitButtonVertical, kTime, ViewIdx(0)));
    EXPECT_EQ(0., doubleKnob(node, kAutoStraightenParamRotation)->getValue());
    EXPECT_EQ(0., doubleKnob(node, kAutoStraightenParamLensShiftV)->getValue());

    // The panel menu retags it, undoably.
    std::list<RotoItemPtr> items;
    items.push_back(sideways);
    effect->onItemMenuActionTriggered(kAutoStraightenActionSetVertical, items);
    ASSERT_TRUE(AutoStraighten::getLineOrientation(sideways, &orientation));
    EXPECT_EQ(AutoStraighten::eLineOrientationVertical, orientation);
}

// dt_iop_ashift_params_t against the knobs: each field's knob, default and range. The crop box
// is computed, not stored (its interactive adjustment belongs to the viewer work of M165), the
// drawn lines are the node's curves with their own orientation knob, and last_quad_lines is the
// four corner knobs.
TEST_F(NativeAutoStraightenTest, KnobsCoverDarktableParameters)
{
    NodePtr node = createChain(64, 48, pattern);
    ASSERT_TRUE(bool(node));

    const struct {
        const char* field;
        const char* knob;
        const char* label;
        double def;
        double minimum;
        double maximum;
    } doubles[] = {
        { "rotation", kAutoStraightenParamRotation, "Rotation", 0., -180., 180. },
        { "lensshift_v", kAutoStraightenParamLensShiftV, "Lens Shift V", 0., -2., 2. },
        { "lensshift_h", kAutoStraightenParamLensShiftH, "Lens Shift H", 0., -2., 2. },
        { "shear", kAutoStraightenParamShear, "Shear", 0., -0.5, 0.5 },
        { "f_length", kAutoStraightenParamFocalLength, "Focal Length", 28., 1., 2000. },
        { "crop_factor", kAutoStraightenParamCropFactor, "Crop Factor", 1., 0.5, 10. },
        { "orthocorr", kAutoStraightenParamLensDependence, "Lens Dependence", 100., 0., 100. },
        { "aspect", kAutoStraightenParamAspectAdjust, "Aspect Adjust", 1., 0.5, 2. },
    };
    for (const auto& d : doubles) {
        KnobDoublePtr knob = doubleKnob(node, d.knob);
        ASSERT_TRUE(bool(knob)) << d.field;
        EXPECT_EQ(d.label, knob->getLabel()) << d.field;
        EXPECT_EQ(d.def, knob->getDefaultValue(0)) << d.field;
        EXPECT_EQ(d.minimum, knob->getMinimum(0)) << d.field;
        EXPECT_EQ(d.maximum, knob->getMaximum(0)) << d.field;
    }

    const struct {
        const char* field;
        const char* knob;
        const char* label;
        int def;
        std::vector<std::string> ids;
    } choices[] = {
        { "mode", kAutoStraightenParamLensModel, "Lens Model", 0, { "generic", "specific" } },
        { "cropmode", kAutoStraightenParamAutoCrop, "Auto Crop", 1, { "off", "largest", "original" } },
        { "(gui) structure", kAutoStraightenParamStructure, "Structure", 0, { "lines", "rectangle" } },
        { "(gui) fit parameters", kAutoStraightenParamFitParameters, "Fit Parameters", 0, { "all", "rotation", "lensShift", "rotationAndLensShift" } },
    };
    for (const auto& c : choices) {
        KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(c.knob));
        ASSERT_TRUE(bool(knob)) << c.field;
        EXPECT_EQ(c.label, knob->getLabel()) << c.field;
        EXPECT_EQ(c.def, knob->getDefaultValue(0)) << c.field;
        const std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();
        ASSERT_EQ(c.ids.size(), entries.size()) << c.field;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            EXPECT_EQ(c.ids[i], entries[i].id) << c.field;
        }
    }

    const struct {
        const char* knob;
        const char* label;
    } buttons[] = {
        { kAutoStraightenParamFitVertical, "Fit Vertical" },
        { kAutoStraightenParamFitHorizontal, "Fit Horizontal" },
        { kAutoStraightenParamFitBoth, "Fit Both" },
    };
    for (const auto& b : buttons) {
        KnobIPtr knob = node->getKnobByName(b.knob);
        ASSERT_TRUE(bool(std::dynamic_pointer_cast<KnobButton>(knob))) << b.knob;
        EXPECT_EQ(b.label, knob->getLabel()) << b.knob;
    }

    // last_quad_lines: darktable's default rectangle at 20% and 80% of the image.
    const struct {
        const char* knob;
        const char* label;
        double x;
        double y;
    } corners[] = {
        { kAutoStraightenParamTopLeft, "Top Left", 0.2, 0.8 },
        { kAutoStraightenParamTopRight, "Top Right", 0.8, 0.8 },
        { kAutoStraightenParamBottomLeft, "Bottom Left", 0.2, 0.2 },
        { kAutoStraightenParamBottomRight, "Bottom Right", 0.8, 0.2 },
    };
    for (const auto& c : corners) {
        KnobDoublePtr knob = doubleKnob(node, c.knob);
        ASSERT_TRUE(bool(knob)) << c.knob;
        ASSERT_EQ(2, knob->getDimension()) << c.knob;
        EXPECT_EQ(c.label, knob->getLabel()) << c.knob;
        EXPECT_TRUE(knob->getIsSecret()) << c.knob << " outside rectangle mode";
        EXPECT_NEAR(c.x * 64, knob->getValue(0), 1e-9) << c.knob;
        EXPECT_NEAR(c.y * 48, knob->getValue(1), 1e-9) << c.knob;
    }
    ASSERT_TRUE(setKnobValue(node, kAutoStraightenParamStructure, std::string("rectangle")));
    for (const auto& c : corners) {
        EXPECT_FALSE(doubleKnob(node, c.knob)->getIsSecret()) << c.knob << " in rectangle mode";
    }
    EXPECT_TRUE(node->getKnobByName(kAutoStraightenParamLineOrientation)->getIsSecret());

    // The specific lens knobs show only for the specific lens model.
    for (const char* name : { kAutoStraightenParamFocalLength, kAutoStraightenParamCropFactor, kAutoStraightenParamLensDependence, kAutoStraightenParamAspectAdjust }) {
        EXPECT_TRUE(node->getKnobByName(name)->getIsSecret()) << name;
    }
    ASSERT_TRUE(setKnobValue(node, kAutoStraightenParamLensModel, std::string("specific")));
    for (const char* name : { kAutoStraightenParamFocalLength, kAutoStraightenParamCropFactor, kAutoStraightenParamLensDependence, kAutoStraightenParamAspectAdjust }) {
        EXPECT_FALSE(node->getKnobByName(name)->getIsSecret()) << name;
    }

    // Excluded fields: no knob stands for them.
    for (const char* name : { "cl", "cr", "ct", "cb", "cropBox", "lastDrawnLines", "lastQuadLines" }) {
        EXPECT_FALSE(bool(node->getKnobByName(name))) << name;
    }
}
