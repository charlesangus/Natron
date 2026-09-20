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

#include "LayerChannelRow.h"

#include <algorithm>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QVariant>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/Button.h"
#include "Gui/ChannelColor.h"
#include "Gui/ComboBox.h"
#include "Gui/Label.h"
#include "Gui/LineEdit.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kChannelNameProperty = "channelName";
const char* const kChannelColorProperty = "channelColor";
const char* const kInvalidProperty = "invalid";
const char* const kColorLayerID = "Color";

QString
qs(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
}

QString
matchesPrefix()
{
    return QString::fromUtf8("matches: ");
}
} // namespace

struct LayerChannelRow::ComboEntry {
    enum KindEnum {
        eKindNone,
        eKindAll,
        eKindRegex,
        eKindLayer,
        eKindChannel,
        eKindNewLayer,
        eKindAbsent
    };

    KindEnum kind;
    std::string value;
    QString text;
    bool separatorAfter;

    ComboEntry(KindEnum k,
               const std::string& v,
               const QString& t)
        : kind(k)
        , value(v)
        , text(t)
        , separatorAfter(false)
    {
    }
};

LayerChannelRow::LayerChannelRow(ModeEnum mode,
                                 QWidget* parent)
    : QWidget(parent)
    , _mode(mode)
    , _layers()
    , _listNewLayerEntry(false)
    , _setRowMode(mode == eModeSetRow0 ? eSetRowModeNone : eSetRowModeLayer)
    , _layerID()
    , _channelValue()
    , _committedPattern()
    , _enabledChannels()
    , _withChannelButtons(false)
    , _absentMarker()
    , _removable(false)
    , _patternValid(true)
    , _updatingCombo(false)
    , _lastComboIndex(-1)
    , _entries()
    , _layout(0)
    , _combo(0)
    , _buttonsContainer(0)
    , _buttonsLayout(0)
    , _channelButtons()
    , _patternEdit(0)
    , _matchesLabel(0)
    , _removeButton(0)
{
    _layout = new QHBoxLayout(this);
    _layout->setContentsMargins(0, 0, 0, 0);
    _layout->setSpacing(3);

    _combo = new ComboBox(this);
    _layout->addWidget(_combo);

    _buttonsContainer = new QWidget(this);
    _buttonsLayout = new QHBoxLayout(_buttonsContainer);
    _buttonsLayout->setContentsMargins(0, 0, 0, 0);
    _buttonsLayout->setSpacing(2);
    _layout->addWidget(_buttonsContainer);

    _patternEdit = new LineEdit(this);
    _patternEdit->setPlaceholderText(tr("layer name pattern"));
    _patternEdit->installEventFilter(this);
    _layout->addWidget(_patternEdit);

    _matchesLabel = new Label(this);
    _layout->addWidget(_matchesLabel);

    _layout->addStretch();

    _removeButton = new Button(QString::fromUtf8("−"), this);
    _removeButton->setToolTip(tr("Remove this layer from the set"));
    _removeButton->setFocusPolicy(Qt::StrongFocus);
    _layout->addWidget(_removeButton);

    QObject::connect(_combo, SIGNAL(currentIndexChanged(int)), this, SLOT(onComboIndexChanged(int)));
    QObject::connect(_patternEdit, SIGNAL(editingFinished()), this, SLOT(onPatternEditingFinished()));
    QObject::connect(_removeButton, SIGNAL(clicked()), this, SLOT(onRemoveClicked()));

    rebuildCombo();
    rebuildChannelButtons();
    refreshPatternValidity();
    refreshVisibility();
}

LayerChannelRow::~LayerChannelRow()
{
}

LayerChannelRow::ModeEnum
LayerChannelRow::getMode() const
{
    return _mode;
}

void
LayerChannelRow::setAvailableLayers(const std::vector<LayerEntry>& layers,
                                    bool listNewLayerEntry)
{
    _layers = layers;
    _listNewLayerEntry = listNewLayerEntry;
    rebuildCombo();
    rebuildChannelButtons();
    refreshPatternValidity();
}

const std::vector<LayerChannelRow::LayerEntry>&
LayerChannelRow::getAvailableLayers() const
{
    return _layers;
}

void
LayerChannelRow::setSetRowValue(SetRowModeEnum mode,
                                const std::string& layerOrPattern,
                                const std::vector<std::string>& enabledChannels)
{
    _setRowMode = mode;
    if (mode == eSetRowModeLayer) {
        _layerID = layerOrPattern;
        _enabledChannels = enabledChannels;
    } else if (mode == eSetRowModeRegex) {
        _committedPattern = layerOrPattern;
        _patternEdit->setText(qs(layerOrPattern));
    }
    rebuildCombo();
    rebuildChannelButtons();
    refreshPatternValidity();
    refreshVisibility();
}

void
LayerChannelRow::setLayerSelectValue(const std::string& layerID,
                                     const std::vector<std::string>& enabledChannels,
                                     bool withChannelButtons)
{
    _setRowMode = eSetRowModeLayer;
    _layerID = layerID;
    _enabledChannels = enabledChannels;
    _withChannelButtons = withChannelButtons;
    rebuildCombo();
    rebuildChannelButtons();
    refreshVisibility();
}

void
LayerChannelRow::setChannelSelectValue(const std::string& layerDotChannel)
{
    _channelValue = layerDotChannel;
    rebuildCombo();
}

void
LayerChannelRow::setAbsentMarker(const QString& text)
{
    _absentMarker = text;
    rebuildCombo();
}

void
LayerChannelRow::clearAbsentMarker()
{
    _absentMarker.clear();
    rebuildCombo();
}

bool
LayerChannelRow::hasAbsentMarker() const
{
    for (std::size_t i = 0; i < _entries.size(); ++i) {
        if (_entries[i].kind == ComboEntry::eKindAbsent) {
            return true;
        }
    }

    return false;
}

void
LayerChannelRow::setRowRemovable(bool removable)
{
    _removable = removable;
    refreshVisibility();
}

bool
LayerChannelRow::isRowRemovable() const
{
    return _removable;
}

LayerChannelRow::SetRowModeEnum
LayerChannelRow::getSetRowMode() const
{
    return _setRowMode;
}

std::string
LayerChannelRow::getCurrentLayerID() const
{
    return _layerID;
}

std::string
LayerChannelRow::getCurrentChannel() const
{
    return _channelValue;
}

std::string
LayerChannelRow::getCommittedPattern() const
{
    return _committedPattern;
}

bool
LayerChannelRow::isPatternValid() const
{
    return _patternValid;
}

std::vector<std::string>
LayerChannelRow::getEnabledChannels() const
{
    return _enabledChannels;
}

bool
LayerChannelRow::isChannelEnabled(const std::string& channel) const
{
    return std::find(_enabledChannels.begin(), _enabledChannels.end(), channel) != _enabledChannels.end();
}

QStringList
LayerChannelRow::getComboEntries() const
{
    QStringList ret;
    for (std::size_t i = 0; i < _entries.size(); ++i) {
        ret.push_back(_entries[i].text);
    }

    return ret;
}

QString
LayerChannelRow::getCurrentComboText() const
{
    return _combo->getCurrentIndexText();
}

QStringList
LayerChannelRow::getChannelButtonNames() const
{
    QStringList ret;
    for (std::size_t i = 0; i < _channelButtons.size(); ++i) {
        ret.push_back(_channelButtons[i]->text());
    }

    return ret;
}

QString
LayerChannelRow::getMatchesText() const
{
    return _matchesLabel->text();
}

ComboBox*
LayerChannelRow::getComboBox() const
{
    return _combo;
}

LineEdit*
LayerChannelRow::getPatternEdit() const
{
    return _patternEdit;
}

Label*
LayerChannelRow::getMatchesLabel() const
{
    return _matchesLabel;
}

Button*
LayerChannelRow::getRemoveButton() const
{
    return _removeButton;
}

Button*
LayerChannelRow::getChannelButton(const std::string& channel) const
{
    for (std::size_t i = 0; i < _channelButtons.size(); ++i) {
        if (_channelButtons[i]->property(kChannelNameProperty).toString() == qs(channel)) {
            return _channelButtons[i];
        }
    }

    return 0;
}

const LayerChannelRow::LayerEntry*
LayerChannelRow::findLayer(const std::string& id) const
{
    for (std::size_t i = 0; i < _layers.size(); ++i) {
        if (_layers[i].id == id) {
            return &_layers[i];
        }
    }

    return 0;
}

QString
LayerChannelRow::currentValueLabel() const
{
    if (_mode == eModeChannelSelect) {
        return qs(_channelValue);
    }
    const LayerEntry* layer = findLayer(_layerID);

    return layer ? qs(layer->label) : qs(_layerID);
}

bool
LayerChannelRow::currentValueIsListed() const
{
    switch (_mode) {
    case eModeSetRow0:
    case eModeSetRowN:
        if (_setRowMode != eSetRowModeLayer) {
            return true;
        }

        return findLayer(_layerID) != 0;
    case eModeLayerSelect:

        return findLayer(_layerID) != 0;
    case eModeChannelSelect: {
        if (_channelValue.empty()) {
            return true;
        }
        for (std::size_t i = 0; i < _layers.size(); ++i) {
            for (std::size_t c = 0; c < _layers[i].channels.size(); ++c) {
                if (_layers[i].id + "." + _layers[i].channels[c] == _channelValue) {
                    return true;
                }
            }
        }

        return false;
    }
    }

    return true;
}

void
LayerChannelRow::rebuildCombo()
{
    _entries.clear();

    std::vector<const LayerEntry*> ordered;
    for (std::size_t i = 0; i < _layers.size(); ++i) {
        ordered.push_back(&_layers[i]);
    }
    if (_mode == eModeSetRow0) {
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (ordered[i]->id == kColorLayerID) {
                std::rotate(ordered.begin(), ordered.begin() + i, ordered.begin() + i + 1);
                break;
            }
        }
    }

    switch (_mode) {
    case eModeSetRow0:
        _entries.push_back(ComboEntry(ComboEntry::eKindNone, std::string(), tr("None")));
        _entries.push_back(ComboEntry(ComboEntry::eKindAll, std::string(), tr("All")));
        _entries.push_back(ComboEntry(ComboEntry::eKindRegex, std::string(), tr("Regex...")));
        _entries.back().separatorAfter = true;
        break;
    case eModeSetRowN:
        _entries.push_back(ComboEntry(ComboEntry::eKindRegex, std::string(), tr("Regex...")));
        _entries.back().separatorAfter = true;
        break;
    case eModeLayerSelect:
        break;
    case eModeChannelSelect:
        _entries.push_back(ComboEntry(ComboEntry::eKindNone, std::string(), tr("None")));
        break;
    }

    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (_mode == eModeChannelSelect) {
            for (std::size_t c = 0; c < ordered[i]->channels.size(); ++c) {
                const std::string& channel = ordered[i]->channels[c];
                _entries.push_back(ComboEntry(ComboEntry::eKindChannel, ordered[i]->id + "." + channel,
                                              qs(ordered[i]->label) + QLatin1Char('.') + qs(channel)));
            }
        } else {
            _entries.push_back(ComboEntry(ComboEntry::eKindLayer, ordered[i]->id, qs(ordered[i]->label)));
        }
    }

    if (!_absentMarker.isEmpty() && !currentValueIsListed()) {
        _entries.push_back(ComboEntry(ComboEntry::eKindAbsent, std::string(),
                                      currentValueLabel() + QLatin1Char(' ') + _absentMarker));
    }

    if (_mode == eModeLayerSelect && _listNewLayerEntry) {
        if (!_entries.empty()) {
            _entries.back().separatorAfter = true;
        }
        _entries.push_back(ComboEntry(ComboEntry::eKindNewLayer, std::string(), tr("New layer...")));
    }

    _updatingCombo = true;
    _combo->clear();
    for (std::size_t i = 0; i < _entries.size(); ++i) {
        _combo->addItem(_entries[i].text);
        if (_entries[i].separatorAfter) {
            _combo->addSeparator();
        }
    }
    selectEntryForCurrentValue();
    _updatingCombo = false;
}

void
LayerChannelRow::selectEntryForCurrentValue()
{
    int index = -1;
    for (std::size_t i = 0; i < _entries.size(); ++i) {
        const ComboEntry& e = _entries[i];
        bool hit = false;
        switch (e.kind) {
        case ComboEntry::eKindNone:
            hit = (_mode == eModeChannelSelect) ? _channelValue.empty() : (_setRowMode == eSetRowModeNone);
            break;
        case ComboEntry::eKindAll:
            hit = (_setRowMode == eSetRowModeAll);
            break;
        case ComboEntry::eKindRegex:
            hit = (_setRowMode == eSetRowModeRegex);
            break;
        case ComboEntry::eKindLayer:
            hit = (_setRowMode == eSetRowModeLayer) && (e.value == _layerID);
            break;
        case ComboEntry::eKindChannel:
            hit = (e.value == _channelValue);
            break;
        case ComboEntry::eKindAbsent:
            hit = true;
            break;
        case ComboEntry::eKindNewLayer:
            break;
        }
        if (hit) {
            index = (int)i;
            break;
        }
    }
    if (index >= 0) {
        _combo->setCurrentIndex_no_emit(index);
    } else {
        _combo->setCurrentText_no_emit(currentValueLabel());
    }
    _lastComboIndex = index;
}

void
LayerChannelRow::clearChannelButtons()
{
    for (std::size_t i = 0; i < _channelButtons.size(); ++i) {
        _buttonsLayout->removeWidget(_channelButtons[i]);
        delete _channelButtons[i];
    }
    _channelButtons.clear();
}

void
LayerChannelRow::rebuildChannelButtons()
{
    clearChannelButtons();

    const LayerEntry* layer = findLayer(_layerID);
    // An absent layer has no listing to build from, so its remembered channels stand in.
    const std::vector<std::string>& channels = layer ? layer->channels : _enabledChannels;
    const QString layerLabel = layer ? qs(layer->label) : qs(_layerID);

    QWidget* previous = _combo;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        Button* b = new Button(qs(channels[i]), _buttonsContainer);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::StrongFocus);
        b->setToolTip(layerLabel + QLatin1Char('.') + qs(channels[i]));
        b->setProperty(kChannelNameProperty, qs(channels[i]));
        QColor color;
        if (getChannelColorFromName(channels[i], &color)) {
            b->setProperty(kChannelColorProperty, QVariant(color));
            b->setStyleSheet(QString::fromUtf8("QPushButton:checked { background-color: %1; }").arg(color.name()));
        }
        b->blockSignals(true);
        b->setChecked(isChannelEnabled(channels[i]));
        b->blockSignals(false);
        QObject::connect(b, &QPushButton::toggled, this, [this, b](bool checked) {
            onChannelButtonToggled(b, checked);
        });
        _buttonsLayout->addWidget(b);
        _channelButtons.push_back(b);
        QWidget::setTabOrder(previous, b);
        previous = b;
    }
    QWidget::setTabOrder(previous, _patternEdit);
    QWidget::setTabOrder(_patternEdit, _removeButton);
}

void
LayerChannelRow::refreshVisibility()
{
    bool isSetRow = (_mode == eModeSetRow0) || (_mode == eModeSetRowN);
    bool showButtons = (isSetRow && _setRowMode == eSetRowModeLayer) || (_mode == eModeLayerSelect && _withChannelButtons);
    bool showPattern = isSetRow && (_setRowMode == eSetRowModeRegex);

    _buttonsContainer->setVisible(showButtons);
    _patternEdit->setVisible(showPattern);
    _matchesLabel->setVisible(showPattern);
    _removeButton->setVisible(_removable);
}

void
LayerChannelRow::refreshPatternValidity()
{
    const QString pattern = _patternEdit->text();
    QRegularExpression re(pattern);

    _patternValid = re.isValid();
    _patternEdit->setProperty(kInvalidProperty, QVariant(!_patternValid));
    if (_patternValid) {
        _patternEdit->setStyleSheet(QString());
        _patternEdit->setToolTip(QString());
    } else {
        _patternEdit->setStyleSheet(QString::fromUtf8("border: 1px solid red;"));
        _patternEdit->setToolTip(re.errorString());
    }

    QStringList matched;
    if (_patternValid) {
        QRegularExpression anchored(QRegularExpression::anchoredPattern(pattern));
        for (std::size_t i = 0; i < _layers.size(); ++i) {
            if (anchored.match(qs(_layers[i].label)).hasMatch()) {
                matched.push_back(qs(_layers[i].label));
            }
        }
    }
    _matchesLabel->setText(matchesPrefix() + (matched.isEmpty() ? tr("no match") : matched.join(QString::fromUtf8(", "))));
}

void
LayerChannelRow::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::EnabledChange) {
        _combo->setEnabled_natron(isEnabled());
    }
    QWidget::changeEvent(e);
}

bool
LayerChannelRow::eventFilter(QObject* watched,
                             QEvent* e)
{
    if ((watched == _patternEdit) && (e->type() == QEvent::KeyPress)) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            _patternEdit->setText(qs(_committedPattern));
            refreshPatternValidity();

            return true;
        }
    }

    return QWidget::eventFilter(watched, e);
}

void
LayerChannelRow::onComboIndexChanged(int index)
{
    if (_updatingCombo || (index < 0) || (index >= (int)_entries.size())) {
        return;
    }
    const ComboEntry entry = _entries[index];
    const bool hadMarker = hasAbsentMarker();

    switch (entry.kind) {
    case ComboEntry::eKindNone:
        if (_mode == eModeChannelSelect) {
            _channelValue.clear();
            if (hadMarker) {
                rebuildCombo();
            }
            _lastComboIndex = _combo->activeIndex();
            Q_EMIT channelSelected(QString());
        } else {
            _setRowMode = eSetRowModeNone;
            if (hadMarker) {
                rebuildCombo();
            }
            _lastComboIndex = _combo->activeIndex();
            refreshVisibility();
            Q_EMIT modeChosen(eSetRowModeNone);
        }
        break;
    case ComboEntry::eKindAll:
        _setRowMode = eSetRowModeAll;
        if (hadMarker) {
            rebuildCombo();
        }
        _lastComboIndex = _combo->activeIndex();
        refreshVisibility();
        Q_EMIT modeChosen(eSetRowModeAll);
        break;
    case ComboEntry::eKindRegex:
        _setRowMode = eSetRowModeRegex;
        if (hadMarker) {
            rebuildCombo();
        }
        _lastComboIndex = _combo->activeIndex();
        refreshVisibility();
        Q_EMIT modeChosen(eSetRowModeRegex);
        break;
    case ComboEntry::eKindLayer: {
        _setRowMode = eSetRowModeLayer;
        _layerID = entry.value;
        const LayerEntry* layer = findLayer(_layerID);
        _enabledChannels = layer ? layer->channels : std::vector<std::string>();
        if (hadMarker) {
            rebuildCombo();
        }
        _lastComboIndex = _combo->activeIndex();
        rebuildChannelButtons();
        refreshVisibility();
        Q_EMIT layerChosen(qs(_layerID));
        break;
    }
    case ComboEntry::eKindChannel:
        _channelValue = entry.value;
        if (hadMarker) {
            rebuildCombo();
        }
        _lastComboIndex = _combo->activeIndex();
        Q_EMIT channelSelected(qs(_channelValue));
        break;
    case ComboEntry::eKindNewLayer:
        _updatingCombo = true;
        if (_lastComboIndex >= 0) {
            _combo->setCurrentIndex_no_emit(_lastComboIndex);
        } else {
            _combo->setCurrentText_no_emit(currentValueLabel());
        }
        _updatingCombo = false;
        Q_EMIT newLayerRequested();
        break;
    case ComboEntry::eKindAbsent:
        _lastComboIndex = index;
        break;
    }
}

void
LayerChannelRow::onChannelButtonToggled(Button* button,
                                        bool checked)
{
    const QString name = button->property(kChannelNameProperty).toString();

    std::vector<std::string> enabled;
    for (std::size_t i = 0; i < _channelButtons.size(); ++i) {
        if (_channelButtons[i]->isChecked()) {
            enabled.push_back(_channelButtons[i]->property(kChannelNameProperty).toString().toStdString());
        }
    }
    _enabledChannels = enabled;
    Q_EMIT channelToggled(name, checked);
}

void
LayerChannelRow::onPatternEditingFinished()
{
    refreshPatternValidity();
    if (!_patternValid) {
        return;
    }
    const std::string pattern = _patternEdit->text().toStdString();
    if (pattern == _committedPattern) {
        return;
    }
    _committedPattern = pattern;
    Q_EMIT patternCommitted(qs(_committedPattern));
}

void
LayerChannelRow::onRemoveClicked()
{
    Q_EMIT removeRequested();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_LayerChannelRow.cpp"
