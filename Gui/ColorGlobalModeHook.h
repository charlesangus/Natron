/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-3.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef Gui_ColorGlobalModeHook_h
#define Gui_ColorGlobalModeHook_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"
#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A colour the global mode's C, S, T and V strokes can drive: a colour knob, or a colour
 * that lives inside another knob's data, such as one entry of a list. The hook belongs to a
 * KnobGui, which is what the global mode tracks, steps from and ends on when it is destroyed;
 * once getKnobGui() returns null nothing else is called.
 *
 * An edit is begin, any number of moves, then end or abandon; end leaves one undo step and
 * abandon puts the colour back as it was at begin with none.
 **/
class ColorGlobalModeHook {
public:
    typedef std::function<void(float* rgb, int count)> DisplayConverter;

    virtual ~ColorGlobalModeHook() { }

    virtual KnobGui* getKnobGui() const = 0;

    virtual QString getLabel() const = 0;

    virtual bool isColorEditable() const = 0;

    virtual ColorKnobRoleEnum getColorRole() const = 0;

    virtual LumaBasis getEditBasis() const = 0;

    virtual DisplayConverter getDisplayConverter() const = 0;

    virtual KnobColorValue getCurrentColorValue() const = 0;

    /**
     * @brief Shows the colour as the one being edited and brings it into view; detach() takes the
     * highlight away again.
     **/
    virtual void attach() = 0;
    virtual void detach() = 0;

    /**
     * @brief Records the hook's knob as the last touched, so Ctrl+E comes back to it.
     **/
    virtual void touch() = 0;

    virtual void beginEdit(const QString& label) = 0;
    virtual void moveEdit(const KnobColorValue& value, const QString& label) = 0;
    virtual void endEdit(const KnobColorValue& value, const QString& label) = 0;
    virtual void abandonEdit() = 0;
};

/**
 * @brief A KnobGui that drives the global mode through a hook rather than as a colour or value
 * knob of its own. Null while it has nothing to drive, for instance no selected entry; such a
 * knob is not eligible for the global mode.
 **/
class ColorGlobalModeHookProvider {
public:
    virtual ~ColorGlobalModeHookProvider() { }

    virtual std::shared_ptr<ColorGlobalModeHook> makeColorGlobalModeHook() = 0;
};

/**
 * @brief The colour-picking display converter for colourManagement's project, or an empty one
 * without colour management.
 **/
ColorGlobalModeHook::DisplayConverter makeColorPickingDisplayConverter(const ProjectColorManagementPtr& colorManagement);

NATRON_NAMESPACE_EXIT

#endif // Gui_ColorGlobalModeHook_h
