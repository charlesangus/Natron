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

#include "MetadataText.h"

#include <charconv>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <QString>

NATRON_NAMESPACE_ENTER

namespace {
std::string
formatDouble(double value)
{
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);

    return std::string(buffer, result.ptr);
}

template <typename T, typename Format>
std::string
joinValues(const std::vector<T>& values,
           Format format)
{
    std::string joined;

    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += format(values[i]);
    }

    return joined;
}
} // namespace

namespace MetadataText {
std::string
escapeHtml(const std::string& text)
{
    QString escaped = QString::fromStdString(text).toHtmlEscaped();

    escaped.replace(QLatin1Char('\n'), QString::fromUtf8("<br />"));

    return escaped.toStdString();
}

std::string
formatValuePlain(const ImageMetadata::Value& value)
{
    switch (value.index()) {
    case 0:

        return std::to_string(std::get<int>(value));
    case 1:

        return formatDouble(std::get<double>(value));
    case 2:

        return std::get<std::string>(value);
    case 3:

        return joinValues(std::get<std::vector<int>>(value), [](int v) { return std::to_string(v); });
    default:

        return joinValues(std::get<std::vector<double>>(value), [](double v) { return formatDouble(v); });
    }
}

std::string
formatValueHtml(const ImageMetadata::Value& value)
{
    return escapeHtml(formatValuePlain(value));
}
} // namespace MetadataText

NATRON_NAMESPACE_EXIT
