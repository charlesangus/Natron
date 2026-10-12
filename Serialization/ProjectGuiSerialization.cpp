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

#include "Serialization/ProjectGuiSerialization.h"

#include <cmath>

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

bool
storedColorIsNear(const NodeGuiSerialization& node,
                  const float* color)
{
    return color && (std::abs(node.colorR - color[0]) <= 0.05) && (std::abs(node.colorG - color[1]) <= 0.05) && (std::abs(node.colorB - color[2]) <= 0.05);
}

} // namespace

bool
resolveNodeGuiUserColor(const NodeGuiSerialization& node,
                        const float* categoryColor,
                        const float* legacyCategoryColor,
                        float* r,
                        float* g,
                        float* b)
{
    if (node.version >= NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_USER_COLOR) {
        if (!node.hasUserColor) {
            return false;
        }
        *r = node.userColorR;
        *g = node.userColorG;
        *b = node.userColorB;

        return true;
    }
    if (!node.colorWasFound || !categoryColor) {
        return false;
    }
    if ( storedColorIsNear(node, categoryColor) || storedColorIsNear(node, legacyCategoryColor) ) {
        return false;
    }
    *r = node.colorR;
    *g = node.colorG;
    *b = node.colorB;

    return true;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
