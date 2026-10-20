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

#include "CopyMetadata.h"

#include <string>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/MetadataKeyFilter.h"

NATRON_NAMESPACE_ENTER

CopyMetadata::CopyMetadata(NodePtr node)
    : NativeEffectBase(node)
    , _mode()
    , _keys()
{
}

void
CopyMetadata::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName("mode");
    mode->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("replace", tr("Replace").toStdString(), tr("Replaces the matching keys of A by those of B.").toStdString()));
        choices.push_back(ChoiceOption("mergeB", tr("Merge, B Wins").toStdString(), tr("Adds the matching keys of B to A, B's value kept on a clash.").toStdString()));
        choices.push_back(ChoiceOption("mergeA", tr("Merge, A Wins").toStdString(), tr("Adds the matching keys of B to A, A's value kept on a clash.").toStdString()));
        mode->populateChoices(choices);
    }
    mode->setDefaultValue((int)eModeReplace);
    page->addKnob(mode);
    _mode = mode;

    KnobStringPtr keys = createKnob<KnobString>(tr("Keys"));
    keys->setName("keys");
    keys->setAnimationEnabled(false);
    keys->setAsMultiLine();
    keys->setHintToolTip(tr("The keys to copy, as a comma- or newline-separated list of globs where * matches "
                            "any characters and ? one, for example exr/*, exif/Make. Empty means every key.")
                             .toStdString());
    page->addKnob(keys);
    _keys = keys;
}

Route
CopyMetadata::route(double time,
                    ViewIdx /*view*/)
{
    return Route::input(0, time);
}

ImageMetadata
CopyMetadata::deriveOutputMetadata(double time,
                                   ViewIdx view)
{
    const ImageMetadata a = getInputMetadata(0, time, view);

    if (!getInput(1)) {
        return a;
    }
    KnobChoicePtr modeKnob = _mode.lock();
    KnobStringPtr keysKnob = _keys.lock();
    const int mode = modeKnob ? modeKnob->getValueAtTime(time, 0, view) : (int)eModeReplace;
    const MetadataKeyFilter filter(keysKnob ? keysKnob->getValueAtTime(time, 0, view) : std::string());

    ImageMetadata filteredB;
    const ImageMetadata b = getInputMetadata(1, time, view);

    for (ImageMetadata::const_iterator it = b.begin(); it != b.end(); ++it) {
        if (filter.matches(it->first)) {
            filteredB.set(it->first, it->second);
        }
    }

    ImageMetadata result;

    if (mode == eModeReplace) {
        for (ImageMetadata::const_iterator it = a.begin(); it != a.end(); ++it) {
            if (!filter.matches(it->first)) {
                result.set(it->first, it->second);
            }
        }
        result.merge(filteredB, ImageMetadata::eMergePreferOther);
    } else {
        result = a;
        result.merge(filteredB, mode == eModeMergeBWins ? ImageMetadata::eMergePreferOther : ImageMetadata::eMergePreferThis);
    }

    return result;
}

NATRON_NAMESPACE_EXIT
