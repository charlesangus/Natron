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
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

CLANG_DIAG_OFF(deprecated)
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
CLANG_DIAG_ON(deprecated)

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/LibraryBinary.h"
#include "Engine/MergingEnum.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoObjectSelect.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING

namespace {

// Tests/fixtures/flat-three-layers.exr: 8x8, every pixel Color (1, 0, 0, 1), diffuse (0, 1, 0),
// specular (0, 0, 1).
const int kSize = 8;

// The square drawn by addSquare(): control points (1, 6), (5, 6), (5, 2), (1, 2) in canonical
// coordinates, so it covers the pixel columns 1..4 and rows 2..5 exactly, with no feather.
const double kSquareX = 1.;
const double kSquareTop = 6.;
const double kSquareSize = 4.;
const int kInsideX = 2;
const int kInsideY = 3;
const int kOutsideX = 7;
const int kOutsideY = 7;

// EXR rows run top-down: the file's row 0 is Natron's y == height - 1.
float
at(const FlatExrImage& image,
   int x,
   int natronY,
   const std::string& channel)
{
    return image.at(x, image.height - 1 - natronY, channel);
}

void
expectChannelEverywhere(const FlatExrImage& image,
                        const std::string& channel,
                        float value)
{
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            EXPECT_EQ(value, at(image, x, y, channel)) << channel << " at (" << x << ", " << y << ")";
        }
    }
}

void
expectFixtureColor(const FlatExrImage& image)
{
    expectChannelEverywhere(image, "R", 1.f);
    expectChannelEverywhere(image, "G", 0.f);
    expectChannelEverywhere(image, "B", 0.f);
    expectChannelEverywhere(image, "A", 1.f);
}

void
expectFixtureSpecular(const FlatExrImage& image)
{
    expectChannelEverywhere(image, "specular.R", 0.f);
    expectChannelEverywhere(image, "specular.G", 0.f);
    expectChannelEverywhere(image, "specular.B", 1.f);
}

#define kTestPluginIDPaintHost "test.natron.built-in.PaintHost"

// A Paint host whose stroke tree merges over a node of its own, not over its input 0.
class PaintHostTestEffect
    : public NativeEffectBase {
public:
    static NodePtr& paintSource()
    {
        static NodePtr source;

        return source;
    }

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new PaintHostTestEffect(n);
    }

    explicit PaintHostTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL { return false; }

    virtual CurveContextKindEnum getCurveContextKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eCurveContextKindPaint;
    }

    virtual NodePtr getPaintTreeSource() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return paintSource();
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDPaintHost,
        .label = "Test Paint Host",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }
};

void
registerPaintHostTestEffectOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(PaintHostTestEffect::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&PaintHostTestEffect::BuildEffect));
        LibraryBinary* binary = new LibraryBinary(functions);

        std::list<std::string> grouping;
        node->getPluginGrouping(&grouping);
        QStringList qgrouping;
        for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
            qgrouping.push_back(QString::fromUtf8(it->c_str()));
        }

        Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                           QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
        p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    });
}

} // namespace

class RotoLayerTest
    : public BaseTest {
protected:
    NodePtr createRotoOnReader(const char* pluginID,
                               KnobLayerSelectPtr* layer)
    {
        NodePtr reader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
        if (!reader) {
            return NodePtr();
        }
        NodePtr roto = createNode(QString::fromUtf8(pluginID));
        if (!roto) {
            return NodePtr();
        }
        connectNodes(reader, roto, 0, true);
        *layer = std::dynamic_pointer_cast<KnobLayerSelect>(roto->getLayerKnob());

        return roto;
    }

    BezierPtr addSquare(const NodePtr& roto)
    {
        BezierPtr square = roto->getRotoContext()->makeSquare(kSquareX, kSquareTop, kSquareSize, 1.);
        if (square) {
            square->getFeatherKnob()->setValue(0.);
        }

        return square;
    }

    // Appends a native Write writing every layer of `node` as one 32-bit float part and renders
    // frame 1 into `tmp`, parsing the result into `image`.
    bool renderAllLayers(const NodePtr& node,
                         const QTemporaryDir& tmp,
                         FlatExrImage* image,
                         std::string* error)
    {
        NodePtr writer = createNativeWriter();
        if (!writer) {
            *error = "writer creation failed";

            return false;
        }
        connectNodes(node, writer, 0, true);

        KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(writer->getKnobByName("partSplitting").get());
        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
        KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
        KnobChannelSet* writerChannels = dynamic_cast<KnobChannelSet*>(writer->getKnobByName(kNodeParamChannelSet).get());
        if (!partSplitting || !bitDepth || !compression || !writerChannels) {
            *error = "writer knobs missing";

            return false;
        }
        partSplitting->setValueFromID("single", 0);
        bitDepth->setValueFromID("32f", 0);
        compression->setValueFromID("none", 0);
        writerChannels->setAll();

        const std::string path = (tmp.path() + QLatin1String("/out.exr")).toStdString();
        writer->setOutputFilesForWriter(path);
        QFile::remove(QString::fromStdString(path));

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        if (!writerEffect) {
            *error = "writer has no OutputEffectInstance";

            return false;
        }
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);

        if (!QFile::exists(QString::fromStdString(path))) {
            *error = "frame was not rendered: " + path;

            return false;
        }

        return readFlatExr(path, image, error);
    }
};

TEST_F(RotoLayerTest, RotoPaintWritesOneChannelOfDiffuseInsideTheShape)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTOPAINT, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(bool(addSquare(roto)));

    layer->setLayer("diffuse");
    std::vector<std::string> r(1, "R");
    layer->setChannels(r);

    // The tree renders the target plane in place: every node of it resolves "diffuse" against
    // the registry, whether or not its own inputs carry that plane.
    NodePtr bottomMerge = roto->getRotoContext()->getRotoPaintBottomMergeNode();
    ASSERT_TRUE(bool(bottomMerge));
    // The merge has no host layer knob: its Layers list is the whole layer, and A is read in its
    // namesake. RotoPaint copies the channels it does not write back from its input.
    EXPECT_FALSE(bool(bottomMerge->getLayerKnob()));
    KnobChannelSetPtr mergeLayers = std::dynamic_pointer_cast<KnobChannelSet>(bottomMerge->getKnobByName(kNativeMergeParamLayers));
    ASSERT_TRUE(bool(mergeLayers));
    const std::vector<ChannelSetRow> rows = mergeLayers->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string("diffuse"), rows[0].layerOrPattern);
    KnobLayerSelectPtr mergeA = std::dynamic_pointer_cast<KnobLayerSelect>(bottomMerge->getKnobByName(kNativeMergeParamALayer));
    ASSERT_TRUE(bool(mergeA));
    EXPECT_EQ(std::string(kNatronLayerSelectAll), mergeA->getLayer());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(roto, tmp, &image, &error)) << error;

    EXPECT_EQ(1.f, at(image, kInsideX, kInsideY, "diffuse.R"));
    EXPECT_EQ(0.f, at(image, kOutsideX, kOutsideY, "diffuse.R"));
    EXPECT_EQ(0.f, at(image, 0, 0, "diffuse.R"));
    expectChannelEverywhere(image, "diffuse.G", 1.f);
    expectChannelEverywhere(image, "diffuse.B", 0.f);
    expectFixtureColor(image);
    expectFixtureSpecular(image);

    project->reset(false, true);
}

TEST_F(RotoLayerTest, RotoPaintWritesATwoChannelRegistryLayerTheInputLacks)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> ab;
    ab.push_back("A");
    ab.push_back("B");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask2", "mask2", "", ab), LayerRegistryEntry::eOriginUser, &error)) << error;

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTOPAINT, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(bool(addSquare(roto)));

    layer->setLayer("mask2");

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    ASSERT_TRUE(renderAllLayers(roto, tmp, &image, &error)) << error;

    ASSERT_NE(-1, image.channelIndex("mask2.A"));
    ASSERT_NE(-1, image.channelIndex("mask2.B"));
    EXPECT_EQ(1.f, at(image, kInsideX, kInsideY, "mask2.A"));
    EXPECT_EQ(1.f, at(image, kInsideX, kInsideY, "mask2.B"));
    EXPECT_EQ(0.f, at(image, kOutsideX, kOutsideY, "mask2.A"));
    EXPECT_EQ(0.f, at(image, kOutsideX, kOutsideY, "mask2.B"));
    expectFixtureColor(image);
    expectChannelEverywhere(image, "diffuse.R", 0.f);
    expectChannelEverywhere(image, "diffuse.G", 1.f);
    expectChannelEverywhere(image, "diffuse.B", 0.f);
    expectFixtureSpecular(image);

    project->reset(false, true);
}

// The fixture is opaque everywhere, so "over" would leave alpha at 1 inside and out; the
// stencil operator (B(1-a)) punches the shape out of the alpha instead, which shows the
// shape reaching the alpha channel while the host copies R, G and B back from the input.
TEST_F(RotoLayerTest, RotoWritesAlphaOnlyIntoColor)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTO, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    EXPECT_EQ(std::string(kNatronColorViewRGBA), layer->getLayer());
    EXPECT_EQ(std::vector<std::string>(1, "A"), layer->getChannels());

    BezierPtr square = addSquare(roto);
    ASSERT_TRUE(bool(square));
    square->getOperatorKnob()->setValueFromID(Merge::getOperatorString(eMergeStencil), 0);
    roto->getRotoContext()->refreshRotoPaintTree();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(roto, tmp, &image, &error)) << error;

    EXPECT_EQ(0.f, at(image, kInsideX, kInsideY, "A"));
    EXPECT_EQ(1.f, at(image, kOutsideX, kOutsideY, "A"));
    EXPECT_EQ(1.f, at(image, 0, 0, "A"));
    expectChannelEverywhere(image, "R", 1.f);
    expectChannelEverywhere(image, "G", 0.f);
    expectChannelEverywhere(image, "B", 0.f);
    expectChannelEverywhere(image, "diffuse.R", 0.f);
    expectChannelEverywhere(image, "diffuse.G", 1.f);
    expectChannelEverywhere(image, "diffuse.B", 0.f);
    expectFixtureSpecular(image);

    project->reset(false, true);
}

// The RotoPaint container is the only knowing reference to "mask": its internal per-item
// Effect/Merge nodes and its global-merge node only follow the container via
// Node::retargetLayerKnob and must not each count as a separate user.
TEST_F(RotoLayerTest, LayerUsersCountOnlyTheRotoPaintContainer)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> a(1, "A");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", a), LayerRegistryEntry::eOriginUser, &error)) << error;

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTOPAINT, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(bool(addSquare(roto)));

    layer->setLayer("mask");

    std::list<NodePtr> users;
    project->getLayerUsers("mask", &users);
    ASSERT_EQ(1u, users.size());
    EXPECT_EQ(roto.get(), users.front().get());

    EXPECT_FALSE(project->removeLayer("mask", &error));
    const std::string rotoName = roto->getScriptName_mt_safe();
    ASSERT_NE(std::string::npos, error.find(rotoName)) << error;
    EXPECT_EQ(error.find(rotoName), error.rfind(rotoName)) << error;
    EXPECT_EQ(std::string::npos, error.find("_Effect")) << error;
    EXPECT_EQ(std::string::npos, error.find("_Merge")) << error;
    EXPECT_EQ(std::string::npos, error.find("globalMerge")) << error;

    project->reset(false, true);
}

// Roto (single-shape, non-paint) has the same internal Effect/Merge nodes as RotoPaint but no
// global-merge node; the container is still the sole user.
TEST_F(RotoLayerTest, LayerUsersCountOnlyTheRotoContainer)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> a(1, "A");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", a), LayerRegistryEntry::eOriginUser, &error)) << error;

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTO, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(bool(addSquare(roto)));

    layer->setLayer("mask");

    std::list<NodePtr> users;
    project->getLayerUsers("mask", &users);
    ASSERT_EQ(1u, users.size());
    EXPECT_EQ(roto.get(), users.front().get());

    EXPECT_FALSE(project->removeLayer("mask", &error));
    const std::string rotoName = roto->getScriptName_mt_safe();
    ASSERT_NE(std::string::npos, error.find(rotoName)) << error;
    EXPECT_EQ(error.find(rotoName), error.rfind(rotoName)) << error;
    EXPECT_EQ(std::string::npos, error.find("_Effect")) << error;
    EXPECT_EQ(std::string::npos, error.find("_Merge")) << error;

    project->reset(false, true);
}

TEST_F(RotoLayerTest, RotoPaintBezierGetsItsRenderNodes)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr paint = roto->getRotoContext();
    ASSERT_TRUE(bool(paint));
    EXPECT_EQ(paint.get(), roto->getCurveContext().get());

    BezierPtr square = addSquare(roto);
    ASSERT_TRUE(bool(square));
    NodePtr effectNode = square->getEffectNode();
    NodePtr mergeNode = square->getMergeNode();
    ASSERT_TRUE(bool(effectNode));
    ASSERT_TRUE(bool(mergeNode));

    NodesList tree;
    paint->getRotoPaintTreeNodes(&tree);
    EXPECT_NE(tree.end(), std::find(tree.begin(), tree.end(), effectNode));
    EXPECT_NE(tree.end(), std::find(tree.begin(), tree.end(), mergeNode));

    project->reset(false, true);
}

// Past one Merge's 64 A inputs the shapes continue in a second global Merge chained through its
// B input; the RotoPaint output reads the end of that chain, and adding a shape leaves the
// connections of the full Merge alone.
TEST_F(RotoLayerTest, RotoPaintOutputReachesEveryShapePastOneMerge)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr paint = roto->getRotoContext();
    ASSERT_TRUE(bool(paint));

    const int nShapes = kNativeMergeMaxAInputs + 6;
    std::vector<NodePtr> effects;
    for (int i = 0; i < nShapes; ++i) {
        BezierPtr square = addSquare(roto);
        ASSERT_TRUE(bool(square));
        ASSERT_TRUE(bool(square->getEffectNode()));
        effects.push_back(square->getEffectNode());
    }

    const auto reachedEffects = [&]() {
        std::vector<NodePtr> reached;
        NodePtr merge = paint->getRotoPaintBottomMergeNode();
        int merges = 0;
        while (merge && (merges < 8) && (merge->getPluginID() == PLUGINID_NATRON_NATIVE_MERGE)) {
            for (int i = 1; i < merge->getNInputs(); ++i) {
                NodePtr input = merge->getInput(i);
                if (input && (i != 2)) {
                    reached.push_back(input);
                }
            }
            merge = merge->getInput(0);
            ++merges;
        }

        return reached;
    };

    std::vector<NodePtr> reached = reachedEffects();
    EXPECT_EQ(effects.size(), reached.size());
    for (std::size_t i = 0; i < effects.size(); ++i) {
        EXPECT_NE(reached.end(), std::find(reached.begin(), reached.end(), effects[i])) << "shape " << i;
    }

    NodePtr bottom = paint->getRotoPaintBottomMergeNode();
    ASSERT_TRUE(bool(bottom));
    NodePtr full = bottom->getInput(0);
    ASSERT_TRUE(bool(full));
    int fullChanges = 0;
    QMetaObject::Connection counting = QObject::connect(full.get(), &Node::inputChanged, [&fullChanges](int) { ++fullChanges; });
    BezierPtr extra = addSquare(roto);
    QObject::disconnect(counting);
    ASSERT_TRUE(bool(extra));
    EXPECT_EQ(0, fullChanges);
    reached = reachedEffects();
    EXPECT_EQ(effects.size() + 1, reached.size());
    EXPECT_NE(reached.end(), std::find(reached.begin(), reached.end(), extra->getEffectNode()));

    project->reset(false, true);
}

namespace {

// Each tool group of the viewer toolbar page, followed by its actions, as "Group" and "Group/Action".
std::vector<std::string>
listToolbar(const NodePtr& node)
{
    std::vector<std::string> names;
    KnobPagePtr toolbar = std::dynamic_pointer_cast<KnobPage>(node->getKnobByName("Toolbar"));
    if (!toolbar) {
        return names;
    }
    std::vector<KnobIPtr> groups = toolbar->getChildren();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        names.push_back(groups[i]->getName());
        KnobGroupPtr group = std::dynamic_pointer_cast<KnobGroup>(groups[i]);
        if (!group) {
            continue;
        }
        std::vector<KnobIPtr> actions = group->getChildren();
        for (std::size_t j = 0; j < actions.size(); ++j) {
            names.push_back(groups[i]->getName() + "/" + actions[j]->getName());
        }
    }

    return names;
}

std::vector<std::string>
expectedCurveToolbar()
{
    const char* names[] = {
        "SelectionToolButton",
        "SelectionToolButton/SelectAllTool",
        "SelectionToolButton/SelectPointsTool",
        "SelectionToolButton/SelectShapesTool",
        "SelectionToolButton/SelectFeatherTool",
        "EditPointsToolButton",
        "EditPointsToolButton/AddPointsTool",
        "EditPointsToolButton/RemovePointsTool",
        "EditPointsToolButton/CuspPointsTool",
        "EditPointsToolButton/SmoothPointsTool",
        "EditPointsToolButton/OpenCloseShapeTool",
        "EditPointsToolButton/RemoveFeatherPointTool",
        "EditBezierToolButton",
        "EditBezierToolButton/DrawBezierTool",
        "EditBezierToolButton/DrawEllipseTool",
        "EditBezierToolButton/DrawRectangleTool",
        "ObjectSelectToolButton",
        "ObjectSelectToolButton/ObjectSelectTool",
        "PaintBrushToolButton",
        "PaintBrushToolButton/PaintSolidTool",
        "PaintBrushToolButton/PencilTool",
        "PaintBrushToolButton/EraserTool",
    };

    return std::vector<std::string>(names, names + sizeof(names) / sizeof(names[0]));
}

} // namespace

TEST_F(RotoLayerTest, RotoToolbarKnobsKeepTheirNamesAndOrder)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    EXPECT_EQ(expectedCurveToolbar(), listToolbar(roto));

    project->reset(false, true);
}

TEST_F(RotoLayerTest, RotoPaintToolbarKnobsKeepTheirNamesAndOrder)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(roto));

    std::vector<std::string> expected = expectedCurveToolbar();
    const char* paintNames[] = {
        "CloneToolButton",
        "CloneToolButton/CloneTool",
        "CloneToolButton/RevealTool",
        "EffectToolButton",
        "EffectToolButton/BlurTool",
        "EffectToolButton/SmearTool",
        "MergeToolButton",
        "MergeToolButton/DodgeTool",
        "MergeToolButton/BurnTool",
    };
    expected.insert(expected.end(), paintNames, paintNames + sizeof(paintNames) / sizeof(paintNames[0]));
    EXPECT_EQ(expected, listToolbar(roto));

    project->reset(false, true);
}

TEST_F(RotoLayerTest, ObjectSelectKnobsStayOutOfTheRenderHash)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    for (const char* pluginID : { PLUGINID_NATRON_ROTO, PLUGINID_NATRON_ROTOPAINT }) {
        NodePtr roto = createNode(QString::fromUtf8(pluginID));
        ASSERT_TRUE(bool(roto)) << pluginID;

        KnobChoicePtr resolution = std::dynamic_pointer_cast<KnobChoice>(roto->getKnobByName("objectInferenceResolution"));
        ASSERT_TRUE(bool(resolution)) << pluginID;
        EXPECT_EQ("Object Inference Resolution", resolution->getLabel());
        EXPECT_TRUE(resolution->isHashExempt());
        EXPECT_FALSE(resolution->getEvaluateOnChange());
        const std::vector<std::string> labels = { "Full", "50%", "25%", "1K", "2K", "4K" };
        const std::vector<ChoiceOption> options = resolution->getEntries_mt_safe();
        ASSERT_EQ(labels.size(), options.size());
        for (std::size_t i = 0; i < labels.size(); ++i) {
            EXPECT_EQ(labels[i], options[i].label);
        }
        EXPECT_EQ(4, resolution->getDefaultValue(0)) << "2K by default";

        const U64 hashBefore = roto->getHashValue();
        resolution->setValue(2);
        EXPECT_EQ(hashBefore, roto->getHashValue()) << pluginID;
        resolution->setValue(0);
        EXPECT_EQ(hashBefore, roto->getHashValue()) << pluginID;

        for (const char* name : { "objectThreshold", "objectDensity", "objectMinArea", "objectHoles", "objectSmooth", "objectFeather" }) {
            KnobIPtr knob = roto->getKnobByName(name);
            ASSERT_TRUE(bool(knob)) << name;
            EXPECT_TRUE(knob->isHashExempt()) << name;
        }
    }

    project->reset(false, true);
}

namespace {

RotoObjectSelect*
objectSelectOf(const NodePtr& node)
{
    RotoPaint* rotoPaint = node ? dynamic_cast<RotoPaint*>(node->getEffectInstance().get()) : NULL;
    RotoPaintInteractPtr interact = rotoPaint ? rotoPaint->getInteract() : RotoPaintInteractPtr();

    return interact ? interact->objectSelect.get() : NULL;
}

} // namespace

// Creating a Roto must not look at model files: until the tool is picked the menu names the models only.
TEST_F(RotoLayerTest, ObjectSelectModelMenuListsInstallStateOnlyOnceThePickerIsUsed)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    const ModelRegistry& registry = *ModelDownloader::process()->registry();
    const std::vector<std::string> ids = registry.byTask(AiModelTask::eMaskObject);
    ASSERT_FALSE(ids.empty());

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(roto));
    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(roto->getKnobByName("model"));
    ASSERT_TRUE(bool(model));
    RotoObjectSelect* objectSelect = objectSelectOf(roto);
    ASSERT_TRUE(objectSelect != NULL);

    std::vector<ChoiceOption> options = model->getEntries_mt_safe();
    ASSERT_EQ(ids.size(), options.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const AiModelEntry* entry = registry.find(ids[i]);
        ASSERT_TRUE(entry != NULL) << ids[i];
        EXPECT_EQ(ids[i], options[i].id);
        EXPECT_EQ(entry->name, options[i].label) << "no install state before the tool is picked";
    }

    objectSelect->setActive(true);
    options = model->getEntries_mt_safe();
    ASSERT_EQ(ids.size(), options.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const AiModelEntry* entry = registry.find(ids[i]);
        ASSERT_TRUE(entry != NULL) << ids[i];
        EXPECT_EQ(ids[i], options[i].id);
        EXPECT_EQ(0u, options[i].label.find(entry->name)) << options[i].label;
    }
    objectSelect->setActive(false);

    project->reset(false, true);
}

TEST_F(RotoLayerTest, ObjectSelectModelChoiceSurvivesSaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    const ModelRegistry& registry = *ModelDownloader::process()->registry();
    const std::vector<std::string> ids = registry.byTask(AiModelTask::eMaskObject);
    ASSERT_LE(2u, ids.size());

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(roto));
    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(roto->getKnobByName("model"));
    ASSERT_TRUE(bool(model));
    const std::string picked = (model->getActiveEntry().id == ids.back()) ? ids.front() : ids.back();
    model->setValueFromID(picked, 0);
    ASSERT_EQ(picked, model->getActiveEntry().id);
    const std::string rotoName = roto->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("object-select-model.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr restored = project->getNodeByName(rotoName);
    ASSERT_TRUE(bool(restored));
    KnobChoicePtr restoredModel = std::dynamic_pointer_cast<KnobChoice>(restored->getKnobByName("model"));
    ASSERT_TRUE(bool(restoredModel));
    EXPECT_EQ(picked, restoredModel->getActiveEntry().id);

    project->reset(false, true);
}

TEST_F(RotoLayerTest, ObjectSelectPutDownLeavesNoSelection)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(roto));
    RotoObjectSelect* objectSelect = objectSelectOf(roto);
    ASSERT_TRUE(objectSelect != NULL);

    objectSelect->setActive(true);
    EXPECT_TRUE(objectSelect->isActive());
    objectSelect->setActive(false);
    EXPECT_FALSE(objectSelect->isActive());
    EXPECT_FALSE(objectSelect->isEncoding());
    EXPECT_TRUE(objectSelect->prompts().empty());
    EXPECT_TRUE(objectSelect->previewContours().empty());
    RotoPaint* rotoPaint = dynamic_cast<RotoPaint*>(roto->getEffectInstance().get());
    ASSERT_TRUE(rotoPaint != NULL);
    EXPECT_FALSE(rotoPaint->isObjectSelectEncoding()) << "no encoding holds the node";

    project->reset(false, true);
}

namespace {

std::string
activeOperatorOf(const KnobIPtr& knob)
{
    KnobChoice* choice = dynamic_cast<KnobChoice*>(knob.get());

    return choice ? choice->getActiveEntry().id : std::string();
}

std::string
strokeOperator(const RotoStrokeItemPtr& stroke)
{
    return activeOperatorOf(stroke->getOperatorKnob());
}

std::string
strokeMergeOperator(const RotoStrokeItemPtr& stroke)
{
    return activeOperatorOf(stroke->getMergeNode()->getKnobByName(kNativeMergeParamOperation));
}

// These are the calls the action bar's knob notifications end in.
void
pickPaintTool(const RotoPaintInteractPtr& interact,
              const KnobButtonPtr& tool,
              const KnobGroupPtr& role)
{
    interact->setCurrentTool(tool);
    interact->onRoleChangedInternal(role);
    interact->onToolChangedInternal(tool);
}

RotoStrokeItemPtr
paintStrokeWith(const RotoPaintInteractPtr& interact,
                const KnobButtonPtr& tool,
                const KnobGroupPtr& role)
{
    pickPaintTool(interact, tool, role);
    interact->makeStroke(false, RotoPoint(10., 10., 1., 0.));

    return interact->strokeBeingPaint;
}

} // namespace

// A stroke begun while the previous one's neat render runs waits to reach the engine; the toolbar
// can change meanwhile, and the stroke still has to commit what its preview showed.
TEST_F(RotoLayerTest, QueuedStrokesCommitTheSettingsOfTheirPenDown)
{
    for (int multiStroke = 0; multiStroke < 2; ++multiStroke) {
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);

        NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
        ASSERT_TRUE(bool(roto));
        RotoPaint* rotoPaint = dynamic_cast<RotoPaint*>(roto->getEffectInstance().get());
        ASSERT_TRUE(rotoPaint != NULL);
        RotoPaintInteractPtr interact = rotoPaint->getInteract();
        ASSERT_TRUE(bool(interact));
        RotoContextPtr context = roto->getRotoContext();
        const double time = context->getTimelineCurrentTime();

        const KnobButtonPtr burn = interact->burnAction.lock();
        const KnobButtonPtr brush = interact->brushAction.lock();
        const KnobButtonPtr dodge = interact->dodgeAction.lock();
        const KnobGroupPtr paintRole = interact->paintBrushToolGroup.lock();
        const KnobGroupPtr mergeRole = interact->mergeBrushToolGroup.lock();
        const KnobDoublePtr size = interact->sizeSpinbox.lock();
        const KnobColorPtr color = interact->colorWheelButton.lock();
        const KnobBoolPtr multiStrokeKnob = interact->multiStrokeEnabled.lock();
        ASSERT_TRUE(burn && brush && dodge && paintRole && mergeRole && size && color && multiStrokeKnob);
        multiStrokeKnob->setValue(multiStroke != 0);

        double timestamp = 0.;
        const auto paint = [&](double x) {
            interact->onOverlayPenDown(time, QPointF(x, 10.), 1., ++timestamp, ePenTypeLMB);
            interact->onOverlayPenMotion(time, QPointF(x + 5., 12.), 1., ++timestamp);
            interact->onOverlayPenUp();
        };
        const auto paintedStrokes = [&]() {
            std::vector<RotoStrokeItemPtr> strokes;
            const std::list<RotoItemPtr> items = context->getOrCreateBaseLayer()->getItems_mt_safe();
            for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
                RotoStrokeItemPtr stroke = std::dynamic_pointer_cast<RotoStrokeItem>(*it);
                if (stroke && !stroke->isEmpty()) {
                    strokes.push_back(stroke);
                }
            }

            return strokes;
        };

        pickPaintTool(interact, burn, mergeRole);
        size->setValue(11.);
        paint(10.);
        ASSERT_EQ(1u, paintedStrokes().size()) << "the first stroke reaches the engine at once";

        size->setValue(22.);
        paint(20.);

        pickPaintTool(interact, brush, paintRole);
        size->setValue(33.);
        color->setValues(0.25, 0.5, 0.75, ViewSpec::all(), eValueChangedReasonNatronGuiEdited);
        paint(30.);
        paint(40.);
        ASSERT_EQ(1u, paintedStrokes().size()) << "the later strokes wait for the neat render";

        pickPaintTool(interact, dodge, mergeRole);
        size->setValue(44.);
        color->setValues(1., 0., 0., ViewSpec::all(), eValueChangedReasonNatronGuiEdited);

        for (int i = 0; i < 50; ++i) {
            QCoreApplication::processEvents();
        }

        const std::vector<RotoStrokeItemPtr> strokes = paintedStrokes();
        int burn11 = 0, burn22 = 0, solid33 = 0;
        for (const RotoStrokeItemPtr& stroke : strokes) {
            const double strokeSize = stroke->getBrushSizeKnob()->getValue();
            EXPECT_NE(44., strokeSize) << "multi-stroke " << multiStroke;
            if (strokeSize == 33.) {
                ++solid33;
                EXPECT_EQ(eRotoStrokeTypeSolid, stroke->getBrushType());
                EXPECT_EQ(Merge::getOperatorString(eMergeOver), strokeOperator(stroke));
                EXPECT_DOUBLE_EQ(0.25, stroke->getColorKnob()->getValue(0));
                EXPECT_DOUBLE_EQ(0.5, stroke->getColorKnob()->getValue(1));
                EXPECT_DOUBLE_EQ(0.75, stroke->getColorKnob()->getValue(2));
            } else {
                (strokeSize == 11. ? burn11 : burn22) += 1;
                EXPECT_EQ(eRotoStrokeTypeBurn, stroke->getBrushType());
                EXPECT_EQ(Merge::getOperatorString(eMergeColorBurn), strokeOperator(stroke));
            }
        }
        EXPECT_EQ(1, burn11) << "multi-stroke " << multiStroke;
        EXPECT_EQ(1, burn22) << "multi-stroke " << multiStroke;
        EXPECT_EQ(multiStroke ? 1 : 2, solid33) << "queued strokes with one configuration share a multi-stroke item";

        project->reset(false, true);
    }
}

TEST_F(RotoLayerTest, NewCloneAndRevealStrokesCompositeOverWithoutChangingOtherTools)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(roto));
    RotoPaint* rotoPaint = dynamic_cast<RotoPaint*>(roto->getEffectInstance().get());
    ASSERT_TRUE(rotoPaint != NULL);
    RotoPaintInteractPtr interact = rotoPaint->getInteract();
    ASSERT_TRUE(bool(interact));

    const KnobButtonPtr clone = interact->cloneAction.lock();
    const KnobButtonPtr reveal = interact->revealAction.lock();
    const KnobButtonPtr brush = interact->brushAction.lock();
    const KnobButtonPtr dodge = interact->dodgeAction.lock();
    const KnobGroupPtr cloneRole = interact->cloneBrushToolGroup.lock();
    const KnobGroupPtr paintRole = interact->paintBrushToolGroup.lock();
    const KnobGroupPtr mergeRole = interact->mergeBrushToolGroup.lock();
    const KnobChoicePtr toolbarOperator = interact->compositingOperatorChoice.lock();
    ASSERT_TRUE(clone && reveal && brush && dodge && cloneRole && paintRole && mergeRole && toolbarOperator);

    const std::string over = Merge::getOperatorString(eMergeOver);
    const std::string copy = Merge::getOperatorString(eMergeCopy);

    RotoStrokeItemPtr first = paintStrokeWith(interact, clone, cloneRole);
    ASSERT_TRUE(bool(first));
    EXPECT_EQ(eRotoStrokeTypeClone, first->getBrushType());
    EXPECT_EQ((int)eMergeOver, toolbarOperator->getValue()) << "first use";
    EXPECT_EQ(over, strokeOperator(first));
    EXPECT_EQ(over, strokeMergeOperator(first)) << "what the stroke actually renders with";

    RotoStrokeItemPtr brushStroke = paintStrokeWith(interact, brush, paintRole);
    EXPECT_EQ(over, strokeOperator(brushStroke));

    RotoStrokeItemPtr revealStroke = paintStrokeWith(interact, reveal, cloneRole);
    EXPECT_EQ(eRotoStrokeTypeReveal, revealStroke->getBrushType());
    EXPECT_EQ((int)eMergeOver, toolbarOperator->getValue()) << "Reveal first use";
    EXPECT_EQ(over, strokeOperator(revealStroke));
    EXPECT_EQ(over, strokeMergeOperator(revealStroke));

    RotoStrokeItemPtr dodgeStroke = paintStrokeWith(interact, dodge, mergeRole);
    EXPECT_EQ(Merge::getOperatorString(eMergeColorDodge), strokeOperator(dodgeStroke));

    RotoStrokeItemPtr afterSwitches = paintStrokeWith(interact, clone, cloneRole);
    EXPECT_NE(first, afterSwitches);
    EXPECT_EQ(over, strokeOperator(afterSwitches)) << "back from another tool";
    EXPECT_EQ(over, strokeMergeOperator(afterSwitches));

    // The tool only picks the default: the toolbar's choice still reaches the stroke.
    // Leaving the tool first: re-picking Clone would keep the empty stroke, already in the layer.
    pickPaintTool(interact, brush, paintRole);
    pickPaintTool(interact, clone, cloneRole);
    toolbarOperator->setValue((int)eMergeCopy);
    interact->makeStroke(false, RotoPoint(20., 20., 1., 0.));
    EXPECT_NE(afterSwitches, interact->strokeBeingPaint);
    EXPECT_EQ(copy, strokeOperator(interact->strokeBeingPaint));
    EXPECT_EQ(copy, strokeMergeOperator(interact->strokeBeingPaint));

    RotoStrokeItemPtr revealAfterSwitches = paintStrokeWith(interact, reveal, cloneRole);
    EXPECT_NE(revealStroke, revealAfterSwitches);
    EXPECT_EQ(eRotoStrokeTypeReveal, revealAfterSwitches->getBrushType());
    EXPECT_EQ(over, strokeOperator(revealAfterSwitches)) << "Reveal back from another tool";
    EXPECT_EQ(over, strokeMergeOperator(revealAfterSwitches));

    pickPaintTool(interact, brush, paintRole);
    pickPaintTool(interact, reveal, cloneRole);
    toolbarOperator->setValue((int)eMergeCopy);
    interact->makeStroke(false, RotoPoint(30., 30., 1., 0.));
    EXPECT_NE(revealAfterSwitches, interact->strokeBeingPaint);
    EXPECT_EQ(copy, strokeOperator(interact->strokeBeingPaint));
    EXPECT_EQ(copy, strokeMergeOperator(interact->strokeBeingPaint));

    // A stroke the toolbar never touches keeps the type's own default.
    RotoContextPtr context = roto->getRotoContext();
    RotoStrokeItemPtr unpainted(new RotoStrokeItem(eRotoStrokeTypeClone, context, "UnpaintedClone", RotoLayerPtr()));
    unpainted->createNodes(false);
    EXPECT_EQ(over, strokeOperator(unpainted));
    EXPECT_EQ(over, strokeMergeOperator(unpainted));

    RotoStrokeItemPtr unpaintedReveal(new RotoStrokeItem(eRotoStrokeTypeReveal, context, "UnpaintedReveal", RotoLayerPtr()));
    unpaintedReveal->createNodes(false);
    EXPECT_EQ(over, strokeOperator(unpaintedReveal));
    EXPECT_EQ(over, strokeMergeOperator(unpaintedReveal));

    project->reset(false, true);
}

TEST_F(RotoLayerTest, PaintHostStrokeTreeMergesOverTheHostSuppliedSource)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);
    registerPaintHostTestEffectOnce();

    NodePtr hostSource = createNode(QString::fromUtf8("Constant"));
    NodePtr inputZero = createNode(QString::fromUtf8("Constant"));
    ASSERT_TRUE(hostSource && inputZero);
    PaintHostTestEffect::paintSource() = hostSource;

    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDPaintHost));
    ASSERT_TRUE(bool(host));
    connectNodes(inputZero, host, 0, true);
    RotoContextPtr paint = host->getRotoContext();
    ASSERT_TRUE(bool(paint));

    BezierPtr square = addSquare(host);
    ASSERT_TRUE(bool(square));

    NodePtr merge = paint->getRotoPaintBottomMergeNode();
    ASSERT_TRUE(bool(merge));
    int merges = 0;
    while (merge && (merges < 8) && (merge->getPluginID() == PLUGINID_NATRON_NATIVE_MERGE)) {
        merge = merge->getInput(0);
        ++merges;
    }
    EXPECT_EQ(hostSource, merge);
    EXPECT_NE(inputZero, merge);

    NodePtr itemMerge = square->getMergeNode();
    ASSERT_TRUE(bool(itemMerge));
    EXPECT_NE(inputZero, itemMerge->getInput(0));

    PaintHostTestEffect::paintSource().reset();
    project->reset(false, true);
}
