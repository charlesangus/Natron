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

NATRON_NAMESPACE_EXIT
