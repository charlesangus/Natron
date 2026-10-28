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

#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "CacheStress.h"
#include "HashSweepEdits.h"
#include "Native/RawFixtures.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Channel/AddLayers.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/Nodes/Raw/RawToImage.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"

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

// Two Transforms in a row, which render differently with and without concatenation.
bool
buildTransformPair(const AppInstancePtr& app,
                   StressGraph* g,
                   std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int source = paritySource(app, root, g, 0, 0);
    const int first = addNode(g, createIn(app, root, PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM));
    const int second = addNode(g, createIn(app, root, PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM));

    if ((source < 0) || (first < 0) || (second < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, source, first, 0) || !connect(*g, first, second, 0)) {
        return fail(error, "could not connect the graph");
    }
    addKnob(g, first, kTransformNodeParamTranslate, 0, { 0.5, 1.25, -0.75 });
    addKnob(g, first, kTransformNodeParamRotate, 0, { 7., -3. });
    addKnob(g, second, kTransformNodeParamTranslate, 1, { 0.5, -1.5 });
    addKnob(g, second, kTransformNodeParamScale, 0, { 0.8, 1.2 });
    g->toggles = { first, second };
    g->probe = second;
    g->window = kWindow;

    return true;
}

// A parity source into an AddLayers that adds the project layer "mask", then a Grade.
bool
buildAddLayers(const AppInstancePtr& app,
               StressGraph* g,
               std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    std::string why;
    if (app->getProject()->addLayer(ImageLayerDesc("mask", "mask", "", std::vector<std::string>(1, "A")), LayerRegistryEntry::eOriginUser, &why) == LayerRegistry::eAddResultRefused) {
        return fail(error, "could not register the mask layer: " + why);
    }
    const int source = paritySource(app, root, g, 0, 0);
    const int add = addNode(g, createIn(app, root, PLUGINID_NATRON_ADDLAYERS));
    const int grade = addNode(g, createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));

    if ((source < 0) || (add < 0) || (grade < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, source, add, 0) || !connect(*g, add, grade, 0)) {
        return fail(error, "could not connect the graph");
    }
    KnobChannelSetPtr layers = std::dynamic_pointer_cast<KnobChannelSet>(g->nodes[add]->getKnobByName(kAddLayersParamLayers));
    if (!layers) {
        return fail(error, "AddLayers has no layers knob");
    }
    layers->setLayer(0, "mask", NULL);
    addKnob(g, grade, kGradeParamWhite, 0, { 1., 0.5, 2. });
    g->toggles = { add, grade };
    g->probe = grade;
    g->window = kWindow;

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

// Version n of a flat RGBA EXR: each version has another size and value.
bool
writeExrVersion(const std::string& path,
                int version,
                std::string* error)
{
    const int width = 24 + 8 * version;
    const int height = 16 + 4 * version;
    OIIO::ImageSpec spec(width, height, 4, OIIO::TypeDesc::FLOAT);

    spec.channelnames = { "R", "G", "B", "A" };
    spec.alpha_channel = 3;
    std::vector<float> pixels((std::size_t)width * height * 4, 0.2f + 0.1f * (float)version);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = 1.f;
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    if (!out || !out->open(path, spec) || !out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) || !out->close()) {
        return fail(error, "could not write " + path);
    }

    return true;
}

// Even versions hold the Bayer fixture's bytes, odd ones the linear DNG's.
bool
writeRawVersion(const std::string& path,
                int version,
                std::string* error)
{
    const std::vector<char> bytes = readFixtureBytes(((version % 2) == 0 ? bayerFixture() : linearFixture()).path());

    if (bytes.empty() || !writeFixtureBytes(path, bytes.data(), bytes.size())) {
        return fail(error, "could not write " + path);
    }

    return true;
}

// Writes version 0 of a file and forgets what an earlier pass recorded about it, as a fresh
// session would.
bool
startFile(const std::string& path,
          const std::function<bool(const std::string&, int, std::string*)>& write,
          StressGraph* g,
          std::string* error)
{
    if (!write(path, 0, error)) {
        return false;
    }
    FileStampRegistry::instance().invalidate(path);
    OiioReadSupport::evictHeaders(std::vector<std::string>(1, path));
    RawDecodeSupport::evict(std::vector<std::string>(1, path));
    g->files.push_back([path, write](int version, std::string* err) {
        return write(path, version, err);
    });

    return true;
}

int
readOf(const AppInstancePtr& app,
       const NodeCollectionPtr& parent,
       StressGraph* g,
       const std::string& path)
{
    const NodePtr read = createIn(app, parent, PLUGINID_NATRON_READ, PLUGIN_MAJOR_NATRON_READ);
    KnobStringBase* file = read ? dynamic_cast<KnobStringBase*>(read->getKnobByName(kOfxImageEffectFileParamName).get()) : NULL;

    if (!file) {
        return -1;
    }
    file->setValue(path);

    return addNode(g, read);
}

// A Read of a flat EXR under a Grade, so the Grade's images are cached under the Read's hash.
bool
buildPinnedExr(const AppInstancePtr& app,
               const std::string& path,
               StressGraph* g,
               std::string* error)
{
    setFormat(app);
    if (!startFile(path, writeExrVersion, g, error)) {
        return false;
    }
    const NodeCollectionPtr root = app->getProject();
    const int read = readOf(app, root, g, path);
    const int grade = addNode(g, createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));

    if ((read < 0) || (grade < 0) || !connect(*g, read, grade, 0)) {
        return fail(error, "could not create the nodes");
    }
    g->probe = grade;
    g->window = kWindow;

    return true;
}

// A Read of a raw file under RawToImage, rendered over a corner of the Bayer fixture's active area.
bool
buildPinnedRaw(const AppInstancePtr& app,
               const std::string& path,
               StressGraph* g,
               std::string* error)
{
    setFormat(app);
    std::string headerError;
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(bayerFixture().path(), &headerError);
    if (!header) {
        return fail(error, headerError);
    }
    if (!startFile(path, writeRawVersion, g, error)) {
        return false;
    }
    const NodeCollectionPtr root = app->getProject();
    const int read = readOf(app, root, g, path);
    const int convert = addNode(g, createIn(app, root, PLUGINID_NATRON_RAWTOIMAGE));

    if ((read < 0) || (convert < 0) || !connect(*g, read, convert, 0)) {
        return fail(error, "could not create the nodes");
    }
    g->probe = convert;
    g->window = RectI(header->window.x1, header->window.y1, header->window.x1 + 48, header->window.y1 + 32);

    return true;
}

// Renders the probe again with nothing changed.
Step
renderAgain()
{
    Step step;

    step.kind = eOpTime;

    return step;
}

// Rewrite, render the stale file twice, Reload, and again with a second rewrite. The Read is node 0.
Options
pinnedUntilReloadScript()
{
    Options options;

    options.ops = opBit(eOpRewriteFile) | opBit(eOpReload);
    options.seeded = false;
    options.scripted.push_back(scriptedStep(eOpRewriteFile, rewriteFilePrim(0, 1)));
    options.scripted.push_back(renderAgain());
    options.scripted.push_back(scriptedStep(eOpReload, reloadPrim(0)));
    options.scripted.push_back(renderAgain());
    options.scripted.push_back(scriptedStep(eOpRewriteFile, rewriteFilePrim(0, 2)));
    options.scripted.push_back(renderAgain());
    options.scripted.push_back(scriptedStep(eOpReload, reloadPrim(0)));

    return options;
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

// An RGB render of a node leaves an image a later RGBA request of the same node must not serve,
// since its alpha is not the node's. Only Legacy renders the layers asked for, so the RGB render is
// a Legacy one; the RGBA one runs in both modes.
TEST_F(CacheStressTest, RgbThenRgbaRequest)
{
    const AppInstancePtr app = getApp();
    StressGraph g;
    std::string error;
    ASSERT_TRUE(buildChain(app, &g, &error)) << error;
    const NodePtr probe = g.nodes[g.probe];
    const std::list<ImageLayerDesc> rgb(1, ImageLayerDesc::getRGBComponents());

    clearEverything(app);
    RenderedPlane reference;
    ASSERT_TRUE(renderProbe(probe, 1., 0, g.window, eRenderSchedulerModeLegacy, true, &reference, &error)) << error;
    ASSERT_EQ(4u, reference.channels.size());

    const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
    for (RenderSchedulerModeEnum mode : modes) {
        clearEverything(app);
        std::vector<RenderedPlane> planes;
        ASSERT_TRUE(renderNodePlanesDirect(probe, 1., ViewIdx(0), 0, g.window, rgb, &planes, &error, false)) << error;
        RenderedPlane got;
        ASSERT_TRUE(renderProbe(probe, 1., 0, g.window, mode, false, &got, &error)) << error;
        EXPECT_EQ(std::string(), compareExact(reference, got)) << "mode " << (int)mode;
    }
}

// Expressions whose results fall outside the Blur size's range render the clamped size.
TEST_F(CacheStressTest, ExpressionOutsideRange)
{
    Options options = defaults();
    options.ops |= opBit(eOpFrameExpression);
    // Knob target 3 is the Blur's size, at least 0.
    options.scripted.push_back(scriptedStep(eOpSetExpression, expressionPrim(3, 0, "-3")));
    options.scripted.push_back(scriptedStep(eOpTime, seekPrim(2)));
    options.scripted.push_back(scriptedStep(eOpFrameExpression, expressionPrim(3, 0, "frame - 4")));
    options.scripted.push_back(scriptedStep(eOpTime, seekPrim(6)));
    options.scripted.push_back(scriptedStep(eOpTime, seekPrim(1)));
    EXPECT_EQ(0, expectClean(getApp(), buildChain, kSeeds, options));
}

// Transform concatenation is a render-affecting setting: toggling it re-keys every node through
// the project digest.
TEST_F(CacheStressTest, RenderSettingEdit)
{
    Options options = defaults();
    // Knob targets of buildTransformPair: first translate.x 0, first rotate 1.
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(0, 0, 0.5)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(1, 0, 7.)));
    options.scripted.push_back(scriptedStep(eOpSetValue, settingPrim("transformCatSupport", 0, 0.)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(0, 0, 1.25)));
    options.scripted.push_back(scriptedStep(eOpSetValue, settingPrim("transformCatSupport", 0, 1.)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(0, 0, 0.5)));
    options.scripted.push_back(scriptedStep(eOpSetValue, settingPrim("transformCatSupport", 0, 0.)));
    EXPECT_EQ(0, expectClean(getApp(), buildTransformPair, kSeeds, options));
    EXPECT_TRUE(appPTR->getCurrentSettings()->isTransformConcatenationEnabled());
}

// A knob that is neither persistent nor evaluate-on-change stays out of the node hash, so it must
// not change what a node renders: after each such edit the cached render equals an empty-cache one.
TEST_F(CacheStressTest, NonHashedKnobSweep)
{
    const std::vector<HashSweep::PluginVersion> plugins = HashSweep::sweptPlugins();
    ASSERT_FALSE(plugins.empty());
    setFormat(getApp());
    const NodeCollectionPtr root = getApp()->getProject();
    std::vector<std::string> failures;
    int edits = 0;

    for (std::size_t i = 0; i < plugins.size(); ++i) {
        const HashSweep::PluginVersion& plugin = plugins[i];
        if (plugin.isOfx) {
            continue;
        }
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        StressGraph g;
        const int source = paritySource(getApp(), root, &g, 0, 0);
        const int index = addNode(&g, createIn(getApp(), root, plugin.id, plugin.major));
        if ((source < 0) || (index < 0)) {
            continue;
        }
        const NodePtr node = g.nodes[index];
        if (!node->getEffectInstance() || (node->getEffectInstance()->getOutputDataKind() != eDataKindImage)) {
            continue;
        }
        if (node->getNInputs() > 0) {
            node->connectInput(g.nodes[source], 0);
        }
        std::vector<std::pair<KnobIPtr, int>> targets;
        const KnobsVec knobs = node->getKnobs();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            // Buttons are actions (render, track, reload), not state; pages, groups and
            // separators only lay out the panel. Toggling their bool value would press them.
            const KnobI* k = it->get();
            if (dynamic_cast<const KnobButton*>(k) || dynamic_cast<const KnobPage*>(k) || dynamic_cast<const KnobGroup*>(k) || dynamic_cast<const KnobSeparator*>(k)) {
                continue;
            }
            if (*it && !(*it)->isRenderHashed()) {
                for (int d = 0; d < (*it)->getDimension(); ++d) {
                    targets.push_back(std::make_pair(*it, d));
                }
            }
        }
        if (targets.empty()) {
            continue;
        }
        RenderedPlane plane;
        std::string error;
        if (!renderProbe(node, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &plane, &error)) {
            continue;
        }
        for (std::size_t t = 0; t < targets.size(); ++t) {
            const KnobIPtr& knob = targets[t].first;
            const int dim = targets[t].second;
            HashSweep::KnobEdit edit;
            bool unsupported = false;
            if (!HashSweep::staticValueSkipReason(knob, dim).empty() || !HashSweep::makeEdit(knob, dim, &edit, &unsupported).empty() || unsupported) {
                continue;
            }
            const std::string label = plugin.describe() + " " + HashSweep::knobLabel(knob, dim);
            if (!edit.apply()) {
                continue;
            }
            ++edits;
            RenderedPlane cached, reference;
            std::string cachedError, referenceError;
            const bool cachedOk = renderProbe(node, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &cached, &cachedError);
            clearEverything(getApp());
            const bool referenceOk = renderProbe(node, 1., 0, kWindow, eRenderSchedulerModeLegacy, true, &reference, &referenceError);
            if (referenceOk && !cachedOk) {
                failures.push_back(label + ": the cached render failed: " + cachedError);
            } else if (referenceOk) {
                const std::string diff = compareExact(reference, cached);
                if (!diff.empty()) {
                    failures.push_back(label + ": edit " + edit.what + ": " + diff);
                }
            }
            edit.revert();
            // Refills the caches under the reverted state for the next edit.
            (void)renderProbe(node, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &plane, &error);
        }
    }

    std::cout << "[non-hashed sweep] " << edits << " edits" << std::endl;
    std::string joined;
    for (std::size_t i = 0; i < failures.size(); ++i) {
        joined += "  " + failures[i] + "\n";
    }
    EXPECT_TRUE(failures.empty()) << failures.size() << " non-hashed knob(s) change the render; make each persistent or evaluate-on-change:\n"
                                  << joined;
}

// Growing a layer AddLayers names, and adding one it does not, change the registry the node and
// the project digest fold in.
TEST_F(CacheStressTest, AddLayersRegistryChange)
{
    Options options = defaults();
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(0, 0, 0.5)));
    options.scripted.push_back(scriptedStep(eOpSetValue, addLayerPrim("mask", 2)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(0, 0, 1.)));
    options.scripted.push_back(scriptedStep(eOpSetValue, addLayerPrim("other", 3)));
    options.scripted.push_back(scriptedStep(eOpSetValue, setValuePrim(0, 0, 0.5)));
    EXPECT_EQ(0, expectClean(getApp(), buildAddLayers, kSeeds, options));
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

// A Read keeps rendering the file as it was loaded, or fails, until Reload; it never mixes in the
// rewritten file, and after Reload it renders the new one.
TEST_F(CacheStressTest, ReadPinnedUntilReload)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("pinned.exr")).toStdString();
    const GraphBuilder builder = [path](const AppInstancePtr& app, StressGraph* g, std::string* error) {
        return buildPinnedExr(app, path, g, error);
    };
    EXPECT_EQ(0, expectClean(getApp(), builder, { 1u }, pinnedUntilReloadScript()));
}

TEST_F(CacheStressTest, RawReadPinnedUntilReload)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("pinned.cr2")).toStdString();
    const GraphBuilder builder = [path](const AppInstancePtr& app, StressGraph* g, std::string* error) {
        return buildPinnedRaw(app, path, g, error);
    };
    EXPECT_EQ(0, expectClean(getApp(), builder, { 1u }, pinnedUntilReloadScript()));
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
