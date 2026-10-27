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

#include "Project.h"

#include <algorithm> // min, max
#include <atomic>
#include <cassert>
#include <cerrno> // errno
#include <cstdlib> // strtoul
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef __NATRON_WIN32__
#include <stdio.h> //for _snprintf
#include <windows.h> //for GetUserName
#include <Lmcons.h> //for UNLEN
#define snprintf _snprintf
#elif defined(__NATRON_UNIX__)
#include <fcntl.h> // open
#include <pwd.h> //for getpwuid
#include <signal.h> // kill
#include <unistd.h> // link, fsync
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun> // QtCore on Qt4, QtConcurrent on Qt5

#include <ofxhXml.h> // OFX::XML::escape

#include <OpenColorIO/OpenColorIO.h>

#include "Global/GitVersion.h"
#include "Global/StrUtils.h"
#ifdef DEBUG
#include "Global/FloatingPointExceptions.h"
#endif

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/YamlScalars.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/FormatSerialization.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Hash64.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ProjectPrivate.h"
#include "Engine/RFV.h"
#include "Engine/RectDSerialization.h"
#include "Engine/RectISerialization.h"
#include "Engine/RotoLayer.h"
#include "Engine/Settings.h"
#include "Engine/StandardPaths.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_ENTER

using std::cout; using std::endl;
using std::make_pair;


static std::string
getUserName()
{
#ifdef __NATRON_WIN32__
    wchar_t user_name[UNLEN + 1];
    DWORD user_name_size = sizeof(user_name);
    GetUserNameW(user_name, &user_name_size);

    std::wstring wUserName(user_name);
    std::string sUserName = StrUtils::utf16_to_utf8(wUserName);
    return sUserName;
#elif defined(__NATRON_UNIX__)
    struct passwd *passwd;
    passwd = getpwuid( getuid() );

    if (passwd && passwd->pw_name) {
        return passwd->pw_name;
    }

    // getpwuid() returns NULL when the uid has no passwd entry (e.g. no
    // /etc/passwd entry, or an LDAP/NSS lookup failure/outage). Fall back
    // to the environment, then to the raw uid, rather than dereferencing
    // a NULL pointer or inventing a placeholder name that could collide
    // with a real account.
    const char* envUser = std::getenv("USER");
    if (!envUser || !envUser[0]) {
        envUser = std::getenv("LOGNAME");
    }
    if (envUser && envUser[0]) {
        return envUser;
    }

    return std::to_string(getuid());
#endif
}

static std::string
generateGUIUserName()
{
    return getUserName() + '@' + QHostInfo::localHostName().toStdString();
}

static std::string
generateUserFriendlyNatronVersionName()
{
    std::string ret(NATRON_APPLICATION_NAME);

    ret.append(" v");
    ret.append(NATRON_VERSION_STRING);
    const std::string status(NATRON_DEVELOPMENT_STATUS);
    ret.append(" " NATRON_DEVELOPMENT_STATUS);
    if (status == NATRON_DEVELOPMENT_RELEASE_CANDIDATE) {
        ret.append( QString::number(NATRON_BUILD_NUMBER).toStdString() );
    }

    return ret;
}


Project::Project(const AppInstancePtr& appInstance)
    : KnobHolder(appInstance)
    , NodeCollection(appInstance)
    , _imp( new ProjectPrivate(this) )
{
    QObject::connect( _imp->autoSaveTimer.get(), SIGNAL(timeout()), this, SLOT(onAutoSaveTimerTriggered()) );
    _imp->projectId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

std::string
Project::getProjectId() const
{
    return _imp->projectId;
}

bool
Project::isProjectId(const std::string& projectId) const
{
    return !projectId.empty() && (projectId == _imp->projectId);
}

// make_shared enabler (because make_shared needs access to the private constructor)
// see https://stackoverflow.com/a/20961251/2607517
struct Project::MakeSharedEnabler: public Project
{
    MakeSharedEnabler(const AppInstancePtr& appInstance) : Project(appInstance) {
    }
};


ProjectPtr
Project::create(const AppInstancePtr& appInstance)
{
    return std::make_shared<Project::MakeSharedEnabler>(appInstance);
}


Project::~Project()
{
    ///wait for all autosaves to finish
    for (std::list<std::shared_ptr<QFutureWatcher<void> > >::iterator it = _imp->autoSaveFutures.begin(); it != _imp->autoSaveFutures.end(); ++it) {
        (*it)->waitForFinished();
    }

    ///Don't clear autosaves if the program is shutting down by user request.
    ///Even if the user replied she/he didn't want to save the current work, we keep an autosave of it.
    //removeAutoSaves();
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

class LoadProjectSplashScreen_RAII
{
    AppInstanceWPtr app;

public:

    LoadProjectSplashScreen_RAII(const AppInstancePtr& app,
                                 const QString& filename)
        : app(app)
    {
        if (app) {
            app->createLoadProjectSplashScreen(filename);
        }
    }

    ~LoadProjectSplashScreen_RAII()
    {
        AppInstancePtr a = app.lock();

        if (a) {
            a->closeLoadProjectSplashScreen();
        }
    }
};

QString
legacyProjectMessage()
{
    return Project::tr("Legacy .ntp projects are not supported. This version of %1 only opens .%2 projects.")
        .arg(QString::fromUtf8(NATRON_APPLICATION_NAME), QString::fromUtf8(NATRON_PROJECT_FILE_EXT));
}

QString
notAProjectMessage(const QString& fileName)
{
    return Project::tr("%1 is not a %2 project").arg(fileName, QString::fromUtf8(NATRON_APPLICATION_NAME));
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

QString
Project::folderProjectFileName()
{
    return QString::fromUtf8(Serialization::kBundleProjectMember.data(), (int)Serialization::kBundleProjectMember.size());
}

bool
Project::classifyOpenTarget(const QString& path,
                            OpenTarget* out,
                            QString* error)
{
    if (path.isEmpty()) {
        *error = tr("No project file was given.");

        return false;
    }
    const QString absolute = QDir::cleanPath(QDir::current().absoluteFilePath(path));
    const QFileInfo info(absolute);
    if (!info.exists()) {
        *error = tr("%1: no such file.").arg(absolute);

        return false;
    }

    const QString projectYaml = folderProjectFileName();
    QString folder;
    if (info.isDir()) {
        folder = absolute;
    } else if (info.fileName() == projectYaml) {
        folder = info.absolutePath();
    }

    if (!folder.isEmpty()) {
        const QFileInfo folderInfo(folder);
        QString stem = folderInfo.fileName();
        if (!QFileInfo(QDir(folder).filePath(projectYaml)).isFile()) {
            *error = notAProjectMessage(stem.isEmpty() ? folder : stem);

            return false;
        }
        if (stem.isEmpty()) {
            stem = QString::fromUtf8("project");
        }
        QString parent = folderInfo.absolutePath();
        StrUtils::ensureLastPathSeparator(parent);
        out->source = folder;
        out->path = parent;
        out->name = stem + QLatin1Char('.') + QString::fromUtf8(NATRON_PROJECT_FILE_EXT);
        out->isFolder = true;

        return true;
    }

    // Content decides, so a renamed bundle still opens; only the old XML format, or a .ntp that
    // is not a bundle, gets the legacy message.
    const Serialization::ProjectFileKind kind = Serialization::detectProjectFileKind(absolute.toStdString());
    if ((kind == Serialization::ProjectFileKind::eLegacyXml) || ((kind != Serialization::ProjectFileKind::eBundle) && (info.suffix().compare(QString::fromUtf8("ntp"), Qt::CaseInsensitive) == 0))) {
        *error = legacyProjectMessage();

        return false;
    }
    if (kind != Serialization::ProjectFileKind::eBundle) {
        *error = notAProjectMessage(info.fileName());

        return false;
    }

    QString directory = info.absolutePath();
    StrUtils::ensureLastPathSeparator(directory);
    out->source = absolute;
    out->path = directory;
    out->name = info.fileName();
    out->isFolder = false;

    return true;
} // Project::classifyOpenTarget

bool
Project::isLoadedFromFolder() const
{
    QMutexLocker l(&_imp->projectLock);

    return _imp->loadedFromFolder;
}

bool
Project::loadProject(const QString & path,
                     const QString & name,
                     bool isUntitledAutosave,
                     bool attemptToLoadAutosave)
{
    QString targetPath = path;
    if (!targetPath.isEmpty()) {
        StrUtils::ensureLastPathSeparator(targetPath);
    }
    targetPath += name;
    OpenTarget target;
    QString refusal;
    if (!classifyOpenTarget(targetPath, &target, &refusal)) {
        Dialogs::errorDialog(tr("Project loader").toStdString(), refusal.toStdString());

        return false;
    }

    reset(false, true);

    try {
        const QString& realPath = target.path;
        QString realName = target.name;
        QString source = target.source;
        bool isAutoSave = isUntitledAutosave;
        if (!getApp()->isBackground() && !isUntitledAutosave) {
            // In Gui mode, attempt to load an auto-save for this project if there's one.
            QString autosaveFileName;
            bool hasAutoSave = findAutoSaveForProject(target, &autosaveFileName);
            if (hasAutoSave) {
                StandardButtonEnum ret = eStandardButtonNo;
                if (attemptToLoadAutosave) {
                    QString text = tr("A recent auto-save of %1 was found.\n"
                                      "Would you like to use it instead? "
                                      "Clicking No will remove this auto-save.")
                                       .arg(target.name);
                    ret = Dialogs::questionDialog(tr("Auto-save").toStdString(),
                                                  text.toStdString(), false, StandardButtons(eStandardButtonYes | eStandardButtonNo),
                                                  eStandardButtonYes);
                }
                if ( (ret == eStandardButtonNo) || (ret == eStandardButtonEscape) ) {
                    QFile::remove(realPath + autosaveFileName);
                } else {
                    realName = autosaveFileName;
                    source = realPath + realName;
                    isAutoSave = true;
                    {
                        QMutexLocker k(&_imp->projectLock);
                        _imp->lastAutoSaveFilePath = source;
                    }
                }
            }
        }

        bool mustSave = false;
        const bool loaded = loadProjectInternal(realPath, realName, source, isAutoSave, isUntitledAutosave, &mustSave);
        if (loaded) {
            QMutexLocker k(&_imp->projectLock);
            _imp->loadedFromFolder = target.isFolder;
        }
        reportLoadWarnings();
        if (!loaded) {
            appPTR->showErrorLog();
        } else if (mustSave) {
            // Saving now would replace the file with a version that no longer matches what the
            // missing plug-in wrote.
            if (hasMissingPluginNode()) {
                appPTR->writeToErrorLog_mt_safe(tr("Project loader"), QDateTime::currentDateTime(),
                                                tr("The project was not saved automatically after loading because a plug-in it uses is missing."));
            } else if (target.isFolder) {
                // The bundle a folder saves to may already exist and hold other work, so only
                // the user may decide to write it.
                appPTR->writeToErrorLog_mt_safe(tr("Project loader"), QDateTime::currentDateTime(),
                                                tr("The project was not saved automatically after loading because it was opened from a folder."));
            } else {
                saveProject(realPath, realName, 0);
            }
        }
    } catch (const std::exception & e) {
        Dialogs::errorDialog( tr("Project loader").toStdString(), tr("Error while loading project: %1").arg( QString::fromUtf8( e.what() ) ).toStdString() );
        if ( !getApp()->isBackground() ) {
            CreateNodeArgs args(PLUGINID_NATRON_VIEWER, shared_from_this() );
            args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
            getApp()->createNode(args);
        }

        return false;
    } catch (...) {
        Dialogs::errorDialog( tr("Project loader").toStdString(), tr("Unknown error while loading project.").toStdString() );
        if ( !getApp()->isBackground() ) {
            CreateNodeArgs args(PLUGINID_NATRON_VIEWER, shared_from_this() );
            args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
            getApp()->createNode(args);
        }

        return false;
    }

    refreshViewersAndPreviews();

    return true;
} // loadProject

bool
Project::hasMissingPluginNode() const
{
    NodesList nodes;
    getNodes_recursive(nodes, false);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ((*it)->getPluginID() == PLUGINID_NATRON_MISSING_PLUGIN) {
            return true;
        }
    }

    return false;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

// The parameter names the native Read and Write take from openfx-io's GenericOCIO
// (see Engine/Nodes/IO/OcioSpaceKnobs.h). The native OCIO nodes share the config pair
// and check their own colourspaces.
const char* const ocioConfigFileKnobName = "ocioConfigFile";
const char* const ocioWorkingSpaceKnobName = "ocioWorkingSpace";
const char* const ocioColorSpaceKnobNames[] = { "ocioInputSpace", "ocioOutputSpace" };

// A "scheme://" source is a built-in config, not a file on disk.
bool
isOCIOConfigFile(const std::string& source)
{
    return !source.empty() && (source.find("://") == std::string::npos);
}

std::string
getStringKnobValue(const NodePtr& node,
                   const char* knobName)
{
    KnobIPtr knob = node->getKnobByName(knobName);

    if (!knob) {
        return std::string();
    }
    KnobStringBasePtr stringKnob = std::dynamic_pointer_cast<KnobStringBase>(knob);
    if (!stringKnob) {
        return std::string();
    }

    return stringKnob->getValue();
}

void
setHostOCIOKnob(const NodePtr& node,
                const char* knobName,
                const std::string& value)
{
    if (!node) {
        return;
    }
    KnobStringBasePtr knob = std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(knobName));
    if (!knob) {
        return;
    }
    if (knob->getValue() != value) {
        knob->setValue(value, ViewSpec::all(), 0, eValueChangedReasonPluginEdited, 0);
    }
    // The secret state is serialized, so a loaded node may have brought it back visible.
    knob->setSecret(true);
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
Project::pushOCIOConfigToNode(const NodePtr& node)
{
    if (!node) {
        return;
    }
    const std::string source = getOCIOConfigSource();
    const std::string workingSpace = getWorkingColorSpace();

    // The working space is a name in the config, so the plug-in must have loaded the config first.
    setHostOCIOKnob(node, ocioConfigFileKnobName, source);
    setHostOCIOKnob(node, ocioWorkingSpaceKnobName, workingSpace);
}

void
Project::pushOCIOConfigToNodes()
{
    NodesList nodes;

    getNodes_recursive(nodes, false);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        pushOCIOConfigToNode(*it);
    }
}

void
Project::reportUnresolvedOCIOColorSpaces()
{
    const OCIO_NAMESPACE::ConstConfigRcPtr config = _imp->colorManagement->getConfig();

    if (!config) {
        return;
    }
    // Marks the messages this function sets, so that it clears its own and no other.
    const QString header = tr("Unresolved OpenColorIO colorspaces:") + QLatin1Char('\n');

    NodesList nodes;
    getNodes_recursive(nodes, false);

    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        QStringList unresolved;
        for (std::size_t i = 0; i < sizeof(ocioColorSpaceKnobNames) / sizeof(ocioColorSpaceKnobNames[0]); ++i) {
            const std::string colorSpace = getStringKnobValue(*it, ocioColorSpaceKnobNames[i]);
            if (colorSpace.empty() || config->getColorSpace(colorSpace.c_str())) {
                continue;
            }
            unresolved.push_back(tr("%1 = \"%2\" is not a colorspace in the OpenColorIO config \"%3\".")
                                     .arg(QString::fromUtf8(ocioColorSpaceKnobNames[i]))
                                     .arg(QString::fromUtf8(colorSpace.c_str()))
                                     .arg(QString::fromUtf8(config->getName())));
        }

        if (!unresolved.isEmpty()) {
            (*it)->setPersistentMessage(eMessageTypeError, (header + unresolved.join(QString::fromUtf8("\n"))).toStdString());
            continue;
        }
        QString current;
        int type;
        (*it)->getPersistentMessage(&current, &type, false);
        if (current.startsWith(header)) {
            (*it)->clearPersistentMessage(false);
        }
    }
} // Project::reportUnresolvedOCIOColorSpaces

void
Project::reportDataKindConflicts()
{
    NodesList nodes;

    getNodes_recursive(nodes, false);

    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        (*it)->refreshDataKindConflictMessage();
    }
} // Project::reportDataKindConflicts

bool
Project::loadProjectInternal(const QString& path,
                             const QString& name,
                             const QString& source,
                             bool isAutoSave,
                             bool isUntitledAutosave,
                             bool* mustSave)
{
    FlagSetter loadingProjectRAII(true, &_imp->isLoadingProject, &_imp->isLoadingProjectMutex);
    const QString& filePath = source;
    std::cout << tr("Loading project: %1").arg(filePath).toStdString() << std::endl;

    if ( !QFile::exists(filePath) ) {
        throw std::invalid_argument( QString( filePath + QString::fromUtf8(" : no such file.") ).toStdString() );
    }

    {
        QMutexLocker k(&_imp->loadWarningsMutex);
        _imp->loadWarnings.clear();
    }

    const std::string filePathStd = filePath.toStdString();
    switch (Serialization::detectProjectFileKind(filePathStd)) {
    case Serialization::ProjectFileKind::eBundle:
    case Serialization::ProjectFileKind::eDirectory:
    case Serialization::ProjectFileKind::eProjectYaml:
        break;
    case Serialization::ProjectFileKind::eLegacyXml:
        throw std::runtime_error(legacyProjectMessage().toStdString());
    case Serialization::ProjectFileKind::eUnknown:
        throw std::runtime_error(notAProjectMessage(QFileInfo(filePath).fileName()).toStdString());
    }

    LoadProjectSplashScreen_RAII __raii_splashscreen__(getApp(), name);

    // Lives until loading ends: decoded data may still point into the mapped archive.
    std::unique_ptr<Serialization::ProjectBundleReader> bundle;
    Serialization::ProjectSerialization projectSerialization;
    Serialization::YamlWarnings warnings;
    try {
        bundle = std::make_unique<Serialization::ProjectBundleReader>(filePathStd);
        projectSerialization = bundle->readProject(&warnings);
    } catch (const Serialization::ProjectBundleError& e) {
        throw std::runtime_error(e.what());
    } catch (const Serialization::YamlNewerFormatError& e) {
        throw std::runtime_error(e.what());
    } catch (const Serialization::YamlError& e) {
        throw std::runtime_error(tr("Unrecognized or damaged project file: %1").arg(QString::fromUtf8(e.what())).toStdString());
    }
    for (const std::string& warning : warnings) {
        addLoadWarning(QString::fromUtf8(warning.c_str()));
    }

    const Serialization::BundleManifest& manifest = bundle->manifest();
    projectSerialization.creationDate = manifest.creationDate;

    bool ret = false;
    {
        FlagSetter __raii_loadingProjectInternal__(true, &_imp->isLoadingProjectInternal, &_imp->isLoadingProjectMutex);

        ret = fromSerialization(projectSerialization, name, path, mustSave);
    } // __raii_loadingProjectInternal__

    // The manifest holds what changes on every save, so that project.yaml does not.
    _imp->lastAuthorName->setValue(manifest.author);
    const QDateTime savedOn = QDateTime::fromString(QString::fromUtf8(manifest.saveDate.c_str()), Qt::ISODate);
    _imp->saveDate->setValue(savedOn.isValid() ? savedOn.toLocalTime().toString().toStdString() : manifest.saveDate);

    getApp()->onProjectNodesRestored();

    // After every node is restored, so that no node's saved config wins.
    pushOCIOConfigToNodes();
    reportUnresolvedOCIOColorSpaces();
    reportDataKindConflicts();

    Format f;
    getProjectDefaultFormat(&f);
    Q_EMIT formatChanged(f);

    _imp->natronVersion->setValue( generateUserFriendlyNatronVersionName() );
    if (isAutoSave) {
        _imp->autoSetProjectFormat = false;
        if (!isUntitledAutosave) {
            QString projectFilename = QString::fromUtf8( _imp->getProjectFilename().c_str() );
            int found = projectFilename.lastIndexOf( QString::fromUtf8(".autosave") );
            if (found != -1) {
                _imp->setProjectFilename( projectFilename.left(found).toStdString() );
            }
            _imp->hasProjectBeenSavedByUser = true;
        } else {
            _imp->hasProjectBeenSavedByUser = false;
            _imp->setProjectFilename(NATRON_PROJECT_UNTITLED);
            _imp->setProjectPath("");
        }
        _imp->lastAutoSave = QDateTime::currentDateTime();
        _imp->ageSinceLastSave = QDateTime();
        _imp->lastAutoSaveFilePath = filePath;

        QString projectPath = QString::fromUtf8( _imp->getProjectPath().c_str() );
        QString projectFilename = QString::fromUtf8( _imp->getProjectFilename().c_str() );
        Q_EMIT projectNameChanged(projectPath + projectFilename, true);
    } else {
        Q_EMIT projectNameChanged(path + name, false);
    }

    /// Try to take the project lock by creating a lock file
    // A render save is this process's own copy, removed once loaded: a lock beside it would
    // outlive it and guard nothing.
    if (!isAutoSave && !isRenderSaveFile(filePath)) {
        QString lockFilePath = getLockAbsoluteFilePath();
        if ( !QFile::exists(lockFilePath) ) {
            createLockFile();
        }
    }

    getApp()->refreshProjectParamsInPython();
    _imp->runOnProjectLoadCallback();

    ///Process all events before flagging that we're no longer loading the project
    ///to avoid multiple renders being called because of reshape events of viewers
    {
#ifdef DEBUG
        boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
        QCoreApplication::processEvents();
    }

    return ret;
} // Project::loadProjectInternal

bool
Project::saveProject(const QString & path,
                     const QString & name,
                     QString* newFilePath)
{
    return saveProject_imp(path, name, false, true, newFilePath);
}

bool
Project::saveProject_imp(const QString & path,
                         const QString & name,
                         bool autoS,
                         bool updateProjectProperties,
                         QString* newFilePath)
{
    {
        QMutexLocker l(&_imp->isLoadingProjectMutex);
        if (_imp->isLoadingProject) {
            return false;
        }
    }

    const bool onMainThread = qApp && (QThread::currentThread() == qApp->thread());
    // The events handled while this call waits or saves may close the app, which would otherwise
    // release the last reference to the project under it.
    const ProjectPtr self = weak_from_this().lock();
    struct MainThreadSaveScope {
        Project* project;

        explicit MainThreadSaveScope(Project* p)
            : project(p)
        {
            if (project) {
                ++project->_imp->mainThreadSaves;
            }
        }

        ~MainThreadSaveScope()
        {
            if (project && (--project->_imp->mainThreadSaves == 0)) {
                Q_EMIT project->mainThreadSavesEnded();
            }
        }
    };
    const MainThreadSaveScope mainThreadSave(onMainThread ? this : 0);

    // A periodic auto-save that finds a save running has nothing to add to it. Any other save was
    // asked for by the user, a script or a render, and dropping it would lose work or start a
    // render without its project, so it waits for the running save instead.
    const bool isRenderSave = name.contains(QString::fromUtf8("RENDER_SAVE"));
    const bool mayBeSkipped = autoS && !isRenderSave;
    {
        QMutexLocker l(&_imp->isSavingProjectMutex);
        while (_imp->isSavingProject) {
            // The running save is on this thread when an event loop it spun reached here: it can
            // only finish after this call returns.
            if (mayBeSkipped || (_imp->savingThread == QThread::currentThread())) {
                return false;
            }
            ++_imp->savesWaiting;
            std::unique_ptr<QEventLoop> loop;
            if (onMainThread) {
                // Connected before the lock is released, so the end of the running save, which
                // needs the lock, always reaches this loop.
                loop = std::make_unique<QEventLoop>();
                QObject::connect(this, &Project::saveFinished, loop.get(), &QEventLoop::quit, Qt::QueuedConnection);
            }
            // The running save calls the Python save callback, so the caller must not hold the
            // Python lock here: the Python entry points that save release it first (see App).
            if (onMainThread) {
                l.unlock();
                // Blocking the main thread could deadlock a save that needs it, and the window
                // would stop repainting. User input stays queued so nothing can be edited under
                // the save. A close asked for by the window manager or a script is not user
                // input: what would close or reset the project refuses while
                // isSavingOnMainThread(). deleteLater() calls made outside this loop wait until
                // it returns.
                loop->exec(QEventLoop::ExcludeUserInputEvents);
                l.relock();
            } else {
                _imp->savingFinishedCond.wait(&_imp->isSavingProjectMutex);
            }
            --_imp->savesWaiting;
        }
        _imp->isSavingProject = true;
        _imp->savingThread = QThread::currentThread();
    }

    QString ret;
    bool saved = false;
    std::string error;

    try {
        if (!autoS) {
            //if  (!isSaveUpToDate() || !QFile::exists(path+name)) {
            //We are saving, do not autosave.
            _imp->autoSaveTimer->stop();

            ret = saveProjectInternal(path, name, false, updateProjectProperties);

            ///We just saved, remove the last auto-save which is now obsolete
            removeLastAutosave();

            //}
        } else {
            if (updateProjectProperties) {
                ///Replace the last auto-save with a more recent one
                removeLastAutosave();
            }

            ret = saveProjectInternal(path, name, true, updateProjectProperties);
        }
        saved = true;
    } catch (const std::exception & e) {
        error = e.what();
        if (autoS) {
            qDebug() << "Save failure: " << e.what();
        }
    }

    {
        QMutexLocker l(&_imp->isSavingProjectMutex);
        _imp->isSavingProject = false;
        _imp->savingThread = 0;
        _imp->savingFinishedCond.wakeAll();
    }
    Q_EMIT saveFinished();

    // Shown once this save no longer counts as running: a save the dialog's event loop lets in,
    // such as a render's, would otherwise be refused as re-entering it.
    if (!saved && !autoS) {
        Dialogs::errorDialog(tr("Save").toStdString(), error);
    }

    ///Save caches ToC
    appPTR->saveCaches();

    if (newFilePath) {
        *newFilePath = ret;
    }

    // Closing the project after "Save" relies on this: reporting a failed save as done would let
    // the caller discard work that never reached the disk.
    return saved;
} // Project::saveProject_imp

bool
Project::isSavingOnMainThread() const
{
    assert(QThread::currentThread() == qApp->thread());

    return _imp->mainThreadSaves > 0;
}

bool
Project::isSavingOnCurrentThread() const
{
    QMutexLocker l(&_imp->isSavingProjectMutex);

    return _imp->isSavingProject && (_imp->savingThread == QThread::currentThread());
}

static QStringList
findBackups(const QString & filePath)
{
    QStringList ret;
    if ( QFile::exists(filePath) ) {
        ret.append(filePath);
    }
    // find files matching filePath.~[0-9]+~
    std::vector<std::pair<qulonglong, QString>> backups;
    QRegularExpression rx(QString::fromUtf8("\\.~(\\d+)~$"));
    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.fileName();
    QDirIterator it(fileInfo.dir());
    while (it.hasNext()) {
        QString filename = it.next();
        QFileInfo file(filename);

        if (file.isDir()) { // Check if it's a dir
            continue;
        }

        // If the filename contains target string - put it in the hitlist
        QString fn = file.fileName();
        // The pattern ends with $, so there is at most one match and
        // lastIndexIn() is just that match.
        QRegularExpressionMatch backupMatch = rx.match(fn);
        if (fn.startsWith(fileName) && backupMatch.hasMatch() && (backupMatch.capturedStart() == fileName.size())) {
            backups.push_back(std::make_pair(backupMatch.captured(1).toULongLong(), file.filePath()));
        }
    }
    // Rotation renames each backup to the next number, oldest first; an alphabetical order would
    // put .~10~ before .~2~ and rename backups over each other once there are ten of them.
    std::sort(backups.begin(), backups.end());
    for (const std::pair<qulonglong, QString>& backup : backups) {
        ret.append(backup.second);
    }

    return ret;
}

// if filePath matches .*\.~[0-9]+~, increment the backup number
// else append .~1~
static QString
nextBackup(const QString & filePath)
{
    QRegularExpression rx(QString::fromUtf8("\\.~(\\d+)~$"));
    QRegularExpressionMatch match = rx.match(filePath);
    if (match.hasMatch()) {
        int i = match.captured(1).toInt();
        return filePath.left(match.capturedStart()) + QString::fromUtf8(".~%1~").arg(i + 1);
    } else {
        return filePath + QString::fromUtf8(".~1~");
    }
}

void
Project::rotateBackups(const QString& filePath,
                       int saveVersions)
{
    // The list is ordered and starts with filePath itself when it exists, so its first
    // saveVersions entries are what survives the save: filePath (about to be replaced) plus
    // saveVersions - 1 numbered backups.
    QStringList backups = findBackups(filePath);
    const bool targetExists = !backups.isEmpty() && backups.first() == filePath;
    while (backups.size() > std::max(saveVersions, targetExists ? 1 : 0)) {
        QFile::remove(backups.last());
        backups.removeLast();
    }
    if (saveVersions <= 0) {
        return;
    }
    for (int i = backups.size() - 1; i >= (targetExists ? 1 : 0); --i) {
        QFile::rename(backups.at(i), nextBackup(backups.at(i)));
    }
    if (!targetExists) {
        return;
    }
    // filePath must stay in place until the new file has been committed over it, or a failed
    // commit would leave no project at all. A hard link costs nothing whatever the bundle size and
    // keeps the old contents when the commit renames the new file over filePath; a copy covers
    // file systems without hard links.
    const QString backup = nextBackup(filePath);
    if (::link(QFile::encodeName(filePath).constData(), QFile::encodeName(backup).constData()) != 0) {
        QFile::copy(filePath, backup);
    }
}

QString
Project::saveProjectInternal(const QString & path,
                             const QString & name,
                             bool autoSave,
                             bool updateProjectProperties)
{
    bool isRenderSave = name.contains( QString::fromUtf8("RENDER_SAVE") );
    QDateTime time = QDateTime::currentDateTime();
    QString timeStr = time.toString();
    QString filePath;

    if (autoSave) {
        bool appendTimeHash = false;
        if ( path.isEmpty() ) {
            filePath = autoSavesDir();

            //If the auto-save is saved in the AutoSaves directory, there will be other autosaves for other opened
            //projects. We uniquely identity it with the time hash of the current time
            appendTimeHash = true;
        } else {
            filePath = path;
        }
        filePath.append( QLatin1Char('/') );
        filePath.append(name);
        if (!isRenderSave) {
            filePath.append( QString::fromUtf8(".autosave") );
        }
        if (!isRenderSave) {
            if (appendTimeHash) {
                Hash64 timeHash;

                Q_FOREACH(QChar ch, timeStr) {
                    timeHash.append<unsigned short>( ch.unicode() );
                }
                timeHash.computeHash();
                QString timeHashStr = QString::number( timeHash.value() );
                filePath.append(QLatin1Char('.') + timeHashStr);
            }
        }

        if (updateProjectProperties) {
            QMutexLocker l(&_imp->projectLock);
            _imp->lastAutoSaveFilePath = filePath;
        }
    } else {
        filePath = path + name;
    }

    std::string newFilePath = _imp->runOnProjectSaveCallback(filePath.toStdString(), autoSave);
    filePath = QString::fromUtf8( newFilePath.c_str() );

    /// Fix file paths before saving.
    QString oldProjectPath = QString::fromUtf8(_imp->getProjectPath().c_str());

    if (!autoSave && updateProjectProperties) {
        _imp->autoSetProjectDirectory(path);
        _imp->saveDate->setValue(timeStr.toStdString());
        _imp->lastAuthorName->setValue(generateGUIUserName());
        _imp->natronVersion->setValue(generateUserFriendlyNatronVersionName());
    }

    try {
        Serialization::ProjectBundleWriter writer;
        Serialization::ProjectSerialization projectSerialization;
        toSerialization(&projectSerialization);
        writer.setProject(projectSerialization);

        // The bundle folder is named after the project file, never after an autosave or temporary
        // file, so a save and the autosave of the same project agree.
        QString bundleFileName = autoSave ? name : QFileInfo(filePath).fileName();
        if (bundleFileName.endsWith(QString::fromUtf8(".ntpz"), Qt::CaseInsensitive)) {
            bundleFileName.chop(5);
        }
        writer.setRootFolder(bundleFileName.toStdString());

        Serialization::BundleManifest manifest;
        manifest.appVersion = NATRON_VERSION_STRING;
        manifest.appCommit = GIT_COMMIT;
        manifest.saveDate = time.toUTC().toString(Qt::ISODate).toStdString();
        manifest.author = generateGUIUserName();
        manifest.creationDate = getProjectCreationTime();
        writer.setManifest(manifest);

        writeProjectFileAtomically(filePath, [&writer](Serialization::ZipOutput& output) { writer.write(output); }, [&]() {
            if (autoSave) {
                return;
            }
            rotateBackups(filePath, appPTR->getCurrentSettings()->saveVersions()); });
    } catch (const Serialization::YamlError& e) {
        if (!autoSave && updateProjectProperties) {
            _imp->autoSetProjectDirectory(oldProjectPath);
        }
        throw std::runtime_error(tr("The project could not be saved: %1").arg(QString::fromUtf8(e.what())).toStdString());
    } catch (...) {
        if (!autoSave && updateProjectProperties) {
            /// Reset the old project path in case of failure.
            _imp->autoSetProjectDirectory(oldProjectPath);
        }
        throw;
    }

    if (!autoSave && updateProjectProperties) {
        QString lockFilePath = getLockAbsoluteFilePath();
        if ( QFile::exists(lockFilePath) ) {
            ///Remove the previous lock file if there was any
            removeLockFile();
        }
        {
            _imp->setProjectFilename( name.toStdString() );
            _imp->setProjectPath( path.toStdString() );
            QMutexLocker l(&_imp->projectLock);
            _imp->hasProjectBeenSavedByUser = true;
            _imp->loadedFromFolder = false;
            _imp->ageSinceLastSave = time;
        }
        Q_EMIT projectNameChanged(path + name, false); //< notify the gui so it can update the title

        //Create the lock file corresponding to the project
        createLockFile();
    } else if (updateProjectProperties) {
        if (!isRenderSave) {
            QString projectPath = QString::fromUtf8( _imp->getProjectPath().c_str() );
            QString projectFilename = QString::fromUtf8( _imp->getProjectFilename().c_str() );
            Q_EMIT projectNameChanged(projectPath + projectFilename, true);
        }
    }
    if (updateProjectProperties) {
        _imp->lastAutoSave = time;
    }

    return filePath;
} // saveProjectInternal

NATRON_NAMESPACE_ANONYMOUS_ENTER

class SaveFileOutput
    : public Serialization::ZipOutput {
public:
    explicit SaveFileOutput(QSaveFile* file)
        : _file(file)
    {
    }

    void write(const void* data,
               std::size_t size) override
    {
        const char* bytes = static_cast<const char*>(data);
        while (size > 0) {
            const qint64 written = _file->write(bytes, (qint64)size);
            if (written <= 0) {
                throw std::runtime_error(Project::tr("Failed to write %1: %2").arg(_file->fileName()).arg(_file->errorString()).toStdString());
            }
            bytes += written;
            size -= (std::size_t)written;
        }
    }

private:
    QSaveFile* _file;
};

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
Project::writeProjectFileAtomically(const QString& filePath,
                                    const std::function<void(Serialization::ZipOutput&)>& writeArchive,
                                    const std::function<void()>& beforeReplace)
{
    // QSaveFile writes next to filePath, syncs, then renames over it, so the file at filePath is
    // always either the previous complete project or the new one. Destroyed without commit(), it
    // removes what it wrote.
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(tr("Failed to open file %1: %2").arg(filePath).arg(file.errorString()).toStdString());
    }
    SaveFileOutput output(&file);
    writeArchive(output);
    if (!file.flush()) {
        throw std::runtime_error(tr("Failed to write %1: %2").arg(filePath).arg(file.errorString()).toStdString());
    }
    if (beforeReplace) {
        beforeReplace();
    }
    if (!file.commit()) {
        throw std::runtime_error(tr("Failed to save to %1: %2").arg(filePath).arg(file.errorString()).toStdString());
    }
    // The rename that committed the file is only durable once its directory is synced; until
    // then a power cut can bring back the previous file, or no file at all for a first save.
    // The new contents are already complete on disk, so failing here does not fail the save.
    const QByteArray dirPath = QFile::encodeName(QFileInfo(filePath).absolutePath());
    const int dirFd = ::open(dirPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int syncError = (dirFd == -1) ? errno : 0;
    if ((dirFd != -1) && (::fsync(dirFd) != 0)) {
        syncError = errno;
    }
    if (dirFd != -1) {
        ::close(dirFd);
    }
    if (syncError != 0) {
        qWarning() << "Could not sync the directory of" << filePath << ":" << qt_error_string(syncError);
    }
}

void
Project::addLoadWarning(const QString& warning)
{
    appPTR->writeToErrorLog_mt_safe(tr("Project loader"), QDateTime::currentDateTime(), warning);
    QMutexLocker k(&_imp->loadWarningsMutex);
    _imp->loadWarnings.push_back(warning);
}

QStringList
Project::getLoadWarnings() const
{
    QMutexLocker k(&_imp->loadWarningsMutex);

    return _imp->loadWarnings;
}

void
Project::reportLoadWarnings()
{
    const QStringList warnings = getLoadWarnings();

    if (warnings.isEmpty()) {
        return;
    }
    if (getApp()->isBackground()) {
        for (const QString& warning : warnings) {
            std::cerr << warning.toStdString() << std::endl;
        }

        return;
    }
    // A dialog cannot usefully show thousands of lines; the error log keeps every one.
    const int shownCount = 20;
    QStringList shown = warnings.mid(0, shownCount);
    if (warnings.size() > shownCount) {
        shown.push_back(tr("... and %1 more, see the error log.").arg(warnings.size() - shownCount));
    }
    const QString message = tr("The project was loaded, but some of it could not be restored:") + QLatin1Char('\n') + shown.join(QLatin1Char('\n'));
    Dialogs::warningDialog(tr("Project loader").toStdString(), message.toStdString());
}

void
Project::autoSave()
{
    ///don't autosave in background mode...
    if ( getApp()->isBackground() ) {
        return;
    }

    QString path = QString::fromUtf8( _imp->getProjectPath().c_str() );
    QString name = QString::fromUtf8( _imp->getProjectFilename().c_str() );
    saveProject_imp(path, name, true, true, 0);
}

void
Project::triggerAutoSave()
{
    ///Should only be called in the main-thread, that is upon user interaction.
    assert( QThread::currentThread() == qApp->thread() );

    if ( getApp()->isBackground() || !appPTR->isLoaded() || isProjectClosing() ) {
        return;
    }

    if ( !hasProjectBeenSavedByUser() && !appPTR->getCurrentSettings()->isAutoSaveEnabledForUnsavedProjects() ) {
        return;
    }

    {
        QMutexLocker l(&_imp->isLoadingProjectMutex);
        if (_imp->isLoadingProject) {
            return;
        }
    }

    _imp->autoSaveTimer->start( appPTR->getCurrentSettings()->getAutoSaveDelayMS() );
}

void
Project::onAutoSaveTimerTriggered()
{
    assert( !getApp()->isBackground() );

    if ( !getApp() ) {
        return;
    }

    ///check that all schedulers are not working.
    ///If so launch an auto-save, otherwise, restart the timer.
    bool canAutoSave = !hasNodeRendering() && !getApp()->isShowingDialog();
    {
        // An auto-save started now would make the saves waiting for the running one wait longer.
        QMutexLocker l(&_imp->isSavingProjectMutex);
        canAutoSave = canAutoSave && (_imp->savesWaiting == 0);
    }

    if (canAutoSave) {
        std::shared_ptr<QFutureWatcher<void> > watcher = std::make_shared<QFutureWatcher<void> >();
        QObject::connect( watcher.get(), SIGNAL(finished()), this, SLOT(onAutoSaveFutureFinished()) );
        watcher->setFuture(QtConcurrent::run(&Project::autoSave, this));
        _imp->autoSaveFutures.push_back(watcher);
    } else {
        ///If the auto-save failed because a render is in progress, try every 2 seconds to auto-save.
        ///We don't use the user-provided timeout interval here because it could be an inapropriate value.
        _imp->autoSaveTimer->start(2000);
    }
}

void
Project::onAutoSaveFutureFinished()
{
    QFutureWatcherBase* future = qobject_cast<QFutureWatcherBase*>( sender() );

    assert(future);
    for (std::list<std::shared_ptr<QFutureWatcher<void> > >::iterator it = _imp->autoSaveFutures.begin(); it != _imp->autoSaveFutures.end(); ++it) {
        if (it->get() == future) {
            _imp->autoSaveFutures.erase(it);
            break;
        }
    }
}

int
Project::autoSaveProjectNameLength(const QString& fileName)
{
    if (fileName.contains(QString::fromUtf8("RENDER_SAVE"))) {
        return -1;
    }
    const QString ntpExt = QLatin1Char('.') + QString::fromUtf8(NATRON_PROJECT_FILE_EXT);
    const QString autoSaveSuffix = ntpExt + QString::fromUtf8(".autosave");
    const int suffixPos = fileName.indexOf(autoSaveSuffix);
    if (suffixPos == -1) {
        return -1;
    }
    // QSaveFile writes beside its target under the target's name plus a random suffix, so a
    // half-written auto-save (or one a crash cut short) also contains ".autosave". Only the time
    // hash from saveProjectInternal may follow it in a finished auto-save.
    const QString tail = fileName.mid(suffixPos + autoSaveSuffix.size());
    if (!tail.isEmpty()) {
        static const QRegularExpression timeHash(QString::fromUtf8("^\\.\\d+$"));
        if (!timeHash.match(tail).hasMatch()) {
            return -1;
        }
    }

    return suffixPos + ntpExt.size();
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

// Drawn once per process. A process id alone does not tell apart two processes in different pid
// namespaces that share the auto-save folder, as containers sharing a home folder do.
const QString&
renderSaveProcessToken()
{
    static const QString token = QString::fromUtf8("%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));

    return token;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

QString
Project::makeRenderSaveFileName(int appID)
{
    static std::atomic<unsigned long long> renderSaveCount(0);

    // removeStaleRenderSaves() reads the process id and token back.
    return QString::fromUtf8("RENDER_SAVE_%1_%2_%3_%4.%5")
        .arg(QCoreApplication::applicationPid())
        .arg(renderSaveProcessToken())
        .arg(appID)
        .arg(++renderSaveCount)
        .arg(QString::fromUtf8(NATRON_PROJECT_FILE_EXT));
}

qint64
Project::renderSaveGraceSeconds()
{
    return 10 * 60;
}

void
Project::removeStaleRenderSaves()
{
    const QDir savesDir(autoSavesDir());
    const QStringList entries = savesDir.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    static const QRegularExpression renderSaveOwner(QString::fromUtf8("^RENDER_SAVE_(\\d+)_([0-9a-f]{8})_"));
    const QDateTime now = QDateTime::currentDateTimeUtc();

    for (const QString& entry : entries) {
        if (!entry.startsWith(QString::fromUtf8("RENDER_SAVE"))) {
            continue;
        }
        const QRegularExpressionMatch owner = renderSaveOwner.match(entry);
        if (owner.hasMatch()) {
            if (owner.captured(2) == renderSaveProcessToken()) {
                continue;
            }
            const pid_t pid = (pid_t)owner.captured(1).toLongLong();
            if ((pid > 0) && ((::kill(pid, 0) == 0) || (errno == EPERM))) {
                continue;
            }
        }
        const QString filePath = savesDir.filePath(entry);
        if (QFileInfo(filePath).lastModified().toUTC().secsTo(now) < renderSaveGraceSeconds()) {
            continue;
        }
        QFile::remove(filePath);
    }
}

bool
Project::isRenderSaveFile(const QString& filePath)
{
    const QFileInfo info(filePath);

    return info.fileName().startsWith(QString::fromUtf8("RENDER_SAVE")) && (QDir(info.absolutePath()) == QDir(autoSavesDir()));
}

bool
Project::saveForRenderProcess(QString* filePath)
{
    // Only a GUI session creates the directory at startup; a session without one has none yet.
    if (!QDir().mkpath(autoSavesDir())) {
        return false;
    }

    return saveProject_imp(QString(), makeRenderSaveFileName(getApp()->getAppID()), true, false, filePath);
}

bool
Project::findAutoSaveForProject(const OpenTarget& target,
                                QString* autoSaveFileName)
{
    const QString projectAbsFilePath = target.path + target.name;
    QDir savesDir(target.path);
    QStringList entries = savesDir.entryList(QDir::Files | QDir::NoDotAndDotDot);

    Q_FOREACH(const QString &entry, entries) {
        const int projectNameLength = autoSaveProjectNameLength(entry);
        if (projectNameLength == -1) {
            continue;
        }
        QString filename = target.path + entry.left(projectNameLength);
        if (filename != projectAbsFilePath) {
            continue;
        }
        if (target.isFolder) {
            // The folder's bundle need not exist, and the autosave may be from the bundle rather
            // than from this folder: it only stands for the folder if it is newer than its edits.
            const QFileInfo autoSaveInfo(savesDir.filePath(entry));
            const QFileInfo projectYamlInfo(QDir(target.source).filePath(folderProjectFileName()));
            if (autoSaveInfo.lastModified() <= projectYamlInfo.lastModified()) {
                continue;
            }
        } else if (!QFile::exists(filename)) {
            continue;
        }
        *autoSaveFileName = entry;

        return true;
    }

    return false;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

const char* const kCustomOCIOConfigID = "Custom config";
const char* const kDefaultOCIOConfigURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kDefaultColorSpace8Bit = "sRGB Encoded Rec.709 (sRGB)";
const char* const kDefaultColorSpace16Bit = "sRGB Encoded Rec.709 (sRGB)";
const char* const kDefaultColorSpaceLog = "ACEScct";
const char* const kDefaultColorSpaceFloat = "ACEScg";

QString
ocioConfigToolTip()
{
    return Project::tr("The OpenColorIO config of this project. It provides the colorspaces of the "
                       "working space, of the file defaults and of every Read, Write and OCIO node, "
                       "and the displays and views of the viewers. "
                       "When \"%1\" is selected, the \"OpenColorIO config file\" parameter is used.")
        .arg(QString::fromUtf8(kCustomOCIOConfigID));
}

QString
ocioConfigFileToolTip()
{
    return Project::tr("The OpenColorIO config file (config.ocio) used when the OpenColorIO config is \"%1\". "
                       "A relative path or one starting with [%2] is resolved against the project directory.")
        .arg(QString::fromUtf8(kCustomOCIOConfigID))
        .arg(QString::fromUtf8(NATRON_PROJECT_ENV_VAR_NAME));
}

std::vector<ChoiceOption>
namesToOptions(const std::vector<std::string>& names)
{
    std::vector<ChoiceOption> ret;

    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!names[i].empty()) {
            ret.push_back(ChoiceOption(names[i], names[i], std::string()));
        }
    }

    return ret;
}

// The Gui's cascading menu splits labels on '/', so one inside a name is shown as U+2215.
std::string
menuSegment(const std::string& text)
{
    std::string ret;

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '/') {
            ret += "\u2215";
        } else {
            ret += text[i];
        }
    }

    return ret;
}

std::vector<std::string>
familySegments(const std::string& family,
               char separator)
{
    std::vector<std::string> ret;
    std::size_t start = 0;

    while (start <= family.size()) {
        std::size_t end = family.find(separator, start);
        if (end == std::string::npos) {
            end = family.size();
        }
        if (end > start) {
            ret.push_back(menuSegment(family.substr(start, end - start)));
        }
        start = end + 1;
    }

    return ret;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

std::vector<ChoiceOption>
Project::colorSpaceOptions(const ProjectColorManagement& cm)
{
    OCIO_NAMESPACE::ConstConfigRcPtr config = cm.getConfig();
    const std::vector<std::string> names = cm.getColorSpaces();
    const char separator = config ? config->getFamilySeparator() : '/';
    std::vector<std::string> ids;
    std::vector<std::string> leaves;
    std::vector<std::string> paths;
    std::vector<std::string> descriptions;
    std::set<std::string> menus;

    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i].empty()) {
            continue;
        }
        std::string family;
        std::string description;
        if (config) {
            OCIO_NAMESPACE::ConstColorSpaceRcPtr cs = config->getColorSpace(names[i].c_str());
            if (cs && cs->getFamily()) {
                family = cs->getFamily();
            }
            if (cs && cs->getDescription()) {
                description = cs->getDescription();
            }
        }
        std::string path;
        const std::vector<std::string> segments = familySegments(family, separator);
        for (std::size_t s = 0; s < segments.size(); ++s) {
            path += segments[s];
            menus.insert(path);
            path += '/';
        }
        const std::string leaf = menuSegment(names[i]);
        ids.push_back(names[i]);
        leaves.push_back(leaf);
        paths.push_back(path + leaf);
        descriptions.push_back(description);
    }

    // A label that is also a submenu, or a repeated label, would desynchronise the Gui menu's
    // item indices from the option indices.
    std::vector<ChoiceOption> ret;
    std::set<std::string> used;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        std::string label = paths[i];
        while (menus.count(label)) {
            label += '/' + leaves[i];
        }
        std::string unique = label;
        for (int n = 2; menus.count(unique) || used.count(unique); ++n) {
            unique = label + " (" + std::to_string(n) + ")";
        }
        used.insert(unique);
        ret.push_back(ChoiceOption(ids[i], unique, descriptions[i]));
    }

    return ret;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

bool
hasOption(const std::vector<ChoiceOption>& options,
          const std::string& id)
{
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i].id == id) {
            return true;
        }
    }

    return false;
}

std::string
firstPresent(const std::vector<ChoiceOption>& options,
             const std::vector<std::string>& candidates)
{
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].empty() && hasOption(options, candidates[i])) {
            return candidates[i];
        }
    }

    return options.empty() ? std::string() : options.front().id;
}

template <std::size_t N>
std::vector<std::string>
resolveRoles(const ProjectColorManagement& cm,
             const char* const (&roles)[N])
{
    std::vector<std::string> ret;

    for (std::size_t i = 0; i < N; ++i) {
        ret.push_back(cm.resolveRoleOrName(roles[i]));
    }

    return ret;
}

const char* const kWorkingSpaceRoles[] = { "scene_linear" };
const char* const kLogRoles[] = { "compositing_log", "scene_linear" };
const char* const kIntegerRoles[] = { "texture_paint", "color_picking", "scene_linear" };
const int kColorManagementKnobCount = 7;

/*
 * Repopulates the knob and, when its active name is not among the new options, switches it to
 * the first available fallback, recording "label: old -> new" in fallbacks. A name equal to
 * previousDefault was the outgoing config's own default rather than a user choice, so it is not
 * recorded, and with movePreviousDefault it moves to the new config's default even when the new
 * config also has it.
 */
void
repopulateWithFallback(const KnobChoicePtr& knob,
                       const std::vector<ChoiceOption>& options,
                       const std::vector<std::string>& fallbackNames,
                       QStringList* fallbacks,
                       const std::string& previousDefault = std::string(),
                       bool movePreviousDefault = false)
{
    const std::string old = knob->getActiveEntry().id;

    knob->populateChoices(options);
    const bool atPreviousDefault = !old.empty() && (old == previousDefault);
    if (!old.empty() && !(atPreviousDefault && movePreviousDefault) && knob->isActiveEntryPresentInEntries()) {
        return;
    }
    const std::string target = firstPresent(options, fallbackNames);
    if (target.empty()) {
        return;
    }
    if (target == old) {
        return;
    }
    knob->setValueFromID(target, 0);
    if (!old.empty() && !atPreviousDefault && fallbacks) {
        fallbacks->push_back(QString::fromUtf8("%1: %2 -> %3")
                                 .arg(QString::fromUtf8(knob->getLabel().c_str()))
                                 .arg(QString::fromUtf8(old.c_str()))
                                 .arg(QString::fromUtf8(target.c_str())));
    }
}

void
repopulateViewerViews(ProjectPrivate* imp,
                      QStringList* fallbacks,
                      const std::string& previousDefault = std::string(),
                      bool movePreviousDefault = false)
{
    const ProjectColorManagement& cm = *imp->colorManagement;
    const std::string display = imp->viewerDisplay->getActiveEntry().id;
    std::vector<std::string> fallbackNames(1, cm.getDefaultView(display));

    repopulateWithFallback(imp->viewerView, namesToOptions(cm.getViews(display)), fallbackNames, fallbacks, previousDefault, movePreviousDefault);
}

/*
 * Sets each knob's value, or its default value, by name. The file defaults are the literal
 * names of the user's per-category choice, so they stay stable if a future config moves a role;
 * a config that lacks one gets the same role fallback as a config switch.
 */
void
applyChoiceDefault(const KnobChoicePtr& knob,
                   const std::vector<std::string>& candidates,
                   bool asKnobDefault)
{
    const std::string id = firstPresent(knob->getEntries_mt_safe(), candidates);

    if (id.empty()) {
        return;
    }
    if (asKnobDefault) {
        knob->setDefaultValueFromID(id, 0);
    } else {
        knob->setValueFromID(id, 0);
    }
}

std::vector<std::string>
withRoles(const char* name,
          const std::vector<std::string>& roles)
{
    std::vector<std::string> ret(1, std::string(name));

    ret.insert(ret.end(), roles.begin(), roles.end());

    return ret;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
Project::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("Settings") );

    _imp->envVars = AppManager::createKnob<KnobPath>( this, tr("Project Paths") );
    _imp->envVars->setName("projectPaths");
    _imp->envVars->setHintToolTip(tr("Specify here project paths. Any path can be used "
                                     "in file paths and can be used between brackets, for example: \n"
                                     "[%1]MyProject.ntpz \n"
                                     "You can add as many project paths as you want and can name them as you want. This way it "
                                     "makes it easy to share your projects and move files around."
                                     " You can chain up paths, like so:\n "
                                     "[%1] = <PathToProject> \n"
                                     "[Scene1] = [%1]/Rush/S_01 \n"
                                     "[Shot1] = [Scene1]/Shot001 \n"
                                     "By default if a file-path is NOT absolute (i.e: not starting with '/' "
                                     " on Unix or a drive name on Windows) "
                                     "then it will be expanded using the [%1] path. "
                                     "Absolute paths are treated as normal."
                                     " The [%1] path will be set automatically to "
                                     " the location of the project file when saving and loading a project."
                                     " The [%2] path will also be set automatically for better sharing of projects with reader nodes.")
                                      .arg(QString::fromUtf8(NATRON_PROJECT_ENV_VAR_NAME))
                                      .arg(QString::fromUtf8(NATRON_OCIO_ENV_VAR_NAME)));
    _imp->envVars->setSecret(false);
    _imp->envVars->setMultiPath(true);

    page->addKnob(_imp->envVars);

    _imp->formatKnob = AppManager::createKnob<KnobChoice>( this, tr("Project Format") );
    _imp->formatKnob->setHintToolTip( tr("The project format is used as the default workspace size for viewers, the default format for generators, and the default output format for writers.  In most cases this should match your plate resolution.") );
    _imp->formatKnob->setName("outputFormat");

    const std::vector<Format> & appFormats = appPTR->getFormats();
    std::vector<ChoiceOption> entries;
    for (U32 i = 0; i < appFormats.size(); ++i) {
        const Format& f = appFormats[i];
        QString formatStr = ProjectPrivate::generateStringFromFormat(f);
        if ( (f.width() == 1920) && (f.height() == 1080) && (f.getPixelAspectRatio() == 1) ) {
            _imp->formatKnob->setDefaultValue(i, 0);
        }
        if ( !f.getName().empty() ) {
            entries.push_back( ChoiceOption(f.getName(), formatStr.toStdString(), "") );
        } else {
            entries.push_back( ChoiceOption( formatStr.toStdString() ) );
        }

        _imp->builtinFormats.push_back(f);
        ++_imp->formatsGeneration;
        Node::invalidateAllHashes("project formats changed");
    }
    _imp->formatKnob->setAddNewLine(false);

    _imp->formatKnob->populateChoices(entries);
    _imp->formatKnob->setAnimationEnabled(false);
    page->addKnob(_imp->formatKnob);

    QObject::connect( _imp->formatKnob.get(), SIGNAL(populated()), this, SLOT(onProjectFormatPopulated()) );

    _imp->addFormatKnob = AppManager::createKnob<KnobButton>( this, tr("New Format...") );
    _imp->addFormatKnob->setName("newFormat");
    page->addKnob(_imp->addFormatKnob);

    _imp->previewMode = AppManager::createKnob<KnobBool>( this, tr("Auto Previews") );
    _imp->previewMode->setName("autoPreviews");
    _imp->previewMode->setHintToolTip( tr("When checked, preview images in the node graph will be "
                                          "refreshed automatically. Disabling this will improve performance.") );
    _imp->previewMode->setAnimationEnabled(false);
    _imp->previewMode->setEvaluateOnChange(false);
    page->addKnob(_imp->previewMode);
    bool autoPreviewEnabled = appPTR->getCurrentSettings()->isAutoPreviewOnForNewProjects();
    _imp->previewMode->setDefaultValue(autoPreviewEnabled, 0);


    _imp->frameRange = AppManager::createKnob<KnobInt>(this, tr("Frame Range"), 2);
    _imp->frameRange->setDefaultValue(1, 0);
    _imp->frameRange->setDefaultValue(250, 1);
    _imp->frameRange->setDimensionName(0, "first");
    _imp->frameRange->setDimensionName(1, "last");
    _imp->frameRange->setEvaluateOnChange(false);
    _imp->frameRange->setName("frameRange");
    _imp->frameRange->setHintToolTip( tr("The frame range of the project as seen by plug-ins. New viewers are created automatically "
                                         "with this frame-range. By default when the first Read node is created, this range "
                                         "will be set to the "
                                         "sequence legnth, unless the Lock Range parameter is checked.") );
    _imp->frameRange->setAnimationEnabled(false);
    _imp->frameRange->setAddNewLine(false);
    page->addKnob(_imp->frameRange);

    _imp->lockFrameRange = AppManager::createKnob<KnobBool>( this, tr("Lock Range") );
    _imp->lockFrameRange->setName("lockRange");
    _imp->lockFrameRange->setDefaultValue(false);
    _imp->lockFrameRange->setAnimationEnabled(false);
    _imp->lockFrameRange->setHintToolTip( tr("By default when the first Read node is created, this range will be set to the "
                                             "sequence length, unless this parameter is checked.") );
    _imp->lockFrameRange->setEvaluateOnChange(false);
    page->addKnob(_imp->lockFrameRange);

    _imp->frameRate = AppManager::createKnob<KnobDouble>( this, tr("Frame Rate") );
    _imp->frameRate->setName("frameRate");
    _imp->frameRate->setHintToolTip( tr("The frame rate of the project. This will serve as a default value for all effects that don't produce "
                                        "special frame rates.") );
    _imp->frameRate->setAnimationEnabled(false);
    _imp->frameRate->setDefaultValue(24);
    _imp->frameRate->setMinimum(0.);
    _imp->frameRate->setMaximum(std::numeric_limits<int>::max());
    _imp->frameRate->setDisplayMinimum(0.);
    _imp->frameRate->setDisplayMaximum(120.);
    page->addKnob(_imp->frameRate);

    _imp->upAxis = AppManager::createKnob<KnobChoice>(this, tr("Up Axis"));
    _imp->upAxis->setName("upAxis");
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Y", "", tr("Y points up in the project's 3D scenes.").toStdString()));
        entries.push_back(ChoiceOption("Z", "", tr("Z points up in the project's 3D scenes.").toStdString()));
        _imp->upAxis->populateChoices(entries);
    }
    _imp->upAxis->setHintToolTip(tr("The up axis of the project's 3D scenes. Every 3D node authors it as its layer's upAxis; "
                                    "USDRead warns when a file's differs, without converting it."));
    _imp->upAxis->setAnimationEnabled(false);
    _imp->upAxis->setDefaultValue(0);
    page->addKnob(_imp->upAxis);

    _imp->metersPerUnit = AppManager::createKnob<KnobDouble>(this, tr("Meters Per Unit"));
    _imp->metersPerUnit->setName("metersPerUnit");
    _imp->metersPerUnit->setHintToolTip(tr("The length of one scene unit in meters in the project's 3D scenes: 1 for meters, 0.01 "
                                           "for centimeters. Every 3D node authors it as its layer's metersPerUnit, and USDCamera "
                                           "converts its millimetre lens settings with it. USDRead warns when a file's differs, "
                                           "without converting it."));
    _imp->metersPerUnit->setAnimationEnabled(false);
    _imp->metersPerUnit->setDefaultValue(1.);
    _imp->metersPerUnit->setMinimum(1e-6);
    _imp->metersPerUnit->setDisplayMinimum(0.001);
    _imp->metersPerUnit->setDisplayMaximum(1.);
    page->addKnob(_imp->metersPerUnit);

    _imp->gpuSupport = AppManager::createKnob<KnobChoice>( this, tr("GPU Rendering") );
    _imp->gpuSupport->setName("gpuRendering");
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Enabled","",tr("Enable GPU rendering if required resources are available and the plugin supports it.").toStdString()));
        entries.push_back(ChoiceOption("Disabled", "", tr("Disable GPU rendering for all plug-ins.").toStdString()));
        entries.push_back(ChoiceOption("Disabled if background","",tr("Disable GPU rendering when rendering with NatronRenderer but not in GUI mode.").toStdString()));
        _imp->gpuSupport->populateChoices(entries);
    }
    _imp->gpuSupport->setAnimationEnabled(false);
    _imp->gpuSupport->setHintToolTip(tr("Select when to activate GPU rendering for plug-ins. Note that if GPU compute is disabled in the Preferences or no OpenGL renderer is available then GPU rendering will not be activated regardless of that value."));
    _imp->gpuSupport->setEvaluateOnChange(false);
    _imp->gpuSupport->setAllDimensionsEnabled(appPTR->getGPUContextPool()->isRenderingEnabled());
    page->addKnob(_imp->gpuSupport);

    KnobPagePtr viewsPage = AppManager::createKnob<KnobPage>( this, tr("Views") );

    _imp->viewsList = AppManager::createKnob<KnobPath>( this, tr("Views List") );
    _imp->viewsList->setName("viewsList");
    _imp->viewsList->setHintToolTip( tr("The list of the views in the project") );
    _imp->viewsList->setAnimationEnabled(false);
    _imp->viewsList->setEvaluateOnChange(false);
    _imp->viewsList->setAsStringList(true);
    std::list<std::string> defaultViews;
    defaultViews.push_back("Main");
    std::string encodedDefaultViews = _imp->viewsList->encodeToKnobTableFormatSingleCol(defaultViews);
    _imp->viewsList->setDefaultValue(encodedDefaultViews);
    viewsPage->addKnob(_imp->viewsList);

    _imp->setupForStereoButton = AppManager::createKnob<KnobButton>( this, tr("Setup views for stereo") );
    _imp->setupForStereoButton->setName("setupForStereo");
    _imp->setupForStereoButton->setHintToolTip( tr("Quickly setup the views list for stereo") );
    _imp->setupForStereoButton->setEvaluateOnChange(false);
    viewsPage->addKnob(_imp->setupForStereoButton);


    KnobPagePtr LayersPage = AppManager::createKnob<KnobPage>( this, tr("Layers") );

    _imp->defaultLayersList = AppManager::createKnob<KnobLayers>( this, tr("Default Layers") );
    _imp->defaultLayersList->setName("defaultLayers");
    _imp->defaultLayersList->setHintToolTip( tr("The list of the default layers available in layers menus on nodes.") );
    _imp->defaultLayersList->setAnimationEnabled(false);
    _imp->defaultLayersList->setEvaluateOnChange(false);
    // The registry owned by ProjectPrivate is the source of truth; this knob is a
    // read-only view of it and must not be written back to the project file.
    _imp->defaultLayersList->setIsPersistent(false);
    // The default value is a fresh registry with no users, which is exactly what
    // reset() restores when it resets every project knob to its default.
    _imp->defaultLayersList->setDefaultValue(encodeLayersKnobTable());
    LayersPage->addKnob(_imp->defaultLayersList);

    KnobPagePtr colorPage = AppManager::createKnob<KnobPage>(this, tr("Color"));

    _imp->ocioConfig = AppManager::createKnob<KnobChoice>(this, tr("OpenColorIO config"));
    _imp->ocioConfig->setName("ocioConfig");
    _imp->ocioConfig->setHintToolTip(ocioConfigToolTip());
    _imp->ocioConfig->setAnimationEnabled(false);
    _imp->ocioConfig->populateChoices(ProjectColorManagement::builtinConfigOptions());
    _imp->ocioConfig->setDefaultValueFromID(kDefaultOCIOConfigURI, 0);
    colorPage->addKnob(_imp->ocioConfig);

    _imp->ocioConfigFile = AppManager::createKnob<KnobFile>(this, tr("OpenColorIO config file"));
    _imp->ocioConfigFile->setName("ocioConfigFile");
    _imp->ocioConfigFile->setHintToolTip(ocioConfigFileToolTip());
    _imp->ocioConfigFile->setAnimationEnabled(false);
    _imp->ocioConfigFile->setDefaultAllDimensionsEnabled(false);
    // The first default is the one serialization compares against, so it must not depend on
    // this machine's preference.
    _imp->ocioConfigFile->setDefaultValue(std::string(), 0);
    colorPage->addKnob(_imp->ocioConfigFile);

    applyNewProjectOCIOConfigDefaults();

    _imp->workingSpace = AppManager::createKnob<KnobChoice>(this, tr("Working space"));
    _imp->workingSpace->setName("workingSpace");
    _imp->workingSpace->setHintToolTip(tr("The colorspace images are processed in. It defaults to the colorspace of the config's scene_linear role."));
    _imp->workingSpace->setAnimationEnabled(false);
    _imp->workingSpace->setCascading(true);
    colorPage->addKnob(_imp->workingSpace);

    _imp->colorSpace8Bit = AppManager::createKnob<KnobChoice>(this, tr("8-bit files"));
    _imp->colorSpace8Bit->setName("colorSpace8Bit");
    _imp->colorSpace8Bit->setHintToolTip(tr("The default colorspace of new Read and Write nodes for 8-bit files."));
    _imp->colorSpace8Bit->setAnimationEnabled(false);
    _imp->colorSpace8Bit->setCascading(true);
    colorPage->addKnob(_imp->colorSpace8Bit);

    _imp->colorSpace16Bit = AppManager::createKnob<KnobChoice>(this, tr("16-bit files"));
    _imp->colorSpace16Bit->setName("colorSpace16Bit");
    _imp->colorSpace16Bit->setHintToolTip(tr("The default colorspace of new Read and Write nodes for 16-bit integer files."));
    _imp->colorSpace16Bit->setAnimationEnabled(false);
    _imp->colorSpace16Bit->setCascading(true);
    colorPage->addKnob(_imp->colorSpace16Bit);

    _imp->colorSpaceLog = AppManager::createKnob<KnobChoice>(this, tr("Log files"));
    _imp->colorSpaceLog->setName("colorSpaceLog");
    _imp->colorSpaceLog->setHintToolTip(tr("The default colorspace of new Read and Write nodes for log-encoded files, such as Cineon and DPX."));
    _imp->colorSpaceLog->setAnimationEnabled(false);
    _imp->colorSpaceLog->setCascading(true);
    colorPage->addKnob(_imp->colorSpaceLog);

    _imp->colorSpaceFloat = AppManager::createKnob<KnobChoice>(this, tr("Floating-point files"));
    _imp->colorSpaceFloat->setName("colorSpaceFloat");
    _imp->colorSpaceFloat->setHintToolTip(tr("The default colorspace of new Read and Write nodes for floating-point files."));
    _imp->colorSpaceFloat->setAnimationEnabled(false);
    _imp->colorSpaceFloat->setCascading(true);
    colorPage->addKnob(_imp->colorSpaceFloat);

    _imp->viewerDisplay = AppManager::createKnob<KnobChoice>(this, tr("Viewer display"));
    _imp->viewerDisplay->setName("viewerDisplay");
    _imp->viewerDisplay->setHintToolTip(tr("The display new viewers start with."));
    _imp->viewerDisplay->setAnimationEnabled(false);
    colorPage->addKnob(_imp->viewerDisplay);

    _imp->viewerView = AppManager::createKnob<KnobChoice>(this, tr("Viewer view"));
    _imp->viewerView->setName("viewerView");
    _imp->viewerView->setHintToolTip(tr("The view new viewers start with."));
    _imp->viewerView->setAnimationEnabled(false);
    colorPage->addKnob(_imp->viewerView);

    refreshColorManagement(false);
    applyColorManagementDefaults(true);

    KnobPagePtr infoPage = AppManager::createKnob<KnobPage>( this, tr("Info").toStdString() );

    _imp->projectName = AppManager::createKnob<KnobString>( this, tr("Project Name") );
    _imp->projectName->setName("projectName");
    _imp->projectName->setIsPersistent(false);
    _imp->projectName->setReadByRenders(false);
    //    _imp->projectName->setAsLabel();
    _imp->projectName->setDefaultEnabled(0, false);
    _imp->projectName->setAnimationEnabled(false);
    _imp->projectName->setDefaultValue(NATRON_PROJECT_UNTITLED);
    infoPage->addKnob(_imp->projectName);

    _imp->projectPath = AppManager::createKnob<KnobString>( this, tr("Project path") );
    _imp->projectPath->setName("projectPath");
    _imp->projectPath->setIsPersistent(false);
    _imp->projectPath->setAnimationEnabled(false);
    _imp->projectPath->setDefaultEnabled(0, false);
    // _imp->projectPath->setAsLabel();
    infoPage->addKnob(_imp->projectPath);

    _imp->natronVersion = AppManager::createKnob<KnobString>( this, tr("Saved With") );
    _imp->natronVersion->setName("softwareVersion");
    _imp->natronVersion->setHintToolTip( tr("The version of %1 that saved this project for the last time.").arg( QString::fromUtf8(NATRON_APPLICATION_NAME) ) );
    // _imp->natronVersion->setAsLabel();
    _imp->natronVersion->setDefaultEnabled(0, false);
    _imp->natronVersion->setEvaluateOnChange(false);
    _imp->natronVersion->setReadByRenders(false);
    _imp->natronVersion->setAnimationEnabled(false);

    _imp->natronVersion->setDefaultValue( generateUserFriendlyNatronVersionName() );
    infoPage->addKnob(_imp->natronVersion);

    _imp->originalAuthorName = AppManager::createKnob<KnobString>( this, tr("Original Author") );
    _imp->originalAuthorName->setName("originalAuthor");
    _imp->originalAuthorName->setHintToolTip( tr("The user name and host name of the original author of the project.") );
    //_imp->originalAuthorName->setAsLabel();
    _imp->originalAuthorName->setDefaultEnabled(0, false);
    _imp->originalAuthorName->setEvaluateOnChange(false);
    _imp->originalAuthorName->setReadByRenders(false);
    _imp->originalAuthorName->setAnimationEnabled(false);
    std::string authorName = generateGUIUserName();
    _imp->originalAuthorName->setDefaultValue(authorName);
    infoPage->addKnob(_imp->originalAuthorName);

    _imp->lastAuthorName = AppManager::createKnob<KnobString>( this, tr("Last Author") );
    _imp->lastAuthorName->setName("lastAuthor");
    _imp->lastAuthorName->setHintToolTip( tr("The user name and host name of the last author of the project.") );
    // _imp->lastAuthorName->setAsLabel();
    _imp->lastAuthorName->setDefaultEnabled(0, false);
    _imp->lastAuthorName->setEvaluateOnChange(false);
    _imp->lastAuthorName->setReadByRenders(false);
    _imp->lastAuthorName->setAnimationEnabled(false);
    _imp->lastAuthorName->setDefaultValue(authorName);
    infoPage->addKnob(_imp->lastAuthorName);


    _imp->projectCreationDate = AppManager::createKnob<KnobString>( this, tr("Created On") );
    _imp->projectCreationDate->setName("creationDate");
    _imp->projectCreationDate->setHintToolTip( tr("The creation date of the project.") );
    //_imp->projectCreationDate->setAsLabel();
    _imp->projectCreationDate->setDefaultEnabled(0, false);
    _imp->projectCreationDate->setEvaluateOnChange(false);
    _imp->projectCreationDate->setReadByRenders(false);
    _imp->projectCreationDate->setAnimationEnabled(false);
    _imp->projectCreationDate->setDefaultValue( QDateTime::currentDateTime().toString().toStdString() );
    infoPage->addKnob(_imp->projectCreationDate);

    _imp->saveDate = AppManager::createKnob<KnobString>( this, tr("Last Saved On") );
    _imp->saveDate->setName("lastSaveDate");
    _imp->saveDate->setHintToolTip( tr("The date this project was last saved.") );
    //_imp->saveDate->setAsLabel();
    _imp->saveDate->setDefaultEnabled(0, false);
    _imp->saveDate->setEvaluateOnChange(false);
    _imp->saveDate->setReadByRenders(false);
    _imp->saveDate->setAnimationEnabled(false);
    infoPage->addKnob(_imp->saveDate);

    KnobStringPtr comments = AppManager::createKnob<KnobString>( this, tr("Comments") );
    comments->setName("comments");
    comments->setHintToolTip( tr("This area is a good place to write some information about the project such as its authors, license "
                                 "and anything worth mentioning about it.") );
    comments->setAsMultiLine();
    comments->setAnimationEnabled(false);
    infoPage->addKnob(comments);

    KnobPagePtr pythonPage = AppManager::createKnob<KnobPage>( this, tr("Python") );
    _imp->onProjectLoadCB = AppManager::createKnob<KnobString>( this, tr("After Project Loaded") );
    _imp->onProjectLoadCB->setName("afterProjectLoad");
    _imp->onProjectLoadCB->setHintToolTip( tr("Add here the name of a Python-defined function that will be called each time this project "
                                              "is loaded either from an auto-save or by a user action. It will be called immediately after all "
                                              "nodes are re-created. This callback will not be called when creating new projects.\n "
                                              "The signature of the callback is: callback(app) where:\n"
                                              "- app: points to the current application instance.") );
    _imp->onProjectLoadCB->setAnimationEnabled(false);
    std::string onProjectLoad = appPTR->getCurrentSettings()->getDefaultOnProjectLoadedCB();
    _imp->onProjectLoadCB->setDefaultValue(onProjectLoad);
    pythonPage->addKnob(_imp->onProjectLoadCB);


    _imp->onProjectSaveCB = AppManager::createKnob<KnobString>( this, tr("Before Project Save") );
    _imp->onProjectSaveCB->setName("beforeProjectSave");
    _imp->onProjectSaveCB->setHintToolTip( tr("Add here the name of a Python-defined function that will be called each time this project "
                                              "is saved by the user. This will be called prior to actually saving the project and can be used "
                                              "to change the filename of the file.\n"
                                              "The signature of the callback is: callback(filename,app,autoSave) where:\n"
                                              "- filename: points to the filename under which the project will be saved"
                                              "- app: points to the current application instance\n"
                                              "- autoSave: True if the save was called due to an auto-save, False otherwise\n"
                                              "You should return the new filename under which the project should be saved.") );
    _imp->onProjectSaveCB->setAnimationEnabled(false);
    std::string onProjectSave = appPTR->getCurrentSettings()->getDefaultOnProjectSaveCB();
    _imp->onProjectSaveCB->setDefaultValue(onProjectSave);
    pythonPage->addKnob(_imp->onProjectSaveCB);

    _imp->onProjectCloseCB = AppManager::createKnob<KnobString>( this, tr("Before Project Close") );
    _imp->onProjectCloseCB->setName("beforeProjectClose");
    _imp->onProjectCloseCB->setHintToolTip( tr("Add here the name of a Python-defined function that will be called each time this project "
                                               "is closed or if the user closes the application while this project is opened. This is called "
                                               "prior to removing anything from the project.\n"
                                               "The signature of the callback is: callback(app) where:\n"
                                               "- app: points to the current application instance.") );
    _imp->onProjectCloseCB->setAnimationEnabled(false);
    std::string onProjectClose = appPTR->getCurrentSettings()->getDefaultOnProjectCloseCB();
    _imp->onProjectCloseCB->setValue(onProjectClose);
    pythonPage->addKnob(_imp->onProjectCloseCB);

    _imp->onNodeCreated = AppManager::createKnob<KnobString>( this, tr("After Node Created") );
    _imp->onNodeCreated->setName("afterNodeCreated");
    _imp->onNodeCreated->setHintToolTip( tr("Add here the name of a Python-defined function that will be called each time a node "
                                            "is created. The boolean variable userEdited will be set to True if the node was created "
                                            "by the user or False otherwise (such as when loading a project, or pasting a node).\n"
                                            "The signature of the callback is: callback(thisNode, app, userEdited) where:\n"
                                            "- thisNode: the node which has just been created\n"
                                            "- userEdited: a boolean indicating whether the node was created by user interaction or from "
                                            "a script/project load/copy-paste\n"
                                            "- app: points to the current application instance.") );
    _imp->onNodeCreated->setAnimationEnabled(false);
    std::string onNodeCreated = appPTR->getCurrentSettings()->getDefaultOnNodeCreatedCB();
    _imp->onNodeCreated->setDefaultValue(onNodeCreated);
    pythonPage->addKnob(_imp->onNodeCreated);

    _imp->onNodeDeleted = AppManager::createKnob<KnobString>( this, tr("Before Node Removal") );
    _imp->onNodeDeleted->setName("beforeNodeRemoval");
    _imp->onNodeDeleted->setHintToolTip( tr("Add here the name of a Python-defined function that will be called each time a node "
                                            "is about to be deleted. This function will not be called when the project is closing.\n"
                                            "The signature of the callback is: callback(thisNode, app) where:\n"
                                            "- thisNode: the node about to be deleted\n"
                                            "- app: points to the current application instance.") );
    _imp->onNodeDeleted->setAnimationEnabled(false);
    std::string onNodeDelete = appPTR->getCurrentSettings()->getDefaultOnNodeDeleteCB();
    _imp->onNodeDeleted->setDefaultValue(onNodeDelete);
    pythonPage->addKnob(_imp->onNodeDeleted);

    // Both are user knobs, because a group that is not refuses the variables, which are user knobs.
    KnobPagePtr rfvPage = AppManager::createKnob<KnobPage>(this, tr(kProjectParamRFVPageLabel));
    rfvPage->setName(kProjectParamRFVPage);
    rfvPage->setAsUserKnob(true);

    _imp->rfvEditKnob = AppManager::createKnob<KnobButton>(this, tr(kProjectParamRFVEditLabel));
    _imp->rfvEditKnob->setName(kProjectParamRFVEdit);
    _imp->rfvEditKnob->setHintToolTip(tr("Add, edit, reorder or delete the project's variables."));
    rfvPage->addKnob(_imp->rfvEditKnob);

    _imp->rfvGroup = AppManager::createKnob<KnobGroup>(this, tr(kProjectParamRFVVariablesLabel));
    _imp->rfvGroup->setName(kProjectParamRFVVariables);
    _imp->rfvGroup->setAsUserKnob(true);
    _imp->rfvGroup->setDefaultValue(true);
    _imp->rfvGroup->setAsRFVGroup(true);
    rfvPage->addKnob(_imp->rfvGroup);

    comments->setAsMultiLine();
    comments->setAnimationEnabled(false);
    infoPage->addKnob(comments);

    Q_EMIT knobsInitialized();
} // initializeKnobs

void
Project::applyColorManagementDefaults(bool asKnobDefaults)
{
    const ProjectColorManagement& cm = *_imp->colorManagement;

    applyChoiceDefault(_imp->workingSpace, resolveRoles(cm, kWorkingSpaceRoles), asKnobDefaults);
    applyChoiceDefault(_imp->colorSpace8Bit, withRoles(kDefaultColorSpace8Bit, resolveRoles(cm, kIntegerRoles)), asKnobDefaults);
    applyChoiceDefault(_imp->colorSpace16Bit, withRoles(kDefaultColorSpace16Bit, resolveRoles(cm, kIntegerRoles)), asKnobDefaults);
    applyChoiceDefault(_imp->colorSpaceLog, withRoles(kDefaultColorSpaceLog, resolveRoles(cm, kLogRoles)), asKnobDefaults);
    applyChoiceDefault(_imp->colorSpaceFloat, withRoles(kDefaultColorSpaceFloat, resolveRoles(cm, kWorkingSpaceRoles)), asKnobDefaults);
    applyChoiceDefault(_imp->viewerDisplay, std::vector<std::string>(1, cm.getDefaultDisplay()), asKnobDefaults);

    // The view menu depends on the display that was just set.
    repopulateViewerViews(_imp.get(), 0);
    applyChoiceDefault(_imp->viewerView, std::vector<std::string>(1, cm.getDefaultView(_imp->viewerDisplay->getActiveEntry().id)), asKnobDefaults);

    _imp->colorManagement->setWorkingSpace(getWorkingColorSpace());
}

void
Project::refreshColorManagement(bool warnOnFallback,
                                bool restoring)
{
    FlagSetter suppressRefresh(true, &_imp->suppressColorManagementRefresh);
    ProjectColorManagement& cm = *_imp->colorManagement;
    const bool isCustom = _imp->ocioConfig->getActiveEntry().id == kCustomOCIOConfigID;
    const std::string source = getRequestedOCIOConfigSource();
    const std::string envOverride = appPTR->getCurrentSettings()->getOCIOEnvOverride();
    std::string envOverrideError;

    // The config in use must be the file the digest's stamp describes: once Reload re-stamps a
    // file that changed on disk, the same source is loaded again.
    const U64 sourceStamp = isOCIOConfigFile(source) ? FileStampRegistry::instance().stamp(source) : 0;

    // What the outgoing config would have chosen for each knob, read while the menus still hold
    // its names; a knob left at that default follows the new config without a warning.
    std::string previousDefaults[kColorManagementKnobCount];
    if (cm.getConfig()) {
        previousDefaults[0] = firstPresent(_imp->workingSpace->getEntries_mt_safe(), resolveRoles(cm, kWorkingSpaceRoles));
        previousDefaults[1] = firstPresent(_imp->colorSpace8Bit->getEntries_mt_safe(), withRoles(kDefaultColorSpace8Bit, resolveRoles(cm, kIntegerRoles)));
        previousDefaults[2] = firstPresent(_imp->colorSpace16Bit->getEntries_mt_safe(), withRoles(kDefaultColorSpace16Bit, resolveRoles(cm, kIntegerRoles)));
        previousDefaults[3] = firstPresent(_imp->colorSpaceLog->getEntries_mt_safe(), withRoles(kDefaultColorSpaceLog, resolveRoles(cm, kLogRoles)));
        previousDefaults[4] = firstPresent(_imp->colorSpaceFloat->getEntries_mt_safe(), withRoles(kDefaultColorSpaceFloat, resolveRoles(cm, kWorkingSpaceRoles)));
        previousDefaults[5] = cm.getDefaultDisplay();
        previousDefaults[6] = cm.getDefaultView(_imp->viewerDisplay->getActiveEntry().id);
    }

    // Choosing Custom before picking a file is an intermediate state, not an error.
    if (!source.empty() && (!cm.getConfig() || (source != cm.getConfigSource()) || (sourceStamp != _imp->loadedOCIOConfigStamp))) {
        std::string error;
        // The Color page is built before the project path knob exists.
        if (cm.load(source, _imp->projectPath ? _imp->getProjectPath() : std::string(), &error) == ProjectColorManagement::eLoadErrorNone) {
            _imp->loadedOCIOConfigStamp = sourceStamp;
        } else {
            const QString consequence = envOverride.empty()
                ? tr("The previous config stays in use.")
                : tr("Nothing renders until the OCIO environment variable names a config that loads.");
            const QString message = tr("Could not load the OpenColorIO config \"%1\": %2\n%3")
                                        .arg(QString::fromUtf8(source.c_str()))
                                        .arg(QString::fromUtf8(error.c_str()))
                                        .arg(consequence);
            if (!envOverride.empty()) {
                envOverrideError = message.toStdString();
            }
            appPTR->writeToErrorLog_mt_safe(tr("Color management"), QDateTime::currentDateTime(), message);
            getApp()->errorDialog(tr("Color management").toStdString(), message.toStdString(), false);
        }
    }
    {
        QMutexLocker k(&_imp->ocioConfigErrorMutex);
        _imp->ocioConfigError = envOverrideError;
    }
    if (!cm.getConfig()) {
        cm.load(kDefaultOCIOConfigURI, std::string(), 0);
    }

    QStringList fallbacks;
    // A restored name is the saved project's choice, even when it matches the outgoing config's default.
    const bool move = !restoring;
    const std::vector<ChoiceOption> spaces = colorSpaceOptions(cm);
    repopulateWithFallback(_imp->workingSpace, spaces, resolveRoles(cm, kWorkingSpaceRoles), &fallbacks, previousDefaults[0], move);
    repopulateWithFallback(_imp->colorSpace8Bit, spaces, resolveRoles(cm, kIntegerRoles), &fallbacks, previousDefaults[1], move);
    repopulateWithFallback(_imp->colorSpace16Bit, spaces, resolveRoles(cm, kIntegerRoles), &fallbacks, previousDefaults[2], move);
    repopulateWithFallback(_imp->colorSpaceLog, spaces, resolveRoles(cm, kLogRoles), &fallbacks, previousDefaults[3], move);
    repopulateWithFallback(_imp->colorSpaceFloat, spaces, resolveRoles(cm, kWorkingSpaceRoles), &fallbacks, previousDefaults[4], move);
    repopulateWithFallback(_imp->viewerDisplay, namesToOptions(cm.getDisplays()), std::vector<std::string>(1, cm.getDefaultDisplay()), &fallbacks, previousDefaults[5], move);
    repopulateViewerViews(_imp.get(), &fallbacks, previousDefaults[6], move);

    if (warnOnFallback && !fallbacks.isEmpty()) {
        const QString message = tr("The OpenColorIO config \"%1\" does not define some of the project's colorspaces, displays or views. "
                                   "They now use these instead:\n%2")
                                    .arg(QString::fromUtf8(cm.getConfigSource().c_str()))
                                    .arg(fallbacks.join(QString::fromUtf8("\n")));
        appPTR->writeToErrorLog_mt_safe(tr("Color management"), QDateTime::currentDateTime(), message);
        getApp()->warningDialog(tr("Color management").toStdString(), message.toStdString(), false);
    }

    if (envOverride.empty()) {
        _imp->ocioConfig->setAllDimensionsEnabled(true);
        _imp->ocioConfig->setHintToolTip(ocioConfigToolTip());
        _imp->ocioConfigFile->setAllDimensionsEnabled(isCustom);
        _imp->ocioConfigFile->setHintToolTip(ocioConfigFileToolTip());
    } else {
        const QString toolTip = tr("Overridden by the OCIO environment variable (%1)").arg(QString::fromUtf8(envOverride.c_str()));
        _imp->ocioConfig->setAllDimensionsEnabled(false);
        _imp->ocioConfig->setHintToolTip(toolTip);
        _imp->ocioConfigFile->setAllDimensionsEnabled(false);
        _imp->ocioConfigFile->setHintToolTip(toolTip);
    }

    onOCIOConfigPathChanged(cm.getConfigDirectory(), false);

    cm.setWorkingSpace(getWorkingColorSpace());
    // The digest folds in the loaded config's source, which no project knob holds when the
    // environment overrides it.
    Node::invalidateAllHashes("OCIO config loaded");
    pushOCIOConfigToNodes();
    cm.notifyConfigChanged();

    reportUnresolvedOCIOColorSpaces();
} // Project::refreshColorManagement

void
Project::applyNewProjectOCIOConfigDefaults()
{
    const std::string source = appPTR->getCurrentSettings()->getDefaultOCIOConfigSourceForNewProjects();
    const std::string uriPrefix("ocio://");
    const bool isURI = source.compare(0, uriPrefix.size(), uriPrefix) == 0;

    try {
        _imp->ocioConfig->setDefaultValueFromID(isURI ? source : std::string(kCustomOCIOConfigID), 0);
    } catch (const std::runtime_error&) {
        _imp->ocioConfig->setDefaultValueFromID(kDefaultOCIOConfigURI, 0);
    }
    _imp->ocioConfigFile->setDefaultValue(isURI ? std::string() : source, 0);
}

void
Project::resetOCIOConfigKnobsForRestore()
{
    FlagSetter suppressRefresh(true, &_imp->suppressColorManagementRefresh);

    _imp->ocioConfig->setValueFromID(kDefaultOCIOConfigURI, 0);
    _imp->ocioConfigFile->setValue(std::string());
}

std::string
Project::getOCIOConfigSource() const
{
    const std::string effective = _imp->colorManagement->getConfigSource();

    return effective.empty() ? getRequestedOCIOConfigSource() : effective;
}

bool
Project::getOCIOConfigError(std::string* error) const
{
    QMutexLocker k(&_imp->ocioConfigErrorMutex);

    if (_imp->ocioConfigError.empty()) {
        return false;
    }
    if (error) {
        *error = _imp->ocioConfigError;
    }

    return true;
}

std::string
Project::getRequestedOCIOConfigSource() const
{
    const std::string envOverride = appPTR->getCurrentSettings()->getOCIOEnvOverride();
    if (!envOverride.empty()) {
        return envOverride;
    }
    if (!_imp->ocioConfig) {
        return kDefaultOCIOConfigURI;
    }
    const std::string id = _imp->ocioConfig->getActiveEntry().id;
    if (id != kCustomOCIOConfigID) {
        return id;
    }
    std::string path = _imp->ocioConfigFile->getValue();
    if (!path.empty()) {
        // canonicalizePath only reads the project paths.
        const_cast<Project*>(this)->canonicalizePath(path);
    }

    return path;
}

std::string
Project::getWorkingColorSpace() const
{
    return _imp->workingSpace ? _imp->workingSpace->getActiveEntry().id : std::string();
}

std::string
Project::getFileColorSpace(FileColorCategoryEnum category) const
{
    KnobChoicePtr knob;

    switch (category) {
    case eFileColorCategory8Bit:
        knob = _imp->colorSpace8Bit;
        break;
    case eFileColorCategory16Bit:
        knob = _imp->colorSpace16Bit;
        break;
    case eFileColorCategoryLog:
        knob = _imp->colorSpaceLog;
        break;
    case eFileColorCategoryFloat:
        knob = _imp->colorSpaceFloat;
        break;
    }

    return knob ? knob->getActiveEntry().id : std::string();
}

void
Project::getDefaultDisplayView(std::string* display,
                               std::string* view) const
{
    if (display) {
        *display = _imp->viewerDisplay ? _imp->viewerDisplay->getActiveEntry().id : std::string();
    }
    if (view) {
        *view = _imp->viewerView ? _imp->viewerView->getActiveEntry().id : std::string();
    }
}

ProjectColorManagementPtr
Project::getColorManagement() const
{
    return _imp->colorManagement;
}

// don't return a reference to a mutex-protected object!
void
Project::getProjectDefaultFormat(Format *f) const
{
    assert(f);
    QMutexLocker l(&_imp->formatMutex);
    ChoiceOption formatSpec = _imp->formatKnob->getActiveEntry();
    // use the label here, because the id does not contain the format specifications.
    // see ProjectPrivate::generateStringFromFormat()
//#pragma message WARN("TODO: can't we store the format somewhere instead of parsing the label???")
    if ( !formatSpec.label.empty() ) {
        ProjectPrivate::generateFormatFromString(QString::fromUtf8( formatSpec.label.c_str() ), f);
    } else {
        _imp->findFormat(_imp->formatKnob->getValue(), f);
    }
}

bool
Project::getProjectFormatAtIndex(int index,
                                 Format* f) const
{
    assert(f);
    QMutexLocker l(&_imp->formatMutex);

    return _imp->findFormat(index, f);
}

int
Project::currentFrame() const
{
    return _imp->timeline->currentFrame();
}

int
Project::tryAddProjectFormat(const Format & f, bool* existed)
{
    //assert( !_imp->formatMutex.tryLock() );
    if ( ( f.left() >= f.right() ) || ( f.bottom() >= f.top() ) ) {
        return -1;
    }

    std::list<Format>::iterator foundFormat = std::find(_imp->builtinFormats.begin(), _imp->builtinFormats.end(), f);
    if ( foundFormat != _imp->builtinFormats.end() ) {
        *existed = true;
        return std::distance(_imp->builtinFormats.begin(), foundFormat);
    } else {
        foundFormat = std::find(_imp->additionalFormats.begin(), _imp->additionalFormats.end(), f);
        if ( foundFormat != _imp->additionalFormats.end() ) {
            *existed = true;
            return std::distance(_imp->additionalFormats.begin(), foundFormat) + _imp->builtinFormats.size();
        }
    }

    std::vector<ChoiceOption> entries;
    for (std::list<Format>::iterator it = _imp->builtinFormats.begin(); it != _imp->builtinFormats.end(); ++it) {
        const Format & f = *it;
        QString formatStr = ProjectPrivate::generateStringFromFormat(f);
        if ( !f.getName().empty() ) {
            entries.push_back( ChoiceOption(f.getName(), formatStr.toStdString(), "") );
        } else {
            entries.push_back( ChoiceOption( formatStr.toStdString() ) );
        }
    }
    for (std::list<Format>::iterator it = _imp->additionalFormats.begin(); it != _imp->additionalFormats.end(); ++it) {
        const Format & f = *it;
        QString formatStr = ProjectPrivate::generateStringFromFormat(f);
        if ( !f.getName().empty() ) {
            entries.push_back( ChoiceOption(f.getName(), formatStr.toStdString(), "") );
        } else {
            entries.push_back( ChoiceOption( formatStr.toStdString() ) );
        }
    }
    QString formatStr = ProjectPrivate::generateStringFromFormat(f);
    _imp->additionalFormats.push_back(f);
    ++_imp->formatsGeneration;
    Node::invalidateAllHashes("project formats changed");
    if ( !f.getName().empty() ) {
        _imp->formatKnob->appendChoice( ChoiceOption(f.getName(), formatStr.toStdString(), "") );
    } else {
        _imp->formatKnob->appendChoice( ChoiceOption( formatStr.toStdString() ) );
    }

    return ( _imp->builtinFormats.size() + _imp->additionalFormats.size() ) - 1;
}

void
Project::setProjectDefaultFormat(const Format & f)
{
    //assert( !_imp->formatMutex.tryLock() );
    bool existed;
    int index = tryAddProjectFormat(f, &existed);

    _imp->formatKnob->setValue(index);
    ///if locked it will trigger a deadlock because some parameters
    ///might respond to this signal by checking the content of the project format.
}

void
Project::getProjectFormatEntries(std::vector<ChoiceOption>* formatStrings,
                                 int* currentValue) const
{
    *formatStrings = _imp->formatKnob->getEntries_mt_safe();
    *currentValue = _imp->formatKnob->getValue();
}

bool
Project::getFormatNameFromRect(const RectI& rect, double par, std::string* name) const
{
    // First try to find the format in the project formats, they have a good chance to be named
    bool foundFormat = false;
    Format f;
    {
        QMutexLocker k(&_imp->formatMutex);

        for (std::list<Format>::const_iterator it = _imp->builtinFormats.begin(); it!=_imp->builtinFormats.end(); ++it) {
            if (it->x1 == rect.x1 && it->y1 == rect.x1 && it->x2 == rect.x2 && it->y2 == rect.y2 && par == it->getPixelAspectRatio()) {
                f = *it;
                foundFormat = true;
                break;
            }
        }
        if (!foundFormat) {
            for (std::list<Format>::const_iterator it = _imp->additionalFormats.begin(); it!=_imp->additionalFormats.end(); ++it) {
                if (it->x1 == rect.x1 && it->y1 == rect.x1 && it->x2 == rect.x2 && it->y2 == rect.y2 && par == it->getPixelAspectRatio()) {
                    f = *it;
                    foundFormat = true;
                    break;
                }
            }
        }
    }
    name->clear();
    if (foundFormat) {
        *name = f.getName();
    }
    return !name->empty();
}

int
Project::getProjectViewsCount() const
{
    std::list<std::vector<std::string> > pairs;

    _imp->viewsList->getTable(&pairs);

    return (int)pairs.size();
}

bool
Project::isGPURenderingEnabledInProject() const
{
    if (!appPTR->getGPUContextPool()->isRenderingEnabled()) {
        return false;
    }
    int index = _imp->gpuSupport->getValue();

    if (index == 0) {
        return true;
    } else if (index == 1) {
        return false;
    } else if (index == 2) {
        return !getApp()->isBackground();
    }
    assert(false);

    return false;
}

LayerRegistry::AddResultEnum
Project::addLayer(const ImageLayerDesc& desc, LayerRegistryEntry::OriginEnum origin, std::string* error)
{
    LayerRegistry::AddResultEnum ret = _imp->layers->add(desc, origin, error);
    if (ret == LayerRegistry::eAddResultAdded || ret == LayerRegistry::eAddResultGrown) {
        Node::invalidateAllHashes("a layer was added or grown");
        if (!isLoadingProject()) {
            notifyLayersChanged();
        }
    }
    return ret;
}

bool
Project::removeLayer(const std::string& id, std::string* error)
{
    std::list<NodePtr> users;

    getLayerUsers(id, &users);
    if (!users.empty()) {
        if (error) {
            std::string msg = "\"" + id + "\" is used by ";
            bool first = true;
            for (std::list<NodePtr>::const_iterator it = users.begin(); it != users.end(); ++it) {
                if (!first) {
                    msg += ", ";
                }
                first = false;
                msg += (*it)->getScriptName_mt_safe();
            }
            msg += ".";
            *error = msg;
        }
        return false;
    }

    bool ok = _imp->layers->remove(id, error);
    if (ok) {
        Node::invalidateAllHashes("a layer was removed");
        if (!isLoadingProject()) {
            notifyLayersChanged();
        }
    }
    return ok;
}

std::shared_ptr<const std::vector<LayerRegistryEntry>>
Project::getLayerRegistrySnapshot() const
{
    return _imp->layers->snapshot();
}

bool
Project::findLayer(const std::string& id, ImageLayerDesc* out) const
{
    return _imp->layers->find(id, out);
}

void
Project::getLayerUsers(const std::string& id, std::list<NodePtr>* users) const
{
    NodesList nodes;

    getNodes_recursive(nodes, true);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        std::set<std::string> ids;
        (*it)->getReferencedLayerIDs(&ids);
        if (ids.find(id) != ids.end()) {
            users->push_back(*it);
        }
    }
}

std::string
Project::encodeLayersKnobTable() const
{
    std::map<std::string, int> usersCount;
    {
        NodesList nodes;
        getNodes_recursive(nodes, true);
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            std::set<std::string> ids;
            (*it)->getReferencedLayerIDs(&ids);
            for (std::set<std::string>::const_iterator id = ids.begin(); id != ids.end(); ++id) {
                ++usersCount[*id];
            }
        }
    }

    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = _imp->layers->snapshot();
    std::list<std::vector<std::string>> table;
    for (std::size_t i = 0; i < snapshot->size(); ++i) {
        const ImageLayerDesc& desc = (*snapshot)[i].desc;
        std::map<std::string, int>::const_iterator found = usersCount.find(desc.getLayerID());
        table.push_back(KnobLayers::makeRow(desc, found == usersCount.end() ? 0 : found->second));
    }

    return _imp->defaultLayersList->encodeToKnobTableFormat(table);
}

void
Project::refreshLayersKnob()
{
    _imp->defaultLayersList->setValue(encodeLayersKnobTable());
}

void
Project::emitProjectLayersChangedSignal()
{
    refreshLayersKnob();
    Q_EMIT projectLayersChanged();
}

void
Project::notifyLayersChanged()
{
    NodesList nodes;
    getNodes_recursive(nodes, true);
    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        (*it)->refreshChannelSelectors();
    }

    // After the selectors: a selector that loses its layer changes "Used by".
    refreshLayersKnob();
    Q_EMIT projectLayersChanged();
}

const std::vector<std::string>&
Project::getProjectViewNames() const
{
    ///Tls is needed to implement getViewName in the multi-plane suite
    std::vector<std::string>& tls = _imp->tlsData->getOrCreateTLSData()->viewNames;

    tls.clear();

    std::list<std::vector<std::string> > pairs;
    _imp->viewsList->getTable(&pairs);
    for (std::list<std::vector<std::string> >::iterator it = pairs.begin();
         it != pairs.end(); ++it) {
        tls.push_back( (*it)[0] );
    }

    return tls;
}

void
Project::setupProjectForStereo()
{
    std::list<std::string> views;

    views.push_back("Left");
    views.push_back("Right");
    std::string encoded = _imp->viewsList->encodeToKnobTableFormatSingleCol(views);
    _imp->viewsList->setValue(encoded);
}

#if 0 // dead code
// replaced by boost::to_lower(str)
static std::string
toLowerString(const std::string& str)
{
    std::string ret;
    std::locale loc;

    for (std::size_t i = 0; i < str.size(); ++i) {
        ret.push_back( std::tolower(str[i], loc) );
    }

    return ret;
}

// replaced by StrUtils::iequals(lhs, rhs)
static bool
caseInsensitiveCompare(const std::string& lhs,
                       const std::string& rhs)
{
    std::string lowerLhs = toLowerString(lhs);
    std::string lowerRhs = toLowerString(rhs);

    return lowerLhs == lowerRhs;
}

#endif

void
Project::createProjectViews(const std::vector<std::string>& views)
{
    std::list<std::string> pairs;

    _imp->viewsList->getTableSingleCol(&pairs);

    for (std::size_t i = 0; i < views.size(); ++i) {
        bool found = false;
        for (std::list<std::string>::iterator it = pairs.begin(); it != pairs.end(); ++it) {
            if ( StrUtils::iequals(*it, views[i]) ) {
                found = true;
                break;
            }
        }
        if ( found || views[i].empty() ) {
            continue;
        }
        std::string view = views[i];
        view[0] = std::toupper(view[0]);
        pairs.push_back(view);
    }
    std::string encoded = _imp->viewsList->encodeToKnobTableFormatSingleCol(pairs);
    _imp->viewsList->setValue(encoded);
}

QString
Project::getProjectFilename() const
{
    return QString::fromUtf8( _imp->getProjectFilename().c_str() );
}

QString
Project::getLastAutoSaveFilePath() const
{
    QMutexLocker l(&_imp->projectLock);

    return _imp->lastAutoSaveFilePath;
}

bool
Project::hasEverAutoSaved() const
{
    return !getLastAutoSaveFilePath().isEmpty();
}

QString
Project::getProjectPath() const
{
    return QString::fromUtf8( _imp->getProjectPath().c_str() );
}

bool
Project::hasProjectBeenSavedByUser() const
{
    QMutexLocker l(&_imp->projectLock);

    return _imp->hasProjectBeenSavedByUser;
}

bool
Project::isAutoPreviewEnabled() const
{
    return _imp->previewMode->getValue();
}

void
Project::toggleAutoPreview()
{
    _imp->previewMode->setValue( !_imp->previewMode->getValue() );
}

TimeLinePtr Project::getTimeLine() const
{
    return _imp->timeline;
}

void
Project::getAdditionalFormats(std::list<Format> *formats) const
{
    assert(formats);
    QMutexLocker l(&_imp->formatMutex);
    *formats = _imp->additionalFormats;
}

bool
Project::isSaveUpToDate() const
{
    QMutexLocker l(&_imp->projectLock);

    return _imp->ageSinceLastSave == _imp->lastAutoSave;
}

void
Project::beginKnobsValuesChanged(ValueChangedReasonEnum /*reason*/)
{
}

void
Project::endKnobsValuesChanged(ValueChangedReasonEnum /*reason*/)
{
}

///this function is only called on the main thread
bool
Project::onKnobValueChanged(KnobI* knob,
                            ValueChangedReasonEnum reason,
                            double /*time*/,
                            ViewSpec /*view*/,
                            bool /*originatedFromMainThread*/)
{
    bool ret = true;
    bool shouldAutoSave = false;

    // Nodes that read a project variable outside an expression hash its value themselves.
    if (knob && (RFV::findDeclaredVariable(this, knob->getName()).get() == knob)) {
        RFV::rehashExtraContextReaders(*this, knob->getName());
        RFV::scheduleReaderRefresh(getApp());
    }

    if ( knob == _imp->viewsList.get() ) {
        /**
         * All cache entries are linked to a view index which may no longer be correct since the user changed the project settings.
         * The only way to overcome this is to wipe the cache.
         **/
        appPTR->clearAllCaches();

        std::vector<std::string> viewNames = getProjectViewNames();
        getApp()->setupViewersForViews(viewNames);
        if (reason == eValueChangedReasonUserEdited) {
            ///views change, notify all OneView nodes via getClipPreferences
            forceComputeInputDependentDataOnAllTrees();
        }
        Q_EMIT projectViewsChanged();
        shouldAutoSave = true;
    } else if ( knob == _imp->setupForStereoButton.get() ) {
        setupProjectForStereo();
        shouldAutoSave = true;
    } else if ( knob == _imp->formatKnob.get() ) {
        int index = _imp->formatKnob->getValue();
        Format frmt;
        bool found = _imp->findFormat(index, &frmt);
        NodesList nodes;
        getNodes_recursive(nodes, true);

        // Refresh nodes with a format parameter
        std::vector<ChoiceOption> entries = _imp->formatKnob->getEntries_mt_safe();
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            (*it)->refreshFormatParamChoice(entries, index, false);
        }
        if (found) {
            ///Format change, hence probably the PAR so run getClipPreferences again
            forceComputeInputDependentDataOnAllTrees();
            Q_EMIT formatChanged(frmt);
            shouldAutoSave = true;
        }
    } else if ( knob == _imp->addFormatKnob.get() ) {
        Q_EMIT mustCreateFormat();
    } else if ((knob == _imp->rfvEditKnob.get()) && (reason == eValueChangedReasonUserEdited)) {
        Q_EMIT mustEditVariables();
    } else if (knob == _imp->previewMode.get()) {
        Q_EMIT autoPreviewChanged( _imp->previewMode->getValue() );
        shouldAutoSave = true;
    } else if (knob == _imp->frameRate.get()) {
        forceComputeInputDependentDataOnAllTrees();
        shouldAutoSave = true;
    } else if ((knob == _imp->upAxis.get()) || (knob == _imp->metersPerUnit.get())) {
        // Both are in the project digest and every scene layer key, so each scene node re-authors
        // its layer when next evaluated.
        forceComputeInputDependentDataOnAllTrees();
        getApp()->renderAllViewers(true);
        shouldAutoSave = true;
    } else if (knob == _imp->frameRange.get()) {
        int first = _imp->frameRange->getValue(0);
        int last = _imp->frameRange->getValue(1);
        rehashNodesUsingFrameRange();
        Q_EMIT frameRangeChanged(first, last);
        shouldAutoSave = true;
    } else if (knob == _imp->gpuSupport.get()) {

        refreshOpenGLRenderingFlagOnNodes();
        shouldAutoSave = true;
    } else if (knob == _imp->ocioConfig.get() || knob == _imp->ocioConfigFile.get()) {
        if (!_imp->suppressColorManagementRefresh) {
            refreshColorManagement(true);
        }
        shouldAutoSave = true;
    } else if (knob == _imp->viewerDisplay.get()) {
        if (!_imp->suppressColorManagementRefresh) {
            repopulateViewerViews(_imp.get(), 0);
        }
        shouldAutoSave = true;
    } else if (knob == _imp->workingSpace.get()) {
        _imp->colorManagement->setWorkingSpace(getWorkingColorSpace());
        // The viewers' display processors start from the working space, so they must be rebuilt.
        if (!_imp->suppressColorManagementRefresh) {
            pushOCIOConfigToNodes();
            _imp->colorManagement->notifyConfigChanged();
        }
        shouldAutoSave = true;
    } else {
        ret = false;
    }

    // others knobs that should trigger auto save
    if (knob == _imp->colorSpace8Bit.get() || knob == _imp->colorSpace16Bit.get() || knob == _imp->colorSpaceLog.get() || knob == _imp->colorSpaceFloat.get() || knob == _imp->viewerView.get() || knob == _imp->lockFrameRange.get() || knob == _imp->onProjectLoadCB.get() || knob == _imp->onProjectSaveCB.get() || knob == _imp->onProjectCloseCB.get() || knob == _imp->onNodeCreated.get() || knob == _imp->onNodeDeleted.get() || knob == _imp->envVars.get()) {
        shouldAutoSave = true;
    }

    // auto save on project knobs change
    if (shouldAutoSave) {
        _imp->lastAutoSave = QDateTime();
    }

    return ret;
} // Project::onKnobValueChanged

void
Project::refreshOpenGLRenderingFlagOnNodes()
{
    _imp->gpuSupport->setAllDimensionsEnabled(appPTR->getGPUContextPool()->isRenderingEnabled());

    const bool activated = isGPURenderingEnabledInProject();
    NodesList allNodes;
    getNodes_recursive(allNodes, false);
    for (NodesList::iterator it = allNodes.begin(); it!=allNodes.end(); ++it) {
        (*it)->onOpenGLEnabledKnobChangedOnProject(activated);
    }
}

void
Project::onKnobContentChanged(const KnobI* knob)
{
    Node::invalidateAllHashes("a project knob changed", knob);
}

bool
Project::isLoadingProject() const
{
    QMutexLocker l(&_imp->isLoadingProjectMutex);

    return _imp->isLoadingProject;
}

bool
Project::isLoadingProjectInternal() const
{
    QMutexLocker l(&_imp->isLoadingProjectMutex);

    return _imp->isLoadingProjectInternal;
}

bool
Project::isGraphWorthLess() const
{
    /*
       bool worthLess = true;
       for (U32 i = 0; i < _imp->currentNodes.size(); ++i) {
        if (!_imp->currentNodes[i]->isOutputNode() && _imp->currentNodes[i]->isActivated()) {
            worthLess = false;
            break;
        }
       }
       return worthLess;
     */

    ///If it has never auto-saved, then the user didn't do anything, hence the project is worthless.
    return ( !hasEverAutoSaved() && !hasProjectBeenSavedByUser() ) || !hasNodes();
}

QString
Project::getLockAbsoluteFilePath() const
{
    QString projectPath = QString::fromUtf8( _imp->getProjectPath().c_str() );
    QString projectFilename = QString::fromUtf8( _imp->getProjectFilename().c_str() );

    if ( projectPath.isEmpty() ) {
        return QString();
    }
    if ( !projectPath.endsWith( QLatin1Char('/') ) ) {
        projectPath.append( QLatin1Char('/') );
    }
    QString lockFilePath = projectPath + projectFilename + QString::fromUtf8(".lock");

    return lockFilePath;
}

void
Project::createLockFile()
{
    QString lastAuthor = QString::fromUtf8( _imp->lastAuthorName->getValue().c_str() );
    QDateTime now = QDateTime::currentDateTime();
    QString lockFilePath = getLockAbsoluteFilePath();

    if ( lockFilePath.isEmpty() ) {
        return;
    }
    QFile f(lockFilePath);
    if ( !f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate) ) {
        return;
    }
    QTextStream ts(&f);
    QString curDateStr = now.toString();
    ts << curDateStr << '\n'
       << lastAuthor << '\n'
       << (qint64)QCoreApplication::applicationPid() << '\n'
       << QHostInfo::localHostName();
}

void
Project::removeLockFile()
{
    QString lockFilePath = getLockAbsoluteFilePath();

    if ( QFile::exists(lockFilePath) ) {
        QFile::remove(lockFilePath);
    }
}

bool
Project::getLockFileInfos(const QString& projectPath,
                          const QString& projectName,
                          QString* authorName,
                          QString* lastSaveDate,
                          QString* host,
                          qint64* appPID) const
{
    QString realPath = projectPath;
    StrUtils::ensureLastPathSeparator(realPath);
    QString lockFilePath = realPath + projectName + QString::fromUtf8(".lock");
    QFile f(lockFilePath);

    if ( !f.open(QIODevice::ReadOnly) ) {
        return false;
    }
    QTextStream ts(&f);
    if ( !ts.atEnd() ) {
        *lastSaveDate = ts.readLine();
    } else {
        return false;
    }
    if ( !ts.atEnd() ) {
        *authorName = ts.readLine();
    } else {
        return false;
    }
    if ( !ts.atEnd() ) {
        QString pidstr = ts.readLine();
        *appPID = pidstr.toLongLong();
    } else {
        return false;
    }
    // host is optional and was added later
    if ( !ts.atEnd() ) {
        *host = ts.readLine();
    } else {
        *host = tr("unknown host");
    }

    return true;
}

void
Project::removeLastAutosave()
{
    /*
     * First remove the last auto-save registered for this project.
     */
    QString filepath = getLastAutoSaveFilePath();

    if ( !filepath.isEmpty() ) {
        QFile::remove(filepath);
    }

    /*
     * Since we may have saved the project to an old project, overwriting the existing file, there might be
     * a oldProject.ntpz.autosave file next to it that belonged to the old project, make sure it gets removed too
     */
    QString projectPath = QString::fromUtf8( _imp->getProjectPath().c_str() );
    QString projectFilename = QString::fromUtf8( _imp->getProjectFilename().c_str() );
    StrUtils::ensureLastPathSeparator(projectPath);
    QString autoSaveFilePath = projectPath + projectFilename + QString::fromUtf8(".autosave");
    if ( QFile::exists(autoSaveFilePath) ) {
        QFile::remove(autoSaveFilePath);
    }
}

void
Project::clearAutoSavesDir()
{
    /*removing all autosave files*/
    QDir savesDir( autoSavesDir() );
    QStringList entries = savesDir.entryList();

    removeStaleRenderSaves();

    Q_FOREACH (const QString& entry, entries) {
        // A render process of a running Natron may not have loaded its file yet.
        if (entry.startsWith(QString::fromUtf8("RENDER_SAVE"))) {
            continue;
        }

        QString searchStr( QLatin1Char('.') );
        searchStr.append( QString::fromUtf8(NATRON_PROJECT_FILE_EXT) );
        searchStr.append( QLatin1Char('.') );
        int suffixPos = entry.indexOf(searchStr);
        if (suffixPos != -1) {
            QString dirToRemove = savesDir.path();
            if ( !dirToRemove.endsWith( QLatin1Char('/') ) ) {
                dirToRemove += QLatin1Char('/');
            }
            dirToRemove += entry;
            QFile::remove(dirToRemove);
        }
    }
}

QString
Project::autoSavesDir()
{
    QString str = StandardPaths::writableLocation(StandardPaths::eStandardLocationData);
    StrUtils::ensureLastPathSeparator(str);

    str += QString::fromUtf8("Autosaves");

    return str;
}

void
Project::resetProject()
{
    reset(false, true);
    if ( !getApp()->isBackground() ) {
        createViewer();
    }
}

class ResetWatcherArgs
    : public GenericWatcherCallerArgs
{
public:

    bool aboutToQuit;

    ResetWatcherArgs()
        : GenericWatcherCallerArgs()
        , aboutToQuit(false)
    {
    }

    virtual ~ResetWatcherArgs()
    {
    }
};

typedef std::shared_ptr<ResetWatcherArgs> ResetWatcherArgsPtr;

void
Project::reset(bool aboutToQuit, bool blocking)
{
    assert( QThread::currentThread() == qApp->thread() );
    {
        QMutexLocker k(&_imp->projectClosingMutex);
        _imp->projectClosing = true;
    }

    if (!blocking) {
        ResetWatcherArgsPtr args = std::make_shared<ResetWatcherArgs>();
        args->aboutToQuit = aboutToQuit;
        if ( !quitAnyProcessingForAllNodes(this, args) ) {
            doResetEnd(aboutToQuit);
        }
    } else {
        {
            NodesList nodesToWatch;
            getNodes_recursive(nodesToWatch, false);
            for (NodesList::const_iterator it = nodesToWatch.begin(); it != nodesToWatch.end(); ++it) {
                (*it)->quitAnyProcessing_blocking(false);
            }
        }
        doResetEnd(aboutToQuit);
    }
} // Project::reset

void
Project::closeProject_blocking(bool aboutToQuit)
{
    assert( QThread::currentThread() == qApp->thread() );
    {
        QMutexLocker k(&_imp->projectClosingMutex);
        _imp->projectClosing = true;
    }
    NodesList nodesToWatch;
    getNodes_recursive(nodesToWatch, false);
    for (NodesList::iterator it = nodesToWatch.begin(); it != nodesToWatch.end(); ++it) {
        (*it)->quitAnyProcessing_blocking(false);
    }

    doResetEnd(aboutToQuit);
}

void
Project::doResetEnd(bool aboutToQuit)
{
    _imp->runOnProjectCloseCallback();
    _imp->projectId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();

    QString lockFilePath = getLockAbsoluteFilePath();
    QString projectPath = QString::fromUtf8( _imp->getProjectPath().c_str() );
    QString projectFilename = QString::fromUtf8( _imp->getProjectFilename().c_str() );
    ///Remove the lock file if we own it
    if ( QFile::exists(lockFilePath) ) {
        QString author;
        QString lastsave;
        QString host;
        qint64 pid;
        if ( getLockFileInfos(projectPath, projectFilename, &author, &lastsave, &host, &pid) ) {
            if ( pid == QCoreApplication::applicationPid() ) {
                QFile::remove(lockFilePath);
            }
        }
    }


    if (aboutToQuit) {
        clearNodesBlocking();
    } else {
        clearNodesNonBlocking();
    }
    // Idle query stages compose the closed project's layers and keep the files they read open, so
    // a file changed on disk since is read again by the next project.
    SceneCaches::releaseIdleQueryStages();

    if (!aboutToQuit) {
        {
            QMutexLocker l(&_imp->projectLock);
            _imp->autoSetProjectFormat = appPTR->getCurrentSettings()->isAutoProjectFormatEnabled();
            _imp->hasProjectBeenSavedByUser = false;
            _imp->loadedFromFolder = false;
            _imp->projectCreationTime = QDateTime::currentDateTime().toMSecsSinceEpoch();
            _imp->setProjectFilename(NATRON_PROJECT_UNTITLED);
            _imp->setProjectPath("");
            _imp->autoSaveTimer->stop();
            _imp->additionalFormats.clear();
            ++_imp->formatsGeneration;
            Node::invalidateAllHashes("project formats changed");
            _imp->layers.reset(new LayerRegistry());
        }
        {
            // The menu must lose the additional formats too: tryAddProjectFormat() indexes it as
            // builtin + additional, so a stale entry would be selected for the next added format.
            std::vector<ChoiceOption> entries;
            for (std::list<Format>::const_iterator it = _imp->builtinFormats.begin(); it != _imp->builtinFormats.end(); ++it) {
                QString formatStr = ProjectPrivate::generateStringFromFormat(*it);
                if (!it->getName().empty()) {
                    entries.push_back(ChoiceOption(it->getName(), formatStr.toStdString(), ""));
                } else {
                    entries.push_back(ChoiceOption(formatStr.toStdString()));
                }
            }
            _imp->formatKnob->populateChoices(entries);
        }
        getApp()->removeAllKeyframesIndicators();

        Q_EMIT projectNameChanged(QString::fromUtf8(NATRON_PROJECT_UNTITLED), false);
        _imp->clearRFVVariables();
        const KnobsVec & knobs = getKnobs();

        beginChanges();
        {
            // Refreshing midway would apply fallbacks to colourspaces that are about to be reset.
            FlagSetter suppressRefresh(true, &_imp->suppressColorManagementRefresh);
            applyNewProjectOCIOConfigDefaults();
            for (U32 i = 0; i < knobs.size(); ++i) {
                for (int j = 0; j < knobs[i]->getDimension(); ++j) {
                    knobs[i]->resetToDefaultValue(j);
                }
            }
        }

        // The colourspace defaults are indices into the default config's menus, which the
        // reset above may have applied to another config's menus; re-apply them by name.
        refreshColorManagement(false);
        applyColorManagementDefaults(false);

        endChanges(true);
        getApp()->refreshProjectParamsInPython();
    }

    {
        QMutexLocker k(&_imp->projectClosingMutex);
        _imp->projectClosing = false;
    }
} // Project::doResetEnd

bool
Project::quitAnyProcessingForAllNodes(AfterQuitProcessingI* receiver,
                                      const GenericWatcherCallerArgsPtr& args)
{
    NodesList nodesToWatch;

    getNodes_recursive(nodesToWatch, false);
    if ( nodesToWatch.empty() ) {
        return false;
    }
    NodeRenderWatcherPtr renderWatcher = std::make_shared<NodeRenderWatcher>(nodesToWatch);
    QObject::connect(renderWatcher.get(), SIGNAL(taskFinished(int,GenericWatcherCallerArgsPtr)), this, SLOT(onQuitAnyProcessingWatcherTaskFinished(int,GenericWatcherCallerArgsPtr)), Qt::UniqueConnection);
    ProjectPrivate::RenderWatcher p;
    p.receiver = receiver;
    p.watcher = renderWatcher;
    _imp->renderWatchers.push_back(p);
    renderWatcher->scheduleBlockingTask(NodeRenderWatcher::eBlockingTaskQuitAnyProcessing, args);

    return true;
}

void
Project::onQuitAnyProcessingWatcherTaskFinished(int taskID,
                                                const GenericWatcherCallerArgsPtr& args)
{
    NodeRenderWatcher* watcher = dynamic_cast<NodeRenderWatcher*>( sender() );

    assert(watcher);
    if (!watcher) {
        return;
    }
    assert(taskID == (int)NodeRenderWatcher::eBlockingTaskQuitAnyProcessing);
    Q_UNUSED(taskID);

    std::list<ProjectPrivate::RenderWatcher>::iterator found = _imp->renderWatchers.end();
    for (std::list<ProjectPrivate::RenderWatcher>::iterator it = _imp->renderWatchers.begin(); it != _imp->renderWatchers.end(); ++it) {
        if (it->watcher.get() == watcher) {
            found = it;
            break;
        }
    }
    assert( found != _imp->renderWatchers.end() );

    if ( found != _imp->renderWatchers.end() ) {
        AfterQuitProcessingI* receiver = found->receiver;
        assert(receiver);
        // Erase before calling the callback, because the callback might destroy this object
        _imp->renderWatchers.erase(found);
        receiver->afterQuitProcessingCallback(args);
    }
}

void
Project::afterQuitProcessingCallback(const GenericWatcherCallerArgsPtr& args)
{
    ResetWatcherArgs* inArgs = dynamic_cast<ResetWatcherArgs*>( args.get() );

    if (inArgs) {
        doResetEnd(inArgs->aboutToQuit);
    }
}

bool
Project::isAutoSetProjectFormatEnabled() const
{
    QMutexLocker l(&_imp->formatMutex);

    return _imp->autoSetProjectFormat;
}

void
Project::setAutoSetProjectFormatEnabled(bool enabled)
{
    QMutexLocker l(&_imp->formatMutex);

    _imp->autoSetProjectFormat = enabled;
}

void
Project::setOrAddProjectFormat(const Format & frmt,
                               bool skipAdd)
{
    if ( frmt.isNull() ) {
        return;
    }

    bool mustRefreshNodeFormats = false;
    Format dispW;
    {
        QMutexLocker l(&_imp->formatMutex);

        if (_imp->autoSetProjectFormat) {
            _imp->autoSetProjectFormat = false;
            dispW = frmt;

            Format df = appPTR->findExistingFormat( dispW.width(), dispW.height(), dispW.getPixelAspectRatio() );
            if ( !df.isNull() ) {
                dispW.setName( df.getName() );
                setProjectDefaultFormat(dispW);
            } else {
                setProjectDefaultFormat(dispW);
            }
        } else if (!skipAdd) {
            dispW = frmt;
            bool existed = false;
            tryAddProjectFormat(dispW, &existed);
            if (!existed) {
                mustRefreshNodeFormats = true;
            }
        }
    }
    if (mustRefreshNodeFormats) {
        // Refresh nodes with a format parameter
        NodesList nodes;
        getNodes_recursive(nodes, true);
        int index = _imp->formatKnob->getValue();
        std::vector<ChoiceOption> entries = _imp->formatKnob->getEntries_mt_safe();
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            (*it)->refreshFormatParamChoice(entries, index, false);
        }

    }
}

bool
Project::tryLock() const
{
    return _imp->projectLock.tryLock();
}

void
Project::unlock() const
{
    assert( !_imp->projectLock.tryLock() );
    _imp->projectLock.unlock();
}

qint64
Project::getProjectCreationTime() const
{
    return _imp->projectCreationTime.load();
}

U64
Project::getContentDigest() const
{
    // The memo key: every input that is not a project knob is read here, and the project knobs
    // (format, frame rate, views, working space) are covered by the project's knobs
    // generation. The format menu also maps onto the format lists, which have their own counter.
    // The generations are read before anything they cover.
    const U64 knobsGeneration = getKnobsContentGeneration();
    const U64 formatsGeneration = _imp->formatsGeneration.load();
    std::shared_ptr<const std::vector<LayerRegistryEntry>> layers = getLayerRegistrySnapshot();
    const std::string ocioSource = getOCIOConfigSource();
    SettingsPtr settings = appPTR->getCurrentSettings();
    const bool concatenation = settings->isTransformConcatenationEnabled();

    U64 digest;
    {
        QMutexLocker l(&_imp->contentDigestMutex);
        if (_imp->contentDigestValid && (_imp->contentDigestKnobsGeneration == knobsGeneration) && (_imp->contentDigestFormatsGeneration == formatsGeneration) && (_imp->contentDigestLayers == layers) && (_imp->contentDigestOCIOSource == ocioSource) && (_imp->contentDigestConcatenation == concatenation)) {
            digest = _imp->contentDigest;
        } else {
            digest = computeContentDigestWithoutFileStamps(layers, ocioSource, concatenation);
            _imp->contentDigestValid = true;
            _imp->contentDigestKnobsGeneration = knobsGeneration;
            _imp->contentDigestFormatsGeneration = formatsGeneration;
            // Holding the snapshot keeps its address from being reused by a later one.
            _imp->contentDigestLayers = layers;
            _imp->contentDigestOCIOSource = ocioSource;
            _imp->contentDigestConcatenation = concatenation;
            _imp->contentDigest = digest;
        }
    }

    // Reloading the config file re-stamps it with no project edit, so its stamp stays out of the memo.
    ContentHasher h;
    h.appendU64(digest);
    const bool ocioIsFile = isOCIOConfigFile(ocioSource);
    h.appendBool(ocioIsFile);
    if (ocioIsFile) {
        h.appendU64(FileStampRegistry::instance().stamp(ocioSource));
    }

    return h.finalize();
}

U64
Project::computeContentDigestWithoutFileStamps(const std::shared_ptr<const std::vector<LayerRegistryEntry>>& layers,
                                               const std::string& ocioSource,
                                               bool concatenation) const
{
    ContentHasher h;

    Format format;
    getProjectDefaultFormat(&format);
    h.appendI64(format.x1);
    h.appendI64(format.y1);
    h.appendI64(format.x2);
    h.appendI64(format.y2);
    h.appendDouble(format.getPixelAspectRatio());

    // The frame range is left out: adding a sequence Read grows it, which would re-key every node.
    // The nodes that depend on it hash it themselves (Node::dependsOnProjectFrameRange()).
    h.appendDouble(getProjectFrameRate());
    h.appendBool(isProjectUpAxisZ());
    h.appendDouble(getProjectMetersPerUnit());

    const std::vector<std::string>& views = getProjectViewNames();
    h.appendU64(views.size());
    for (std::size_t i = 0; i < views.size(); ++i) {
        h.appendString(views[i]);
    }

    h.appendString(ocioSource);
    h.appendString(getWorkingColorSpace());

    h.appendU64(layers ? layers->size() : 0);
    if (layers) {
        for (std::vector<LayerRegistryEntry>::const_iterator it = layers->begin(); it != layers->end(); ++it) {
            h.appendString(it->desc.getLayerID());
            h.appendString(it->desc.getLayerLabel());
            const std::vector<std::string>& channels = it->desc.getChannels();
            h.appendU64(channels.size());
            for (std::size_t c = 0; c < channels.size(); ++c) {
                h.appendString(channels[c]);
            }
        }
    }

    // GPU rendering is left out: GPU and CPU outputs differ by about a rounding step, so either
    // may serve the other.
    h.appendBool(concatenation);

    return h.finalize();
}

// Functions to escape / unescape characters from UTF-8 XML strings.
// Note that the unescape function matches the escape function,
// and cannot decode any HTML entity (such as Unicode chars).
// A far more complete decoding function can be found at:
// https://bitbucket.org/cggaertner/cstuff/src/master/entities.c
std::string
Project::escapeXML(const std::string &istr)
{
    std::string str( istr );

    for (size_t i = 0; i < str.size(); ++i) {
        switch (str[i]) {
        case '<':
            str.replace(i, 1, "&lt;");
            i += 3;
            break;

        case '>':
            str.replace(i, 1, "&gt;");
            i += 3;
            break;
        case '&':
            str.replace(i, 1, "&amp;");
            i += 4;
            break;
        case '"':
            str.replace(i, 1, "&quot;");
            i += 5;
            break;
        case '\'':
            str.replace(i, 1, "&apos;");
            i += 5;
            break;
        default: {
            unsigned char c = (unsigned char)(str[i]);
            // Escape even the whitespace characters '\n' '\r' '\t', although they are valid
            // XML, because they would be converted to space when re-read.
            // See http://www.w3.org/TR/xml/#AVNormalize
            if ( ( ( 0x01 <= c) && ( c <= 0x1f) ) || ( ( 0x7F <= c) && ( c <= 0x9F) ) ) {
                // these characters must be escaped in XML 1.1
                // http://www.w3.org/TR/xml/#sec-references
                std::string ns = "&#x";
                if (c > 0xf) {
                    int d = c / 0x10;
                    ns += ('0' + d);     // d cannot be more than 9 (because c <= 0x9F)
                }
                int d = c & 0xf;
                ns += d < 10 ? ('0' + d) : ('A' + d - 10);
                ns += ';';
                str.replace(i, 1, ns);
                i += ns.size() + 1;
            } else if (0xC0 <= c && c < 0xE0) { // 2-byte UTF-8
                i += 1;
            } else if (0xE0 <= c && c < 0xF0) { // 3-byte UTF-8
                i += 2;
            } else if (0xF0 <= c && c < 0xF8) { // 4-byte UTF-8
                i += 3;
            }

            break;
        }
        }
    }
    assert( str == OFX::XML::escape(istr) ); // check that this escaped string is consistent with the one in HostSupport
    return str;
} // Project::escapeXML

std::string
Project::unescapeXML(const std::string &istr)
{
    std::string str = istr;

    for (size_t i = 0; i < str.size(); ++i) {
        unsigned char c = (unsigned char)(str[i]);
        if (0xC0 <= c && c < 0xE0) { // 2-byte UTF-8
            i += 1;
        } else if (0xE0 <= c && c < 0xF0) { // 3-byte UTF-8
            i += 2;
        } else if (0xF0 <= c && c < 0xF8) { // 4-byte UTF-8
            i += 3;
        } else if (str[i] == '&') {
            if ( !str.compare(i + 1, 3, "lt;") ) {
                str.replace(i, 4, 1, '<');
            } else if ( !str.compare(i + 1, 3, "gt;") ) {
                str.replace(i, 4, 1, '>');
            } else if ( !str.compare(i + 1, 4, "amp;") ) {
                str.replace(i, 5, 1, '&');
            } else if ( !str.compare(i + 1, 5, "apos;") ) {
                str.replace(i, 6, 1, '\'');
            } else if ( !str.compare(i + 1, 5, "quot;") ) {
                str.replace(i, 6, 1, '"');
            } else if ( !str.compare(i + 1, 1, "#") ) {
                size_t end = str.find_first_of(";", i + 2);
                if (end == std::string::npos) {
                    // malformed XML
                    return str;
                }
                char *tail = NULL;
                int errno_save = errno;
                bool hex = str[i + 2] == 'x' || str[i + 2] == 'X';
                int prefix = hex ? 3 : 2; // prefix length: "&#" or "&#x"
                char *head = &str[i + prefix];

                errno = 0;
                unsigned long cp = std::strtoul(head, &tail, hex ? 16 : 10);
                bool fail = errno || (tail - &str[0]) != (long)end || cp > 0xff; // only handle 0x01-0xff
                errno = errno_save;
                if (fail) {
                    return str;
                }
                // replace from '&' to ';' (thus the +1)
                str.replace(i, tail - head + 1 + prefix, 1, (char)cp);
            }
        }
    }

    return str;
}

void
Project::getEnvironmentVariables(std::map<std::string, std::string>& env) const
{
    std::list<std::vector<std::string> > table;

    _imp->envVars->getTable(&table);
    for (std::list<std::vector<std::string> >::iterator it = table.begin(); it != table.end(); ++it) {
        assert(it->size() == 2);
        env[(*it)[0]] = (*it)[1];
    }
}

void
Project::expandVariable(const std::map<std::string, std::string>& env,
                        std::string& str)
{
    ///Loop while we can still expand variables, up to NATRON_PROJECT_ENV_VAR_MAX_RECURSION recursions
    for (int i = 0; i < NATRON_PROJECT_ENV_VAR_MAX_RECURSION; ++i) {
        for (std::map<std::string, std::string>::const_iterator it = env.begin(); it != env.end(); ++it) {
            if ( expandVariable(it->first, it->second, str) ) {
                break;
            }
        }
    }
}

bool
Project::expandVariable(const std::string& varName,
                        const std::string& varValue,
                        std::string& str)
{
    if ( ( str.size() > (varName.size() + 2) ) && ///can contain the environment variable name
         ( str[0] == '[') && /// env var name is bracketed
         ( str.substr( 1, varName.size() ) == varName) && /// starts with the environment variable name
         ( str[varName.size() + 1] == ']') ) { /// env var name is bracketed
        str.erase(str.begin() + varName.size() + 1);
        str.erase( str.begin() );
        str.replace(0, varName.size(), varValue);

        return true;
    }

    return false;
}

void
Project::findReplaceVariable(const std::map<std::string, std::string>& env,
                             std::string& str)
{
    std::string longestName;
    std::string longestVar;

    for (std::map<std::string, std::string>::const_iterator it = env.begin(); it != env.end(); ++it) {
        if ( ( str.size() >= it->second.size() ) &&
             ( it->second.size() > longestVar.size() ) &&
             ( str.substr( 0, it->second.size() ) == it->second) ) {
            longestName = it->first;
            longestVar = it->second;
        }
    }
    if ( !longestName.empty() && !longestVar.empty() ) {
        std::string replaceStr;
        replaceStr += '[';
        replaceStr += longestName;
        replaceStr += ']';
        str.replace(0, longestVar.size(), replaceStr);
    }
}

void
Project::makeRelativeToVariable(const std::string& varName,
                                const std::string& varValue,
                                std::string& str)
{
    bool hasTrailingSep = !varValue.empty() && (varValue[varValue.size() - 1] == '/' || varValue[varValue.size() - 1] == '\\');

    if ( ( str.size() > varValue.size() ) && (str.substr( 0, varValue.size() ) == varValue) ) {
        if (hasTrailingSep) {
            str = '[' + varName + ']' + str.substr( varValue.size(), str.size() );
        } else {
            str = '[' + varName + "]/" + str.substr( varValue.size() + 1, str.size() );
        }
    } else {
        QDir dir( QString::fromUtf8( varValue.c_str() ) );
        QString relative = dir.relativeFilePath( QString::fromUtf8( str.c_str() ) );
        if (hasTrailingSep) {
            str = '[' + varName + ']' + relative.toStdString();
        } else {
            str = '[' + varName + "]/" + relative.toStdString();
        }
    }
}

bool
Project::fixFilePath(const std::string& projectPathName,
                     const std::string& newProjectPath,
                     std::string& filePath)
{
    if ( ( filePath.size() < (projectPathName.size() + 2) ) //< filepath doesn't have enough space to  contain the variable
         || ( filePath[0] != '[')
         || ( filePath[projectPathName.size() + 1] != ']')
         || ( filePath.substr( 1, projectPathName.size() ) != projectPathName) ) {
        return false;
    }

    canonicalizePath(filePath);

    if ( newProjectPath.empty() ) {
        return true; //keep it absolute if the variables points to nothing
    } else {
        QDir dir( QString::fromUtf8( newProjectPath.c_str() ) );
        if ( !dir.exists() ) {
            return false;
        }

        filePath = dir.relativeFilePath( QString::fromUtf8( filePath.c_str() ) ).toStdString();
        if (newProjectPath[newProjectPath.size() - 1] == '/') {
            filePath = '[' + projectPathName + ']' + filePath;
        } else {
            filePath = '[' + projectPathName + "]/" + filePath;
        }

        return true;
    }
}

bool
Project::isRelative(const std::string& str)
{
    // A URI such as OpenColorIO's "ocio://<name>" names a built-in resource rather
    // than a location on disk, so resolving it against the project directory turns
    // it into a path that exists nowhere.
    if (hasUriScheme(str)) {
        return false;
    }
#ifdef __NATRON_WIN32__

    return ( str.empty() || ( !str.empty() && (str[0] != '/')
                              && ( !(str.size() >= 2 && str[1] == ':') ) ) );
#else  //Unix
    return ( str.empty() || (str[0] != '/') );
#endif
}

bool
Project::hasUriScheme(const std::string& str)
{
    const std::string::size_type sep = str.find("://");

    if ((sep == std::string::npos) || (sep == 0)) {
        return false;
    }
    for (std::string::size_type i = 0; i < sep; ++i) {
        const char c = str[i];
        const bool isSchemeChar = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '+') || (c == '-') || (c == '.');
        if (!isSchemeChar) {
            return false;
        }
    }

    return true;
}

void
Project::canonicalizePath(std::string& str)
{
    std::map<std::string, std::string> envvar;

    getEnvironmentVariables(envvar);

    expandVariable(envvar, str);

    if ( !str.empty() ) {
        ///Now check if the string is relative

        if ( isRelative(str) ) {
            ///If it doesn't start with an env var but is relative, prepend the project env var
            std::map<std::string, std::string>::iterator foundProject = envvar.find(NATRON_PROJECT_ENV_VAR_NAME);
            if ( foundProject != envvar.end() ) {
                if ( foundProject->second.empty() ) {
                    return;
                }
                const char& c = foundProject->second[foundProject->second.size() - 1];
                bool addTrailingSlash = c != '/' && c != '\\';
                std::string copy = foundProject->second;
                if (addTrailingSlash) {
                    copy += '/';
                }
                copy += str;
                str = copy;
            }
        }

        ///Canonicalize
        QFileInfo info( QString::fromUtf8( str.c_str() ) );
        QString canonical =  info.canonicalFilePath();
        if ( canonical.isEmpty() ) {
            return;
        } else {
            str = canonical.toStdString();
        }
    }
}

void
Project::simplifyPath(std::string& str)
{
    std::map<std::string, std::string> envvar;

    getEnvironmentVariables(envvar);

    Project::findReplaceVariable(envvar, str);
}

void
Project::makeRelativeToProject(std::string& str)
{
    std::map<std::string, std::string> envvar;

    getEnvironmentVariables(envvar);

    std::map<std::string, std::string>::iterator found = envvar.find(NATRON_PROJECT_ENV_VAR_NAME);
    if ( ( found != envvar.end() ) && !found->second.empty() ) {
        makeRelativeToVariable(found->first, found->second, str);
    }
}

void
Project::onOCIOConfigPathChanged(const std::string& path,
                                 bool block)
{
    beginChanges();

    try {
        std::string oldEnv;
        try {
            oldEnv = _imp->envVars->getValue();
        } catch (...) {
            // ignore
        }
        std::list<std::vector<std::string> > table;
        _imp->envVars->decodeFromKnobTableFormat(oldEnv, &table);

        // An empty value would make simplifyPath rewrite every path as relative to [OCIO].
        bool found = false;
        for (std::list<std::vector<std::string> >::iterator it = table.begin(); it != table.end(); ++it) {
            if ((*it)[0] == NATRON_OCIO_ENV_VAR_NAME) {
                found = true;
                if (path.empty()) {
                    table.erase(it);
                } else {
                    (*it)[1] = path;
                }
                break;
            }
        }
        if (!found && !path.empty()) {
            std::vector<std::string> vec(2);
            vec[0] = NATRON_OCIO_ENV_VAR_NAME;
            vec[1] = path;
            table.push_back(vec);
        }

        std::string newEnv = _imp->envVars->encodeToKnobTableFormat(table);

        if (oldEnv != newEnv) {
            if (!path.empty() && appPTR->getCurrentSettings()->isAutoFixRelativeFilePathEnabled()) {
                fixRelativeFilePaths(NATRON_OCIO_ENV_VAR_NAME, path, block);
            }
            _imp->envVars->setValue(newEnv);
        }
        endChanges(block);
    } catch (std::logic_error&) {
        // ignore
    }
}

double
Project::getProjectFrameRate() const
{
    return _imp->frameRate->getValue();
}

bool
Project::isProjectUpAxisZ() const
{
    return _imp->upAxis->getValue() == 1;
}

double
Project::getProjectMetersPerUnit() const
{
    return _imp->metersPerUnit->getValue();
}

KnobPathPtr
Project::getEnvVarKnob() const
{
    return _imp->envVars;
}

KnobGroupPtr
Project::getRFVGroup() const
{
    return _imp->rfvGroup;
}

void
Project::refreshVariablesAfterTimeChange(bool isPlayback,
                                         double time)
{
    assert(QThread::currentThread() == qApp->thread());
    if (!_imp->rfvGroup) {
        return;
    }
    const KnobsVec children = _imp->rfvGroup->getChildren();
    for (KnobsVec::const_iterator it = children.begin(); it != children.end(); ++it) {
        (*it)->onTimeChanged(isPlayback, time);
    }
}

void
Project::onUserKnobsChanged()
{
    RFV::refreshNodeVariableGroups(*this);
    // An expression reading rfv may name a variable the project declares only from here on.
    RFV::refreshExpressionListeners(*this);
    RFV::rehashExtraContextReaders(*this, std::string());
    RFV::scheduleReaderRefresh(getApp());
    // Such an expression failed while the variable was undeclared and is applied again.
    AppInstancePtr app = getApp();
    if (app) {
        app->recheckInvalidExpressions();
    }
}

std::string
Project::getOnProjectLoadCB() const
{
    return _imp->onProjectLoadCB->getValue();
}

std::string
Project::getOnProjectSaveCB() const
{
    return _imp->onProjectSaveCB->getValue();
}

std::string
Project::getOnProjectCloseCB() const
{
    return _imp->onProjectCloseCB->getValue();
}

std::string
Project::getOnNodeCreatedCB() const
{
    return _imp->onNodeCreated->getValue();
}

std::string
Project::getOnNodeDeleteCB() const
{
    return _imp->onNodeDeleted->getValue();
}

bool
Project::isProjectClosing() const
{
    QMutexLocker k(&_imp->projectClosingMutex);

    return _imp->projectClosing;
}

bool
Project::isFrameRangeLocked() const
{
    return _imp->lockFrameRange->getValue();
}

void
Project::getFrameRange(double* first,
                       double* last) const
{
    *first = _imp->frameRange->getValue(0);
    *last = _imp->frameRange->getValue(1);
}

void
Project::unionFrameRangeWith(int first,
                             int last)
{
    int curFirst, curLast;
    bool mustSet = !_imp->frameRange->hasModifications() && first != last;

    curFirst = _imp->frameRange->getValue(0);
    curLast = _imp->frameRange->getValue(1);
    const int oldFirst = curFirst;
    const int oldLast = curLast;
    curFirst = !mustSet ? std::min(first, curFirst) : first;
    curLast = !mustSet ? std::max(last, curLast) : last;
    const bool changed = (curFirst != oldFirst) || (curLast != oldLast);
    beginChanges();
    _imp->frameRange->setValue(curFirst, ViewSpec::all(), 0);
    _imp->frameRange->setValue(curLast, ViewSpec::all(), 1);
    endChanges();
    // No value-changed handler runs while a project loads, and nodes hashed earlier in the load
    // saw the saved range.
    if (changed && isLoadingProject()) {
        rehashNodesUsingFrameRange();
    }
}

void
Project::recomputeFrameRangeFromReaders()
{
    int first = 1, last = 1;

    recomputeFrameRangeForAllReaders(&first, &last);

    const bool changed = (first != _imp->frameRange->getValue(0)) || (last != _imp->frameRange->getValue(1));
    beginChanges();
    _imp->frameRange->setValue(first, ViewSpec::all(), 0);
    _imp->frameRange->setValue(last,  ViewSpec::all(), 1);
    endChanges();
    // No value-changed handler runs while a project loads, and nodes hashed earlier in the load
    // saw the saved range.
    if (changed && isLoadingProject()) {
        rehashNodesUsingFrameRange();
    }
}

void
Project::rehashNodesUsingFrameRange()
{
    NodesList nodes;
    getNodes_recursive(nodes, false);

    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!(*it)->isNodeCreated() || !(*it)->dependsOnProjectFrameRange()) {
            continue;
        }
        (*it)->markHashDirty();
    }
}

void
Project::createViewer()
{
    if ( getApp()->isBackground() ) {
        return;
    }

    CreateNodeArgs args( PLUGINID_NATRON_VIEWER, shared_from_this() );
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    getApp()->createNode(args);
}

static bool
hasNodeOutputsInList(const NodesList& nodes,
                     const NodePtr& node)
{
    const NodesWList& outputs = node->getGuiOutputs();
    bool foundOutput = false;

    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (*it != node) {
            for (NodesWList::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
                if (it2->lock() == *it) {
                    foundOutput = true;
                    break;
                }
            }
            if (foundOutput) {
                break;
            }
        }
    }

    return foundOutput;
}

static bool
hasNodeInputsInList(const NodesList& nodes,
                    const NodePtr& node)
{
    const std::vector<NodeWPtr>& inputs = node->getGuiInputs();
    bool foundInput = false;

    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (*it != node) {
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                NodePtr input = inputs[i].lock();
                if (input == *it) {
                    foundInput = true;
                    break;
                }
            }
            if (foundInput) {
                break;
            }
        }
    }

    return foundInput;
}

static void
addTreeInputs(const NodesList& nodes,
              const NodePtr& node,
              Project::NodesTree& tree,
              NodesList& markedNodes)
{
    if ( std::find(markedNodes.begin(), markedNodes.end(), node) != markedNodes.end() ) {
        return;
    }

    if ( std::find(nodes.begin(), nodes.end(), node) == nodes.end() ) {
        return;
    }

    if ( !hasNodeInputsInList(nodes, node) ) {
        Project::TreeInput input;
        input.node = node;
        const std::vector<NodeWPtr>& inputs = node->getGuiInputs();
        input.inputs.resize( inputs.size() );
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            input.inputs[i] = inputs[i].lock();
        }
        tree.inputs.push_back(input);
        markedNodes.push_back(node);
    } else {
        tree.inbetweenNodes.push_back(node);
        markedNodes.push_back(node);
        const std::vector<NodeWPtr>& inputs = node->getGuiInputs();
        for (std::vector<NodeWPtr>::const_iterator it2 = inputs.begin(); it2 != inputs.end(); ++it2) {
            NodePtr input = it2->lock();
            if (input) {
                addTreeInputs(nodes, input, tree, markedNodes);
            }
        }
    }
}

void
Project::extractTreesFromNodes(const NodesList& nodes,
                               std::list<Project::NodesTree>& trees)
{
    NodesList markedNodes;

    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        bool isOutput = !hasNodeOutputsInList(nodes, *it);
        if (isOutput) {
            NodesTree tree;
            tree.output.node = *it;
            const NodesWList& outputs = (*it)->getGuiOutputs();
            for (NodesWList::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
                NodePtr output = it2->lock();
                if (!output) {
                    continue;
                }
                int idx = output->inputIndex(*it);
                tree.output.outputs.push_back( std::make_pair(idx, *it2) );
            }

            const std::vector<NodeWPtr>& inputs = (*it)->getGuiInputs();
            for (U32 i = 0; i < inputs.size(); ++i) {
                NodePtr input = inputs[i].lock();
                if (input) {
                    addTreeInputs(nodes, input, tree, markedNodes);
                }
            }

            if ( tree.inputs.empty() ) {
                TreeInput input;
                input.node = *it;

                const std::vector<NodeWPtr>& inputs = (*it)->getGuiInputs();
                input.inputs.resize( inputs.size() );
                for (std::size_t i = 0; i < inputs.size(); ++i) {
                    input.inputs[i] = inputs[i].lock();
                }
                tree.inputs.push_back(input);
            }

            trees.push_back(tree);
        }
    }
}

bool
Project::addFormat(const std::string& formatSpec)
{
    Format f;

    if ( ProjectPrivate::generateFormatFromString(QString::fromUtf8( formatSpec.c_str() ), &f) ) {
        QMutexLocker k(&_imp->formatMutex);
        bool existed;
        tryAddProjectFormat(f, &existed);

        return true;
    } else {
        return false;
    }
}

void
Project::onProjectFormatPopulated()
{
    int index = _imp->formatKnob->getValue();
    NodesList nodes;

    getNodes_recursive(nodes, true);
    std::vector<ChoiceOption> entries = _imp->formatKnob->getEntries_mt_safe();
    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        (*it)->refreshFormatParamChoice(entries, index, false);
    }
}

void
Project::setTimeLine(const TimeLinePtr& timeline)
{
    _imp->timeline = timeline;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_Project.cpp"
