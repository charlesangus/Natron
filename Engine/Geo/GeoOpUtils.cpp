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

#include "GeoOpUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

NATRON_NAMESPACE_ENTER

namespace {

template <typename F>
void
visitNumericStorage(GeoStorageEnum storage,
                    F&& f)
{
    switch (storage) {
    case eGeoStorageInt32:
        f(std::int32_t());
        break;
    case eGeoStorageInt64:
        f(std::int64_t());
        break;
    case eGeoStorageFloat32:
        f(float());
        break;
    case eGeoStorageFloat64:
        f(double());
        break;
    case eGeoStorageString:
        break;
    }
}

bool
isFloatStorage(GeoStorageEnum s)
{
    return s == eGeoStorageFloat32 || s == eGeoStorageFloat64;
}

bool
isWideStorage(GeoStorageEnum s)
{
    return s == eGeoStorageInt64 || s == eGeoStorageFloat64;
}

std::vector<std::string_view>
splitTokens(std::string_view text)
{
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace((unsigned char)text[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && !std::isspace((unsigned char)text[i])) {
            ++i;
        }
        if (i > start) {
            tokens.push_back(text.substr(start, i - start));
        }
    }

    return tokens;
}

bool
parseUnsigned(std::string_view text,
              std::size_t* pos,
              GeoIndex* value)
{
    const std::size_t start = *pos;
    GeoIndex v = 0;
    while (*pos < text.size() && std::isdigit((unsigned char)text[*pos])) {
        v = v * 10 + GeoIndex(text[*pos] - '0');
        ++(*pos);
    }
    *value = v;

    return *pos > start;
}

void
selectAllLive(const GeoIndexMap& map,
              GeoElementMask& out)
{
    map.forEachLiveRun([&](GeoIndex /*index*/, GeoOffset offset, GeoOffset count) {
        std::fill_n(out.begin() + std::ptrdiff_t(offset), count, char(1));
    });
}

bool
selectNumbers(const GeoIndexMap& map,
              std::string_view token,
              GeoElementMask& out,
              std::string* error)
{
    std::size_t pos = 0;
    GeoIndex first = 0;
    GeoIndex last = 0;
    GeoIndex take = 1;
    GeoIndex every = 1;
    bool ok = parseUnsigned(token, &pos, &first);
    last = first;
    if (ok && pos < token.size() && token[pos] == '-') {
        ++pos;
        ok = parseUnsigned(token, &pos, &last);
    }
    if (ok && pos < token.size() && token[pos] == ':') {
        ++pos;
        ok = parseUnsigned(token, &pos, &every);
        if (ok && pos < token.size() && token[pos] == ',') {
            ++pos;
            take = every;
            ok = parseUnsigned(token, &pos, &every);
        }
    }
    if (!ok || pos != token.size() || every <= 0 || take <= 0) {
        if (error) {
            *error = "malformed number range \"" + std::string(token) + "\"";
        }

        return false;
    }
    const GeoIndex lo = std::min(first, last);
    const GeoIndex hi = std::min(std::max(first, last), map.size() - 1);
    for (GeoIndex i = lo; i <= hi; ++i) {
        if ((i - lo) % every < take) {
            out[std::size_t(map.offsetFromIndex(i))] = 1;
        }
    }

    return true;
}

template <typename T>
double
componentAsDouble(const GeoAttribute& a,
                  std::size_t offset,
                  int component)
{
    return double(a.get<T>(offset, component));
}

double
numericComponent(const GeoAttribute& a,
                 std::size_t offset,
                 int component)
{
    switch (a.storage()) {
    case eGeoStorageInt32:
        return componentAsDouble<std::int32_t>(a, offset, component);
    case eGeoStorageInt64:
        return componentAsDouble<std::int64_t>(a, offset, component);
    case eGeoStorageFloat32:
        return componentAsDouble<float>(a, offset, component);
    case eGeoStorageFloat64:
        return componentAsDouble<double>(a, offset, component);
    case eGeoStorageString:
        break;
    }

    return 0.;
}

template <typename T>
bool
compareValues(const T& a,
              const T& b,
              GeoCompareOpEnum op)
{
    switch (op) {
    case eGeoCompareEqual:
        return a == b;
    case eGeoCompareNotEqual:
        return a != b;
    case eGeoCompareLess:
        return a < b;
    case eGeoCompareLessEqual:
        return a <= b;
    case eGeoCompareGreater:
        return a > b;
    case eGeoCompareGreaterEqual:
        return a >= b;
    }

    return false;
}

bool
selectByCompare(const GeoDetail& detail,
                GeoAttribClassEnum cls,
                std::string_view token,
                GeoElementMask& out,
                std::string* error)
{
    GeoAttribCompare compare;
    if (!geoParseAttribCompare(token, &compare, error)) {
        return false;
    }
    const GeoIndexMap& map = detail.indexMap(cls);
    if (const GeoAttribute* a = detail.findAttribute(cls, compare.attribute)) {
        map.forEachLive([&](GeoIndex /*index*/, GeoOffset offset) {
            if (geoEvalAttribCompare(*a, std::size_t(offset), compare)) {
                out[std::size_t(offset)] = 1;
            }
        });

        return true;
    }
    if (cls == eGeoAttribClassVertex) {
        if (const GeoAttribute* a = detail.findAttribute(eGeoAttribClassPoint, compare.attribute)) {
            map.forEachLive([&](GeoIndex /*index*/, GeoOffset offset) {
                if (geoEvalAttribCompare(*a, std::size_t(detail.vertexPoint(offset)), compare)) {
                    out[std::size_t(offset)] = 1;
                }
            });
        }
    }

    return true;
}

bool
selectToken(const GeoDetail& detail,
            GeoAttribClassEnum cls,
            std::string_view token,
            GeoElementMask& out,
            std::string* error)
{
    const GeoIndexMap& map = detail.indexMap(cls);
    if (token.empty()) {
        return true;
    }
    if (token == "*") {
        selectAllLive(map, out);

        return true;
    }
    if (token.front() == '@') {
        return selectByCompare(detail, cls, token, out, error);
    }
    if (std::isdigit((unsigned char)token.front())) {
        return selectNumbers(map, token, out, error);
    }
    for (const std::string& name : detail.groupNames(cls)) {
        if (!geoGlobMatch(token, name)) {
            continue;
        }
        if (const GeoElementGroup* group = detail.findGroup(cls, name)) {
            group->forEachLiveMember(map, [&](GeoIndex /*index*/, GeoOffset offset) {
                out[std::size_t(offset)] = 1;
            });
        }
    }

    return true;
}

GeoMatrix4
axisRotation(int axis,
             double degrees)
{
    const double r = degrees * M_PI / 180.;
    const double c = std::cos(r);
    const double s = std::sin(r);
    GeoMatrix4 m = geoMatrixIdentity();
    const int a = (axis + 1) % 3;
    const int b = (axis + 2) % 3;
    m[a * 4 + a] = c;
    m[a * 4 + b] = -s;
    m[b * 4 + a] = s;
    m[b * 4 + b] = c;

    return m;
}

GeoMatrix4
translation(double x,
            double y,
            double z)
{
    GeoMatrix4 m = geoMatrixIdentity();
    m[3] = x;
    m[7] = y;
    m[11] = z;

    return m;
}

} // anonymous namespace

std::vector<std::size_t>
geoLiveOffsets(const GeoIndexMap& map)
{
    std::vector<std::size_t> offsets;
    offsets.reserve(std::size_t(map.size()));
    map.forEachLiveRun([&](GeoIndex /*index*/, GeoOffset offset, GeoOffset count) {
        for (GeoOffset i = 0; i < count; ++i) {
            offsets.push_back(std::size_t(offset + i));
        }
    });

    return offsets;
}

std::vector<std::string_view>
geoSplitPattern(std::string_view pattern)
{
    const std::vector<std::string_view> words = splitTokens(pattern);
    std::vector<std::string_view> tokens;
    tokens.reserve(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::string_view word = words[i];
        const std::size_t at = word.find_first_not_of("^!");
        if ((at == std::string_view::npos) || (word[at] != '@')) {
            tokens.push_back(word);
            continue;
        }
        // Words are views into pattern, so a comparison spread over several is the span from the
        // first word's start to the last one's end.
        const char* const begin = word.data();
        std::size_t last = i;
        auto joined = [&]() {
            return std::string_view(begin, std::size_t(words[last].data() + words[last].size() - begin));
        };
        auto valueOf = [](std::string_view comparison) -> std::optional<std::string_view> {
            const std::size_t op = comparison.find_first_of("=!<>", comparison.find('@') + 1);
            if (op == std::string_view::npos) {
                return std::nullopt;
            }
            const std::size_t value = comparison.find_first_not_of("=!<> \t\r\n", op);

            return value == std::string_view::npos ? std::string_view() : comparison.substr(value);
        };
        auto unclosedQuote = [](std::string_view value) {
            return !value.empty() && ((value.front() == '"') || (value.front() == '\'')) && ((value.size() < 2) || (value.back() != value.front()));
        };
        while (last + 1 < words.size()) {
            const std::optional<std::string_view> value = valueOf(joined());
            if (!value) {
                if (std::string_view("=!<>").find(words[last + 1].front()) == std::string_view::npos) {
                    break;
                }
            } else if (!value->empty() && !unclosedQuote(*value)) {
                break;
            }
            ++last;
        }
        tokens.push_back(joined());
        i = last;
    }

    return tokens;
}

std::vector<std::array<double, 3>>
geoPointPositions(const GeoDetail& detail)
{
    const std::size_t n = std::size_t(detail.pointMap().offsetSize());
    std::vector<std::array<double, 3>> out(n);
    if (n == 0) {
        return out;
    }
    const GeoAttribute& p = detail.position();
    if (p.storage() == eGeoStorageFloat64) {
        p.copyDense<double>(0, n, std::span<double>(out.front().data(), n * 3));
    } else {
        std::vector<float> values(n * 3);
        p.copyDense<float>(0, n, std::span<float>(values));
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = { double(values[i * 3]), double(values[i * 3 + 1]), double(values[i * 3 + 2]) };
        }
    }

    return out;
}

std::vector<GeoOffset>
geoMaskedOffsets(const GeoElementMask& mask)
{
    std::vector<GeoOffset> offsets;
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (mask[i]) {
            offsets.push_back(GeoOffset(i));
        }
    }

    return offsets;
}

bool
geoGlobMatch(std::string_view pattern,
             std::string_view s)
{
    std::size_t p = 0;
    std::size_t i = 0;
    std::size_t starP = std::string_view::npos;
    std::size_t starI = 0;
    while (i < s.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == s[i])) {
            ++p;
            ++i;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starI = i;
        } else if (starP != std::string_view::npos) {
            p = starP + 1;
            i = ++starI;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }

    return p == pattern.size();
}

bool
geoNameMatchesPattern(std::string_view patternList,
                      std::string_view name)
{
    bool matched = false;
    for (std::string_view token : splitTokens(patternList)) {
        if (token.front() == '^') {
            if (geoGlobMatch(token.substr(1), name)) {
                matched = false;
            }
        } else if (geoGlobMatch(token, name)) {
            matched = true;
        }
    }

    return matched;
}

bool
geoParseAttribComponent(std::string_view text,
                        std::string* name,
                        int* component)
{
    std::size_t end = 0;
    while (end < text.size() && text[end] != '.' && text[end] != '[') {
        ++end;
    }
    if (end == 0) {
        return false;
    }
    *name = std::string(text.substr(0, end));
    *component = 0;
    if (end == text.size()) {
        return true;
    }
    const std::string_view suffix = text.substr(end);
    if (suffix.front() == '.') {
        if (suffix.size() != 2) {
            return false;
        }
        switch (suffix[1]) {
        case 'x':
        case 'r':
        case 'u':
            *component = 0;
            return true;
        case 'y':
        case 'g':
        case 'v':
            *component = 1;
            return true;
        case 'z':
        case 'b':
            *component = 2;
            return true;
        case 'w':
        case 'a':
            *component = 3;
            return true;
        default:
            return false;
        }
    }
    if (suffix.back() != ']' || suffix.size() < 3) {
        return false;
    }
    std::size_t pos = 1;
    GeoIndex value = 0;
    if (!parseUnsigned(suffix, &pos, &value) || pos != suffix.size() - 1 || value >= GeoAttribute::kMaxTupleSize) {
        return false;
    }
    *component = int(value);

    return true;
}

bool
geoParseAttribCompare(std::string_view text,
                      GeoAttribCompare* compare,
                      std::string* error)
{
    std::string_view t = text;
    if (!t.empty() && t.front() == '@') {
        t.remove_prefix(1);
    }
    const std::size_t opPos = t.find_first_of("=!<>");
    const auto fail = [&]() {
        if (error) {
            *error = "malformed attribute comparison \"" + std::string(text) + "\"";
        }

        return false;
    };
    if (opPos == std::string_view::npos) {
        return fail();
    }
    if (!geoParseAttribComponent(t.substr(0, opPos), &compare->attribute, &compare->component)) {
        return fail();
    }
    std::string_view rest = t.substr(opPos);
    static const std::pair<std::string_view, GeoCompareOpEnum> kOps[] = {
        { "==", eGeoCompareEqual },
        { "!=", eGeoCompareNotEqual },
        { "<=", eGeoCompareLessEqual },
        { ">=", eGeoCompareGreaterEqual },
        { "=", eGeoCompareEqual },
        { "<", eGeoCompareLess },
        { ">", eGeoCompareGreater },
    };
    for (const auto& op : kOps) {
        if (rest.substr(0, op.first.size()) == op.first) {
            compare->op = op.second;
            rest.remove_prefix(op.first.size());
            while (!rest.empty() && std::isspace((unsigned char)rest.front())) {
                rest.remove_prefix(1);
            }
            while (!rest.empty() && std::isspace((unsigned char)rest.back())) {
                rest.remove_suffix(1);
            }
            if ((rest.size() >= 2) && ((rest.front() == '"') || (rest.front() == '\'')) && (rest.back() == rest.front())) {
                rest = rest.substr(1, rest.size() - 2);
            }
            compare->value = std::string(rest);

            return true;
        }
    }

    return fail();
}

bool
geoEvalAttribCompare(const GeoAttribute& attribute,
                     std::size_t offset,
                     const GeoAttribCompare& compare)
{
    if (attribute.isArray() || compare.component < 0 || compare.component >= attribute.tupleSize()) {
        return false;
    }
    if (attribute.storage() == eGeoStorageString) {
        const std::string_view s = attribute.getString(offset, compare.component);
        if (compare.op == eGeoCompareEqual) {
            return geoGlobMatch(compare.value, s);
        }
        if (compare.op == eGeoCompareNotEqual) {
            return !geoGlobMatch(compare.value, s);
        }

        return compareValues<std::string_view>(s, compare.value, compare.op);
    }
    char* end = nullptr;
    const double value = std::strtod(compare.value.c_str(), &end);
    if (compare.value.empty() || end != compare.value.c_str() + compare.value.size()) {
        return false;
    }

    return compareValues<double>(numericComponent(attribute, offset, compare.component), value, compare.op);
}

bool
geoSelectElements(const GeoDetail& detail,
                  GeoAttribClassEnum cls,
                  std::string_view pattern,
                  GeoElementMask* mask,
                  std::string* error)
{
    const GeoIndexMap& map = detail.indexMap(cls);
    const std::size_t size = std::size_t(map.offsetSize());
    mask->assign(size, 0);
    const std::vector<std::string_view> tokens = geoSplitPattern(pattern);
    if (tokens.empty()) {
        selectAllLive(map, *mask);

        return true;
    }
    GeoElementMask live(size, 0);
    selectAllLive(map, live);
    GeoElementMask tokenMask;
    for (std::string_view token : tokens) {
        const bool remove = token.front() == '^';
        if (remove) {
            token.remove_prefix(1);
        }
        const bool negate = !token.empty() && token.front() == '!';
        if (negate) {
            token.remove_prefix(1);
        }
        tokenMask.assign(size, 0);
        if (!selectToken(detail, cls, token, tokenMask, error)) {
            return false;
        }
        for (std::size_t i = 0; i < size; ++i) {
            const bool hit = negate ? (live[i] && !tokenMask[i]) : bool(tokenMask[i]);
            if (hit) {
                (*mask)[i] = remove ? 0 : 1;
            }
        }
    }

    return true;
}

std::optional<GeoAttributeDesc>
geoPromotedDesc(const GeoAttributeDesc& a,
                const GeoAttributeDesc& b)
{
    if (a.isArray != b.isArray) {
        return std::nullopt;
    }
    if ((a.storage == eGeoStorageString) != (b.storage == eGeoStorageString)) {
        return std::nullopt;
    }
    GeoAttributeDesc out = a;
    out.tupleSize = std::max(a.tupleSize, b.tupleSize);
    if (out.typeInfo == eGeoTypeInfoNone) {
        out.typeInfo = b.typeInfo;
    }
    if (a.storage == eGeoStorageString) {
        return out;
    }
    const bool wide = isWideStorage(a.storage) || isWideStorage(b.storage);
    if (isFloatStorage(a.storage) || isFloatStorage(b.storage)) {
        out.storage = wide ? eGeoStorageFloat64 : eGeoStorageFloat32;
    } else {
        out.storage = wide ? eGeoStorageInt64 : eGeoStorageInt32;
    }

    return out;
}

void
geoCopyAttributeElements(const GeoAttribute& src,
                         std::span<const std::size_t> srcOffsets,
                         GeoAttribute& dst,
                         std::span<const std::size_t> dstOffsets)
{
    const std::size_t n = std::min(srcOffsets.size(), dstOffsets.size());
    srcOffsets = srcOffsets.first(n);
    dstOffsets = dstOffsets.first(n);
    if (n == 0 || src.isArray() != dst.isArray()) {
        return;
    }
    const int srcTuple = src.tupleSize();
    const int dstTuple = dst.tupleSize();
    const int shared = std::min(srcTuple, dstTuple);
    const bool srcString = src.storage() == eGeoStorageString;
    if (srcString != (dst.storage() == eGeoStorageString)) {
        return;
    }

    if (srcString) {
        if (src.isArray()) {
            std::vector<std::string> in;
            std::vector<std::string> out;
            for (std::size_t i = 0; i < n; ++i) {
                src.getStringArray(srcOffsets[i], in);
                const std::size_t tuples = in.size() / std::size_t(srcTuple);
                out.assign(tuples * std::size_t(dstTuple), std::string());
                for (std::size_t t = 0; t < tuples; ++t) {
                    for (int c = 0; c < shared; ++c) {
                        out[t * std::size_t(dstTuple) + std::size_t(c)] = in[t * std::size_t(srcTuple) + std::size_t(c)];
                    }
                }
                dst.setStringArray(dstOffsets[i], out);
            }

            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < shared; ++c) {
                dst.setString(dstOffsets[i], src.getString(srcOffsets[i], c), c);
            }
        }

        return;
    }

    visitNumericStorage(src.storage(), [&](auto srcZero) {
        using S = decltype(srcZero);
        visitNumericStorage(dst.storage(), [&](auto dstZero) {
            using D = decltype(dstZero);
            if (src.isArray()) {
                std::vector<S> in;
                std::vector<D> out;
                for (std::size_t i = 0; i < n; ++i) {
                    src.template getArray<S>(srcOffsets[i], in);
                    const std::size_t tuples = in.size() / std::size_t(srcTuple);
                    out.assign(tuples * std::size_t(dstTuple), D(0));
                    for (std::size_t t = 0; t < tuples; ++t) {
                        for (int c = 0; c < shared; ++c) {
                            out[t * std::size_t(dstTuple) + std::size_t(c)] = static_cast<D>(in[t * std::size_t(srcTuple) + std::size_t(c)]);
                        }
                    }
                    dst.template setArray<D>(dstOffsets[i], std::span<const D>(out));
                }

                return;
            }
            std::vector<S> in(n * std::size_t(srcTuple));
            src.template copyDense<S>(srcOffsets, std::span<S>(in));
            if constexpr (std::is_same_v<S, D>) {
                if (srcTuple == dstTuple) {
                    dst.template scatterDense<D>(dstOffsets, std::span<const D>(in));

                    return;
                }
            }
            std::vector<D> out(n * std::size_t(dstTuple), D(0));
            for (std::size_t i = 0; i < n; ++i) {
                for (int c = 0; c < shared; ++c) {
                    out[i * std::size_t(dstTuple) + std::size_t(c)] = static_cast<D>(in[i * std::size_t(srcTuple) + std::size_t(c)]);
                }
            }
            dst.template scatterDense<D>(dstOffsets, std::span<const D>(out));
        });
    });
}

GeoAttribute&
geoConformAttribute(GeoDetailBuilder& builder,
                    GeoAttribClassEnum cls,
                    const GeoAttributeDesc& desc)
{
    const GeoAttribute* existing = builder.detail().findAttribute(cls, desc.name);
    if (!existing || existing->desc() == desc) {
        return builder.addAttribute(cls, desc);
    }
    const GeoAttribute old = *existing;
    GeoAttribute& conformed = builder.addAttribute(cls, desc);
    if (cls == eGeoAttribClassDetail) {
        const std::size_t zero = 0;
        geoCopyAttributeElements(old, std::span<const std::size_t>(&zero, 1), conformed, std::span<const std::size_t>(&zero, 1));
    } else {
        const std::vector<std::size_t> live = geoLiveOffsets(builder.detail().indexMap(cls));
        geoCopyAttributeElements(old, live, conformed, live);
    }

    return conformed;
}

void
geoFillWithDefaultOf(const GeoAttribute& src,
                     GeoAttribute& dst,
                     std::size_t begin,
                     std::size_t end)
{
    if (begin >= end || src.isArray() || dst.isArray()) {
        return;
    }
    const int srcTuple = src.tupleSize();
    const int dstTuple = dst.tupleSize();
    visitNumericStorage(src.storage(), [&](auto srcZero) {
        using S = decltype(srcZero);
        std::array<S, GeoAttribute::kMaxTupleSize> in {};
        src.template getDefault<S>(std::span<S>(in.data(), std::size_t(srcTuple)));
        visitNumericStorage(dst.storage(), [&](auto dstZero) {
            using D = decltype(dstZero);
            std::array<D, GeoAttribute::kMaxTupleSize> out {};
            for (int c = 0; c < std::min(srcTuple, dstTuple); ++c) {
                out[std::size_t(c)] = static_cast<D>(in[std::size_t(c)]);
            }
            dst.template fill<D>(begin, end, std::span<const D>(out.data(), std::size_t(dstTuple)));
        });
    });
}

void
geoAdoptDefault(const GeoAttribute& src,
                GeoAttribute& dst)
{
    if (dst.size() != 0) {
        geoFillWithDefaultOf(src, dst, 0, dst.size());

        return;
    }
    if (src.isArray() || dst.isArray()) {
        return;
    }
    const int srcTuple = src.tupleSize();
    const int dstTuple = dst.tupleSize();
    visitNumericStorage(src.storage(), [&](auto srcZero) {
        using S = decltype(srcZero);
        std::array<S, GeoAttribute::kMaxTupleSize> in {};
        src.template getDefault<S>(std::span<S>(in.data(), std::size_t(srcTuple)));
        visitNumericStorage(dst.storage(), [&](auto dstZero) {
            using D = decltype(dstZero);
            std::array<D, GeoAttribute::kMaxTupleSize> out {};
            for (int c = 0; c < std::min(srcTuple, dstTuple); ++c) {
                out[std::size_t(c)] = static_cast<D>(in[std::size_t(c)]);
            }
            dst.template setDefault<D>(std::span<const D>(out.data(), std::size_t(dstTuple)));
        });
    });
}

GeoMatrix4
geoComposeTransform(const std::array<double, 3>& translate,
                    const std::array<double, 3>& rotateDegrees,
                    const std::array<double, 3>& scale,
                    const std::array<double, 3>& pivot,
                    GeoRotateOrderEnum order)
{
    static const int kAxes[6][3] = {
        { 0, 1, 2 },
        { 0, 2, 1 },
        { 1, 0, 2 },
        { 1, 2, 0 },
        { 2, 0, 1 },
        { 2, 1, 0 },
    };
    const int o = (order >= eGeoRotateOrderXYZ && order <= eGeoRotateOrderZYX) ? int(order) : 0;
    GeoMatrix4 rotation = geoMatrixIdentity();
    for (int i = 0; i < 3; ++i) {
        const int axis = kAxes[o][i];
        rotation = geoMatrixMultiply(axisRotation(axis, rotateDegrees[std::size_t(axis)]), rotation);
    }
    GeoMatrix4 scaling = geoMatrixIdentity();
    scaling[0] = scale[0];
    scaling[5] = scale[1];
    scaling[10] = scale[2];

    GeoMatrix4 m = translation(translate[0], translate[1], translate[2]);
    m = geoMatrixMultiply(m, translation(pivot[0], pivot[1], pivot[2]));
    m = geoMatrixMultiply(m, rotation);
    m = geoMatrixMultiply(m, scaling);

    return geoMatrixMultiply(m, translation(-pivot[0], -pivot[1], -pivot[2]));
}

NATRON_NAMESPACE_EXIT
