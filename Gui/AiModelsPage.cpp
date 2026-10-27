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

#include "AiModelsPage.h"

#include <exception>

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QRadioButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelKnob.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/AppManager.h"
#include "Engine/Settings.h"

#include "Gui/Button.h"
#include "Gui/Gui.h"
#include "Gui/Label.h"

NATRON_NAMESPACE_ENTER

namespace {

enum Column {
    eColumnName = 0,
    eColumnTask,
    eColumnSize,
    eColumnLicence,
    eColumnStatus,
    eColumnDefault,
    eColumnCount
};

QString
taskLabel(AiModelTask task)
{
    switch (task) {
    case AiModelTask::eMaskObject:
        return AiModelsPage::tr("Mask Object");
    case AiModelTask::eDenoise:
        return AiModelsPage::tr("Denoise");
    case AiModelTask::eDenoiseRawBayer:
        return AiModelsPage::tr("Raw Denoise (Bayer)");
    case AiModelTask::eDenoiseLinear:
        return AiModelsPage::tr("Raw Denoise (Linear)");
    case AiModelTask::eUpscale2x:
        return AiModelsPage::tr("Upscale 2x");
    case AiModelTask::eUpscale4x:
        return AiModelsPage::tr("Upscale 4x");
    }

    return QString();
}

// Chip colours by darktable-ai's OSAID class, open-source first.
QColor
osaidColour(const std::string& osaid)
{
    if (osaid == "Open Source AI") {
        return QColor(0x3f, 0x9a, 0x48);
    } else if (osaid == "Open Weights") {
        return QColor(0xc0, 0x90, 0x20);
    }

    return QColor(0x80, 0x80, 0x80);
}

QIcon
osaidChip(const std::string& osaid,
          int size)
{
    QPixmap pixmap(size, size);

    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(osaidColour(osaid));
    painter.drawEllipse(QRectF(1, 1, size - 2, size - 2));

    return QIcon(pixmap);
}

QString
licenceToolTip(const AiModelEntry& entry)
{
    QString text = AiModelsPage::tr("Weights licence: %1\nTraining-data licence: %2\nOSAID class: %3")
                       .arg(QString::fromStdString(entry.weightsLicence), QString::fromStdString(entry.trainingDataLicence),
                            QString::fromStdString(entry.osaid));
    if (!entry.cardUrl.empty()) {
        text += QString::fromUtf8("\n") + AiModelsPage::tr("Model card: %1").arg(QString::fromStdString(entry.cardUrl));
    }

    return text;
}

QTableWidgetItem*
readOnlyItem(const QString& text)
{
    QTableWidgetItem* item = new QTableWidgetItem(text);

    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    item->setToolTip(text);

    return item;
}

} // namespace

AiModelsPage::AiModelsPage(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QString::fromUtf8("aiModelsPage"));
    setFrameShadow(QFrame::Sunken);
    setFrameShape(QFrame::Box);

    QVBoxLayout* layout = new QVBoxLayout(this);

    Label* intro = new Label(tr("Models the AI nodes run. Downloads go to your user data folder; "
                                "Default picks the model new nodes start with. The dot by a name is its "
                                "OSAID class: green Open Source AI, amber Open Weights. Hover a row for its licences."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    _table = new QTableWidget(this);
    _table->setObjectName(QString::fromUtf8("aiModelsTable"));
    _table->setColumnCount(eColumnCount);
    _table->setHorizontalHeaderLabels(QStringList() << tr("Name") << tr("Task") << tr("Size") << tr("Licence")
                                                    << tr("Status") << tr("Default"));
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _table->setWordWrap(false);
    _table->setTextElideMode(Qt::ElideRight);
    const int chipSize = _table->fontMetrics().height() * 2 / 3;
    _table->setIconSize(QSize(chipSize, chipSize));
    _table->verticalHeader()->hide();
    _table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    _table->verticalHeader()->setDefaultSectionSize(_table->fontMetrics().height() + 8);
    QHeaderView* header = _table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setTextElideMode(Qt::ElideRight);
    for (int c = 0; c < eColumnCount; ++c) {
        header->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    }
    header->setSectionResizeMode(eColumnName, QHeaderView::Stretch);
    // Licence names vary in length: a fixed width keeps the row on screen, and the tooltip has the full text.
    header->setSectionResizeMode(eColumnLicence, QHeaderView::Fixed);
    header->resizeSection(eColumnLicence, _table->fontMetrics().horizontalAdvance(QString::fromUtf8("Apache-2.0")) + 16);
    layout->addWidget(_table, 1);

    _message = new Label(this);
    _message->setObjectName(QString::fromUtf8("aiModelsMessage"));
    _message->setWordWrap(true);
    _message->setTextFormat(Qt::RichText);
    _message->hide();
    layout->addWidget(_message);

    QHBoxLayout* buttons = new QHBoxLayout();
    _downloadButton = new Button(tr("Download"), this);
    _downloadButton->setObjectName(QString::fromUtf8("aiModelsDownload"));
    _downloadButton->setToolTip(tr("Downloads the selected model after showing its licences, or cancels its download."));
    _deleteButton = new Button(tr("Delete"), this);
    _deleteButton->setObjectName(QString::fromUtf8("aiModelsDelete"));
    _deleteButton->setToolTip(tr("Deletes the selected model, and the models that share its download."));
    buttons->addWidget(_downloadButton);
    buttons->addWidget(_deleteButton);
    buttons->addStretch();
    layout->addLayout(buttons);

    try {
        _downloader = ModelDownloader::process();
    } catch (const std::exception& e) {
        showMessage(tr("The model list cannot be read: %1").arg(QString::fromUtf8(e.what())), true);
    }

    QObject::connect(_downloadButton, &QPushButton::clicked, this, &AiModelsPage::onDownloadClicked);
    QObject::connect(_deleteButton, &QPushButton::clicked, this, &AiModelsPage::onDeleteClicked);
    QObject::connect(_table, &QTableWidget::itemSelectionChanged, this, &AiModelsPage::onSelectionChanged);
    if (_downloader) {
        QObject::connect(_downloader, &ModelDownloader::progress, this, &AiModelsPage::onProgress);
        QObject::connect(_downloader, &ModelDownloader::finished, this, &AiModelsPage::onFinished);
        QObject::connect(_downloader, &ModelDownloader::removed, this, &AiModelsPage::onRemoved);
        buildRows();
    }
    refreshButtons();
}

AiModelsPage::~AiModelsPage() = default;

void
AiModelsPage::buildRows()
{
    const ModelRegistry& registry = *_downloader->registry();
    const std::vector<AiModelEntry>& models = registry.models();

    _table->setRowCount((int)models.size());
    _ids.clear();
    for (std::size_t i = 0; i < models.size(); ++i) {
        const AiModelEntry& entry = models[i];
        const int row = (int)i;
        _ids.push_back(entry.id);

        const QString licences = licenceToolTip(entry);
        QTableWidgetItem* name = readOnlyItem(QString::fromStdString(entry.name));
        name->setIcon(osaidChip(entry.osaid, _table->iconSize().width()));
        name->setToolTip(QString::fromStdString(entry.name + " (" + entry.id + ")") + QString::fromUtf8("\n") + licences);
        _table->setItem(row, eColumnName, name);
        _table->setItem(row, eColumnTask, readOnlyItem(taskLabel(entry.task)));
        _table->setItem(row, eColumnSize, readOnlyItem(QLocale().formattedDataSize((qint64)entry.size, 0)));
        QTableWidgetItem* licence = readOnlyItem(QString::fromStdString(entry.weightsLicence));
        licence->setToolTip(licences);
        _table->setItem(row, eColumnLicence, licence);

        _table->setItem(row, eColumnStatus, readOnlyItem(QString()));

        _table->setItem(row, eColumnDefault, readOnlyItem(QString()));
        QWidget* cell = new QWidget(_table);
        QHBoxLayout* cellLayout = new QHBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        QRadioButton* radio = new QRadioButton(cell);
        radio->setObjectName(QString::fromStdString("aiModelDefault_" + entry.id));
        radio->setToolTip(tr("New %1 nodes start with this model.").arg(taskLabel(entry.task)));
        cellLayout->addWidget(radio, 0, Qt::AlignCenter);
        _table->setCellWidget(row, eColumnDefault, cell);

        QButtonGroup*& group = _defaultGroups[(int)entry.task];
        if (!group) {
            group = new QButtonGroup(this);
            group->setExclusive(true);
        }
        group->addButton(radio, row);
        const std::string id = entry.id;
        const AiModelTask task = entry.task;
        QObject::connect(radio, &QRadioButton::toggled, this, [task, id](bool checked) {
            if (checked) {
                appPTR->getCurrentSettings()->setAiDefaultModel(task, id);
            }
        });
    }
    refresh();
}

void
AiModelsPage::refresh()
{
    if (!_downloader) {
        return;
    }
    for (int row = 0; row < (int)_ids.size(); ++row) {
        refreshRow(row);
    }
    const ModelRegistry& registry = *_downloader->registry();
    for (const auto& [taskIndex, group] : _defaultGroups) {
        const AiModelEntry* picked = ModelKnob::defaultModel(registry, (AiModelTask)taskIndex);
        const int row = picked ? rowOf(picked->id) : -1;
        QAbstractButton* button = group->button(row);
        if (button && !button->isChecked()) {
            // Showing the stored default is not a choice the user made.
            button->blockSignals(true);
            button->setChecked(true);
            button->blockSignals(false);
        }
    }
    refreshButtons();
}

void
AiModelsPage::showEvent(QShowEvent* e)
{
    refresh();
    QFrame::showEvent(e);
}

void
AiModelsPage::refreshRow(int row)
{
    const std::string& id = _ids[row];
    QString status;
    const auto downloading = _progress.find(id);

    if (downloading != _progress.end()) {
        status = (downloading->second < 0) ? tr("Downloading") : tr("%1%").arg(downloading->second);
    } else if (_downloader->registry()->resolve(id).status == AiModelStatus::eInstalled) {
        status = tr("Installed");
    } else {
        status = tr("Missing");
    }
    QTableWidgetItem* item = _table->item(row, eColumnStatus);
    item->setText(status);
    item->setToolTip(status);
}

void
AiModelsPage::refreshButtons()
{
    const std::string id = selectedId();

    if (!_downloader || id.empty()) {
        _downloadButton->setText(tr("Download"));
        _downloadButton->setEnabled(false);
        _deleteButton->setEnabled(false);

        return;
    }
    const bool downloading = _downloader->isDownloading(id);
    const bool installed = (_downloader->registry()->resolve(id).status == AiModelStatus::eInstalled);
    _downloadButton->setText(downloading ? tr("Cancel Download") : tr("Download"));
    _downloadButton->setEnabled(downloading || !installed);
    _deleteButton->setEnabled(installed && !downloading);
}

std::string
AiModelsPage::selectedId() const
{
    const QList<QTableWidgetItem*> selection = _table->selectedItems();

    if (selection.isEmpty()) {
        return std::string();
    }
    const int row = selection.front()->row();

    return ((row >= 0) && (row < (int)_ids.size())) ? _ids[row] : std::string();
}

int
AiModelsPage::rowOf(const std::string& id) const
{
    for (int row = 0; row < (int)_ids.size(); ++row) {
        if (_ids[row] == id) {
            return row;
        }
    }

    return -1;
}

void
AiModelsPage::showMessage(const QString& text,
                          bool isError)
{
    if (text.isEmpty()) {
        _message->hide();

        return;
    }
    _message->setText(isError ? QString::fromUtf8("<span style=\"color:#e06060;\">%1</span>").arg(text.toHtmlEscaped()) : text.toHtmlEscaped());
    _message->show();
}

void
AiModelsPage::onSelectionChanged()
{
    refreshButtons();
}

void
AiModelsPage::onDownloadClicked()
{
    const std::string id = selectedId();
    const AiModelEntry* entry = _downloader ? _downloader->registry()->find(id) : nullptr;

    if (!entry) {
        return;
    }
    if (_downloader->isDownloading(id)) {
        _downloader->cancel(id);

        return;
    }

    QMessageBox confirm(QMessageBox::Question, tr("Download %1").arg(QString::fromStdString(entry->name)),
                        QString::fromStdString(ModelKnob::licenceNotice(*entry)), QMessageBox::Yes | QMessageBox::No, this);
    confirm.setObjectName(QString::fromUtf8("aiModelLicenceNotice"));
    confirm.setTextFormat(Qt::RichText);
    Gui::setQMessageBoxAppropriateFont(&confirm);
    Gui::setQMessageBoxThemedIcon(&confirm);
    confirm.setDefaultButton(QMessageBox::No);
    if ((confirm.exec() != QMessageBox::Yes)) {
        return;
    }

    std::string error;
    showMessage(QString(), false);
    if (!_downloader->start(id, &error)) {
        showMessage(QString::fromStdString(error), true);

        return;
    }
    for (const AiModelEntry* model : _downloader->registry()->packageModels(*entry)) {
        _progress[model->id] = -1;
        const int row = rowOf(model->id);
        if (row >= 0) {
            refreshRow(row);
        }
    }
    refreshButtons();
}

void
AiModelsPage::onDeleteClicked()
{
    const std::string id = selectedId();
    const AiModelEntry* entry = _downloader ? _downloader->registry()->find(id) : nullptr;

    if (!entry) {
        return;
    }
    QStringList names;
    for (const AiModelEntry* model : _downloader->registry()->packageModels(*entry)) {
        names << QString::fromStdString(model->name);
    }
    QString text = tr("Delete %1?").arg(names.join(QString::fromUtf8(", ")));
    if (names.size() > 1) {
        text += QString::fromUtf8("\n") + tr("These models share one download, so they are deleted together.");
    }
    QMessageBox confirm(QMessageBox::Question, tr("Delete Model"), text, QMessageBox::Yes | QMessageBox::No, this);
    confirm.setObjectName(QString::fromUtf8("aiModelDeleteConfirm"));
    Gui::setQMessageBoxAppropriateFont(&confirm);
    Gui::setQMessageBoxThemedIcon(&confirm);
    confirm.setDefaultButton(QMessageBox::No);
    if (confirm.exec() != QMessageBox::Yes) {
        return;
    }

    std::string error;
    showMessage(QString(), false);
    if (!_downloader->remove(id, &error)) {
        showMessage(QString::fromStdString(error), true);
    }
    refresh();
}

void
AiModelsPage::onProgress(const QString& id,
                         qint64 received,
                         qint64 total)
{
    const std::string modelId = id.toStdString();

    _progress[modelId] = (total > 0) ? (int)((received * 100) / total) : -1;
    const int row = rowOf(modelId);
    if (row >= 0) {
        refreshRow(row);
    }
}

void
AiModelsPage::onFinished(const QString& id,
                         bool ok,
                         const QString& error)
{
    const std::string modelId = id.toStdString();

    _progress.erase(modelId);
    if (!ok) {
        showMessage(error, true);
    }
    const int row = rowOf(modelId);
    if (row >= 0) {
        refreshRow(row);
    }
    refreshButtons();
}

void
AiModelsPage::onRemoved(const QString& id)
{
    const int row = rowOf(id.toStdString());

    if (row >= 0) {
        refreshRow(row);
    }
    refreshButtons();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_AiModelsPage.cpp"
