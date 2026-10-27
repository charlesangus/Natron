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

#ifndef Engine_Ai_ModelDownloader_h
#define Engine_Ai_ModelDownloader_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <memory>
#include <string>

#include <QObject>
#include <QString>

#include "Engine/Ai/ModelRegistry.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Downloads and installs the manifest's models. A download streams the package into
 * <models>/.tmp-<package>/, checks its size and sha256 against the manifest, unpacks it, checks
 * every model of the package against its config.json, and only then renames it into
 * <models>/<package>/. Any failure deletes the temporary folder and leaves the installed
 * models as they were. A failed or cancelled download starts over from the beginning.
 *
 * Models sharing a package share its download: asking for a model whose package is already
 * downloading joins that download, and the signals are emitted for every model of the package.
 *
 * Lives on a thread with an event loop. Unpacking runs on the global thread pool.
 **/
class ModelDownloader
    : public QObject {
    Q_OBJECT

public:
    explicit ModelDownloader(std::shared_ptr<const ModelRegistry> registry, QObject* parent = nullptr);
    ~ModelDownloader() override;

    /**
     * @brief The downloader over ModelRegistry::process(), owned by the application. Created by
     * the first call, which must come from the main thread.
     **/
    static ModelDownloader* process();

    const std::shared_ptr<const ModelRegistry>& registry() const
    {
        return _registry;
    }

    /**
     * @brief Starts downloading the package of model id, or joins its download if one is running.
     * Returns false, setting error and emitting nothing, when the id is unknown or the temporary
     * folder cannot be made; otherwise finished() follows.
     **/
    bool start(const std::string& id, std::string* error = nullptr);

    /**
     * @brief Cancels the download of id's package, for every model joined to it. Once unpacking
     * has begun the install still stops before anything is renamed into place.
     **/
    void cancel(const std::string& id);

    bool isDownloading(const std::string& id) const;

    /**
     * @brief Deletes the installed package of model id, and so every model of that package, and
     * drops the cached inference sessions so none keeps a deleted model open. Refused, setting
     * error, while the package is downloading or when the folder cannot be deleted. Emits
     * removed() for every model of the package.
     **/
    bool remove(const std::string& id, std::string* error = nullptr);

    /**
     * @brief start(), then waits for the result on a local event loop. For the command line
     * and tests.
     **/
    bool download(const std::string& id, std::string* error = nullptr);

Q_SIGNALS:
    // total is -1 while the size is unknown.
    void progress(const QString& id, qint64 received, qint64 total);
    void finished(const QString& id, bool ok, const QString& error);
    void removed(const QString& id);

private:
    struct Job;

    Job* findJob(const std::string& package) const;
    void onDataReady(const std::string& package);
    void onDownloaded(const std::string& package);
    void finishJob(const std::string& package, const std::string& error);

    std::shared_ptr<const ModelRegistry> _registry;
    std::map<std::string, std::unique_ptr<Job>> _jobs;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_ModelDownloader_h
