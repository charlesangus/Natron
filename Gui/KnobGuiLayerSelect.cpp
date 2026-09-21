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

#include "KnobGuiLayerSelect.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Gui/Gui.h"
#include "Gui/LayerChannelRow.h"
#include "Gui/NewLayerDialog.h"

NATRON_NAMESPACE_ENTER

struct KnobGuiLayerSelectPrivate {
    KnobLayerSelectWPtr knob;
    LayerChannelRow* row;

    KnobGuiLayerSelectPrivate()
        : knob()
        , row(0)
    {
    }
};

KnobGuiLayerSelect::KnobGuiLayerSelect(KnobIPtr knob,
                                       KnobGuiContainerI* container)
    : KnobGuiLayerChannelBase(knob, container)
    , _imp(new KnobGuiLayerSelectPrivate())
{
    _imp->knob = std::dynamic_pointer_cast<KnobLayerSelect>(knob);
}

KnobGuiLayerSelect::~KnobGuiLayerSelect()
{
}

void
KnobGuiLayerSelect::removeSpecificGui()
{
    KnobGuiLayerChannelBase::removeSpecificGui();
    _imp->row = 0;
}

LayerChannelRow*
KnobGuiLayerSelect::getRow() const
{
    return _imp->row;
}

void
KnobGuiLayerSelect::createWidget(QHBoxLayout* layout)
{
    createContainer(layout);

    _imp->row = new LayerChannelRow(LayerChannelRow::eModeLayerSelect, getContainer());
    _imp->row->setAbsentMarker(getAbsentMarkerText());
    QObject::connect(_imp->row, &LayerChannelRow::layerChosen, this, [this](const QString& id) {
        onLayerChosen(id);
    });
    QObject::connect(_imp->row, &LayerChannelRow::channelToggled, this, [this](const QString&, bool) {
        onChannelToggled();
    });
    QObject::connect(_imp->row, &LayerChannelRow::newLayerRequested, this, [this]() {
        onNewLayerRequested();
    });
    getContainerLayout()->addWidget(_imp->row);

    refresh(true);
}

void
KnobGuiLayerSelect::refreshWidgets()
{
    KnobLayerSelectPtr knob = _imp->knob.lock();

    if (!knob || !_imp->row) {
        return;
    }
    const std::vector<LayerChannelRow::LayerEntry>& layers = getLayers();
    if (!sameLayerEntries(_imp->row->getAvailableLayers(), layers)) {
        _imp->row->setAvailableLayers(layers, isTargetKnob());
    }

    const std::string layerID = knob->getLayer();
    std::vector<std::string> enabled = knob->getChannels();
    if (enabled.empty()) {
        const LayerChannelRow::LayerEntry* entry = findLayer(layerID);
        if (entry) {
            enabled = entry->channels;
        }
    }
    if (_imp->row->getCurrentLayerID() != layerID || _imp->row->getEnabledChannels() != enabled) {
        _imp->row->setLayerSelectValue(layerID, enabled, knob->getWithChannelButtons());
    }
}

void
KnobGuiLayerSelect::onLayerChosen(const QString& layerID)
{
    KnobLayerSelectPtr knob = _imp->knob.lock();

    if (knob) {
        pushValue(knob->encode(layerID.toStdString(), std::vector<std::string>()));
    }
}

void
KnobGuiLayerSelect::onChannelToggled()
{
    KnobLayerSelectPtr knob = _imp->knob.lock();

    if (!knob || !_imp->row) {
        return;
    }
    const std::vector<std::string> enabled = _imp->row->getEnabledChannels();
    // An empty channel list is the knob's spelling of "every channel", so the selection
    // cannot hold zero channels: the last button springs back.
    if (enabled.empty()) {
        scheduleRefresh(false);

        return;
    }
    pushValue(knob->encode(knob->getLayer(), enabled));
}

void
KnobGuiLayerSelect::onNewLayerRequested()
{
    // The row is still inside its combo's own change signal; the registry add below
    // repopulates that combo, so the dialog waits for the event loop.
    QTimer::singleShot(0, this, [this]() {
        openNewLayerDialog();
    });
}

void
KnobGuiLayerSelect::openNewLayerDialog()
{
    KnobLayerSelectPtr knob = _imp->knob.lock();
    NodePtr node = getNode();
    AppInstancePtr app = node ? node->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (!knob || !project) {
        return;
    }
    NewLayerDialog dialog(ImageLayerDesc::getNoneComponents(), getGui());
    if (!dialog.exec()) {
        return;
    }

    const ImageLayerDesc desc = dialog.getComponents();
    std::string error;
    LayerRegistry::AddResultEnum ret = project->addLayer(desc, LayerRegistryEntry::eOriginUser, &error);
    if (ret == LayerRegistry::eAddResultRefused) {
        Dialogs::errorDialog(tr("Layer").toStdString(), error);

        return;
    }
    pushValue(knob->encode(desc.getLayerID(), std::vector<std::string>()));
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiLayerSelect.cpp"
