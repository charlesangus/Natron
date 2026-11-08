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

#ifndef Gui_ValueKnobGlobalModeTarget_h
#define Gui_ValueKnobGlobalModeTarget_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPointer>
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Engine/GlobalModeKind.h"
#include "Engine/GlobalModeStroke.h"

#include "Gui/GlobalModeTarget.h"
#include "Gui/GuiFwd.h"
#include "Gui/ValueKnobEditSession.h"

NATRON_NAMESPACE_ENTER

struct ColorTrackballHudChip;
class GlobalModeRowHighlight;

/**
 * @brief "<node label>: <knob script name>", the hint block's title for a value target.
 **/
QString globalModeValueTitle(const KnobIPtr& knob);

/**
 * @brief The hint block's rate line for a lone knob of kind (the knob's own increment for size
 * and generic knobs, its label for a 2D spatial one).
 **/
QString globalModeValueRateLine(GlobalModeKindEnum kind, const KnobIPtr& knob);

/**
 * @brief What value targets share: one stroke at a time over one or two numeric knobs, written
 * through a ValueKnobEditSession so each stroke is one undo step, the edited rows outlined, and
 * the kind's glyph on the reveal.
 **/
class ValueGlobalModeTargetBase
    : public GlobalModeTarget {
public:
    ValueGlobalModeTargetBase();
    virtual ~ValueGlobalModeTargetBase() OVERRIDE;

    virtual DockablePanel* getPanel() const OVERRIDE;
    virtual void attach() OVERRIDE;
    virtual void detach() OVERRIDE;
    virtual bool beginStroke() OVERRIDE;
    virtual void moveStroke(double dx, double dy, double gearFactor) OVERRIDE;
    virtual void endStroke() OVERRIDE;
    virtual void abandonStroke() OVERRIDE;
    virtual void syncReveal(ColorTrackballReveal* reveal, bool strokeActive) OVERRIDE;

    bool isStroking() const
    {
        return _session.isActive();
    }

    /**
     * @brief The kind the reveal and the stroke maths use right now.
     **/
    GlobalModeKindEnum getStrokeKind() const
    {
        return getStrokeSpec().kind;
    }

protected:
    /**
     * @brief The knobs a stroke started now would edit, in slot order.
     **/
    virtual std::vector<KnobGuiPtr> getStrokeKnobGuis() const = 0;

    /**
     * @brief The stroke maths' kind and lock; increments and isInt are filled in from the
     * knobs.
     **/
    virtual GlobalModeValueStrokeSpec getStrokeSpec() const = 0;

    virtual QString getStrokeLabel() const = 0;

    /**
     * @brief Folds the travel so far into a new start, so a lock changed mid-stroke applies
     * from where the value is now instead of reinterpreting the earlier pixels.
     **/
    void restartStrokeTravel();

    /**
     * @brief Applies change, which swaps the knobs being edited, so that a running stroke
     * commits what it did to the old knobs as its own undo step and carries on with the new
     * ones.
     **/
    void switchStrokeKnobs(const std::function<void()>& change);

    /**
     * @brief Moves the row outline onto the current knobs.
     **/
    void refreshHighlight();

private:
    GlobalModeValueStrokeSpec fullSpec() const;

    GlobalModeValueRates _rates;
    ValueKnobEditSession _session;
    GlobalModeValueStroke _stroke;
    std::vector<QPointer<GlobalModeRowHighlight>> _highlights;
    bool _attached;
};

/**
 * @brief Global mode on one KnobDouble or KnobInt outside a transform group. Q/W/E/R lock the
 * stroke to dimension 1-4 and A frees it again; a lock past the knob's dimensions is ignored. A
 * lone skew knob edits its X/Y sibling with it when the node has one, x drag on skew X and y on
 * skew Y.
 **/
class ValueKnobGlobalModeTarget
    : public ValueGlobalModeTargetBase {
public:
    explicit ValueKnobGlobalModeTarget(const KnobGuiPtr& knobGui);
    virtual ~ValueKnobGlobalModeTarget() OVERRIDE;

    virtual KnobGui* getKnobGui() const OVERRIDE FINAL;
    virtual std::vector<KnobGui*> getKnobGuis() const OVERRIDE FINAL;
    virtual bool handleKey(int key, Qt::KeyboardModifiers modifiers, bool strokeActive) OVERRIDE FINAL;
    virtual void fillHudState(ColorTrackballHudState* state) const OVERRIDE FINAL;

    GlobalModeKindEnum getKind() const
    {
        return _kind;
    }

    /**
     * @brief The locked slot, or kGlobalModeLockAll.
     **/
    int getLockDimension() const
    {
        return _lock;
    }

protected:
    virtual std::vector<KnobGuiPtr> getStrokeKnobGuis() const OVERRIDE FINAL;
    virtual GlobalModeValueStrokeSpec getStrokeSpec() const OVERRIDE FINAL;
    virtual QString getStrokeLabel() const OVERRIDE FINAL;

private:
    int slotCount() const;

    KnobGuiWPtr _knobGui;
    KnobGuiWPtr _skewSibling;
    bool _siblingFirst;
    GlobalModeKindEnum _kind;
    int _lock;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ValueKnobGlobalModeTarget_h
