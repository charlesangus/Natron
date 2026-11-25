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

#include "PrimSelector.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneOps.h"

NATRON_NAMESPACE_ENTER

void
PrimSelector::create(KnobHolder* holder,
                     const KnobPagePtr& page,
                     const std::string& scriptName,
                     const QString& label,
                     const QString& purpose)
{
    KnobStringPtr knob = AppManager::createKnob<KnobString>(holder, label);

    knob->setName(scriptName);
    knob->setHintToolTip(purpose + QLatin1Char(' ')
                         + EffectInstance::tr("A USD path expression: absolute prim paths separated by spaces, whose names may "
                                              "use * (any run of characters) and ? (any one character), such as /World/Geo/*; "
                                              "// matches at any depth (/World//*, //*Light*); {isa:Mesh} and the other USD "
                                              "predicates filter by prim; + or a space unites, & intersects, - subtracts and "
                                              "~ complements, with parentheses for grouping."));
    knob->setAnimationEnabled(false);
    knob->setPrimPathHint(KnobString::ePrimPathHintPattern);
    page->addKnob(knob);
    _knob = knob;
}

KnobStringPtr
PrimSelector::getKnob() const
{
    return _knob.lock();
}

std::string
PrimSelector::getExpression() const
{
    KnobStringPtr knob = _knob.lock();

    return knob ? knob->getValue() : std::string();
}

bool
PrimSelector::isBlank(const std::string& expression)
{
    return expression.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool
PrimSelector::validate(const std::string& expression,
                       std::string* errorMessage)
{
    PXR_NS::SdfPathExpression parsed;

    return SceneOps::parsePrimExpression(expression, &parsed, errorMessage);
}

bool
PrimSelector::getReach(const std::string& expression,
                       Reach* reach,
                       std::string* errorMessage)
{
    *reach = Reach();
    PXR_NS::SdfPathExpression parsed;
    if (!SceneOps::parsePrimExpression(expression, &parsed, errorMessage)) {
        return false;
    }
    if (parsed.IsEmpty()) {
        return true;
    }

    std::vector<std::pair<PXR_NS::SdfPathExpression::Op, int>> stack;
    bool complemented = false;
    bool literalOnly = true;
    std::vector<std::string> prefixes;
    std::vector<std::string> literals;
    auto onLogic = [&](const std::vector<std::pair<PXR_NS::SdfPathExpression::Op, int>>& ops) {
        stack = ops;
        if (ops.empty()) {
            return;
        }
        const PXR_NS::SdfPathExpression::Op op = ops.back().first;
        if (op == PXR_NS::SdfPathExpression::Complement) {
            complemented = true;
        }
        if ((op != PXR_NS::SdfPathExpression::Union) && (op != PXR_NS::SdfPathExpression::ImpliedUnion)) {
            literalOnly = false;
        }
    };
    auto onReference = [&](const PXR_NS::SdfPathExpression::ExpressionReference&) {
        complemented = true;
    };
    auto onPattern = [&](const PXR_NS::SdfPathExpression::PathPattern& pattern) {
        const bool literal = pattern.GetComponents().empty() && pattern.GetPredicateExprs().empty() && !pattern.IsProperty()
            && pattern.GetPrefix().IsAbsoluteRootOrPrimPath() && !pattern.GetPrefix().IsAbsoluteRootPath();
        if (!literal) {
            literalOnly = false;
        } else {
            literals.push_back(pattern.GetPrefix().GetString());
        }
        // Without a complement, what a difference takes away cannot add a match.
        for (const std::pair<PXR_NS::SdfPathExpression::Op, int>& op : stack) {
            if ((op.first == PXR_NS::SdfPathExpression::Difference) && (op.second == 1)) {
                return;
            }
        }
        const PXR_NS::SdfPath prefix = pattern.GetPrefix().GetPrimPath();
        prefixes.push_back(prefix.IsEmpty() ? std::string("/") : prefix.GetString());
    };
    parsed.WalkWithOpStack(onLogic, onReference, onPattern);
    if (complemented) {
        prefixes.assign(1, std::string("/"));
        literalOnly = false;
    }
    reach->prefixes = prefixes;
    reach->literalOnly = literalOnly;
    if (literalOnly) {
        reach->literals = literals;
    }

    return true;
} // PrimSelector::getReach

NATRON_NAMESPACE_EXIT
