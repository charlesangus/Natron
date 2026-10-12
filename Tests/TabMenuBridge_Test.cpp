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
#include <memory>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Settings.h"

#include "Gui/TabMenuBridge.h"

NATRON_NAMESPACE_USING

namespace {
QJsonArray
parseItems(const QString& json)
{
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);

    EXPECT_EQ(QJsonParseError::NoError, error.error);
    EXPECT_TRUE(doc.isArray());

    return doc.array();
}

QString
noShortcut(const QString&,
           const QString&)
{
    return QString();
}

class FakePlugins {
public:
    Plugin* add(const QString& id,
                const QString& label,
                int major,
                bool deprecated = false,
                const QString& iconPath = QString())
    {
        _owned.emplace_back(new Plugin(0, QString(), id, label, iconPath, QStringList(),
                                       QStringList() << QString::fromUtf8("Filter"), 0,
                                       major, 0, false, false, deprecated));
        Plugin* p = _owned.back().get();
        p->setLabelWithoutSuffix(label);
        _map[id.toStdString()].insert(p);

        return p;
    }

    const PluginsMap& map() const
    {
        return _map;
    }

private:
    std::vector<std::unique_ptr<Plugin>> _owned;
    PluginsMap _map;
};

std::vector<QJsonObject>
itemsWithId(const QJsonArray& items,
            const QString& id)
{
    std::vector<QJsonObject> ret;

    for (const QJsonValue& v : items) {
        QJsonObject o = v.toObject();
        if (o.value(QString::fromUtf8("id")).toString() == id) {
            ret.push_back(o);
        }
    }

    return ret;
}
} // anonymous namespace

TEST(TabMenuBridge, RegistryListsOnlyUserCreatablePlugins)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    QJsonArray items = parseItems(TabMenuBridge::nodeItemsJson(plugins, noShortcut));

    ASSERT_FALSE(items.isEmpty());

    std::set<std::pair<QString, int>> creatable;
    std::size_t creatableCount = 0;
    for (PluginsMap::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        for (PluginVersionsOrdered::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
            if ((*it2)->getIsUserCreatable()) {
                creatable.insert(std::make_pair((*it2)->getPluginID(), (*it2)->getMajorVersion()));
                ++creatableCount;
            }
        }
    }

    EXPECT_EQ(creatableCount, (std::size_t)items.size());
    for (const QJsonValue& v : items) {
        QJsonObject o = v.toObject();
        std::pair<QString, int> key(o.value(QString::fromUtf8("id")).toString(), o.value(QString::fromUtf8("major")).toInt());
        EXPECT_TRUE(creatable.count(key)) << key.first.toStdString();
    }
}

TEST(TabMenuBridge, DotItemCarriesLabelGroupingColourAndShortcut)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    PluginsMap::const_iterator found = plugins.find(PLUGINID_NATRON_DOT);

    ASSERT_TRUE(found != plugins.end());
    ASSERT_FALSE(found->second.empty());
    const Plugin* dot = *found->second.rbegin();

    QString expectedGroup = QString::fromUtf8("Nodes");
    for (const QString& g : dot->getGrouping()) {
        expectedGroup += QLatin1Char('/') + g;
    }
    QString seenGroup;
    TabMenuBridge::ShortcutLookup lookup = [&](const QString& group, const QString& id) {
        if (id == QString::fromUtf8(PLUGINID_NATRON_DOT)) {
            seenGroup = group;
            return QString::fromUtf8(".");
        }
        return QString();
    };

    std::vector<QJsonObject> dots = itemsWithId(parseItems(TabMenuBridge::nodeItemsJson(plugins, lookup)),
                                                QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_EQ(1u, dots.size());
    const QJsonObject& item = dots.front();

    EXPECT_EQ(dot->getLabelWithoutSuffix(), item.value(QString::fromUtf8("label")).toString());
    EXPECT_EQ(dot->getMajorVersion(), item.value(QString::fromUtf8("major")).toInt());
    QStringList grouping;
    for (const QJsonValue& g : item.value(QString::fromUtf8("grouping")).toArray()) {
        grouping << g.toString();
    }
    EXPECT_EQ(dot->getGrouping(), grouping);
    EXPECT_EQ(expectedGroup, seenGroup);
    EXPECT_EQ(QString::fromUtf8("."), item.value(QString::fromUtf8("shortcut")).toString());

    float r, g, b;
    appPTR->getCurrentSettings()->getNodeCategoryColor(Node::categoryForPlugin(*dot), &r, &g, &b);
    const QString color = item.value(QString::fromUtf8("color")).toString();
    EXPECT_TRUE(QRegularExpression(QString::fromUtf8("^#[0-9a-f]{6}$")).match(color).hasMatch()) << color.toStdString();
    EXPECT_EQ(QColor::fromRgbF(std::clamp(r, 0.f, 1.f), std::clamp(g, 0.f, 1.f), std::clamp(b, 0.f, 1.f)).name(), color);
}

TEST(TabMenuBridge, NewestVersionKeepsTheBareLabelAndComesFirst)
{
    FakePlugins plugins;

    plugins.add(QString::fromUtf8("test.Blurry"), QString::fromUtf8("Blurry"), 1);
    plugins.add(QString::fromUtf8("test.Blurry"), QString::fromUtf8("Blurry"), 2);

    QJsonArray items = parseItems(TabMenuBridge::nodeItemsJson(plugins.map(), noShortcut));
    ASSERT_EQ(2, items.size());
    EXPECT_EQ(2, items[0].toObject().value(QString::fromUtf8("major")).toInt());
    EXPECT_EQ(QString::fromUtf8("Blurry"), items[0].toObject().value(QString::fromUtf8("label")).toString());
    EXPECT_EQ(1, items[1].toObject().value(QString::fromUtf8("major")).toInt());
    EXPECT_EQ(QString::fromUtf8("Blurry 1"), items[1].toObject().value(QString::fromUtf8("label")).toString());
}

TEST(TabMenuBridge, DeprecatedVersionsAreSkipped)
{
    FakePlugins plugins;

    plugins.add(QString::fromUtf8("test.Old"), QString::fromUtf8("Old"), 1, true);
    plugins.add(QString::fromUtf8("test.Mixed"), QString::fromUtf8("Mixed"), 1);
    plugins.add(QString::fromUtf8("test.Mixed"), QString::fromUtf8("Mixed"), 2, true);

    QJsonArray items = parseItems(TabMenuBridge::nodeItemsJson(plugins.map(), noShortcut));
    ASSERT_EQ(1, items.size());
    const QJsonObject item = items[0].toObject();
    EXPECT_EQ(QString::fromUtf8("test.Mixed"), item.value(QString::fromUtf8("id")).toString());
    EXPECT_EQ(1, item.value(QString::fromUtf8("major")).toInt());
    EXPECT_EQ(QString::fromUtf8("Mixed 1"), item.value(QString::fromUtf8("label")).toString());
}

TEST(TabMenuBridge, IconIsEmptyUnlessTheFileExists)
{
    FakePlugins plugins;
    const QString existing = QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/deep-tiled.exr");

    plugins.add(QString::fromUtf8("test.WithIcon"), QString::fromUtf8("WithIcon"), 1, false, existing);
    plugins.add(QString::fromUtf8("test.MissingIcon"), QString::fromUtf8("MissingIcon"), 1, false,
                QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/no-such-icon.png"));

    QJsonArray items = parseItems(TabMenuBridge::nodeItemsJson(plugins.map(), noShortcut));
    std::vector<QJsonObject> withIcon = itemsWithId(items, QString::fromUtf8("test.WithIcon"));
    std::vector<QJsonObject> missingIcon = itemsWithId(items, QString::fromUtf8("test.MissingIcon"));
    ASSERT_EQ(1u, withIcon.size());
    ASSERT_EQ(1u, missingIcon.size());
    EXPECT_EQ(existing, withIcon.front().value(QString::fromUtf8("icon")).toString());
    EXPECT_EQ(QString(), missingIcon.front().value(QString::fromUtf8("icon")).toString());
    EXPECT_EQ(QString(), missingIcon.front().value(QString::fromUtf8("shortcut")).toString());
}

TEST(TabMenuBridge, CreateNodeWithoutAnOpenMenuDoesNothing)
{
    EXPECT_FALSE(TabMenuBridge::createNode(QString::fromUtf8(PLUGINID_NATRON_DOT), 1));
}
