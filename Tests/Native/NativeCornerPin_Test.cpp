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

#include <cmath>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/CornerPin.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/Settings.h"
#include "Engine/UndoCommand.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

using TransformMath::Mat3;

namespace {

const char* const kCornerPinID = PLUGINID_NATRON_CORNERPIN;
const int kCornerPinMajor = PLUGIN_MAJOR_NATRON_CORNERPIN;
const double kTime = 1.;

// Both sides resample once through the same matrix, composed in a different order.
const double kResamplingTolerance = 1e-5;
// Motion blur averages many samples, each with the rounding above.
const double kMotionBlurTolerance = 1e-4;
const double kMatrixTolerance = 1e-9;

const char* const kEnableNames[4] = { kCornerPinNodeParamEnable1, kCornerPinNodeParamEnable2, kCornerPinNodeParamEnable3, kCornerPinNodeParamEnable4 };

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

CornerPinNode*
cornerPin(const NodePtr& node)
{
    return node ? dynamic_cast<CornerPinNode*>(node->getEffectInstance().get()) : NULL;
}

// Point i of pin enabled and mapping (fx, fy) to (tx, ty).
void
setPoint(CornerPinPoints* pin,
         int i,
         double fx,
         double fy,
         double tx,
         double ty)
{
    pin->enable[i] = true;
    pin->from[i][0] = fx;
    pin->from[i][1] = fy;
    pin->to[i][0] = tx;
    pin->to[i][1] = ty;
}

CornerPinPoints
noPoints()
{
    CornerPinPoints pin;

    for (int i = 0; i < 4; ++i) {
        pin.enable[i] = false;
    }

    return pin;
}

void
expectMatrixNear(const Mat3& expected,
                 const Mat3& actual,
                 const char* what)
{
    ASSERT_NE(0., expected.m[8]) << what;
    ASSERT_NE(0., actual.m[8]) << what;
    for (int k = 0; k < 9; ++k) {
        EXPECT_NEAR(expected.m[k] / expected.m[8], actual.m[k] / actual.m[8], kMatrixTolerance) << what << ", element " << k;
    }
}

Mat3
sampling(const CornerPinPoints& pin,
         bool invert = false,
         double amount = 1.)
{
    Mat3 m;

    EXPECT_TRUE(CornerPinOp::inverseMatrix(pin, amount, invert, &m));

    return m;
}

double
zAt(const Mat3& m,
    double x,
    double y)
{
    return (m * TransformMath::Point3(x, y, 1.)).z;
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

void
expectPlanesNear(const RenderedPlane& a,
                 const RenderedPlane& b,
                 double tolerance,
                 const char* what)
{
    ASSERT_EQ(a.pixels.size(), b.pixels.size()) << what;
    double maxDiff = 0.;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        maxDiff = std::max(maxDiff, (double)std::abs(a.pixels[i] - b.pixels[i]));
    }
    EXPECT_LE(maxDiff, tolerance) << what;
}

bool
setPoints(const NodePtr& node,
          const char* name,
          double x,
          double y)
{
    return setKnobValues(node, name, { x, y });
}

bool
setExtraMatrix(const NodePtr& node,
               const Mat3& m)
{
    return setKnobValues(node, kCornerPinNodeParamExtraMatrix, std::vector<double>(m.m, m.m + 9));
}

bool
disableAllPoints(const NodePtr& node)
{
    for (int i = 0; i < 4; ++i) {
        if (!setKnobValues(node, kEnableNames[i], { 0. })) {
            return false;
        }
    }

    return true;
}

// A mild perspective pin on the 64x48 source.
bool
setPerspectivePin(const NodePtr& node)
{
    return setPoints(node, kCornerPinNodeParamFrom1, 0., 0.) && setPoints(node, kCornerPinNodeParamFrom2, 64., 0.) && setPoints(node, kCornerPinNodeParamFrom3, 64., 48.) && setPoints(node, kCornerPinNodeParamFrom4, 0., 48.) && setPoints(node, kCornerPinNodeParamTo1, 4., 2.) && setPoints(node, kCornerPinNodeParamTo2, 58., 6.) && setPoints(node, kCornerPinNodeParamTo3, 62., 44.) && setPoints(node, kCornerPinNodeParamTo4, 3., 40.);
}

} // namespace

TEST(NativeCornerPinMatrix, OnePointIsATranslation)
{
    CornerPinPoints pin = noPoints();
    setPoint(&pin, 0, 10., 20., 15., 27.);
    expectMatrixNear(Mat3(1., 0., -5., 0., 1., -7., 0., 0., 1.), sampling(pin), "1 point");
}

TEST(NativeCornerPinMatrix, TwoPointsAreASimilarity)
{
    CornerPinPoints pin = noPoints();
    setPoint(&pin, 0, 0., 0., 0., 0.);
    setPoint(&pin, 1, 10., 0., 0., 20.);
    expectMatrixNear(Mat3(0., 0.5, 0., -0.5, 0., 0., 0., 0., 1.), sampling(pin), "2 points");
}

TEST(NativeCornerPinMatrix, ThreePointsAreAffine)
{
    CornerPinPoints pin = noPoints();
    setPoint(&pin, 0, 0., 0., 5., 5.);
    setPoint(&pin, 1, 10., 0., 25., 5.);
    setPoint(&pin, 2, 0., 10., 5., 15.);
    expectMatrixNear(Mat3(0.5, 0., -2.5, 0., 1., -5., 0., 0., 1.), sampling(pin), "3 points");
}

TEST(NativeCornerPinMatrix, FourPointsAreAHomography)
{
    // "to" is "from" under F, so the sampling matrix is F's inverse.
    const Mat3 F(1., 0., 0., 0., 1., 0., 0.01, 0., 1.);
    const Mat3 Finv(1., 0., 0., 0., 1., 0., -0.01, 0., 1.);
    const double from[4][2] = { { 0., 0. }, { 10., 0. }, { 10., 10. }, { 0., 10. } };
    CornerPinPoints pin;
    for (int i = 0; i < 4; ++i) {
        const TransformMath::Point3 q = F * TransformMath::Point3(from[i][0], from[i][1], 1.);
        setPoint(&pin, i, from[i][0], from[i][1], q.x / q.z, q.y / q.z);
    }
    const Mat3 S = sampling(pin);
    expectMatrixNear(Finv, S, "4 points");
    EXPECT_GT(zAt(S, pin.to[0][0], pin.to[0][1]), 0.);
}

TEST(NativeCornerPinMatrix, ZeroPointsApplyTheExtraMatrixAlone)
{
    CornerPinPoints pin = noPoints();
    pin.extraMatrix = Mat3(2., 0.5, 3., 0., 1.5, -4., 0.001, 0., 1.);
    // Points that would move the image if they were enabled.
    pin.from[0][0] = 7.;
    const Mat3 S = sampling(pin);
    for (int k = 0; k < 9; ++k) {
        EXPECT_EQ(pin.extraMatrix.m[k], S.m[k]);
    }
    Mat3 extraInverse;
    ASSERT_TRUE(pin.extraMatrix.inverse(&extraInverse));
    expectMatrixNear(extraInverse, sampling(pin, true), "0 points, inverted");
    EXPECT_FALSE(CornerPinOp::isIdentityPin(pin));
    EXPECT_TRUE(CornerPinOp::isIdentityPin(noPoints()));
}

TEST(NativeCornerPinMatrix, ExtraMatrixAppliesBeforeThePin)
{
    CornerPinPoints pin = noPoints();
    setPoint(&pin, 0, 10., 20., 15., 27.);
    pin.extraMatrix = Mat3(2., 0., 0., 0., 2., 0., 0., 0., 1.);
    expectMatrixNear(Mat3(2., 0., -5., 0., 2., -7., 0., 0., 1.), sampling(pin), "pin x extra");
}

TEST(NativeCornerPinMatrix, InvertIsTheExactInverse)
{
    CornerPinPoints pin;
    setPoint(&pin, 0, 0., 0., 4., 2.);
    setPoint(&pin, 1, 64., 0., 58., 6.);
    setPoint(&pin, 2, 64., 48., 62., 44.);
    setPoint(&pin, 3, 0., 48., 3., 40.);
    pin.extraMatrix = Mat3(1., 0.1, 2., 0., 0.9, -1., 0., 0., 1.);
    const Mat3 product = sampling(pin, true) * sampling(pin, false);
    expectMatrixNear(Mat3::identity(), product, "forward x sampling");
    // The forward matrix keeps the first "from" point in front.
    EXPECT_GT(zAt(sampling(pin, true), pin.from[0][0], pin.from[0][1]), 0.);
}

TEST(NativeCornerPinMatrix, AmountBlendsToFromFrom)
{
    CornerPinPoints pin = noPoints();
    setPoint(&pin, 0, 10., 20., 15., 27.);
    pin.amount = 0.;
    expectMatrixNear(Mat3::identity(), sampling(pin), "amount 0");
    EXPECT_TRUE(CornerPinOp::isIdentityPin(pin));

    pin.amount = 0.5;
    expectMatrixNear(Mat3(1., 0., -2.5, 0., 1., -3.5, 0., 0., 1.), sampling(pin), "amount 0.5");
    EXPECT_FALSE(CornerPinOp::isIdentityPin(pin));
    // The host's blend amount multiplies the knob's.
    expectMatrixNear(Mat3(1., 0., -1.25, 0., 1., -1.75, 0., 0., 1.), sampling(pin, false, 0.5), "amount 0.5 x 0.5");
}

TEST(NativeCornerPinMatrix, DegeneratePointsFallBack)
{
    // Four collinear points: no homography, no affine transform from any three; the similarity
    // from points 1 and 2 applies.
    CornerPinPoints collinear;
    setPoint(&collinear, 0, 0., 0., 0., 0.);
    setPoint(&collinear, 1, 10., 0., 0., 20.);
    setPoint(&collinear, 2, 20., 0., 0., 40.);
    setPoint(&collinear, 3, 30., 0., 0., 60.);
    expectMatrixNear(Mat3(0., 0.5, 0., -0.5, 0., 0., 0., 0., 1.), sampling(collinear), "4 collinear");

    // Point 4 aligned with points 1 and 2: the affine transform from points 1-3 applies.
    CornerPinPoints oneAligned;
    setPoint(&oneAligned, 0, 0., 0., 5., 5.);
    setPoint(&oneAligned, 1, 10., 0., 25., 5.);
    setPoint(&oneAligned, 2, 0., 10., 5., 15.);
    setPoint(&oneAligned, 3, 20., 0., 45., 5.);
    expectMatrixNear(Mat3(0.5, 0., -2.5, 0., 1., -5., 0., 0., 1.), sampling(oneAligned), "point 4 aligned");

    // Two coincident points: the translation of point 1.
    CornerPinPoints coincident = noPoints();
    setPoint(&coincident, 0, 3., 4., 5., 7.);
    setPoint(&coincident, 1, 3., 4., 9., 9.);
    expectMatrixNear(Mat3(1., 0., -2., 0., 1., -3., 0., 0., 1.), sampling(coincident), "2 coincident");
}

TEST(NativeCornerPinMatrix, FirstPointKeepsAPositiveZ)
{
    // Mirrored and rotated quads, whose solved matrices come out with either sign.
    const double quads[3][4][2] = {
        { { 64., 0. }, { 0., 0. }, { 0., 48. }, { 64., 48. } },
        { { 64., 48. }, { 0., 48. }, { 0., 0. }, { 64., 0. } },
        { { 10., 40. }, { 5., 3. }, { 60., 8. }, { 50., 45. } },
    };
    for (int q = 0; q < 3; ++q) {
        CornerPinPoints pin;
        setPoint(&pin, 0, 0., 0., quads[q][0][0], quads[q][0][1]);
        setPoint(&pin, 1, 64., 0., quads[q][1][0], quads[q][1][1]);
        setPoint(&pin, 2, 64., 48., quads[q][2][0], quads[q][2][1]);
        setPoint(&pin, 3, 0., 48., quads[q][3][0], quads[q][3][1]);
        EXPECT_GT(zAt(sampling(pin), pin.to[0][0], pin.to[0][1]), 0.) << "quad " << q;
        EXPECT_GT(zAt(sampling(pin, true), pin.from[0][0], pin.from[0][1]), 0.) << "quad " << q;
    }
}

class NativeCornerPinTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 64, 48, "nativeCornerPin", 1.));
    }

    NodePtr makeSource(int x = 0,
                       int y = 0)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    NodePtr makeNode(const char* id,
                     int major,
                     const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(id), major);

        EXPECT_TRUE(isNative(node)) << id;
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    void expectBothSchedulerModesAgree(const NodePtr& node)
    {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            std::vector<int> unplannedPulls;
            const RectI window = RectD(0., 0., 64., 48.).toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << node->getScriptName() << ", mipmap " << mipmapLevel << ": " << describe(m);
            for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
                EXPECT_EQ(0, unplannedPulls[p]) << node->getScriptName() << ", mipmap " << mipmapLevel << ", pool " << poolSizes[p];
            }
        }
    }

    // A CornerPin with no point enabled whose extra matrix is m: one resample through m.
    NodePtr makeSingleResample(const NodePtr& source,
                               const Mat3& m)
    {
        NodePtr single = makeNode(kCornerPinID, kCornerPinMajor, source);

        EXPECT_TRUE(disableAllPoints(single));
        EXPECT_TRUE(setExtraMatrix(single, m));

        return single;
    }
};

TEST_F(NativeCornerPinTest, KnobsUseTheSharedNames)
{
    resetProject();
    NodePtr node = makeNode(kCornerPinID, kCornerPinMajor, NodePtr());
    ASSERT_TRUE(bool(node));
    const char* const names[] = {
        kCornerPinNodeParamTo1,
        kCornerPinNodeParamTo2,
        kCornerPinNodeParamTo3,
        kCornerPinNodeParamTo4,
        kCornerPinNodeParamFrom1,
        kCornerPinNodeParamFrom2,
        kCornerPinNodeParamFrom3,
        kCornerPinNodeParamFrom4,
        kCornerPinNodeParamEnable1,
        kCornerPinNodeParamEnable2,
        kCornerPinNodeParamEnable3,
        kCornerPinNodeParamEnable4,
        kCornerPinNodeParamCopyFrom,
        kCornerPinNodeParamCopyFromSingle,
        kCornerPinNodeParamCopyTo,
        kCornerPinNodeParamCopyToSingle,
        kCornerPinNodeParamSetToInput,
        kCornerPinNodeParamOverlayPoints,
        kCornerPinNodeParamAmount,
        kCornerPinNodeParamExtraMatrix,
        kCornerPinNodeParamInvert,
        kCornerPinNodeParamInteractive,
        kCornerPinNodeParamFilter,
        kCornerPinNodeParamClamp,
        kCornerPinNodeParamBlackOutside,
        kCornerPinNodeParamMotionBlur,
        kCornerPinNodeParamShutter,
        kCornerPinNodeParamShutterOffset,
        kCornerPinNodeParamShutterCustomOffset,
        kCornerPinNodeParamSrcClipChanged,
        kCornerPinNodeParamToGroup,
        kCornerPinNodeParamFromGroup,
        kOfxMixParamName,
    };
    for (const char* name : names) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }
    KnobIPtr extra = node->getKnobByName(kCornerPinNodeParamExtraMatrix);
    ASSERT_TRUE(bool(extra));
    EXPECT_EQ(9, extra->getDimension());
}

TEST_F(NativeCornerPinTest, ConnectingASourceSetsThePointsOnce)
{
    resetProject();
    NodePtr source = makeSource(20, 10);
    NodePtr node = makeNode(kCornerPinID, kCornerPinMajor, source);
    ASSERT_TRUE(bool(node));
    KnobDouble* from3 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamFrom3).get());
    KnobDouble* to1 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamTo1).get());
    KnobBool* srcClipChanged = dynamic_cast<KnobBool*>(node->getKnobByName(kCornerPinNodeParamSrcClipChanged).get());
    ASSERT_TRUE(from3 && to1 && srcClipChanged);
    EXPECT_EQ(20. + 64., from3->getValue(0));
    EXPECT_EQ(10. + 48., from3->getValue(1));
    EXPECT_EQ(20., to1->getValue(0));
    EXPECT_EQ(10., to1->getValue(1));
    EXPECT_TRUE(srcClipChanged->getValue());

    setParitySourceOrigin(source, 0, 0);
    node->disconnectInput(0);
    connectNodes(source, node, 0, true);
    EXPECT_EQ(20. + 64., from3->getValue(0));

    // Set to Input moves "from" only.
    KnobButton* setToInput = dynamic_cast<KnobButton*>(node->getKnobByName(kCornerPinNodeParamSetToInput).get());
    ASSERT_TRUE(setToInput != NULL);
    setToInput->trigger();
    EXPECT_EQ(64., from3->getValue(0));
    EXPECT_EQ(48., from3->getValue(1));
    EXPECT_EQ(20., to1->getValue(0));
}

TEST_F(NativeCornerPinTest, CopyButtons)
{
    resetProject();
    NodePtr node = makeNode(kCornerPinID, kCornerPinMajor, makeSource());
    ASSERT_TRUE(bool(node));
    KnobDouble* from1 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamFrom1).get());
    KnobDouble* to1 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamTo1).get());
    ASSERT_TRUE(from1 && to1);
    from1->setValueAtTime(0., 1., ViewSpec::all(), 0);
    from1->setValueAtTime(2., 9., ViewSpec::all(), 0);

    dynamic_cast<KnobButton*>(node->getKnobByName(kCornerPinNodeParamCopyFrom).get())->trigger();
    EXPECT_EQ(2, to1->getKeyFramesCount(ViewSpec(0), 0));
    EXPECT_EQ(5., to1->getValueAtTime(1., 0));

    ASSERT_TRUE(setPoints(node, kCornerPinNodeParamTo2, 30., 31.));
    dynamic_cast<KnobButton*>(node->getKnobByName(kCornerPinNodeParamCopyToSingle).get())->trigger();
    KnobDouble* from2 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamFrom2).get());
    ASSERT_TRUE(from2 != NULL);
    EXPECT_EQ(30., from2->getValue(0));
    EXPECT_EQ(31., from2->getValue(1));
}

// A copy with animation is one undoable step: undo brings back the "to" curves, values and
// srcClipChanged as they were, redo copies again.
TEST_F(NativeCornerPinTest, CopyButtonUndoRestoresTheDestination)
{
    resetProject();
    NodePtr node = makeNode(kCornerPinID, kCornerPinMajor, makeSource());
    CornerPinNode* effect = cornerPin(node);
    ASSERT_TRUE(effect != NULL);
    KnobDouble* from1 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamFrom1).get());
    KnobDouble* to1 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamTo1).get());
    KnobDouble* to2 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamTo2).get());
    KnobDouble* from2 = dynamic_cast<KnobDouble*>(node->getKnobByName(kCornerPinNodeParamFrom2).get());
    KnobBool* srcClipChanged = dynamic_cast<KnobBool*>(node->getKnobByName(kCornerPinNodeParamSrcClipChanged).get());
    ASSERT_TRUE(from1 && to1 && to2 && from2 && srcClipChanged);
    from1->setValueAtTime(0., 1., ViewSpec::all(), 0);
    from1->setValueAtTime(2., 9., ViewSpec::all(), 0);
    to1->setValueAtTime(0., 3., ViewSpec::all(), 0);
    to1->setValueAtTime(2., 7., ViewSpec::all(), 0);
    to1->setValueAtTime(4., 11., ViewSpec::all(), 0);
    ASSERT_TRUE(setPoints(node, kCornerPinNodeParamTo2, 30., 31.));
    srcClipChanged->setValue(false);

    UndoCommandPtr command = effect->makeCopyPointsCommand(true, true, kTime, true);
    ASSERT_NE(nullptr, command.get());
    command->redo();
    EXPECT_EQ(2, to1->getKeyFramesCount(ViewSpec(0), 0));
    EXPECT_EQ(5., to1->getValueAtTime(1., 0));
    EXPECT_EQ(from2->getValue(0), to2->getValue(0));
    EXPECT_EQ(from2->getValue(1), to2->getValue(1));
    EXPECT_TRUE(srcClipChanged->getValue());

    command->undo();
    EXPECT_EQ(3, to1->getKeyFramesCount(ViewSpec(0), 0));
    EXPECT_EQ(3., to1->getValueAtTime(0., 0));
    EXPECT_EQ(7., to1->getValueAtTime(2., 0));
    EXPECT_EQ(11., to1->getValueAtTime(4., 0));
    EXPECT_EQ(30., to2->getValue(0));
    EXPECT_EQ(31., to2->getValue(1));
    EXPECT_FALSE(srcClipChanged->getValue());

    command->redo();
    EXPECT_EQ(2, to1->getKeyFramesCount(ViewSpec(0), 0));
    EXPECT_EQ(5., to1->getValueAtTime(1., 0));
}

TEST_F(NativeCornerPinTest, KnobsReachTheMatrix)
{
    resetProject();
    NodePtr node = makeNode(kCornerPinID, kCornerPinMajor, makeSource());
    CornerPinNode* effect = cornerPin(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamEnable2, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamEnable3, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamEnable4, { 0. }));
    ASSERT_TRUE(setPoints(node, kCornerPinNodeParamFrom1, 10., 20.));
    ASSERT_TRUE(setPoints(node, kCornerPinNodeParamTo1, 15., 27.));
    Mat3 m;
    ASSERT_TRUE(effect->getInverseMatrix(kTime, ViewIdx(0), &m));
    expectMatrixNear(Mat3(1., 0., -5., 0., 1., -7., 0., 0., 1.), m, "knobs, 1 point");

    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamInvert, { 1. }));
    ASSERT_TRUE(effect->getInverseMatrix(kTime, ViewIdx(0), &m));
    expectMatrixNear(Mat3(1., 0., 5., 0., 1., 7., 0., 0., 1.), m, "knobs, inverted");
}

TEST_F(NativeCornerPinTest, IdentityRules)
{
    resetProject();
    NodePtr node = makeNode(kCornerPinID, kCornerPinMajor, makeSource());
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    const RectI window(0, 0, 64, 48);
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;

    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    ASSERT_TRUE(setPoints(node, kCornerPinNodeParamTo2, 60., 3.));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamAmount, { 0. }));
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));

    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamMotionBlur, { 1. }));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
}

// A Transform after a CornerPin resamples the source once, through the product of both matrices.
TEST_F(NativeCornerPinTest, ConcatenatesIntoAFollowingTransform)
{
    resetProject();
    ASSERT_TRUE(appPTR->getCurrentSettings()->isTransformConcatenationEnabled());
    NodePtr source = makeSource();
    NodePtr pin = makeNode(kCornerPinID, kCornerPinMajor, source);
    NodePtr transform = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, pin);
    ASSERT_TRUE(pin && transform);
    ASSERT_TRUE(setPerspectivePin(pin));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamRotate, { 12. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamScale, { 0.8, 0.9 }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamTranslate, { 2.5, -1.5 }));

    EXPECT_TRUE(pin->getEffectInstance()->getCanTransform());
    InputMatrixMap transforms;
    transform->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    ASSERT_TRUE(bool(transforms[0].cat));
    EXPECT_EQ(pin->getEffectInstance(), transforms[0].newInputEffect);
    EXPECT_EQ(source->getEffectInstance(), transforms[0].newInputEffect->getInput(transforms[0].newInputNbToFetchFrom));

    TransformNode* transformEffect = dynamic_cast<TransformNode*>(transform->getEffectInstance().get());
    ASSERT_TRUE(transformEffect != NULL);
    TransformMath::TransformParams p;
    transformEffect->getTransformParams(kTime, ViewIdx(0), &p);
    Mat3 pinMatrix;
    ASSERT_TRUE(cornerPin(pin)->getInverseMatrix(kTime, ViewIdx(0), &pinMatrix));
    NodePtr single = makeSingleResample(source, pinMatrix * TransformMath::inverseTransformCanonical(p, 1., false));

    const RectI window(0, 0, 64, 48);
    RenderedPlane chained, combined;
    ASSERT_TRUE(renderRGBA(transform, window, &chained));
    ASSERT_TRUE(renderRGBA(single, window, &combined));
    expectPlanesNear(combined, chained, kResamplingTolerance, "CornerPin -> Transform against one resample");
}

// A CornerPin after a Transform applies the upstream matrix too.
TEST_F(NativeCornerPinTest, AppliesAPrecedingTransform)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr transform = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    NodePtr pin = makeNode(kCornerPinID, kCornerPinMajor, transform);
    ASSERT_TRUE(pin && transform);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamRotate, { -9. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamTranslate, { 3., 2. }));
    ASSERT_TRUE(setPerspectivePin(pin));

    InputMatrixMap transforms;
    pin->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    EXPECT_EQ(transform->getEffectInstance(), transforms[0].newInputEffect);

    TransformMath::TransformParams p;
    dynamic_cast<TransformNode*>(transform->getEffectInstance().get())->getTransformParams(kTime, ViewIdx(0), &p);
    Mat3 pinMatrix;
    ASSERT_TRUE(cornerPin(pin)->getInverseMatrix(kTime, ViewIdx(0), &pinMatrix));
    NodePtr single = makeSingleResample(source, TransformMath::inverseTransformCanonical(p, 1., false) * pinMatrix);

    const RectI window(0, 0, 64, 48);
    RenderedPlane chained, combined;
    ASSERT_TRUE(renderRGBA(pin, window, &chained));
    ASSERT_TRUE(renderRGBA(single, window, &combined));
    expectPlanesNear(combined, chained, kResamplingTolerance, "Transform -> CornerPin against one resample");
}

TEST_F(NativeCornerPinTest, MaskAndMixStopItOfferingItsMatrix)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr pin = makeNode(kCornerPinID, kCornerPinMajor, source);
    ASSERT_TRUE(bool(pin));
    ASSERT_TRUE(setPerspectivePin(pin));
    EffectInstancePtr effect = pin->getEffectInstance();
    EffectInstancePtr inputToTransform;
    Transform::Matrix3x3 matrix;
    EXPECT_TRUE(effect->getCanTransform());
    EXPECT_EQ(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));

    ASSERT_TRUE(setKnobValues(pin, kOfxMixParamName, { 0.5 }));
    EXPECT_NE(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
    ASSERT_TRUE(setKnobValues(pin, kOfxMixParamName, { 1. }));

    NodePtr mask = makeSource(10, 6);
    connectNodes(mask, pin, 1, true);
    ASSERT_TRUE(setKnobValues(pin, "enableMask_Mask", { 1. }));
    EXPECT_FALSE(effect->getCanTransform());
    EXPECT_NE(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
    std::list<int> inputs;
    EXPECT_TRUE(effect->getInputsHoldingTransform(&inputs));
    ASSERT_EQ(1u, inputs.size());
    EXPECT_EQ(0, inputs.front());

    ASSERT_TRUE(setKnobValues(pin, kOfxMixParamName, { 0.5 }));
    expectBothSchedulerModesAgree(pin);
}

// Without a mask, a mix below 1 blends in the source under the pixel, so the region of definition
// keeps the source's even when the pin moves the image off it.
TEST_F(NativeCornerPinTest, MixWithoutAMaskKeepsTheSourceRegion)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr pin = makeNode(kCornerPinID, kCornerPinMajor, source);
    ASSERT_TRUE(bool(pin));
    for (int i = 1; i < 4; ++i) {
        ASSERT_TRUE(setKnobValues(pin, kEnableNames[i], { 0. }));
    }
    ASSERT_TRUE(setPoints(pin, kCornerPinNodeParamFrom1, 0., 0.));
    ASSERT_TRUE(setPoints(pin, kCornerPinNodeParamTo1, 200., 0.));
    ASSERT_TRUE(setKnobValues(pin, kOfxMixParamName, { 0.5 }));

    bool isProjectFormat = false;
    EffectInstancePtr sourceEffect = source->getEffectInstance();
    RectD srcRoD;
    ASSERT_NE(eStatusFailed, sourceEffect->getRegionOfDefinition_public(sourceEffect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &srcRoD, &isProjectFormat));
    EffectInstancePtr effect = pin->getEffectInstance();
    RectD rod;
    ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    EXPECT_LE(rod.x1, srcRoD.x1);
    EXPECT_LE(rod.y1, srcRoD.y1);
    EXPECT_GE(rod.x2, srcRoD.x2);
    EXPECT_GE(rod.y2, srcRoD.y2);
    EXPECT_GE(rod.x2, 200. + srcRoD.x2);

    const RectI window(0, 0, 64, 48);
    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(pin, window, &plane));
    const int points[3][2] = { { 1, 1 }, { 20, 30 }, { 62, 46 } };
    for (const auto& p : points) {
        const float* pixel = &plane.pixels[((std::size_t)(p[1] - window.y1) * window.width() + (p[0] - window.x1)) * 4];
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(0.5 * paritySourceColorValue(c, p[0], p[1], kTime), pixel[c], 1e-6) << "pixel (" << p[0] << ", " << p[1] << "), channel " << c;
        }
    }
}

// A one-point pin whose "to" moves over the shutter blurs like a Transform with the same motion.
TEST_F(NativeCornerPinTest, MotionBlurOnAnAnimatedPin)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr pin = makeNode(kCornerPinID, kCornerPinMajor, source);
    NodePtr transform = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(pin && transform);
    for (int i = 1; i < 4; ++i) {
        ASSERT_TRUE(setKnobValues(pin, kEnableNames[i], { 0. }));
    }
    ASSERT_TRUE(setPoints(pin, kCornerPinNodeParamFrom1, 0., 0.));
    KnobDouble* to1 = dynamic_cast<KnobDouble*>(pin->getKnobByName(kCornerPinNodeParamTo1).get());
    KnobDouble* translate = dynamic_cast<KnobDouble*>(transform->getKnobByName(kTransformNodeParamTranslate).get());
    ASSERT_TRUE(to1 && translate);
    for (KnobDouble* knob : { to1, translate }) {
        knob->setValueAtTime(0., 0., ViewSpec::all(), 0);
        knob->setValueAtTime(0., 0., ViewSpec::all(), 1);
        knob->setValueAtTime(2., 8., ViewSpec::all(), 0);
        knob->setValueAtTime(2., 4., ViewSpec::all(), 1);
    }
    for (const NodePtr& node : { pin, transform }) {
        ASSERT_TRUE(setKnobValues(node, node == pin ? kCornerPinNodeParamMotionBlur : kTransformNodeParamMotionBlur, { 1. }));
        ASSERT_TRUE(setKnobValue(node, node == pin ? kCornerPinNodeParamShutterOffset : kResamplerParamShutterOffset, "centered"));
    }

    EffectInstancePtr effect = pin->getEffectInstance();
    EffectInstancePtr inputToTransform;
    Transform::Matrix3x3 matrix;
    EXPECT_NE(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
    EXPECT_EQ(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, true, ViewIdx(0), &inputToTransform, &matrix));

    const RectI window(0, 0, 64, 48);
    RenderedPlane blurredPin, blurredTransform;
    ASSERT_TRUE(renderRGBA(pin, window, &blurredPin));
    ASSERT_TRUE(renderRGBA(transform, window, &blurredTransform));
    expectPlanesNear(blurredTransform, blurredPin, kMotionBlurTolerance, "animated pin against Transform");
    expectBothSchedulerModesAgree(pin);
}

TEST_F(NativeCornerPinTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    NodePtr pin = makeNode(kCornerPinID, kCornerPinMajor, makeSource());
    ASSERT_TRUE(bool(pin));
    ASSERT_TRUE(setPerspectivePin(pin));
    expectBothSchedulerModesAgree(pin);
}

TEST_F(NativeCornerPinTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_CORNERPIN, PLUGIN_MAJOR_NATRON_CORNERPIN, "CornerPin");
}
