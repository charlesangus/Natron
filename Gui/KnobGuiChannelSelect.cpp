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

#include "KnobGuiChannelSelect.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/KnobChannelSelect.h"

#include "Gui/LayerChannelRow.h"

NATRON_NAMESPACE_ENTER

struct KnobGuiChannelSelectPrivate {
    KnobChannelSelectWPtr knob;
    LayerChannelRow* row;

    KnobGuiChannelSelectPrivate()
        : knob()
        , row(0)
    {
    }
};

KnobGuiChannelSelect::KnobGuiChannelSelect(KnobIPtr knob,
                                           KnobGuiContainerI* container)
    : KnobGuiLayerChannelBase(knob, container)
    , _imp(new KnobGuiChannelSelectPrivate())
{
    _imp->knob = std::dynamic_pointer_cast<KnobChannelSelect>(knob);
}

KnobGuiChannelSelect::~KnobGuiChannelSelect()
{
}

void
KnobGuiChannelSelect::removeSpecificGui()
{
    KnobGuiLayerChannelBase::removeSpecificGui();
    _imp->row = 0;
}

LayerChannelRow*
KnobGuiChannelSelect::getRow() const
{
    return _imp->row;
}

void
KnobGuiChannelSelect::createWidget(QHBoxLayout* layout)
{
    createContainer(layout);

    _imp->row = new LayerChannelRow(LayerChannelRow::eModeChannelSelect, getContainer());
    _imp->row->setAbsentMarker(getAbsentMarkerText());
    QObject::connect(_imp->row, &LayerChannelRow::channelSelected, this, [this](const QString& value) {
        onChannelSelected(value);
    });
    getContainerLayout()->addWidget(_imp->row);

    refresh(true);
}

void
KnobGuiChannelSelect::refreshWidgets()
{
    KnobChannelSelectPtr knob = _imp->knob.lock();

    if (!knob || !_imp->row) {
        return;
    }
    const std::vector<LayerChannelRow::LayerEntry>& layers = getLayers();
    if (!sameLayerEntries(_imp->row->getAvailableLayers(), layers)) {
        _imp->row->setAvailableLayers(layers, false);
    }
    const std::string value = knob->get();
    if (_imp->row->getCurrentChannel() != value) {
        _imp->row->setChannelSelectValue(value);
    }
}

void
KnobGuiChannelSelect::onChannelSelected(const QString& layerDotChannel)
{
    KnobChannelSelectPtr knob = _imp->knob.lock();

    if (knob) {
        pushValue(knob->encode(layerDotChannel.toStdString()));
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiChannelSelect.cpp"
