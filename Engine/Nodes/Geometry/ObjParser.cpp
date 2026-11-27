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

#include "ObjParser.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace {

struct Corner {
    std::int64_t point = -1;
    std::int64_t texcoord = -1;
    std::int64_t normal = -1;
};

struct Prim {
    bool polyline = false;
    std::size_t firstCorner = 0;
    std::size_t cornerCount = 0;
    int name = -1;
    std::size_t groupSet = 0;
};

class Reader {
public:
    bool read(std::string_view text, std::string* error);
    void build(GeoDetailBuilder& builder) const;

private:
    bool parseLine(const std::vector<std::string_view>& tokens, std::string* error);
    bool parseNumbers(const std::vector<std::string_view>& tokens, std::vector<double>* out, std::string* error) const;
    bool resolve(std::string_view token, std::size_t count, std::int64_t* index, std::string* error) const;
    bool parseCorner(std::string_view token, Corner* corner, std::string* error) const;
    bool fail(const std::string& what, std::string* error) const;

    std::size_t _line = 0;
    std::vector<std::array<double, 3>> _positions;
    std::vector<std::array<double, 3>> _colours;
    bool _anyColour = false;
    std::vector<std::array<float, 3>> _texcoords;
    std::vector<std::array<float, 3>> _normals;
    std::vector<Corner> _corners;
    std::vector<Prim> _prims;
    std::vector<std::string> _names;
    int _currentName = -1;
    std::vector<std::string> _groupNames;
    std::vector<std::vector<std::size_t>> _groupSets { {} };
};

std::string
sanitizeGroupName(std::string_view name)
{
    std::string out(name);
    for (char& c : out) {
        if (!std::isalnum((unsigned char)c) && c != '_') {
            c = '_';
        }
    }
    if (!out.empty() && std::isdigit((unsigned char)out.front())) {
        out.insert(out.begin(), '_');
    }

    return out;
}

std::vector<std::string_view>
splitWhitespace(std::string_view line)
{
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && !std::isspace((unsigned char)line[i])) {
            ++i;
        }
        if (i > start) {
            tokens.push_back(line.substr(start, i - start));
        }
    }

    return tokens;
}

bool
Reader::fail(const std::string& what,
             std::string* error) const
{
    if (error) {
        *error = "line " + std::to_string(_line) + ": " + what;
    }

    return false;
}

bool
Reader::parseNumbers(const std::vector<std::string_view>& tokens,
                     std::vector<double>* out,
                     std::string* error) const
{
    out->clear();
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string_view t = tokens[i];
        double value = 0.;
        const char* begin = t.data();
        if (!t.empty() && t.front() == '+') {
            ++begin;
        }
        const std::from_chars_result r = std::from_chars(begin, t.data() + t.size(), value);
        if (r.ec != std::errc() || r.ptr != t.data() + t.size()) {
            return fail("\"" + std::string(t) + "\" is not a number", error);
        }
        out->push_back(value);
    }

    return true;
}

bool
Reader::resolve(std::string_view token,
                std::size_t count,
                std::int64_t* index,
                std::string* error) const
{
    std::int64_t value = 0;
    const std::from_chars_result r = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || r.ec != std::errc() || r.ptr != token.data() + token.size() || value == 0) {
        return fail("\"" + std::string(token) + "\" is not an element index", error);
    }
    const std::int64_t resolved = value > 0 ? value - 1 : std::int64_t(count) + value;
    if (resolved < 0 || resolved >= std::int64_t(count)) {
        return fail("index " + std::string(token) + " is out of range", error);
    }
    *index = resolved;

    return true;
}

bool
Reader::parseCorner(std::string_view token,
                    Corner* corner,
                    std::string* error) const
{
    const std::size_t slash1 = token.find('/');
    if (!resolve(token.substr(0, slash1), _positions.size(), &corner->point, error)) {
        return false;
    }
    if (slash1 == std::string_view::npos) {
        return true;
    }
    const std::string_view rest = token.substr(slash1 + 1);
    const std::size_t slash2 = rest.find('/');
    const std::string_view texcoord = rest.substr(0, slash2);
    if (!texcoord.empty() && !resolve(texcoord, _texcoords.size(), &corner->texcoord, error)) {
        return false;
    }
    if (slash2 == std::string_view::npos) {
        return true;
    }
    const std::string_view normal = rest.substr(slash2 + 1);

    return normal.empty() || resolve(normal, _normals.size(), &corner->normal, error);
}

bool
Reader::parseLine(const std::vector<std::string_view>& tokens,
                  std::string* error)
{
    const std::string_view key = tokens.front();
    std::vector<double> numbers;
    if (key == "v") {
        if (!parseNumbers(tokens, &numbers, error)) {
            return false;
        }
        if (numbers.size() < 3) {
            return fail("a vertex needs three coordinates", error);
        }
        _positions.push_back({ numbers[0], numbers[1], numbers[2] });
        const std::size_t colourAt = numbers.size() == 6 ? 3 : numbers.size() == 7 ? 4
                                                                                   : 0;
        _colours.push_back(colourAt ? std::array<double, 3> { numbers[colourAt], numbers[colourAt + 1], numbers[colourAt + 2] }
                                    : std::array<double, 3> { 1., 1., 1. });
        _anyColour = _anyColour || colourAt != 0;
    } else if (key == "vt" || key == "vn") {
        if (!parseNumbers(tokens, &numbers, error)) {
            return false;
        }
        if (numbers.empty() || (key == "vn" && numbers.size() < 3)) {
            return fail("too few values", error);
        }
        std::array<float, 3> value = { 0.f, 0.f, 0.f };
        for (std::size_t i = 0; i < 3 && i < numbers.size(); ++i) {
            value[i] = float(numbers[i]);
        }
        (key == "vt" ? _texcoords : _normals).push_back(value);
    } else if (key == "f" || key == "l") {
        Prim prim;
        prim.polyline = key == "l";
        prim.firstCorner = _corners.size();
        prim.cornerCount = tokens.size() - 1;
        prim.name = _currentName;
        prim.groupSet = _groupSets.size() - 1;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            Corner corner;
            if (!parseCorner(tokens[i], &corner, error)) {
                return false;
            }
            _corners.push_back(corner);
        }
        const GeoPrimitiveTypeEnum type = prim.polyline ? eGeoPrimitivePolyline : eGeoPrimitivePolygon;
        if (GeoOffset(prim.cornerCount) < geoPrimitiveMinVertexCount(type)) {
            _corners.resize(prim.firstCorner);
        } else {
            _prims.push_back(prim);
        }
    } else if (key == "g") {
        std::vector<std::size_t> set;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::string name = sanitizeGroupName(tokens[i]);
            std::size_t id = 0;
            while (id < _groupNames.size() && _groupNames[id] != name) {
                ++id;
            }
            if (id == _groupNames.size()) {
                _groupNames.push_back(name);
            }
            set.push_back(id);
        }
        _groupSets.push_back(std::move(set));
    } else if (key == "o") {
        std::string name;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            name += (i > 1 ? " " : "") + std::string(tokens[i]);
        }
        int id = 0;
        while (id < int(_names.size()) && _names[std::size_t(id)] != name) {
            ++id;
        }
        if (id == int(_names.size())) {
            _names.push_back(name);
        }
        _currentName = id;
    }

    return true;
}

bool
Reader::read(std::string_view text,
             std::string* error)
{
    std::string logical;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++_line;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::size_t hash = line.find('#');
        if (hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        std::size_t last = line.find_last_not_of(" \t");
        if (last != std::string_view::npos && line[last] == '\\') {
            logical.append(line.substr(0, last));
            logical.push_back(' ');
            continue;
        }
        logical.append(line);
        const std::vector<std::string_view> tokens = splitWhitespace(logical);
        if (!tokens.empty() && !parseLine(tokens, error)) {
            return false;
        }
        logical.clear();
    }
    if (!logical.empty()) {
        const std::vector<std::string_view> tokens = splitWhitespace(logical);
        if (!tokens.empty() && !parseLine(tokens, error)) {
            return false;
        }
    }

    return true;
}

void
Reader::build(GeoDetailBuilder& builder) const
{
    if (_positions.empty()) {
        return;
    }
    const GeoOffset firstPoint = builder.appendPoints(GeoIndex(_positions.size()));
    for (std::size_t i = 0; i < _positions.size(); ++i) {
        builder.setPointPosition(firstPoint + GeoOffset(i), _positions[i][0], _positions[i][1], _positions[i][2]);
    }
    if (_anyColour) {
        GeoAttributeDesc desc;
        desc.name = "Cd";
        desc.tupleSize = 3;
        desc.typeInfo = eGeoTypeInfoColour;
        GeoAttribute& cd = builder.addAttribute(eGeoAttribClassPoint, desc);
        for (std::size_t i = 0; i < _colours.size(); ++i) {
            const float c[3] = { float(_colours[i][0]), float(_colours[i][1]), float(_colours[i][2]) };
            cd.setTuple<float>(std::size_t(firstPoint) + i, std::span<const float>(c, 3));
        }
    }
    if (_prims.empty()) {
        return;
    }

    std::vector<GeoOffset> primOffsets(_prims.size());
    for (std::size_t runStart = 0; runStart < _prims.size();) {
        std::size_t runEnd = runStart;
        std::vector<std::int32_t> counts;
        std::vector<GeoOffset> points;
        while (runEnd < _prims.size() && _prims[runEnd].polyline == _prims[runStart].polyline) {
            const Prim& prim = _prims[runEnd];
            counts.push_back(std::int32_t(prim.cornerCount));
            for (std::size_t c = 0; c < prim.cornerCount; ++c) {
                points.push_back(firstPoint + GeoOffset(_corners[prim.firstCorner + c].point));
            }
            ++runEnd;
        }
        const GeoOffset first = builder.appendPrimitives(_prims[runStart].polyline ? eGeoPrimitivePolyline : eGeoPrimitivePolygon, counts, points);
        for (std::size_t i = runStart; i < runEnd; ++i) {
            primOffsets[i] = first + GeoOffset(i - runStart);
        }
        runStart = runEnd;
    }

    bool anyTexcoord = false;
    bool anyNormal = false;
    for (const Corner& c : _corners) {
        anyTexcoord = anyTexcoord || c.texcoord >= 0;
        anyNormal = anyNormal || c.normal >= 0;
    }
    if (anyTexcoord || anyNormal) {
        GeoAttribute* uv = nullptr;
        GeoAttribute* n = nullptr;
        if (anyTexcoord) {
            GeoAttributeDesc desc;
            desc.name = "uv";
            desc.tupleSize = 3;
            uv = &builder.addAttribute(eGeoAttribClassVertex, desc);
        }
        if (anyNormal) {
            GeoAttributeDesc desc;
            desc.name = "N";
            desc.tupleSize = 3;
            desc.typeInfo = eGeoTypeInfoNormal;
            n = &builder.addAttribute(eGeoAttribClassVertex, desc);
        }
        for (std::size_t p = 0; p < _prims.size(); ++p) {
            const GeoVertexRange range = builder.detail().primitiveVertices(primOffsets[p]);
            for (std::size_t c = 0; c < _prims[p].cornerCount; ++c) {
                const Corner& corner = _corners[_prims[p].firstCorner + c];
                const std::size_t vertex = std::size_t(range.start) + c;
                if (uv && corner.texcoord >= 0) {
                    uv->setTuple<float>(vertex, std::span<const float>(_texcoords[std::size_t(corner.texcoord)]));
                }
                if (n && corner.normal >= 0) {
                    n->setTuple<float>(vertex, std::span<const float>(_normals[std::size_t(corner.normal)]));
                }
            }
        }
    }

    if (!_names.empty()) {
        GeoAttributeDesc desc;
        desc.name = "name";
        desc.storage = eGeoStorageString;
        GeoAttribute& name = builder.addAttribute(eGeoAttribClassPrimitive, desc);
        for (std::size_t p = 0; p < _prims.size(); ++p) {
            if (_prims[p].name >= 0) {
                name.setString(std::size_t(primOffsets[p]), _names[std::size_t(_prims[p].name)]);
            }
        }
    }

    std::vector<std::vector<GeoOffset>> members(_groupNames.size());
    for (std::size_t p = 0; p < _prims.size(); ++p) {
        for (std::size_t id : _groupSets[_prims[p].groupSet]) {
            members[id].push_back(primOffsets[p]);
        }
    }
    for (std::size_t id = 0; id < _groupNames.size(); ++id) {
        if (!_groupNames[id].empty()) {
            builder.addGroup(eGeoAttribClassPrimitive, _groupNames[id]).add(members[id]);
        }
    }
}
} // anonymous namespace

bool
ObjParser::parseString(std::string_view text,
                       GeoDetailBuilder& builder,
                       std::string* error)
{
    Reader reader;
    if (!reader.read(text, error)) {
        return false;
    }
    reader.build(builder);

    return true;
}

bool
ObjParser::parseFile(const std::string& path,
                     GeoDetailBuilder& builder,
                     std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open \"" + path + "\"";
        }

        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        if (error) {
            *error = "cannot read \"" + path + "\"";
        }

        return false;
    }
    if (!parseString(text.str(), builder, error)) {
        if (error) {
            *error = path + ", " + *error;
        }

        return false;
    }

    return true;
}

NATRON_NAMESPACE_EXIT
