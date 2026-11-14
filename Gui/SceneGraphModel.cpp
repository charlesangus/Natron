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
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QtCore/QElapsedTimer>
#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtGui/QFont>
#include <QtGui/QIcon>

#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

#ifdef emit
#define NATRON_SCENEGRAPHMODEL_RESTORE_EMIT
#undef emit
#endif
#include <pxr/usd/usdGeom/modelAPI.h>
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
    bool hasChildren = false;
    // None, Unloaded or Loaded: the other states belong to the model's requests, not the prim.
    SceneGraphModel::PayloadStateEnum payload = SceneGraphModel::ePayloadStateNone;
    bool unknownBounds = false;

    bool operator==(const PrimData& other) const
    {
        return (name == other.name) && (type == other.type) && (kind == other.kind) && (hasChildren == other.hasChildren) && (payload == other.payload) && (unknownBounds == other.unknownBounds);
    }
};

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
            if (path.IsAbsoluteRootOrPrimPath() && !overflow) {
                resynced.push_back(path);
            }
        }
        for (const SdfPath& path : notice.GetChangedInfoOnlyPaths()) {
            if (path.IsPrimPath() && !overflow) {
                infoOnly.push_back(path);
            }
        }
        if ((resynced.size() + infoOnly.size()) > kMaxPendingPaths) {
            overflow = true;
            resynced.clear();
            infoOnly.clear();
        }
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

struct SceneGraphModelItem {
    SceneGraphModelItem* parent = nullptr;
    int row = 0;
    SdfPath path;
    TfToken nameToken;
    QString pathString;
    PrimData data;
    // Whether childNames holds the prim's children; children are the first of them.
    bool namesRead = false;
    TfTokenVector childNames;
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

    int subtreeSize() const
    {
        int size = (int)children.size();

        for (const std::unique_ptr<SceneGraphModelItem>& child : children) {
            size += child->subtreeSize();
        }

        return size;
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
    TfTokenVector names;
    // How many of names are loaded after the update.
    std::size_t loadedCount = 0;
    std::unordered_map<TfToken, PrimData, TfToken::HashFunctor> newChildData;
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
    }

    void emitChangedHere(Item* item)
    {
        if (item->children.empty()) {
            return;
        }
        Q_EMIT model->dataChanged(model->createIndex(0, 0, item->children.front().get()),
                                  model->createIndex((int)item->children.size() - 1, eColumnCount - 1, item->children.back().get()));
        for (const std::unique_ptr<Item>& child : item->children) {
            emitChangedHere(child.get());
        }
    }

    void emitDrawStates(Item* item)
    {
        if (item->children.empty()) {
            return;
        }
        Q_EMIT model->dataChanged(model->createIndex(0, eColumnDrawState, item->children.front().get()),
                                  model->createIndex((int)item->children.size() - 1, eColumnDrawState, item->children.back().get()));
        for (const std::unique_ptr<Item>& child : item->children) {
            emitDrawStates(child.get());
        }
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

    std::unique_ptr<Item> makeChild(Item* parent,
                                    const TfToken& name,
                                    const PrimData& data)
    {
        std::unique_ptr<Item> child = makeItem(parent->path, name, data);

        child->parent = parent;

        return child;
    }

    // Reads item and, when its children were read, them again; recurses into the kept children
    // when deep, or into the ones named in deepNames. Needs the stage's lock.
    void gatherUpdate(const UsdStageRefPtr& stage,
                      Item* item,
                      bool deep,
                      const std::unordered_set<TfToken, TfToken::HashFunctor>& deepNames,
                      std::vector<ItemUpdate>* updates)
    {
        const UsdPrim prim = stage->GetPrimAtPath(item->path);
        ItemUpdate update;

        update.item = item;
        update.data = readPrimData(prim);
        if (!item->namesRead) {
            updates->push_back(std::move(update));

            return;
        }
        update.readChildren = true;
        if (prim) {
            update.names = prim.GetFilteredChildrenNames(childPredicate());
        }
        std::unordered_map<TfToken, std::size_t, TfToken::HashFunctor> newPosition;
        for (std::size_t i = 0; i < update.names.size(); ++i) {
            newPosition[update.names[i]] = i;
        }
        std::vector<Item*> kept;
        std::size_t lastKeptPosition = 0;
        bool anyKept = false;
        for (const std::unique_ptr<Item>& child : item->children) {
            std::unordered_map<TfToken, std::size_t, TfToken::HashFunctor>::const_iterator found = newPosition.find(child->nameToken);
            if (found == newPosition.end()) {
                continue;
            }
            if (anyKept && (found->second <= lastKeptPosition)) {
                update.replaceChildren = true;
            }
            lastKeptPosition = found->second;
            anyKept = true;
            kept.push_back(child.get());
        }
        update.loadedCount = std::min(item->children.size(), update.names.size());
        if (anyKept && !update.replaceChildren) {
            update.loadedCount = std::max(update.loadedCount, lastKeptPosition + 1);
        }
        // A view fetches when a row opens, and this row may have opened before its payload's
        // children existed.
        if ((item->data.payload == ePayloadStateUnloaded) && (update.data.payload != ePayloadStateUnloaded)) {
            update.loadedCount = std::max(update.loadedCount, std::min(update.names.size(), (std::size_t)kFetchBatchSize));
        }
        for (std::size_t i = 0; i < update.loadedCount; ++i) {
            const TfToken& name = update.names[i];
            if (update.replaceChildren || !item->findChild(name)) {
                update.newChildData[name] = readPrimData(stage->GetPrimAtPath(item->path.AppendChild(name)));
            }
        }
        const bool replace = update.replaceChildren;
        updates->push_back(std::move(update));
        if (replace) {
            return;
        }
        const std::unordered_set<TfToken, TfToken::HashFunctor> none;
        for (Item* child : kept) {
            if (deep || (deepNames.find(child->nameToken) != deepNames.end())) {
                gatherUpdate(stage, child, true, none, updates);
            }
        }
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
            } else {
                item->data.hasChildren = update.data.hasChildren;
            }
        }
        if (!update.readChildren) {
            return childrenFlagChanged && item->children.empty();
        }
        std::unordered_set<TfToken, TfToken::HashFunctor> newNames(update.names.begin(), update.names.end());
        if (update.replaceChildren) {
            newNames.clear();
        }
        // Removes the gone children, last first, a run of rows at a time.
        int last = (int)item->children.size() - 1;
        while (last >= 0) {
            if (newNames.find(item->children[last]->nameToken) != newNames.end()) {
                --last;
                continue;
            }
            int first = last;
            while ((first > 0) && (newNames.find(item->children[first - 1]->nameToken) == newNames.end())) {
                --first;
            }
            model->beginRemoveRows(index, first, last);
            for (int i = first; i <= last; ++i) {
                itemCount -= 1 + item->children[i]->subtreeSize();
            }
            item->children.erase(item->children.begin() + first, item->children.begin() + last + 1);
            item->renumber();
            model->endRemoveRows();
            last = first - 1;
        }
        // Inserts the new names among the kept children, a run of rows at a time.
        std::size_t row = 0;
        std::size_t i = 0;
        while (i < update.loadedCount) {
            if ((row < item->children.size()) && (item->children[row]->nameToken == update.names[i])) {
                ++row;
                ++i;
                continue;
            }
            std::size_t end = i;
            while ((end < update.loadedCount) && !((row < item->children.size()) && (item->children[row]->nameToken == update.names[end]))) {
                ++end;
            }
            model->beginInsertRows(index, (int)row, (int)(row + (end - i) - 1));
            std::vector<std::unique_ptr<Item>> inserted;
            for (std::size_t n = i; n < end; ++n) {
                inserted.push_back(makeChild(item, update.names[n], update.newChildData[update.names[n]]));
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
        item->childNames = std::move(update.names);

        return childrenFlagChanged && item->children.empty();
    }

    void applyUpdates(std::vector<ItemUpdate>& updates)
    {
        ChangeScope scope(this);
        bool layoutChanged = false;

        for (ItemUpdate& update : updates) {
            if (applyUpdate(update)) {
                layoutChanged = true;
            }
        }
        if (layoutChanged) {
            // Views keep whether an item without rows has children; only a relayout asks again.
            Q_EMIT model->layoutAboutToBeChanged();
            Q_EMIT model->layoutChanged();
        }
    }

    void resetTo(bool hasStage,
                 const PrimData& rootData)
    {
        ChangeScope scope(this);
        model->beginResetModel();
        root.children.clear();
        root.childNames.clear();
        root.namesRead = false;
        root.rowOfValid = false;
        root.data = hasStage ? rootData : PrimData();
        loadingPaths.clear();
        loadFailures.clear();
        owedFetches.clear();
        itemCount = 0;
        ++resetCount;
        model->endResetModel();
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

    // Reads the next batch of item's children. Returns whether any were added.
    bool fetch(Item* item,
               std::size_t upTo,
               bool* busy = nullptr)
    {
        if (!source || changing) {
            return false;
        }
        std::vector<std::unique_ptr<Item>> fetched;
        bool namesRead = item->namesRead;
        TfTokenVector names;
        const bool read = source->tryReadStage([&](const SceneStage* sceneStage) {
            if (!sceneStage || !isListened(sceneStage->getStage())) {
                return;
            }
            const UsdStageRefPtr& stage = sceneStage->getStage();
            if (!namesRead) {
                const UsdPrim prim = stage->GetPrimAtPath(item->path);
                if (prim) {
                    names = prim.GetFilteredChildrenNames(childPredicate());
                }
                namesRead = true;
            }
            const TfTokenVector& allNames = item->namesRead ? item->childNames : names;
            const std::size_t end = std::min(allNames.size(), std::max(upTo, item->children.size() + kFetchBatchSize));
            for (std::size_t i = item->children.size(); i < end; ++i) {
                const SdfPath path = item->path.AppendChild(allNames[i]);
                fetched.push_back(makeChild(item, allNames[i], readPrimData(stage->GetPrimAtPath(path))));
            }
        });
        if (!read) {
            oweFetch(item, upTo);
            if (busy) {
                *busy = true;
            }

            return false;
        }
        ChangeScope scope(this);
        if (namesRead && !item->namesRead) {
            item->namesRead = true;
            item->childNames = std::move(names);
        }
        if (fetched.empty()) {
            return false;
        }
        const int first = (int)item->children.size();
        model->beginInsertRows(indexOf(item), first, first + (int)fetched.size() - 1);
        itemCount += (int)fetched.size();
        for (std::unique_ptr<Item>& child : fetched) {
            child->row = (int)item->children.size();
            item->children.push_back(std::move(child));
        }
        item->placeLoadingRow();
        item->rowOfValid = false;
        model->endInsertRows();

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
    static const QIcon material(QString::fromUtf8(":/Resources/Images/NativeNodes/USDMaterial.png"));
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
    if (_imp->source) {
        // Notices are sent by whoever edits the stage, under its lock.
        _imp->source->readStage([this](const SceneStage*) { _imp->stopListening(); });
    }
    for (Implementation::RetiredListener& retired : _imp->retiredListeners) {
        retired.source->readStage([&retired](const SceneStage*) { retired.revoke(); });
    }
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
    _imp->changedHere.clear();
    _imp->resetTo(false, PrimData());
    refresh();
}

const SceneGraphStageSourcePtr&
SceneGraphModel::getSource() const
{
    return _imp->source;
}

void
SceneGraphModel::refresh()
{
    _imp->reapRetiredListeners();
    if (_imp->source && readEdits()) {
        retryOwedReads();
    }
}

bool
SceneGraphModel::hasOwedReads() const
{
    return _imp->settleOwed || !_imp->owedFetches.empty();
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
    const U64 editCount = _imp->source->getEditCount();
    if (_imp->listener && (editCount == _imp->lastEditCount) && !_imp->listener->hasPending()) {
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
    std::vector<ItemUpdate> updates;
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
        SdfPathVector resynced;
        SdfPathVector infoOnly;
        bool overflow;
        {
            std::lock_guard<std::mutex> lock(_imp->listener->mutex);
            resynced.swap(_imp->listener->resynced);
            infoOnly.swap(_imp->listener->infoOnly);
            overflow = _imp->listener->overflow;
            _imp->listener->overflow = false;
        }
        action = eUpdate;
        const std::unordered_set<TfToken, TfToken::HashFunctor> none;
        if (overflow) {
            _imp->gatherUpdate(stage, &_imp->root, true, none, &updates);

            return;
        }
        SdfPath::RemoveDescendentPaths(&resynced);
        // Each resynced path changes its parent's children and everything under it.
        std::map<SdfPath, std::unordered_set<TfToken, TfToken::HashFunctor>> byParent;
        bool rootResynced = false;
        for (const SdfPath& path : resynced) {
            if (path == SdfPath::AbsoluteRootPath()) {
                rootResynced = true;
                break;
            }
            byParent[path.GetParentPath()].insert(path.GetNameToken());
        }
        if (rootResynced) {
            _imp->gatherUpdate(stage, &_imp->root, true, none, &updates);

            return;
        }
        for (const std::pair<const SdfPath, std::unordered_set<TfToken, TfToken::HashFunctor>>& entry : byParent) {
            if (Item* item = _imp->findItem(entry.first)) {
                _imp->gatherUpdate(stage, item, false, entry.second, &updates);
            }
        }
        for (const SdfPath& path : infoOnly) {
            if (SdfPathFindLongestPrefix(resynced.begin(), resynced.end(), path) != resynced.end()) {
                continue;
            }
            Item* item = _imp->findItem(path);
            if (item && (item != &_imp->root)) {
                ItemUpdate update;
                update.item = item;
                update.data = readPrimData(stage->GetPrimAtPath(path));
                updates.push_back(std::move(update));
            }
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
        _imp->applyUpdates(updates);
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
        if (!child && populate) {
            if (!item->namesRead) {
                _imp->fetch(item, 0, busy);
            }
            const TfTokenVector::const_iterator found = std::find(item->childNames.begin(), item->childNames.end(), name);
            if (found == item->childNames.end()) {
                return QModelIndex();
            }
            _imp->fetch(item, (std::size_t)(found - item->childNames.begin()) + 1, busy);
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
        if (!item || !parent.namesRead || (item->namesRead && (item->childNames != parent.names))) {
            continue;
        }
        Implementation::ChangeScope scope(_imp.get());
        if (!item->namesRead) {
            item->namesRead = true;
            item->childNames = std::move(parent.names);
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
    for (const QString& path : _imp->loadingPaths) {
        if (!outstanding || failures.contains(path)) {
            touched.insert(path);
        }
    }
    _imp->loadingPaths -= touched;
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
    for (const QString& path : touched) {
        if (Item* item = _imp->findItem(path)) {
            if (item != &_imp->root) {
                _imp->removeLoadingRow(item);
                _imp->emitRowChanged(item);
            }
        }
    }
}

void
SceneGraphModel::setChangedHere(const QSet<QString>& paths)
{
    if (paths == _imp->changedHere) {
        return;
    }
    _imp->changedHere = paths;
    _imp->emitChangedHere(&_imp->root);
}

QString
SceneGraphModel::drawStateLabel(int state)
{
    switch (state) {
    case eSceneDrawStateBBox:
        return tr("BBox");
    case eSceneDrawStateChildren:
        return tr("Children");
    case eSceneDrawStateFull:
        return tr("Full");
    case eSceneDrawStateLoadAll:
        return tr("Load All");
    case eSceneDrawStateHidden:
        return tr("Hidden");
    default:
        return QString();
    }
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
    _imp->emitDrawStates(&_imp->root);
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
    case Qt::ToolTipRole: {
        if (item->path.IsEmpty()) {
            return QVariant();
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
    if (!item->data.hasChildren || _imp->owedFetches.count(item->path)) {
        return false;
    }

    return !item->namesRead || (item->children.size() < item->childNames.size());
}

void
SceneGraphModel::fetchMore(const QModelIndex& parent)
{
    if (canFetchMore(parent)) {
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
        default:
            return QVariant();
        }
    case Qt::ToolTipRole:
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

struct SceneGraphPrimWalk::Implementation {
    int primsPerClockRead = kPrimsPerClockRead;
    SdfPath next;
    const SceneStage* stage = nullptr;
    U64 editCount = 0;
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
    }

    const Usd_PrimFlagsPredicate predicate = childPredicate();
    QElapsedTimer timer;
    timer.start();
    int visited = 0;
    while (prim) {
        const bool isRoot = prim.IsPseudoRoot();
        const bool listed = isRoot || predicate(prim);
        // Not checked after adding a path: "more" must mean that a listed prim is left.
        if (listed && (found->size() >= cap)) {
            break;
        }
        if (listed && !isRoot && matches(QString::fromStdString(prim.GetName().GetString()))) {
            *found << QString::fromStdString(prim.GetPath().GetString());
        }
        prim = nextPrimOfWalk(prim, listed);
        if (((++visited % _imp->primsPerClockRead) == 0) && (timer.elapsed() >= budgetMs)) {
            break;
        }
    }
    _imp->next = prim ? prim.GetPath() : SdfPath();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SceneGraphModel.cpp"
