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

#ifndef Gui_ScenePrimInspector_h
#define Gui_ScenePrimInspector_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <functional>
#include <memory>

#include <QWidget>
#include <QtCore/QString>

#include "Global/GlobalDefines.h"
#include "Gui/SceneGraphModel.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One read of one prim for one tab, on the inspector's worker. readStage() takes the stage
 * lock for the length of the call; the viewer and the tree wait on that lock on the main thread,
 * so a tab should take it in short calls and do its slow work between them.
 **/
class ScenePrimInspectorRead {
public:
    ScenePrimInspectorRead(const SceneGraphStageSourcePtr& source,
                           const QString& path,
                           double frame,
                           double framesPerSecond,
                           const std::atomic<U64>& latestGeneration,
                           U64 generation);

    const QString& getPath() const;

    double getFrame() const;

    double getFramesPerSecond() const;

    // Whether another read has replaced this one, so its result would not be shown.
    bool isCancelled() const;

    // Calls read with the stage, or null when there is none. Returns false, without calling
    // read, when the read is cancelled, also after waiting for the lock.
    bool readStage(const std::function<void(const SceneStage* stage)>& read) const;

private:
    const SceneGraphStageSourcePtr& _source;
    const QString& _path;
    double _frame;
    double _framesPerSecond;
    const std::atomic<U64>& _latestGeneration;
    U64 _generation;
};

/**
 * @brief A page of the inspector. Only the tab showing is read.
 **/
class ScenePrimInspectorTab {
public:
    virtual ~ScenePrimInspectorTab() { }

    virtual QString getTitle() const = 0;

    // The inspector's tab widget becomes its parent.
    virtual QWidget* getWidget() const = 0;

    /**
     * @brief Called on the worker. Returns what shows the result, which the main thread runs
     * unless a newer read started, or an empty function when the read was cancelled.
     **/
    virtual std::function<void()> read(const ScenePrimInspectorRead& read) = 0;

    // Shows the last result greyed, as no longer current. Main thread.
    virtual void setStale(bool stale) = 0;

    // Main thread.
    virtual void clear() = 0;
};

/**
 * @brief The selected prim's tabs: Attributes, Relationships, Metadata and Composition at the given frame, each
 * row naming the node or file that authored it. Reads run on a worker; a new prim cancels the read
 * under way, and while a read has not finished, for instance with the stage lock busy with a load,
 * the last result stays up, greyed. Nothing is read while the inspector is hidden.
 *
 * Scripts find its widgets by object name: sceneGraphInspector, sceneGraphInspectorTabs,
 * sceneGraphInspectorPrim, sceneGraphAttributes, sceneGraphRelationships, sceneGraphMetadata and sceneGraphComposition.
 * It mirrors its state into the natronSceneGraphInspectorPath (the prim of the rows shown),
 * natronSceneGraphInspectorBusy, natronSceneGraphInspectorStale,
 * natronSceneGraphInspectorReadsStarted, natronSceneGraphInspectorReadsFinished and
 * natronSceneGraphInspectorReadsCancelled properties.
 **/
class ScenePrimInspector
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit ScenePrimInspector(QWidget* parent);

    ~ScenePrimInspector() override;

    void addTab(std::unique_ptr<ScenePrimInspectorTab> tab);

    // An empty path or a null source clears the tabs.
    void setPrim(const SceneGraphStageSourcePtr& source,
                 const QString& path,
                 double frame,
                 double framesPerSecond);

    // Waits for the read under way. Nothing is read after this.
    void stop();

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ScenePrimInspector_h
