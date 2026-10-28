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

#ifndef Engine_Nodes_Metadata_MetadataText_h
#define Engine_Nodes_Metadata_MetadataText_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/Nodes/Metadata/ImageMetadata.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Text forms of metadata values, shared by the nodes that display metadata.
 **/
namespace MetadataText {
/**
 * @brief The text escaped for a Qt rich-text label, newlines turned into line breaks.
 **/
std::string escapeHtml(const std::string& text);

/**
 * @brief The value as plain text: an int or a double as written by std::to_chars (shortest
 * round-trip form), a string as is, and a vector as its elements joined by ", ".
 **/
std::string formatValuePlain(const ImageMetadata::Value& value);

/**
 * @brief formatValuePlain(), escaped for a rich-text label.
 **/
std::string formatValueHtml(const ImageMetadata::Value& value);
} // namespace MetadataText

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_MetadataText_h
