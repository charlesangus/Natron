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

#ifndef NATRON_GUI_KNOBGUILAYERCHANNELBASE_H
#define NATRON_GUI_KNOBGUILAYERCHANNELBASE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGui.h"
#include "Gui/LayerChannelRow.h"

class QVBoxLayout;

NATRON_NAMESPACE_ENTER

struct KnobGuiLayerChannelBasePrivate;

/**
 * @brief What the channel-set, layer-select and channel-select knob GUIs share: one
 * composite widget in the field column, the layer list obtained from the node for the
 * knob's role, repopulation on the node's layer refresh and on registry changes, and
 * one non-mergeable undo step per user action. Subclasses build their rows inside
 * getContainer() and implement refreshWidgets().
 **/
class KnobGuiLayerChannelBase
    : public KnobGui {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    KnobGuiLayerChannelBase(KnobIPtr knob,
                            KnobGuiContainerI* container);

    virtual ~KnobGuiLayerChannelBase() OVERRIDE;

    virtual KnobIPtr getKnob() const OVERRIDE FINAL;

    virtual void removeSpecificGui() OVERRIDE;

public Q_SLOTS:

    void onLayerListRefreshed();

private Q_SLOTS:

    void onDeferredRefresh();

protected:
    /**
     * @brief Creates the composite widget and its vertical layout inside the field
     * layout; the subclass then adds its rows to getContainerLayout().
     **/
    void createContainer(QHBoxLayout* layout);

    QWidget* getContainer() const;
    QVBoxLayout* getContainerLayout() const;

    NodePtr getNode() const;

    /// Whether the knob lists the project registry rather than an input's layers.
    bool isTargetKnob() const;

    /// The suffix a selected-but-unlisted value is shown with, per the knob's role.
    QString getAbsentMarkerText() const;

    const std::vector<LayerChannelRow::LayerEntry>& getLayers() const;
    const LayerChannelRow::LayerEntry* findLayer(const std::string& id) const;

    /**
     * @brief Applies newValue to the knob as one undo step. A no-op when the knob
     * already holds it.
     **/
    void pushValue(const std::string& newValue);

    void scheduleRefresh(bool relistLayers);

    /**
     * @brief Rebuilds the widgets from the knob value, fetching the layer list from the
     * node again first when relistLayers is set.
     **/
    void refresh(bool relistLayers);

    /// Rebuilds the widgets from the knob value and getLayers().
    virtual void refreshWidgets() = 0;

    virtual void createWidget(QHBoxLayout* layout) OVERRIDE = 0;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;
    virtual void setEnabled() OVERRIDE FINAL;
    virtual void setReadOnly(bool readOnly, int dimension) OVERRIDE FINAL;
    virtual void setDirty(bool dirty) OVERRIDE FINAL;
    virtual void updateGUI(int dimension) OVERRIDE FINAL;
    virtual void reflectAnimationLevel(int dimension, AnimationLevelEnum level) OVERRIDE FINAL;
    virtual void reflectExpressionState(int dimension, bool hasExpr) OVERRIDE FINAL;
    virtual void updateToolTip() OVERRIDE FINAL;

private:
    virtual bool shouldAddStretch() const OVERRIDE FINAL
    {
        return false;
    }

    void listLayers();

    std::unique_ptr<KnobGuiLayerChannelBasePrivate> _imp;
};

/// Whether two layer lists carry the same entries in the same order.
bool sameLayerEntries(const std::vector<LayerChannelRow::LayerEntry>& a,
                      const std::vector<LayerChannelRow::LayerEntry>& b);

/**
 * @brief The layers the given node offers for the given knob's role, Color sorted
 * first. Usable for any knob on the node, not just knobs with a KnobGuiLayerChannelBase.
 **/
std::vector<LayerChannelRow::LayerEntry> listLayerEntriesForKnob(const NodePtr& node, const KnobIPtr& knob);

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUILAYERCHANNELBASE_H
