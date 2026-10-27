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

#include "LoopBegin.h"

#include <set>

#include <QtCore/QString>

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

namespace {

bool
isLoopNameIdentifier(const std::string& name)
{
    if (name.empty() || ((name[0] >= '0') && (name[0] <= '9'))) {
        return false;
    }
    for (char c : name) {
        const bool alnum = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9'));
        if (!alnum && (c != '_')) {
            return false;
        }
    }

    return true;
}

std::string
loopNameOf(const NodePtr& node)
{
    const LoopBegin* begin = node ? dynamic_cast<const LoopBegin*>(node->getEffectInstance().get()) : NULL;

    return begin ? begin->getLoopName() : std::string();
}
} // anonymous namespace

std::list<NodePtr>
otherLoopBeginsInGroup(const NodePtr& node)
{
    std::list<NodePtr> others;
    NodeCollectionPtr group = node ? node->getGroup() : NodeCollectionPtr();

    if (!group) {
        return others;
    }
    const NodesList nodes = group->getNodes();
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ((*it != node) && LoopBegin::isLoopBegin(*it)) {
            others.push_back(*it);
        }
    }

    return others;
}

void
refreshLoopMessage(const NodePtr& node,
                   MessageTypeEnum type,
                   const std::string& message,
                   std::string* posted)
{
    if (!node) {
        return;
    }
    QString current;
    int currentType = 0;
    node->getPersistentMessage(&current, &currentType, false);
    if (!message.empty()) {
        if ((current != QString::fromUtf8(message.c_str())) || (currentType != (int)type)) {
            node->setPersistentMessage(type, message);
        }
        *posted = message;

        return;
    }
    // Only our own message is taken back: anything posted since has replaced it.
    if (!posted->empty() && (current == QString::fromUtf8(posted->c_str()))) {
        node->clearPersistentMessage(false);
    }
    posted->clear();
}

LoopBegin::LoopBegin(NodePtr node)
    : NativeEffectBase(node)
{
}

bool
LoopBegin::isLoopBegin(const NodePtr& node)
{
    return node && (node->getPluginID() == PLUGINID_NATRON_LOOPBEGIN);
}

std::string
LoopBegin::getLoopName() const
{
    KnobStringPtr name = _name.lock();

    return name ? name->getValue() : std::string();
}

std::string
LoopBegin::getLoopNameError() const
{
    const std::string name = getLoopName();

    if (!isLoopNameIdentifier(name)) {
        return tr("The loop name \"%1\" is not valid: use letters, digits and underscores, not starting with a digit.")
            .arg(QString::fromUtf8(name.c_str()))
            .toStdString();
    }
    const std::list<NodePtr> others = otherLoopBeginsInGroup(getNode());
    for (std::list<NodePtr>::const_iterator it = others.begin(); it != others.end(); ++it) {
        if (loopNameOf(*it) == name) {
            return tr("Another LoopBegin in this group, %1, is also named \"%2\".")
                .arg(QString::fromUtf8((*it)->getScriptName_mt_safe().c_str()))
                .arg(QString::fromUtf8(name.c_str()))
                .toStdString();
        }
    }

    return std::string();
}

void
LoopBegin::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobStringPtr name = createKnob<KnobString>(tr("Name"));

    name->setName(kLoopBeginParamName);
    name->setAnimationEnabled(false);
    name->setDefaultValue("loop1");
    name->setHintToolTip(tr("The loop's name. A loop nested in this one sees this loop's variables as "
                            "loops.<name>.iteration, loops.<name>.count, loops.<name>.t and "
                            "loops.<name>.frame. Unique within the group."));
    page->addKnob(name);
    _name = name;
}

Route
LoopBegin::route(double time,
                 ViewIdx /*view*/)
{
    return Route::input(0, time);
}

bool
LoopBegin::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    KnobStringPtr name = _name.lock();

    if (!name || (k != name.get())) {
        return false;
    }
    // The LoopEnds below override the loops.<name>.* variables, so their read-sets move.
    invalidateRenderContextReads();
    refreshNameMessage();
    const std::list<NodePtr> others = otherLoopBeginsInGroup(getNode());
    for (std::list<NodePtr>::const_iterator it = others.begin(); it != others.end(); ++it) {
        LoopBegin* other = dynamic_cast<LoopBegin*>((*it)->getEffectInstance().get());
        if (other) {
            other->refreshNameMessage();
        }
    }

    return true;
}

void
LoopBegin::onEffectCreated(bool /*mayCreateFileDialog*/,
                           const CreateNodeArgs& /*args*/)
{
    AppInstancePtr app = getApp();
    KnobStringPtr name = _name.lock();

    // A new or pasted LoopBegin takes the first free loopN; a loaded one keeps its saved name.
    if (name && app && !app->getProject()->isLoadingProject()) {
        std::set<std::string> taken;
        const std::list<NodePtr> others = otherLoopBeginsInGroup(getNode());
        for (std::list<NodePtr>::const_iterator it = others.begin(); it != others.end(); ++it) {
            taken.insert(loopNameOf(*it));
        }
        if (taken.count(name->getValue())) {
            for (int i = 1;; ++i) {
                const std::string candidate = "loop" + std::to_string(i);
                if (!taken.count(candidate)) {
                    name->setValue(candidate);
                    break;
                }
            }
        }
    }
    refreshNameMessage();
}

void
LoopBegin::onKnobsLoaded()
{
    refreshNameMessage();
}

void
LoopBegin::refreshNameMessage()
{
    refreshLoopMessage(getNode(), eMessageTypeError, getLoopNameError(), &_postedMessage);
}

NATRON_NAMESPACE_EXIT
