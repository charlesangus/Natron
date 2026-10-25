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

#include "KnobLinkGroup.h"

#include <algorithm>

#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobFactory.h"

NATRON_NAMESPACE_ENTER

KnobLinkGroupPtr
KnobLinkGroup::create(const KnobIPtr& source)
{
    if (!source) {
        return KnobLinkGroupPtr();
    }
    KnobHelperPtr stateKnob = appPTR->getKnobFactory().createKnob(source->typeName(), NULL, source->getName(), 1, false);
    if (!stateKnob) {
        return KnobLinkGroupPtr();
    }

    return KnobLinkGroupPtr(new KnobLinkGroup(stateKnob));
}

KnobLinkGroup::KnobLinkGroup(const KnobHelperPtr& stateKnob)
    : _mutex()
    , _stateKnob(stateKnob)
    , _expression()
    , _expressionHasRet(false)
    , _permanent(false)
    , _members()
    , _syncing(false)
{
}

KnobLinkGroup::~KnobLinkGroup()
{
}

KnobHelperPtr
KnobLinkGroup::getStateKnob() const
{
    return _stateKnob;
}

std::string
KnobLinkGroup::getExpression(bool* hasRetVariable) const
{
    QMutexLocker k(&_mutex);

    *hasRetVariable = _expressionHasRet;

    return _expression;
}

void
KnobLinkGroup::setExpression(const std::string& expression,
                             bool hasRetVariable)
{
    QMutexLocker k(&_mutex);

    _expression = expression;
    _expressionHasRet = hasRetVariable;
}

bool
KnobLinkGroup::isPermanent() const
{
    QMutexLocker k(&_mutex);

    return _permanent;
}

void
KnobLinkGroup::setPermanent(bool permanent)
{
    QMutexLocker k(&_mutex);

    _permanent = permanent;
}

bool
KnobLinkGroup::isMemberNodeLink(const KnobI* knob,
                                int dimension) const
{
    QMutexLocker k(&_mutex);

    for (std::vector<Member>::const_iterator it = _members.begin(); it != _members.end(); ++it) {
        if ((it->key == knob) && (it->dimension == dimension)) {
            return it->nodeLink;
        }
    }

    return false;
}

void
KnobLinkGroup::setMemberNodeLink(const KnobI* knob,
                                 int dimension,
                                 bool nodeLink)
{
    QMutexLocker k(&_mutex);

    for (std::vector<Member>::iterator it = _members.begin(); it != _members.end(); ++it) {
        if ((it->key == knob) && (it->dimension == dimension)) {
            it->nodeLink = nodeLink;

            return;
        }
    }
}

void
KnobLinkGroup::addMember(const KnobIPtr& knob,
                         int dimension)
{
    QMutexLocker k(&_mutex);

    for (std::vector<Member>::iterator it = _members.begin(); it != _members.end(); ++it) {
        if ((it->key == knob.get()) && (it->dimension == dimension)) {
            it->linked = true;

            return;
        }
    }
    Member m;
    m.knob = knob;
    m.key = knob.get();
    m.dimension = dimension;
    m.linked = true;
    m.nodeLink = false;
    _members.push_back(m);
}

int
KnobLinkGroup::removeMember(const KnobI* knob,
                            int dimension)
{
    QMutexLocker k(&_mutex);
    int nLive = 0;

    for (std::vector<Member>::iterator it = _members.begin(); it != _members.end();) {
        if ((it->key == knob) && (it->dimension == dimension)) {
            it = _members.erase(it);
        } else {
            if (!it->knob.expired()) {
                ++nLive;
            }
            ++it;
        }
    }

    return nLive;
}

bool
KnobLinkGroup::hasMember(const KnobI* knob,
                         int dimension) const
{
    QMutexLocker k(&_mutex);

    for (std::vector<Member>::const_iterator it = _members.begin(); it != _members.end(); ++it) {
        if ((it->key == knob) && (it->dimension == dimension)) {
            return true;
        }
    }

    return false;
}

bool
KnobLinkGroup::isMemberLinked(const KnobI* knob,
                              int dimension) const
{
    QMutexLocker k(&_mutex);

    for (std::vector<Member>::const_iterator it = _members.begin(); it != _members.end(); ++it) {
        if ((it->key == knob) && (it->dimension == dimension)) {
            return it->linked;
        }
    }

    return false;
}

void
KnobLinkGroup::setMemberLinked(const KnobI* knob,
                               int dimension,
                               bool linked)
{
    QMutexLocker k(&_mutex);

    for (std::vector<Member>::iterator it = _members.begin(); it != _members.end(); ++it) {
        if ((it->key == knob) && (it->dimension == dimension)) {
            it->linked = linked;

            return;
        }
    }
}

std::vector<KnobLinkGroup::Member>
KnobLinkGroup::getMembers() const
{
    QMutexLocker k(&_mutex);
    std::vector<Member> ret;

    for (std::vector<Member>::const_iterator it = _members.begin(); it != _members.end(); ++it) {
        if (!it->knob.expired()) {
            ret.push_back(*it);
        }
    }

    return ret;
}

int
KnobLinkGroup::getLiveMembersCount() const
{
    QMutexLocker k(&_mutex);

    return (int)std::count_if(_members.begin(), _members.end(), [](const Member& m) { return !m.knob.expired(); });
}

std::vector<KnobLinkGroup::Member>
KnobLinkGroup::clearMembers()
{
    QMutexLocker k(&_mutex);
    std::vector<Member> ret;

    ret.swap(_members);

    return ret;
}

bool
KnobLinkGroup::beginSync()
{
    bool expected = false;

    return _syncing.compare_exchange_strong(expected, true);
}

void
KnobLinkGroup::endSync()
{
    _syncing = false;
}

NATRON_NAMESPACE_EXIT
