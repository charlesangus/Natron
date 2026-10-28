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

#include "RFVPatternMatch.h"

#include <cstring>
#include <regex>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RFV {

const char* const kPatternRegexPrefix = "re:";

namespace {

    // Malformed bytes decode to themselves, so a value that is not UTF-8 still matches literally.
    std::u32string
    decodeUtf8(const std::string& s)
    {
        std::u32string out;
        out.reserve(s.size());
        std::size_t i = 0;
        while (i < s.size()) {
            const unsigned char lead = (unsigned char)s[i];
            int extra = 0;
            char32_t cp = lead;
            if ((lead & 0xE0) == 0xC0) {
                extra = 1;
                cp = lead & 0x1F;
            } else if ((lead & 0xF0) == 0xE0) {
                extra = 2;
                cp = lead & 0x0F;
            } else if ((lead & 0xF8) == 0xF0) {
                extra = 3;
                cp = lead & 0x07;
            }
            bool valid = (extra > 0) && (i + extra < s.size());
            for (int k = 1; valid && (k <= extra); ++k) {
                const unsigned char next = (unsigned char)s[i + k];
                if ((next & 0xC0) != 0x80) {
                    valid = false;
                } else {
                    cp = (cp << 6) | (next & 0x3F);
                }
            }
            if (valid) {
                out.push_back(cp);
                i += extra + 1;
            } else {
                out.push_back(lead);
                ++i;
            }
        }

        return out;
    }

    // Whether the glob element at pos matches c; next receives the position after the element.
    bool
    matchesElement(const std::u32string& glob,
                   std::size_t pos,
                   char32_t c,
                   std::size_t* next)
    {
        const char32_t head = glob[pos];
        if (head == U'?') {
            *next = pos + 1;

            return true;
        }
        if (head == U'[') {
            std::size_t first = pos + 1;
            const bool negate = (first < glob.size()) && (glob[first] == U'!');
            if (negate) {
                ++first;
            }
            std::size_t close = first;
            if ((close < glob.size()) && (glob[close] == U']')) {
                ++close;
            }
            while ((close < glob.size()) && (glob[close] != U']')) {
                ++close;
            }
            if (close < glob.size()) {
                bool member = false;
                for (std::size_t i = first; i < close;) {
                    if ((i + 2 < close) && (glob[i + 1] == U'-')) {
                        member = member || ((glob[i] <= c) && (c <= glob[i + 2]));
                        i += 3;
                    } else {
                        member = member || (glob[i] == c);
                        ++i;
                    }
                }
                *next = close + 1;

                return member != negate;
            }
        }
        *next = pos + 1;

        return head == c;
    }

    bool
    matchesGlobDecoded(const std::u32string& value,
                       const std::u32string& glob)
    {
        std::size_t v = 0;
        std::size_t g = 0;
        std::size_t starGlob = std::u32string::npos;
        std::size_t starValue = 0;
        while (v < value.size()) {
            if ((g < glob.size()) && (glob[g] == U'*')) {
                starGlob = g++;
                starValue = v;
                continue;
            }
            std::size_t next = 0;
            if ((g < glob.size()) && matchesElement(glob, g, value[v], &next)) {
                g = next;
                ++v;
                continue;
            }
            // Let the last '*' swallow one more character and retry from just after it.
            if (starGlob != std::u32string::npos) {
                g = starGlob + 1;
                v = ++starValue;
                continue;
            }

            return false;
        }
        while ((g < glob.size()) && (glob[g] == U'*')) {
            ++g;
        }

        return g == glob.size();
    }

} // anonymous namespace

std::vector<std::string>
splitPatternLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = text.find('\n', start);
        std::string line = text.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
        if (!line.empty() && (line.back() == '\r')) {
            line.pop_back();
        }
        lines.push_back(line);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return lines;
}

bool
matchesGlob(const std::string& value,
            const std::string& glob)
{
    return matchesGlobDecoded(decodeUtf8(value), decodeUtf8(glob));
}

int
firstMatch(const std::string& value,
           const std::vector<std::string>& lines,
           std::string* error)
{
    const CompiledPatterns patterns(lines);
    if (error) {
        *error = patterns.getError();
    }

    return patterns.firstMatch(value);
}

CompiledPatterns::CompiledPatterns(const std::vector<std::string>& lines)
    : _lines(lines)
{
    const std::size_t prefixLength = std::strlen(kPatternRegexPrefix);
    _compiled.resize(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        Line& compiled = _compiled[i];
        if (line.empty()) {
            continue;
        }
        if (line.compare(0, prefixLength, kPatternRegexPrefix) != 0) {
            compiled.kind = Line::Kind::Glob;
            compiled.glob = decodeUtf8(line);
            continue;
        }
        // Compiled even past any match, so a broken line is reported wherever it is.
        try {
            compiled.regex = std::regex(line.substr(prefixLength), std::regex::ECMAScript);
            compiled.kind = Line::Kind::Regex;
        } catch (const std::regex_error& e) {
            compiled.kind = Line::Kind::Broken;
            if (_error.empty()) {
                _error = "Input " + std::to_string(i) + ": " + e.what();
            }
        }
    }
}

int
CompiledPatterns::firstMatch(const std::string& value) const
{
    std::u32string decoded;
    bool isDecoded = false;
    for (std::size_t i = 0; i < _compiled.size(); ++i) {
        const Line& line = _compiled[i];
        switch (line.kind) {
        case Line::Kind::Empty:
        case Line::Kind::Broken:
            break;
        case Line::Kind::Glob:
            if (!isDecoded) {
                decoded = decodeUtf8(value);
                isDecoded = true;
            }
            if (matchesGlobDecoded(decoded, line.glob)) {
                return (int)i;
            }
            break;
        case Line::Kind::Regex:
            if (std::regex_match(value, line.regex)) {
                return (int)i;
            }
            break;
        }
    }

    return -1;
}

std::string
patternForLabel(const std::string& label)
{
    const bool regexPrefixed = label.compare(0, std::strlen(kPatternRegexPrefix), kPatternRegexPrefix) == 0;
    if (!regexPrefixed && (label.find_first_of("*?[") == std::string::npos)) {
        return label;
    }
    std::string pattern = kPatternRegexPrefix;
    for (char c : label) {
        if ((c != '\0') && std::strchr("\\^$.|?*+()[]{}", c)) {
            pattern += '\\';
        }
        pattern += c;
    }

    return pattern;
}

} // namespace RFV

NATRON_NAMESPACE_EXIT
