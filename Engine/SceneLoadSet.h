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

#ifndef NATRON_ENGINE_SCENELOADSET_H
#define NATRON_ENGINE_SCENELOADSET_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Which payloads a scene is evaluated with, as rules on prim paths that mirror OpenUSD's
 * stage load rules: the rule of the nearest path at or above a prim decides whether its payload
 * is included. With no rules at all, every payload is. Kept free of OpenUSD so that evaluation
 * arguments can carry it; SceneOps converts it to and from UsdStageLoadRules.
 **/
class SceneLoadSet {
public:
    enum RuleEnum {
        // The payload at the path and every one below it.
        eRuleAll,
        // The payload at the path, none below it.
        eRuleOnly,
        // No payload at or below the path.
        eRuleNone
    };

    struct Entry {
        std::string path;
        RuleEnum rule;

        bool operator==(const Entry& other) const
        {
            return (path == other.path) && (rule == other.rule);
        }

        bool operator!=(const Entry& other) const
        {
            return !(*this == other);
        }
    };

    // Every payload included.
    SceneLoadSet();

    // Sorted by path; of two entries for one path, the later one is kept.
    explicit SceneLoadSet(const std::vector<Entry>& entries);

    // No payload included.
    static SceneLoadSet none();

    // Every payload included, named so a borrower that needs the whole scene says so.
    static std::shared_ptr<const SceneLoadSet> everything();

    const std::vector<Entry>& getEntries() const;

    /**
     * @brief base with the payload of every prim on the way to each of paths, and of the paths
     * themselves, included and nothing below them. Null, as an empty set, includes everything
     * already and comes back as it is.
     **/
    static std::shared_ptr<const SceneLoadSet> withPaths(const std::shared_ptr<const SceneLoadSet>& base,
                                                         const std::vector<std::string>& paths);

    /**
     * @brief The set including a prim's payload exactly when a or b does, the payload at a path
     * and those below it decided apart (an eRuleOnly includes the one, not the others). Null when
     * either includes everything.
     **/
    static std::shared_ptr<const SceneLoadSet> unite(const std::shared_ptr<const SceneLoadSet>& a,
                                                     const std::shared_ptr<const SceneLoadSet>& b);

    /**
     * @brief The rule one entry at path would need to include what this set includes at path and
     * below it where no entry below path decides: the rule of the nearest entry at or above path,
     * eRuleNone for an eRuleOnly above it, eRuleAll with no entries.
     **/
    RuleEnum getRuleAt(const std::string& path) const;

    /**
     * @brief entries as a load set without the entries that change what it includes nowhere,
     * so that sets differing only by those compare equal. Of two entries for one path, the later
     * one is kept. Null when it includes everything.
     **/
    static std::shared_ptr<const SceneLoadSet> minimal(const std::vector<Entry>& entries);

    U64 getHash() const;

    bool operator==(const SceneLoadSet& other) const;

    bool operator!=(const SceneLoadSet& other) const
    {
        return !(*this == other);
    }

    /**
     * @brief The paths whose rule is not the same in a and b, an entry in one and not the other
     * included, sorted. A null set includes every payload, as an empty one does.
     **/
    static std::vector<std::string> getChangedPaths(const SceneLoadSet* a,
                                                    const SceneLoadSet* b);

private:
    std::vector<Entry> _entries;
    U64 _hash;
};

// Null for an evaluation that includes every payload.
typedef std::shared_ptr<const SceneLoadSet> SceneLoadSetPtr;

/**
 * @brief Where the prim patterns an evaluation matched can select prims, as the literal leading
 * paths PrimSelector::getReach() gives widened to the payloads above them
 * (SceneOps::getLoadReach()), gathered while the evaluation runs. A load set change
 * that reaches none of them (SceneOps::loadChangeReaches()) leaves that evaluation's scene as it
 * is. Empty when no pattern was matched against the loaded payloads. MT-safe.
 **/
class SceneLoadReach {
public:
    void add(const std::vector<std::string>& prefixes);

    std::vector<std::string> getPrefixes() const;

private:
    mutable std::mutex _mutex;
    std::vector<std::string> _prefixes;
};

typedef std::shared_ptr<SceneLoadReach> SceneLoadReachPtr;

/**
 * @brief Who a scene evaluation is for. A viewer evaluates with its own id and the load set of
 * its stage; renders, writes and scripts use the default, id 0 with every payload included.
 * Nodes keep their memoized layers per id, so two evaluators do not evict each other.
 **/
struct SceneEvaluationContext {
    U64 id;
    SceneLoadSetPtr loadSet;
    // Gathers the reach of the patterns the evaluation matches, when set.
    SceneLoadReachPtr reach;

    SceneEvaluationContext()
        : id(0)
        , loadSet()
        , reach()
    {
    }

    SceneEvaluationContext(U64 id_,
                           const SceneLoadSetPtr& loadSet_,
                           const SceneLoadReachPtr& reach_ = SceneLoadReachPtr())
        : id(id_)
        , loadSet(loadSet_)
        , reach(reach_)
    {
    }

    bool includesEverything() const
    {
        return !loadSet || loadSet->getEntries().empty();
    }

    // Whether loadSet includes the same payloads as other's, null and empty sets being one.
    bool hasSameLoadSet(const SceneEvaluationContext& other) const
    {
        if (loadSet == other.loadSet) {
            return true;
        }
        if (includesEverything() || other.includesEverything()) {
            return includesEverything() && other.includesEverything();
        }

        return *loadSet == *other.loadSet;
    }

    // A new id, never 0 and never handed out before in this process.
    static U64 makeId();

    // The context of the evaluation running on the calling thread, the default outside one.
    static const SceneEvaluationContext& current();

    class Scope;
};

// Makes context the calling thread's current one for the scope's life.
class SceneEvaluationContext::Scope {
public:
    explicit Scope(const SceneEvaluationContext& context);
    ~Scope();

private:
    const SceneEvaluationContext* _previous;
    SceneEvaluationContext _context;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SCENELOADSET_H
