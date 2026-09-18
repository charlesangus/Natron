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

#ifndef CACHEMEMORYPRESSUREGUARD_H
#define CACHEMEMORYPRESSUREGUARD_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

// AppManager::checkCacheFreeMemoryIsGoodEnough() runs on every app-wide cache allocation and,
// whenever the host's free RAM is below Settings::getUnreachableRamPercent() of total RAM, evicts
// LRU entries from both the node cache and the deep image cache until it isn't -- regardless of
// which test triggered the allocation. On a host that is already below that bar (common: it reads
// MemFree, which page cache keeps low), a test asserting that an entry it just created is still
// resident would fail for a reason that has nothing to do with the code under test. Pin the knob
// to 0 for the guard's lifetime to make that eviction unreachable, and restore whatever it was
// set to beforehand.
class DisableUnreachableRAMPurging {
public:
    DisableUnreachableRAMPurging()
        : _knob(appPTR->getCurrentSettings()->getKnobByNameAndType<KnobInt>("unreachableRAMPercent"))
        , _previousValue(0)
    {
        if (_knob) {
            _previousValue = _knob->getValue();
            _knob->setValue(0);
        }
    }

    ~DisableUnreachableRAMPurging()
    {
        if (_knob) {
            _knob->setValue(_previousValue);
        }
    }

private:
    KnobIntPtr _knob;
    int _previousValue;
};

NATRON_NAMESPACE_EXIT

#endif // CACHEMEMORYPRESSUREGUARD_H
