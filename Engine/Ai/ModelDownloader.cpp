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

#include "ModelDownloader.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QUrl>
#include <QtConcurrentRun>

#include "Engine/Ai/ModelPackage.h"
#include "Engine/FileDownloader.h"

NATRON_NAMESPACE_ENTER

namespace {

// What the install step needs, copied so the worker shares nothing with the downloader.
struct InstallRequest {
    std::string requestedId;
    std::string package;
    std::string sha256;
    std::uint64_t size = 0;
    // Each model of the package with its files.
    std::vector<std::pair<std::string, std::vector<std::string>>> models;
    std::filesystem::path modelsDirectory;
    std::filesystem::path temporary;
    std::filesystem::path archive;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

const char* const kCancelled = "download cancelled";

// Empty on success.
std::string
installPackage(const InstallRequest& request)
{
    try {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(request.archive, ec);
        if (ec || (size != request.size)) {
            return "model \"" + request.requestedId + "\": downloaded " + (ec ? std::string("nothing") : std::to_string(size) + " bytes") + ", the manifest expects " + std::to_string(request.size);
        }
        const std::string sha256 = sha256OfFile(request.archive.string());
        if (sha256 != request.sha256) {
            return "model \"" + request.requestedId + "\": sha256 mismatch, expected " + request.sha256 + ", got " + sha256;
        }
        if (*request.cancelled) {
            return "model \"" + request.requestedId + "\": " + kCancelled;
        }

        const std::filesystem::path unpacked = request.temporary / request.package;
        unpackModelPackage(request.archive.string(), request.package, unpacked.string());
        for (const auto& model : request.models) {
            for (const std::string& file : model.second) {
                if (!std::filesystem::is_regular_file(unpacked / file, ec)) {
                    return "model \"" + model.first + "\": the package has no " + file;
                }
            }
            readModelConfig(unpacked.string(), model.second.front());
        }
        if (*request.cancelled) {
            return "model \"" + request.requestedId + "\": " + kCancelled;
        }

        const std::filesystem::path installed = request.modelsDirectory / request.package;
        const std::filesystem::path previous = request.temporary / ".previous";
        const bool replacing = std::filesystem::exists(installed, ec);
        if (replacing) {
            std::filesystem::rename(installed, previous);
        }
        std::filesystem::rename(unpacked, installed, ec);
        if (ec) {
            if (replacing) {
                std::error_code restoreError;
                std::filesystem::rename(previous, installed, restoreError);
            }

            return "model \"" + request.requestedId + "\": cannot install into " + installed.string() + ": " + ec.message();
        }
    } catch (const std::exception& e) {
        return "model \"" + request.requestedId + "\": " + e.what();
    }

    return std::string();
}

} // anon namespace

struct ModelDownloader::Job {
    std::string requestedId;
    std::vector<std::string> ids;
    InstallRequest install;
    FileDownloader* downloader = nullptr;
    std::unique_ptr<QFile> file;
    std::string writeError;
};

ModelDownloader::ModelDownloader(std::shared_ptr<const ModelRegistry> registry,
                                 QObject* parent)
    : QObject(parent)
    , _registry(std::move(registry))
{
}

ModelDownloader::~ModelDownloader()
{
    for (auto& job : _jobs) {
        *job.second->install.cancelled = true;
        if (job.second->downloader) {
            QObject::disconnect(job.second->downloader, nullptr, this, nullptr);
            if (QNetworkReply* reply = job.second->downloader->getReply()) {
                QObject::disconnect(reply, nullptr, this, nullptr);
            }
            delete job.second->downloader;
        }
    }
}

ModelDownloader*
ModelDownloader::process()
{
    static ModelDownloader* downloader = new ModelDownloader(ModelRegistry::process(), QCoreApplication::instance());

    return downloader;
}

ModelDownloader::Job*
ModelDownloader::findJob(const std::string& package) const
{
    const auto it = _jobs.find(package);

    return (it == _jobs.end()) ? nullptr : it->second.get();
}

bool
ModelDownloader::start(const std::string& id,
                       std::string* error)
{
    const AiModelEntry* entry = _registry->find(id);
    if (!entry) {
        if (error) {
            *error = "unknown model \"" + id + "\"";
        }

        return false;
    }
    if (findJob(entry->package)) {
        return true;
    }

    auto job = std::make_unique<Job>();
    job->requestedId = id;
    InstallRequest& install = job->install;
    install.requestedId = id;
    install.package = entry->package;
    install.sha256 = entry->sha256;
    install.size = entry->size;
    for (const AiModelEntry* model : _registry->packageModels(*entry)) {
        job->ids.push_back(model->id);
        install.models.emplace_back(model->id, model->files);
    }
    install.modelsDirectory = _registry->modelsDirectory();
    install.temporary = install.modelsDirectory / (".tmp-" + entry->package);
    install.archive = install.temporary / (entry->package + ".dtmodel");
    install.cancelled = std::make_shared<std::atomic<bool>>(false);

    std::error_code ec;
    std::filesystem::remove_all(install.temporary, ec);
    std::filesystem::create_directories(install.temporary, ec);
    job->file = std::make_unique<QFile>(QString::fromStdString(install.archive.string()));
    if (ec || !job->file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = "model \"" + id + "\": cannot write " + install.archive.string() + ": " + (ec ? ec.message() : job->file->errorString().toStdString());
        }
        std::filesystem::remove_all(install.temporary, ec);

        return false;
    }

    const std::string package = entry->package;
    job->downloader = new FileDownloader(QUrl(QString::fromStdString(entry->url)), false);
    QNetworkReply* reply = job->downloader->getReply();
    const std::vector<std::string> ids = job->ids;
    QObject::connect(reply, &QNetworkReply::readyRead, this, [this, package] { onDataReady(package); });
    QObject::connect(reply, &QNetworkReply::downloadProgress, this, [this, ids](qint64 received, qint64 total) {
        for (const std::string& model : ids) {
            Q_EMIT progress(QString::fromStdString(model), received, total);
        }
    });
    QObject::connect(job->downloader, &FileDownloader::downloaded, this, [this, package] { onDownloaded(package); });
    _jobs.emplace(package, std::move(job));

    return true;
}

void
ModelDownloader::onDataReady(const std::string& package)
{
    Job* job = findJob(package);
    if (!job || !job->downloader || !job->file) {
        return;
    }
    QNetworkReply* reply = job->downloader->getReply();
    const QByteArray data = reply->readAll();
    if (job->writeError.empty() && (job->file->write(data) != data.size())) {
        job->writeError = "cannot write " + job->install.archive.string() + ": " + job->file->errorString().toStdString();
        reply->abort();
    }
}

void
ModelDownloader::onDownloaded(const std::string& package)
{
    Job* job = findJob(package);
    if (!job || !job->downloader) {
        return;
    }
    QNetworkReply* reply = job->downloader->getReply();
    const QByteArray rest = job->downloader->downloadedData();
    if (job->writeError.empty() && !rest.isEmpty() && (job->file->write(rest) != rest.size())) {
        job->writeError = "cannot write " + job->install.archive.string() + ": " + job->file->errorString().toStdString();
    }
    job->file->close();
    job->file.reset();
    const QNetworkReply::NetworkError networkError = reply->error();
    const std::string networkErrorString = reply->errorString().toStdString();
    job->downloader->deleteLater();
    job->downloader = nullptr;

    if (*job->install.cancelled) {
        finishJob(package, "model \"" + job->requestedId + "\": " + kCancelled);

        return;
    }
    if (!job->writeError.empty()) {
        finishJob(package, "model \"" + job->requestedId + "\": " + job->writeError);

        return;
    }
    if (networkError != QNetworkReply::NoError) {
        finishJob(package, "model \"" + job->requestedId + "\": download failed: " + networkErrorString);

        return;
    }

    auto* watcher = new QFutureWatcher<std::string>(this);
    QObject::connect(watcher, &QFutureWatcher<std::string>::finished, this, [this, watcher, package] {
        const std::string error = watcher->result();
        watcher->deleteLater();
        finishJob(package, error);
    });
    watcher->setFuture(QtConcurrent::run(installPackage, job->install));
}

void
ModelDownloader::finishJob(const std::string& package,
                           const std::string& error)
{
    const auto it = _jobs.find(package);
    if (it == _jobs.end()) {
        return;
    }
    std::unique_ptr<Job> job = std::move(it->second);
    _jobs.erase(it);

    std::error_code ec;
    std::filesystem::remove_all(job->install.temporary, ec);

    const QString message = QString::fromStdString(error);
    for (const std::string& id : job->ids) {
        Q_EMIT finished(QString::fromStdString(id), error.empty(), message);
    }
}

void
ModelDownloader::cancel(const std::string& id)
{
    const AiModelEntry* entry = _registry->find(id);
    Job* job = entry ? findJob(entry->package) : nullptr;
    if (!job) {
        return;
    }
    *job->install.cancelled = true;
    // abort() may finish the reply, and so the job, before it returns.
    if (job->downloader) {
        job->downloader->getReply()->abort();
    }
}

bool
ModelDownloader::isDownloading(const std::string& id) const
{
    const AiModelEntry* entry = _registry->find(id);

    return entry && findJob(entry->package);
}

bool
ModelDownloader::download(const std::string& id,
                          std::string* error)
{
    bool done = false;
    bool ok = false;
    QString message;
    QEventLoop loop;
    const QString qid = QString::fromStdString(id);
    const QMetaObject::Connection connection = QObject::connect(this, &ModelDownloader::finished, &loop,
                                                                [&](const QString& finishedId, bool finishedOk, const QString& finishedError) {
                                                                    if (finishedId == qid) {
                                                                        done = true;
                                                                        ok = finishedOk;
                                                                        message = finishedError;
                                                                        loop.quit();
                                                                    }
                                                                });
    if (!start(id, error)) {
        QObject::disconnect(connection);

        return false;
    }
    if (!done) {
        loop.exec();
    }
    QObject::disconnect(connection);
    if (!ok && error) {
        *error = message.toStdString();
    }

    return ok;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ModelDownloader.cpp"
