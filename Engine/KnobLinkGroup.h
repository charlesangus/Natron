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

#ifndef Engine_KnobLinkGroup_h
#define Engine_KnobLinkGroup_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One dimension of another knob that shares a link group with a dimension of this knob.
 **/
struct KnobLinkedDimension {
    int dimension;
    KnobIPtr knob;
    int otherDimension;
};

/**
 * @brief A two-way link between knob dimensions. Members have no master: the group holds the
 * current value, animation and expression, and every linked member keeps a copy of them, which
 * KnobHelper updates whenever any linked member changes. A member that unlinks stays in the group,
 * so that it can rejoin it later. The group dissolves when it has a single live member left.
 **/
class KnobLinkGroup {
public:
    struct Member {
        KnobIWPtr knob;

        // Identifies the member from its destructor, once the weak pointer has expired.
        const KnobI* key;
        int dimension;
        bool linked;

        // True if the member joined through Node::linkAllKnobsTo(). Two members that are both node links
        // make their nodes Linked Nodes; a Paste Link member in the same group does not.
        bool nodeLink;
    };

    /**
     * @brief Returns a group whose state is a copy of the given dimension of source, or a null pointer
     * when the knob type cannot hold that state. The source is not added as a member.
     **/
    static KnobLinkGroupPtr create(const KnobIPtr& source);

    ~KnobLinkGroup();

    /**
     * @brief A single-dimension knob of the type of the first member, holding the value and
     * animation of the group. It has no holder.
     **/
    KnobHelperPtr getStateKnob() const;

    std::string getExpression(bool* hasRetVariable) const;
    void setExpression(const std::string& expression, bool hasRetVariable);

    bool isPermanent() const;
    void setPermanent(bool permanent);

    /**
     * @brief True if the member joined the group by linking whole nodes rather than single knobs.
     **/
    bool isMemberNodeLink(const KnobI* knob, int dimension) const;
    void setMemberNodeLink(const KnobI* knob, int dimension, bool nodeLink);

    void addMember(const KnobIPtr& knob, int dimension);

    /**
     * @brief Removes the member and returns the number of live members left.
     **/
    int removeMember(const KnobI* knob, int dimension);

    bool hasMember(const KnobI* knob, int dimension) const;
    bool isMemberLinked(const KnobI* knob, int dimension) const;
    void setMemberLinked(const KnobI* knob, int dimension, bool linked);

    /**
     * @brief Live members, linked or not.
     **/
    std::vector<Member> getMembers() const;
    int getLiveMembersCount() const;

    /**
     * @brief Empties the group and returns the members it had.
     **/
    std::vector<Member> clearMembers();

    /**
     * @brief Returns false if the group is already copying a member's state to the others, in which
     * case the caller is that copy echoing back and must not start another one.
     **/
    bool beginSync();
    void endSync();

private:
    explicit KnobLinkGroup(const KnobHelperPtr& stateKnob);

    mutable QMutex _mutex;
    KnobHelperPtr _stateKnob;
    std::string _expression;
    bool _expressionHasRet;
    bool _permanent;
    std::vector<Member> _members;
    std::atomic<bool> _syncing;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_KnobLinkGroup_h
