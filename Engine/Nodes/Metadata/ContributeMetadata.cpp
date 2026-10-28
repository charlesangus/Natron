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

#include "ContributeMetadata.h"

#include <string>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"

NATRON_NAMESPACE_ENTER

ContributeMetadata::ContributeMetadata(NodePtr node)
    : NativeEffectBase(node)
    , _note()
    , _mode()
    , _dropKey()
{
}

void
ContributeMetadata::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobStringPtr note = createKnob<KnobString>(tr("Note"));
    note->setName("note");
    note->setAnimationEnabled(false);
    note->setHintToolTip(tr("The text published as fr.natron.ContributeMetadata.note.").toStdString());
    page->addKnob(note);
    _note = note;

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName("mode");
    mode->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("inheritAll", tr("Inherit All").toStdString(), tr("Keeps every key of Source.").toStdString()));
        choices.push_back(ChoiceOption("dropOneKey", tr("Drop One Key").toStdString(), tr("Keeps every key of Source but the Drop Key one.").toStdString()));
        choices.push_back(ChoiceOption("inheritNothing", tr("Inherit Nothing").toStdString(), tr("Keeps no key of Source.").toStdString()));
        mode->populateChoices(choices);
    }
    mode->setDefaultValue((int)eModeInheritAll);
    mode->setHintToolTip(tr("What this node keeps of the metadata of Source, on top of the keys it always "
                            "publishes.")
                             .toStdString());
    page->addKnob(mode);
    _mode = mode;

    KnobStringPtr dropKey = createKnob<KnobString>(tr("Drop Key"));
    dropKey->setName("dropKey");
    dropKey->setAnimationEnabled(false);
    dropKey->setDefaultValue("ofx/sampletype");
    dropKey->setHintToolTip(tr("The key of Source dropped when Mode is Drop One Key. A key Source does not "
                               "hold drops nothing, and a key this node publishes is published anyway.")
                                .toStdString());
    page->addKnob(dropKey);
    _dropKey = dropKey;
}

Route
ContributeMetadata::route(double time,
                          ViewIdx /*view*/)
{
    return Route::input(0, time);
}

ImageMetadata
ContributeMetadata::deriveOutputMetadata(double time,
                                         ViewIdx view)
{
    KnobChoicePtr modeKnob = _mode.lock();
    const int mode = modeKnob ? modeKnob->getValueAtTime(time, 0, view) : (int)eModeInheritAll;
    ImageMetadata metadata;

    if (mode != eModeInheritNothing) {
        metadata = getUpstreamMetadata(time, view);
        if (mode == eModeDropOneKey) {
            KnobStringPtr dropKeyKnob = _dropKey.lock();
            if (dropKeyKnob) {
                metadata.remove(dropKeyKnob->getValueAtTime(time, 0, view));
            }
        }
    }

    KnobStringPtr noteKnob = _note.lock();
    const std::string prefix(kContributeMetadataKeyPrefix);

    metadata.setString(prefix + "note", noteKnob ? noteKnob->getValueAtTime(time, 0, view) : std::string());
    metadata.setInt(prefix + "revision", 1);
    metadata.setDouble(prefix + "quality", 0.75);
    // ImageMetadata holds no string vector, so the two tags are published joined.
    metadata.setString(prefix + "tags", "reviewed, approved");
    metadata.setIntVector(prefix + "renderRegion", std::vector<int>({ 0, 0, 1280, 720 }));
    metadata.setDoubleVector(prefix + "weights", std::vector<double>({ 1., 0.5, 0.25 }));
    metadata.setDouble("ofx/framerate", 30.);

    return metadata;
}

NATRON_NAMESPACE_EXIT
