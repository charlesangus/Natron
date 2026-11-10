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

#ifndef Engine_Nodes_Scene_PrimSelector_h
#define Engine_Nodes_Scene_PrimSelector_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include <QString>

#include "Engine/EngineFwd.h"
#include "Global/GlobalDefines.h"

// Free of USD, like the node headers that include it. The selection itself is
// SceneOps::selectPrims().

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob a scene node names the prims it acts on with: a non-animatable string holding a
 * USD path expression (SceneOps::parsePrimExpression()), such as /World/Box, /World/*,
 * /World/Geo//* or "/World/Box /World/Cam". The node evaluates it against its input's composed
 * stage with SceneOps::selectPrims(getExpression()).
 **/
class PrimSelector {
public:
    /**
     * @brief Creates the knob on page. purpose opens its tooltip, which goes on to explain the
     * expression syntax.
     **/
    void create(KnobHolder* holder,
                const KnobPagePtr& page,
                const std::string& scriptName,
                const QString& label,
                const QString& purpose);

    KnobStringPtr getKnob() const;

    // The knob's value, or empty before create().
    std::string getExpression() const;

    // Whether expression is blank, which selects no prim.
    static bool isBlank(const std::string& expression);

    /**
     * @brief Whether expression parses, without a stage to evaluate it on, so a node can report a
     * bad expression even while its input is empty. Returns false with *errorMessage naming the
     * failure.
     **/
    static bool validate(const std::string& expression,
                         std::string* errorMessage);

    struct Reach {
        // The literal leading paths of the patterns that can add a match, "/" for one with none.
        std::vector<std::string> prefixes;
        // Whether the expression is a union of plain prim paths, which then are in literals.
        bool literalOnly = false;
        std::vector<std::string> literals;
    };

    /**
     * @brief Where expression can select prims, from its text alone: no prim outside every one of
     * the prefixes is ever selected. The patterns a difference takes away add no prefix; an
     * expression holding a complement, or a pattern such as //*Light*, reaches everywhere. A
     * union of plain paths selects those paths wherever they exist, so it needs no query. Returns
     * false with *errorMessage set when expression does not parse.
     **/
    static bool getReach(const std::string& expression,
                         Reach* reach,
                         std::string* errorMessage);

private:
    KnobStringWPtr _knob;
};

/**
 * @brief What a node's prim pattern last selected in each evaluation context, a few contexts at
 * once, so that an evaluation whose input is the same and whose load set changed only where the
 * pattern cannot reach (PrimSelector::getReach()) takes those matches without querying a stage.
 * SceneNodeAuthoring::matchPattern() fills and reads it.
 **/
class PrimPatternCache {
public:
    PrimPatternCache();

    ~PrimPatternCache();

    // Forgets every context's matches.
    void invalidate();

    // How many times the matches were looked for on a stage rather than taken from the cache.
    U64 getQueryCount() const;

    // SceneNodeAuthoring's view of the cache; defined in SceneNodeAuthoring.cpp.
    struct State;
    State& getState();

private:
    std::unique_ptr<State> _state;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_PrimSelector_h
