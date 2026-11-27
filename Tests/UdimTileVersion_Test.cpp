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

#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Metadata/EffectMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#define kUdimPaintTestPluginID "test.natron.built-in.UdimTileVersionPaint"

NATRON_NAMESPACE_ENTER

namespace {
constexpr InputSpec kUdimPaintInputs[] = {
    InputSpec { .label = "A", .optional = true },
    InputSpec { .label = "B", .optional = true },
};

/**
 * @brief A painter keeping a stroke count per UDIM tile: an edit of one tile moves only that
 * tile's version, folded with its inputs' versions of the same tile. It declares its tiles in its
 * output metadata.
 **/
class UdimPaintTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new UdimPaintTestEffect(n);
    }

    explicit UdimPaintTestEffect(NodePtr n)
        : NativeEffectBase(n)
        , _mutex()
        , _strokes()
        , _declaredTiles(std::vector<int>())
        , _tileHashCalls(0)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    void paint(int tile)
    {
        {
            std::lock_guard<std::mutex> locker(_mutex);
            ++_strokes[tile];
        }
        getNode()->markHashDirty();
    }

    // Set before the metadata is first asked for: the declaration is not in the node's hash.
    void declareTiles(const ImageMetadata::Value& tiles)
    {
        _declaredTiles = tiles;
    }

    int tileHashCallCount() const
    {
        return _tileHashCalls.load();
    }

    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL
    {
        std::lock_guard<std::mutex> locker(_mutex);
        h.appendU64(_strokes.size());
        for (std::map<int, int>::const_iterator it = _strokes.begin(); it != _strokes.end(); ++it) {
            h.appendI64(it->first);
            h.appendI64(it->second);
        }
    }

    virtual U64 getUdimTileHash(int tile,
                                double time,
                                ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++_tileHashCalls;
        NodePtr node = getNode();
        ContentHasher h;
        h.appendString(node->getPluginID());
        bool hasExpression = false;
        h.appendU64(node->getKnobsContentDigest(&hasExpression));
        h.appendI64(tile);
        {
            std::lock_guard<std::mutex> locker(_mutex);
            std::map<int, int>::const_iterator found = _strokes.find(tile);
            h.appendI64(found == _strokes.end() ? 0 : found->second);
        }
        for (int i = 0; i < 2; ++i) {
            NodePtr input = node->getInput(i);
            h.appendBool(bool(input));
            if (input) {
                h.appendU64(udimTileVersion(input, tile, time, view));
            }
        }

        return h.finalize();
    }

protected:
    virtual ImageMetadata deriveOutputMetadata(double /*time*/,
                                               ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ImageMetadata m;
        m.set(kNatronMetadataKeyUdimTiles, _declaredTiles);

        return m;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kUdimPaintTestPluginID,
        .label = "Test UDIM Paint",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
        .inputs = kUdimPaintInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    mutable std::mutex _mutex;
    std::map<int, int> _strokes;
    ImageMetadata::Value _declaredTiles;
    std::atomic<int> _tileHashCalls;
};

void
registerUdimPaintTestEffectOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(UdimPaintTestEffect::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&UdimPaintTestEffect::BuildEffect));
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

const double kTime = 1.;

std::vector<int>
parsed(const ImageMetadata::Value& value)
{
    std::vector<int> tiles;
    std::string error;
    EXPECT_TRUE(parseUdimTiles(value, &tiles, &error)) << error;

    return tiles;
}
} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

class UdimTileVersion
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        registerUdimPaintTestEffectOnce();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createPaint(UdimPaintTestEffect** effect)
    {
        NodePtr node = createNode(QString::fromUtf8(kUdimPaintTestPluginID));
        *effect = node ? dynamic_cast<UdimPaintTestEffect*>(node->getEffectInstance().get()) : NULL;
        EXPECT_TRUE(*effect != NULL);

        return node;
    }

    NodePtr createGrade(const NodePtr& source)
    {
        NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        EXPECT_TRUE(bool(grade));
        if (grade && source) {
            connectNodes(source, grade, 0, true);
        }

        return grade;
    }

    static U64 version(const NodePtr& node,
                       int tile)
    {
        return udimTileVersion(node, tile, kTime, ViewIdx(0));
    }
};

TEST_F(UdimTileVersion, StringAndVectorFormsParseToTheSameTiles)
{
    const std::vector<int> expected = { 1001, 1002, 1003, 1004, 1011 };

    EXPECT_EQ(expected, parsed(ImageMetadata::Value(std::string("1001-1004,1011"))));
    EXPECT_EQ(expected, parsed(ImageMetadata::Value(std::string(" 1011 , 1002-1004,1001 ,1003"))));
    EXPECT_EQ(expected, parsed(ImageMetadata::Value(std::vector<int>({ 1011, 1004, 1001, 1003, 1002, 1011 }))));
    EXPECT_EQ(std::vector<int>({ 1005 }), parsed(ImageMetadata::Value(1005)));
    EXPECT_EQ(std::vector<int>({ 1005 }), parsed(ImageMetadata::Value(std::string("1005-1005"))));
}

TEST_F(UdimTileVersion, JunkIsRejected)
{
    const std::vector<ImageMetadata::Value> junk = {
        ImageMetadata::Value(std::string()),
        ImageMetadata::Value(std::string("  ")),
        ImageMetadata::Value(std::string("abc")),
        ImageMetadata::Value(std::string("1001-")),
        ImageMetadata::Value(std::string("-1001")),
        ImageMetadata::Value(std::string("1001,,1002")),
        ImageMetadata::Value(std::string("1001,")),
        ImageMetadata::Value(std::string("1004-1001")),
        ImageMetadata::Value(std::string("1001 1002")),
        ImageMetadata::Value(std::string("1001x")),
        ImageMetadata::Value(std::string("1000")),
        ImageMetadata::Value(std::string("1001-99999999999999999999")),
        ImageMetadata::Value(0),
        ImageMetadata::Value(0.5),
        ImageMetadata::Value(std::vector<double>({ 1001. })),
        ImageMetadata::Value(std::vector<int>()),
        ImageMetadata::Value(std::vector<int>({ 1001, 5 })),
    };

    for (std::size_t i = 0; i < junk.size(); ++i) {
        std::vector<int> tiles(1, 1001);
        std::string error;
        EXPECT_FALSE(parseUdimTiles(junk[i], &tiles, &error)) << "junk #" << i;
        EXPECT_TRUE(tiles.empty()) << "junk #" << i;
        EXPECT_FALSE(error.empty()) << "junk #" << i;
    }
}

TEST_F(UdimTileVersion, DeclaredTilesPassThroughAGrade)
{
    UdimPaintTestEffect* paint = NULL;
    NodePtr source = createPaint(&paint);
    ASSERT_TRUE(paint != NULL);
    paint->declareTiles(ImageMetadata::Value(std::string("1001-1002,1011")));
    NodePtr grade = createGrade(source);
    ASSERT_TRUE(bool(grade));

    const ImageMetadata metadata = getEffectOutputMetadata(grade->getEffectInstance(), kTime, ViewIdx(0));
    const ImageMetadata::Value* value = metadata.find(kNatronMetadataKeyUdimTiles);
    ASSERT_TRUE(value != NULL);
    EXPECT_EQ(std::vector<int>({ 1001, 1002, 1011 }), parsed(*value));
}

TEST_F(UdimTileVersion, EditOfOneTileMovesOnlyThatTileThroughAGrade)
{
    UdimPaintTestEffect* paint = NULL;
    NodePtr source = createPaint(&paint);
    ASSERT_TRUE(paint != NULL);
    NodePtr grade = createGrade(source);
    ASSERT_TRUE(bool(grade));

    const U64 source1001 = version(source, 1001);
    const U64 source1002 = version(source, 1002);
    const U64 grade1001 = version(grade, 1001);
    const U64 grade1002 = version(grade, 1002);
    EXPECT_EQ(source1001, version(source, 1001));
    EXPECT_EQ(grade1001, version(grade, 1001));

    paint->paint(1001);

    EXPECT_NE(source1001, version(source, 1001));
    EXPECT_EQ(source1002, version(source, 1002));
    EXPECT_NE(grade1001, version(grade, 1001));
    EXPECT_EQ(grade1002, version(grade, 1002));
}

TEST_F(UdimTileVersion, GradeEditMovesEveryTile)
{
    UdimPaintTestEffect* paint = NULL;
    NodePtr source = createPaint(&paint);
    ASSERT_TRUE(paint != NULL);
    NodePtr grade = createGrade(source);
    ASSERT_TRUE(bool(grade));
    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);

    const U64 source1001 = version(source, 1001);
    const U64 grade1001 = version(grade, 1001);
    const U64 grade1002 = version(grade, 1002);
    const U64 grade1011 = version(grade, 1011);

    multiply->setValues(2., 2., 2., 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    EXPECT_EQ(source1001, version(source, 1001));
    EXPECT_NE(grade1001, version(grade, 1001));
    EXPECT_NE(grade1002, version(grade, 1002));
    EXPECT_NE(grade1011, version(grade, 1011));
}

TEST_F(UdimTileVersion, SharedSubgraphIsWalkedOncePerCall)
{
    UdimPaintTestEffect* rootEffect = NULL;
    NodePtr root = createPaint(&rootEffect);
    ASSERT_TRUE(rootEffect != NULL);

    // Each level reads the previous one through both inputs: without memoization the root would
    // be asked 2^16 times.
    const int kLevels = 16;
    NodePtr previous = root;
    for (int level = 0; level < kLevels; ++level) {
        UdimPaintTestEffect* effect = NULL;
        NodePtr node = createPaint(&effect);
        ASSERT_TRUE(effect != NULL);
        connectNodes(previous, node, 0, true);
        connectNodes(previous, node, 1, true);
        previous = node;
    }
    NodePtr grade = createGrade(previous);
    ASSERT_TRUE(bool(grade));

    const U64 before = version(grade, 1003);
    EXPECT_EQ(1, rootEffect->tileHashCallCount());

    rootEffect->paint(1003);
    EXPECT_NE(before, version(grade, 1003));
    EXPECT_EQ(2, rootEffect->tileHashCallCount());
}
