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

#ifndef NATRON_GUI_LAYERCHANNELROW_H
#define NATRON_GUI_LAYERCHANNELROW_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QStringList>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

class QHBoxLayout;

NATRON_NAMESPACE_ENTER

/**
 * @brief One "[ComboBox] [channel buttons...] | [pattern] matches: ... [-]" row shared by the
 * channel-set, layer-select and channel-select knob GUIs. It is a plain Qt widget fed with
 * plain data: it never touches a knob, a node or a project, and only reports user actions
 * through its signals. The owner is expected to apply the change to the knob and call the
 * setters back with the resulting value.
 **/
class LayerChannelRow
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum ModeEnum {
        eModeSetRow0,
        eModeSetRowN,
        eModeLayerSelect,
        eModeChannelSelect
    };

    enum SetRowModeEnum {
        eSetRowModeNone,
        eSetRowModeAll,
        eSetRowModeLayer,
        eSetRowModeRegex
    };

    Q_ENUM(SetRowModeEnum)

    struct LayerEntry {
        std::string id;
        std::string label;
        std::vector<std::string> channels;
    };

    explicit LayerChannelRow(ModeEnum mode,
                             QWidget* parent = 0);

    virtual ~LayerChannelRow() OVERRIDE;

    ModeEnum getMode() const;

    /**
     * @brief Replaces the layers the combo offers. In layer-select mode listNewLayerEntry
     * appends a "New layer..." entry after a separator. The current value is kept and
     * re-resolved against the new list.
     **/
    void setAvailableLayers(const std::vector<LayerEntry>& layers,
                            bool listNewLayerEntry);

    const std::vector<LayerEntry>& getAvailableLayers() const;

    /// Set-row modes only.
    void setSetRowValue(SetRowModeEnum mode,
                        const std::string& layerOrPattern,
                        const std::vector<std::string>& enabledChannels);

    /// Layer-select mode only.
    void setLayerSelectValue(const std::string& layerID,
                             const std::vector<std::string>& enabledChannels,
                             bool withChannelButtons);

    /// Channel-select mode only: "layer.channel", or empty for None.
    void setChannelSelectValue(const std::string& layerDotChannel);

    /**
     * @brief Shows the current value as one extra combo item suffixed with @p text (e.g.
     * "(not in input)") while that value is not among the listed entries. The item goes
     * away when the marker is cleared or the user picks another entry.
     **/
    void setAbsentMarker(const QString& text);
    void clearAbsentMarker();
    bool hasAbsentMarker() const;

    void setRowRemovable(bool removable);
    bool isRowRemovable() const;

    SetRowModeEnum getSetRowMode() const;
    std::string getCurrentLayerID() const;
    std::string getCurrentChannel() const;
    std::string getCommittedPattern() const;
    bool isPatternValid() const;
    std::vector<std::string> getEnabledChannels() const;
    bool isChannelEnabled(const std::string& channel) const;

    QStringList getComboEntries() const;
    Q_INVOKABLE QString getCurrentComboText() const;
    QStringList getChannelButtonNames() const;
    QString getMatchesText() const;

    ComboBox* getComboBox() const;
    LineEdit* getPatternEdit() const;
    Label* getMatchesLabel() const;
    Button* getRemoveButton() const;
    Button* getChannelButton(const std::string& channel) const;

Q_SIGNALS:

    void modeChosen(LayerChannelRow::SetRowModeEnum setRowMode);
    void layerChosen(const QString& layerID);
    void channelToggled(const QString& channel, bool on);
    void channelSelected(const QString& layerDotChannel);
    void patternCommitted(const QString& pattern);
    void newLayerRequested();
    void removeRequested();

protected:
    virtual void changeEvent(QEvent* e) OVERRIDE FINAL;
    virtual bool eventFilter(QObject* watched, QEvent* e) OVERRIDE FINAL;

private Q_SLOTS:

    void onComboIndexChanged(int index);
    void onPatternEditingFinished();
    void onRemoveClicked();

private:
    struct ComboEntry;

    void onChannelButtonToggled(Button* button, bool checked);
    void rebuildCombo();
    void rebuildChannelButtons();
    void refreshVisibility();
    void refreshPatternValidity();
    void selectEntryForCurrentValue();
    bool currentValueIsListed() const;
    QString currentValueLabel() const;
    const LayerEntry* findLayer(const std::string& id) const;
    void clearChannelButtons();

    ModeEnum _mode;
    std::vector<LayerEntry> _layers;
    bool _listNewLayerEntry;
    SetRowModeEnum _setRowMode;
    std::string _layerID;
    std::string _channelValue;
    std::string _committedPattern;
    std::vector<std::string> _enabledChannels;
    bool _withChannelButtons;
    QString _absentMarker;
    bool _removable;
    bool _patternValid;
    bool _updatingCombo;
    int _lastComboIndex;
    std::vector<ComboEntry> _entries;

    QHBoxLayout* _layout;
    ComboBox* _combo;
    QWidget* _buttonsContainer;
    QHBoxLayout* _buttonsLayout;
    std::vector<Button*> _channelButtons;
    LineEdit* _patternEdit;
    Label* _matchesLabel;
    Button* _removeButton;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_LAYERCHANNELROW_H
