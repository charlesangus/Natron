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

#ifndef Gui_SceneGraphModel_h
#define Gui_SceneGraphModel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <vector>

#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QStringList>

#include <pxr/pxr.h>

#include "Global/GlobalDefines.h"

// Only pxr.h's namespace macros and forward declarations, like SceneViewport.h: the panel's header
// includes this one, and the USD headers stay out of it.

PXR_NAMESPACE_OPEN_SCOPE
class Usd_PrimFlagsPredicate;
PXR_NAMESPACE_CLOSE_SCOPE

class QIcon;
class QString;

NATRON_NAMESPACE_ENTER

/**
 * @brief The children the scene graph lists: active, defined and not abstract, instance proxies
 * and unloaded payloads included. Whatever lists a prim's children uses it, so they agree.
 **/
PXR_NS::Usd_PrimFlagsPredicate childPredicate();

// The icon the scene graph gives a prim of this type name, null when it has none.
QIcon iconForType(const QString& type);

class SceneDrawStates;
class SceneStage;

/**
 * @brief A composed stage the scene graph shows, and the lock it is read under.
 **/
class SceneGraphStageSource {
public:
    virtual ~SceneGraphStageSource() { }

    /**
     * @brief Calls read with the stage, or null when there is none, holding whatever keeps edits
     * of the stage out until read returns. MT-safe.
     **/
    virtual void readStage(const std::function<void(const SceneStage* stage)>& read) const = 0;

    /**
     * @brief readStage() for the main thread, which must not wait for a load to commit: returns
     * false without calling read while an edit holds the lock. The edit's end is announced by a
     * signal of whoever owns the stage, on which to try again. MT-safe.
     **/
    virtual bool tryReadStage(const std::function<void(const SceneStage* stage)>& read) const = 0;

    // Moves whenever the stage is edited; cheap, without the lock.
    virtual U64 getEditCount() const = 0;
};

typedef std::shared_ptr<SceneGraphStageSource> SceneGraphStageSourcePtr;

/**
 * @brief A walk over a stage's prims in the order the scene graph lists them: a prim, then its
 * children, in the stage's order. It runs in slices with the stage's lock released between them,
 * so only the next prim's path outlives a slice: a recomposition can invalidate any UsdPrim or
 * sibling iterator. Moving along USD's sibling links never lists a prim's children whole, so the
 * budget also holds inside a prim with millions of them, and finding the path again costs the same
 * however far along the list the walk is.
 **/
class SceneGraphPrimWalk {
public:
    static constexpr int kPrimsPerClockRead = 256;

    explicit SceneGraphPrimWalk(int primsPerClockRead = kPrimsPerClockRead);

    ~SceneGraphPrimWalk();

    // The next step() starts at the stage's root.
    void start();

    void stop();

    bool isOver() const;

    /**
     * @brief Makes step() match prims with text, a USD path expression as a prim pattern knob takes
     * it (SceneOps::parsePrimExpression()), rather than by name; empty text goes back to names. The
     * expression's predicates are USD's collection predicates and two more, {loaded} and {active},
     * each taking false as in {loaded:false}. Inactive prims are matched too, so that
     * {active:false} can find them; the walk never goes below one. The search is incremental: a
     * prim under which the expression can't match has its children skipped. Returns false with
     * *errorMessage set, matching nothing, when text doesn't parse or names a predicate the library
     * lacks. The next step() starts over.
     **/
    bool setExpression(const QString& text, QString* errorMessage);

    bool hasExpression() const;

    /**
     * @brief How many prims with an unloaded payload the walk has passed since it last started over,
     * under which it could not look although it might have matched there.
     **/
    int getUnloadedBranchCount() const;

    /**
     * @brief The paths of the branches getUnloadedBranchCount() counts, in the order the walk
     * passed them.
     **/
    QStringList getUnloadedBranchPaths() const;

    /**
     * @brief Adds to found the paths of the prims the expression matches, or without one those
     * whose names satisfy matches, until budgetMs is spent, or found holds cap paths with another
     * prim to match next, or no prim is left. At least one prim is looked at. Called with stage's
     * lock held; a null stage ends the walk. Clears found and starts over when the stage is another
     * one, has been edited, or has lost the next prim.
     **/
    void step(const SceneStage* stage,
              int budgetMs,
              int cap,
              const std::function<bool(const QString& name)>& matches,
              QStringList* found);

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

struct SceneGraphPathRows;
typedef std::shared_ptr<SceneGraphPathRows> SceneGraphPathRowsPtr;

/**
 * @brief The prims of a composed stage as a tree: active, defined, non-abstract prims, instance
 * proxies included, in the stage's order. Children are read only when a view asks for them, in
 * batches, so a prim with many children or a deep stage costs what is shown. Every value data()
 * returns is read once into the item; data() never touches the stage.
 *
 * The model listens to the stage's change notices: refresh(), after the stage is edited, reads
 * again only the expanded items under the paths the edits resynced, and inserts and removes rows
 * for what changed, keeping the other rows and their persistent indexes. A different stage
 * resets the model.
 *
 * Prims under an unloaded payload have no rows until it loads, and its row reports that it can have
 * children. A load changes only the rows under its prim.
 **/
class SceneGraphModel
    : public QAbstractItemModel {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum ColumnEnum {
        eColumnName = 0,
        eColumnType,
        eColumnKind,
        // The row's draw state in the viewer the panel is linked to.
        eColumnDrawState,
        eColumnCount
    };

    enum RoleEnum {
        // The prim's path, a QString.
        ePrimPathRole = Qt::UserRole + 1,
        // Whether the prim has a spec in the viewed node's own layers or has a descendant that does, a bool.
        eChangedHereRole,
        // The row's payload, a PayloadStateEnum.
        ePayloadStateRole,
        // Whether the viewer can box the row only by loading it: its payload is unloaded and no
        // extentsHint on a model bounds it, a bool.
        eUnknownBoundsRole,
        // The row's draw state in the linked viewer, set on it or not, a SceneDrawStateEnum. None
        // without a viewer.
        eDrawStateRole,
        // Whether that state comes from an ancestor or the default rather than from the row, a bool.
        eDrawStateInheritedRole,
        // Where that state comes from, a SceneDrawOriginEnum.
        eDrawStateOriginRole,
        // The script name of the node whose draw rule set the row's state, for a state the graph
        // sets on the row itself; empty otherwise, and when the rule names no node.
        eDrawStateOriginNodeRole,
    };

    enum PayloadStateEnum {
        ePayloadStateNone = 0,
        ePayloadStateUnloaded,
        ePayloadStateLoading,
        ePayloadStateLoaded,
        ePayloadStateFailed,
    };

    // How many children one fetchMore() adds.
    static constexpr int kFetchBatchSize = 1000;

    // Reads the linked viewer's draw states; false, without calling read, while an edit holds them.
    typedef std::function<bool(const std::function<void(const SceneDrawStates&)>&)> DrawStatesReader;
    // Sets the linked viewer's draw state at a path to a SceneDrawStateEnum, or for -1 resets the path
    // and the rows below it to the graph.
    typedef std::function<void(const QString& path, int state)> DrawStateWriter;

    // "BBox", "Children", "Full", "Load All" or "Hidden".
    static QString drawStateLabel(int state);

    // The draw state column's tooltip for a SceneDrawOriginEnum, naming node for a graph-set row.
    static QString drawOriginTip(int origin, const QString& node);

    // The mime type of dragged prim paths: UTF-8, one path per line.
    static QString primPathsMimeType();

    // The mime data of a drag of paths, owned by the caller: primPathsMimeType() and the text, one
    // path per line. Null for no paths.
    static QMimeData* mimeDataOfPaths(const QStringList& paths);

    explicit SceneGraphModel(QObject* parent = nullptr);

    ~SceneGraphModel() override;

    // Shows source's stage, or nothing for null. Resets the model.
    void setSource(const SceneGraphStageSourcePtr& source);

    const SceneGraphStageSourcePtr& getSource() const;

    /**
     * @brief Brings the read items up to date with the stage. Does nothing when the stage's edit
     * count has not moved and no notice came in; reads the edited subtrees otherwise. No read on the
     * main thread waits for the stage: one that finds it held by an edit is owed, the row it was for
     * shows the loading row meanwhile, and the next refresh() or settleLoads() reads it.
     **/
    void refresh();

    /**
     * @brief The index of the prim at path, column 0, reading the children on the way when populate.
     * Invalid with *busy set when an edit held the stage; deferredReadsDone() says when to ask again.
     **/
    QModelIndex indexOfPath(const QString& path, bool populate, bool* busy = nullptr);

    // Whether a read that found the stage busy has yet to happen.
    bool hasOwedReads() const;

    // Whether the model is adding, removing or resetting rows, so that a view's selection is
    // changing because rows came or went rather than because the user picked rows.
    bool isChanging() const;

    /**
     * @brief Adds rows a SceneGraphPathRowsBuilder read, without touching the stage, so that
     * indexOfPath() finds their paths without reading them. A parent whose children differ from what
     * the builder saw is passed over, as are all of them when the model shows another stage.
     **/
    void addPathRows(const SceneGraphPathRowsPtr& rows);

    QString pathOf(const QModelIndex& index) const;

    // None for a prim without a payload, and for a path no row has been read for.
    PayloadStateEnum getPayloadState(const QString& path);

    /**
     * @brief Shows the rows at paths whose payloads are unloaded as loading, each with a disabled
     * row under it, until the stage's notice of their load or settleLoads() says otherwise. Asks
     * nothing of the stage.
     **/
    void markLoading(const QStringList& paths);

    /**
     * @brief Takes in what the stage has done with its load requests: refresh(), the prims whose
     * files did not open, and, once no request is left, the end of any row still shown as loading.
     **/
    void settleLoads();

    /**
     * @brief Marks the rows at paths, which the caller gives with their ancestors. Rows read later
     * are marked too, so the set stays small however large the stage is.
     **/
    void setChangedHere(const QSet<QString>& paths);

    bool isChangedHere(const QString& path) const;

    /**
     * @brief Where the draw state column reads and edits: the linked viewer, by path, whichever stage
     * the model shows. Never the stage. Without a reader the column is empty; without a writer it is
     * read only.
     **/
    void setDrawStateAccess(const DrawStatesReader& reader, const DrawStateWriter& writer);

    // The linked viewer's draw states changed: the column reads them again.
    void refreshDrawStates();

    int getChangedHereCount() const;

    // How many items have been read, the root's children and down.
    int getItemCount() const;

    // How many times the model has been reset since it was made.
    int getResetCount() const;

    // How many change notices the stages it has shown have sent since it started listening to them.
    U64 getStageNoticeCount() const;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    // The draw state column takes a SceneDrawStateEnum, or -1 to reset the row to the graph.
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override;

Q_SIGNALS:
    // Every read owed for rows has now happened: what asked for rows and found the stage busy can ask again.
    void deferredReadsDone();

private:
    // Takes in the stage's edits. False if an edit held the stage, leaving them to read later.
    bool readEdits();

    void settle();

    void retryOwedReads();

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

struct SceneChangedHereRow {
    QString path;
    QString name;
    QString type;
    QString kind;
    // Indexes into SceneChangedHereRows::rows; a top-level row has no parent, -1.
    int parent = -1;
    int rowInParent = 0;
    std::vector<int> children;

    bool operator==(const SceneChangedHereRow& other) const = default;
};

struct SceneChangedHereRows {
    std::vector<SceneChangedHereRow> rows;
    std::vector<int> topLevel;
    QHash<QString, int> rowOfPath;

    bool hasSameRows(const SceneChangedHereRows& other) const;
};

typedef std::shared_ptr<const SceneChangedHereRows> SceneChangedHereRowsPtr;

/**
 * @brief Reads, on any thread, the rows SceneGraphModel::indexOfPath() would read for paths, which
 * can be a whole sibling list, a batch at a time so that the stage's lock is let go in between.
 **/
class SceneGraphPathRowsBuilder {
public:
    explicit SceneGraphPathRowsBuilder(const QStringList& paths);

    ~SceneGraphPathRowsBuilder();

    bool isDone() const;

    // Called with stage's lock held; a null stage ends the read with no rows.
    void readBatch(const SceneStage* stage);

    SceneGraphPathRowsPtr take();

private:
    SceneGraphPathRowsPtr _rows;
    std::size_t _nextParent = 0;
};

/**
 * @brief Reads the rows of the Changed Here filter from the marked paths alone, one prim by path at
 * a time, so that the cost follows how many prims the node authored and not how many siblings they
 * have. Siblings come out sorted by name, the stage's order being known only by listing all of them.
 * A marked path the scene graph would not list (an over with no def, an inactive or abstract prim)
 * has no row, nor have its descendants.
 **/
class SceneChangedHereRowsBuilder {
public:
    // The paths come with their ancestors, as SceneGraphModel::setChangedHere() wants them.
    explicit SceneChangedHereRowsBuilder(const QSet<QString>& paths);

    ~SceneChangedHereRowsBuilder();

    bool isDone() const;

    // Reads up to kBatchSize paths. Called with stage's lock held; a null stage reads no prim.
    void readBatch(const SceneStage* stage);

    SceneChangedHereRowsPtr take();

    static constexpr int kBatchSize = 1000;

private:
    QStringList _paths;
    int _next = 0;
    std::shared_ptr<SceneChangedHereRows> _rows;
};

/**
 * @brief A tree of the rows a SceneChangedHereRowsBuilder read, with SceneGraphModel's columns and
 * roles. It holds what it shows, so data() never touches the stage and no row needs fetching.
 **/
class SceneChangedHereModel
    : public QAbstractItemModel {
public:
    explicit SceneChangedHereModel(QObject* parent = nullptr);

    ~SceneChangedHereModel() override;

    // Resets the model, which shows nothing for null.
    void setRows(const SceneChangedHereRowsPtr& rows);

    const SceneChangedHereRowsPtr& getRows() const;

    int getRowCount() const;

    bool contains(const QString& path) const;

    QModelIndex indexOfPath(const QString& path) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override;

private:
    SceneChangedHereRowsPtr _rows;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneGraphModel_h
