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

#ifndef APPINSTANCE_H
#define APPINSTANCE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>
#include <list>
#include <string>
#include <utility>
#include <limits>
#include <cstddef>              // for NULL

#include <QtGlobal>      // for Q_UNUSED
#include <QObject>  // for Q_OBJECT, Q_SIGNALS, Q_SLOTS
#include <QMutex>

#include "Global/GlobalDefines.h"
#include "Engine/RectD.h"
#include "Engine/TimeLineKeyFrames.h"
#include "Engine/EngineFwd.h"

#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER

struct AppInstancePrivate;

/**
 * @brief The Natron build that saved the project being loaded, -1 and empty strings for what
 * the file does not say.
 **/
class ProjectBeingLoadedInfo {
public:
    int vMajor, vMinor, vRev;
    std::string gitBranch, gitCommit;
    std::string osStr;
    int bits;

    ProjectBeingLoadedInfo()
        : vMajor(-1)
        , vMinor(-1)
        , vRev(-1)
        , gitBranch()
        , gitCommit()
        , osStr()
        , bits(-1)
    {
    }
};

class FlagSetter
{
    bool* p;
    QMutex* lock;

public:

    FlagSetter(bool initialValue,
               bool* p);

    FlagSetter(bool initialValue,
               bool* p,
               QMutex* mutex);

    ~FlagSetter();
};

class FlagIncrementer
{
    int* p;
    QMutex* lock;

public:

    FlagIncrementer(int* p);

    FlagIncrementer(int* p,
                    QMutex* mutex);

    ~FlagIncrementer();
};

enum CreateNodeStatusEnum {
    eCreateNodeStatusCreated,
    eCreateNodeStatusPluginNotFound,
    eCreateNodeStatusFailed
};

// How AppInstance::startWritersRendering runs its renders. A background (no GUI) app has no event
// loop to return to, so it runs every mode as eRenderModeBlocking.
enum RenderModeEnum {
    // In this process, returning at once; the caller keeps the user from editing while it runs.
    eRenderModeForeground,
    // In a separate process, rendering the project as saved when the render is started.
    eRenderModeBackground,
    // In this process, returning once every render has finished.
    eRenderModeBlocking
};

class UndoRecorderI;

class AppInstance
    : public QObject
    , public std::enable_shared_from_this<AppInstance>
    , public TimeLineKeyFrames
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    // TODO: enable_shared_from_this
    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>

    AppInstance(int appID);

public:

    virtual ~AppInstance();

    virtual void aboutToQuit();
    virtual bool isBackground() const { return true; }


    struct RenderWork
    {
        OutputEffectInstance* writer;
        int firstFrame;
        int lastFrame;
        int frameStep;
        bool useRenderStats;
        bool isRestart;

        RenderWork()
            : writer(0)
            , firstFrame(0)
            , lastFrame(0)
            , frameStep(0)
            , useRenderStats(false)
            , isRestart(false)
        {
        }

        RenderWork(OutputEffectInstance* writer,
                   int firstFrame,
                   int lastFrame,
                   int frameStep,
                   bool useRenderStats)
            : writer(writer)
            , firstFrame(firstFrame)
            , lastFrame(lastFrame)
            , frameStep(frameStep)
            , useRenderStats(useRenderStats)
            , isRestart(false)
        {
        }
    };

    void load(const CLArgs& cl, bool makeEmptyInstance);

protected:

    AppInstance(const AppInstance&) = delete;

    AppInstance& operator=(const AppInstance&) = delete;

    virtual void loadInternal(const CLArgs& cl, bool makeEmptyInstance);

    void executeCommandLinePythonCommands(const CLArgs& args);

public:

    int getAppID() const;

    void exportDocs(const QString path);

    /** @brief Create a new node  in the node graph.
    **/
    NodePtr createNode(CreateNodeArgs & args);

    /**
     * @brief Creates a node from the serialization in args. When its plug-in is not installed or
     * cannot be instantiated, creates a MissingPluginNode holding that serialization instead, so
     * that saving writes the node back unchanged. The serialization's children are never created
     * for a placeholder. args is modified to name the plug-in that was used.
     **/
    NodePtr createNodeOrMissingPluginPlaceholder(CreateNodeArgs& args, bool* isPlaceholder);

    NodePtr createMissingPluginPlaceholder(CreateNodeArgs& args);

    /**
     * @brief Until cleared, a node created in the project with no collection goes into the given one, and
     * the first node created there with no position lands at (x, y). Nodes created out of the project, without
     * a GUI or from a serialization are left alone. A null collection clears the context.
     **/
    void setNodeCreationContext(const NodeCollectionPtr& collection, double x, double y);

    void clearNodeCreationContext();

    struct NodeCreationContext {
        NodeCollectionWPtr collection;
        double x = 0;
        double y = 0;
        bool positionPending = false;
    };

    /**
     * @brief The whole context, including whether its position has been used, so that a caller
     * that sets a context of its own can put the previous one back with restoreNodeCreationContext().
     **/
    NodeCreationContext getNodeCreationContext() const;

    void restoreNodeCreationContext(const NodeCreationContext& context);

    /**
     * @brief The collection set by setNodeCreationContext(), or null when no context is set.
     **/
    NodeCollectionPtr getNodeCreationContextCollection() const;

    NodePtr createReader(const std::string& filename,
                         CreateNodeArgs& args);


    NodePtr createWriter(const std::string& filename,
                         CreateNodeArgs& args,
                         int firstFrame = std::numeric_limits<int>::min(), int lastFrame = std::numeric_limits<int>::max());

    NodePtr getNodeByFullySpecifiedName(const std::string & name) const;

    ProjectPtr getProject() const;
    TimeLinePtr getTimeLine() const;

    /*true if the user is NOT scrubbing the timeline*/
    virtual bool shouldRefreshPreview() const
    {
        return false;
    }

    virtual void connectViewersToViewerCache()
    {
    }

    virtual void disconnectViewersFromViewerCache()
    {
    }

    virtual void errorDialog(const std::string & title, const std::string & message, bool useHtml) const;
    virtual void errorDialog(const std::string & title, const std::string & message, bool* stopAsking, bool useHtml) const;
    virtual void warningDialog(const std::string & title, const std::string & message, bool useHtml) const;
    virtual void warningDialog(const std::string & title, const std::string & message, bool* stopAsking, bool useHtml) const;
    virtual void informationDialog(const std::string & title, const std::string & message, bool useHtml) const;
    virtual void informationDialog(const std::string & title, const std::string & message, bool* stopAsking, bool useHtml) const;
    virtual Natron::StandardButtonEnum questionDialog(const std::string & title,
                                              const std::string & message,
                                              bool useHtml,
                                              Natron::StandardButtons buttons =
                                                  Natron::StandardButtons(Natron::StandardButtonEnum::eStandardButtonYes | Natron::StandardButtonEnum::eStandardButtonNo),
                                              Natron::StandardButtonEnum defaultButton = Natron::StandardButtonEnum::eStandardButtonNoButton) const WARN_UNUSED_RETURN;

    /**
     * @brief Asks a question to the user and returns the reply.
     * @param stopAsking Set to true if the user do not want Natron to ask the question again.
     **/
    virtual Natron::StandardButtonEnum questionDialog(const std::string & /*title*/,
                                              const std::string & /*message*/,
                                              bool /*useHtml*/,
                                              Natron::StandardButtons /*buttons*/,
                                              Natron::StandardButtonEnum /*defaultButton*/,
                                              bool* /*stopAsking*/)
    {
        return eStandardButtonYes;
    }

    /**
     * @brief Called once a loaded project has created and restored all of its nodes.
     **/
    virtual void onProjectNodesRestored()
    {
    }

    virtual void setupViewersForViews(const std::vector<std::string>& /*viewNames*/)
    {
    }

    virtual void notifyRenderStarted(const QString & /*sequenceName*/,
                                     int /*firstFrame*/,
                                     int /*lastFrame*/,
                                     int /*frameStep*/,
                                     bool /*canPause*/,
                                     OutputEffectInstance* /*writer*/,
                                     const ProcessHandlerPtr & /*process*/)
    {
    }

    virtual void notifyRenderRestarted( OutputEffectInstance* /*writer*/,
                                        const ProcessHandlerPtr & /*process*/)
    {
    }

    virtual bool isShowingDialog() const
    {
        return false;
    }

    virtual bool isGuiFrozen() const { return false; }

    virtual void progressStart(const NodePtr& node,
                               const std::string &message,
                               const std::string &messageid,
                               bool canCancel = true)
    {
        Q_UNUSED(node);
        Q_UNUSED(message);
        Q_UNUSED(messageid);
        Q_UNUSED(canCancel);
    }

    virtual void progressEnd(const NodePtr& /*node*/)
    {
    }

    virtual bool progressUpdate(const NodePtr& /*node*/,
                                double /*t*/)
    {
        return true;
    }

    /**
     * @brief Checks for a new version of Natron
     **/
    void checkForNewVersion() const;
    virtual void onMaxPanelsOpenedChanged(int /*maxPanels*/)
    {
    }

    virtual void onRenderQueuingChanged(bool /*queueingEnabled*/)
    {
    }

    double getProjectFrameRate() const;

    virtual std::string openImageFileDialog() { return std::string(); }

    virtual std::string saveImageFileDialog() { return std::string(); }

    /**
     * @brief Given writer names, start rendering the given RenderRequest. If empty all Writers in the project
     * will be rendered using the frame ranges.
     **/
    void startWritersRenderingFromNames(bool enableRenderStats,
                                        RenderModeEnum mode,
                                        const std::list<std::string>& writers,
                                        const std::list<std::pair<int, std::pair<int, int>>>& frameRanges);
    void startWritersRendering(RenderModeEnum mode, const std::list<RenderWork>& writers);

    /**
     * @brief Renders writers in this process while nothing can edit what they read. Without an
     * interface the render blocks until every frame is written; an app with one returns at once
     * and blocks the interface behind a modal progress dialog until the renders end.
     **/
    virtual void renderForeground(const std::list<RenderWork>& writers);

    /**
     * @brief Renders work again in a separate process from savePath, the file saved when the
     * render was first submitted, so the restart sees the project as it was then.
     * Returns false, starting nothing, if that file is gone.
     **/
    bool restartBackgroundRender(const RenderWork& work, const QString& savePath);

    /**
     * @brief Deletes savePath, a file saved for a render process, once no task can restart from
     * it. Files still held are deleted when the app quits.
     **/
    void releaseRenderSaveFile(const QString& savePath);

    /**
     * @brief Whether a render of writer started by this app is running or queued.
     **/
    bool isRenderActiveOrQueued(const OutputEffectInstance* writer) const;

    /**
     * @brief The process of writer's running or queued separate-process render, if any.
     **/
    ProcessHandlerPtr getRenderProcess(const OutputEffectInstance* writer) const;

    /**
     * @brief Turns the -w/--writer arguments of cl into RenderWork requests, resolving each
     * named Write node and applying its optional filename override.
     **/
    void getWritersWorkForCL(const CLArgs& cl, std::list<AppInstance::RenderWork>& requests);

public:

    void addInvalidExpressionKnob(const KnobIPtr& knob);
    void removeInvalidExpressionKnob(const KnobI* knob);
    void recheckInvalidExpressions();

    virtual void clearViewersLastRenderedTexture() {}

    virtual void toggleAutoHideGraphInputs() {}

    bool isCreatingPythonGroup() const;

    bool isCreatingNodeTree() const;

    void setIsCreatingNodeTree(bool b);

    /**
     * @brief The recorder that adds edits made from Python to the project window's undo history.
     * The window installs it; applications without one have none and record nothing.
     **/
    void setUndoRecorder(const std::shared_ptr<UndoRecorderI>& recorder);

    std::shared_ptr<UndoRecorderI> getUndoRecorder() const;

    /**
     * @brief The recorder if an edit made now should be recorded, null otherwise: off the main
     * thread, while the project loads, while a node tree or PyPlug is built, while recording is
     * suppressed and outside any open step or pushed command.
     **/
    std::shared_ptr<UndoRecorderI> getRecordingUndoRecorder() const;

    /**
     * @brief Set while recorded edits are undone or redone. Python callbacks do not run then,
     * since the records already hold what the callbacks did.
     **/
    void setReplayingUndo(bool replaying);

    bool isReplayingUndo() const;

    virtual void appendToScriptEditor(const std::string& str);
    virtual void printAutoDeclaredVariable(const std::string& str);

    void getFrameRange(double* first, double* last) const;

    virtual void setLastViewerUsingTimeline(const NodePtr& /*node*/) {}

    virtual ViewerInstance* getLastViewerUsingTimeline() const { return 0; }

    bool loadPythonScript(const QFileInfo& file);
    virtual void queueRedrawForAllViewers() {}

    virtual void renderAllViewers(bool /* canAbort*/) {}

    virtual void abortAllViewers() {}

    virtual void refreshAllPreviews() {}

    virtual void declareCurrentAppVariable_Python();

    /**
     * @brief Makes app.<name> match the project's knobs on the existing app object, keeping
     * every other attribute, such as the nodes': declares the knobs not declared yet and deletes
     * the attributes of knobs that no longer exist. Returns the script it ran; throws
     * std::runtime_error when a knob name is a Python keyword or the script fails.
     **/
    std::string declareProjectParamsToPython();

    /**
     * @brief declareProjectParamsToPython(), printing the script it ran and logging failures.
     **/
    void refreshProjectParamsInPython();

    void execOnProjectCreatedCallback();

    virtual void createLoadProjectSplashScreen(const QString& /*projectFile*/) {}

    virtual void updateProjectLoadStatus(const QString& /*str*/) {}

    virtual void closeLoadProjectSplashScreen() {}

    std::string getAppIDString() const;
    const std::list<NodePtr>& getNodesBeingCreated() const;
    virtual bool isDraftRenderEnabled() const { return false; }

    virtual void setDraftRenderEnabled(bool /*b*/) {}

    virtual void setUserIsPainting(const NodePtr& /*rotopaintNode*/,
                                   const RotoStrokeItemPtr& /*stroke*/,
                                   bool /*isPainting*/) {}

    virtual void getActiveRotoDrawingStroke(NodePtr* /*node*/,
                                            RotoStrokeItemPtr* /*stroke*/,
                                            bool* /*isPainting*/) const { }

    virtual bool isRenderStatsActionChecked() const { return false; }

    bool saveTemp(const std::string& filename);
    virtual bool save(const std::string& filename);
    virtual bool saveAs(const std::string& filename);
    virtual AppInstancePtr loadProject(const std::string& filename);

    ///Close the current project but keep the window
    virtual bool resetProject();

    ///Reset + close window, quit if last window
    virtual bool closeProject();

    ///Opens a new window
    virtual AppInstancePtr newProject();
    virtual void* getOfxHostOSHandle() const { return NULL; }

    virtual void updateLastPaintStrokeData(int /*newAge*/,
                                           const std::list<std::pair<Point, double> >& /*points*/,
                                           const RectD& /*lastPointsBbox*/,
                                           int /*strokeIndex*/) {}

    virtual void getLastPaintStrokePoints(std::list<std::list<std::pair<Point, double> > >* /*strokes*/,
                                          int* /*strokeIndex*/) const {}

    virtual int getStrokeLastIndex() const { return -1; }

    virtual void getStrokeAndMultiStrokeIndex(RotoStrokeItemPtr* /*stroke*/,
                                              int* /*strokeIndex*/) const {}

    virtual void getRenderStrokeData(RectD* /*lastStrokeMovementBbox*/,
                                     std::list<std::pair<Point, double> >* /*lastStrokeMovementPoints*/,
                                     double */*distNextIn*/,
                                     ImagePtr* /*strokeImage*/) const {}

    virtual void updateStrokeImage(const ImagePtr& /*image*/,
                                   double /*distNextOut*/,
                                   bool /*setDistNextOut*/) {}

    virtual RectD getLastPaintStrokeBbox() const { return RectD(); }

    virtual RectD getPaintStrokeWholeBbox() const { return RectD(); }

    void removeRenderFromQueue(OutputEffectInstance* writer);
    virtual void reloadScriptEditorFonts() {}

    const ProjectBeingLoadedInfo& getProjectBeingLoadedInfo() const;
    void setProjectBeingLoadedInfo(const ProjectBeingLoadedInfo& info);

public Q_SLOTS:

    void quit();

    void quitNow();

    virtual void redrawAllViewers() {}

    void triggerAutoSave();

    void clearOpenFXPluginsCaches();

    void clearAllLastRenderedImages();

    void newVersionCheckDownloaded();

    void newVersionCheckError();

    void onBackgroundRenderProcessFinished();

    void onQueuedRenderFinished(int retCode);

    void onRenderFinishedForExitStatus(int retCode);

Q_SIGNALS:

    void pluginsPopulated();

protected:
    virtual void onGroupCreationFinished(const NodePtr& node, const Serialization::NodeSerializationPtr& serialization, bool autoConnect);
    virtual void createNodeGui(const NodePtr& /*node*/,
                               const NodePtr& /*parentmultiinstance*/,
                               const CreateNodeArgs& /*args*/)
    {
    }

private:
    void startNextQueuedRender(OutputEffectInstance* finishedWriter, const ProcessHandler* finishedProcess);

    NodePtr createNodeInternal(CreateNodeArgs& args, CreateNodeStatusEnum* status = nullptr);

    void setGroupLabelIDAndVersion(const NodePtr& node,
                                   const QString& pythonModulePath,
                                   const QString &pythonModule);

    NodePtr createNodeFromPythonModule(Plugin* plugin,
                                       const CreateNodeArgs& args);

    std::unique_ptr<AppInstancePrivate> _imp;
};

class CreatingNodeTreeFlag_RAII
{
    AppInstanceWPtr _app;

public:

    CreatingNodeTreeFlag_RAII(const AppInstancePtr& app)
        : _app(app)
    {
        app->setIsCreatingNodeTree(true);
    }

    ~CreatingNodeTreeFlag_RAII()
    {
        AppInstancePtr a = _app.lock();

        if (a) {
            a->setIsCreatingNodeTree(false);
        }
    }
};

NATRON_NAMESPACE_EXIT

#endif // APPINSTANCE_H
