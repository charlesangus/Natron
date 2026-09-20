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

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QKeyEvent>
#include <QStringList>

#include <gtest/gtest.h>

#include "Gui/Button.h"
#include "Gui/ChannelColor.h"
#include "Gui/ComboBox.h"
#include "Gui/Label.h"
#include "Gui/LayerChannelRow.h"
#include "Gui/LineEdit.h"

NATRON_NAMESPACE_USING

namespace {

typedef LayerChannelRow::LayerEntry LayerEntry;

LayerEntry
makeLayer(const std::string& id,
          const std::vector<std::string>& channels)
{
    LayerEntry e;
    e.id = id;
    e.label = id;
    e.channels = channels;

    return e;
}

std::vector<std::string>
rgba()
{
    std::vector<std::string> v;
    v.push_back("R");
    v.push_back("G");
    v.push_back("B");
    v.push_back("A");

    return v;
}

std::vector<std::string>
rgb()
{
    std::vector<std::string> v;
    v.push_back("R");
    v.push_back("G");
    v.push_back("B");

    return v;
}

std::vector<std::string>
single(const std::string& c)
{
    return std::vector<std::string>(1, c);
}

// Color deliberately not first, so the row-0 hoisting is observable.
std::vector<LayerEntry>
sampleLayers()
{
    std::vector<LayerEntry> layers;
    layers.push_back(makeLayer("diffuse", rgb()));
    layers.push_back(makeLayer("Color", rgba()));
    layers.push_back(makeLayer("specular", rgb()));
    layers.push_back(makeLayer("depth", single("Z")));

    return layers;
}

QStringList
sl(const char* a,
   const char* b = 0,
   const char* c = 0,
   const char* d = 0,
   const char* e = 0,
   const char* f = 0,
   const char* g = 0,
   const char* h = 0)
{
    QStringList ret;
    const char* all[] = { a, b, c, d, e, f, g, h };
    for (int i = 0; i < 8; ++i) {
        if (all[i]) {
            ret.push_back(QString::fromUtf8(all[i]));
        }
    }

    return ret;
}

int
comboIndexOf(const LayerChannelRow& row,
             const char* text)
{
    return row.getComboEntries().indexOf(QString::fromUtf8(text));
}

void
pressEscape(QWidget* w)
{
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
}

// The raw focus chain also threads through containers and labels; Tab only stops on
// widgets that accept it and are shown, which is what the user experiences.
QWidget*
nextTabStop(QWidget* from,
            QWidget* root)
{
    QWidget* w = from->nextInFocusChain();
    while (w && (w != from) && (!(w->focusPolicy() & Qt::TabFocus) || !w->isVisibleTo(root) || (w == root))) {
        w = w->nextInFocusChain();
    }

    return w;
}

} // namespace

TEST(LayerChannelRow, SetRow0EntryOrderHoistsColor)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRow0);
    row.setAvailableLayers(sampleLayers(), false);

    EXPECT_EQ(sl("None", "All", "Regex...", "Color", "diffuse", "specular", "depth"), row.getComboEntries());
    EXPECT_EQ(QString::fromUtf8("None"), row.getCurrentComboText());
    EXPECT_FALSE(row.getComboBox()->count() == 0);
}

TEST(LayerChannelRow, SetRowNEntryOrderKeepsListOrder)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRowN);
    row.setAvailableLayers(sampleLayers(), false);

    EXPECT_EQ(sl("Regex...", "diffuse", "Color", "specular", "depth"), row.getComboEntries());
}

TEST(LayerChannelRow, LayerSelectEntryOrderWithAndWithoutNewLayer)
{
    LayerChannelRow row(LayerChannelRow::eModeLayerSelect);
    row.setAvailableLayers(sampleLayers(), false);
    EXPECT_EQ(sl("diffuse", "Color", "specular", "depth"), row.getComboEntries());

    row.setAvailableLayers(sampleLayers(), true);
    EXPECT_EQ(sl("diffuse", "Color", "specular", "depth", "New layer..."), row.getComboEntries());
}

TEST(LayerChannelRow, ChannelSelectEntryOrder)
{
    std::vector<LayerEntry> layers;
    layers.push_back(makeLayer("Color", rgba()));
    layers.push_back(makeLayer("depth", single("Z")));

    LayerChannelRow row(LayerChannelRow::eModeChannelSelect);
    row.setAvailableLayers(layers, false);
    EXPECT_EQ(sl("None", "Color.R", "Color.G", "Color.B", "Color.A", "depth.Z"), row.getComboEntries());
    EXPECT_EQ(QString::fromUtf8("None"), row.getCurrentComboText());

    row.setChannelSelectValue("depth.Z");
    EXPECT_EQ(QString::fromUtf8("depth.Z"), row.getCurrentComboText());

    int selectedCount = 0;
    QString selected;
    QObject::connect(&row, &LayerChannelRow::channelSelected, [&](const QString& v) {
        ++selectedCount;
        selected = v;
    });
    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "Color.A"));
    EXPECT_EQ(1, selectedCount);
    EXPECT_EQ(QString::fromUtf8("Color.A"), selected);
    EXPECT_EQ("Color.A", row.getCurrentChannel());

    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "None"));
    EXPECT_EQ(2, selectedCount);
    EXPECT_TRUE(selected.isEmpty());
    EXPECT_TRUE(row.getCurrentChannel().empty());
}

TEST(LayerChannelRow, ChoosingLayerRebuildsCheckedColouredButtonsAndEmitsOnce)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRow0);
    row.setAvailableLayers(sampleLayers(), false);

    int chosenCount = 0;
    QString chosen;
    QObject::connect(&row, &LayerChannelRow::layerChosen, [&](const QString& id) {
        ++chosenCount;
        chosen = id;
    });

    EXPECT_FALSE(row.getChannelButton("R"));
    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "Color"));

    EXPECT_EQ(1, chosenCount);
    EXPECT_EQ(QString::fromUtf8("Color"), chosen);
    EXPECT_EQ(LayerChannelRow::eSetRowModeLayer, row.getSetRowMode());
    EXPECT_EQ("Color", row.getCurrentLayerID());
    EXPECT_EQ(sl("R", "G", "B", "A"), row.getChannelButtonNames());
    EXPECT_EQ(rgba(), row.getEnabledChannels());

    const char* names[] = { "R", "G", "B", "A" };
    for (int i = 0; i < 4; ++i) {
        Button* b = row.getChannelButton(names[i]);
        ASSERT_TRUE(b);
        EXPECT_TRUE(b->isCheckable());
        EXPECT_TRUE(b->isChecked());
        EXPECT_EQ(QString::fromUtf8("Color.") + QString::fromUtf8(names[i]), b->toolTip());
        QColor expected;
        ASSERT_TRUE(getChannelColorFromName(names[i], &expected));
        EXPECT_EQ(expected, b->property("channelColor").value<QColor>());
        EXPECT_FALSE(b->styleSheet().isEmpty());
    }

    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "depth"));
    EXPECT_EQ(2, chosenCount);
    EXPECT_EQ(sl("Z"), row.getChannelButtonNames());
    Button* z = row.getChannelButton("Z");
    ASSERT_TRUE(z);
    EXPECT_TRUE(z->isChecked());
    EXPECT_FALSE(z->property("channelColor").isValid());
    EXPECT_TRUE(z->styleSheet().isEmpty());
}

TEST(LayerChannelRow, ToggleEmitsOncePerClickAndTracksEnabledChannels)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRowN);
    row.setAvailableLayers(sampleLayers(), false);
    std::vector<std::string> enabled;
    enabled.push_back("R");
    enabled.push_back("B");
    row.setSetRowValue(LayerChannelRow::eSetRowModeLayer, "diffuse", enabled);

    EXPECT_EQ(QString::fromUtf8("diffuse"), row.getCurrentComboText());
    EXPECT_TRUE(row.getChannelButton("R")->isChecked());
    EXPECT_FALSE(row.getChannelButton("G")->isChecked());
    EXPECT_TRUE(row.getChannelButton("B")->isChecked());

    int toggledCount = 0;
    QString toggledName;
    bool toggledOn = false;
    QObject::connect(&row, &LayerChannelRow::channelToggled, [&](const QString& name, bool on) {
        ++toggledCount;
        toggledName = name;
        toggledOn = on;
    });

    row.getChannelButton("G")->click();
    EXPECT_EQ(1, toggledCount);
    EXPECT_EQ(QString::fromUtf8("G"), toggledName);
    EXPECT_TRUE(toggledOn);
    EXPECT_EQ(rgb(), row.getEnabledChannels());

    row.getChannelButton("R")->click();
    EXPECT_EQ(2, toggledCount);
    EXPECT_EQ(QString::fromUtf8("R"), toggledName);
    EXPECT_FALSE(toggledOn);
    std::vector<std::string> gb;
    gb.push_back("G");
    gb.push_back("B");
    EXPECT_EQ(gb, row.getEnabledChannels());
}

TEST(LayerChannelRow, NoneAllRegexHideButtonsAndEmitModeOnce)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRow0);
    row.setAvailableLayers(sampleLayers(), false);
    row.setSetRowValue(LayerChannelRow::eSetRowModeLayer, "Color", rgba());
    EXPECT_TRUE(row.getChannelButton("R")->parentWidget()->isVisibleTo(&row));

    int modeCount = 0;
    LayerChannelRow::SetRowModeEnum mode = LayerChannelRow::eSetRowModeLayer;
    QObject::connect(&row, &LayerChannelRow::modeChosen, [&](LayerChannelRow::SetRowModeEnum m) {
        ++modeCount;
        mode = m;
    });

    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "All"));
    EXPECT_EQ(1, modeCount);
    EXPECT_EQ(LayerChannelRow::eSetRowModeAll, mode);
    EXPECT_FALSE(row.getChannelButton("R")->parentWidget()->isVisibleTo(&row));
    EXPECT_FALSE(row.getPatternEdit()->isVisibleTo(&row));

    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "None"));
    EXPECT_EQ(2, modeCount);
    EXPECT_EQ(LayerChannelRow::eSetRowModeNone, mode);

    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "Regex..."));
    EXPECT_EQ(3, modeCount);
    EXPECT_EQ(LayerChannelRow::eSetRowModeRegex, mode);
    EXPECT_TRUE(row.getPatternEdit()->isVisibleTo(&row));
    EXPECT_TRUE(row.getMatchesLabel()->isVisibleTo(&row));
    EXPECT_FALSE(row.getChannelButton("R")->parentWidget()->isVisibleTo(&row));
}

TEST(LayerChannelRow, RegexValidationMatchesCommitAndRevert)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRowN);
    row.setAvailableLayers(sampleLayers(), false);
    row.setSetRowValue(LayerChannelRow::eSetRowModeRegex, "spec.*", std::vector<std::string>());

    LineEdit* edit = row.getPatternEdit();
    EXPECT_EQ(QString::fromUtf8("spec.*"), edit->text());
    EXPECT_TRUE(row.isPatternValid());
    EXPECT_FALSE(edit->property("invalid").toBool());
    EXPECT_EQ(QString::fromUtf8("matches: specular"), row.getMatchesText());

    int committedCount = 0;
    QString committed;
    QObject::connect(&row, &LayerChannelRow::patternCommitted, [&](const QString& p) {
        ++committedCount;
        committed = p;
    });

    edit->setText(QString::fromUtf8("("));
    Q_EMIT edit->editingFinished();
    EXPECT_FALSE(row.isPatternValid());
    EXPECT_TRUE(edit->property("invalid").toBool());
    EXPECT_FALSE(edit->toolTip().isEmpty());
    EXPECT_FALSE(edit->styleSheet().isEmpty());
    EXPECT_EQ(0, committedCount);
    EXPECT_EQ("spec.*", row.getCommittedPattern());
    EXPECT_EQ(QString::fromUtf8("matches: no match"), row.getMatchesText());

    edit->setText(QString::fromUtf8("d.*"));
    Q_EMIT edit->editingFinished();
    EXPECT_TRUE(row.isPatternValid());
    EXPECT_FALSE(edit->property("invalid").toBool());
    EXPECT_TRUE(edit->toolTip().isEmpty());
    EXPECT_TRUE(edit->styleSheet().isEmpty());
    EXPECT_EQ(1, committedCount);
    EXPECT_EQ(QString::fromUtf8("d.*"), committed);
    EXPECT_EQ("d.*", row.getCommittedPattern());
    EXPECT_EQ(QString::fromUtf8("matches: diffuse, depth"), row.getMatchesText());

    Q_EMIT edit->editingFinished();
    EXPECT_EQ(1, committedCount);

    edit->setText(QString::fromUtf8("zzz"));
    Q_EMIT edit->editingFinished();
    EXPECT_EQ(2, committedCount);
    EXPECT_EQ(QString::fromUtf8("matches: no match"), row.getMatchesText());

    edit->setText(QString::fromUtf8("spec"));
    pressEscape(edit);
    EXPECT_EQ(QString::fromUtf8("zzz"), edit->text());
    EXPECT_EQ(2, committedCount);

    edit->setText(QString::fromUtf8("["));
    pressEscape(edit);
    EXPECT_EQ(QString::fromUtf8("zzz"), edit->text());
    EXPECT_TRUE(row.isPatternValid());
    EXPECT_FALSE(edit->property("invalid").toBool());
}

TEST(LayerChannelRow, AbsentMarkerItemPresentOnlyWhileSet)
{
    LayerChannelRow row(LayerChannelRow::eModeLayerSelect);
    row.setAvailableLayers(sampleLayers(), true);
    row.setLayerSelectValue("mask", rgba(), true);
    EXPECT_FALSE(row.hasAbsentMarker());

    row.setAbsentMarker(QString::fromUtf8("(not in project)"));
    EXPECT_TRUE(row.hasAbsentMarker());
    EXPECT_EQ(sl("diffuse", "Color", "specular", "depth", "mask (not in project)", "New layer..."), row.getComboEntries());
    EXPECT_EQ(QString::fromUtf8("mask (not in project)"), row.getCurrentComboText());
    EXPECT_EQ(sl("R", "G", "B", "A"), row.getChannelButtonNames());

    row.clearAbsentMarker();
    EXPECT_FALSE(row.hasAbsentMarker());
    EXPECT_EQ(sl("diffuse", "Color", "specular", "depth", "New layer..."), row.getComboEntries());

    row.setAbsentMarker(QString::fromUtf8("(not in project)"));
    EXPECT_TRUE(row.hasAbsentMarker());

    int chosenCount = 0;
    QObject::connect(&row, &LayerChannelRow::layerChosen, [&](const QString&) {
        ++chosenCount;
    });
    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "specular"));
    EXPECT_EQ(1, chosenCount);
    EXPECT_FALSE(row.hasAbsentMarker());
    EXPECT_EQ(sl("diffuse", "Color", "specular", "depth", "New layer..."), row.getComboEntries());
    EXPECT_EQ(QString::fromUtf8("specular"), row.getCurrentComboText());

    row.setLayerSelectValue("Color", rgba(), true);
    row.setAbsentMarker(QString::fromUtf8("(not in project)"));
    EXPECT_FALSE(row.hasAbsentMarker());
}

TEST(LayerChannelRow, AbsentMarkerInChannelSelect)
{
    LayerChannelRow row(LayerChannelRow::eModeChannelSelect);
    row.setAvailableLayers(sampleLayers(), false);
    row.setChannelSelectValue("mask.A");
    row.setAbsentMarker(QString::fromUtf8("(not in input)"));
    EXPECT_TRUE(row.hasAbsentMarker());
    EXPECT_EQ(QString::fromUtf8("mask.A (not in input)"), row.getCurrentComboText());

    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "None"));
    EXPECT_FALSE(row.hasAbsentMarker());
    EXPECT_EQ(QString::fromUtf8("None"), row.getCurrentComboText());
}

TEST(LayerChannelRow, NewLayerEntryEmitsAndReverts)
{
    LayerChannelRow row(LayerChannelRow::eModeLayerSelect);
    row.setAvailableLayers(sampleLayers(), true);
    row.setLayerSelectValue("specular", rgb(), false);
    EXPECT_FALSE(row.getChannelButton("R")->parentWidget()->isVisibleTo(&row));

    int newCount = 0;
    int chosenCount = 0;
    QObject::connect(&row, &LayerChannelRow::newLayerRequested, [&]() {
        ++newCount;
    });
    QObject::connect(&row, &LayerChannelRow::layerChosen, [&](const QString&) {
        ++chosenCount;
    });

    row.getComboBox()->setCurrentIndex(comboIndexOf(row, "New layer..."));
    EXPECT_EQ(1, newCount);
    EXPECT_EQ(0, chosenCount);
    EXPECT_EQ(QString::fromUtf8("specular"), row.getCurrentComboText());
    EXPECT_EQ("specular", row.getCurrentLayerID());
}

TEST(LayerChannelRow, RemoveButtonFollowsRemovableAndEmitsOnce)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRowN);
    row.setAvailableLayers(sampleLayers(), false);
    EXPECT_FALSE(row.isRowRemovable());
    EXPECT_FALSE(row.getRemoveButton()->isVisibleTo(&row));

    row.setRowRemovable(true);
    EXPECT_TRUE(row.getRemoveButton()->isVisibleTo(&row));

    int removeCount = 0;
    QObject::connect(&row, &LayerChannelRow::removeRequested, [&]() {
        ++removeCount;
    });
    row.getRemoveButton()->click();
    EXPECT_EQ(1, removeCount);

    row.setRowRemovable(false);
    EXPECT_FALSE(row.getRemoveButton()->isVisibleTo(&row));
}

TEST(LayerChannelRow, ProgrammaticValueSettersEmitNothing)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRow0);
    int emitted = 0;
    QObject::connect(&row, &LayerChannelRow::modeChosen, [&](LayerChannelRow::SetRowModeEnum) {
        ++emitted;
    });
    QObject::connect(&row, &LayerChannelRow::layerChosen, [&](const QString&) {
        ++emitted;
    });
    QObject::connect(&row, &LayerChannelRow::channelToggled, [&](const QString&, bool) {
        ++emitted;
    });
    QObject::connect(&row, &LayerChannelRow::patternCommitted, [&](const QString&) {
        ++emitted;
    });

    row.setAvailableLayers(sampleLayers(), false);
    row.setSetRowValue(LayerChannelRow::eSetRowModeAll, std::string(), std::vector<std::string>());
    row.setSetRowValue(LayerChannelRow::eSetRowModeLayer, "diffuse", rgb());
    row.setSetRowValue(LayerChannelRow::eSetRowModeRegex, "diff.*", std::vector<std::string>());
    row.setAvailableLayers(sampleLayers(), false);
    EXPECT_EQ(0, emitted);
    EXPECT_EQ(QString::fromUtf8("Regex..."), row.getCurrentComboText());
    EXPECT_EQ(QString::fromUtf8("matches: diffuse"), row.getMatchesText());
}

TEST(LayerChannelRow, DisablingGreysEveryChild)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRow0);
    row.setAvailableLayers(sampleLayers(), false);
    row.setSetRowValue(LayerChannelRow::eSetRowModeLayer, "Color", rgba());
    row.setRowRemovable(true);

    row.setEnabled(false);
    EXPECT_FALSE(row.getComboBox()->isEnabled());
    EXPECT_FALSE(row.getComboBox()->getEnabled_natron());
    EXPECT_FALSE(row.getChannelButton("R")->isEnabled());
    EXPECT_FALSE(row.getRemoveButton()->isEnabled());

    row.setEnabled(true);
    EXPECT_TRUE(row.getComboBox()->getEnabled_natron());
    EXPECT_TRUE(row.getChannelButton("R")->isEnabled());
}

TEST(LayerChannelRow, TabOrderRunsComboButtonsRemove)
{
    LayerChannelRow row(LayerChannelRow::eModeSetRowN);
    row.setAvailableLayers(sampleLayers(), false);
    row.setSetRowValue(LayerChannelRow::eSetRowModeLayer, "diffuse", rgb());
    row.setRowRemovable(true);

    QWidget* w = row.getComboBox();
    w = nextTabStop(w, &row);
    EXPECT_EQ(row.getChannelButton("R"), w);
    w = nextTabStop(w, &row);
    EXPECT_EQ(row.getChannelButton("G"), w);
    w = nextTabStop(w, &row);
    EXPECT_EQ(row.getChannelButton("B"), w);
    w = nextTabStop(w, &row);
    EXPECT_EQ(row.getRemoveButton(), w);
}
