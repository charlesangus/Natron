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

#include "Global/Macros.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "MergeTool/DiffPrinter.h"
#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/Sha256.h"
#include "Serialization/TemplateNodeSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/GraphOpYaml.h"
#include "Serialization/Yaml/ManifestYaml.h"
#include "Serialization/Yaml/ProjectYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_USING
using namespace Serialization;

namespace {

constexpr int kExitClean = 0;
constexpr int kExitConflicts = 1;
constexpr int kExitDiffers = 1;
constexpr int kExitError = 2;

const char* const kUsage = "usage:\n"
                           "  natron-merge diff [--yaml] <before> <after>\n"
                           "  natron-merge merge [--scope template:<key>|all] <base> <mine> <theirs> -o <out>\n"
                           "  natron-merge git-driver <base> <current> <other>\n"
                           "  natron-merge textconv <file>\n"
                           "\n"
                           "diff exits 0 when the projects are the same, 1 when they differ; merge and\n"
                           "git-driver exit 0 for a clean merge and 1 when it left conflicts. Any error\n"
                           "exits 2.\n";

class UsageError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct LoadedProject {
    ProjectSerialization project;
    // The SHA-256 of the bundle's project.yaml, or empty for an empty file.
    std::string contentHash;
    // The folder the bundle's members sit under, without the trailing '/'.
    std::string root;
    BundleManifest manifest;
};

LoadedProject
load(const std::string& path)
{
    LoadedProject loaded;
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        throw std::runtime_error(path + ": no such file");
    }
    // git hands a merge driver an empty file for the ancestor of two independent additions.
    if (std::filesystem::is_regular_file(path, error) && (std::filesystem::file_size(path, error) == 0) && !error) {
        return loaded;
    }
    if (detectProjectFileKind(path) == ProjectFileKind::eLegacyXml) {
        throw std::runtime_error(path + ": an old-format project; natron-merge reads project bundles (.ntpz)");
    }
    const ProjectBundleReader reader(path);
    YamlWarnings warnings;
    loaded.project = reader.readProject(&warnings);
    loaded.contentHash = sha256Hex(reader.projectYaml());
    for (const std::string& warning : warnings) {
        std::cerr << "natron-merge: " << path << ": " << warning << "\n";
    }
    loaded.root = reader.root();
    if (!loaded.root.empty() && (loaded.root.back() == '/')) {
        loaded.root.pop_back();
    }
    loaded.manifest = reader.manifest();

    return loaded;
}

// The time now as ISO 8601 in UTC, as a template import records it.
std::string
utcNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc {};
    gmtime_r(&now, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);

    return std::string(text);
}

// Replaces path only once the whole bundle is written, so a failed merge leaves the old file.
void
save(const ProjectSerialization& project,
     const std::string& root,
     const BundleManifest& manifest,
     const std::string& path)
{
    ProjectBundleWriter writer;
    writer.setProject(project);
    writer.setRootFolder(root);
    writer.setManifest(manifest);
    const std::string temporary = path + ".natron-merge-tmp";
    try {
        writer.write(temporary);
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::string
bundleFolderFor(const std::string& path)
{
    std::string name = std::filesystem::path(path).filename().string();
    const std::string extension = ".ntpz";
    if (name.size() > extension.size()) {
        std::string tail = name.substr(name.size() - extension.size());
        std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (tail == extension) {
            name.resize(name.size() - extension.size());
        }
    }

    return name;
}

void
sortNodes(std::list<NodeSerializationPtr>* nodes)
{
    nodes->sort([](const NodeSerializationPtr& a, const NodeSerializationPtr& b) { return a->scriptName < b->scriptName; });
    for (NodeSerializationPtr& node : *nodes) {
        node = std::make_shared<NodeSerialization>(*node);
        node->knobs.sort([](const KnobSerialization& a, const KnobSerialization& b) { return a.name < b.name; });
        sortNodes(&node->children);
    }
}

std::string
pathOf(const std::list<NodeSerializationPtr>& nodes,
       const std::string& id,
       const std::string& prefix = std::string())
{
    for (const NodeSerializationPtr& node : nodes) {
        const std::string path = prefix.empty() ? node->scriptName : prefix + "." + node->scriptName;
        if (node->id == id) {
            return path;
        }
        const std::string inside = pathOf(node->children, id, path);
        if (!inside.empty()) {
            return inside;
        }
    }

    return std::string();
}

std::string
describeConflict(const MergeConflict& conflict,
                 const ProjectSerialization& base,
                 const ProjectSerialization& merged)
{
    std::string where = conflict.nodeId.empty() ? std::string() : pathOf(merged.nodes.nodes, conflict.nodeId);
    if (where.empty() && !conflict.baseNodeId.empty()) {
        where = pathOf(base.nodes.nodes, conflict.baseNodeId);
    }
    if (where.empty()) {
        where = conflict.baseNodeId.empty() && conflict.nodeId.empty() ? std::string("Project") : std::string("?");
    }
    if (!conflict.item.empty()) {
        where += " " + conflict.item;
    }
    if (!conflict.knob.empty()) {
        where += "." + conflict.knob;
        if (conflict.dimension == kMergeConflictAllDimensions) {
            where += "[all]";
        } else if (conflict.dimension >= 0) {
            where += "[" + std::to_string(conflict.dimension) + "]";
        }
    }
    if (!conflict.input.empty()) {
        where += " input " + conflict.input;
    }

    return std::string(mergeConflictKindName(conflict.kind)) + ": " + where;
}

// Writes the merge of base, mine and theirs to out, keeping mine's conflicts from merges of other
// template imports. Returns the exit status.
int
runMerge(const std::string& basePath,
         const std::string& minePath,
         const std::string& theirsPath,
         const std::string& outPath,
         const std::optional<std::string>& templateKey,
         const std::string& root)
{
    LoadedProject base = load(basePath);
    const LoadedProject mine = load(minePath);
    LoadedProject theirs = load(theirsPath);
    MergePlan plan;
    MergeOptions options;
    if (templateKey) {
        // A template's own imports and conflicts are not the shot's, as when updating in Natron.
        theirs.project.templates.clear();
        theirs.project.mergeConflicts.clear();
        // Published versions hold the template node, which is not the template's content.
        eraseTemplateNodes(&base.project.nodes.nodes);
        eraseTemplateNodes(&theirs.project.nodes.nodes);
        const bool imported = std::any_of(mine.project.templates.begin(), mine.project.templates.end(), [&](const TemplateImportSerialization& entry) { return entry.key == *templateKey; });
        if (!imported) {
            throw std::runtime_error(minePath + ": no template import has the key " + *templateKey);
        }
        plan = planMerge(base.project, mine.project, theirs.project, *templateKey);
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(theirsPath, error);
        options.templatePath = error ? theirsPath : absolute.lexically_normal().string();
        options.templateContentHash = theirs.contentHash;
        options.templateImportedAt = utcNow();
    } else {
        plan = planProjectMerge(base.project, mine.project, theirs.project);
        if (!base.project.nodes.nodes.empty() && plan.baseToMine.baseToOther.empty()) {
            std::cerr << "natron-merge: no node of " << basePath << " is in " << minePath << "; to update a template import, pass --scope template:<key>\n";
        }
    }
    MergeResult result = threeWayMerge(base.project, mine.project, theirs.project, plan, options);
    if (!result.error.empty()) {
        throw std::runtime_error(theirsPath + ": " + result.error);
    }
    result.merged.mergeConflicts = conflictsToSave(base.project, mine.project, theirs.project, plan, result);
    save(result.merged, root.empty() ? mine.root : root, mine.manifest, outPath);
    for (const MergeConflict& conflict : result.conflicts) {
        std::cerr << "natron-merge: conflict: " << describeConflict(conflict, base.project, result.merged) << "\n";
    }
    if (!result.conflicts.empty()) {
        std::cerr << "natron-merge: " << result.conflicts.size() << ((result.conflicts.size() == 1) ? " conflict" : " conflicts") << " kept mine; they are saved in " << outPath << "\n";

        return kExitConflicts;
    }

    return kExitClean;
}

int
diffCommand(const std::vector<std::string>& args)
{
    bool yaml = false;
    std::vector<std::string> files;
    for (const std::string& arg : args) {
        if (arg == "--yaml") {
            yaml = true;
        } else if (!arg.empty() && (arg[0] == '-')) {
            throw UsageError("diff: unknown option " + arg);
        } else {
            files.push_back(arg);
        }
    }
    if (files.size() != 2) {
        throw UsageError("diff takes two files");
    }
    const ProjectSerialization before = editableState(load(files[0]).project);
    const ProjectSerialization after = editableState(load(files[1]).project);
    const std::vector<GraphOp> ops = diffSnapshots(before, after);
    if (yaml) {
        std::cout << (ops.empty() ? std::string() : encodeGraphOpsYaml(ops));
    } else {
        std::cout << MergeTool::printDiff(before, after, ops);
    }

    return ops.empty() ? kExitClean : kExitDiffers;
}

int
mergeCommand(const std::vector<std::string>& args)
{
    std::optional<std::string> templateKey;
    std::string out;
    std::vector<std::string> files;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if ((arg == "-o") || (arg == "--scope")) {
            if (i + 1 >= args.size()) {
                throw UsageError("merge: " + arg + " needs a value");
            }
            const std::string& value = args[++i];
            if (arg == "-o") {
                out = value;
            } else if (value == "all") {
                templateKey.reset();
            } else if (value.rfind("template:", 0) == 0) {
                templateKey = value.substr(9);
                if (!isValidTemplateKey(*templateKey)) {
                    throw UsageError("merge: \"" + *templateKey + "\" is not a template key (letters, digits and underscores)");
                }
            } else {
                throw UsageError("merge: --scope is template:<key> or all, not " + value);
            }
        } else if (!arg.empty() && (arg[0] == '-')) {
            throw UsageError("merge: unknown option " + arg);
        } else {
            files.push_back(arg);
        }
    }
    if ((files.size() != 3) || out.empty()) {
        throw UsageError("merge takes three files and -o <out>");
    }

    return runMerge(files[0], files[1], files[2], out, templateKey, bundleFolderFor(out));
}

int
gitDriverCommand(const std::vector<std::string>& args)
{
    if (args.size() != 3) {
        throw UsageError("git-driver takes %O %A %B");
    }

    return runMerge(args[0], args[1], args[2], args[1], std::nullopt, std::string());
}

int
textconvCommand(const std::vector<std::string>& args)
{
    if (args.size() != 1) {
        throw UsageError("textconv takes one file");
    }
    ProjectSerialization project = load(args[0]).project;
    sortNodes(&project.nodes.nodes);
    InlineBlobSink sink;
    std::cout << encodeProjectYaml(project, sink);

    return kExitClean;
}

} // namespace

int
main(int argc,
     char* argv[])
{
    std::vector<std::string> args(argv + std::min(argc, 2), argv + argc);
    const std::string command = (argc > 1) ? argv[1] : std::string();
    try {
        if ((command == "-h") || (command == "--help")) {
            std::cout << kUsage;

            return kExitClean;
        }
        if (command == "diff") {
            return diffCommand(args);
        }
        if (command == "merge") {
            return mergeCommand(args);
        }
        if (command == "git-driver") {
            return gitDriverCommand(args);
        }
        if (command == "textconv") {
            return textconvCommand(args);
        }
        throw UsageError(command.empty() ? std::string("no command") : "unknown command " + command);
    } catch (const UsageError& e) {
        std::cerr << "natron-merge: " << e.what() << "\n"
                  << kUsage;
    } catch (const std::exception& e) {
        std::cerr << "natron-merge: " << e.what() << "\n";
    }

    return kExitError;
}
