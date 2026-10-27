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

#include "NodeSettingsPanel.h"

#include <exception>
#include <fstream>
#include <istream>
#include <iterator>
#include <list>
#include <stdexcept>
#include <string>
#include <vector>

#include <QStyle>

#include "Global/FStreamsSupport.h"

#include "Engine/EffectInstance.h"
#include "Engine/Knob.h" // KnobHolder
#include "Engine/Node.h"
#include "Engine/RotoLayer.h"
#include "Engine/Utils.h" // convertFromPlainText

#include "Gui/Button.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h" // appPTR
#include "Gui/GuiDefines.h"
#include "Gui/Menu.h"
#include "Gui/MultiInstancePanel.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/TrackerPanel.h"
#include "Gui/RotoPanel.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/Yaml/PresetYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

using std::make_pair;
NATRON_NAMESPACE_ENTER

NodeSettingsPanel::NodeSettingsPanel(const MultiInstancePanelPtr& multiPanel,
                                     Gui* gui,
                                     const NodeGuiPtr& NodeUi,
                                     QVBoxLayout* container,
                                     QWidget* parent)
    : DockablePanel(gui,
                    multiPanel.get() != NULL ? dynamic_cast<KnobHolder*>(multiPanel.get()) : NodeUi->getNode()->getEffectInstance().get(),
                    container,
                    DockablePanel::eHeaderModeFullyFeatured,
                    false,
                    NodeUi->getNode()->getEffectInstance().get(),
                    QString::fromUtf8(NodeUi->getNode()->getLabel().c_str()),
                    QString::fromUtf8(NodeUi->getNode()->getPluginDescription().c_str()),
                    parent)
    , _nodeGUI(NodeUi)
    , _selected(false)
    , _settingsButton(0)
    , _multiPanel(multiPanel)
{
    if (multiPanel) {
        multiPanel->initializeKnobsPublic();
    }


    QObject::connect( this, SIGNAL(closeChanged(bool)), NodeUi.get(), SLOT(onSettingsPanelClosedChanged(bool)) );
    const QSize mediumBSize( TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
    const QSize mediumIconSize( TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE) );
    QIcon pixSettings;
    appPTR->getIcon(NATRON_PIXMAP_SETTINGS, TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), &pixSettings);
    _settingsButton = new Button(pixSettings, QString(), getHeaderWidget());
    _settingsButton->setFixedSize(mediumBSize);
    _settingsButton->setIconSize(mediumIconSize);
    _settingsButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Settings and presets."), NATRON_NAMESPACE::WhiteSpaceNormal) );
    _settingsButton->setFocusPolicy(Qt::NoFocus);
    QObject::connect( _settingsButton, SIGNAL(clicked()), this, SLOT(onSettingsButtonClicked()) );
    insertHeaderWidget(1, _settingsButton);
}

NodeSettingsPanel::~NodeSettingsPanel()
{
    NodeGuiPtr node = getNode();

    if (node) {
        node->removeSettingsPanel();
    }
}

void
NodeSettingsPanel::setSelected(bool s)
{
    if (s != _selected) {
        _selected = s;
        style()->unpolish(this);
        style()->polish(this);
    }
}

void
NodeSettingsPanel::setPyPlugUIEnabled(bool enabled)
{
    DockablePanel::setPyPlugUIEnabled(enabled);
    _settingsButton->setEnabled(enabled);
}

void
NodeSettingsPanel::centerOnItem()
{
    getNode()->centerGraphOnIt();
}

RotoPanel*
NodeSettingsPanel::initializeRotoPanel()
{
    if (getNode()->getNode()->getCurveContext()) {
        return new RotoPanel(_nodeGUI.lock(), this);
    } else {
        return NULL;
    }
}

TrackerPanel*
NodeSettingsPanel::initializeTrackerPanel()
{
    if ( getNode()->getNode()->getEffectInstance()->isBuiltinTrackerNode() ) {
        return new TrackerPanel(_nodeGUI.lock(), this);
    } else {
        return NULL;
    }
}

QColor
NodeSettingsPanel::getCurrentColor() const
{
    NodeGuiPtr node = getNode();
    QColor color;

    if (!node->getUserColor(&color)) {
        color = node->getCurrentColor();
    }

    return color;
}

void
NodeSettingsPanel::initializeExtraGui(QVBoxLayout* layout)
{
    if ( _multiPanel && !_multiPanel->isGuiCreated() ) {
        _multiPanel->createMultiInstanceGui(layout);
    }
}

void
NodeSettingsPanel::onSettingsButtonClicked()
{
    Menu menu(this);
    //menu.setFont(QFont(appFont,appFontSize));
    NodeGuiPtr node = getNode();
    QAction* importPresets = new QAction(tr("Import presets"), &menu);
    QObject::connect( importPresets, SIGNAL(triggered()), this, SLOT(onImportPresetsActionTriggered()) );
    QAction* exportAsPresets = new QAction(tr("Export as presets"), &menu);
    QObject::connect( exportAsPresets, SIGNAL(triggered()), this, SLOT(onExportPresetsActionTriggered()) );

    menu.addAction(importPresets);
    menu.addAction(exportAsPresets);
    menu.addSeparator();

    QAction* manageUserParams = new QAction(tr("Manage user parameters..."), &menu);
    QObject::connect( manageUserParams, SIGNAL(triggered()), this, SLOT(onManageUserParametersActionTriggered()) );
    manageUserParams->setEnabled(node->getNode()->getEffectInstance()->getIsEditable());
    menu.addAction(manageUserParams);

    menu.addSeparator();


    QAction* setKeyOnAll = new QAction(tr("Set key on all parameters"), &menu);
    QObject::connect( setKeyOnAll, SIGNAL(triggered()), this, SLOT(setKeyOnAllParameters()) );
    QAction* removeAnimationOnAll = new QAction(tr("Remove animation on all parameters"), &menu);
    QObject::connect( removeAnimationOnAll, SIGNAL(triggered()), this, SLOT(removeAnimationOnAllParameters()) );
    menu.addAction(setKeyOnAll);
    menu.addAction(removeAnimationOnAll);

    if (!node->getNode()->getEffectInstance()->getIsEditable()) {
        importPresets->setEnabled(false);
        setKeyOnAll->setEnabled(false);
        removeAnimationOnAll->setEnabled(false);
    }

    if (!node->getDagGui() || !node->getDagGui()->getGui() || node->getDagGui()->getGui()->isGUIFrozen()) {
        importPresets->setEnabled(false);
        exportAsPresets->setEnabled(false);
        setKeyOnAll->setEnabled(false);
        removeAnimationOnAll->setEnabled(false);
    }

    menu.exec( _settingsButton->mapToGlobal( _settingsButton->pos() ) );
}

void
NodeSettingsPanel::onImportPresetsActionTriggered()
{
    if (!isHolderEditable()) {
        return;
    }
    std::vector<std::string> filters;

    filters.push_back(NATRON_PRESETS_FILE_EXT);
    std::string filename = getGui()->popOpenFileDialog(false, filters, getGui()->getLastLoadProjectDirectory().toStdString(), false);
    if ( filename.empty() ) {
        return;
    }


    FStreamsSupport::ifstream ifile;
    FStreamsSupport::open(&ifile, filename);
    if (!ifile) {
        Dialogs::errorDialog( tr("Presets").toStdString(), tr("Failed to open file: ").toStdString() + filename, false );

        return;
    }
    const std::string text((std::istreambuf_iterator<char>(ifile)), std::istreambuf_iterator<char>());

    if (Serialization::isXmlDocument(text)) {
        Dialogs::errorDialog(tr("Presets").toStdString(), tr("This preset uses the old XML .nps format, which this version of Natron cannot open (no compatibility, by design).").toStdString(), false);

        return;
    }

    std::list<Serialization::NodeSerializationPtr> nodeSerialization;
    try {
        nodeSerialization = Serialization::decodePresetYaml(text);
    } catch (const std::exception& e) {
        Dialogs::errorDialog(tr("Presets").toStdString(), tr("Cannot read the preset %1: %2").arg(QString::fromUtf8(filename.c_str())).arg(QString::fromUtf8(e.what())).toStdString(), false);

        return;
    }

    NodeGuiPtr node = getNode();
    if (nodeSerialization.front()->pluginID != node->getNode()->getPluginID()) {
        QString err = tr("You cannot load %1 which are presets for the plug-in %2 on the plug-in %3.")
                          .arg(QString::fromUtf8(filename.c_str()))
                          .arg(QString::fromUtf8(nodeSerialization.front()->pluginID.c_str()))
                          .arg(QString::fromUtf8(node->getNode()->getPluginID().c_str()));
        Dialogs::errorDialog(tr("Presets").toStdString(), err.toStdString());

        return;
    }

    node->restoreInternal(node, nodeSerialization);
}

static bool
endsWith(const std::string &str,
         const std::string &suffix)
{
    return ( ( str.size() >= suffix.size() ) &&
             (str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0) );
}

void
NodeSettingsPanel::onExportPresetsActionTriggered()
{
    std::vector<std::string> filters;

    filters.push_back(NATRON_PRESETS_FILE_EXT);
    std::string filename = getGui()->popSaveFileDialog(false, filters, getGui()->getLastSaveProjectDirectory().toStdString(), false);
    if ( filename.empty() ) {
        return;
    }

    if ( !endsWith(filename, "." NATRON_PRESETS_FILE_EXT) ) {
        filename.append("." NATRON_PRESETS_FILE_EXT);
    }

    NodeGuiPtr node = getNode();
    std::list<Serialization::NodeSerializationPtr> nodeSerialization;
    node->serializeInternal(nodeSerialization);
    std::string text;
    try {
        text = Serialization::encodePresetYaml(nodeSerialization);
    } catch (const std::exception& e) {
        Dialogs::errorDialog(tr("Presets").toStdString(), e.what());

        return;
    }

    FStreamsSupport::ofstream ofile;
    FStreamsSupport::open(&ofile, filename);
    if (!ofile) {
        Dialogs::errorDialog( tr("Presets").toStdString(),
                              tr("Failed to open file %1.").arg( QString::fromUtf8( filename.c_str() ) ).toStdString(), false );

        return;
    }
    ofile << text;
    ofile.close();
    if (ofile.fail()) {
        Dialogs::errorDialog(tr("Presets").toStdString(),
                             tr("Failed to write file %1.").arg(QString::fromUtf8(filename.c_str())).toStdString(), false);
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_NodeSettingsPanel.cpp"
