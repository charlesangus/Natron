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

#include "Serialization/Merge/MergeHandler.h"

#include <algorithm>
#include <variant>

#include "Serialization/Merge/MergeRules.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

using MergeDetail::dimensionState;
using MergeDetail::Pick;
using MergeDetail::pick;

namespace {

    typedef std::vector<std::string> Lines;

    // Each line keeps its line break, so a text without a final break merges back without one.
    Lines
    splitLines(const std::string& text)
    {
        Lines lines;
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t end = text.find('\n', start);
            const std::size_t next = (end == std::string::npos) ? text.size() : end + 1;
            lines.push_back(text.substr(start, next - start));
            start = next;
        }

        return lines;
    }

    // For each base line, the index of the other line it is kept as, or -1, from a longest common
    // subsequence. The common head and tail are matched first so the table only spans the edits.
    std::vector<int>
    matchLines(const Lines& base,
               const Lines& other)
    {
        std::vector<int> result(base.size(), -1);
        std::size_t head = 0;
        while ((head < base.size()) && (head < other.size()) && (base[head] == other[head])) {
            result[head] = int(head);
            ++head;
        }
        std::size_t tail = 0;
        while ((tail < base.size() - head) && (tail < other.size() - head) && (base[base.size() - 1 - tail] == other[other.size() - 1 - tail])) {
            result[base.size() - 1 - tail] = int(other.size() - 1 - tail);
            ++tail;
        }
        const std::size_t rows = base.size() - head - tail;
        const std::size_t columns = other.size() - head - tail;
        std::vector<std::vector<int>> lengths(rows + 1, std::vector<int>(columns + 1, 0));
        for (std::size_t i = rows; i-- > 0;) {
            for (std::size_t j = columns; j-- > 0;) {
                if (base[head + i] == other[head + j]) {
                    lengths[i][j] = lengths[i + 1][j + 1] + 1;
                } else {
                    lengths[i][j] = std::max(lengths[i + 1][j], lengths[i][j + 1]);
                }
            }
        }
        std::size_t i = 0;
        std::size_t j = 0;
        while ((i < rows) && (j < columns)) {
            if (base[head + i] == other[head + j]) {
                result[head + i] = int(head + j);
                ++i;
                ++j;
            } else if (lengths[i + 1][j] >= lengths[i][j + 1]) {
                ++i;
            } else {
                ++j;
            }
        }

        return result;
    }

    Lines
    slice(const Lines& lines,
          std::size_t from,
          std::size_t to)
    {
        return Lines(lines.begin() + from, lines.begin() + to);
    }

    std::optional<std::string>
    stringValue(const std::optional<KnobDimensionSerialization>& state,
                const KnobSerialization* knob,
                std::size_t index)
    {
        std::optional<KnobValueSerialization> value;
        if (state) {
            value = state->value;
        } else if (knob && (index < knob->dimensions.size())) {
            value = knob->dimensions[index].defaultValue;
        }
        if (!value) {
            return std::string();
        }
        if (!std::holds_alternative<std::string>(*value)) {
            return std::nullopt;
        }

        return std::get<std::string>(*value);
    }

    KnobDimensionSerialization
    withoutValue(const std::optional<KnobDimensionSerialization>& dimension)
    {
        KnobDimensionSerialization result = dimension ? *dimension : KnobDimensionSerialization();
        result.value.reset();
        result.defaultValue.reset();

        return result;
    }

} // namespace

bool
mergeTextLines(const std::string& base,
               const std::string& mine,
               const std::string& theirs,
               std::string* merged)
{
    const Lines baseLines = splitLines(base);
    const Lines mineLines = splitLines(mine);
    const Lines theirsLines = splitLines(theirs);
    const std::vector<int> inMine = matchLines(baseLines, mineLines);
    const std::vector<int> inTheirs = matchLines(baseLines, theirsLines);

    std::string result;
    std::size_t b = 0;
    std::size_t m = 0;
    std::size_t t = 0;
    for (;;) {
        std::size_t stable = b;
        while ((stable < baseLines.size()) && ((inMine[stable] < 0) || (inTheirs[stable] < 0))) {
            ++stable;
        }
        const bool atEnd = (stable == baseLines.size());
        const std::size_t mineEnd = atEnd ? mineLines.size() : std::size_t(inMine[stable]);
        const std::size_t theirsEnd = atEnd ? theirsLines.size() : std::size_t(inTheirs[stable]);
        const Lines baseHunk = slice(baseLines, b, stable);
        const Lines mineHunk = slice(mineLines, m, mineEnd);
        const Lines theirsHunk = slice(theirsLines, t, theirsEnd);
        const Pick choice = pick(baseHunk, mineHunk, theirsHunk);
        if (choice == Pick::eConflict) {
            return false;
        }
        for (const std::string& line : (choice == Pick::eTheirs) ? theirsHunk : mineHunk) {
            result += line;
        }
        if (atEnd) {
            break;
        }
        result += baseLines[stable];
        b = stable + 1;
        m = mineEnd + 1;
        t = theirsEnd + 1;
    }
    *merged = std::move(result);

    return true;
}

bool
TextKnobMergeHandler::mergeDimension(const KnobSerialization* base,
                                     const KnobSerialization* mine,
                                     const KnobSerialization* theirs,
                                     std::size_t index,
                                     KnobDimensionSerialization* merged,
                                     std::vector<MergeConflict>* conflicts) const
{
    const std::optional<KnobDimensionSerialization> b = dimensionState(base, index);
    const std::optional<KnobDimensionSerialization> m = dimensionState(mine, index);
    const std::optional<KnobDimensionSerialization> t = dimensionState(theirs, index);
    const bool animated = (b && b->curve) || (m && m->curve) || (t && t->curve);
    const Pick rest = animated ? Pick::eConflict : pick(withoutValue(b), withoutValue(m), withoutValue(t));
    if (rest == Pick::eConflict) {
        return ScalarKnobMergeHandler::mergeDimension(base, mine, theirs, index, merged, conflicts);
    }
    const KnobSerialization* frame = mine ? mine : theirs;
    const std::optional<std::string> baseText = stringValue(b, base ? base : frame, index);
    const std::optional<std::string> mineText = stringValue(m, mine ? mine : frame, index);
    const std::optional<std::string> theirsText = stringValue(t, theirs ? theirs : frame, index);
    std::string text;
    if (!baseText || !mineText || !theirsText || !mergeTextLines(*baseText, *mineText, *theirsText, &text)) {
        return false;
    }
    const KnobSerialization* side = (rest == Pick::eTheirs) ? theirs : mine;
    if (side && (index < side->dimensions.size())) {
        *merged = side->dimensions[index];
    }
    merged->value = text;

    return true;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
