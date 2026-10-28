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

#include <QtCore/QAbstractItemModel>
#include <QtCore/QStringList>

#include "Global/GlobalDefines.h"

// Free of USD, like SceneViewport.h: the panel's header includes it.

NATRON_NAMESPACE_ENTER

class SceneStage;

/**
 * @brief A composed stage the scene graph shows, and the lock it is read under.
 **/
class SceneGraphStageSource {
public:
    virtual ~SceneGraphStageSource() { }

    /**
     * @brief Calls read with the stage, or null when there is none, holding whatever keeps edits
     * of the stage out until read returns. Called on the main thread.
     **/
    virtual void readStage(const std::function<void(const SceneStage* stage)>& read) const = 0;

    // Moves whenever the stage is edited; cheap, without the lock.
    virtual U64 getEditCount() const = 0;
};

typedef std::shared_ptr<SceneGraphStageSource> SceneGraphStageSourcePtr;

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
        eColumnCount
    };

    enum RoleEnum {
        // The prim's path, a QString.
        ePrimPathRole = Qt::UserRole + 1,
    };

    // How many children one fetchMore() adds.
    static constexpr int kFetchBatchSize = 1000;

    // The mime type of dragged prim paths: UTF-8, one path per line.
    static QString primPathsMimeType();

    explicit SceneGraphModel(QObject* parent = nullptr);

    ~SceneGraphModel() override;

    // Shows source's stage, or nothing for null. Resets the model.
    void setSource(const SceneGraphStageSourcePtr& source);

    const SceneGraphStageSourcePtr& getSource() const;

    /**
     * @brief Brings the read items up to date with the stage. Does nothing when the stage's edit
     * count has not moved and no notice came in; reads the edited subtrees otherwise.
     **/
    void refresh();

    // The index of the prim at path, column 0, reading the children on the way when populate.
    QModelIndex indexOfPath(const QString& path, bool populate);

    QString pathOf(const QModelIndex& index) const;

    // How many items have been read, the root's children and down.
    int getItemCount() const;

    // How many times the model has been reset since it was made.
    int getResetCount() const;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneGraphModel_h
