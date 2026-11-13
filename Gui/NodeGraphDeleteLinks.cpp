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

#include "NodeGraphDeleteLinks.h"

#include <map>
#include <set>
#include <tuple>

#include <QCoreApplication>
#include <QStringList>

NATRON_NAMESPACE_ENTER

namespace {
typedef std::tuple<std::string, std::string, std::string, std::string> LinkKey;

LinkKey
keyOf(const BrokenLink& l)
{
    return std::make_tuple(l.masterNode, l.masterKnob, l.slaveNode, l.slaveKnob);
}

QString
utf8(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
}
} // anonymous namespace

std::size_t
countDistinctBrokenLinks(const std::vector<BrokenLink>& links)
{
    std::set<LinkKey> seen;
    for (std::size_t i = 0; i < links.size(); ++i) {
        seen.insert(keyOf(links[i]));
    }

    return seen.size();
}

QString
brokenLinksPrompt(const std::vector<BrokenLink>& links,
                  std::size_t maxShown)
{
    std::set<LinkKey> seen;
    std::vector<std::string> masterOrder;
    std::map<std::string, std::vector<const BrokenLink*>> byMaster;
    for (std::size_t i = 0; i < links.size(); ++i) {
        if (!seen.insert(keyOf(links[i])).second) {
            continue;
        }
        std::vector<const BrokenLink*>& group = byMaster[links[i].masterNode];
        if (group.empty()) {
            masterOrder.push_back(links[i].masterNode);
        }
        group.push_back(&links[i]);
    }
    if (seen.empty()) {
        return QString();
    }

    QStringList lines;
    lines << QCoreApplication::translate("NodeGraph", "%1 link(s) from nodes that stay read nodes being deleted. Deleting them may break these links:").arg(seen.size());
    lines << QString();

    std::size_t shown = 0;
    for (std::size_t m = 0; m < masterOrder.size() && shown < maxShown; ++m) {
        const std::vector<const BrokenLink*>& group = byMaster[masterOrder[m]];
        lines << utf8(masterOrder[m]) + QLatin1Char(':');
        for (std::size_t i = 0; i < group.size() && shown < maxShown; ++i, ++shown) {
            const BrokenLink& l = *group[i];
            lines << QCoreApplication::translate("NodeGraph", "    %1.%2 reads %3.%4")
                         .arg(utf8(l.slaveNode), utf8(l.slaveKnob), utf8(l.masterNode), utf8(l.masterKnob));
        }
    }
    if (shown < seen.size()) {
        lines << QCoreApplication::translate("NodeGraph", "and %1 more").arg(seen.size() - shown);
    }
    lines << QString();
    lines << QCoreApplication::translate("NodeGraph", "Continue anyway?");

    return lines.join(QLatin1Char('\n'));
}

NATRON_NAMESPACE_EXIT
