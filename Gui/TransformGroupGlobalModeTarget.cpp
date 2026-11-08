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

#include "TransformGroupGlobalModeTarget.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCoreApplication>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "Gui/ColorTrackballHud.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobGuiContainerI.h"

NATRON_NAMESPACE_ENTER

namespace {

QString
groupText(const char* text)
{
    return QCoreApplication::translate("TransformGroupGlobalModeTarget", text);
}

KnobGuiPtr
guiFor(KnobGuiContainerI* container,
       const KnobDoublePtr& knob)
{
    return (container && knob) ? container->getKnobGui(knob) : KnobGuiPtr();
}

GlobalModeKindEnum
kindOf(GlobalModeGroupMemberEnum member)
{
    switch (member) {
    case eGlobalModeGroupMemberTranslate:
    case eGlobalModeGroupMemberCenter:
        return eGlobalModeKindTranslate;
    case eGlobalModeGroupMemberRotate:
        return eGlobalModeKindRotate;
    case eGlobalModeGroupMemberScale:
        return eGlobalModeKindScale;
    case eGlobalModeGroupMemberSkew:
        return eGlobalModeKindSkew;
    }

    return eGlobalModeKindTranslate;
}

} // namespace

TransformGroupGlobalModeTarget::TransformGroupGlobalModeTarget(KnobGuiContainerI* container,
                                                               const TransformInteractKnobs& knobs,
                                                               GlobalModeGroupMemberEnum member,
                                                               const KnobI* entryKnob)
    : ValueGlobalModeTargetBase()
    , _translate(guiFor(container, knobs.translate))
    , _rotate(guiFor(container, knobs.rotate))
    , _scale(guiFor(container, knobs.scale))
    , _skewX(guiFor(container, knobs.skewX))
    , _skewY(guiFor(container, knobs.skewY))
    , _center(guiFor(container, knobs.center))
    , _member(member)
    , _skewOnY(knobs.skewY && (entryKnob == knobs.skewY.get()))
    , _lock(kGlobalModeLockAll)
{
}

TransformGroupGlobalModeTarget::~TransformGroupGlobalModeTarget()
{
}

bool
TransformGroupGlobalModeTarget::memberOf(const TransformInteractKnobs& knobs,
                                         const KnobI* knob,
                                         GlobalModeGroupMemberEnum* member)
{
    if (!knob) {
        return false;
    }
    if (knob == knobs.translate.get()) {
        *member = eGlobalModeGroupMemberTranslate;
    } else if (knob == knobs.rotate.get()) {
        *member = eGlobalModeGroupMemberRotate;
    } else if (knob == knobs.scale.get()) {
        *member = eGlobalModeGroupMemberScale;
    } else if ((knob == knobs.skewX.get()) || (knob == knobs.skewY.get())) {
        *member = eGlobalModeGroupMemberSkew;
    } else if (knob == knobs.center.get()) {
        *member = eGlobalModeGroupMemberCenter;
    } else {
        return false;
    }

    return true;
}

std::vector<KnobGuiPtr>
TransformGroupGlobalModeTarget::memberKnobGuis(GlobalModeGroupMemberEnum member) const
{
    std::vector<KnobGuiPtr> result;
    KnobGuiPtr knobGui;

    switch (member) {
    case eGlobalModeGroupMemberTranslate:
        knobGui = _translate.lock();
        break;
    case eGlobalModeGroupMemberRotate:
        knobGui = _rotate.lock();
        break;
    case eGlobalModeGroupMemberScale:
        knobGui = _scale.lock();
        break;
    case eGlobalModeGroupMemberCenter:
        knobGui = _center.lock();
        break;
    case eGlobalModeGroupMemberSkew: {
        KnobGuiPtr x = _skewX.lock();
        KnobGuiPtr y = _skewY.lock();

        if (x) {
            result.push_back(x);
        }
        if (y) {
            result.push_back(y);
        }

        return result;
    }
    }
    if (knobGui) {
        result.push_back(knobGui);
    }

    return result;
}

bool
TransformGroupGlobalModeTarget::hasMember(GlobalModeGroupMemberEnum member) const
{
    const std::vector<KnobGuiPtr> knobGuis = memberKnobGuis(member);

    for (std::size_t i = 0; i < knobGuis.size(); ++i) {
        if (!knobGuis[i]->isSecretRecursive()) {
            return true;
        }
    }

    return false;
}

KnobGui*
TransformGroupGlobalModeTarget::getKnobGui() const
{
    if ((_member == eGlobalModeGroupMemberSkew) && _skewOnY) {
        if (KnobGuiPtr y = _skewY.lock()) {
            return y.get();
        }
    }

    const std::vector<KnobGuiPtr> knobGuis = memberKnobGuis(_member);

    return knobGuis.empty() ? 0 : knobGuis.front().get();
}

std::vector<KnobGui*>
TransformGroupGlobalModeTarget::getKnobGuis() const
{
    std::vector<KnobGui*> result;
    const KnobGuiWPtr* all[6] = { &_translate, &_rotate, &_scale, &_skewX, &_skewY, &_center };

    for (int i = 0; i < 6; ++i) {
        if (KnobGuiPtr knobGui = all[i]->lock()) {
            result.push_back(knobGui.get());
        }
    }

    return result;
}

std::vector<KnobGuiPtr>
TransformGroupGlobalModeTarget::getStrokeKnobGuis() const
{
    return memberKnobGuis(_member);
}

GlobalModeValueStrokeSpec
TransformGroupGlobalModeTarget::getStrokeSpec() const
{
    GlobalModeValueStrokeSpec spec;

    spec.kind = kindOf(_member);
    spec.lockDimension = _lock;

    return spec;
}

QString
TransformGroupGlobalModeTarget::getStrokeLabel() const
{
    return groupText("Global edit");
}

bool
TransformGroupGlobalModeTarget::handleKey(int key,
                                          Qt::KeyboardModifiers modifiers,
                                          bool /*strokeActive*/)
{
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        return false;
    }

    // Every member edits two axes except rotate; the lock keys stay meaningful across a switch.
    const GlobalModeValueKeyAction action = classifyGlobalModeValueKey(key, true, 2);

    switch (action.action) {
    case eGlobalModeValueKeyNone:
        return false;
    case eGlobalModeValueKeyLockAll:
    case eGlobalModeValueKeyLockDimension: {
        const int lock = (action.action == eGlobalModeValueKeyLockAll) ? kGlobalModeLockAll : action.dimension;

        if (lock == _lock) {
            return false;
        }
        _lock = lock;
        restartStrokeTravel();

        return true;
    }
    case eGlobalModeValueKeySwitchMember: {
        if ((action.member == _member) || !hasMember(action.member)) {
            return false;
        }

        const GlobalModeGroupMemberEnum member = action.member;

        switchStrokeKnobs([this, member]() {
            _member = member;
            _skewOnY = false;
        });

        return true;
    }
    }

    return false;
}

void
TransformGroupGlobalModeTarget::fillHudState(ColorTrackballHudState* state) const
{
    KnobGui* active = getKnobGui();
    KnobIPtr knob = active ? active->getKnob() : KnobIPtr();

    state->knobLabel = globalModeValueTitle(knob);

    struct MemberChip {
        const char* key;
        const char* caption;
        GlobalModeGroupMemberEnum member;
    };
    const MemberChip members[5] = {
        { "Q", QT_TRANSLATE_NOOP("TransformGroupGlobalModeTarget", "Translate"), eGlobalModeGroupMemberTranslate },
        { "W", QT_TRANSLATE_NOOP("TransformGroupGlobalModeTarget", "Rotate"), eGlobalModeGroupMemberRotate },
        { "E", QT_TRANSLATE_NOOP("TransformGroupGlobalModeTarget", "Scale"), eGlobalModeGroupMemberScale },
        { "R", QT_TRANSLATE_NOOP("TransformGroupGlobalModeTarget", "Skew"), eGlobalModeGroupMemberSkew },
        { "T", QT_TRANSLATE_NOOP("TransformGroupGlobalModeTarget", "Center"), eGlobalModeGroupMemberCenter },
    };
    std::vector<ColorTrackballHudChip> memberRow;

    for (int i = 0; i < 5; ++i) {
        ColorTrackballHudChip chip;

        chip.key = QString::fromUtf8(members[i].key);
        chip.caption = groupText(members[i].caption);
        chip.lit = (members[i].member == _member);
        chip.dimmed = !hasMember(members[i].member);
        memberRow.push_back(chip);
    }
    state->chipRows.push_back(memberRow);

    const bool oneAxis = (_member == eGlobalModeGroupMemberRotate);
    std::vector<ColorTrackballHudChip> axisRow;
    const char* axisKeys[3] = { "1", "2", "A" };

    for (int i = 0; i < 3; ++i) {
        ColorTrackballHudChip chip;

        chip.key = QString::fromUtf8(axisKeys[i]);
        if (i == 2) {
            chip.caption = groupText("Both");
            chip.lit = !oneAxis && (_lock == kGlobalModeLockAll);
        } else {
            const bool skew = (_member == eGlobalModeGroupMemberSkew);

            chip.caption = (i == 0) ? (skew ? groupText("Skew X") : groupText("X")) : (skew ? groupText("Skew Y") : groupText("Y"));
            chip.lit = (_lock == i);
        }
        chip.dimmed = oneAxis;
        axisRow.push_back(chip);
    }
    state->chipRows.push_back(axisRow);
    switch (_member) {
    case eGlobalModeGroupMemberTranslate:
        state->rateLine = groupText("Translate: 1 image px per px");
        break;
    case eGlobalModeGroupMemberRotate:
        state->rateLine = groupText("Rotate: 0.25\u00B0 per px  (right/up CCW)");
        break;
    case eGlobalModeGroupMemberScale:
        state->rateLine = groupText("Scale: 0.5% per px, uniform");
        break;
    case eGlobalModeGroupMemberSkew:
        state->rateLine = groupText("Skew: x drag \u2192 Skew X, y drag \u2192 Skew Y");
        break;
    case eGlobalModeGroupMemberCenter:
        state->rateLine = groupText("Center: 1 image px per px");
        break;
    }
} // TransformGroupGlobalModeTarget::fillHudState

NATRON_NAMESPACE_EXIT
