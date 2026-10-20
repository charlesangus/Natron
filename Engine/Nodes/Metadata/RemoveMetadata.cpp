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

#include "RemoveMetadata.h"

#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/MetadataKeyFilter.h"

NATRON_NAMESPACE_ENTER

RemoveMetadata::RemoveMetadata(NodePtr node)
    : NativeEffectBase(node)
    , _removeAll()
    , _keys()
{
}

void
RemoveMetadata::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobBoolPtr removeAll = createKnob<KnobBool>(tr("Remove All"));
    removeAll->setName("removeAll");
    removeAll->setAnimationEnabled(false);
    removeAll->setDefaultValue(false);
    removeAll->setHintToolTip(tr("Removes every key, whatever the key list says.").toStdString());
    page->addKnob(removeAll);
    _removeAll = removeAll;

    KnobStringPtr keys = createKnob<KnobString>(tr("Keys"));
    keys->setName("keys");
    keys->setAnimationEnabled(false);
    keys->setAsMultiLine();
    keys->setHintToolTip(tr("The keys to remove, as a comma- or newline-separated list of globs where * "
                            "matches any characters and ? one, for example exr/*, exif/Make.")
                             .toStdString());
    page->addKnob(keys);
    _keys = keys;

    refreshKeysVisibility();
}

void
RemoveMetadata::refreshKeysVisibility()
{
    KnobBoolPtr removeAll = _removeAll.lock();
    KnobStringPtr keys = _keys.lock();

    if (removeAll && keys) {
        keys->setSecret(removeAll->getValue());
    }
}

bool
RemoveMetadata::knobChanged(KnobI* k,
                            ValueChangedReasonEnum /*reason*/,
                            ViewSpec /*view*/,
                            double /*time*/,
                            bool /*originatedFromMainThread*/)
{
    KnobBoolPtr removeAll = _removeAll.lock();

    if (removeAll && (k == removeAll.get())) {
        refreshKeysVisibility();

        return true;
    }

    return false;
}

void
RemoveMetadata::onKnobsLoaded()
{
    refreshKeysVisibility();
}

Route
RemoveMetadata::route(double time,
                      ViewIdx /*view*/)
{
    return Route::input(0, time);
}

ImageMetadata
RemoveMetadata::deriveOutputMetadata(double time,
                                     ViewIdx view)
{
    const ImageMetadata input = getUpstreamMetadata(time, view);
    KnobBoolPtr removeAll = _removeAll.lock();
    KnobStringPtr keysKnob = _keys.lock();

    if (!removeAll || !keysKnob) {
        return input;
    }
    if (removeAll->getValueAtTime(time, 0, view)) {
        return ImageMetadata();
    }

    const std::string list = keysKnob->getValueAtTime(time, 0, view);
    const MetadataKeyFilter filter(list);

    // An empty list keeps everything here, where the filter alone would match every key.
    if (filter.matchesAll()) {
        return input;
    }

    ImageMetadata result;

    for (ImageMetadata::const_iterator it = input.begin(); it != input.end(); ++it) {
        if (!filter.matches(it->first)) {
            result.set(it->first, it->second);
        }
    }

    return result;
}

NATRON_NAMESPACE_EXIT
