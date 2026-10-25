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

#include "ProjectGui.h"

#include <algorithm> // find
#include <fstream>
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTimer>
#include <QDebug>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Backdrop.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/PyParameter.h" // Param
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/Settings.h"
#include "Engine/Utils.h" // convertFromPlainText
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Gui/BackdropGui.h"
#include "Gui/Button.h"
#include "Gui/ComboBox.h"
#include "Gui/CurveEditor.h"
#include "Gui/DialogButtonBox.h"
#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/Histogram.h"
#include "Gui/Label.h"
#include "Gui/LineEdit.h"
#include "Gui/MultiInstancePanel.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/PythonPanels.h"
#include "Gui/RegisteredTabs.h"
#include "Gui/ScriptEditor.h"
#include "Gui/SpinBox.h"
#include "Gui/Splitter.h"
#include "Gui/TabWidget.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

ProjectGui::ProjectGui(Gui* gui)
    : _gui(gui)
    , _project()
    , _panel(NULL)
    , _created(false)
    , _colorPickersEnabled()
{
}

ProjectGui::~ProjectGui()
{
}

void
ProjectGui::initializeKnobsGui()
{
    assert(_panel);
    _panel->initializeKnobs();
}

void
ProjectGui::create(ProjectPtr projectInternal,
                   QVBoxLayout* container,
                   QWidget* parent)

{
    _project = projectInternal;

    QObject::connect( projectInternal.get(), SIGNAL(mustCreateFormat()), this, SLOT(createNewFormat()) );
    QObject::connect( projectInternal.get(), SIGNAL(knobsInitialized()), this, SLOT(initializeKnobsGui()) );

    _panel = new DockablePanel(_gui,
                               projectInternal.get(),
                               container,
                               DockablePanel::eHeaderModeReadOnlyName,
                               false,
                               projectInternal.get(),
                               tr("Project Settings"),
                               tr("The settings of the current project."),
                               parent);

    _created = true;
}

bool
ProjectGui::isVisible() const
{
    return _panel->isVisible();
}

void
ProjectGui::setVisible(bool visible)
{
    _panel->setClosed(!visible);
}

void
ProjectGui::createNewFormat()
{
    ProjectPtr project = _project.lock();
    AddFormatDialog dialog( project.get(), _gui->getApp()->getGui() );

    if ( dialog.exec() ) {
        project->setOrAddProjectFormat( dialog.getFormat() );
    }
}

AddFormatDialog::AddFormatDialog(Project *project,
                                 Gui* gui)
    : QDialog(gui)
    , _gui(gui)
{
    _mainLayout = new QVBoxLayout(this);
    _mainLayout->setSpacing(0);
    _mainLayout->setContentsMargins(5, 5, 0, 0);
    setLayout(_mainLayout);
    setWindowTitle( tr("New Format") );

    _fromViewerLine = new QWidget(this);
    _fromViewerLineLayout = new QHBoxLayout(_fromViewerLine);
    _fromViewerLine->setLayout(_fromViewerLineLayout);

    _copyFromViewerCombo = new ComboBox(_fromViewerLine);

    project->getViewers(&_viewers);


    for (std::list<ViewerInstance*>::iterator it = _viewers.begin(); it != _viewers.end(); ++it) {
        _copyFromViewerCombo->addItem( QString::fromUtf8( (*it)->getNode()->getLabel().c_str() ) );
    }
    _fromViewerLineLayout->addWidget(_copyFromViewerCombo);

    _copyFromViewerButton = new Button(tr("Copy from"), _fromViewerLine);
    _copyFromViewerButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(
                                           tr("Fill the new format with the currently"
                                              " displayed region of definition of the viewer"
                                              " indicated on the left."), NATRON_NAMESPACE::WhiteSpaceNormal) );
    QObject::connect( _copyFromViewerButton, SIGNAL(clicked()), this, SLOT(onCopyFromViewer()) );
    _mainLayout->addWidget(_fromViewerLine);

    _fromViewerLineLayout->addWidget(_copyFromViewerButton);
    _parametersLine = new QWidget(this);
    _parametersLineLayout = new QHBoxLayout(_parametersLine);
    _mainLayout->addWidget(_parametersLine);

    _widthLabel = new Label(QString::fromUtf8("w:"), _parametersLine);
    _parametersLineLayout->addWidget(_widthLabel);
    _widthSpinBox = new SpinBox(this, SpinBox::eSpinBoxTypeInt);
    _widthSpinBox->setMaximum(99999);
    _widthSpinBox->setMinimum(1);
    _widthSpinBox->setValue(1);
    _parametersLineLayout->addWidget(_widthSpinBox);


    _heightLabel = new Label(QString::fromUtf8("h:"), _parametersLine);
    _parametersLineLayout->addWidget(_heightLabel);
    _heightSpinBox = new SpinBox(this, SpinBox::eSpinBoxTypeInt);
    _heightSpinBox->setMaximum(99999);
    _heightSpinBox->setMinimum(1);
    _heightSpinBox->setValue(1);
    _parametersLineLayout->addWidget(_heightSpinBox);


    _pixelAspectLabel = new Label(tr("pixel aspect:"), _parametersLine);
    _parametersLineLayout->addWidget(_pixelAspectLabel);
    _pixelAspectSpinBox = new SpinBox(this, SpinBox::eSpinBoxTypeDouble);
    _pixelAspectSpinBox->setMinimum(0.);
    _pixelAspectSpinBox->setValue(1.);
    _parametersLineLayout->addWidget(_pixelAspectSpinBox);


    _formatNameLine = new QWidget(this);
    _formatNameLayout = new QHBoxLayout(_formatNameLine);
    _formatNameLine->setLayout(_formatNameLayout);
    _mainLayout->addWidget(_formatNameLine);


    _nameLabel = new Label(tr("Name:"), _formatNameLine);
    _formatNameLayout->addWidget(_nameLabel);
    _nameLineEdit = new LineEdit(_formatNameLine);
    _formatNameLayout->addWidget(_nameLineEdit);

    _buttonBox = new DialogButtonBox(QDialogButtonBox::StandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel), Qt::Horizontal, this);
    QObject::connect( _buttonBox, SIGNAL(rejected()), this, SLOT(reject()) );
    QObject::connect( _buttonBox, SIGNAL(accepted()), this, SLOT(accept()) );
    _mainLayout->addWidget(_buttonBox);
}

void
AddFormatDialog::onCopyFromViewer()
{
    QString activeText = _copyFromViewerCombo->itemText( _copyFromViewerCombo->activeIndex() );

    for (std::list<ViewerInstance*>::iterator it = _viewers.begin(); it != _viewers.end(); ++it) {
        if ( (*it)->getNode()->getLabel() == activeText.toStdString() ) {
            ViewerTab* tab = _gui->getViewerTabForInstance(*it);
            RectD f = tab->getViewer()->getRoD(0);
            RectD canonicalFormat = tab->getViewer()->getCanonicalFormat(0);
            double par = tab->getViewer()->getPAR(0);
            const RectI format = canonicalFormat.toPixelEnclosing(0, par);
            _widthSpinBox->setValue( format.width() );
            _heightSpinBox->setValue( format.height() );
            _pixelAspectSpinBox->setValue(par);
        }
    }
}

Format
AddFormatDialog::getFormat() const
{
    int w = (int)_widthSpinBox->value();
    int h = (int)_heightSpinBox->value();
    double pa = _pixelAspectSpinBox->value();
    QString name = _nameLineEdit->text();

    return Format(0, 0, w, h, name.toStdString(), pa);
}

NodesGuiList
ProjectGui::getVisibleNodes() const
{
    return _gui->getVisibleNodes_mt_safe();
}

void
ProjectGui::clearColorPickers()
{
    while ( !_colorPickersEnabled.empty() ) {
        _colorPickersEnabled.front()->setPickingEnabled(false);
    }

    _colorPickersEnabled.clear();
}

void
ProjectGui::registerNewColorPicker(KnobColorPtr knob)
{
    clearColorPickers();
    _colorPickersEnabled.push_back(knob);
}

void
ProjectGui::removeColorPicker(KnobColorPtr knob)
{
    std::vector<KnobColorPtr>::iterator found = std::find(_colorPickersEnabled.begin(), _colorPickersEnabled.end(), knob);

    if ( found != _colorPickersEnabled.end() ) {
        _colorPickersEnabled.erase(found);
    }
}

void
ProjectGui::setPickersColor(double r,
                            double g,
                            double b,
                            double a)
{
    if ( _colorPickersEnabled.empty() ) {
        return;
    }
    KnobColorPtr first = _colorPickersEnabled.front();

    for (U32 i = 0; i < _colorPickersEnabled.size(); ++i) {
        if ( !_colorPickersEnabled[i]->areAllDimensionsEnabled() ) {
            _colorPickersEnabled[i]->activateAllDimensions();
        }
        if (_colorPickersEnabled[i]->getDimension() == 3) {
            _colorPickersEnabled[i]->setValues(r, g, b, ViewSpec::all(), eValueChangedReasonNatronGuiEdited);
        } else {
            _colorPickersEnabled[i]->setValues(r, g, b, a, ViewSpec::all(), eValueChangedReasonNatronGuiEdited);
        }
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ProjectGui.cpp"
