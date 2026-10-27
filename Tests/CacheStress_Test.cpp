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
#include "CacheStress.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);
const std::vector<std::uint32_t> kSeeds = { 1u, 2u, 3u };
const int kSteps = 16;

bool
fail(std::string* error,
     const std::string& what)
{
    *error = what;

    return false;
}

bool
setFormat(const AppInstancePtr& app)
{
    app->getProject()->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "cacheStress", 1.));

    return true;
}

int
paritySource(const AppInstancePtr& app,
             const NodeCollectionPtr& parent,
             StressGraph* g,
             int x,
             int y)
{
    const NodePtr source = createIn(app, parent, kTestPluginIDParitySource);

    if (source) {
        setParitySourceOrigin(source, x, y);
    }

    return addNode(g, source);
}

int
checkerBoard(const AppInstancePtr& app,
             const NodeCollectionPtr& parent,
             StressGraph* g)
{
    const NodePtr checker = createIn(app, parent, PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);

    if (checker && !setKnobValues(checker, kCheckerBoardParamBoxSize, { 8., 8. })) {
        return -1;
    }

    return addNode(g, checker);
}

bool
selectChannel(const NodePtr& node,
              const std::string& name,
              const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

bool
connect(const StressGraph& g,
        int source,
        int node,
        int input)
{
    return g.nodes[node]->connectInput(g.nodes[source], input);
}

Step
scriptedStep(CacheStress::OpKind kind,
             const Primitive& prim)
{
    Step step;

    step.kind = kind;
    step.prims.push_back(prim);

    return step;
}

Primitive
seekPrim(double time)
{
    Primitive p;

    p.kind = ePrimSeek;
    p.time = time;

    return p;
}

// (1) STMap with every input, the Map slot listed twice so the walk toggles it twice as often.
bool
buildSTMap(const AppInstancePtr& app,
           StressGraph* g,
           std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int uv = paritySource(app, root, g, 0, 0);
    const int uvFlat = addNode(g, createConstantIn(app, root, kWindow, { 0.25, 0.75, 0., 1. }));
    const int checker = checkerBoard(app, root, g);
    const int flat = addNode(g, createConstantIn(app, root, kWindow, { 0.6, 0.2, 0.1, 1. }));
    const int mask = addNode(g, createConstantIn(app, root, kWindow, { 0., 0., 0., 0.5 }));
    const int map = paritySource(app, root, g, 8, 4);
    const int stmap = addNode(g, createIn(app, root, PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP));

    if ((uv < 0) || (uvFlat < 0) || (checker < 0) || (flat < 0) || (mask < 0) || (map < 0) || (stmap < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, uv, stmap, kSTMapInputUV) || !connect(*g, checker, stmap, kSTMapInputSource) || !connect(*g, map, stmap, kSTMapInputMap)) {
        return fail(error, "could not connect the STMap");
    }
    if (!selectChannel(g->nodes[stmap], kSTMapParamChannelU, "rgba.R") || !selectChannel(g->nodes[stmap], kSTMapParamChannelV, "rgba.G")) {
        return fail(error, "could not pick the UV channels");
    }
    addSlot(g, stmap, kSTMapInputUV, { uv, uvFlat }, false);
    addSlot(g, stmap, kSTMapInputSource, { checker, flat }, false);
    addSlot(g, stmap, kSTMapInputMask, { mask, map });
    addSlot(g, stmap, kSTMapInputMap, { map, mask, flat });
    addSlot(g, stmap, kSTMapInputMap, { map, mask, flat });
    const int scale = addKnob(g, stmap, kSTMapParamUVScale, 0, { 1., 0.5, 1.5 });
    const int offset = addKnob(g, stmap, kSTMapParamUVOffset, 1, { 0., 0.1, -0.2 });
    addKnob(g, stmap, kMapInputParamGamma, 0, { 1., 2., 0.5 }, false);
    addKnob(g, checker, kCheckerBoardParamBoxSize, 0, { 8., 4., 16. }, false);
    addKnob(g, flat, kConstantParamColor, 0, { 0.6, 0.9 });
    g->exprTargets = { scale, offset };
    g->links.push_back(std::make_pair(offset, scale));
    g->toggles = { stmap, checker };
    g->probe = stmap;
    g->window = kWindow;

    return true;
}

// (2) IDistort into IBlur, both with a Map.
bool
buildIDistortIBlur(const AppInstancePtr& app,
                   StressGraph* g,
                   std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int source = paritySource(app, root, g, 0, 0);
    const int checker = checkerBoard(app, root, g);
    const int uv = paritySource(app, root, g, 4, 4);
    const int flat = addNode(g, createConstantIn(app, root, kWindow, { 0.1, -0.1, 0., 1. }));
    const int map = paritySource(app, root, g, 12, 0);
    const int idistort = addNode(g, createIn(app, root, PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT));
    const int iblur = addNode(g, createIn(app, root, PLUGINID_NATRON_IBLUR, PLUGIN_MAJOR_NATRON_IBLUR));

    if ((source < 0) || (checker < 0) || (uv < 0) || (flat < 0) || (map < 0) || (idistort < 0) || (iblur < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, source, idistort, kIDistortInputSource) || !connect(*g, uv, idistort, kIDistortInputUV) || !connect(*g, map, idistort, kIDistortInputMap) || !connect(*g, idistort, iblur, kIBlurInputSource) || !connect(*g, map, iblur, kIBlurInputMap)) {
        return fail(error, "could not connect the chain");
    }
    if (!selectChannel(g->nodes[idistort], kIDistortParamChannelU, "rgba.R") || !selectChannel(g->nodes[idistort], kIDistortParamChannelV, "rgba.G")) {
        return fail(error, "could not pick the UV channels");
    }
    addSlot(g, idistort, kIDistortInputSource, { source, checker }, false);
    addSlot(g, idistort, kIDistortInputUV, { uv, flat });
    addSlot(g, idistort, kIDistortInputMap, { map, flat });
    addSlot(g, iblur, kIBlurInputMap, { map, uv });
    addSlot(g, iblur, kIBlurInputMask, { flat, map });
    const int uvScale = addKnob(g, idistort, kIDistortParamUVScale, 0, { 2., 1., 4. });
    const int size = addKnob(g, iblur, kIBlurParamSize, 0, { 2., 4., 0. });
    addKnob(g, iblur, kMapInputParamGamma, 0, { 1., 0.5 }, false);
    addKnob(g, idistort, kMapInputParamGamma, 0, { 1., 2. }, false);
    addKnob(g, flat, kConstantParamColor, 0, { 0.1, 0.3 });
    g->exprTargets = { uvScale, size };
    g->toggles = { idistort, iblur };
    g->probe = iblur;
    g->window = kWindow;

    return true;
}

// (3) A Switch and a Dissolve into a Merge, each with inputs that grow and shrink.
bool
buildMergeSwitchDissolve(const AppInstancePtr& app,
                         StressGraph* g,
                         std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int c0 = addNode(g, createConstantIn(app, root, kWindow, { 0.8, 0.2, 0.1, 1. }));
    const int c1 = addNode(g, createConstantIn(app, root, RectI(8, 8, 40, 32), { 0.1, 0.7, 0.3, 0.5 }));
    const int c2 = addNode(g, createConstantIn(app, root, RectI(16, 0, 64, 24), { 0.2, 0.2, 0.9, 1. }));
    const int ramp = paritySource(app, root, g, 0, 0);
    const int sw = addNode(g, createIn(app, root, PLUGINID_NATRON_SWITCH, PLUGIN_MAJOR_NATRON_SWITCH));
    const int dissolve = addNode(g, createIn(app, root, PLUGINID_NATRON_DISSOLVE, PLUGIN_MAJOR_NATRON_DISSOLVE));
    const int merge = addNode(g, createIn(app, root, PLUGINID_NATRON_NATIVE_MERGE, PLUGIN_MAJOR_NATRON_NATIVE_MERGE));

    if ((c0 < 0) || (c1 < 0) || (c2 < 0) || (ramp < 0) || (sw < 0) || (dissolve < 0) || (merge < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, c0, sw, 0) || !connect(*g, c1, dissolve, Dissolve::sourceInput(0)) || !connect(*g, ramp, dissolve, Dissolve::sourceInput(1)) || !connect(*g, sw, merge, kNativeMergeInputB) || !connect(*g, dissolve, merge, NativeMerge::aInput(0))) {
        return fail(error, "could not connect the graph");
    }
    for (int k = 0; k < 3; ++k) {
        addSlot(g, sw, k, { c0, c1, c2, ramp });
    }
    for (int k = 0; k < 3; ++k) {
        addSlot(g, dissolve, Dissolve::sourceInput(k), { c1, c2, ramp, c0 });
    }
    addSlot(g, dissolve, kDissolveMaskInput, { c1, c2 });
    addSlot(g, merge, kNativeMergeInputB, { sw, dissolve, c2 });
    for (int k = 0; k < 3; ++k) {
        addSlot(g, merge, NativeMerge::aInput(k), { dissolve, c1, c2, ramp });
    }
    addSlot(g, merge, kNativeMergeInputMask, { c1, ramp });
    addKnob(g, sw, kSwitchParamWhich, 0, { 0., 1., 2. });
    const int which = addKnob(g, dissolve, kDissolveParamWhich, 0, { 0., 0.5, 1.5, 2. });
    addKnob(g, c1, kConstantParamColor, 3, { 0.5, 1., 0. }, false);
    addKnob(g, c2, kConstantParamColor, 0, { 0.2, 0.6 });
    g->exprTargets = { which };
    g->swappable = { merge, dissolve };
    g->toggles = { sw, dissolve, merge };
    g->probe = merge;
    g->window = kWindow;

    return true;
}

// The Grade -> Blur -> Transform chain inside collection, fed by source, from *first to *last.
bool
buildChainIn(const AppInstancePtr& app,
             const NodeCollectionPtr& collection,
             StressGraph* g,
             int source,
             int* first,
             int* last,
             std::string* error)
{
    const int grade = addNode(g, createIn(app, collection, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));
    const int blur = addNode(g, createIn(app, collection, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR));
    const int transform = addNode(g, createIn(app, collection, PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM));

    if ((grade < 0) || (blur < 0) || (transform < 0)) {
        return fail(error, "could not create the chain");
    }
    if (!connect(*g, source, grade, 0) || !connect(*g, grade, blur, 0) || !connect(*g, blur, transform, 0)) {
        return fail(error, "could not connect the chain");
    }
    const int white = addKnob(g, grade, kGradeParamWhite, 0, { 1., 0.5, 2. });
    const int multiply = addKnob(g, grade, kGradeParamMultiply, 1, { 1., 0.8, 1.25 });
    addKnob(g, grade, kGradeParamGamma, 2, { 1., 0.8 }, false);
    addKnob(g, blur, kBlurParamSize, 0, { 0., 1., 3. }, false);
    const int translate = addKnob(g, transform, kTransformNodeParamTranslate, 0, { 0., 3.5, -2. });
    const int rotate = addKnob(g, transform, kTransformNodeParamRotate, 0, { 0., 10., -5. }, false);
    const int scale = addKnob(g, transform, kTransformNodeParamScale, 1, { 1., 0.75 });
    g->exprTargets.insert(g->exprTargets.end(), { white, multiply, translate, scale });
    g->wholeValueSources.push_back(rotate);
    g->links.push_back(std::make_pair(white, multiply));
    g->links.push_back(std::make_pair(scale, translate));
    g->toggles.insert(g->toggles.end(), { grade, blur, transform });
    *first = grade;
    *last = transform;

    return true;
}

// (4) Grade -> Blur -> Transform with expressions and links across the nodes.
bool
buildChain(const AppInstancePtr& app,
           StressGraph* g,
           std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int source = paritySource(app, root, g, 0, 0);
    const int other = addNode(g, createConstantIn(app, root, kWindow, { 0.3, 0.6, 0.9, 1. }));

    if ((source < 0) || (other < 0)) {
        return fail(error, "could not create the sources");
    }
    int first = -1;
    int last = -1;
    if (!buildChainIn(app, root, g, source, &first, &last, error)) {
        return false;
    }
    addSlot(g, first, 0, { source, other }, false);
    g->probe = last;
    g->window = kWindow;

    return true;
}

// (5) The same chain inside a Group, fed through GroupInput and read through GroupOutput by a Dot
// outside, since a Group renders nothing of its own.
bool
buildGroupedChain(const AppInstancePtr& app,
                  StressGraph* g,
                  std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int source = paritySource(app, root, g, 0, 0);
    const int other = addNode(g, createConstantIn(app, root, kWindow, { 0.3, 0.6, 0.9, 1. }));
    const int group = addNode(g, createEmptyGroupIn(app, root));
    const int dot = addNode(g, createIn(app, root, PLUGINID_NATRON_DOT));

    if ((source < 0) || (other < 0) || (group < 0) || (dot < 0)) {
        return fail(error, "could not create the nodes");
    }
    const NodeCollectionPtr inside = std::dynamic_pointer_cast<NodeCollection>(g->nodes[group]->getEffectInstance());
    if (!inside) {
        return fail(error, "the group has no collection");
    }
    const int input = addNode(g, createIn(app, inside, PLUGINID_NATRON_INPUT));
    const int output = addNode(g, createIn(app, inside, PLUGINID_NATRON_OUTPUT));
    if ((input < 0) || (output < 0)) {
        return fail(error, "could not create GroupInput and GroupOutput");
    }
    int first = -1;
    int last = -1;
    if (!buildChainIn(app, inside, g, input, &first, &last, error)) {
        return false;
    }
    if (!connect(*g, last, output, 0) || !connect(*g, source, group, 0) || !connect(*g, group, dot, 0)) {
        return fail(error, "could not connect the group");
    }
    addSlot(g, group, 0, { source, other }, false);
    g->toggles.push_back(group);
    g->probe = dot;
    g->window = kWindow;

    return true;
}

// (6) A viewer showing X on A and, on B, one of X, Y and Z.
bool
buildViewer(const AppInstancePtr& app,
            StressGraph* g,
            std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int ramp = paritySource(app, root, g, 0, 0);
    const int flat = addNode(g, createConstantIn(app, root, kWindow, { 0.4, 0.5, 0.6, 1. }));
    const int x = addNode(g, createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));
    const int y = addNode(g, createIn(app, root, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR));
    const int z = addNode(g, createConstantIn(app, root, kWindow, { 0.9, 0.1, 0.1, 1. }));
    const int viewer = addNode(g, createViewerIn(app, root));

    if ((ramp < 0) || (flat < 0) || (x < 0) || (y < 0) || (z < 0) || (viewer < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, ramp, x, 0) || !connect(*g, ramp, y, 0) || !connect(*g, x, viewer, 0) || !connect(*g, y, viewer, 1) || !connect(*g, z, viewer, 2)) {
        return fail(error, "could not connect the viewer");
    }
    InspectorNode* inspector = dynamic_cast<InspectorNode*>(g->nodes[viewer].get());
    if (!inspector) {
        return fail(error, "the viewer is not an InspectorNode");
    }
    inspector->setInputA(0);
    inspector->setInputB(0);
    addSlot(g, x, 0, { ramp, flat }, false);
    addSlot(g, y, 0, { ramp, flat }, false);
    addKnob(g, x, kGradeParamWhite, 0, { 1., 2., 0.5 });
    addKnob(g, y, kBlurParamSize, 0, { 0., 3., 1. }, false);
    addKnob(g, z, kConstantParamColor, 0, { 0.9, 0.2 });
    g->toggles = { x, y };
    g->viewer = viewer;
    g->viewerInputs = { 0, 1, 2 };
    g->window = kWindow;

    return true;
}

// The viewer graph's knob targets, in the order buildViewer adds them.
const int kViewerXWhite = 0;
const int kViewerYSize = 1;

// ErodeDilate with a size of one pixel: an identity at mipmap 1 and not at mipmap 0.
bool
buildErode(const AppInstancePtr& app,
           StressGraph* g,
           std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int source = paritySource(app, root, g, 0, 0);
    const int erode = addNode(g, createIn(app, root, PLUGINID_NATRON_ERODE, PLUGIN_MAJOR_NATRON_ERODE));

    if ((source < 0) || (erode < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, source, erode, 0) || !setKnobValues(g->nodes[erode], kErodeDilateParamSize, { 1., 1. })) {
        return fail(error, "could not set up the erode");
    }
    g->probe = erode;
    g->window = kWindow;

    return true;
}

Options
defaults()
{
    Options options;

    options.ops = defaultOps();
    options.steps = kSteps;

    return options;
}

} // namespace

class CacheStressTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }
};

TEST_F(CacheStressTest, STMapWithAToggledMap)
{
    EXPECT_EQ(0, expectClean(getApp(), buildSTMap, kSeeds, defaults()));
}

TEST_F(CacheStressTest, IDistortAndIBlurWithAMap)
{
    EXPECT_EQ(0, expectClean(getApp(), buildIDistortIBlur, kSeeds, defaults()));
}

TEST_F(CacheStressTest, MergeSwitchAndDissolveWithGrowingInputs)
{
    EXPECT_EQ(0, expectClean(getApp(), buildMergeSwitchDissolve, kSeeds, defaults()));
}

TEST_F(CacheStressTest, GradeBlurTransformWithExpressionsAndLinks)
{
    EXPECT_EQ(0, expectClean(getApp(), buildChain, kSeeds, defaults()));
}

TEST_F(CacheStressTest, GradeBlurTransformInsideAGroup)
{
    EXPECT_EQ(0, expectClean(getApp(), buildGroupedChain, kSeeds, defaults()));
}

// Edits behind a viewer whose B side stays on X; switching B is the disabled case below.
TEST_F(CacheStressTest, ViewerABProbe)
{
    EXPECT_EQ(0, expectClean(getApp(), buildViewer, kSeeds, defaults()));
}

// An expression on frame, on a knob with no keyframes, keys the node's images on time.
TEST_F(CacheStressTest, FrameExpression)
{
    Options options = defaults();
    options.ops |= opBit(eOpFrameExpression);
    // Knob target 0 is the Grade's white.
    options.scripted.push_back(scriptedStep(eOpFrameExpression, expressionPrim(0, 0, "frame*0.1")));
    options.scripted.push_back(scriptedStep(eOpTime, seekPrim(2)));
    options.scripted.push_back(scriptedStep(eOpTime, seekPrim(4)));
    EXPECT_EQ(0, expectClean(getApp(), buildChain, kSeeds, options));
}

// A whole-value expression (getValue() with no dimension) records a dimension -1 dependency that
// adds nothing to the reader's hash, so editing the source leaves the reader's image in place.
// Fixed by M72 - Expression Dependencies (E1); enable it there.
TEST_F(CacheStressTest, DISABLED_WholeValueExpression_M72)
{
    // Knob targets of buildChain: white 0, multiply 1, gamma 2, size 3, translate 4, rotate 5.
    Options options = defaults();
    options.ops |= opBit(eOpWholeValueExpression);
    options.scripted.push_back(scriptedStep(eOpWholeValueExpression, dimensionExpressionPrim(0, 0, 5, -1)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(5, 0, 2.)));
    EXPECT_EQ(0, expectClean(getApp(), buildChain, kSeeds, options));
}

// An A/B switch moves the viewer hash, so after an edit and a revert upstream the key of an
// earlier step cannot answer with that step's image.
TEST_F(CacheStressTest, ViewerABSwitch)
{
    Options options = defaults();
    options.ops |= opBit(eOpViewerB);
    options.scripted.push_back(scriptedStep(eOpViewerB, viewerBPrim(1)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(kViewerYSize, 0, 3.)));
    options.scripted.push_back(scriptedStep(eOpViewerB, viewerBPrim(0)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(kViewerXWhite, 0, 2.)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(kViewerXWhite, 0, 1.)));
    options.scripted.push_back(scriptedStep(eOpViewerB, viewerBPrim(1)));
    EXPECT_EQ(0, expectClean(getApp(), buildViewer, kSeeds, options));
}

// A one-pixel erode is an identity at mipmap 1 and not at mipmap 0, so the identity answer of one
// level must not answer for the other.
TEST_F(CacheStressTest, IdentityAcrossScales)
{
    Options options;
    options.ops = opBit(eOpMipmap);
    options.seeded = false;
    options.scripted.push_back(scriptedStep(eOpMipmap, mipmapPrim(1)));
    options.scripted.push_back(scriptedStep(eOpMipmap, mipmapPrim(0)));
    EXPECT_EQ(0, expectClean(getApp(), buildErode, { 1u }, options));
}
