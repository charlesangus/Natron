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

#ifndef Gui_AiModelsPage_h
#define Gui_AiModelsPage_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <string>
#include <vector>

#include <QFrame>
#include <QString>

#include "Gui/GuiFwd.h"

class QButtonGroup;
class QTableWidget;

NATRON_NAMESPACE_ENTER

class ModelDownloader;

/**
 * @brief The Preferences page that lists the AI models of the manifest, downloads and deletes
 * them, and picks the default model of each task.
 **/
class AiModelsPage
    : public QFrame {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit AiModelsPage(QWidget* parent = nullptr);
    ~AiModelsPage() override;

    // Re-reads install states and the default models, which Discard may have reverted.
    void refresh();

protected:
    void showEvent(QShowEvent* e) override;

private Q_SLOTS:
    void onDownloadClicked();
    void onDeleteClicked();
    void onSelectionChanged();
    void onProgress(const QString& id, qint64 received, qint64 total);
    void onFinished(const QString& id, bool ok, const QString& error);
    void onRemoved(const QString& id);

private:
    void buildRows();
    void refreshRow(int row);
    void refreshButtons();
    std::string selectedId() const;
    int rowOf(const std::string& id) const;
    void showMessage(const QString& text, bool isError);

    ModelDownloader* _downloader = nullptr;
    QTableWidget* _table = nullptr;
    Button* _downloadButton = nullptr;
    Button* _deleteButton = nullptr;
    Label* _message = nullptr;
    std::vector<std::string> _ids;
    std::map<int, QButtonGroup*> _defaultGroups;
    // Percent downloaded of the models downloading now, -1 while the size is unknown.
    std::map<std::string, int> _progress;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_AiModelsPage_h
