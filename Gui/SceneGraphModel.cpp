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

#include "SceneGraphModel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QtCore/QElapsedTimer>
#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QIcon>

#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneKindWalk.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneTeardown.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

#ifdef emit
#define NATRON_SCENEGRAPHMODEL_RESTORE_EMIT
#undef emit
#endif
#include <pxr/usd/usdGeom/modelAPI.h>

#include "Engine/Nodes/Scene/ScenePredicates.h"
#ifdef NATRON_SCENEGRAPHMODEL_RESTORE_EMIT
#undef NATRON_SCENEGRAPHMODEL_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// Past this many resynced paths between two refreshes, everything read is read again.
const std::size_t kMaxPendingPaths = 4096;

// Above this many children, finding a child by name goes through a map.
const std::size_t kRowMapThreshold = 32;

// Unfiltered, as a run of prims the walk rejects is as long to skip as a run it lists: the
// predicate is applied by the caller, where the budget can be checked between prims.
UsdPrim
nextPrimOfWalk(const UsdPrim& prim,
               bool descend)
{
    static const Usd_PrimFlagsPredicate allPrims = UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate);

    if (descend) {
        const UsdPrimSiblingRange children = prim.GetFilteredChildren(allPrims);
        if (!children.empty()) {
            return *children.begin();
        }
    }
    for (UsdPrim up = prim; up && !up.IsPseudoRoot(); up = up.GetParent()) {
        const UsdPrim sibling = up.GetFilteredNextSibling(allPrims);
        if (sibling) {
            return sibling;
        }
    }

    return UsdPrim();
}

struct PrimData {
    QString name;
    QString type;
    QString kind;
    bool imageable = false;
    // The purpose the prim authors, or the fallback when it authors none: its purpose without a
    // parent, which is all a read of the prim alone can know.
    TfToken ownPurpose;
    bool purposeAuthored = false;
    bool hasChildren = false;
    // None, Unloaded or Loaded: the other states belong to the model's requests, not the prim.
    SceneGraphModel::PayloadStateEnum payload = SceneGraphModel::ePayloadStateNone;
    bool unknownBounds = false;

    bool operator==(const PrimData& other) const
    {
        return (name == other.name) && (type == other.type) && (kind == other.kind) && (imageable == other.imageable) && (ownPurpose == other.ownPurpose) && (purposeAuthored == other.purposeAuthored) && (hasChildren == other.hasChildren) && (payload == other.payload) && (unknownBounds == other.unknownBounds);
    }
};

// A row's computed purpose, and the prim it is authored on, empty for the fallback.
struct PurposeCell {
    TfToken purpose;
    // Whether rows below that author none take it.
    bool inheritable = false;
    SdfPath origin;

    bool operator==(const PurposeCell& other) const
    {
        return (purpose == other.purpose) && (inheritable == other.inheritable) && (origin == other.origin);
    }
};

/**
 * @brief UsdGeomImageable::ComputePurposeInfo(parentInfo) for the prim at path from what was read of
 * it and its parent's cell, without the stage: so a row's purpose costs one step down from its
 * parent's, and an edit of an ancestor's purpose is carried down the rows read without reading them
 * again. Null parent for a top-level prim.
 **/
PurposeCell
derivePurpose(const PrimData& data,
              const SdfPath& path,
              const PurposeCell* parent)
{
    PurposeCell cell;

    if (data.purposeAuthored) {
        cell.purpose = data.ownPurpose;
        cell.inheritable = true;
        cell.origin = path;
    } else if (parent && parent->inheritable) {
        cell = *parent;
    } else {
        cell.purpose = data.ownPurpose;
    }

    return cell;
}

PrimData
readPrimData(const UsdPrim& prim)
{
    PrimData data;

    if (!prim) {
        return data;
    }
    data.name = prim.IsPseudoRoot() ? QString::fromUtf8("/") : QString::fromStdString(prim.GetName().GetString());
    data.type = QString::fromStdString(prim.GetTypeName().GetString());
    TfToken kind;
    if (prim.GetKind(&kind)) {
        data.kind = QString::fromStdString(kind.GetString());
    }
    if (!prim.IsPseudoRoot()) {
        // USD reads purpose from imageable prims only; any other passes its parent's through.
        data.imageable = prim.IsA<UsdGeomImageable>();
        const UsdGeomImageable::PurposeInfo own = UsdGeomImageable(prim).ComputePurposeInfo(UsdGeomImageable::PurposeInfo());
        data.ownPurpose = own.purpose;
        data.purposeAuthored = own.isInheritable;
    }
    if (prim.HasAuthoredPayloads()) {
        data.payload = prim.IsLoaded() ? SceneGraphModel::ePayloadStateLoaded : SceneGraphModel::ePayloadStateUnloaded;
    }
    if (data.payload == SceneGraphModel::ePayloadStateUnloaded) {
        // UsdGeomBBoxCache reads the hint of a model alone.
        const UsdAttribute hint = prim.IsModel() ? UsdGeomModelAPI(prim).GetExtentsHintAttr() : UsdAttribute();
        data.unknownBounds = !(hint && hint.HasAuthoredValue());
    }
    data.hasChildren = !prim.GetFilteredChildren(childPredicate()).empty() || (data.payload == SceneGraphModel::ePayloadStateUnloaded);

    return data;
}

QVariant
columnHeader(int section,
             Qt::Orientation orientation,
             int role)
{
    if ((orientation != Qt::Horizontal) || (role != Qt::DisplayRole)) {
        return QVariant();
    }
    switch (section) {
    case SceneGraphModel::eColumnName:
        return SceneGraphModel::tr("Name");
    case SceneGraphModel::eColumnType:
        return SceneGraphModel::tr("Type");
    case SceneGraphModel::eColumnKind:
        return SceneGraphModel::tr("Kind");
    case SceneGraphModel::eColumnPurpose:
        return SceneGraphModel::tr("Purpose");
    case SceneGraphModel::eColumnDrawState:
        return SceneGraphModel::tr("Draw State");
    default:
        return QVariant();
    }
}

// Collects what the stage's edits resynced, on whichever thread edits the stage.
class StageListener
    : public TfWeakBase {
public:
    explicit StageListener(std::atomic<U64>* noticeCount)
        : noticeCount(noticeCount)
    {
    }

    void onObjectsChanged(const UsdNotice::ObjectsChanged& notice)
    {
        ++*noticeCount;
        std::lock_guard<std::mutex> lock(mutex);

        for (const SdfPath& path : notice.GetResyncedPaths()) {
            if (overflow) {
                break;
            }
            if (path.IsAbsoluteRootOrPrimPath()) {
                resynced.push_back(path);
            } else if (isPurposePath(path)) {
                infoOnly.push_back(path.GetPrimPath());
            }
        }
        for (const SdfPath& path : notice.GetChangedInfoOnlyPaths()) {
            if (overflow) {
                break;
            }
            if (path.IsPrimPath()) {
                infoOnly.push_back(path);
            } else if (isPurposePath(path)) {
                infoOnly.push_back(path.GetPrimPath());
            }
        }
        if ((resynced.size() + infoOnly.size()) > kMaxPendingPaths) {
            overflow = true;
            resynced.clear();
            infoOnly.clear();
        }
    }

    // Purpose is an attribute, whose edits name its property path rather than the prim's.
    static bool isPurposePath(const SdfPath& path)
    {
        return path.IsPrimPropertyPath() && (path.GetNameToken() == UsdGeomTokens->purpose);
    }

    bool hasPending()
    {
        std::lock_guard<std::mutex> lock(mutex);

        return overflow || !resynced.empty() || !infoOnly.empty();
    }

    std::atomic<U64>* noticeCount;
    std::mutex mutex;
    SdfPathVector resynced;
    SdfPathVector infoOnly;
    bool overflow = false;
};

// Never changed once shared: an item given new names gets a new list, so that a copy of the pointer
// tells whether the names it was read from are still the item's.
typedef std::shared_ptr<const TfTokenVector> NamesPtr;

const NamesPtr&
noNames()
{
    static const NamesPtr none = std::make_shared<const TfTokenVector>();

    return none;
}

struct SceneGraphModelItem {
    SceneGraphModelItem* parent = nullptr;
    int row = 0;
    SdfPath path;
    TfToken nameToken;
    QString pathString;
    PrimData data;
    // From data and the parent's, kept up to date by derivePurposes().
    PurposeCell purpose;
    // Whether childNames holds the prim's children; children are the first of them.
    bool namesRead = false;
    NamesPtr childNames = noNames();
    std::vector<std::unique_ptr<SceneGraphModelItem>> children;
    // Shown after the children while the payload loads. It has no prim, so it is kept out of them.
    std::unique_ptr<SceneGraphModelItem> loadingRow;
    std::unordered_map<TfToken, int, TfToken::HashFunctor> rowOf;
    bool rowOfValid = false;

    SceneGraphModelItem* findChild(const TfToken& name)
    {
        if (children.size() <= kRowMapThreshold) {
            for (const std::unique_ptr<SceneGraphModelItem>& child : children) {
                if (child->nameToken == name) {
                    return child.get();
                }
            }

            return nullptr;
        }
        if (!rowOfValid) {
            rowOf.clear();
            for (std::size_t i = 0; i < children.size(); ++i) {
                rowOf[children[i]->nameToken] = (int)i;
            }
            rowOfValid = true;
        }
        std::unordered_map<TfToken, int, TfToken::HashFunctor>::const_iterator found = rowOf.find(name);

        return (found == rowOf.end()) ? nullptr : children[found->second].get();
    }

    void renumber()
    {
        for (std::size_t i = 0; i < children.size(); ++i) {
            children[i]->row = (int)i;
        }
        placeLoadingRow();
        rowOfValid = false;
    }

    void placeLoadingRow()
    {
        if (loadingRow) {
            loadingRow->row = (int)children.size();
        }
    }
};

typedef SceneGraphModelItem Item;

std::unique_ptr<Item>
makeItem(const SdfPath& parentPath,
         const TfToken& name,
         const PrimData& data)
{
    std::unique_ptr<Item> item(new Item);

    item->path = parentPath.AppendChild(name);
    item->nameToken = name;
    item->pathString = QString::fromStdString(item->path.GetString());
    item->data = data;

    return item;
}

// What one item reads from the stage, gathered under the stage's lock and applied after it.
struct ItemUpdate {
    Item* item = nullptr;
    PrimData data;
    bool readChildren = false;
    // Every child is replaced: the kept ones changed order.
    bool replaceChildren = false;
    NamesPtr names;
    // How many of names are loaded after the update.
    std::size_t loadedCount = 0;
    // By row before the update, whether the child stays.
    std::vector<bool> kept;
    // The rows of the names among the first loadedCount that no kept child has, in their order.
    std::vector<std::unique_ptr<Item>> newChildren;
};

// What gathering an item's update needs of it, copied so that the stage can be read away from the
// tree: the children are the first childCount of names.
struct GatherNode {
    SdfPath path;
    bool namesRead = false;
    SceneGraphModel::PayloadStateEnum payload = SceneGraphModel::ePayloadStateNone;
    NamesPtr names = noNames();
    std::size_t childCount = 0;
    // The prims of the kept children are read again too; otherwise only the ones in refreshNames.
    bool refreshKept = false;
    std::unordered_set<TfToken, TfToken::HashFunctor> refreshNames;

    // Whether item is still what this was copied from, so that an update gathered from it applies.
    bool matches(const Item& item) const
    {
        return (item.namesRead == namesRead) && (item.data.payload == payload) && (item.children.size() == childCount) && (item.childNames == names);
    }
};

struct GatheredUpdate {
    SdfPath path;
    // What the update was gathered from; null for a prim whose children are not read again.
    const GatherNode* node = nullptr;
    ItemUpdate update;
};

typedef std::unordered_set<SdfPath, SdfPath::Hash> PathSet;

bool
isUnder(const SdfPath& path,
        const PathSet& gone)
{
    for (SdfPath up = path; !up.IsEmpty() && !up.IsAbsoluteRootPath(); up = up.GetParentPath()) {
        if (gone.count(up)) {
            return true;
        }
    }

    return false;
}

// Reads node's prim and, when its children were read, them again, the rows the update adds
// included. Adds the children it removes or replaces to gone, and skips a node under one: its row
// is not there to update. Needs the stage's lock.
void
gatherUpdate(const UsdStageRefPtr& stage,
             const GatherNode& node,
             const PathSet& nodePaths,
             PathSet* gone,
             std::vector<GatheredUpdate>* updates)
{
    if (isUnder(node.path, *gone)) {
        return;
    }
    const UsdPrim prim = stage->GetPrimAtPath(node.path);
    GatheredUpdate gathered;

    gathered.path = node.path;
    gathered.node = &node;
    ItemUpdate& update = gathered.update;
    update.data = readPrimData(prim);
    if (!node.namesRead) {
        updates->push_back(std::move(gathered));

        return;
    }
    update.readChildren = true;
    std::shared_ptr<TfTokenVector> names = std::make_shared<TfTokenVector>();
    if (prim) {
        *names = prim.GetFilteredChildrenNames(childPredicate());
    }
    std::unordered_map<TfToken, std::size_t, TfToken::HashFunctor> newPosition;
    for (std::size_t i = 0; i < names->size(); ++i) {
        newPosition[(*names)[i]] = i;
    }
    const TfTokenVector& oldNames = *node.names;
    const std::size_t childCount = std::min(node.childCount, oldNames.size());
    update.kept.assign(childCount, false);
    std::unordered_set<TfToken, TfToken::HashFunctor> keptNames;
    std::size_t lastKeptPosition = 0;
    bool anyKept = false;
    for (std::size_t j = 0; j < childCount; ++j) {
        std::unordered_map<TfToken, std::size_t, TfToken::HashFunctor>::const_iterator found = newPosition.find(oldNames[j]);
        if (found == newPosition.end()) {
            continue;
        }
        if (anyKept && (found->second <= lastKeptPosition)) {
            update.replaceChildren = true;
        }
        lastKeptPosition = found->second;
        anyKept = true;
        update.kept[j] = true;
        keptNames.insert(oldNames[j]);
    }
    if (update.replaceChildren) {
        update.kept.assign(childCount, false);
        keptNames.clear();
    }
    for (std::size_t j = 0; j < childCount; ++j) {
        if (!update.kept[j]) {
            gone->insert(node.path.AppendChild(oldNames[j]));
        }
    }
    update.loadedCount = std::min(childCount, names->size());
    if (anyKept && !update.replaceChildren) {
        update.loadedCount = std::max(update.loadedCount, lastKeptPosition + 1);
    }
    // A view fetches when a row opens, and this row may have opened before its payload's
    // children existed.
    if ((node.payload == SceneGraphModel::ePayloadStateUnloaded) && (update.data.payload != SceneGraphModel::ePayloadStateUnloaded)) {
        update.loadedCount = std::max(update.loadedCount, std::min(names->size(), (std::size_t)SceneGraphModel::kFetchBatchSize));
    }
    for (std::size_t i = 0; i < update.loadedCount; ++i) {
        const TfToken& name = (*names)[i];
        if (keptNames.find(name) == keptNames.end()) {
            update.newChildren.push_back(makeItem(node.path, name, readPrimData(stage->GetPrimAtPath(node.path.AppendChild(name)))));
        }
    }
    update.names = std::move(names);
    updates->push_back(std::move(gathered));
    for (const TfToken& name : keptNames) {
        if (!node.refreshKept && (node.refreshNames.find(name) == node.refreshNames.end())) {
            continue;
        }
        const SdfPath childPath = node.path.AppendChild(name);
        if (nodePaths.count(childPath)) {
            continue;
        }
        GatheredUpdate child;
        child.path = childPath;
        child.update.data = readPrimData(stage->GetPrimAtPath(childPath));
        updates->push_back(std::move(child));
    }
}

// One step of a read of rows: the children of path from row from on, through upTo or a batch,
// and through wanted when it is one of them.
struct FetchLevel {
    SdfPath path;
    // The item's names when it had read them; null has the worker read them.
    NamesPtr names;
    std::size_t from = 0;
    std::size_t upTo = 0;
    // The child the next level is under; empty on the last.
    TfToken wanted;
    // Filled by gather().
    bool done = false;
    std::vector<std::unique_ptr<Item>> rows;
};

// The edits read in one pass, the items to gather again and the prims whose own data changed, or
// a read of rows.
struct GatherRequest {
    SceneGraphStageSourcePtr source;
    // The stage the items were read from, compared, never dereferenced.
    const UsdStage* stage = nullptr;
    int resetCount = 0;
    std::vector<GatherNode> nodes;
    SdfPathVector infoOnly;
    // A read of rows rather than of edits: down from the first level, each under the one before.
    std::vector<FetchLevel> levels;
    // The path the rows are read to reach; empty for a fetchMore().
    SdfPath target;
    // Filled by gather().
    std::vector<GatheredUpdate> updates;
    // The stage read was not the one the items were read from: nothing was gathered.
    bool otherStage = false;
    // Set when nodes holds a copy of the root and every named item, and resynced and infoOnly the
    // paths the listener queued, which prepare() turns into the items to gather again.
    bool unprepared = false;
    bool all = false;
    SdfPathVector resynced;

    bool isEmpty() const
    {
        return nodes.empty() && infoOnly.empty() && levels.empty();
    }

    // Needs no lock: it reads none of the stage.
    void prepare()
    {
        if (!unprepared) {
            return;
        }
        unprepared = false;
        std::vector<GatherNode> named;
        named.swap(nodes);
        SdfPathVector queuedInfo;
        queuedInfo.swap(infoOnly);
        SdfPath::RemoveDescendentPaths(&resynced);
        for (const SdfPath& path : resynced) {
            if (path == SdfPath::AbsoluteRootPath()) {
                all = true;
                break;
            }
        }
        if (all) {
            for (GatherNode& node : named) {
                node.refreshKept = true;
            }
            nodes.swap(named);
            resynced.clear();

            return;
        }
        std::unordered_map<SdfPath, std::size_t, SdfPath::Hash> namedIndex;
        namedIndex.reserve(named.size());
        for (std::size_t i = 0; i < named.size(); ++i) {
            namedIndex.emplace(named[i].path, i);
        }
        // By named item, the names of the children it has rows for, made when first asked.
        std::unordered_map<std::size_t, std::unordered_set<TfToken, TfToken::HashFunctor>> rowNames;
        // Whether the tree has a row for path: the root, or a row among its named parent's.
        const auto hasRow = [&](const SdfPath& path) {
            if (path.IsAbsoluteRootPath()) {
                return true;
            }
            const std::unordered_map<SdfPath, std::size_t, SdfPath::Hash>::const_iterator parent = namedIndex.find(path.GetParentPath());
            if (parent == namedIndex.end()) {
                return false;
            }
            const GatherNode& node = named[parent->second];
            const std::size_t count = std::min(node.childCount, node.names->size());
            std::unordered_map<std::size_t, std::unordered_set<TfToken, TfToken::HashFunctor>>::iterator found = rowNames.find(parent->second);
            if (found == rowNames.end()) {
                found = rowNames.emplace(parent->second, std::unordered_set<TfToken, TfToken::HashFunctor>(node.names->begin(), node.names->begin() + (std::ptrdiff_t)count)).first;
            }

            return found->second.count(path.GetNameToken()) > 0;
        };
        // Each resynced path changes its parent's children and everything under it.
        std::map<SdfPath, GatherNode> byPath;
        SdfPathVector unnamedParents;
        for (const SdfPath& path : resynced) {
            const SdfPath parentPath = path.GetParentPath();
            const std::unordered_map<SdfPath, std::size_t, SdfPath::Hash>::const_iterator parent = namedIndex.find(parentPath);
            if (parent == namedIndex.end()) {
                // A row whose children were never read: only its own data can have changed.
                if (hasRow(parentPath)) {
                    unnamedParents.push_back(parentPath);
                }
                continue;
            }
            std::map<SdfPath, GatherNode>::iterator found = byPath.find(parentPath);
            if (found == byPath.end()) {
                found = byPath.emplace(parentPath, named[parent->second]).first;
                found->second.refreshKept = false;
            }
            found->second.refreshNames.insert(path.GetNameToken());
        }
        if (!resynced.empty()) {
            for (const GatherNode& node : named) {
                if (SdfPathFindLongestPrefix(resynced.begin(), resynced.end(), node.path) == resynced.end()) {
                    continue;
                }
                std::map<SdfPath, GatherNode>::iterator found = byPath.find(node.path);
                if (found == byPath.end()) {
                    byPath.emplace(node.path, node).first->second.refreshKept = true;
                } else {
                    found->second.refreshKept = true;
                }
            }
        }
        nodes.reserve(byPath.size());
        for (std::pair<const SdfPath, GatherNode>& entry : byPath) {
            nodes.push_back(std::move(entry.second));
        }
        std::sort(unnamedParents.begin(), unnamedParents.end());
        unnamedParents.erase(std::unique(unnamedParents.begin(), unnamedParents.end()), unnamedParents.end());
        for (const SdfPath& path : queuedInfo) {
            if (SdfPathFindLongestPrefix(resynced.begin(), resynced.end(), path) != resynced.end()) {
                continue;
            }
            if (!path.IsAbsoluteRootPath() && hasRow(path) && !std::binary_search(unnamedParents.begin(), unnamedParents.end(), path)) {
                infoOnly.push_back(path);
            }
        }
        infoOnly.insert(infoOnly.end(), unnamedParents.begin(), unnamedParents.end());
        resynced.clear();
    }

    bool isFetch() const
    {
        return !levels.empty();
    }

    // On the main thread, which the source is released on; the rest may be large.
    static void retire(std::shared_ptr<GatherRequest> request)
    {
        if (request) {
            request->source.reset();
            SceneTeardown::release(std::move(request));
        }
    }

    // Needs the stage's lock.
    void gather(const UsdStageRefPtr& usdStage)
    {
        if (get_pointer(usdStage) != stage) {
            otherStage = true;

            return;
        }
        if (isFetch()) {
            fetch(usdStage);

            return;
        }
        // A parent before its children, so that an update knows whether its row is still there.
        std::sort(nodes.begin(), nodes.end(), [](const GatherNode& a, const GatherNode& b) {
            const std::size_t aCount = a.path.GetPathElementCount();
            const std::size_t bCount = b.path.GetPathElementCount();

            return (aCount != bCount) ? (aCount < bCount) : (a.path < b.path);
        });
        PathSet nodePaths;
        for (const GatherNode& node : nodes) {
            nodePaths.insert(node.path);
        }
        PathSet gone;
        for (const GatherNode& node : nodes) {
            gatherUpdate(usdStage, node, nodePaths, &gone, &updates);
        }
        for (const SdfPath& path : infoOnly) {
            GatheredUpdate gathered;
            gathered.path = path;
            gathered.update.data = readPrimData(usdStage->GetPrimAtPath(path));
            updates.push_back(std::move(gathered));
        }
    }

    void fetch(const UsdStageRefPtr& usdStage)
    {
        for (FetchLevel& level : levels) {
            if (!level.names) {
                const UsdPrim prim = usdStage->GetPrimAtPath(level.path);
                level.names = prim ? std::make_shared<const TfTokenVector>(prim.GetFilteredChildrenNames(childPredicate())) : noNames();
            }
            const TfTokenVector& names = *level.names;
            std::size_t through = 0;
            bool found = level.wanted.IsEmpty();
            if (!found) {
                const TfTokenVector::const_iterator it = std::find(names.begin(), names.end(), level.wanted);
                found = (it != names.end());
                through = found ? (std::size_t)(it - names.begin()) + 1 : 0;
            }
            const std::size_t end = std::min(names.size(), std::max(std::max(level.upTo, through), level.from + SceneGraphModel::kFetchBatchSize));
            for (std::size_t i = level.from; i < end; ++i) {
                level.rows.push_back(makeItem(level.path, names[i], readPrimData(usdStage->GetPrimAtPath(level.path.AppendChild(names[i])))));
            }
            level.done = true;
            if (!found) {
                return;
            }
        }
    }
};

typedef std::shared_ptr<GatherRequest> GatherRequestPtr;

// Gathers requests one after the other on a thread of its own, and hands each back to deliver(),
// which runs on that thread and must post it to the main thread.
class GatherWorker {
public:
    explicit GatherWorker(const std::function<void(GatherRequestPtr)>& deliver)
        : _deliver(deliver)
    {
    }

    ~GatherWorker()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _quit = true;
        }
        _wake.notify_all();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    // Requests queued for an earlier reset are dropped: their items are gone.
    void post(GatherRequestPtr request)
    {
        std::vector<GatherRequestPtr> dropped;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (GatherRequestPtr& queued : _queue) {
                if (queued->resetCount != request->resetCount) {
                    dropped.push_back(std::move(queued));
                }
            }
            _queue.erase(std::remove(_queue.begin(), _queue.end(), GatherRequestPtr()), _queue.end());
            _queue.push_back(std::move(request));
            if (!_thread.joinable()) {
                _thread = std::thread([this]() { run(); });
            }
        }
        _wake.notify_one();
        for (GatherRequestPtr& request : dropped) {
            GatherRequest::retire(std::move(request));
        }
    }

private:
    void run()
    {
        for (;;) {
            GatherRequestPtr request;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _wake.wait(lock, [this]() { return _quit || !_queue.empty(); });
                if (_quit) {
                    return;
                }
                request = std::move(_queue.front());
                _queue.pop_front();
            }
            GatherRequest* gathering = request.get();
            gathering->prepare();
            gathering->source->readStage([gathering](const SceneStage* sceneStage) {
                if (sceneStage) {
                    gathering->gather(sceneStage->getStage());
                } else {
                    gathering->otherStage = true;
                }
            });
            // Handed over whole, so that the source it holds is released on the main thread.
            _deliver(std::move(request));
        }
    }

    std::function<void(GatherRequestPtr)> _deliver;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<GatherRequestPtr> _queue;
    bool _quit = false;
    std::thread _thread;
};

} // anon namespace

struct SceneGraphPathRows {
    struct Parent {
        SdfPath path;
        std::unordered_set<TfToken, TfToken::HashFunctor> wanted;
        bool namesRead = false;
        TfTokenVector names;
        // How many of names have to be read to reach the last one wanted.
        std::size_t rowCount = 0;
        std::vector<std::unique_ptr<Item>> rows;
    };

    const UsdStage* stage = nullptr;
    // A parent before its children: its rows are what the children's parent is found in.
    std::vector<Parent> parents;
};

struct SceneGraphModel::Implementation {
    SceneGraphModel* model;
    SceneGraphStageSourcePtr source;
    Item root;
    UsdStageWeakPtr listenedStage;
    std::atomic<U64> stageNoticeCount { 0 };
    std::unique_ptr<StageListener> listener;
    TfNotice::Key key;
    U64 lastEditCount = 0;
    int itemCount = 0;
    int resetCount = 0;
    QSet<QString> changedHere;
    QSet<QString> loadingPaths;
    // By prim path, the USD error.
    QHash<QString, QString> loadFailures;
    // Reads that found the stage busy: children by parent, up to how many, and the load's outcome.
    std::map<SdfPath, std::size_t> owedFetches;
    bool settleOwed = false;
    // Set while deferred: what refresh(), settleLoads() and refreshDrawStates() were asked for.
    bool deferred = false;
    bool stale = false;
    bool settleStale = false;
    bool drawStatesStale = false;
    // The paths of ended loads whose rows are redrawn a slice at a time, and the next of them.
    std::vector<QString> settleQueue;
    std::size_t settleNext = 0;
    QTimer* settleTimer = nullptr;
    int settleSliceCount = 0;
    // Whether edits are gathered on the worker rather than under the main thread's read.
    bool readsOnWorker = false;
    std::unique_ptr<GatherWorker> worker;
    // The reset the gather on the worker was asked for in, or -1 when none is.
    int gatheringReset = -1;
    // Paths whose update was gathered from rows that changed meanwhile, gathered again next.
    SdfPathVector carried;
    // A gather found another stage than the rows were read from: every row is read again.
    bool carryAll = false;
    // The items besides the root whose names are read, which are the ones an edit can add rows to.
    std::unordered_set<Item*> namedItems;
    // The gathered edits being applied a slice at a time, and the next of their updates.
    GatherRequestPtr applying;
    std::size_t applyNext = 0;
    QTimer* applyTimer = nullptr;
    int applySliceMs = 8;
    int applySliceCount = 0;
    // Reads of rows on the worker: how many, how many by the path of their first level, and the
    // paths they were asked for to reach.
    int fetchesInFlight = 0;
    std::map<SdfPath, int> fetchingUnder;
    std::set<SdfPath> fetchTargets;
    struct RetiredListener {
        SceneGraphStageSourcePtr source;
        std::unique_ptr<StageListener> listener;
        TfNotice::Key key;

        // Needs the stage's lock.
        void revoke()
        {
            if (key.IsValid()) {
                TfNotice::Revoke(key);
            }
            listener.reset();
        }
    };
    std::vector<RetiredListener> retiredListeners;
    DrawStatesReader drawStatesReader;
    DrawStateWriter drawStateWriter;
    struct DrawCell {
        int state = 0;
        bool inherited = false;
        int origin = (int)eSceneDrawOriginDefault;
        QString originNode;
    };
    // By path, what the viewer's draw states resolved to since they last changed.
    mutable QHash<QString, DrawCell> drawCells;
    // Set while the model changes rows: a slot connected to the change signals must not fetch,
    // since children are not yet a prefix of childNames and a nested insert would land inside
    // the change in flight.
    int changing = 0;

    struct ChangeScope {
        explicit ChangeScope(Implementation* imp)
            : imp(imp)
        {
            ++imp->changing;
        }

        ~ChangeScope()
        {
            --imp->changing;
        }

        Implementation* imp;
    };

    explicit Implementation(SceneGraphModel* model)
        : model(model)
    {
        root.path = SdfPath::AbsoluteRootPath();
        root.pathString = QString::fromUtf8("/");
        applyTimer = new QTimer(model);
        applyTimer->setSingleShot(true);
        applyTimer->setInterval(0);
        QObject::connect(applyTimer, &QTimer::timeout, model, [this]() { applySlice(); });
        settleTimer = new QTimer(model);
        settleTimer->setSingleShot(true);
        settleTimer->setInterval(0);
        QObject::connect(settleTimer, &QTimer::timeout, model, [this]() { settleSlice(); });
    }

    bool hasPendingSettle() const
    {
        return settleNext < settleQueue.size();
    }

    // Redraws the rows of settleQueue for up to applySliceMs, at least one, and has the event loop
    // redraw the rest after: a load of a whole city ends thousands of rows' loading at once.
    void settleSlice()
    {
        if (!hasPendingSettle()) {
            return;
        }
        ++settleSliceCount;
        QElapsedTimer timer;
        timer.start();
        while (hasPendingSettle()) {
            // A copy: a slot the changes reach may settle again, or reset the model.
            const QString path = settleQueue[settleNext++];
            if (Item* item = findItem(path)) {
                if (item != &root) {
                    // Asked for again since the load ended.
                    if (!loadingPaths.contains(path)) {
                        removeLoadingRow(item);
                    }
                    emitRowChanged(item);
                }
            }
            if (timer.elapsed() >= applySliceMs) {
                break;
            }
        }
        if (hasPendingSettle()) {
            settleTimer->start();

            return;
        }
        clearSettleQueue();
    }

    void clearSettleQueue()
    {
        settleTimer->stop();
        std::vector<QString> done;
        done.swap(settleQueue);
        settleNext = 0;
        SceneTeardown::release(std::move(done));
    }

    /**
     * @brief Announces columns first to last of every row as changed, through the top-level rows
     * alone: each dataChanged() makes a view lay its header out again, so one per parent would
     * cost a layout per open parent. A view must repaint the rows below itself on this signal, as
     * refreshDrawStates() requires.
     **/
    void emitColumnsChanged(int first,
                            int last)
    {
        if (root.children.empty()) {
            return;
        }
        Q_EMIT model->dataChanged(model->createIndex(0, first, root.children.front().get()),
                                  model->createIndex((int)root.children.size() - 1, last, root.children.back().get()));
    }

    // False without a viewer to read, and while an edit holds its draw states: the change that
    // edit makes is followed by a refreshDrawStates().
    bool drawCellOf(const Item* item,
                    DrawCell* cell) const
    {
        if (!drawStatesReader || item->path.IsEmpty()) {
            return false;
        }
        const QHash<QString, DrawCell>::const_iterator found = drawCells.constFind(item->pathString);
        if (found != drawCells.constEnd()) {
            *cell = found.value();

            return true;
        }
        const std::string path = item->pathString.toStdString();
        const bool free = drawStatesReader([&](const SceneDrawStates& states) {
            const SceneDrawResolved resolved = states.resolve(path);
            cell->state = (int)resolved.state;
            cell->inherited = (resolved.origin == eSceneDrawOriginInherited) || (resolved.origin == eSceneDrawOriginDefault);
            cell->origin = (int)resolved.origin;
            cell->originNode.clear();
            SceneDrawStates::Policy policy;
            if ((resolved.origin == eSceneDrawOriginGraphPolicy) && states.getPolicyState(path, &policy)) {
                cell->originNode = QString::fromStdString(policy.origin);
            }
        });
        if (!free) {
            return false;
        }
        drawCells.insert(item->pathString, *cell);

        return true;
    }

    Item* itemOf(const QModelIndex& index) const
    {
        return index.isValid() ? static_cast<Item*>(index.internalPointer()) : const_cast<Item*>(&root);
    }

    QModelIndex indexOf(Item* item) const
    {
        if (!item || (item == &root)) {
            return QModelIndex();
        }

        return model->createIndex(item->row, 0, item);
    }

    // Needs the stage's lock.
    void stopListening()
    {
        if (key.IsValid()) {
            TfNotice::Revoke(key);
        }
        listener.reset();
        listenedStage = UsdStageWeakPtr();
    }

    // Without waiting for the stage: a listener revoked during an edit could be destroyed while
    // that edit's notice is delivered to it, so one that finds the stage busy stays registered,
    // kept with the source that can read its stage, until a later read gets the lock.
    void stopListeningOrRetire()
    {
        if (source->tryReadStage([this](const SceneStage*) { stopListening(); })) {
            return;
        }
        RetiredListener retired;
        retired.source = source;
        retired.listener = std::move(listener);
        retired.key = key;
        retiredListeners.push_back(std::move(retired));
        listener.reset();
        key = TfNotice::Key();
        listenedStage = UsdStageWeakPtr();
    }

    void reapRetiredListeners()
    {
        retiredListeners.erase(std::remove_if(retiredListeners.begin(), retiredListeners.end(), [](RetiredListener& retired) {
                                   return retired.source->tryReadStage([&retired](const SceneStage*) { retired.revoke(); });
                               }),
                               retiredListeners.end());
    }

    // Compares raw pointers: under C++20, a TfWeakPtr/TfRefPtr comparison resolves to Tf's
    // reversed friend operator==, which calls itself forever.
    bool isListened(const UsdStageRefPtr& stage) const
    {
        return get_pointer(listenedStage) == get_pointer(stage);
    }

    // Needs the stage's lock.
    void listen(const UsdStageRefPtr& stage)
    {
        stopListening();
        listener.reset(new StageListener(&stageNoticeCount));
        key = TfNotice::Register(TfCreateWeakPtr(listener.get()), &StageListener::onObjectsChanged, UsdStageWeakPtr(stage));
        listenedStage = stage;
    }

    PayloadStateEnum payloadStateOf(const Item* item) const
    {
        if (item->data.payload != ePayloadStateUnloaded) {
            return item->data.payload;
        }
        if (loadingPaths.contains(item->pathString)) {
            return ePayloadStateLoading;
        }

        return loadFailures.contains(item->pathString) ? ePayloadStateFailed : ePayloadStateUnloaded;
    }

    Item* findItem(const QString& path)
    {
        const std::string pathString = path.toStdString();

        if (!SdfPath::IsValidPathString(pathString)) {
            return nullptr;
        }
        const SdfPath sdfPath(pathString);

        return (sdfPath.IsAbsolutePath() && sdfPath.IsAbsoluteRootOrPrimPath()) ? findItem(sdfPath) : nullptr;
    }

    /**
     * @brief Derives item's purpose from its parent's, then its children's: all of them with
     * attached, for rows being inserted, or else only below a row whose purpose changed, whose
     * purpose cell is announced.
     **/
    void derivePurposes(Item* item,
                        bool attached)
    {
        if (item->path.IsEmpty() || (item == &root)) {
            return;
        }
        const PurposeCell before = item->purpose;
        item->purpose = derivePurpose(item->data, item->path, item->parent ? &item->parent->purpose : nullptr);
        if (!attached) {
            if (item->purpose == before) {
                return;
            }
            const QModelIndex cell = model->createIndex(item->row, eColumnPurpose, item);
            Q_EMIT model->dataChanged(cell, cell);
        }
        for (const std::unique_ptr<Item>& child : item->children) {
            derivePurposes(child.get(), attached);
        }
    }

    void emitRowChanged(Item* item)
    {
        Q_EMIT model->dataChanged(indexOf(item), model->createIndex(item->row, eColumnCount - 1, item));
    }

    void addLoadingRow(Item* item)
    {
        if (item->loadingRow) {
            return;
        }
        ChangeScope scope(this);
        const int row = (int)item->children.size();
        model->beginInsertRows(indexOf(item), row, row);
        item->loadingRow.reset(new Item);
        item->loadingRow->parent = item;
        item->loadingRow->data.name = SceneGraphModel::tr("Loading...");
        item->placeLoadingRow();
        model->endInsertRows();
    }

    void removeLoadingRow(Item* item)
    {
        if (!item->loadingRow) {
            return;
        }
        ChangeScope scope(this);
        const int row = item->loadingRow->row;
        model->beginRemoveRows(indexOf(item), row, row);
        item->loadingRow.reset();
        model->endRemoveRows();
    }

    void setNamed(Item* item,
                  const NamesPtr& names)
    {
        item->namesRead = true;
        item->childNames = names ? names : noNames();
        if (item != &root) {
            namedItems.insert(item);
        }
    }

    // Takes item and what is under it out of the registry; returns how many items that is.
    int forget(Item* item)
    {
        int count = 1;

        if (item->namesRead) {
            namedItems.erase(item);
        }
        for (const std::unique_ptr<Item>& child : item->children) {
            count += forget(child.get());
        }

        return count;
    }

    // Adds rows, from the first of them item has not, after item's children.
    void appendRows(Item* item,
                    std::vector<std::unique_ptr<Item>>& rows,
                    std::size_t firstRow)
    {
        const std::size_t first = item->children.size();

        if ((first < firstRow) || (first >= firstRow + rows.size())) {
            return;
        }
        ChangeScope scope(this);
        const std::size_t count = firstRow + rows.size() - first;
        model->beginInsertRows(indexOf(item), (int)first, (int)(first + count) - 1);
        itemCount += (int)count;
        for (std::size_t i = first - firstRow; i < rows.size(); ++i) {
            std::unique_ptr<Item>& child = rows[i];
            child->parent = item;
            child->row = (int)item->children.size();
            derivePurposes(child.get(), true);
            item->children.push_back(std::move(child));
        }
        item->placeLoadingRow();
        item->rowOfValid = false;
        model->endInsertRows();
    }

    GatherNode nodeOf(const Item* item,
                      bool refreshKept) const
    {
        GatherNode node;

        node.path = item->path;
        node.namesRead = item->namesRead;
        node.payload = item->data.payload;
        node.names = item->childNames;
        node.childCount = item->children.size();
        node.refreshKept = refreshKept;

        return node;
    }

    // Applies an update gathered by gatherUpdate(), outside the stage's lock.
    bool applyUpdate(ItemUpdate& update)
    {
        Item* item = update.item;
        const QModelIndex index = indexOf(item);
        bool childrenFlagChanged = false;

        if (update.data.payload != ePayloadStateUnloaded) {
            loadingPaths.remove(item->pathString);
            loadFailures.remove(item->pathString);
            removeLoadingRow(item);
        }
        if (!(update.data == item->data)) {
            childrenFlagChanged = (update.data.hasChildren != item->data.hasChildren);
            if (item != &root) {
                item->data = update.data;
                Q_EMIT model->dataChanged(index, model->createIndex(item->row, eColumnCount - 1, item));
                derivePurposes(item, false);
            } else {
                item->data.hasChildren = update.data.hasChildren;
            }
        }
        if (!update.readChildren) {
            return childrenFlagChanged && item->children.empty();
        }
        const auto isKept = [&update](int row) {
            return ((std::size_t)row < update.kept.size()) && update.kept[row];
        };
        // Removes the gone children, last first, a run of rows at a time.
        int last = (int)item->children.size() - 1;
        while (last >= 0) {
            if (isKept(last)) {
                --last;
                continue;
            }
            int first = last;
            while ((first > 0) && !isKept(first - 1)) {
                --first;
            }
            model->beginRemoveRows(index, first, last);
            std::vector<std::unique_ptr<Item>> removed;
            for (int i = first; i <= last; ++i) {
                itemCount -= forget(item->children[i].get());
                removed.push_back(std::move(item->children[i]));
            }
            item->children.erase(item->children.begin() + first, item->children.begin() + last + 1);
            item->renumber();
            model->endRemoveRows();
            // A removed subtree can hold as many rows as the stage has prims.
            SceneTeardown::release(std::move(removed));
            last = first - 1;
        }
        // Inserts the new names among the kept children, a run of rows at a time.
        const TfTokenVector& names = *update.names;
        std::size_t row = 0;
        std::size_t i = 0;
        std::size_t next = 0;
        while (i < update.loadedCount) {
            if ((row < item->children.size()) && (item->children[row]->nameToken == names[i])) {
                ++row;
                ++i;
                continue;
            }
            std::size_t end = i;
            while ((end < update.loadedCount) && !((row < item->children.size()) && (item->children[row]->nameToken == names[end]))) {
                ++end;
            }
            if (next + (end - i) > update.newChildren.size()) {
                break;
            }
            model->beginInsertRows(index, (int)row, (int)(row + (end - i) - 1));
            std::vector<std::unique_ptr<Item>> inserted;
            for (std::size_t n = i; n < end; ++n) {
                std::unique_ptr<Item>& child = update.newChildren[next++];
                child->parent = item;
                derivePurposes(child.get(), true);
                inserted.push_back(std::move(child));
            }
            itemCount += (int)inserted.size();
            item->children.insert(item->children.begin() + row,
                                  std::make_move_iterator(inserted.begin()),
                                  std::make_move_iterator(inserted.end()));
            item->renumber();
            model->endInsertRows();
            row += end - i;
            i = end;
        }
        setNamed(item, update.names);

        return childrenFlagChanged && item->children.empty();
    }

    // Applies one gathered update; an update whose item changed since it was copied is not
    // applied, its path is carried to the next gather instead. Returns whether views must lay out
    // again.
    bool applyOne(GatheredUpdate& gathered)
    {
        Item* item = findItem(gathered.path);

        if (!item || (!gathered.node && (item == &root))) {
            // Under a row removed or replaced by an update before it.
            return false;
        }
        if (gathered.node && !gathered.node->matches(*item)) {
            carried.push_back(gathered.path);

            return false;
        }
        gathered.update.item = item;

        return applyUpdate(gathered.update);
    }

    void emitLayout()
    {
        // Views keep whether an item without rows has children; only a relayout asks again.
        Q_EMIT model->layoutAboutToBeChanged();
        Q_EMIT model->layoutChanged();
    }

    void applyGathered(std::vector<GatheredUpdate>& updates)
    {
        ChangeScope scope(this);
        bool layout = false;

        for (GatheredUpdate& gathered : updates) {
            layout = applyOne(gathered) || layout;
        }
        if (layout) {
            emitLayout();
        }
    }

    // Applies the updates of applying for up to applySliceMs, at least one, and has the event
    // loop apply the rest after: a heavy stage's edits take longer to apply than a frame lasts.
    void applySlice()
    {
        // Held here too: a slot the changes reach may reset the model, which lets go of applying.
        GatherRequestPtr held = applying;

        if (!held) {
            return;
        }
        ++applySliceCount;
        {
            ChangeScope scope(this);
            QElapsedTimer timer;
            timer.start();
            bool layout = false;
            while ((applying == held) && (applyNext < held->updates.size())) {
                layout = applyOne(held->updates[applyNext++]) || layout;
                if (timer.elapsed() >= applySliceMs) {
                    break;
                }
            }
            if (layout) {
                emitLayout();
            }
        }
        if (applying != held) {
            GatherRequest::retire(std::move(held));

            return;
        }
        if (applyNext < held->updates.size()) {
            applyTimer->start();

            return;
        }
        gatheringReset = -1;
        applying.reset();
        GatherRequest::retire(std::move(held));
        model->refresh();
    }

    // The resynced and info-only paths the listener queued, with a copy of the root and of every
    // named item for prepare() to pick the items to read again from, off the main thread. Copies
    // the items' names by pointer and none of their rows, so that the request costs what the
    // items whose names were read do.
    GatherRequestPtr makeRequest(SdfPathVector resynced,
                                 const SdfPathVector& infoOnly,
                                 bool all)
    {
        GatherRequestPtr request = newRequest();

        request->unprepared = true;
        request->all = all;
        request->resynced = std::move(resynced);
        request->infoOnly = infoOnly;
        request->nodes.reserve(namedItems.size() + 1);
        request->nodes.push_back(nodeOf(&root, false));
        for (const Item* item : namedItems) {
            request->nodes.push_back(nodeOf(item, false));
        }

        return request;
    }

    GatherRequestPtr newRequest() const
    {
        GatherRequestPtr request = std::make_shared<GatherRequest>();

        request->source = source;
        request->stage = get_pointer(listenedStage);
        request->resetCount = resetCount;

        return request;
    }

    bool isGathering() const
    {
        return gatheringReset == resetCount;
    }

    void postToWorker(GatherRequestPtr request)
    {
        if (!worker) {
            SceneGraphModel* target = model;
            worker.reset(new GatherWorker([target](GatherRequestPtr gathered) {
                QMetaObject::invokeMethod(target, [target, request = std::move(gathered)]() mutable { target->_imp->onGathered(std::move(request)); }, Qt::QueuedConnection);
            }));
        }
        worker->post(std::move(request));
    }

    void postGather(GatherRequestPtr request)
    {
        gatheringReset = resetCount;
        postToWorker(std::move(request));
    }

    // On the main thread: a request for an earlier reset is dropped, and one that found another
    // stage has every row read again.
    void onGathered(GatherRequestPtr request)
    {
        if (request->resetCount != resetCount) {
            GatherRequest::retire(std::move(request));

            return;
        }
        if (request->isFetch()) {
            onFetched(std::move(request));

            return;
        }
        if (request->otherStage) {
            carryAll = true;
            gatheringReset = -1;
            GatherRequest::retire(std::move(request));
            model->refresh();

            return;
        }
        applying = std::move(request);
        applyNext = 0;
        applySlice();
    }

    // Posts a read of rows, which the row under it shows as loading until it lands, when it has
    // none to show yet.
    void postFetch(GatherRequestPtr request)
    {
        const SdfPath first = request->levels.front().path;

        ++fetchesInFlight;
        ++fetchingUnder[first];
        if (!request->target.IsEmpty()) {
            fetchTargets.insert(request->target);
        }
        Item* item = findItem(first);
        if (item && (item != &root) && item->children.empty() && !changing) {
            addLoadingRow(item);
        }
        postToWorker(std::move(request));
    }

    FetchLevel levelOf(const Item* item) const
    {
        FetchLevel level;

        level.path = item->path;
        if (item->namesRead) {
            level.names = item->childNames;
        }
        level.from = item->children.size();

        return level;
    }

    // Reads the next batch of item's children on the worker.
    void postFetchMore(Item* item)
    {
        if (!listener) {
            return;
        }
        GatherRequestPtr request = newRequest();
        request->levels.push_back(levelOf(item));
        postFetch(std::move(request));
    }

    // Reads on the worker the rows down to path's, from item, the deepest read row above it, on.
    // False when there is nothing to read: path is not among item's children.
    bool postPathFetch(Item* item,
                       const SdfPath& path)
    {
        if (!listener) {
            return false;
        }
        if (fetchTargets.count(path)) {
            return true;
        }
        SdfPathVector prefixes = path.GetPrefixes();
        prefixes.erase(std::remove(prefixes.begin(), prefixes.end(), SdfPath::AbsoluteRootPath()), prefixes.end());
        const std::size_t depth = item->path.GetPathElementCount();
        if (depth >= prefixes.size()) {
            return false;
        }
        const TfToken& wanted = prefixes[depth].GetNameToken();
        FetchLevel first = levelOf(item);
        if (item->namesRead) {
            const TfTokenVector& names = *item->childNames;
            const TfTokenVector::const_iterator found = std::find(names.begin(), names.end(), wanted);
            if (found == names.end()) {
                return false;
            }
            first.upTo = (std::size_t)(found - names.begin()) + 1;
        }
        first.wanted = wanted;
        GatherRequestPtr request = newRequest();
        request->target = path;
        request->levels.push_back(std::move(first));
        for (std::size_t i = depth + 1; i < prefixes.size(); ++i) {
            FetchLevel level;
            level.path = prefixes[i - 1];
            level.wanted = prefixes[i].GetNameToken();
            request->levels.push_back(std::move(level));
        }
        postFetch(std::move(request));

        return true;
    }

    // Asks again for what a read of rows was for, once the rows it read no longer fit the tree.
    void askAgain(const GatherRequest& request)
    {
        if (!request.target.IsEmpty()) {
            model->indexOfPath(QString::fromStdString(request.target.GetString()), true);

            return;
        }
        Item* item = findItem(request.levels.front().path);
        if (item) {
            const QModelIndex index = indexOf(item);
            if (model->canFetchMore(index)) {
                postFetchMore(item);
            }
        }
    }

    // Adds the rows a read on the worker found. False if a level's item changed meanwhile so that
    // its rows no longer fit it.
    bool applyFetched(GatherRequest& request)
    {
        Item* item = findItem(request.levels.front().path);
        const int reset = resetCount;

        for (FetchLevel& level : request.levels) {
            if (!item) {
                return (&level != &request.levels.front());
            }
            if (!level.done || !level.names) {
                return true;
            }
            if (item->namesRead && (item->childNames != level.names) && (*item->childNames != *level.names)) {
                return false;
            }
            if (item->children.size() < level.from) {
                return false;
            }
            if (!item->namesRead) {
                ChangeScope scope(this);
                setNamed(item, level.names);
            }
            appendRows(item, level.rows, level.from);
            if (level.wanted.IsEmpty() || (resetCount != reset)) {
                return true;
            }
            item = item->findChild(level.wanted);
        }

        return true;
    }

    void onFetched(GatherRequestPtr request)
    {
        const SdfPath first = request->levels.front().path;

        --fetchesInFlight;
        std::map<SdfPath, int>::iterator under = fetchingUnder.find(first);
        if ((under != fetchingUnder.end()) && (--under->second <= 0)) {
            fetchingUnder.erase(under);
        }
        if (!request->target.IsEmpty()) {
            fetchTargets.erase(request->target);
        }
        if (request->otherStage) {
            carryAll = true;
        } else if (!applyFetched(*request)) {
            askAgain(*request);
        }
        if (!fetchingUnder.count(first)) {
            Item* item = findItem(first);
            if (item && (item != &root) && !loadingPaths.contains(item->pathString)) {
                removeLoadingRow(item);
            }
        }
        GatherRequest::retire(std::move(request));
        model->refresh();
        if ((fetchesInFlight == 0) && owedFetches.empty()) {
            Q_EMIT model->deferredReadsDone();
        }
    }

    void resetTo(bool hasStage,
                 const PrimData& rootData)
    {
        // The reset drops the fetches in flight, whose end is what a waiter on deferred reads awaits.
        const bool readsOwed = (fetchesInFlight > 0) || !owedFetches.empty();
        resetRows(hasStage, rootData);
        if (readsOwed && (fetchesInFlight == 0) && owedFetches.empty()) {
            Q_EMIT model->deferredReadsDone();
        }
    }

    void resetRows(bool hasStage,
                   const PrimData& rootData)
    {
        ChangeScope scope(this);
        model->beginResetModel();
        // What the tree read from a heavy stage holds takes longer to destroy than a frame lasts.
        std::vector<std::unique_ptr<Item>> old;
        old.swap(root.children);
        NamesPtr oldNames = std::move(root.childNames);
        root.childNames = noNames();
        std::unordered_map<TfToken, int, TfToken::HashFunctor> oldRowOf;
        oldRowOf.swap(root.rowOf);
        std::unordered_set<Item*> oldNamed;
        oldNamed.swap(namedItems);
        QSet<QString> oldLoading;
        oldLoading.swap(loadingPaths);
        QHash<QString, QString> oldFailures;
        oldFailures.swap(loadFailures);
        GatherRequestPtr oldApplying = std::move(applying);
        applying.reset();
        applyNext = 0;
        applyTimer->stop();
        root.namesRead = false;
        root.rowOfValid = false;
        root.data = hasStage ? rootData : PrimData();
        std::map<SdfPath, std::size_t> oldOwed;
        oldOwed.swap(owedFetches);
        clearSettleQueue();
        fetchesInFlight = 0;
        std::map<SdfPath, int> oldFetchingUnder;
        oldFetchingUnder.swap(fetchingUnder);
        std::set<SdfPath> oldFetchTargets;
        oldFetchTargets.swap(fetchTargets);
        SdfPathVector oldCarried;
        oldCarried.swap(carried);
        carryAll = false;
        itemCount = 0;
        ++resetCount;
        model->endResetModel();
        GatherRequest::retire(std::move(oldApplying));
        SceneTeardown::release(std::move(old));
        SceneTeardown::release(std::move(oldNames));
        SceneTeardown::release(std::move(oldRowOf));
        SceneTeardown::release(std::move(oldNamed));
        SceneTeardown::release(std::move(oldLoading));
        SceneTeardown::release(std::move(oldFailures));
        SceneTeardown::release(std::move(oldOwed));
        SceneTeardown::release(std::move(oldFetchingUnder));
        SceneTeardown::release(std::move(oldFetchTargets));
        SceneTeardown::release(std::move(oldCarried));
    }

    // The read of item's children ran into an edit of the stage. A row with nothing to show yet
    // says so rather than looking empty.
    void oweFetch(Item* item,
                  std::size_t upTo)
    {
        std::size_t& owed = owedFetches[item->path];

        owed = std::max(owed, upTo);
        if ((item != &root) && item->children.empty()) {
            addLoadingRow(item);
        }
    }

    // Reads the next batch of item's children under the main thread's read. Returns whether any
    // were added.
    bool fetch(Item* item,
               std::size_t upTo,
               bool* busy = nullptr)
    {
        if (!source || changing) {
            return false;
        }
        std::vector<std::unique_ptr<Item>> fetched;
        bool namesRead = item->namesRead;
        NamesPtr names = item->childNames;
        const std::size_t from = item->children.size();
        const bool read = source->tryReadStage([&](const SceneStage* sceneStage) {
            if (!sceneStage || !isListened(sceneStage->getStage())) {
                return;
            }
            const UsdStageRefPtr& stage = sceneStage->getStage();
            if (!namesRead) {
                const UsdPrim prim = stage->GetPrimAtPath(item->path);
                names = prim ? std::make_shared<const TfTokenVector>(prim.GetFilteredChildrenNames(childPredicate())) : noNames();
                namesRead = true;
            }
            const TfTokenVector& allNames = *names;
            const std::size_t end = std::min(allNames.size(), std::max(upTo, from + kFetchBatchSize));
            for (std::size_t i = from; i < end; ++i) {
                const SdfPath path = item->path.AppendChild(allNames[i]);
                fetched.push_back(makeItem(item->path, allNames[i], readPrimData(stage->GetPrimAtPath(path))));
            }
        });
        if (!read) {
            oweFetch(item, upTo);
            if (busy) {
                *busy = true;
            }

            return false;
        }
        if (namesRead && !item->namesRead) {
            ChangeScope scope(this);
            setNamed(item, names);
        }
        if (fetched.empty()) {
            return false;
        }
        appendRows(item, fetched, from);

        return true;
    }

    Item* findItem(const SdfPath& path)
    {
        if (path == SdfPath::AbsoluteRootPath()) {
            return &root;
        }
        Item* item = &root;
        for (const SdfPath& prefix : path.GetPrefixes()) {
            if (prefix.IsAbsoluteRootPath()) {
                continue;
            }
            item = item->findChild(prefix.GetNameToken());
            if (!item) {
                return nullptr;
            }
        }

        return item;
    }
};

Usd_PrimFlagsPredicate
childPredicate()
{
    // Unloaded prims are shown, so a payload can be found before it is loaded.
    return UsdTraverseInstanceProxies(UsdPrimIsActive && UsdPrimIsDefined && !UsdPrimIsAbstract);
}

QIcon
iconForType(const QString& type)
{
    static const QIcon camera(QString::fromUtf8(":/Resources/Images/NativeNodes/USDCamera.png"));
    static const QIcon light(QString::fromUtf8(":/Resources/Images/NativeNodes/USDLight.png"));
    static const QIcon material(QString::fromUtf8(":/Resources/Images/NativeNodes/USDPreviewSurface.png"));
    static const QIcon xform(QString::fromUtf8(":/Resources/Images/NativeNodes/USDXformPrim.png"));
    static const QIcon scope(QString::fromUtf8(":/Resources/Images/NativeNodes/USDScopePrim.png"));

    if (type == QLatin1String("Camera")) {
        return camera;
    }
    if (type.endsWith(QLatin1String("Light"))) {
        return light;
    }
    if ((type == QLatin1String("Material")) || (type == QLatin1String("Shader")) || (type == QLatin1String("NodeGraph"))) {
        return material;
    }
    if (type == QLatin1String("Xform")) {
        return xform;
    }
    if (type == QLatin1String("Scope")) {
        return scope;
    }

    return QIcon();
}

QString
SceneGraphModel::primPathsMimeType()
{
    return QString::fromUtf8("application/x-natron-prim-paths");
}

QMimeData*
SceneGraphModel::mimeDataOfPaths(const QStringList& paths)
{
    if (paths.isEmpty()) {
        return nullptr;
    }
    QMimeData* mime = new QMimeData;
    const QString joined = paths.join(QLatin1Char('\n'));
    mime->setData(primPathsMimeType(), joined.toUtf8());
    mime->setText(joined);

    return mime;
}

SceneGraphModel::SceneGraphModel(QObject* parent)
    : QAbstractItemModel(parent)
    , _imp(new Implementation(this))
{
}

SceneGraphModel::~SceneGraphModel()
{
    // Waits for the gather under way; what it posts is dropped with this object.
    _imp->worker.reset();
    if (_imp->source) {
        // Notices are sent by whoever edits the stage, under its lock.
        _imp->source->readStage([this](const SceneStage*) { _imp->stopListening(); });
    }
    for (Implementation::RetiredListener& retired : _imp->retiredListeners) {
        retired.source->readStage([&retired](const SceneStage*) { retired.revoke(); });
    }
    SceneTeardown::release(std::move(_imp->root.children));
}

void
SceneGraphModel::setSource(const SceneGraphStageSourcePtr& source)
{
    if (source == _imp->source) {
        refresh();

        return;
    }
    if (_imp->source) {
        _imp->stopListeningOrRetire();
    }
    _imp->source = source;
    _imp->settleOwed = false;
    QSet<QString> oldChangedHere;
    oldChangedHere.swap(_imp->changedHere);
    SceneTeardown::release(std::move(oldChangedHere));
    // One reset to the new stage's root rather than one to nothing and one more from refresh(),
    // which the old tree's gathers and reads in flight would hold back anyway.
    bool hasStage = false;
    PrimData rootData;
    if (source) {
        const U64 editCount = source->getEditCount();
        (void)source->tryReadStage([&](const SceneStage* sceneStage) {
            _imp->lastEditCount = editCount;
            if (sceneStage) {
                const UsdStageRefPtr& stage = sceneStage->getStage();
                _imp->listen(stage);
                rootData = readPrimData(stage->GetPseudoRoot());
                hasStage = true;
            }
        });
    }
    _imp->resetTo(hasStage, rootData);
}

const SceneGraphStageSourcePtr&
SceneGraphModel::getSource() const
{
    return _imp->source;
}

void
SceneGraphModel::refresh()
{
    if (_imp->deferred) {
        _imp->stale = true;

        return;
    }
    _imp->reapRetiredListeners();
    if (_imp->source && readEdits()) {
        retryOwedReads();
    }
}

void
SceneGraphModel::setReadsOnWorker(bool onWorker)
{
    _imp->readsOnWorker = onWorker;
}

void
SceneGraphModel::setApplySliceMs(int ms)
{
    _imp->applySliceMs = std::max(0, ms);
}

int
SceneGraphModel::getApplySliceCount() const
{
    return _imp->applySliceCount;
}

void
SceneGraphModel::setDeferred(bool deferred)
{
    if (deferred == _imp->deferred) {
        return;
    }
    _imp->deferred = deferred;
    if (deferred) {
        return;
    }
    if (_imp->settleStale) {
        _imp->settleStale = false;
        _imp->settleOwed = bool(_imp->source);
    }
    const bool drawStates = _imp->drawStatesStale;
    _imp->drawStatesStale = false;
    if (_imp->stale) {
        _imp->stale = false;
        refresh();
    }
    if (drawStates) {
        refreshDrawStates();
    }
}

bool
SceneGraphModel::isDeferred() const
{
    return _imp->deferred;
}

bool
SceneGraphModel::isStale() const
{
    return _imp->stale || _imp->settleStale || _imp->drawStatesStale;
}

bool
SceneGraphModel::hasPendingSettle() const
{
    return _imp->hasPendingSettle();
}

int
SceneGraphModel::getSettleSliceCount() const
{
    return _imp->settleSliceCount;
}

bool
SceneGraphModel::hasOwedReads() const
{
    return _imp->settleOwed || !_imp->owedFetches.empty() || (_imp->fetchesInFlight > 0) || _imp->hasPendingSettle();
}

bool
SceneGraphModel::isChanging() const
{
    return _imp->changing > 0;
}

void
SceneGraphModel::retryOwedReads()
{
    if (_imp->settleOwed) {
        settle();
    }
    if (_imp->owedFetches.empty()) {
        return;
    }
    std::map<SdfPath, std::size_t> owed;
    owed.swap(_imp->owedFetches);
    for (const std::pair<const SdfPath, std::size_t>& entry : owed) {
        Item* item = _imp->findItem(entry.first);
        if (!item) {
            continue;
        }
        _imp->fetch(item, entry.second);
        if (!_imp->owedFetches.count(entry.first) && !_imp->loadingPaths.contains(item->pathString)) {
            _imp->removeLoadingRow(item);
        }
    }
    if (_imp->owedFetches.empty()) {
        Q_EMIT deferredReadsDone();
    }
}

bool
SceneGraphModel::readEdits()
{
    if (_imp->isGathering() || (_imp->fetchesInFlight > 0)) {
        // The gather's or read's end refreshes again, and reads what was edited meanwhile: rows a
        // read brings in after the notices of their edits were taken in would keep what it saw.
        return false;
    }
    const U64 editCount = _imp->source->getEditCount();
    if (_imp->listener && (editCount == _imp->lastEditCount) && !_imp->listener->hasPending() && _imp->carried.empty() && !_imp->carryAll) {
        return true;
    }
    enum {
        eNone,
        eClear,
        eReset,
        eUpdate
    } action
        = eNone;
    PrimData rootData;
    GatherRequestPtr request;
    SdfPathVector resynced;
    SdfPathVector infoOnly;
    bool overflow = false;
    const bool read = _imp->source->tryReadStage([&](const SceneStage* sceneStage) {
        _imp->lastEditCount = editCount;
        if (!sceneStage) {
            if (_imp->listener || _imp->root.data.hasChildren) {
                _imp->stopListening();
                action = eClear;
            }

            return;
        }
        const UsdStageRefPtr& stage = sceneStage->getStage();
        if (!_imp->listener || !_imp->isListened(stage)) {
            _imp->listen(stage);
            rootData = readPrimData(stage->GetPseudoRoot());
            action = eReset;

            return;
        }
        {
            std::lock_guard<std::mutex> lock(_imp->listener->mutex);
            resynced.swap(_imp->listener->resynced);
            infoOnly.swap(_imp->listener->infoOnly);
            overflow = _imp->listener->overflow;
            _imp->listener->overflow = false;
        }
        resynced.insert(resynced.end(), _imp->carried.begin(), _imp->carried.end());
        _imp->carried.clear();
        overflow = overflow || _imp->carryAll;
        _imp->carryAll = false;
        action = eUpdate;
        if (!_imp->readsOnWorker) {
            request = _imp->makeRequest(std::move(resynced), infoOnly, overflow);
            request->prepare();
            request->gather(stage);
        }
    });
    if (!read) {
        // The notices stay queued and the edit count unread, so the next refresh reads them.
        return false;
    }
    switch (action) {
    case eNone:
        break;
    case eClear:
        _imp->resetTo(false, PrimData());
        break;
    case eReset:
        _imp->resetTo(true, rootData);
        break;
    case eUpdate:
        if (_imp->readsOnWorker) {
            if (resynced.empty() && infoOnly.empty() && !overflow) {
                break;
            }
            // The rows are copied outside the stage's lock; the worker picks the ones to read
            // again, and reads their prims.
            request = _imp->makeRequest(std::move(resynced), infoOnly, overflow);
            _imp->postGather(std::move(request));

            return false;
        }
        _imp->applyGathered(request->updates);
        GatherRequest::retire(std::move(request));
        break;
    }

    return true;
} // SceneGraphModel::readEdits

QModelIndex
SceneGraphModel::indexOfPath(const QString& path,
                             bool populate,
                             bool* busy)
{
    const std::string pathString = path.toStdString();

    if (!SdfPath::IsValidPathString(pathString)) {
        return QModelIndex();
    }
    const SdfPath sdfPath(pathString);
    if (!sdfPath.IsAbsolutePath() || !sdfPath.IsPrimPath()) {
        return QModelIndex();
    }
    Item* item = &_imp->root;
    for (const SdfPath& prefix : sdfPath.GetPrefixes()) {
        if (prefix.IsAbsoluteRootPath()) {
            continue;
        }
        const TfToken& name = prefix.GetNameToken();
        Item* child = item->findChild(name);
        if (!child && populate && _imp->readsOnWorker) {
            if (_imp->postPathFetch(item, sdfPath) && busy) {
                *busy = true;
            }

            return QModelIndex();
        }
        if (!child && populate) {
            if (!item->namesRead) {
                _imp->fetch(item, 0, busy);
            }
            const TfTokenVector& names = *item->childNames;
            const TfTokenVector::const_iterator found = std::find(names.begin(), names.end(), name);
            if (found == names.end()) {
                return QModelIndex();
            }
            _imp->fetch(item, (std::size_t)(found - names.begin()) + 1, busy);
            child = item->findChild(name);
        }
        if (!child) {
            return QModelIndex();
        }
        item = child;
    }

    return _imp->indexOf(item);
}

void
SceneGraphModel::addPathRows(const SceneGraphPathRowsPtr& rows)
{
    if (!rows || !rows->stage || _imp->changing || (rows->stage != get_pointer(_imp->listenedStage))) {
        return;
    }
    for (SceneGraphPathRows::Parent& parent : rows->parents) {
        Item* item = _imp->findItem(parent.path);
        if (!item || !parent.namesRead || (item->namesRead && (*item->childNames != parent.names))) {
            continue;
        }
        Implementation::ChangeScope scope(_imp.get());
        if (!item->namesRead) {
            _imp->setNamed(item, std::make_shared<const TfTokenVector>(std::move(parent.names)));
        }
        const std::size_t first = item->children.size();
        if (first >= parent.rows.size()) {
            continue;
        }
        beginInsertRows(_imp->indexOf(item), (int)first, (int)parent.rows.size() - 1);
        _imp->itemCount += (int)(parent.rows.size() - first);
        for (std::size_t i = first; i < parent.rows.size(); ++i) {
            std::unique_ptr<Item>& child = parent.rows[i];
            child->parent = item;
            child->row = (int)i;
            _imp->derivePurposes(child.get(), true);
            item->children.push_back(std::move(child));
        }
        item->placeLoadingRow();
        item->rowOfValid = false;
        endInsertRows();
    }
}

SceneGraphPathRowsBuilder::SceneGraphPathRowsBuilder(const QStringList& paths)
    : _rows(std::make_shared<SceneGraphPathRows>())
{
    std::map<SdfPath, std::unordered_set<TfToken, TfToken::HashFunctor>> wanted;

    for (const QString& path : paths) {
        const std::string pathString = path.toStdString();
        if (!SdfPath::IsValidPathString(pathString)) {
            continue;
        }
        const SdfPath sdfPath(pathString);
        if (!sdfPath.IsAbsolutePath() || !sdfPath.IsPrimPath()) {
            continue;
        }
        for (const SdfPath& prefix : sdfPath.GetPrefixes()) {
            if (!prefix.IsAbsoluteRootPath()) {
                wanted[prefix.GetParentPath()].insert(prefix.GetNameToken());
            }
        }
    }
    for (std::pair<const SdfPath, std::unordered_set<TfToken, TfToken::HashFunctor>>& entry : wanted) {
        SceneGraphPathRows::Parent parent;
        parent.path = entry.first;
        parent.wanted = std::move(entry.second);
        _rows->parents.push_back(std::move(parent));
    }
}

SceneGraphPathRowsBuilder::~SceneGraphPathRowsBuilder() = default;

bool
SceneGraphPathRowsBuilder::isDone() const
{
    return _nextParent >= _rows->parents.size();
}

void
SceneGraphPathRowsBuilder::readBatch(const SceneStage* stage)
{
    const UsdStageRefPtr usdStage = stage ? stage->getStage() : UsdStageRefPtr();

    if (!usdStage) {
        _rows->stage = nullptr;
        _nextParent = _rows->parents.size();

        return;
    }
    _rows->stage = get_pointer(usdStage);
    SceneGraphPathRows::Parent& parent = _rows->parents[_nextParent];
    if (!parent.namesRead) {
        const UsdPrim prim = usdStage->GetPrimAtPath(parent.path);
        if (prim) {
            parent.names = prim.GetFilteredChildrenNames(childPredicate());
        }
        parent.namesRead = true;
        for (std::size_t i = 0; i < parent.names.size(); ++i) {
            if (parent.wanted.find(parent.names[i]) != parent.wanted.end()) {
                parent.rowCount = i + 1;
            }
        }
    }
    const std::size_t end = std::min(parent.rowCount, parent.rows.size() + SceneGraphModel::kFetchBatchSize);
    for (std::size_t i = parent.rows.size(); i < end; ++i) {
        parent.rows.push_back(makeItem(parent.path, parent.names[i], readPrimData(usdStage->GetPrimAtPath(parent.path.AppendChild(parent.names[i])))));
    }
    if (parent.rows.size() >= parent.rowCount) {
        ++_nextParent;
    }
}

SceneGraphPathRowsPtr
SceneGraphPathRowsBuilder::take()
{
    return std::move(_rows);
}

QString
SceneGraphModel::pathOf(const QModelIndex& index) const
{
    return index.isValid() ? _imp->itemOf(index)->pathString : QString();
}

SceneGraphModel::PayloadStateEnum
SceneGraphModel::getPayloadState(const QString& path)
{
    const Item* item = _imp->findItem(path);

    return item ? _imp->payloadStateOf(item) : ePayloadStateNone;
}

void
SceneGraphModel::markLoading(const QStringList& paths)
{
    for (const QString& path : paths) {
        Item* item = _imp->findItem(path);
        if (!item || (item == &_imp->root) || (item->data.payload != ePayloadStateUnloaded)) {
            continue;
        }
        _imp->loadFailures.remove(path);
        _imp->loadingPaths.insert(path);
        _imp->addLoadingRow(item);
        _imp->emitRowChanged(item);
    }
}

void
SceneGraphModel::settleLoads()
{
    if (!_imp->source) {
        return;
    }
    if (_imp->deferred) {
        _imp->settleStale = true;
        _imp->stale = true;

        return;
    }
    // Owed until read: the edit in the way may be a sync, which ends with a refresh().
    _imp->settleOwed = true;
    refresh();
}

void
SceneGraphModel::settle()
{
    QHash<QString, QString> failures;
    bool outstanding = false;
    const bool read = _imp->source->tryReadStage([&](const SceneStage* stage) {
        if (!stage) {
            return;
        }
        for (const SceneStage::LoadFailure& failure : stage->getLoadFailures()) {
            failures.insert(QString::fromStdString(failure.prim.GetString()), QString::fromStdString(failure.error));
        }
        outstanding = stage->hasOutstandingLoads();
    });
    if (!read) {
        return;
    }
    _imp->settleOwed = false;
    QSet<QString> touched;
    if (!outstanding) {
        touched.swap(_imp->loadingPaths);
    } else {
        for (QHash<QString, QString>::const_iterator it = failures.constBegin(); it != failures.constEnd(); ++it) {
            if (_imp->loadingPaths.remove(it.key())) {
                touched.insert(it.key());
            }
        }
    }
    if (failures != _imp->loadFailures) {
        for (QHash<QString, QString>::const_iterator it = failures.constBegin(); it != failures.constEnd(); ++it) {
            if (_imp->loadFailures.value(it.key()) != it.value()) {
                touched.insert(it.key());
            }
        }
        for (QHash<QString, QString>::const_iterator it = _imp->loadFailures.constBegin(); it != _imp->loadFailures.constEnd(); ++it) {
            if (!failures.contains(it.key())) {
                touched.insert(it.key());
            }
        }
        _imp->loadFailures = failures;
    }
    if (touched.isEmpty()) {
        return;
    }
    _imp->settleQueue.reserve(_imp->settleQueue.size() + (std::size_t)touched.size());
    for (const QString& path : touched) {
        _imp->settleQueue.push_back(path);
    }
    SceneTeardown::release(std::move(touched));
    _imp->settleSlice();
}

void
SceneGraphModel::setChangedHere(const QSet<QString>& paths)
{
    if (paths == _imp->changedHere) {
        return;
    }
    _imp->changedHere = paths;
    _imp->emitColumnsChanged(0, eColumnCount - 1);
}

QString
SceneGraphModel::drawStateLabel(int state)
{
    switch (state) {
    case eSceneDrawStateBBox:
        return tr("BBox");
    case eSceneDrawStateChildren:
        return tr("Expand");
    case eSceneDrawStateFull:
        return tr("Geometry");
    case eSceneDrawStateLoadAll:
        return tr("Load + Draw");
    case eSceneDrawStateHidden:
        return tr("Hide");
    default:
        return QString();
    }
}

QString
SceneGraphModel::rowsText(int count)
{
    return (count == 1) ? tr("1 row") : tr("%1 rows").arg(count);
}

QString
SceneGraphModel::kindPluralText(const QString& kind)
{
    if (kind == QString::fromUtf8("assembly")) {
        return tr("assemblies");
    }
    if (kind == QString::fromUtf8("component")) {
        return tr("components");
    }
    if (kind == QString::fromUtf8("subcomponent")) {
        return tr("subcomponents");
    }

    return tr("%1 prims").arg(kind);
}

QString
SceneGraphModel::kindCountText(const QString& kind,
                               int count)
{
    if (count != 1) {
        return tr("%1 %2").arg(count).arg(kindPluralText(kind));
    }

    return isBuiltInKindScope(PXR_NS::TfToken(kind.toStdString())) ? tr("1 %1").arg(kind) : tr("1 %1 prim").arg(kind);
}

QString
SceneGraphModel::loadingText(int rows,
                             const QString& scope)
{
    if (scope == QString::fromUtf8("selected")) {
        return tr("Loading %1").arg(rowsText(rows));
    }
    if (scope == QString::fromUtf8("below")) {
        return tr("Loading %1 and every payload below them").arg(rowsText(rows));
    }

    return tr("Loading down to %1 under %2").arg(kindPluralText(scope), rowsText(rows));
}

QString
SceneGraphModel::unloadingText(int rows,
                               const QString& scope)
{
    if (scope == QString::fromUtf8("selected")) {
        return tr("Unloading %1").arg(rowsText(rows));
    }

    return tr("Unloading below %1 under %2").arg(kindPluralText(scope), rowsText(rows));
}

QString
SceneGraphModel::drawOriginTip(int origin,
                               const QString& node)
{
    switch (origin) {
    case eSceneDrawOriginViewer:
        return tr("Set in this viewer");
    case eSceneDrawOriginGraphPolicy:
        return node.isEmpty() ? tr("Set by the node graph") : tr("Set by %1").arg(node);
    case eSceneDrawOriginInherited:
        return tr("Inherited from the row above");
    default:
        return tr("Default: nothing sets this row or a row above it");
    }
}

QString
SceneGraphModel::purposeTip(const QString& purpose,
                            bool inherited,
                            const QString& origin)
{
    if (!inherited) {
        return tr("Purpose: %1 (authored here)").arg(purpose);
    }
    if (origin.isEmpty()) {
        return tr("Purpose: %1 (the fallback: nothing above authors one)").arg(purpose);
    }

    return tr("Purpose: %1 (from %2)").arg(purpose, origin);
}

void
SceneGraphModel::setDrawStateAccess(const DrawStatesReader& reader,
                                    const DrawStateWriter& writer)
{
    _imp->drawStatesReader = reader;
    _imp->drawStateWriter = writer;
    refreshDrawStates();
}

void
SceneGraphModel::refreshDrawStates()
{
    _imp->drawCells.clear();
    if (_imp->deferred) {
        _imp->drawStatesStale = true;

        return;
    }
    _imp->emitColumnsChanged(eColumnDrawState, eColumnDrawState);
}

bool
SceneGraphModel::isChangedHere(const QString& path) const
{
    return _imp->changedHere.contains(path);
}

int
SceneGraphModel::getChangedHereCount() const
{
    return _imp->changedHere.size();
}

int
SceneGraphModel::getItemCount() const
{
    return _imp->itemCount;
}

int
SceneGraphModel::getResetCount() const
{
    return _imp->resetCount;
}

U64
SceneGraphModel::getStageNoticeCount() const
{
    return _imp->stageNoticeCount;
}

QModelIndex
SceneGraphModel::index(int row,
                       int column,
                       const QModelIndex& parent) const
{
    if ((column < 0) || (column >= eColumnCount) || (parent.isValid() && (parent.column() != 0))) {
        return QModelIndex();
    }
    Item* item = _imp->itemOf(parent);
    if ((row < 0) || (row >= rowCount(parent))) {
        return QModelIndex();
    }

    return createIndex(row, column, (row < (int)item->children.size()) ? item->children[row].get() : item->loadingRow.get());
}

QModelIndex
SceneGraphModel::parent(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return QModelIndex();
    }

    return _imp->indexOf(_imp->itemOf(index)->parent);
}

int
SceneGraphModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() && (parent.column() != 0)) {
        return 0;
    }

    const Item* item = _imp->itemOf(parent);

    return (int)item->children.size() + (item->loadingRow ? 1 : 0);
}

int
SceneGraphModel::columnCount(const QModelIndex& /*parent*/) const
{
    return eColumnCount;
}

bool
SceneGraphModel::hasChildren(const QModelIndex& parent) const
{
    if (parent.isValid() && (parent.column() != 0)) {
        return false;
    }
    const Item* item = _imp->itemOf(parent);

    return !item->children.empty() || item->loadingRow || item->data.hasChildren;
}

QVariant
SceneGraphModel::data(const QModelIndex& index,
                      int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }
    const Item* item = _imp->itemOf(index);
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case eColumnName:
            return item->data.name;
        case eColumnType:
            return item->data.type;
        case eColumnKind:
            return item->data.kind;
        case eColumnPurpose:
            return item->data.imageable ? QVariant(QString::fromStdString(item->purpose.purpose.GetString())) : QVariant();
        case eColumnDrawState: {
            Implementation::DrawCell cell;

            return _imp->drawCellOf(item, &cell) ? QVariant(drawStateLabel(cell.state)) : QVariant();
        }
        default:
            return QVariant();
        }
    case Qt::EditRole: {
        Implementation::DrawCell cell;
        if ((index.column() == eColumnDrawState) && _imp->drawCellOf(item, &cell)) {
            return cell.state;
        }

        return QVariant();
    }
    case eDrawStateRole: {
        Implementation::DrawCell cell;

        return _imp->drawCellOf(item, &cell) ? QVariant(cell.state) : QVariant();
    }
    case eDrawStateInheritedRole: {
        Implementation::DrawCell cell;

        return _imp->drawCellOf(item, &cell) ? QVariant(cell.inherited) : QVariant();
    }
    case eDrawStateOriginRole: {
        Implementation::DrawCell cell;

        return _imp->drawCellOf(item, &cell) ? QVariant(cell.origin) : QVariant();
    }
    case eDrawStateOriginNodeRole: {
        Implementation::DrawCell cell;

        return _imp->drawCellOf(item, &cell) ? QVariant(cell.originNode) : QVariant();
    }
    case ePurposeInheritedRole:
        return item->data.imageable ? QVariant(!item->data.purposeAuthored) : QVariant();
    case ePurposeOriginRole:
        return item->data.imageable ? QVariant(QString::fromStdString(item->purpose.origin.GetString())) : QVariant();
    case Qt::ToolTipRole: {
        if (item->path.IsEmpty()) {
            return QVariant();
        }
        if ((index.column() == eColumnPurpose) && item->data.imageable) {
            return purposeTip(QString::fromStdString(item->purpose.purpose.GetString()), !item->data.purposeAuthored, QString::fromStdString(item->purpose.origin.GetString()));
        }
        Implementation::DrawCell cell;
        if ((index.column() == eColumnDrawState) && _imp->drawCellOf(item, &cell)) {
            return drawOriginTip(cell.origin, cell.originNode);
        }
        QString tip = _imp->changedHere.contains(item->pathString) ? tr("%1\nChanged here").arg(item->pathString) : item->pathString;
        switch (_imp->payloadStateOf(item)) {
        case ePayloadStateNone:
            break;
        case ePayloadStateUnloaded:
            tip += QLatin1Char('\n') + tr("Payload not loaded");
            break;
        case ePayloadStateLoading:
            tip += QLatin1Char('\n') + tr("Payload loading");
            break;
        case ePayloadStateLoaded:
            tip += QLatin1Char('\n') + tr("Payload loaded");
            break;
        case ePayloadStateFailed:
            tip += QLatin1Char('\n') + tr("Payload failed to load: %1").arg(_imp->loadFailures.value(item->pathString));
            break;
        }

        return tip;
    }
    case Qt::DecorationRole:
        if (index.column() == eColumnName) {
            const QIcon icon = iconForType(item->data.type);
            if (!icon.isNull()) {
                return icon;
            }
        }

        return QVariant();
    case ePrimPathRole:
        return item->pathString;
    case eChangedHereRole:
        return _imp->changedHere.contains(item->pathString);
    case ePayloadStateRole:
        return (int)_imp->payloadStateOf(item);
    case eUnknownBoundsRole:
        return item->data.unknownBounds;
    case Qt::FontRole:
        if ((index.column() == eColumnName) && _imp->changedHere.contains(item->pathString)) {
            QFont font;
            font.setBold(true);

            return font;
        }
        if (index.column() == eColumnDrawState) {
            Implementation::DrawCell cell;
            if (_imp->drawCellOf(item, &cell) && cell.inherited) {
                QFont font;
                font.setItalic(true);

                return font;
            }
        }
        if ((index.column() == eColumnPurpose) && item->data.imageable && !item->data.purposeAuthored) {
            QFont font;
            font.setItalic(true);

            return font;
        }

        return QVariant();
    default:
        return QVariant();
    }
}

QVariant
SceneGraphModel::headerData(int section,
                            Qt::Orientation orientation,
                            int role) const
{
    return columnHeader(section, orientation, role);
}

Qt::ItemFlags
SceneGraphModel::flags(const QModelIndex& index) const
{
    if (!index.isValid() || _imp->itemOf(index)->path.IsEmpty()) {
        return Qt::NoItemFlags;
    }
    const Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;

    return ((index.column() == eColumnDrawState) && _imp->drawStatesReader && _imp->drawStateWriter) ? (flags | Qt::ItemIsEditable) : flags;
}

bool
SceneGraphModel::setData(const QModelIndex& index,
                         const QVariant& value,
                         int role)
{
    if ((role != Qt::EditRole) || (index.column() != eColumnDrawState) || !(flags(index) & Qt::ItemIsEditable)) {
        return false;
    }
    bool isInt = false;
    const int state = value.toInt(&isInt);
    if (!isInt || (state < -1) || (state > (int)eSceneDrawStateHidden)) {
        return false;
    }
    // The viewer's change comes back through refreshDrawStates().
    _imp->drawStateWriter(_imp->itemOf(index)->pathString, state);

    return true;
}

bool
SceneGraphModel::canFetchMore(const QModelIndex& parent) const
{
    if ((parent.isValid() && (parent.column() != 0)) || _imp->changing) {
        return false;
    }
    const Item* item = _imp->itemOf(parent);
    // Asking again would only find the stage busy again; the owed read adds the rows.
    if (!item->data.hasChildren || _imp->owedFetches.count(item->path) || _imp->fetchingUnder.count(item->path)) {
        return false;
    }

    return !item->namesRead || (item->children.size() < item->childNames->size());
}

bool
SceneGraphModel::hasFetchedAll(const QModelIndex& parent) const
{
    if (parent.isValid() && (parent.column() != 0)) {
        return true;
    }
    const Item* item = _imp->itemOf(parent);
    if (!item->data.hasChildren) {
        return true;
    }
    if (_imp->owedFetches.count(item->path) || _imp->fetchingUnder.count(item->path)) {
        return false;
    }

    return item->namesRead && (item->children.size() >= item->childNames->size());
}

void
SceneGraphModel::fetchMore(const QModelIndex& parent)
{
    if (!canFetchMore(parent)) {
        return;
    }
    if (_imp->readsOnWorker) {
        _imp->postFetchMore(_imp->itemOf(parent));
    } else {
        _imp->fetch(_imp->itemOf(parent), 0);
    }
}

QStringList
SceneGraphModel::mimeTypes() const
{
    return QStringList() << primPathsMimeType() << QString::fromUtf8("text/plain");
}

QMimeData*
SceneGraphModel::mimeData(const QModelIndexList& indexes) const
{
    QStringList paths;
    QSet<const Item*> seen;

    for (const QModelIndex& index : indexes) {
        if (!index.isValid()) {
            continue;
        }
        const Item* item = _imp->itemOf(index);
        if (!seen.contains(item)) {
            seen.insert(item);
            paths << item->pathString;
        }
    }

    return SceneGraphModel::mimeDataOfPaths(paths);
}

Qt::DropActions
SceneGraphModel::supportedDragActions() const
{
    return Qt::CopyAction;
}

bool
SceneChangedHereRows::hasSameRows(const SceneChangedHereRows& other) const
{
    return rows == other.rows;
}

SceneChangedHereRowsBuilder::SceneChangedHereRowsBuilder(const QSet<QString>& paths)
    : _paths(paths.values())
    , _rows(std::make_shared<SceneChangedHereRows>())
{
    // A parent's path is a prefix of its children's, so it sorts first and is read before them.
    std::sort(_paths.begin(), _paths.end());
}

SceneChangedHereRowsBuilder::~SceneChangedHereRowsBuilder() = default;

bool
SceneChangedHereRowsBuilder::isDone() const
{
    return _next >= _paths.size();
}

void
SceneChangedHereRowsBuilder::readBatch(const SceneStage* stage)
{
    const UsdStageRefPtr usdStage = stage ? stage->getStage() : UsdStageRefPtr();

    for (int i = 0; (i < kBatchSize) && !isDone(); ++i) {
        const QString& path = _paths.at(_next++);
        const int separator = (int)path.lastIndexOf(QLatin1Char('/'));
        int parent = -1;

        if (separator > 0) {
            const auto found = _rows->rowOfPath.constFind(path.left(separator));
            if (found == _rows->rowOfPath.constEnd()) {
                continue;
            }
            parent = found.value();
        }
        const std::string pathString = path.toStdString();
        const UsdPrim prim = (usdStage && SdfPath::IsValidPathString(pathString)) ? usdStage->GetPrimAtPath(SdfPath(pathString)) : UsdPrim();
        if (!prim || !prim.IsActive() || !prim.IsDefined() || prim.IsAbstract()) {
            continue;
        }
        const PrimData data = readPrimData(prim);
        const int row = (int)_rows->rows.size();
        SceneChangedHereRow added;

        added.path = path;
        added.name = data.name;
        added.type = data.type;
        added.kind = data.kind;
        // The marked paths come with their ancestors, so a parent row is the prim's parent.
        PurposeCell parentPurpose;
        if (parent >= 0) {
            const SceneChangedHereRow& above = _rows->rows[parent];
            parentPurpose.purpose = TfToken(above.purpose.toStdString());
            parentPurpose.inheritable = above.purposeInheritable;
            parentPurpose.origin = above.purposeOrigin.isEmpty() ? SdfPath() : SdfPath(above.purposeOrigin.toStdString());
        }
        const PurposeCell purpose = derivePurpose(data, prim.GetPath(), (parent >= 0) ? &parentPurpose : nullptr);
        added.imageable = data.imageable;
        added.purpose = QString::fromStdString(purpose.purpose.GetString());
        added.purposeInherited = !data.purposeAuthored;
        added.purposeInheritable = purpose.inheritable;
        added.purposeOrigin = QString::fromStdString(purpose.origin.GetString());
        added.parent = parent;
        std::vector<int>& siblings = (parent >= 0) ? _rows->rows[parent].children : _rows->topLevel;
        added.rowInParent = (int)siblings.size();
        siblings.push_back(row);
        _rows->rows.push_back(std::move(added));
        _rows->rowOfPath.insert(path, row);
    }
}

SceneChangedHereRowsPtr
SceneChangedHereRowsBuilder::take()
{
    SceneChangedHereRowsPtr rows = std::move(_rows);

    _rows = std::make_shared<SceneChangedHereRows>();

    return rows;
}

SceneChangedHereModel::SceneChangedHereModel(QObject* parent)
    : QAbstractItemModel(parent)
{
}

SceneChangedHereModel::~SceneChangedHereModel() = default;

void
SceneChangedHereModel::setRows(const SceneChangedHereRowsPtr& rows)
{
    beginResetModel();
    _rows = rows;
    endResetModel();
}

const SceneChangedHereRowsPtr&
SceneChangedHereModel::getRows() const
{
    return _rows;
}

int
SceneChangedHereModel::getRowCount() const
{
    return _rows ? (int)_rows->rows.size() : 0;
}

bool
SceneChangedHereModel::contains(const QString& path) const
{
    return _rows && _rows->rowOfPath.contains(path);
}

QModelIndex
SceneChangedHereModel::indexOfPath(const QString& path) const
{
    if (!contains(path)) {
        return QModelIndex();
    }
    const int row = _rows->rowOfPath.value(path);

    return createIndex(_rows->rows[row].rowInParent, 0, (quintptr)row);
}

QModelIndex
SceneChangedHereModel::index(int row,
                             int column,
                             const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent)) {
        return QModelIndex();
    }
    const std::vector<int>& siblings = parent.isValid() ? _rows->rows[parent.internalId()].children : _rows->topLevel;

    return createIndex(row, column, (quintptr)siblings[row]);
}

QModelIndex
SceneChangedHereModel::parent(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return QModelIndex();
    }
    const int parentRow = _rows->rows[index.internalId()].parent;

    if (parentRow < 0) {
        return QModelIndex();
    }

    return createIndex(_rows->rows[parentRow].rowInParent, 0, (quintptr)parentRow);
}

int
SceneChangedHereModel::rowCount(const QModelIndex& parent) const
{
    if (!_rows || (parent.column() > 0)) {
        return 0;
    }

    return (int)(parent.isValid() ? _rows->rows[parent.internalId()].children.size() : _rows->topLevel.size());
}

int
SceneChangedHereModel::columnCount(const QModelIndex& /*parent*/) const
{
    return SceneGraphModel::eColumnCount;
}

QVariant
SceneChangedHereModel::data(const QModelIndex& index,
                            int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }
    const SceneChangedHereRow& row = _rows->rows[index.internalId()];

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case SceneGraphModel::eColumnName:
            return row.name;
        case SceneGraphModel::eColumnType:
            return row.type;
        case SceneGraphModel::eColumnKind:
            return row.kind;
        case SceneGraphModel::eColumnPurpose:
            return row.imageable ? QVariant(row.purpose) : QVariant();
        default:
            return QVariant();
        }
    case SceneGraphModel::ePurposeInheritedRole:
        return row.imageable ? QVariant(row.purposeInherited) : QVariant();
    case SceneGraphModel::ePurposeOriginRole:
        return row.imageable ? QVariant(row.purposeOrigin) : QVariant();
    case Qt::ToolTipRole:
        if ((index.column() == SceneGraphModel::eColumnPurpose) && row.imageable) {
            return SceneGraphModel::purposeTip(row.purpose, row.purposeInherited, row.purposeOrigin);
        }

        return SceneGraphModel::tr("%1\nChanged here").arg(row.path);
    case Qt::DecorationRole:
        if (index.column() == SceneGraphModel::eColumnName) {
            const QIcon icon = iconForType(row.type);
            if (!icon.isNull()) {
                return icon;
            }
        }

        return QVariant();
    case SceneGraphModel::ePrimPathRole:
        return row.path;
    case SceneGraphModel::eChangedHereRole:
        return true;
    case Qt::FontRole:
        if (index.column() == SceneGraphModel::eColumnName) {
            QFont font;
            font.setBold(true);

            return font;
        }
        if ((index.column() == SceneGraphModel::eColumnPurpose) && row.imageable && row.purposeInherited) {
            QFont font;
            font.setItalic(true);

            return font;
        }

        return QVariant();
    default:
        return QVariant();
    }
}

QVariant
SceneChangedHereModel::headerData(int section,
                                  Qt::Orientation orientation,
                                  int role) const
{
    return columnHeader(section, orientation, role);
}

Qt::ItemFlags
SceneChangedHereModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QStringList
SceneChangedHereModel::mimeTypes() const
{
    return QStringList() << SceneGraphModel::primPathsMimeType() << QString::fromUtf8("text/plain");
}

QMimeData*
SceneChangedHereModel::mimeData(const QModelIndexList& indexes) const
{
    QStringList paths;
    QSet<quintptr> seen;

    for (const QModelIndex& index : indexes) {
        if (index.isValid() && !seen.contains(index.internalId())) {
            seen.insert(index.internalId());
            paths << _rows->rows[index.internalId()].path;
        }
    }

    return SceneGraphModel::mimeDataOfPaths(paths);
}

Qt::DropActions
SceneChangedHereModel::supportedDragActions() const
{
    return Qt::CopyAction;
}

namespace {

// What the filter's expression is evaluated on. The walk visits prims only, so every path is a prim's.
// The predicates of node patterns, and the load and active states the scene graph shows: kept out
// of nodes, whose output must not depend on what a viewer has loaded.
const UsdObjectPredicateLibrary&
filterPredicateLibrary()
{
    static const UsdObjectPredicateLibrary library = []() {
        UsdObjectPredicateLibrary result = ScenePredicates::library();
        // USD reports an inactive prim as not loaded, but it has no payload to load; only an
        // active prim's own unloaded payload makes it unloaded.
        auto isLoaded = [](const UsdObject& object, bool loaded) {
            const UsdPrim prim = object.GetPrim();

            if (!prim || !prim.IsActive()) {
                return false;
            }

            return loaded ? prim.IsLoaded() : isUnloadedPayload(prim);
        };
        auto isActive = [](const UsdObject& object, bool active) {
            const UsdPrim prim = object.GetPrim();

            return prim && (prim.IsActive() == active);
        };
        result.Define("loaded", std::move(isLoaded), { { "loaded", true } });
        result.Define("active", std::move(isActive), { { "active", true } });

        return result;
    }();

    return library;
}

} // anonymous namespace

struct SceneGraphPrimWalk::Implementation {
    int primsPerClockRead = kPrimsPerClockRead;
    SdfPath next;
    const SceneStage* stage = nullptr;
    U64 editCount = 0;
    int unloadedBranches = 0;
    QStringList unloadedPaths;
    bool hasExpression = false;
    ScenePredicates::PathEval eval;
    // Valid from the walk's start on stage; it holds where the depth-first search is.
    ScenePredicates::Searcher searcher;
};

SceneGraphPrimWalk::SceneGraphPrimWalk(int primsPerClockRead)
    : _imp(new Implementation)
{
    _imp->primsPerClockRead = primsPerClockRead;
}

SceneGraphPrimWalk::~SceneGraphPrimWalk()
{
}

void
SceneGraphPrimWalk::start()
{
    _imp->next = SdfPath::AbsoluteRootPath();
    _imp->stage = nullptr;
    _imp->unloadedBranches = 0;
    _imp->unloadedPaths.clear();
}

void
SceneGraphPrimWalk::stop()
{
    _imp->next = SdfPath();
    _imp->stage = nullptr;
}

bool
SceneGraphPrimWalk::isOver() const
{
    return _imp->next.IsEmpty();
}

bool
SceneGraphPrimWalk::setExpression(const QString& text,
                                  QString* errorMessage)
{
    _imp->stage = nullptr;
    _imp->searcher = ScenePredicates::Searcher();
    _imp->eval = ScenePredicates::PathEval();
    _imp->hasExpression = !text.trimmed().isEmpty();
    if (!_imp->hasExpression) {
        return true;
    }
    SdfPathExpression parsed;
    std::string error;
    if (!SceneOps::parsePrimExpression(text.toStdString(), &parsed, &error)) {
        *errorMessage = QString::fromStdString(error);

        return false;
    }
    if (!ScenePredicates::link(parsed, filterPredicateLibrary(), &_imp->eval, &error)) {
        *errorMessage = QString::fromUtf8("\"%1\" can't be evaluated: %2").arg(text, QString::fromStdString(error));

        return false;
    }

    return true;
}

bool
SceneGraphPrimWalk::hasExpression() const
{
    return _imp->hasExpression;
}

int
SceneGraphPrimWalk::getUnloadedBranchCount() const
{
    return _imp->unloadedBranches;
}

QStringList
SceneGraphPrimWalk::getUnloadedBranchPaths() const
{
    return _imp->unloadedPaths;
}

void
SceneGraphPrimWalk::step(const SceneStage* stage,
                         int budgetMs,
                         int cap,
                         const std::function<bool(const QString& name)>& matches,
                         QStringList* found)
{
    if (!stage) {
        stop();

        return;
    }
    if (isOver()) {
        return;
    }
    const UsdStageRefPtr& usdStage = stage->getStage();
    UsdPrim prim;
    if ((stage == _imp->stage) && (stage->getEditCount() == _imp->editCount)) {
        prim = usdStage->GetPrimAtPath(_imp->next);
    }
    if (!prim) {
        found->clear();
        prim = usdStage->GetPseudoRoot();
        _imp->stage = stage;
        _imp->editCount = stage->getEditCount();
        _imp->unloadedBranches = 0;
        _imp->unloadedPaths.clear();
        if (_imp->hasExpression) {
            _imp->searcher = _imp->eval.MakeIncrementalSearcher(ScenePredicates::PrimOfPath { UsdStageWeakPtr(usdStage) });
        }
    }

    const bool byExpression = _imp->hasExpression;
    if (byExpression && _imp->eval.IsEmpty()) {
        stop();

        return;
    }
    const Usd_PrimFlagsPredicate predicate = childPredicate();
    QElapsedTimer timer;
    timer.start();
    int visited = 0;
    while (prim) {
        const bool isRoot = prim.IsPseudoRoot();
        const bool listed = isRoot || predicate(prim);
        const bool candidate = !isRoot && (listed || (byExpression && prim.IsDefined() && !prim.IsAbstract() && !prim.IsActive()));
        // Not checked after adding a path: "more" must mean that a prim to match is left.
        if (candidate && (found->size() >= cap)) {
            break;
        }
        bool descend = listed;
        if (candidate) {
            if (byExpression) {
                const SdfPredicateFunctionResult result = _imp->searcher.Next(prim.GetPath());
                if (result.GetValue()) {
                    *found << QString::fromStdString(prim.GetPath().GetString());
                } else if (result.IsConstant()) {
                    descend = false;
                }
            } else if (matches(QString::fromStdString(prim.GetName().GetString()))) {
                *found << QString::fromStdString(prim.GetPath().GetString());
            }
            if (descend && isUnloadedPayload(prim)) {
                ++_imp->unloadedBranches;
                _imp->unloadedPaths << QString::fromStdString(prim.GetPath().GetString());
            }
        }
        prim = nextPrimOfWalk(prim, descend);
        if (((++visited % _imp->primsPerClockRead) == 0) && (timer.elapsed() >= budgetMs)) {
            break;
        }
    }
    _imp->next = prim ? prim.GetPath() : SdfPath();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SceneGraphModel.cpp"
