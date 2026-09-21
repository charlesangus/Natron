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

#include "KnobGuiLayerChannelBase.h"

#include <list>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_ENTER

bool
sameLayerEntries(const std::vector<LayerChannelRow::LayerEntry>& a,
                 const std::vector<LayerChannelRow::LayerEntry>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].label != b[i].label || a[i].channels != b[i].channels) {
            return false;
        }
    }

    return true;
}

struct KnobGuiLayerChannelBasePrivate {
    KnobIWPtr knob;
    QWidget* container;
    QVBoxLayout* layout;
    std::vector<LayerChannelRow::LayerEntry> layers;
    int userEditDepth;
    bool refreshPending;
    bool relistPending;

    KnobGuiLayerChannelBasePrivate()
        : knob()
        , container(0)
        , layout(0)
        , layers()
        , userEditDepth(0)
        , refreshPending(false)
        , relistPending(false)
    {
    }
};

KnobGuiLayerChannelBase::KnobGuiLayerChannelBase(KnobIPtr knob,
                                                 KnobGuiContainerI* container)
    : KnobGui(knob, container)
    , _imp(new KnobGuiLayerChannelBasePrivate())
{
    _imp->knob = knob;

    NodePtr node = getNode();
    if (node) {
        QObject::connect(node.get(), SIGNAL(layerListRefreshed()), this, SLOT(onLayerListRefreshed()));
        AppInstancePtr app = node->getApp();
        ProjectPtr project = app ? app->getProject() : ProjectPtr();
        if (project) {
            QObject::connect(project.get(), SIGNAL(projectLayersChanged()), this, SLOT(onLayerListRefreshed()));
        }
    }
}

KnobGuiLayerChannelBase::~KnobGuiLayerChannelBase()
{
}

KnobIPtr
KnobGuiLayerChannelBase::getKnob() const
{
    return _imp->knob.lock();
}

NodePtr
KnobGuiLayerChannelBase::getNode() const
{
    KnobIPtr k = _imp->knob.lock();
    EffectInstance* effect = k ? dynamic_cast<EffectInstance*>(k->getHolder()) : 0;

    return effect ? effect->getNode() : NodePtr();
}

bool
KnobGuiLayerChannelBase::isTargetKnob() const
{
    NodePtr node = getNode();

    return node && node->isTargetLayerKnob(_imp->knob.lock());
}

QString
KnobGuiLayerChannelBase::getAbsentMarkerText() const
{
    return isTargetKnob() ? tr("(not in project)") : tr("(not in input)");
}

void
KnobGuiLayerChannelBase::removeSpecificGui()
{
    if (_imp->container) {
        _imp->container->deleteLater();
    }
    _imp->container = 0;
    _imp->layout = 0;
}

void
KnobGuiLayerChannelBase::createContainer(QHBoxLayout* layout)
{
    _imp->container = new QWidget(layout->parentWidget());
    _imp->layout = new QVBoxLayout(_imp->container);
    _imp->layout->setContentsMargins(0, 0, 0, 0);
    _imp->layout->setSpacing(2);

    layout->parentWidget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    enableRightClickMenu(_imp->container, 0);
    layout->addWidget(_imp->container);
}

QWidget*
KnobGuiLayerChannelBase::getContainer() const
{
    return _imp->container;
}

QVBoxLayout*
KnobGuiLayerChannelBase::getContainerLayout() const
{
    return _imp->layout;
}

const std::vector<LayerChannelRow::LayerEntry>&
KnobGuiLayerChannelBase::getLayers() const
{
    return _imp->layers;
}

const LayerChannelRow::LayerEntry*
KnobGuiLayerChannelBase::findLayer(const std::string& id) const
{
    for (std::size_t i = 0; i < _imp->layers.size(); ++i) {
        if (_imp->layers[i].id == id) {
            return &_imp->layers[i];
        }
    }

    return 0;
}

void
KnobGuiLayerChannelBase::listLayers()
{
    _imp->layers.clear();
    KnobIPtr k = _imp->knob.lock();
    NodePtr node = getNode();
    if (!k || !node) {
        return;
    }
    std::list<ImageLayerDesc> descs;
    node->listLayersForKnob(k, &descs);
    descs.sort([](const ImageLayerDesc& a, const ImageLayerDesc& b) {
        return a.isColorLayer() && !b.isColorLayer();
    });
    for (std::list<ImageLayerDesc>::const_iterator it = descs.begin(); it != descs.end(); ++it) {
        LayerChannelRow::LayerEntry entry;
        entry.id = it->getLayerID();
        entry.label = it->getLayerLabel();
        entry.channels = it->getChannels();
        _imp->layers.push_back(entry);
    }
}

void
KnobGuiLayerChannelBase::pushValue(const std::string& newValue)
{
    KnobStringBasePtr knob = std::dynamic_pointer_cast<KnobStringBase>(_imp->knob.lock());

    if (!knob) {
        return;
    }
    const std::string oldValue = knob->getValue();
    if (oldValue == newValue) {
        return;
    }
    KnobUndoCommand<std::string>* cmd = new KnobUndoCommand<std::string>(shared_from_this(), oldValue, newValue);
    cmd->setMergeable(false);
    ++_imp->userEditDepth;
    pushUndoCommand(cmd);
    --_imp->userEditDepth;
}

void
KnobGuiLayerChannelBase::scheduleRefresh(bool relistLayers)
{
    _imp->relistPending = _imp->relistPending || relistLayers;
    if (_imp->refreshPending) {
        return;
    }
    _imp->refreshPending = true;
    QTimer::singleShot(0, this, SLOT(onDeferredRefresh()));
}

void
KnobGuiLayerChannelBase::refresh(bool relistLayers)
{
    if (!_imp->container || !_imp->knob.lock()) {
        return;
    }
    if (relistLayers) {
        listLayers();
    }
    refreshWidgets();
}

void
KnobGuiLayerChannelBase::onDeferredRefresh()
{
    const bool relist = _imp->relistPending;

    _imp->refreshPending = false;
    _imp->relistPending = false;
    refresh(relist);
}

void
KnobGuiLayerChannelBase::updateGUI(int /*dimension*/)
{
    // A row is still inside its own signal emission while the value it asked for is
    // applied; rebuilding it there would delete the emitting widget.
    if (_imp->userEditDepth > 0) {
        scheduleRefresh(false);

        return;
    }
    refresh(false);
}

void
KnobGuiLayerChannelBase::onLayerListRefreshed()
{
    if (_imp->userEditDepth > 0) {
        scheduleRefresh(true);

        return;
    }
    refresh(true);
}

void
KnobGuiLayerChannelBase::_hide()
{
    if (_imp->container) {
        _imp->container->hide();
    }
}

void
KnobGuiLayerChannelBase::_show()
{
    if (_imp->container) {
        _imp->container->show();
    }
}

void
KnobGuiLayerChannelBase::setEnabled()
{
    if (_imp->container) {
        _imp->container->setEnabled(getKnob()->isEnabled(0));
    }
}

void
KnobGuiLayerChannelBase::setReadOnly(bool readOnly,
                                     int /*dimension*/)
{
    if (_imp->container) {
        _imp->container->setEnabled(!readOnly);
    }
}

void
KnobGuiLayerChannelBase::setDirty(bool /*dirty*/)
{
}

void
KnobGuiLayerChannelBase::reflectAnimationLevel(int /*dimension*/,
                                               AnimationLevelEnum /*level*/)
{
}

void
KnobGuiLayerChannelBase::reflectExpressionState(int /*dimension*/,
                                                bool /*hasExpr*/)
{
}

void
KnobGuiLayerChannelBase::updateToolTip()
{
    if (_imp->container && hasToolTip()) {
        _imp->container->setToolTip(toolTip());
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiLayerChannelBase.cpp"
