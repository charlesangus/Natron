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

#ifndef Gui_TransformGroupGlobalModeTarget_h
#define Gui_TransformGroupGlobalModeTarget_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/GlobalModeStroke.h"

#include "Gui/GuiFwd.h"
#include "Gui/ValueKnobGlobalModeTarget.h"

NATRON_NAMESPACE_ENTER

struct TransformInteractKnobs;

/**
 * @brief Global mode on the knobs of one transform interact, edited as a group: Q/W/E/R/T make
 * translate, rotate, scale, skew or center the active member, 1/2 lock the stroke to x or y and
 * A frees it. Skew edits skewX and skewY together in one stroke. A member the node lacks, or
 * hides, cannot be switched to.
 **/
class TransformGroupGlobalModeTarget
    : public ValueGlobalModeTargetBase {
public:
    /**
     * @brief Engaged on member; entryKnob, when it is skewY, makes skewY the skew member's knob
     * so that stepping on from it moves past both skew knobs.
     **/
    TransformGroupGlobalModeTarget(KnobGuiContainerI* container,
                                   const TransformInteractKnobs& knobs,
                                   GlobalModeGroupMemberEnum member,
                                   const KnobI* entryKnob = 0);
    virtual ~TransformGroupGlobalModeTarget() OVERRIDE;

    /**
     * @brief The member whose knob is knob, false when knob is none of them.
     **/
    static bool memberOf(const TransformInteractKnobs& knobs, const KnobI* knob, GlobalModeGroupMemberEnum* member);

    virtual KnobGui* getKnobGui() const OVERRIDE FINAL;
    virtual std::vector<KnobGui*> getKnobGuis() const OVERRIDE FINAL;
    virtual bool handleKey(int key, Qt::KeyboardModifiers modifiers, bool strokeActive) OVERRIDE FINAL;
    virtual void fillHudState(ColorTrackballHudState* state) const OVERRIDE FINAL;

    GlobalModeGroupMemberEnum getMember() const
    {
        return _member;
    }

    /**
     * @brief The locked axis, or kGlobalModeLockAll.
     **/
    int getLockDimension() const
    {
        return _lock;
    }

    /**
     * @brief Whether member exists on the node and is shown, which switching to it requires.
     **/
    bool hasMember(GlobalModeGroupMemberEnum member) const;

protected:
    virtual std::vector<KnobGuiPtr> getStrokeKnobGuis() const OVERRIDE FINAL;
    virtual GlobalModeValueStrokeSpec getStrokeSpec() const OVERRIDE FINAL;
    virtual QString getStrokeLabel() const OVERRIDE FINAL;

private:
    std::vector<KnobGuiPtr> memberKnobGuis(GlobalModeGroupMemberEnum member) const;

    KnobGuiWPtr _translate;
    KnobGuiWPtr _rotate;
    KnobGuiWPtr _scale;
    KnobGuiWPtr _skewX;
    KnobGuiWPtr _skewY;
    KnobGuiWPtr _center;
    GlobalModeGroupMemberEnum _member;
    bool _skewOnY;
    int _lock;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_TransformGroupGlobalModeTarget_h
