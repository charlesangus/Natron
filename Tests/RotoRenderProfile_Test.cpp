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

#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLatencyProbe.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kMaskStages[] = {
    "cache_lookup", "allocate", "polygon", "feather_setup", "fill", "feather", "conversion"
};
const char* const kRotoPaintStages[] = { "setup", "tree" };

struct ProfileLine {
    QByteArray raw;
    QJsonObject object;
};

QList<ProfileLine>
readLines(const QString& path)
{
    QList<ProfileLine> lines;
    QFile f(path);

    if (!f.open(QIODevice::ReadOnly)) {
        return lines;
    }
    for (const QByteArray& line : f.readAll().split('\n')) {
        if (!line.trimmed().isEmpty()) {
            ProfileLine entry;
            entry.raw = line;
            entry.object = QJsonDocument::fromJson(line).object();
            lines.append(entry);
        }
    }

    return lines;
}

// A JSON object keeps the last of duplicate keys, so the count is taken on the raw line.
int
keyCount(const QByteArray& raw,
         const std::string& key)
{
    const QByteArray quoted = QByteArray("\"") + QByteArray(key.c_str()) + QByteArray("\":");

    return (int)raw.count(quoted);
}

// The profile is process-wide; a test must not inherit one from the environment or another test.
class RotoRenderProfileTest
    : public BaseTest {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        RotoRenderProfile::stop();
        appPTR->clearAllCaches();
    }

    void TearDown() override
    {
        RotoRenderProfile::stop();
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }

    bool render(const NodePtr& node,
                const RectI& roi,
                double time)
    {
        const ViewIdx view(0);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 view,
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 node->getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstancePtr effect = node->getEffectInstance();
        const RectD canonicalWindow = roi.toCanonical_noClipping(0, 1.);
        std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
        if (EffectInstance::computeRequestPass(time, view, 0, canonicalWindow, node, *request) == eStatusFailed) {
            return false;
        }
        frameRenderArgs.updateNodesRequest(*request);

        std::list<ImageLayerDesc> components;
        components.push_back(ImageLayerDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(time,
                                           RenderScale::fromMipmapLevel(0),
                                           0,
                                           view,
                                           false /*byPassCache*/,
                                           roi,
                                           RectD(),
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           time);
        std::map<ImageLayerDesc, ImagePtr> layers;
        try {
            if (effect->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) {
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }

        return !layers.empty() && bool(layers.begin()->second);
    }
};

} // namespace

TEST_F(RotoRenderProfileTest, OffWithoutAPath)
{
    EXPECT_TRUE(RotoRenderProfile::instance() == nullptr);
    {
        RotoRenderProfile::Scope scope(RotoRenderProfile::eKindMask, 1., 0);
        EXPECT_FALSE(scope.active());
        RotoRenderProfile::StageTimer timer(RotoRenderProfile::eStageFill);
    }
    EXPECT_FALSE(RotoRenderProfile::start(QString::fromUtf8("/nonexistent-dir/roto-profile.jsonl")));
    EXPECT_TRUE(RotoRenderProfile::instance() == nullptr);
}

// Two feathered shapes rendered once from a cold cache: each mask is one miss record holding every
// mask stage exactly once, and the RotoPaint render that drew them is one record of its own.
TEST_F(RotoRenderProfileTest, EveryStageAppearsOncePerMask)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("roto-profile.jsonl"));

    getApp()->getProject()->reset(false, true);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    const double time = 1.;
    BezierPtr first = context->makeSquare(20., 20., 60., time);
    BezierPtr second = context->makeSquare(120., 100., 80., time);
    ASSERT_TRUE(first && second);
    first->setFeatherDistance(8., time);
    second->setFeatherDistance(12., time);
    context->refreshRotoPaintTree();

    ASSERT_TRUE(RotoRenderProfile::start(path));
    ASSERT_TRUE(render(roto, RectI(0, 0, 256, 256), time));
    RotoRenderProfile::stop();

    const QList<ProfileLine> lines = readLines(path);
    ASSERT_FALSE(lines.isEmpty());

    std::set<std::string> missedItems;
    int rotoPaintRecords = 0;
    for (const ProfileLine& line : lines) {
        const std::string kind = line.object.value(QString::fromUtf8("kind")).toString().toStdString();
        ASSERT_TRUE(kind == "mask" || kind == "rotopaint") << line.raw.constData();
        EXPECT_EQ(1, keyCount(line.raw, "total_ms")) << line.raw.constData();
        EXPECT_EQ(1, keyCount(line.raw, "other_ms")) << line.raw.constData();
        EXPECT_GE(line.object.value(QString::fromUtf8("total_ms")).toDouble(-1.), 0.) << line.raw.constData();
        if (kind == "mask") {
            for (const char* stage : kMaskStages) {
                EXPECT_EQ(1, keyCount(line.raw, std::string(stage) + "_ms")) << stage << ": " << line.raw.constData();
                EXPECT_EQ(1, keyCount(line.raw, std::string(stage) + "_calls")) << stage << ": " << line.raw.constData();
            }
            for (const char* stage : kRotoPaintStages) {
                EXPECT_EQ(0, keyCount(line.raw, std::string(stage) + "_ms")) << stage << ": " << line.raw.constData();
            }
            EXPECT_EQ(std::string("bezier"), line.object.value(QString::fromUtf8("type")).toString().toStdString());
            if (!line.object.value(QString::fromUtf8("hit")).toBool(true)) {
                missedItems.insert(line.object.value(QString::fromUtf8("item")).toString().toStdString());
                EXPECT_EQ(1, line.object.value(QString::fromUtf8("cache_lookup_calls")).toInt()) << line.raw.constData();
                EXPECT_EQ(1, line.object.value(QString::fromUtf8("allocate_calls")).toInt()) << line.raw.constData();
                EXPECT_EQ(1, line.object.value(QString::fromUtf8("feather_setup_calls")).toInt()) << line.raw.constData();
                for (const char* stage : { "polygon_calls", "fill_calls", "feather_calls", "conversion_calls" }) {
                    EXPECT_GE(line.object.value(QString::fromUtf8(stage)).toInt(), 1) << stage << ": " << line.raw.constData();
                }
                EXPECT_GT(line.object.value(QString::fromUtf8("pixels")).toDouble(), 0.) << line.raw.constData();
            }
        } else {
            ++rotoPaintRecords;
            for (const char* stage : kRotoPaintStages) {
                EXPECT_EQ(1, keyCount(line.raw, std::string(stage) + "_ms")) << stage << ": " << line.raw.constData();
                EXPECT_EQ(1, keyCount(line.raw, std::string(stage) + "_calls")) << stage << ": " << line.raw.constData();
            }
            for (const char* stage : kMaskStages) {
                EXPECT_EQ(0, keyCount(line.raw, std::string(stage) + "_ms")) << stage << ": " << line.raw.constData();
            }
            EXPECT_EQ(2, line.object.value(QString::fromUtf8("items")).toInt()) << line.raw.constData();
            EXPECT_GE(line.object.value(QString::fromUtf8("setup_calls")).toInt(), 1) << line.raw.constData();
            EXPECT_EQ(1, line.object.value(QString::fromUtf8("tree_calls")).toInt()) << line.raw.constData();
        }
    }
    EXPECT_GE(rotoPaintRecords, 1);
    EXPECT_EQ(2u, missedItems.size());
    EXPECT_TRUE(missedItems.count(first->getScriptName()) == 1);
    EXPECT_TRUE(missedItems.count(second->getScriptName()) == 1);
}
