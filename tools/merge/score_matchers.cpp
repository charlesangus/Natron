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

// Scores the prototype node matchers and merge-scope rules against the merge-scenario corpus:
//
//   natron-score-matchers Tests/fixtures/merge-scenarios [--verbose]
//
// Prints one table per comparison (base against mine, base against theirs, mine's merge scope)
// with a row per scenario merge and a column per strategy or rule. Exits 2 when the corpus or an
// expected file cannot be read or names a node the snapshots lack.

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Merge/NodeMatcher.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_USING
using namespace Serialization;

namespace fs = std::filesystem;

namespace {

typedef std::map<std::string, std::optional<std::string>> ExpectedMap;

struct ExpectedMerge {
    std::string templateKey;
    ExpectedMap baseMine;
    ExpectedMap baseTheirs;
    std::set<std::string> scope;
};

struct Score {
    int correct = 0;
    int wrong = 0;
    int missing = 0;

    void add(const Score& other)
    {
        correct += other.correct;
        wrong += other.wrong;
        missing += other.missing;
    }

    std::string str() const
    {
        std::ostringstream out;
        out << correct << '/' << wrong << '/' << missing;
        return out.str();
    }
};

struct ScopeScore {
    int extra = 0;
    int missing = 0;

    std::string str() const
    {
        if (!extra && !missing) {
            return "ok";
        }
        std::ostringstream out;
        out << '+' << extra << " -" << missing;
        return out.str();
    }
};

struct Row {
    std::string label;
    std::vector<Score> mine;
    std::vector<Score> theirs;
    std::vector<ScopeScore> scope;
};

class CorpusError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

ProjectSerialization
readSnapshot(const fs::path& path)
{
    if (!fs::exists(path)) {
        throw CorpusError(path.string() + " does not exist");
    }
    ProjectBundleReader reader(path.string());
    YamlWarnings warnings;

    return reader.readProject(&warnings);
}

NodeOrigins
readOrigins(const fs::path& path)
{
    NodeOrigins origins;
    if (!fs::exists(path)) {
        return origins;
    }
    const YAML::Node root = YAML::LoadFile(path.string());
    const YAML::Node entries = root["origins"];
    if (entries && entries.IsMap()) {
        for (YAML::const_iterator it = entries.begin(); it != entries.end(); ++it) {
            origins[it->first.as<std::string>()] = NodeOrigin { it->second["template"].as<std::string>(), it->second["node"].as<std::string>() };
        }
    }

    return origins;
}

ExpectedMap
readExpectedMap(const YAML::Node& node,
                const std::string& context)
{
    if (!node || !node.IsMap()) {
        throw CorpusError(context + " is missing or not a map");
    }
    ExpectedMap result;
    for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
        if (it->second.IsNull()) {
            result[it->first.as<std::string>()] = std::nullopt;
        } else {
            result[it->first.as<std::string>()] = it->second.as<std::string>();
        }
    }

    return result;
}

std::vector<ExpectedMerge>
readExpected(const fs::path& path)
{
    const YAML::Node root = YAML::LoadFile(path.string());
    const YAML::Node merges = root["merges"];
    if (!merges || !merges.IsSequence() || (merges.size() == 0)) {
        throw CorpusError(path.string() + ": no merges");
    }
    std::vector<ExpectedMerge> result;
    for (const YAML::Node& merge : merges) {
        ExpectedMerge expected;
        expected.templateKey = merge["template"].as<std::string>();
        const std::string context = path.string() + " [" + expected.templateKey + "]";
        expected.baseMine = readExpectedMap(merge["base_mine"], context + " base_mine");
        expected.baseTheirs = readExpectedMap(merge["base_theirs"], context + " base_theirs");
        for (const YAML::Node& name : merge["scope"]) {
            expected.scope.insert(name.as<std::string>());
        }
        result.push_back(std::move(expected));
    }

    return result;
}

void
checkExpected(const MergeGraph& base,
              const MergeGraph& other,
              const ExpectedMap& expected,
              const std::string& context)
{
    for (const std::pair<const std::string, MergeGraph::Node>& entry : base.nodes()) {
        if (!expected.count(entry.second.path)) {
            throw CorpusError(context + " does not say what base node " + entry.second.path + " matches");
        }
    }
    for (const std::pair<const std::string, std::optional<std::string>>& entry : expected) {
        if (!base.findByPath(entry.first)) {
            throw CorpusError(context + " names " + entry.first + ", which the base lacks");
        }
        if (entry.second && !other.findByPath(*entry.second)) {
            throw CorpusError(context + " names " + *entry.second + ", which the other side lacks");
        }
    }
}

std::string
pathOf(const MergeGraph& graph,
       const std::string& id)
{
    const MergeGraph::Node* node = graph.find(id);

    return node ? node->path : std::string("(none)");
}

Score
scoreCorrespondence(const MergeGraph& base,
                    const MergeGraph& other,
                    const ExpectedMap& expected,
                    const NodeCorrespondence& actual,
                    const std::string& context,
                    bool verbose)
{
    Score score;
    for (const std::pair<const std::string, MergeGraph::Node>& entry : base.nodes()) {
        const std::optional<std::string>& want = expected.at(entry.second.path);
        const std::string got = actual.otherFor(entry.first);
        const std::optional<std::string> gotPath = got.empty() ? std::nullopt : std::optional<std::string>(pathOf(other, got));
        if (gotPath == want) {
            ++score.correct;
            continue;
        }
        if (!gotPath) {
            ++score.missing;
        } else {
            ++score.wrong;
        }
        if (verbose) {
            std::cout << "  " << context << ": " << entry.second.path << " -> " << gotPath.value_or("(none)")
                      << ", expected " << want.value_or("(none)") << '\n';
        }
    }

    return score;
}

ScopeScore
scoreScope(const MergeGraph& mine,
           const std::set<std::string>& actualIds,
           const std::set<std::string>& expected,
           const std::string& context,
           bool verbose)
{
    std::set<std::string> actual;
    for (const std::string& id : actualIds) {
        actual.insert(pathOf(mine, id));
    }
    ScopeScore score;
    for (const std::string& path : actual) {
        if (!expected.count(path)) {
            ++score.extra;
            if (verbose) {
                std::cout << "  " << context << ": " << path << " in scope, expected out\n";
            }
        }
    }
    for (const std::string& path : expected) {
        if (!actual.count(path)) {
            ++score.missing;
            if (verbose) {
                std::cout << "  " << context << ": " << path << " out of scope, expected in\n";
            }
        }
    }

    return score;
}

void
checkScopeNames(const MergeGraph& mine,
                const std::set<std::string>& expected,
                const std::string& context)
{
    for (const std::string& path : expected) {
        if (!mine.findByPath(path)) {
            throw CorpusError(context + " scope names " + path + ", which mine lacks");
        }
    }
}

std::string
pad(const std::string& text,
    std::size_t width)
{
    return (text.size() >= width) ? text + ' ' : text + std::string(width - text.size(), ' ');
}

void
printTable(const std::string& title,
           const std::vector<std::string>& columns,
           const std::vector<Row>& rows,
           const std::vector<std::vector<std::string>>& cells,
           const std::vector<std::string>& totals)
{
    std::size_t labelWidth = 5;
    for (const Row& row : rows) {
        labelWidth = std::max(labelWidth, row.label.size() + 2);
    }
    std::size_t cellWidth = 10;
    for (const std::string& column : columns) {
        cellWidth = std::max(cellWidth, column.size() + 2);
    }
    std::cout << '\n'
              << title << '\n'
              << pad("scenario", labelWidth);
    for (const std::string& column : columns) {
        std::cout << pad(column, cellWidth);
    }
    std::cout << '\n';
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::cout << pad(rows[r].label, labelWidth);
        for (const std::string& cell : cells[r]) {
            std::cout << pad(cell, cellWidth);
        }
        std::cout << '\n';
    }
    if (!totals.empty()) {
        std::cout << pad("total", labelWidth);
        for (const std::string& cell : totals) {
            std::cout << pad(cell, cellWidth);
        }
        std::cout << '\n';
    }
}

} // namespace

int
main(int argc,
     char** argv)
{
    std::optional<fs::path> corpus;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verbose") {
            verbose = true;
        } else if (!corpus) {
            corpus = fs::path(arg);
        } else {
            std::cerr << "usage: natron-score-matchers <merge-scenarios folder> [--verbose]\n";
            return 2;
        }
    }
    if (!corpus) {
        std::cerr << "usage: natron-score-matchers <merge-scenarios folder> [--verbose]\n";
        return 2;
    }

    const std::vector<std::unique_ptr<NodeMatcher>> matchers = prototypeNodeMatchers();
    const std::vector<MergeScopeRule>& rules = allMergeScopeRules();
    std::vector<Row> rows;

    try {
        std::vector<fs::path> expectedFiles;
        for (const fs::directory_entry& entry : fs::directory_iterator(*corpus / "expected")) {
            if (entry.path().extension() == ".yaml") {
                expectedFiles.push_back(entry.path());
            }
        }
        std::sort(expectedFiles.begin(), expectedFiles.end());
        if (expectedFiles.empty()) {
            throw CorpusError((*corpus / "expected").string() + " holds no scenario");
        }

        for (const fs::path& expectedFile : expectedFiles) {
            const std::string scenario = expectedFile.stem().string();
            const fs::path folder = *corpus / scenario;
            const ProjectSerialization baseProject = readSnapshot(folder / "base.ntpz");
            const ProjectSerialization mineProject = readSnapshot(folder / "mine.ntpz");
            const ProjectSerialization theirsProject = readSnapshot(folder / "theirs.ntpz");
            const MergeGraph base(baseProject);
            const MergeGraph mine(mineProject, readOrigins(folder / "mine.provenance.yaml"));
            const MergeGraph theirs(theirsProject);
            const std::vector<ExpectedMerge> merges = readExpected(expectedFile);

            for (const ExpectedMerge& merge : merges) {
                Row row;
                row.label = (merges.size() > 1) ? scenario + " [" + merge.templateKey + "]" : scenario;
                checkExpected(base, mine, merge.baseMine, row.label + " base_mine");
                checkExpected(base, theirs, merge.baseTheirs, row.label + " base_theirs");
                checkScopeNames(mine, merge.scope, row.label);
                if (verbose) {
                    std::cout << row.label << '\n';
                }
                for (const std::unique_ptr<NodeMatcher>& matcher : matchers) {
                    const std::string name(matcher->name());
                    row.mine.push_back(scoreCorrespondence(base, mine, merge.baseMine, matcher->match(base, mine, merge.templateKey), name + " mine", verbose));
                    row.theirs.push_back(scoreCorrespondence(base, theirs, merge.baseTheirs, matcher->match(base, theirs, merge.templateKey), name + " theirs", verbose));
                }
                for (MergeScopeRule rule : rules) {
                    row.scope.push_back(scoreScope(mine, mergeScope(base, mine, merge.templateKey, rule), merge.scope, std::string(mergeScopeRuleName(rule)), verbose));
                }
                rows.push_back(std::move(row));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "natron-score-matchers: " << e.what() << '\n';
        return 2;
    }

    std::vector<std::string> matcherNames;
    for (const std::unique_ptr<NodeMatcher>& matcher : matchers) {
        matcherNames.emplace_back(matcher->name());
    }
    std::vector<std::string> ruleNames;
    for (MergeScopeRule rule : rules) {
        ruleNames.emplace_back(mergeScopeRuleName(rule));
    }

    for (int side = 0; side < 2; ++side) {
        std::vector<std::vector<std::string>> cells;
        std::vector<Score> totals(matchers.size());
        for (const Row& row : rows) {
            const std::vector<Score>& scores = side ? row.theirs : row.mine;
            std::vector<std::string> line;
            for (std::size_t i = 0; i < scores.size(); ++i) {
                line.push_back(scores[i].str());
                totals[i].add(scores[i]);
            }
            cells.push_back(std::move(line));
        }
        std::vector<std::string> totalCells;
        for (const Score& total : totals) {
            totalCells.push_back(total.str());
        }
        printTable(side ? "Base <-> theirs: correct/wrong/missing base nodes" : "Base <-> mine: correct/wrong/missing base nodes",
                   matcherNames, rows, cells, totalCells);
    }

    std::vector<std::vector<std::string>> scopeCells;
    std::vector<int> scopeExact(rules.size(), 0);
    for (const Row& row : rows) {
        std::vector<std::string> line;
        for (std::size_t i = 0; i < row.scope.size(); ++i) {
            line.push_back(row.scope[i].str());
            if (!row.scope[i].extra && !row.scope[i].missing) {
                ++scopeExact[i];
            }
        }
        scopeCells.push_back(std::move(line));
    }
    std::vector<std::string> scopeTotals;
    for (int exact : scopeExact) {
        scopeTotals.push_back(std::to_string(exact) + "/" + std::to_string(rows.size()) + " ok");
    }
    printTable("Mine's merge scope: ok, or +nodes wrongly in -nodes wrongly out", ruleNames, rows, scopeCells, scopeTotals);

    return 0;
}
