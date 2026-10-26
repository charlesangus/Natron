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

#ifndef NATRON_ENGINE_PROJECT_H
#define NATRON_ENGINE_PROJECT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <map>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QDateTime>
#include <QObject>
#include <QStringList>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"

#include "Engine/AfterQuitProcessingI.h"
#include "Engine/EngineFwd.h"
#include "Engine/Format.h"
#include "Engine/Knob.h"
#include "Engine/LayerRegistry.h"
#include "Engine/NodeGroup.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include "Serialization/SerializationFwd.h"

// The project settings page and RFV group holding the project's Reverse-Flow Variables, see RFV.h.
#define kProjectParamRFVPage "rfvPage"
#define kProjectParamRFVPageLabel "Variables"
#define kProjectParamRFVVariables "rfvVariables"
#define kProjectParamRFVVariablesLabel "Variables"
#define kProjectParamRFVEdit "editVariables"
#define kProjectParamRFVEditLabel "Edit variables..."

NATRON_NAMESPACE_ENTER

struct ProjectPrivate;

class Project
    : public KnobHolder
    , public NodeCollection
    , public AfterQuitProcessingI
    , public std::enable_shared_from_this<Project>
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

friend struct ProjectPrivate;

struct MakeSharedEnabler;

// constructors should be privatized in any class that derives from std::enable_shared_from_this<>
Project(const AppInstancePtr& appInstance);

public:
    static ProjectPtr create(const AppInstancePtr& appInstance);

    virtual ~Project();

    //these are per project thread-local data
    struct ProjectTLSData
    {
        std::vector<std::string> viewNames;
    };

    typedef std::shared_ptr<ProjectTLSData> ProjectDataTLSPtr;

    /**
     * @brief What a path opens as a project: the file or folder the reader opens (source), and
     * the directory (path, with a trailing separator) and file name the project is known by.
     * A folder is known by the bundle it is saved as, so `/a/test/` is `/a/` + `test.ntpz`.
     **/
    struct OpenTarget {
        QString source;
        QString path;
        QString name;
        bool isFolder = false;
    };

    /**
     * @brief Says whether path can be opened as a project without touching any project. path is
     * a bundle (any zip archive, whatever its extension), a folder holding project.yaml, or a
     * project.yaml file, which stands for its folder. Otherwise returns false with a message
     * for the user in error: the legacy-format message for an old XML project or any other
     * `.ntp` file, "<file name> is not a Natron project" for anything else. Whether a bundle
     * or folder really holds a readable project is only known once it is read.
     **/
    static bool classifyOpenTarget(const QString& path, OpenTarget* out, QString* error);

    /**
     * @brief The name of the file that makes a folder a project (an unzipped bundle): project.yaml.
     **/
    static QString folderProjectFileName();

    /**
     * @brief Loads the project at path + name: a file, a folder or a project.yaml, as
     * classifyOpenTarget() describes. A path it refuses is reported and the current project is
     * left untouched; otherwise the current project is reset before loading.
     **/
    bool loadProject(const QString & path, const QString & name, bool isUntitledAutosave = false, bool attemptToLoadAutosave = true);

    /**
     * @brief True when the project was opened from a folder and not saved since: the bundle
     * named by getProjectPath() + getProjectFilename() may not exist, or may hold another version.
     **/
    bool isLoadedFromFolder() const;

    bool hasMissingPluginNode() const;

    /**
     * @brief Records a problem that loading worked around, such as an input whose node is
     * missing, and writes it to the error log. The list is emptied when the next load starts.
     **/
    void addLoadWarning(const QString& warning);

    QStringList getLoadWarnings() const;

    /**
     * @brief Writes a project file at filePath without ever leaving a partial one there:
     * writeArchive writes the whole archive to a new file in the same directory, beforeReplace
     * runs once it is complete and synced, then that file replaces filePath. When writeArchive
     * or the file system fails, filePath is left as it was and std::runtime_error is thrown
     * (or whatever writeArchive threw).
     **/
    static void writeProjectFileAtomically(const QString& filePath,
                                           const std::function<void(Serialization::ZipOutput&)>& writeArchive,
                                           const std::function<void()>& beforeReplace = std::function<void()>());

    /**
     * @brief Makes room for a save over filePath that keeps saveVersions versions: drops the
     * oldest numbered backups, shifts the rest up by one and links (or copies) the current
     * filePath to filePath.~1~. filePath itself is never removed or renamed, so it survives
     * when the save that follows fails.
     **/
    static void rotateBackups(const QString& filePath, int saveVersions);

    /**
     * @brief Saves the project with the given path and name corresponding to a file on disk.
     * @param autoSave If true then it will save the project in a temporary file instead (see autoSave()).
     * @returns The actual filepath of the file saved
     **/
    bool saveProject(const QString & path, const QString & name, QString* newFilePath);

    /**
     * @brief Saves the project. A periodic auto-save that finds another save running is skipped.
     * Any other save (by the user, a script, before closing or for a render process) that finds
     * a save running in another thread waits for it and then runs, so it is never dropped; on the
     * main thread the wait keeps processing events, except user input, so the running save may
     * still use the main thread or the Python lock. The caller must not hold the Python lock.
     * Returns false when the save failed or was skipped.
     **/
    bool saveProject_imp(const QString & path, const QString & name, bool autoSave, bool updateProjectProperties, QString* newFilePath = 0);

    /**
     * @brief True while a saveProject_imp() call on the main thread is waiting for another save
     * or saving. Its wait processes events, so whatever would close or reset the project from an
     * event must refuse while this is true; mainThreadSavesEnded() tells when it may retry.
     * Main thread only.
     **/
    bool isSavingOnMainThread() const WARN_UNUSED_RETURN;

    /**
     * @brief True when this thread is running a save, and so is inside an event loop that save
     * spun if it is asking for another one.
     **/
    bool isSavingOnCurrentThread() const WARN_UNUSED_RETURN;

    /**
     * @brief Saves the project to a new file in autoSavesDir() for a render process to load,
     * named after this process, this app instance and a per-process counter so that no other
     * render or instance writes the same file. The render process removes it once loaded, and
     * the app removes it when the render ends or is taken off the queue; removeStaleRenderSaves()
     * removes those whose Natron process is gone.
     **/
    bool saveForRenderProcess(QString* filePath);

    /**
     * @brief Returns a file name for saveForRenderProcess() that no earlier call in this process
     * returned.
     **/
    static QString makeRenderSaveFileName(int appID) WARN_UNUSED_RETURN;

    /**
     * @brief True when filePath names a file saveForRenderProcess() wrote.
     **/
    static bool isRenderSaveFile(const QString& filePath) WARN_UNUSED_RETURN;

    /**
     * @brief Removes from autoSavesDir() the render saves, and anything left beside them, that
     * neither this process nor another running process may still hand to a render process:
     * those older than renderSaveGraceSeconds() whose owning process is not running.
     **/
    static void removeStaleRenderSaves();

    /**
     * @brief How long a render save is kept whatever its owner looks like from here: a process
     * in another pid namespace sharing the folder may look gone while it still runs.
     **/
    static qint64 renderSaveGraceSeconds() WARN_UNUSED_RETURN;

    /**
     * @brief Same as saveProject except that it will save the project in a temporary file
     * so it doesn't overwrite the project.
     **/
    void autoSave();


    /**
     * @brief Same as autoSave() but the auto-save is run in a separate thread instead.
     **/
    void triggerAutoSave();

    /**
     * @brief Returns the path to where the auto save files are stored on disk.
     **/
    static QString autoSavesDir() WARN_UNUSED_RETURN;

    /**
     * @brief Returns the length of the project file name the auto-save file fileName was made
     * for (name.ntpz in name.ntpz.autosave or name.ntpz.autosave.<digits>), or -1 when fileName
     * is not an auto-save: a render save, or the temporary file an auto-save is being written to.
     **/
    static int autoSaveProjectNameLength(const QString& fileName) WARN_UNUSED_RETURN;

    bool findAutoSaveForProject(const OpenTarget& target, QString* autoSaveFileName);

    /**
     * @brief Returns true if the project is currently loading.
     **/
    bool isLoadingProject() const;

    bool isLoadingProjectInternal() const;

    QString getProjectFilename() const WARN_UNUSED_RETURN;

    QString getLastAutoSaveFilePath() const;

    bool hasEverAutoSaved() const;

    QString getProjectPath() const WARN_UNUSED_RETURN;

    bool hasProjectBeenSavedByUser() const WARN_UNUSED_RETURN;

    bool isSaveUpToDate() const WARN_UNUSED_RETURN;

    //QDateTime getProjectAgeSinceLastSave() const WARN_UNUSED_RETURN ;

    //QDateTime getProjectAgeSinceLastAutosave() const WARN_UNUSED_RETURN;

    void getProjectDefaultFormat(Format *f) const;

    bool getProjectFormatAtIndex(int index, Format* f) const;

    void getProjectFormatEntries(std::vector<ChoiceOption>* formatStrings, int* currentValue) const;

    bool getFormatNameFromRect(const RectI& rect, double par, std::string* name) const;

    void getAdditionalFormats(std::list<Format> *formats) const;

    void setupProjectForStereo();

    void createProjectViews(const std::vector<std::string>& views);

    const std::vector<std::string>& getProjectViewNames() const;

    int getProjectViewsCount() const;

    bool isGPURenderingEnabledInProject() const;

    LayerRegistry::AddResultEnum addLayer(const ImageLayerDesc& desc, LayerRegistryEntry::OriginEnum origin, std::string* error);

    bool removeLayer(const std::string& id, std::string* error);

    std::shared_ptr<const std::vector<LayerRegistryEntry>> getLayerRegistrySnapshot() const;

    bool findLayer(const std::string& id, ImageLayerDesc* out) const;

    void getLayerUsers(const std::string& id, std::list<NodePtr>* users) const;

    // Re-emits projectLayersChanged() from contexts (e.g. ProjectPrivate) that cannot
    // Q_EMIT a signal directly because they are not a member of this QObject.
    void emitProjectLayersChangedSignal();

    // Rebuilds the read-only Layers page (Layer / Channels / Used by) from the registry.
    // Registry mutations call it themselves; nodes call it when a layer reference changes,
    // since that alters "Used by" without touching the registry.
    void refreshLayersKnob();

    void setOrAddProjectFormat(const Format & frmt, bool skipAdd = false);

    bool isAutoSetProjectFormatEnabled() const;

    void setAutoSetProjectFormatEnabled(bool enabled);

    bool isAutoPreviewEnabled() const;

    void toggleAutoPreview();

    TimeLinePtr getTimeLine() const WARN_UNUSED_RETURN;

    int currentFrame() const WARN_UNUSED_RETURN;


    /**
     * @brief Returns true if the project is considered as irrelevant and shouldn't be autosaved anyway.
     * Such a graph is for example a simple Viewer.
     **/
    bool isGraphWorthLess() const;

    bool tryLock() const;

    void unlock() const;

    qint64 getProjectCreationTime() const;

    /**
     * @brief Hash of the project settings that change what nodes render: output format,
     * frame rate, views, OpenColorIO config and working space, layer registry and
     * render-affecting preferences. Never 0. Memoized until one of those changes; the OCIO
     * config file's stamp is looked up on every call, since Reload changes it with no edit.
     **/
    U64 getContentDigest() const;

    /**
     * @brief The OpenColorIO config the project actually uses: an "ocio://" URI or an absolute
     * path. After a failed load this is the config still in use, not the requested one.
     **/
    std::string getOCIOConfigSource() const;

    /**
     * @brief The config the OCIO environment variable or the project's config knobs ask for.
     * Nothing else may read the project's config knobs.
     **/
    std::string getRequestedOCIOConfigSource() const;

    /**
     * @brief True, filling \p error, while the config the OCIO environment variable forces cannot
     * be loaded. Renders must fail then rather than go through another config.
     **/
    bool getOCIOConfigError(std::string* error) const;

    std::string getWorkingColorSpace() const;

    std::string getFileColorSpace(FileColorCategoryEnum category) const;

    void getDefaultDisplayView(std::string* display, std::string* view) const;

    ProjectColorManagementPtr getColorManagement() const;

    /**
     * @brief One option per colourspace of the config of \p cm: its id the colourspace name, its
     * label the '/'-separated path of the colourspace's family followed by the name, for a
     * cascading choice, and its tooltip the colourspace description. Every label is unique and
     * none is also a submenu of another.
     **/
    static std::vector<ChoiceOption> colorSpaceOptions(const ProjectColorManagement& cm);

    /**
     * @brief Remove all the autosave files from the disk.
     **/
    static void clearAutoSavesDir();
    void removeLastAutosave();

    QString getLockAbsoluteFilePath() const;
    void createLockFile();
    void removeLockFile();
    bool getLockFileInfos(const QString& projectPath, const QString& projectName, QString* authorName, QString* lastSaveDate, QString* host, qint64* appPID) const;

    virtual bool isProject() const OVERRIDE
    {
        return true;
    }

    // Every node's hash folds in the project's render settings.
    virtual void onKnobContentChanged(const KnobI* knob) OVERRIDE FINAL;

    /**
     * @brief Returns the user environment variables for the project
     **/
    void getEnvironmentVariables(std::map<std::string, std::string>& env) const;

    /**
     * @brief Expands the environment variables in the given string that are found in env
     **/
    static void expandVariable(const std::map<std::string, std::string>& env, std::string& str);
    static bool expandVariable(const std::string& varName, const std::string& varValue, std::string& str);

    /**
     * @brief Try to find in str a variable from env. The longest match is replaced by the variable name.
     **/
    static void findReplaceVariable(const std::map<std::string, std::string>& env, std::string& str);

    /**
     * @brief Make the given string relative to the given variable.
     * If the path indicated by varValue doesn't exist then str will be unchanged.
     **/
    static void makeRelativeToVariable(const std::string& varName, const std::string& varValue, std::string& str);
    static bool isRelative(const std::string& str);

    /**
     * @brief Returns true if str starts with a URI scheme, e.g. "ocio://".
     **/
    static bool hasUriScheme(const std::string& str);

    /**
     * @brief Puts every node asking for an OpenColorIO colorspace the project's effective
     * config does not define into an error state naming those parameters, and clears that
     * error, and only that one, from nodes whose colorspaces all resolve.
     **/
    void reportUnresolvedOCIOColorSpaces();

    /**
     * @brief Sets the "ocioConfigFile" parameter of every node that has one to
     * getOCIOConfigSource() and hides it, so a node's own or saved value never wins, then sets
     * the hidden "ocioWorkingSpace" parameter to getWorkingColorSpace().
     **/
    void pushOCIOConfigToNodes();

    /**
     * @brief As pushOCIOConfigToNodes() for one node; a Read or Write also pushes to the
     * decoder or encoder it embeds.
     **/
    void pushOCIOConfigToNode(const NodePtr& node);

    /**
     * @brief Puts every node holding an input whose data kind it cannot handle into an error
     * state naming those inputs. Data kinds are never serialized, so they are recomputed from
     * the restored graph; an edge found invalid is kept exactly as the user made it, since
     * silently rewiring a loaded project would be a worse answer than showing the problem.
     * Runs Node::refreshDataKindConflictMessage() over the whole graph, so a node whose inputs
     * are all acceptable ends up with no such error rather than keeping an older one.
     **/
    void reportDataKindConflicts();

    /**
     * @brief If str is relative it will canonicalize the path, i.e expand all variables and '.' and '..' that may
     * be. When returning from this function str will be an absolute path.
     * It internally uses expandVariable
     **/
    void canonicalizePath(std::string& str);

    /**
     * @brief Tries to find any variable that str could start with and simplify the given path with the longest
     * variable matching. It internally uses findReplaceVariable
     **/
    void simplifyPath(std::string& str);

    /**
     * @brief Same as simplifyPath but will only try with the [Project] variable
     **/
    void makeRelativeToProject(std::string& str);

    bool fixFilePath(const std::string& projectPathName, const std::string& newProjectPath,
                     std::string& filePath);


    void onOCIOConfigPathChanged(const std::string& path, bool blockevaluation);


    static std::string escapeXML(const std::string &input);
    static std::string unescapeXML(const std::string &input);

    double getProjectFrameRate() const;

    KnobPathPtr getEnvVarKnob() const;

    /**
     * @brief The RFV group whose user knobs are the project's Reverse-Flow Variables. They are
     * saved with their definitions, recreated on load and deleted when the project is reset.
     **/
    KnobGroupPtr getRFVGroup() const;

    /**
     * @brief Has the variables, the only project knobs that can animate or hold an expression, tell
     * their interfaces that their value at the new time may differ (see KnobI::onTimeChanged()).
     **/
    void refreshVariablesAfterTimeChange(bool isPlayback, double time);

    /**
     * @brief The project's variables may have changed: every ReverseFlowVariableSet node re-sorts
     * its variables by the names the project declares (see RFV::refreshNodeVariableGroups()).
     **/
    virtual void onUserKnobsChanged() OVERRIDE FINAL;
    std::string getOnProjectLoadCB() const;
    std::string getOnProjectSaveCB() const;
    std::string getOnProjectCloseCB() const;
    std::string getOnNodeCreatedCB() const;
    std::string getOnNodeDeleteCB() const;

    bool isProjectClosing() const;

    bool isFrameRangeLocked() const;

    void getFrameRange(double* first, double* last) const;

    void unionFrameRangeWith(int first, int last);

    void recomputeFrameRangeFromReaders();

    void createViewer();

    void resetProject();


    /**
     * @brief Calls quitAnyProcessing for all nodes in the group and in each subgroup
     * This is called only when calling AppManager::abortAnyProcessing()
     * @returns True if a node is in the project and a watcher was installed, false otherwise
     **/
    bool quitAnyProcessingForAllNodes(AfterQuitProcessingI* receiver, const GenericWatcherCallerArgsPtr& args);

    void refreshOpenGLRenderingFlagOnNodes();

    /**
     * @brief Fills serialization with everything a project file saves: the running build, the
     * nodes, the project knobs, the user formats and layers, the timeline and the creation date.
     **/
    void toSerialization(Serialization::ProjectSerialization* serialization) const;

    /**
     * @brief Restores the project from serialization, as loading a project file does once it
     * is read. name and path are the project file's.
     **/
    bool fromSerialization(const Serialization::ProjectSerialization& serialization,
                           const QString& name,
                           const QString& path,
                           bool* mustSave);

private:

    Project(const Project&) = delete;

    Project& operator=(const Project&) = delete;

    virtual void afterQuitProcessingCallback(const GenericWatcherCallerArgsPtr& args) OVERRIDE FINAL;

public:

    struct TreeOutput
    {
        NodePtr node;
        std::list<std::pair<int, NodeWPtr> > outputs;
    };

    struct TreeInput
    {
        NodePtr node;
        std::vector<NodePtr> inputs;
    };

    struct NodesTree
    {
        TreeOutput output;
        std::list<TreeInput> inputs;
        NodesList inbetweenNodes;
    };

    static void extractTreesFromNodes(const NodesList& nodes, std::list<Project::NodesTree>& trees);

    void closeProject_blocking(bool aboutToQuit);

    bool addFormat(const std::string& formatSpec);

    void setTimeLine(const TimeLinePtr& timeline);


    /**
     * @brief Resets the project state clearing all nodes and the project name.
     **/
    void reset(bool aboutToQuit, bool blocking);

public Q_SLOTS:

    void onQuitAnyProcessingWatcherTaskFinished(int taskID, const GenericWatcherCallerArgsPtr& args);

    void onAutoSaveTimerTriggered();

    void onAutoSaveFutureFinished();

    void onProjectFormatPopulated();

Q_SIGNALS:

    void mustCreateFormat();

    // The user pressed the variables page's edit button.
    void mustEditVariables();

    void formatChanged(Format);

    void frameRangeChanged(int, int);

    void autoPreviewChanged(bool);

    void projectNameChanged(QString name, bool modified);

    // Emitted from the saving thread whenever saveProject_imp() ends, so that a save waiting on
    // the main thread can stop waiting.
    void saveFinished();

    // Emitted on the main thread once no saveProject_imp() call there is waiting or saving, so
    // that a close refused during the save can be asked for again.
    void mainThreadSavesEnded();

    void knobsInitialized();

    void projectViewsChanged();

    void projectLayersChanged();

private:
    U64 computeContentDigestWithoutFileStamps(const std::shared_ptr<const std::vector<LayerRegistryEntry>>& layers,
                                              const std::string& ocioSource,
                                              bool concatenation) const;

    // Emits projectLayersChanged() and refreshes every node's channel selector menus;
    // called after a registry mutation outside of project load (load batches its own
    // single emission after all nodes are restored, see ProjectPrivate::restoreFromSerialization).
    void notifyLayersChanged();

    // Re-keys the nodes whose hash includes the project frame range.
    void rehashNodesUsingFrameRange();

    std::string encodeLayersKnobTable() const;

    /*Returns the index of the format*/
    int tryAddProjectFormat(const Format & f, bool* existed);

    void setProjectDefaultFormat(const Format & f);

    bool loadProjectInternal(const QString& path, const QString& name, const QString& source,
                             bool isAutoSave, bool isUntitledAutosave, bool* mustSave);

    QString saveProjectInternal(const QString & path, const QString & name, bool autosave, bool updateProjectProperties);

    void reportLoadWarnings();

    void doResetEnd(bool aboutToQuit);

    /**
     * @brief Loads getRequestedOCIOConfigSource(), repopulates the Color page menus with fallbacks for
     * names the config lacks, re-derives the config knobs' enabled state and fires configChanged.
     * Enabled is serialized, so this must run after every restore.
     **/
    void refreshColorManagement(bool warnOnFallback);

    void applyColorManagementDefaults(bool asKnobDefaults);

    /**
     * @brief Makes the preference's config the default of the config knobs. Callers must
     * suppress the colour management refresh.
     **/
    void applyNewProjectOCIOConfigDefaults();

    /**
     * @brief A config knob missing from a project file held the Studio default when it was saved,
     * whatever this machine's new-project preference is.
     **/
    void resetOCIOConfigKnobsForRestore();

    /**
     * @brief Must be implemented to initialize any knob using the
     * KnobFactory.
     **/
    virtual void initializeKnobs() OVERRIDE FINAL;

    /**
     * @brief Used to bracket a series of call to onKnobValueChanged(...) in case many complex changes are done
     * at once. If not called, onKnobValueChanged() will call automatically bracket its call be a begin/end
     * but this can lead to worse performance. You can overload this to make all changes to params at once.
     **/
    virtual void beginKnobsValuesChanged(ValueChangedReasonEnum reason) OVERRIDE FINAL;

    /**
     * @brief Used to bracket a series of call to onKnobValueChanged(...) in case many complex changes are done
     * at once. If not called, onKnobValueChanged() will call automatically bracket its call be a begin/end
     * but this can lead to worse performance. You can overload this to make all changes to params at once.
     **/
    virtual void endKnobsValuesChanged(ValueChangedReasonEnum reason)  OVERRIDE FINAL;

    /**
     * @brief Called whenever a param changes. It calls the virtual
     * portion paramChangedByUser(...) and brackets the call by a begin/end if it was
     * not done already.
     **/
    virtual bool onKnobValueChanged(KnobI* k,
                                    ValueChangedReasonEnum reason,
                                    double time,
                                    ViewSpec view,
                                    bool originatedFromMainThread)  OVERRIDE FINAL;

    std::unique_ptr<ProjectPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PROJECT_H
