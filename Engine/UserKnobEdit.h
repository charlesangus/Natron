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

#ifndef NATRON_ENGINE_USERKNOBEDIT_H
#define NATRON_ENGINE_USERKNOBEDIT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <set>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Serialization/KnobSerialization.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Everything a user can edit on a user knob, page or group.
 *
 * typeName is the knob class's typeNameStatic() and dimension its dimension count, the number of
 * curves of a Parametric knob. parentName is the script name of the page or group holding the
 * knob, and index its position among that parent's children; for a page both are empty and -1.
 * defaults holds one value per dimension, in the knob's own storage type, except for a Choice,
 * whose single default is the ID of a menu entry. extra holds the type-specific properties in the
 * form a saved user knob has them: a Choice's entries, a numeric knob's range, a file's sequence
 * flag, a path's multi-path flag, the channel buttons of a LayerSelect or ChannelSet, a String's
 * display, a Parametric's curves and a Button's script.
 **/
struct UserKnobSpec {
    std::string typeName;
    int dimension;
    std::string name;
    std::string label;
    std::string tooltip;
    bool secret;
    bool newLine;
    bool animates;
    bool evaluatesOnChange;
    std::string parentName;
    int index;
    std::vector<Serialization::KnobValueSerialization> defaults;
    Serialization::KnobExtraSerialization extra;
    bool groupAsTab;
    bool useHostOverlayHandle;

    UserKnobSpec()
        : typeName()
        , dimension(1)
        , name()
        , label()
        , tooltip()
        , secret(false)
        , newLine(true)
        , animates(true)
        , evaluatesOnChange(true)
        , parentName()
        , index(-1)
        , defaults()
        , extra()
        , groupAsTab(false)
        , useHostOverlayHandle(false)
    {
    }

    bool operator==(const UserKnobSpec&) const = default;
};

bool isPageOrGroupType(const std::string& typeName);

/**
 * @brief Int, Double or Color.
 **/
bool isNumericType(const std::string& typeName);

/**
 * @brief wanted made script-friendly, with a number appended when it is taken by a knob of
 * holder, a node of a Group holder or a Python keyword. With numbered set a number is always
 * appended, from 1; otherwise the free name itself comes first, then from 2.
 **/
std::string uniqueKnobName(KnobHolder* holder, const std::string& wanted, bool numbered = false);

/**
 * @brief The spec of a knob as it is now.
 **/
UserKnobSpec specFromKnob(const KnobIPtr& knob);

/**
 * @brief An empty string when spec can be applied to knob on holder (or, for a null knob,
 * created on it), otherwise why not: a name that is empty, not a Python identifier, a Python
 * keyword, the name of another knob of the holder or, on a Group, of a node inside it; a type
 * that cannot be created or that differs from the knob's; a parent that is not a user page or
 * group of the holder; a Choice default that is not one of its entries.
 **/
std::string validateSpec(KnobHolder* holder, const KnobIPtr& knob, const UserKnobSpec& spec);

/**
 * @brief Creates a user knob, page or group from spec, as one undo entry. An empty parentName
 * puts a knob on the holder's User page. Returns null and sets error when the spec does not
 * validate.
 **/
KnobIPtr createUserKnob(KnobHolder* holder, const UserKnobSpec& spec, std::string* error = nullptr);

/**
 * @brief Edits knob in place to match spec, as one undo entry: the knob keeps its values,
 * animation, expressions and links. A rename rewrites the expressions that name the knob. The
 * type and dimension cannot change here (see changeUserKnobType()). Returns false and sets
 * error, changing nothing, when the spec does not validate.
 **/
bool applySpec(KnobHolder* holder, const KnobIPtr& knob, const UserKnobSpec& spec, std::string* error = nullptr);

/**
 * @brief The other knobs whose expressions read knob or that are linked to it, for a caller to
 * confirm a removal with.
 **/
std::list<KnobIPtr> getUserKnobDependents(const KnobIPtr& knob);

/**
 * @brief Removes a user knob (with the children of a page or group) as one undo entry.
 **/
bool deleteUserKnob(KnobHolder* holder, const KnobIPtr& knob);

/**
 * @brief Moves a user knob one step up or down within its parent, as one undo entry.
 **/
bool moveUserKnob(KnobHolder* holder, const KnobIPtr& knob, bool up);

enum UserKnobTypeChangeEnum {
    eUserKnobTypeChangeConverted,
    eUserKnobTypeChangeReset,
    eUserKnobTypeChangeNeedsReset,
    eUserKnobTypeChangeFailed
};

/**
 * @brief Whether a knob of type fromType with fromDimension dimensions converts to toType with
 * toDimension dimensions keeping its values and curves: Int, Double and Color between each other
 * with 1 to 4 dimensions (Color with 3 or 4).
 **/
bool isUserKnobTypeChangeConvertible(const std::string& fromType, int fromDimension, const std::string& toType, int toDimension);

/**
 * @brief Replaces knob by a knob of typeName with dimension dimensions, at the same place and
 * with the same name, as one undo entry. For a convertible pair the values and curves carry
 * over, rounded to integers for an Int; extra dimensions are dropped and new ones take the
 * knob's default. The knob's own expressions and links move to the new knob, and other knobs'
 * expressions and links follow it. Any other change is a reset to the new type's defaults, made
 * only when allowReset is set, otherwise nothing changes and eUserKnobTypeChangeNeedsReset is
 * returned. newKnob is set to the knob now in place.
 **/
UserKnobTypeChangeEnum changeUserKnobType(KnobHolder* holder,
                                          const KnobIPtr& knob,
                                          const std::string& typeName,
                                          int dimension,
                                          bool allowReset,
                                          KnobIPtr* newKnob = nullptr,
                                          std::string* error = nullptr);

/**
 * @brief The given user knobs, groups and pages, with their children, as YAML text in the form
 * a node saves its user pages. A knob or group not on a copied page is written under its page.
 **/
std::string copyUserKnobs(KnobHolder* holder, const std::list<KnobIPtr>& knobs);

/**
 * @brief Creates what copyUserKnobs() wrote, as one undo entry. With no parent each page is
 * created as a new user page; with a page or group of holder as parent, the children of every
 * page go into it from index on (-1 for the end). Names already on holder get a numeric suffix.
 * Values, curves and expressions are pasted as they were; a link to a knob of another node
 * pastes as a link when that knob exists, otherwise as an unlinked copy. Returns the top-level
 * knobs created; sets error and creates nothing when the text cannot be read.
 **/
std::list<KnobIPtr> pasteUserKnobs(KnobHolder* holder,
                                   const std::string& text,
                                   const KnobIPtr& parent = KnobIPtr(),
                                   int index = -1,
                                   std::string* error = nullptr);

/**
 * @brief expression, an expression of listener, with its references to the knob of holder that
 * was named oldName renamed to newName: by the node's script name, by thisNode from the same
 * node, and by thisGroup from a node inside a Group holder.
 **/
std::string renameUserKnobInExpression(KnobHolder* holder,
                                       const KnobIPtr& listener,
                                       const std::string& expression,
                                       const std::string& oldName,
                                       const std::string& newName);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_USERKNOBEDIT_H
