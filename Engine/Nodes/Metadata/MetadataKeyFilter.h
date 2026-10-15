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

#ifndef Engine_Nodes_Metadata_MetadataKeyFilter_h
#define Engine_Nodes_Metadata_MetadataKeyFilter_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <QRegularExpression>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A list of glob patterns selecting metadata keys. The list is comma- and/or
 * newline-separated, each entry trimmed of whitespace; '*' matches any run of characters,
 * including '/', and '?' matches exactly one. An empty list matches every key.
 **/
class MetadataKeyFilter {
public:
    explicit MetadataKeyFilter(const std::string& globList);

    bool matchesAll() const
    {
        return _patterns.empty();
    }

    bool matches(const std::string& key) const;

private:
    std::vector<QRegularExpression> _patterns;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_MetadataKeyFilter_h
