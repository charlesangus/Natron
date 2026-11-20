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
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <gtest/gtest.h>

#include "MergeScenarios.h"

#include "Serialization/Merge/MergeScope.h"
#include "Serialization/Merge/NodeMatcher.h"

NATRON_NAMESPACE_USING
using namespace MergeScenarios;

namespace {

typedef std::function<Serialization::NodeCorrespondence(const Serialization::MergeGraph&, const Serialization::MergeGraph&, const std::string&)> Strategy;

// Strategies the matcher was chosen over, kept to show what each would get on the corpus.
Serialization::NodeCorrespondence
matchByIdOnly(const Serialization::MergeGraph& base,
              const Serialization::MergeGraph& other,
              const std::string& templateKey)
{
    std::map<std::string, std::vector<std::string>> claims;
    for (const std::pair<const std::string, Serialization::MergeGraph::Node>& entry : other.nodes()) {
        const Serialization::NodeOriginSerialization& origin = entry.second.node->origin;
        const std::string claimed = (origin.isSet() && (origin.templateKey == templateKey)) ? origin.nodeId : entry.first;
        if (base.find(claimed)) {
            claims[claimed].push_back(entry.first);
        }
    }
    Serialization::NodeCorrespondence result;
    for (const std::pair<const std::string, std::vector<std::string>>& claim : claims) {
        if (claim.second.size() == 1) {
            result.baseToOther[claim.first] = claim.second.front();
        }
    }

    return result;
}

Serialization::NodeCorrespondence
matchByName(const Serialization::MergeGraph& base,
            const Serialization::MergeGraph& other,
            const std::string& /*templateKey*/)
{
    Serialization::NodeCorrespondence result;
    for (const std::pair<const std::string, Serialization::MergeGraph::Node>& entry : base.nodes()) {
        const Serialization::MergeGraph::Node* found = other.findByPath(entry.second.path);
        if (found) {
            result.baseToOther[entry.first] = found->node->id;
        }
    }

    return result;
}

struct Score {
    int correct = 0;
    int wrong = 0;
    int missing = 0;
    std::string details;
};

Score
score(const Serialization::MergeGraph& base,
      const Serialization::MergeGraph& other,
      const Serialization::NodeCorrespondence& correspondence,
      const std::map<std::string, std::string>& expected)
{
    Score result;
    for (const std::pair<const std::string, Serialization::MergeGraph::Node>& entry : base.nodes()) {
        const std::string got = correspondence.otherFor(entry.first);
        const std::string gotPath = got.empty() ? std::string() : other.find(got)->path;
        std::map<std::string, std::string>::const_iterator want = expected.find(entry.second.path);
        if (want == expected.end()) {
            result.details += " unlisted:" + entry.second.path;
            ++result.wrong;
        } else if (gotPath == want->second) {
            ++result.correct;
        } else {
            (gotPath.empty() ? result.missing : result.wrong) += 1;
            result.details += " " + entry.second.path + "->" + (gotPath.empty() ? std::string("~") : gotPath);
        }
    }

    return result;
}

std::set<std::string>
pathsOf(const Serialization::MergeGraph& graph,
        const std::set<std::string>& ids)
{
    std::set<std::string> paths;
    for (const std::string& id : ids) {
        paths.insert(graph.find(id)->path);
    }

    return paths;
}

std::string
expectedPath(const std::string& scenario)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR) + "/merge-scenarios/expected/" + scenario + ".yaml";
}

} // namespace

TEST(NodeMatcher, EveryExpectedFileHasAScenarioAndBack)
{
    const std::map<std::string, Scenario> all = scenarios();
    std::set<std::string> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(std::string(NATRON_TESTS_FIXTURES_DIR) + "/merge-scenarios/expected")) {
        if (entry.path().extension() == ".yaml") {
            files.insert(entry.path().stem().string());
        }
    }
    std::set<std::string> built;
    for (const std::pair<const std::string, Scenario>& scenario : all) {
        built.insert(scenario.first);
    }
    EXPECT_EQ(files, built);
}

TEST(NodeMatcher, MatchesAndScopesEveryScenarioAsExpected)
{
    const std::vector<std::pair<std::string, Strategy>> strategies = {
        { "IdPlusAdoption", Serialization::matchNodes },
        { "IdOnly", matchByIdOnly },
        { "NameBased", matchByName },
    };
    std::ostringstream table;
    table << std::left << std::setw(44) << "scenario [import]";
    for (const std::pair<std::string, Strategy>& strategy : strategies) {
        table << " | " << std::setw(22) << (strategy.first + " mine/theirs");
    }
    table << " | MatchedEnclosed scope\n";
    std::vector<int> totals(strategies.size(), 0);
    int bases = 0;

    for (const std::pair<const std::string, Scenario>& scenario : scenarios()) {
        const std::vector<ExpectedMerge> merges = readExpected(expectedPath(scenario.first));
        EXPECT_FALSE(merges.empty()) << scenario.first;
        const Serialization::ProjectSerialization baseProject = scenario.second.base.project();
        const Serialization::ProjectSerialization mineProject = scenario.second.mine.project();
        const Serialization::ProjectSerialization theirsProject = scenario.second.theirs.project();
        const Serialization::MergeGraph base(baseProject);
        const Serialization::MergeGraph mine(mineProject);
        const Serialization::MergeGraph theirs(theirsProject);

        for (const ExpectedMerge& merge : merges) {
            SCOPED_TRACE(scenario.first + " [" + merge.templateKey + "]");
            table << std::setw(44) << (scenario.first + " [" + merge.templateKey + "]");
            for (std::size_t i = 0; i < strategies.size(); ++i) {
                const Score mineScore = score(base, mine, strategies[i].second(base, mine, merge.templateKey), merge.baseMine);
                const Score theirsScore = score(base, theirs, strategies[i].second(base, theirs, merge.templateKey), merge.baseTheirs);
                std::ostringstream cell;
                cell << mineScore.correct << "/" << (mineScore.correct + mineScore.wrong + mineScore.missing) << "  " << theirsScore.correct << "/" << (theirsScore.correct + theirsScore.wrong + theirsScore.missing);
                table << " | " << std::setw(22) << cell.str();
                totals[i] += mineScore.correct + theirsScore.correct;
                if (i == 0) {
                    EXPECT_EQ(mineScore.wrong + mineScore.missing, 0) << "mine:" << mineScore.details;
                    EXPECT_EQ(theirsScore.wrong + theirsScore.missing, 0) << "theirs:" << theirsScore.details;
                    bases += 2 * int(base.nodes().size());
                }
            }

            const std::set<std::string> scope = pathsOf(mine, Serialization::mergeScope(mine, Serialization::matchNodes(base, mine, merge.templateKey)));
            EXPECT_EQ(scope, merge.scope);
            table << " | " << ((scope == merge.scope) ? "ok" : "differs") << "\n";
        }
    }

    table << std::setw(44) << "correct matches";
    for (int total : totals) {
        std::ostringstream cell;
        cell << total << "/" << bases;
        table << " | " << std::setw(22) << cell.str();
    }
    table << "\n";
    std::cout << table.str();
}

TEST(NodeMatcher, MatchingIgnoresTheOrderNodesAreListedIn)
{
    for (const std::pair<const std::string, Scenario>& scenario : scenarios()) {
        SCOPED_TRACE(scenario.first);
        const Serialization::ProjectSerialization baseProject = scenario.second.base.project();
        const Serialization::ProjectSerialization mineProject = scenario.second.mine.project();
        Serialization::ProjectSerialization reversedProject = mineProject;
        reversedProject.nodes.nodes.reverse();
        const Serialization::MergeGraph base(baseProject);
        const Serialization::NodeCorrespondence first = Serialization::matchNodes(base, Serialization::MergeGraph(mineProject), "grade_tpl");
        const Serialization::NodeCorrespondence again = Serialization::matchNodes(base, Serialization::MergeGraph(mineProject), "grade_tpl");
        const Serialization::NodeCorrespondence reversed = Serialization::matchNodes(base, Serialization::MergeGraph(reversedProject), "grade_tpl");
        EXPECT_EQ(first.baseToOther, again.baseToOther);
        EXPECT_EQ(first.baseToOther, reversed.baseToOther);
        EXPECT_EQ(first.adopted, reversed.adopted);
    }
}

TEST(NodeMatcher, RecordedAdoptionsMatchByIdentityNextTime)
{
    const Scenario scenario = scenarios().at("delete-recreate-grade");
    const Serialization::ProjectSerialization baseProject = scenario.base.project();
    Serialization::ProjectSerialization mineProject = scenario.mine.project();
    const Serialization::MergeGraph base(baseProject);
    const Serialization::NodeCorrespondence first = Serialization::matchNodes(base, Serialization::MergeGraph(mineProject), "grade_tpl");
    const std::string grade = hexId("b:Grade1");
    ASSERT_EQ(first.otherFor(grade), hexId("m:new"));
    EXPECT_EQ(first.adopted.count(hexId("m:new")), 1u);

    Serialization::recordAdoptions(first, "grade_tpl", &mineProject);
    Serialization::NodeSerializationPtr adopted = Serialization::findNodeSerialization(mineProject.nodes.nodes, hexId("m:new"));
    ASSERT_TRUE(adopted != nullptr);
    EXPECT_EQ(adopted->origin.templateKey, "grade_tpl");
    EXPECT_EQ(adopted->origin.nodeId, grade);

    // Rewired away from the template's chain, the node still matches: it now has an identity.
    adopted->inputs.clear();
    const Serialization::NodeCorrespondence second = Serialization::matchNodes(base, Serialization::MergeGraph(mineProject), "grade_tpl");
    EXPECT_EQ(second.otherFor(grade), hexId("m:new"));
    EXPECT_TRUE(second.adopted.empty());
}

TEST(NodeMatcher, AnOriginNamingAnotherImportIsNeverAdopted)
{
    Scenario scenario = scenarios().at("imported-twice");
    scenario.mine.remove("Grade1");
    scenario.mine.get("Blur1").inputs["Source"] = "TplIn";
    const Serialization::ProjectSerialization baseProject = scenario.base.project();
    const Serialization::ProjectSerialization mineProject = scenario.mine.project();
    const Serialization::NodeCorrespondence match = Serialization::matchNodes(Serialization::MergeGraph(baseProject), Serialization::MergeGraph(mineProject), "grade_tpl");
    EXPECT_TRUE(match.otherFor(hexId("b:Grade1")).empty());
    EXPECT_EQ(match.otherFor(hexId("b:Blur1")), hexId("m:Blur1"));
}
