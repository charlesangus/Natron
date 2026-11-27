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

#ifndef Engine_Nodes_Geometry_GeoNodeKnobUtils_h
#define Engine_Nodes_Geometry_GeoNodeKnobUtils_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QtCore/QString>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

/** @brief The value of @p knob's first dimension at @p time, or @p fallback once the knob is gone. */
template <typename T>
T
geoKnobValueAt(const std::weak_ptr<Knob<T>>& knob,
               T fallback,
               double time,
               ViewSpec view,
               int dimension = 0)
{
    if (const std::shared_ptr<Knob<T>> locked = knob.lock()) {
        return locked->getValueAtTime(time, dimension, view);
    }

    return fallback;
}

/** @brief Shows or hides @p knob, if it is still there. */
template <typename KnobT>
void
geoSetKnobVisible(const std::weak_ptr<KnobT>& knob,
                  bool visible)
{
    if (const std::shared_ptr<KnobT> locked = knob.lock()) {
        locked->setSecret(!visible);
    }
}

/** @brief A knob of type @p KnobT on @p page, not animated, with the name and tooltip given. */
template <typename KnobT>
std::shared_ptr<KnobT>
geoAddKnob(KnobHolder* holder,
           const KnobPagePtr& page,
           const std::string& label,
           const char* name,
           const std::string& toolTip)
{
    std::shared_ptr<KnobT> knob = AppManager::createKnob<KnobT>(holder, label);
    knob->setName(name);
    knob->setHintToolTip(toolTip);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

inline KnobBoolPtr
geoAddBoolKnob(KnobHolder* holder,
               const KnobPagePtr& page,
               const std::string& label,
               const char* name,
               const std::string& toolTip,
               bool defaultValue)
{
    KnobBoolPtr knob = geoAddKnob<KnobBool>(holder, page, label, name, toolTip);
    knob->setDefaultValue(defaultValue);

    return knob;
}

inline KnobStringPtr
geoAddStringKnob(KnobHolder* holder,
                 const KnobPagePtr& page,
                 const std::string& label,
                 const char* name,
                 const std::string& toolTip,
                 const std::string& defaultValue)
{
    KnobStringPtr knob = geoAddKnob<KnobString>(holder, page, label, name, toolTip);
    knob->setDefaultValue(defaultValue);

    return knob;
}

/** @brief An animatable double knob with a hard minimum and the display range given. */
inline KnobDoublePtr
geoAddDoubleKnob(KnobHolder* holder,
                 const KnobPagePtr& page,
                 const std::string& label,
                 const char* name,
                 const std::string& toolTip,
                 double defaultValue,
                 double minimum,
                 double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);
    knob->setName(name);
    knob->setHintToolTip(toolTip);
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    knob->setDisplayMinimum(minimum);
    knob->setDisplayMaximum(displayMaximum);
    page->addKnob(knob);

    return knob;
}

inline KnobChoicePtr
geoAddChoiceKnob(KnobHolder* holder,
                 const KnobPagePtr& page,
                 const std::string& label,
                 const char* name,
                 const std::string& toolTip,
                 const std::vector<ChoiceOption>& options,
                 int defaultValue)
{
    KnobChoicePtr knob = geoAddKnob<KnobChoice>(holder, page, label, name, toolTip);
    knob->populateChoices(options);
    knob->setDefaultValue(defaultValue);

    return knob;
}

/** @brief The notes one per line, as a node's info shows them. */
inline std::string
geoJoinNotes(const std::vector<std::string>& notes)
{
    std::string text;
    for (const std::string& note : notes) {
        text += (text.empty() ? "" : "\n") + note;
    }

    return text;
}

/** @brief Whether the node info @p current does not already open with the line @p estimate. */
inline bool
geoEstimateIsNews(const std::string& current,
                  const std::string& estimate)
{
    if (current.compare(0, estimate.size(), estimate) != 0) {
        return true;
    }

    return (current.size() != estimate.size()) && (current[estimate.size()] != '\n');
}

/**
 * @brief A callback showing a pre-build estimate as @p node's info, or none in background mode,
 * where nobody sees the info before the build ends. An info already opening with the same
 * estimate is left alone, so a render whose results do not change posts nothing new.
 **/
inline std::function<void(const std::string&)>
geoEstimatePoster(const NodePtr& node)
{
    if (!node || appPTR->isBackground()) {
        return std::function<void(const std::string&)>();
    }

    return [node](const std::string& estimate) {
        QString current;
        int type = eMessageTypeInfo;
        node->getPersistentMessage(&current, &type, false);
        if ((type != eMessageTypeInfo) || geoEstimateIsNews(current.toStdString(), estimate)) {
            node->setQuietInfoMessage(estimate);
        }
    };
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoNodeKnobUtils_h
