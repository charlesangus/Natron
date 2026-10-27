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

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include <ofxImageEffect.h>

#include "Engine/AppManager.h" // TO_DPIX, TO_DPIY
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
                   const RenderRangeCustom& custom,
                   bool separateProcess)
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

    if (range.step < 1) {
        range.errorKind = eRenderRangeErrorStepBelowOne;
    } else if (range.first > range.last) {
        range.errorKind = eRenderRangeErrorFirstAfterLast;
    } else if (separateProcess && (range.first < 0)) {
        range.errorKind = eRenderRangeErrorNegativeFirstInSeparateProcess;
    } else {
        range.errorKind = eRenderRangeErrorNone;
    }
    range.error = (range.errorKind != eRenderRangeErrorNone);

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
    mainLayout->setSpacing(TO_DPIY(12));

    // The output path is shown only when it fits on the same line; a long path would otherwise
    // force the dialog as wide as the longest file name.
    QGroupBox* writersGroup = new QGroupBox(tr("Writers"), this);
    QVBoxLayout* writersGroupLayout = new QVBoxLayout(writersGroup);

    QWidget* writersListWidget = new QWidget(writersGroup);
    QVBoxLayout* writersListLayout = new QVBoxLayout(writersListWidget);
    writersListLayout->setContentsMargins(0, 0, 0, 0);
    writersListLayout->setSpacing(TO_DPIY(4));

    const int kMaxInlineLineChars = 80;
    for (std::vector<WriterRowInfo>::const_iterator it = _imp->writers.begin(); it != _imp->writers.end(); ++it) {
        QString name = QString::fromUtf8(it->node->getLabel_mt_safe().c_str());
        QString path = writerOutputFileName(it->node);

        QString text = name.toHtmlEscaped();
        if (!path.isEmpty() && ((name.size() + path.size()) <= kMaxInlineLineChars)) {
            text += QString::fromUtf8("&nbsp;&nbsp;<span style=\"color:#8c8c8c;\">%1</span>").arg(path.toHtmlEscaped());
        }

        Label* writerLabel = new Label(writersListWidget);
        writerLabel->setTextFormat(Qt::RichText);
        writerLabel->setText(text);
        writersListLayout->addWidget(writerLabel);
    }
    writersListLayout->addStretch();

    QScrollArea* writersScroll = new QScrollArea(writersGroup);
    writersScroll->setFrameShape(QFrame::NoFrame);
    writersScroll->setWidgetResizable(true);
    writersScroll->setWidget(writersListWidget);
    writersScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    writersScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // Cap the list at a handful of rows so a project with many writers scrolls instead of
    // growing the dialog past the screen.
    const int maxVisibleRows = 6;
    const int rowCount = std::max((int)_imp->writers.size(), 1);
    writersScroll->setMaximumHeight(TO_DPIY(22) * std::min(rowCount, maxVisibleRows) + TO_DPIY(8));
    writersGroupLayout->addWidget(writersScroll);
    mainLayout->addWidget(writersGroup);

    QGroupBox* rangeGroup = new QGroupBox(tr("Frame Range"), this);
    QVBoxLayout* rangeGroupLayout = new QVBoxLayout(rangeGroup);
    rangeGroupLayout->setSpacing(TO_DPIY(8));

    QHBoxLayout* presetLayout = new QHBoxLayout();
    Label* presetLabel = new Label(tr("Range:"), rangeGroup);
    _imp->presetCombo = new ComboBox(rangeGroup);
    _imp->presetCombo->addItem(tr("Write node range"));
    _imp->presetCombo->addItem(tr("Project range"));
    _imp->presetCombo->addItem(tr("Custom"));
    presetLayout->addWidget(presetLabel);
    presetLayout->addWidget(_imp->presetCombo);
    presetLayout->addStretch();
    rangeGroupLayout->addLayout(presetLayout);

    QHBoxLayout* rangeLayout = new QHBoxLayout();
    Label* firstLabel = new Label(tr("First:"), rangeGroup);
    _imp->firstSpin = new SpinBox(rangeGroup, SpinBox::eSpinBoxTypeInt);
    Label* lastLabel = new Label(tr("Last:"), rangeGroup);
    _imp->lastSpin = new SpinBox(rangeGroup, SpinBox::eSpinBoxTypeInt);
    Label* stepLabel = new Label(tr("Step:"), rangeGroup);
    _imp->stepSpin = new SpinBox(rangeGroup, SpinBox::eSpinBoxTypeInt);
    _imp->stepSpin->setMinimum(0);
    rangeLayout->addWidget(firstLabel);
    rangeLayout->addWidget(_imp->firstSpin);
    rangeLayout->addSpacing(TO_DPIX(12));
    rangeLayout->addWidget(lastLabel);
    rangeLayout->addWidget(_imp->lastSpin);
    rangeLayout->addSpacing(TO_DPIX(12));
    rangeLayout->addWidget(stepLabel);
    rangeLayout->addWidget(_imp->stepSpin);
    rangeLayout->addStretch();
    rangeGroupLayout->addLayout(rangeLayout);

    _imp->errorLabel = new Label(rangeGroup);
    _imp->errorLabel->setStyleSheet(QString::fromUtf8("color: red;"));
    rangeGroupLayout->addWidget(_imp->errorLabel);

    mainLayout->addWidget(rangeGroup);

    QGroupBox* modeGroup = new QGroupBox(tr("Mode"), this);
    QVBoxLayout* modeGroupLayout = new QVBoxLayout(modeGroup);
    _imp->backgroundCheck = new QCheckBox(tr("Render in background (Progress panel)"), modeGroup);
    modeGroupLayout->addWidget(_imp->backgroundCheck);
    mainLayout->addWidget(modeGroup);

    _imp->buttons = new DialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    mainLayout->addWidget(_imp->buttons);

    setMinimumWidth(TO_DPIX(420));

    QObject::connect(_imp->presetCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(onPresetChanged(int)));
    QObject::connect(_imp->firstSpin, SIGNAL(valueChanged(double)), this, SLOT(onRangeValueChanged(double)));
    QObject::connect(_imp->lastSpin, SIGNAL(valueChanged(double)), this, SLOT(onRangeValueChanged(double)));
    QObject::connect(_imp->stepSpin, SIGNAL(valueChanged(double)), this, SLOT(onRangeValueChanged(double)));
    QObject::connect(_imp->backgroundCheck, SIGNAL(toggled(bool)), this, SLOT(onBackgroundToggled(bool)));
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
    const bool separateProcess = _imp->backgroundCheck->isChecked();

    for (std::vector<WriterRowInfo>::const_iterator it = _imp->writers.begin(); it != _imp->writers.end(); ++it) {
        RenderRange range = resolveRenderRange(preset, it->writerFirst, it->writerLast, it->writerStep,
                                               _imp->projectFirst, _imp->projectLast, custom, separateProcess);
        if (!range.error) {
            continue;
        }
        hasError = true;
        if (!message.isEmpty()) {
            continue;
        }
        switch (range.errorKind) {
        case eRenderRangeErrorStepBelowOne:
            message = tr("Frame step must be at least 1.");
            break;
        case eRenderRangeErrorFirstAfterLast:
            message = tr("First frame is greater than the last frame.");
            break;
        case eRenderRangeErrorNegativeFirstInSeparateProcess:
            message = tr("A background render runs in a separate process, which cannot start before frame 0. "
                         "Change the frame range, or render in the foreground.");
            break;
        case eRenderRangeErrorNone:
            break;
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
RenderDialog::onBackgroundToggled(bool /*checked*/)
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
    const bool separateProcess = _imp->backgroundCheck->isChecked();

    for (std::vector<WriterRowInfo>::const_iterator it = _imp->writers.begin(); it != _imp->writers.end(); ++it) {
        RenderRange range = resolveRenderRange(preset, it->writerFirst, it->writerLast, it->writerStep,
                                               _imp->projectFirst, _imp->projectLast, custom, separateProcess);
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

bool
showRenderDialog(Gui* gui,
                 const std::list<NodePtr>& writers)
{
    if (!gui || writers.empty()) {
        return false;
    }

    RenderDialog dialog(gui, writers);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    std::list<AppInstance::RenderWork> works = dialog.getRenderWorks();
    if (works.empty() || !gui->getApp()) {
        return true;
    }

    if (dialog.isBackgroundChecked()) {
        try {
            gui->getApp()->startWritersRendering(eRenderModeBackground, works);
        } catch (const std::exception& e) {
            Dialogs::errorDialog(RenderDialog::tr("Render").toStdString(), e.what());
        }
    } else {
        gui->getApp()->renderForeground(works);
    }

    return true;
}

NATRON_NAMESPACE_EXIT
