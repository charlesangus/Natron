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
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtGui/QIcon>

#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// Past this many resynced paths between two refreshes, everything read is read again.
const std::size_t kMaxPendingPaths = 4096;

// Above this many children, finding a child by name goes through a map.
const std::size_t kRowMapThreshold = 32;

Usd_PrimFlagsPredicate
childPredicate()
{
    // Unloaded prims are shown, so a payload can be found before it is loaded.
    return UsdTraverseInstanceProxies(UsdPrimIsActive && UsdPrimIsDefined && !UsdPrimIsAbstract);
}

struct PrimData {
    QString name;
    QString type;
    QString kind;
    bool hasChildren = false;

    bool operator==(const PrimData& other) const
    {
        return (name == other.name) && (type == other.type) && (kind == other.kind) && (hasChildren == other.hasChildren);
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
    data.hasChildren = !prim.GetFilteredChildren(childPredicate()).empty() || (prim.HasAuthoredPayloads() && !prim.IsLoaded());

    return data;
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

// Collects what the stage's edits resynced, on whichever thread edits the stage.
class StageListener
    : public TfWeakBase {
public:
    void onObjectsChanged(const UsdNotice::ObjectsChanged& notice)
    {
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
        rowOfValid = false;
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

struct SceneGraphModel::Implementation {
    SceneGraphModel* model;
    SceneGraphStageSourcePtr source;
    Item root;
    UsdStageWeakPtr listenedStage;
    std::unique_ptr<StageListener> listener;
    TfNotice::Key key;
    U64 lastEditCount = 0;
    int itemCount = 0;
    int resetCount = 0;
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

    // Needs the stage's lock.
    void listen(const UsdStageRefPtr& stage)
    {
        stopListening();
        listener.reset(new StageListener);
        key = TfNotice::Register(TfCreateWeakPtr(listener.get()), &StageListener::onObjectsChanged, UsdStageWeakPtr(stage));
        listenedStage = stage;
    }

    std::unique_ptr<Item> makeChild(Item* parent,
                                    const TfToken& name,
                                    const PrimData& data)
    {
        std::unique_ptr<Item> child(new Item);

        child->parent = parent;
        child->path = parent->path.AppendChild(name);
        child->nameToken = name;
        child->pathString = QString::fromStdString(child->path.GetString());
        child->data = data;

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

        return false;
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
        itemCount = 0;
        ++resetCount;
        model->endResetModel();
    }

    // Reads the next batch of item's children. Returns whether any were added.
    bool fetch(Item* item,
               std::size_t upTo)
    {
        if (!source || changing) {
            return false;
        }
        std::vector<std::unique_ptr<Item>> fetched;
        bool namesRead = item->namesRead;
        TfTokenVector names;
        source->readStage([&](const SceneStage* sceneStage) {
            if (!sceneStage || (listenedStage != sceneStage->getStage())) {
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

QString
SceneGraphModel::primPathsMimeType()
{
    return QString::fromUtf8("application/x-natron-prim-paths");
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
}

void
SceneGraphModel::setSource(const SceneGraphStageSourcePtr& source)
{
    if (source == _imp->source) {
        refresh();

        return;
    }
    if (_imp->source) {
        _imp->source->readStage([this](const SceneStage*) { _imp->stopListening(); });
    }
    _imp->source = source;
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
    if (!_imp->source) {
        return;
    }
    const U64 editCount = _imp->source->getEditCount();
    if (_imp->listener && (editCount == _imp->lastEditCount) && !_imp->listener->hasPending()) {
        return;
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
    _imp->source->readStage([&](const SceneStage* sceneStage) {
        _imp->lastEditCount = editCount;
        if (!sceneStage) {
            if (_imp->listener || _imp->root.data.hasChildren) {
                _imp->stopListening();
                action = eClear;
            }

            return;
        }
        const UsdStageRefPtr& stage = sceneStage->getStage();
        if (!_imp->listener || (_imp->listenedStage != stage)) {
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
}

QModelIndex
SceneGraphModel::indexOfPath(const QString& path,
                             bool populate)
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
                _imp->fetch(item, 0);
            }
            const TfTokenVector::const_iterator found = std::find(item->childNames.begin(), item->childNames.end(), name);
            if (found == item->childNames.end()) {
                return QModelIndex();
            }
            _imp->fetch(item, (std::size_t)(found - item->childNames.begin()) + 1);
            child = item->findChild(name);
        }
        if (!child) {
            return QModelIndex();
        }
        item = child;
    }

    return _imp->indexOf(item);
}

QString
SceneGraphModel::pathOf(const QModelIndex& index) const
{
    return index.isValid() ? _imp->itemOf(index)->pathString : QString();
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

QModelIndex
SceneGraphModel::index(int row,
                       int column,
                       const QModelIndex& parent) const
{
    if ((column < 0) || (column >= eColumnCount) || (parent.isValid() && (parent.column() != 0))) {
        return QModelIndex();
    }
    Item* item = _imp->itemOf(parent);
    if ((row < 0) || (row >= (int)item->children.size())) {
        return QModelIndex();
    }

    return createIndex(row, column, item->children[row].get());
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

    return (int)_imp->itemOf(parent)->children.size();
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

    return !item->children.empty() || item->data.hasChildren;
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
        default:
            return QVariant();
        }
    case Qt::ToolTipRole:
        return item->pathString;
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
    default:
        return QVariant();
    }
}

QVariant
SceneGraphModel::headerData(int section,
                            Qt::Orientation orientation,
                            int role) const
{
    if ((orientation != Qt::Horizontal) || (role != Qt::DisplayRole)) {
        return QVariant();
    }
    switch (section) {
    case eColumnName:
        return tr("Name");
    case eColumnType:
        return tr("Type");
    case eColumnKind:
        return tr("Kind");
    default:
        return QVariant();
    }
}

Qt::ItemFlags
SceneGraphModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

bool
SceneGraphModel::canFetchMore(const QModelIndex& parent) const
{
    if ((parent.isValid() && (parent.column() != 0)) || _imp->changing) {
        return false;
    }
    const Item* item = _imp->itemOf(parent);
    if (!item->data.hasChildren) {
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
    if (paths.isEmpty()) {
        return nullptr;
    }
    QMimeData* mime = new QMimeData;
    const QString joined = paths.join(QLatin1Char('\n'));
    mime->setData(primPathsMimeType(), joined.toUtf8());
    mime->setText(joined);

    return mime;
}

Qt::DropActions
SceneGraphModel::supportedDragActions() const
{
    return Qt::CopyAction;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SceneGraphModel.cpp"
