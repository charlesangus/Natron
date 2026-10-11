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

#include "RenderDialog.h"

#include <limits>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include <ofxImageEffect.h>

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"

#include "Gui/ComboBox.h"
#include "Gui/DialogButtonBox.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/Label.h"
#include "Gui/SpinBox.h"

NATRON_NAMESPACE_ENTER

RenderRange
resolveRenderRange(RenderRangePresetEnum preset,
                   int writerFirst,
                   int writerLast,
                   int writerStep,
                   int projectFirst,
                   int projectLast,
                   const RenderRangeCustom& custom)
{
    RenderRange range;

    switch (preset) {
    case eRenderRangePresetCustom:
        range.first = custom.first;
        range.last = custom.last;
        range.step = custom.step;
        break;
    case eRenderRangePresetProjectRange:
        range.first = projectFirst;
        range.last = projectLast;
        range.step = writerStep;
        break;
    case eRenderRangePresetWriteNodeRange:
    default:
        if ((writerFirst == std::numeric_limits<int>::min()) || (writerLast == std::numeric_limits<int>::max())) {
            range.first = projectFirst;
            range.last = projectLast;
        } else {
            range.first = writerFirst;
            range.last = writerLast;
        }
        range.step = writerStep;
        break;
    }

    range.error = (range.first > range.last) || (range.step < 1);

    return range;
}

namespace {
struct WriterRowInfo {
    NodePtr node;
    OutputEffectInstance* effect;
    int writerFirst;
    int writerLast;
    int writerStep;
};

// Remembered across dialog instances within the running process; "session" means
// this, not anything persisted to disk.
struct RenderDialogSessionState {
    bool initialized;
    RenderRangePresetEnum preset;
    RenderRangeCustom custom;
    bool background;

    RenderDialogSessionState()
        : initialized(false)
        , preset(eRenderRangePresetWriteNodeRange)
        , custom()
        , background(false)
    {
    }
};

RenderDialogSessionState sessionState;

QString
writerOutputFileName(const NodePtr& node)
{
    KnobIPtr fileKnob = node->getKnobByName(kOfxImageEffectFileParamName);
    KnobOutputFile* outputFileKnob = dynamic_cast<KnobOutputFile*>(fileKnob.get());

    if (!outputFileKnob) {
        return QString();
    }

    return QString::fromUtf8(outputFileKnob->getValue().c_str());
}
} // anonymous namespace

struct RenderDialogPrivate {
    Gui* gui;
    std::vector<WriterRowInfo> writers;
    int projectFirst;
    int projectLast;

    QTreeWidget* writersTree;
    ComboBox* presetCombo;
    SpinBox* firstSpin;
    SpinBox* lastSpin;
    SpinBox* stepSpin;
    Label* errorLabel;
    QCheckBox* backgroundCheck;
    DialogButtonBox* buttons;

    RenderDialogPrivate()
        : gui(0)
        , writers()
        , projectFirst(1)
        , projectLast(1)
        , writersTree(0)
        , presetCombo(0)
        , firstSpin(0)
        , lastSpin(0)
        , stepSpin(0)
        , errorLabel(0)
        , backgroundCheck(0)
        , buttons(0)
    {
    }
};

RenderDialog::RenderDialog(Gui* gui,
                           const std::list<NodePtr>& writers)
    : QDialog(gui)
    , _imp(new RenderDialogPrivate())
{
    _imp->gui = gui;

    setWindowTitle(tr("Render"));

    for (std::list<NodePtr>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
        const NodePtr& node = *it;
        if (!node) {
            continue;
        }
        EffectInstancePtr effect = node->getEffectInstance();
        OutputEffectInstance* outputEffect = effect ? dynamic_cast<OutputEffectInstance*>(effect.get()) : 0;
        if (!outputEffect) {
            continue;
        }

        WriterRowInfo info;
        info.node = node;
        info.effect = outputEffect;

        double firstD, lastD;
        outputEffect->getFrameRange_public(outputEffect->getHash(), &firstD, &lastD, true);
        if ((firstD == std::numeric_limits<int>::min()) || (lastD == std::numeric_limits<int>::max())) {
            info.writerFirst = std::numeric_limits<int>::min();
            info.writerLast = std::numeric_limits<int>::max();
        } else {
            info.writerFirst = (int)firstD;
            info.writerLast = (int)lastD;
        }
        info.writerStep = node->getFrameStepKnobValue();

        _imp->writers.push_back(info);
    }

    double projectFirstD = 1.;
    double projectLastD = 1.;
    if (gui && gui->getApp() && gui->getApp()->getProject()) {
        gui->getApp()->getProject()->getFrameRange(&projectFirstD, &projectLastD);
    }
    _imp->projectFirst = (int)projectFirstD;
    _imp->projectLast = (int)projectLastD;

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    _imp->writersTree = new QTreeWidget(this);
    _imp->writersTree->setColumnCount(3);
    QStringList headers;
    headers << tr("Writer") << tr("Output File") << tr("Write Range");
    _imp->writersTree->setHeaderLabels(headers);
    _imp->writersTree->setRootIsDecorated(false);
    _imp->writersTree->setSelectionMode(QAbstractItemView::NoSelection);
    _imp->writersTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (std::vector<WriterRowInfo>::const_iterator it = _imp->writers.begin(); it != _imp->writers.end(); ++it) {
        RenderRange writerRange = resolveRenderRange(eRenderRangePresetWriteNodeRange,
                                                     it->writerFirst, it->writerLast, it->writerStep,
                                                     _imp->projectFirst, _imp->projectLast,
                                                     RenderRangeCustom());
        QString rangeText = writerRange.error
            ? tr("invalid")
            : QString::fromUtf8("%1-%2").arg(writerRange.first).arg(writerRange.last);
        QTreeWidgetItem* item = new QTreeWidgetItem(_imp->writersTree);
        item->setText(0, QString::fromUtf8(it->node->getLabel_mt_safe().c_str()));
        item->setText(1, writerOutputFileName(it->node));
        item->setText(2, rangeText);
    }
    mainLayout->addWidget(_imp->writersTree);

    QHBoxLayout* presetLayout = new QHBoxLayout();
    Label* presetLabel = new Label(tr("Frame range:"), this);
    _imp->presetCombo = new ComboBox(this);
    _imp->presetCombo->addItem(tr("Write node range"));
    _imp->presetCombo->addItem(tr("Project range"));
    _imp->presetCombo->addItem(tr("Custom"));
    presetLayout->addWidget(presetLabel);
    presetLayout->addWidget(_imp->presetCombo);
    mainLayout->addLayout(presetLayout);

    QHBoxLayout* rangeLayout = new QHBoxLayout();
    Label* firstLabel = new Label(tr("First:"), this);
    _imp->firstSpin = new SpinBox(this, SpinBox::eSpinBoxTypeInt);
    Label* lastLabel = new Label(tr("Last:"), this);
    _imp->lastSpin = new SpinBox(this, SpinBox::eSpinBoxTypeInt);
    Label* stepLabel = new Label(tr("Step:"), this);
    _imp->stepSpin = new SpinBox(this, SpinBox::eSpinBoxTypeInt);
    _imp->stepSpin->setMinimum(0);
    rangeLayout->addWidget(firstLabel);
    rangeLayout->addWidget(_imp->firstSpin);
    rangeLayout->addWidget(lastLabel);
    rangeLayout->addWidget(_imp->lastSpin);
    rangeLayout->addWidget(stepLabel);
    rangeLayout->addWidget(_imp->stepSpin);
    mainLayout->addLayout(rangeLayout);

    _imp->errorLabel = new Label(this);
    _imp->errorLabel->setStyleSheet(QString::fromUtf8("color: red;"));
    mainLayout->addWidget(_imp->errorLabel);

    _imp->backgroundCheck = new QCheckBox(tr("Render in background (Progress panel)"), this);
    mainLayout->addWidget(_imp->backgroundCheck);

    _imp->buttons = new DialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    mainLayout->addWidget(_imp->buttons);

    QObject::connect(_imp->presetCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(onPresetChanged(int)));
    QObject::connect(_imp->firstSpin, SIGNAL(valueChanged(double)), this, SLOT(onRangeValueChanged(double)));
    QObject::connect(_imp->lastSpin, SIGNAL(valueChanged(double)), this, SLOT(onRangeValueChanged(double)));
    QObject::connect(_imp->stepSpin, SIGNAL(valueChanged(double)), this, SLOT(onRangeValueChanged(double)));
    QObject::connect(_imp->buttons, SIGNAL(accepted()), this, SLOT(onOkClicked()));
    QObject::connect(_imp->buttons, SIGNAL(rejected()), this, SLOT(reject()));

    if (!sessionState.initialized && !_imp->writers.empty()) {
        const WriterRowInfo& firstWriter = _imp->writers.front();
        RenderRange resolved = resolveRenderRange(eRenderRangePresetWriteNodeRange,
                                                  firstWriter.writerFirst, firstWriter.writerLast, firstWriter.writerStep,
                                                  _imp->projectFirst, _imp->projectLast,
                                                  RenderRangeCustom());
        sessionState.custom.first = resolved.first;
        sessionState.custom.last = resolved.last;
        sessionState.custom.step = firstWriter.writerStep;
        sessionState.initialized = true;
    }

    _imp->presetCombo->setCurrentIndex((int)sessionState.preset);
    _imp->firstSpin->setValue(sessionState.custom.first);
    _imp->lastSpin->setValue(sessionState.custom.last);
    _imp->stepSpin->setValue(sessionState.custom.step);
    _imp->backgroundCheck->setChecked(sessionState.background);

    refreshCustomFieldsEnabled();
    refreshValidation();
} // RenderDialog::RenderDialog

RenderDialog::~RenderDialog()
{
}

void
RenderDialog::refreshCustomFieldsEnabled()
{
    bool isCustom = (_imp->presetCombo->activeIndex() == (int)eRenderRangePresetCustom);

    _imp->firstSpin->setEnabled(isCustom);
    _imp->lastSpin->setEnabled(isCustom);
    _imp->stepSpin->setEnabled(isCustom);
}

void
RenderDialog::refreshValidation()
{
    RenderRangePresetEnum preset = (RenderRangePresetEnum)_imp->presetCombo->activeIndex();
    RenderRangeCustom custom;

    custom.first = (int)_imp->firstSpin->value();
    custom.last = (int)_imp->lastSpin->value();
    custom.step = (int)_imp->stepSpin->value();

    bool hasError = _imp->writers.empty();
    QString message = hasError ? tr("There is no writer to render.") : QString();

    for (std::vector<WriterRowInfo>::const_iterator it = _imp->writers.begin(); it != _imp->writers.end(); ++it) {
        RenderRange range = resolveRenderRange(preset, it->writerFirst, it->writerLast, it->writerStep,
                                               _imp->projectFirst, _imp->projectLast, custom);
        if (range.error) {
            hasError = true;
            if (message.isEmpty()) {
                message = (range.step < 1)
                    ? tr("Frame step must be at least 1.")
                    : tr("First frame is greater than the last frame.");
            }
        }
    }

    _imp->errorLabel->setText(message);
    _imp->errorLabel->setVisible(!message.isEmpty());

    QAbstractButton* okButton = _imp->buttons->button(QDialogButtonBox::Ok);
    if (okButton) {
        okButton->setEnabled(!hasError);
    }
}

void
RenderDialog::onPresetChanged(int /*index*/)
{
    refreshCustomFieldsEnabled();
    refreshValidation();
}

void
RenderDialog::onRangeValueChanged(double /*value*/)
{
    refreshValidation();
}

void
RenderDialog::onOkClicked()
{
    sessionState.preset = (RenderRangePresetEnum)_imp->presetCombo->activeIndex();
    sessionState.custom.first = (int)_imp->firstSpin->value();
    sessionState.custom.last = (int)_imp->lastSpin->value();
    sessionState.custom.step = (int)_imp->stepSpin->value();
    sessionState.background = _imp->backgroundCheck->isChecked();

    accept();
}

std::list<AppInstance::RenderWork>
RenderDialog::getRenderWorks() const
{
    std::list<AppInstance::RenderWork> works;
    RenderRangePresetEnum preset = (RenderRangePresetEnum)_imp->presetCombo->activeIndex();
    RenderRangeCustom custom;

    custom.first = (int)_imp->firstSpin->value();
    custom.last = (int)_imp->lastSpin->value();
    custom.step = (int)_imp->stepSpin->value();

    bool useStats = (_imp->gui && _imp->gui->getApp()) ? _imp->gui->getApp()->isRenderStatsActionChecked() : false;

    for (std::vector<WriterRowInfo>::const_iterator it = _imp->writers.begin(); it != _imp->writers.end(); ++it) {
        RenderRange range = resolveRenderRange(preset, it->writerFirst, it->writerLast, it->writerStep,
                                               _imp->projectFirst, _imp->projectLast, custom);
        if (range.error) {
            continue;
        }
        works.push_back(AppInstance::RenderWork(it->effect, range.first, range.last, range.step, useStats));
    }

    return works;
}

bool
RenderDialog::isBackgroundChecked() const
{
    return _imp->backgroundCheck->isChecked();
}

void
showRenderDialog(Gui* gui,
                 const std::list<NodePtr>& writers)
{
    if (!gui || writers.empty()) {
        return;
    }

    RenderDialog dialog(gui, writers);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    std::list<AppInstance::RenderWork> works = dialog.getRenderWorks();
    if (works.empty() || !gui->getApp()) {
        return;
    }

    gui->getApp()->startWritersRendering(false, works);
}

NATRON_NAMESPACE_EXIT
