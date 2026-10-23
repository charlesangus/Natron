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

#include "Serialization/Yaml/YamlScalars.h"

#include <array>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <system_error>
#include <type_traits>

#include <locale.h>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    constexpr std::array<std::string_view, 9> kInterpolationTokens = { "K", "L", "S", "R", "C", "H", "F", "X", "N" };

    template <typename T>
    std::string
    formatFloatingPoint(T value)
    {
        if (std::isnan(value)) {
            return ".nan";
        }
        if (std::isinf(value)) {
            return value < 0 ? "-.inf" : ".inf";
        }
        std::array<char, 64> buffer;
        std::to_chars_result result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (result.ec != std::errc()) {
            throw YamlError("could not format a floating point value");
        }

        return std::string(buffer.data(), result.ptr);
    }

    template <typename T>
    std::optional<T>
    parseFloatingPoint(std::string_view text)
    {
        if (!text.empty() && (text.front() == '+')) {
            text.remove_prefix(1);
            if (!text.empty() && (text.front() == '-')) {
                return std::nullopt;
            }
        }
        if ((text == ".inf") || (text == ".Inf") || (text == ".INF")) {
            return std::numeric_limits<T>::infinity();
        }
        if ((text == "-.inf") || (text == "-.Inf") || (text == "-.INF")) {
            return -std::numeric_limits<T>::infinity();
        }
        if ((text == ".nan") || (text == ".NaN") || (text == ".NAN")) {
            return std::numeric_limits<T>::quiet_NaN();
        }
        if (text.empty()) {
            return std::nullopt;
        }
        // from_chars would read "inf" and "nan" too; YAML spells those with a leading dot.
        const char first = (text.front() == '-') && (text.size() > 1) ? text[1] : text.front();
        if (!(((first >= '0') && (first <= '9')) || (first == '.'))) {
            return std::nullopt;
        }
        T value = T();
        std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ptr != text.data() + text.size()) {
            return std::nullopt;
        }
        if (result.ec == std::errc::result_out_of_range) {
            // libstdc++ reports subnormal results as out of range and leaves value unset, though
            // they are exact round-trips of what formatFloatingPoint writes; strtod_l in the C
            // locale gives the correctly rounded value.
            static const locale_t cLocale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
            const std::string copy(text);
            char* end = nullptr;
            if constexpr (std::is_same_v<T, float>) {
                value = strtof_l(copy.c_str(), &end, cLocale);
            } else {
                value = strtod_l(copy.c_str(), &end, cLocale);
            }
            if ((end != copy.c_str() + copy.size()) || std::isinf(value)) {
                return std::nullopt;
            }

            return value;
        }
        if (result.ec != std::errc()) {
            return std::nullopt;
        }

        return value;
    }

} // namespace

std::string
formatYamlDouble(double value)
{
    return formatFloatingPoint(value);
}

std::string
formatYamlFloat(float value)
{
    return formatFloatingPoint(value);
}

std::optional<double>
parseYamlDouble(std::string_view text)
{
    return parseFloatingPoint<double>(text);
}

std::optional<float>
parseYamlFloat(std::string_view text)
{
    return parseFloatingPoint<float>(text);
}

std::optional<std::int64_t>
parseYamlInteger(std::string_view text)
{
    if (!text.empty() && (text.front() == '+')) {
        text.remove_prefix(1);
    }
    if (text.empty() || (text.front() == '+')) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
    if ((result.ec != std::errc()) || (result.ptr != text.data() + text.size())) {
        return std::nullopt;
    }

    return value;
}

std::string_view
keyframeInterpolationToken(int interpolation)
{
    if ((interpolation < 0) || (interpolation >= (int)kInterpolationTokens.size())) {
        throw YamlError("a keyframe has the unknown interpolation " + std::to_string(interpolation));
    }

    return kInterpolationTokens[interpolation];
}

std::optional<int>
keyframeInterpolationFromToken(std::string_view token)
{
    for (std::size_t i = 0; i < kInterpolationTokens.size(); ++i) {
        if (kInterpolationTokens[i] == token) {
            return (int)i;
        }
    }

    return std::nullopt;
}

bool
isXmlDocument(std::string_view text)
{
    static constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
    if (text.substr(0, kByteOrderMark.size()) == kByteOrderMark) {
        text.remove_prefix(kByteOrderMark.size());
    }
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return false;
    }
    text.remove_prefix(start);

    return (text.substr(0, 5) == "<?xml") || (text.substr(0, 9) == "<!DOCTYPE");
}

namespace YamlDetail {

    namespace {

        bool
        isReservedPlainWord(std::string_view text)
        {
            static const std::array<std::string_view, 9> kWords = { "true", "false", "null", "yes", "no", "on", "off", "y", "n" };
            for (std::string_view word : kWords) {
                if (text.size() != word.size()) {
                    continue;
                }
                bool same = true;
                for (std::size_t i = 0; i < word.size(); ++i) {
                    char c = text[i];
                    if ((c >= 'A') && (c <= 'Z')) {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                    if (c != word[i]) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    return true;
                }
            }

            return false;
        }

        bool
        isSafePlainKey(std::string_view key)
        {
            if (key.empty() || isReservedPlainWord(key)) {
                return false;
            }
            const char first = key.front();
            if (!(((first >= 'A') && (first <= 'Z')) || ((first >= 'a') && (first <= 'z')) || (first == '_'))) {
                return false;
            }
            for (char c : key) {
                const bool ok = ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_') || (c == '.') || (c == '-') || (c == '/');
                if (!ok) {
                    return false;
                }
            }

            return true;
        }

        std::string
        scalarText(const YAML::Node& node,
                   const std::string& context,
                   const char* expected)
        {
            if (!node || !node.IsScalar()) {
                fail(node, context, std::string("expected ") + expected);
            }

            return node.Scalar();
        }

    } // namespace

    void
    emitKey(YAML::Emitter& em,
            std::string_view key)
    {
        em << YAML::Key;
        if (isSafePlainKey(key)) {
            em << std::string(key);
        } else {
            em << YAML::DoubleQuoted << std::string(key);
        }
    }

    void
    emitString(YAML::Emitter& em,
               std::string_view value)
    {
        em << YAML::DoubleQuoted << std::string(value);
    }

    void
    emitDouble(YAML::Emitter& em,
               double value)
    {
        em << formatYamlDouble(value);
    }

    void
    emitFloat(YAML::Emitter& em,
              float value)
    {
        em << formatYamlFloat(value);
    }

    void
    emitDoublePair(YAML::Emitter& em,
                   std::string_view key,
                   double a,
                   double b)
    {
        emitKey(em, key);
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitDouble(em, a);
        emitDouble(em, b);
        em << YAML::EndSeq;
    }

    void
    emitFloatTriple(YAML::Emitter& em,
                    std::string_view key,
                    float r,
                    float g,
                    float b)
    {
        emitKey(em, key);
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitFloat(em, r);
        emitFloat(em, g);
        emitFloat(em, b);
        em << YAML::EndSeq;
    }

    std::vector<float>
    readFloatTriple(const YAML::Node& node,
                    const std::string& context)
    {
        if (!node.IsSequence() || (node.size() != 3)) {
            fail(node, context, "expected [r, g, b]");
        }

        return { readFloat(node[0], context), readFloat(node[1], context), readFloat(node[2], context) };
    }

    void
    emitInt(YAML::Emitter& em,
            long long value)
    {
        em << std::to_string(value);
    }

    void
    emitBool(YAML::Emitter& em,
             bool value)
    {
        em << (value ? "true" : "false");
    }

    void
    emitStringList(YAML::Emitter& em,
                   const std::vector<std::string>& values)
    {
        em << YAML::Flow << YAML::BeginSeq;
        for (const std::string& value : values) {
            emitString(em, value);
        }
        em << YAML::EndSeq;
    }

    void
    emitStringList(YAML::Emitter& em,
                   const std::list<std::string>& values)
    {
        em << YAML::Flow << YAML::BeginSeq;
        for (const std::string& value : values) {
            emitString(em, value);
        }
        em << YAML::EndSeq;
    }

    std::string
    documentText(const YAML::Emitter& em)
    {
        if (!em.good()) {
            throw YamlError("could not write the YAML document: " + em.GetLastError());
        }
        std::string text(em.c_str(), em.size());
        if (text.empty() || (text.back() != '\n')) {
            text.push_back('\n');
        }

        return text;
    }

    YAML::Node
    loadDocument(std::string_view text,
                 std::string_view what)
    {
        try {
            return YAML::Load(std::string(text));
        } catch (const YAML::Exception& e) {
            throw YamlError(std::string(what) + " is not valid YAML: " + e.what());
        }
    }

    std::string
    where(const YAML::Node& node)
    {
        if (!node) {
            return std::string();
        }
        const YAML::Mark mark = node.Mark();
        if (mark.is_null()) {
            return std::string();
        }

        return " (line " + std::to_string(mark.line + 1) + ", column " + std::to_string(mark.column + 1) + ")";
    }

    void
    fail(const YAML::Node& node,
         const std::string& context,
         const std::string& message)
    {
        throw YamlError(context + ": " + message + where(node));
    }

    bool
    isPlainScalar(const YAML::Node& node)
    {
        return node && node.IsScalar() && (node.Tag() == "?");
    }

    double
    readDouble(const YAML::Node& node,
               const std::string& context)
    {
        const std::string text = scalarText(node, context, "a number");
        std::optional<double> value = parseYamlDouble(text);
        if (!value) {
            fail(node, context, "expected a number, found \"" + text + "\"");
        }

        return *value;
    }

    float
    readFloat(const YAML::Node& node,
              const std::string& context)
    {
        const std::string text = scalarText(node, context, "a number");
        std::optional<float> value = parseYamlFloat(text);
        if (!value) {
            fail(node, context, "expected a number, found \"" + text + "\"");
        }

        return *value;
    }

    long long
    readInt64(const YAML::Node& node,
              const std::string& context)
    {
        const std::string text = scalarText(node, context, "an integer");
        std::optional<std::int64_t> value = parseYamlInteger(text);
        if (!value) {
            fail(node, context, "expected an integer, found \"" + text + "\"");
        }

        return *value;
    }

    int
    readInt(const YAML::Node& node,
            const std::string& context)
    {
        const long long value = readInt64(node, context);
        if ((value < std::numeric_limits<int>::min()) || (value > std::numeric_limits<int>::max())) {
            fail(node, context, "integer out of range");
        }

        return (int)value;
    }

    unsigned int
    readUInt(const YAML::Node& node,
             const std::string& context)
    {
        const long long value = readInt64(node, context);
        if ((value < 0) || (value > (long long)std::numeric_limits<unsigned int>::max())) {
            fail(node, context, "integer out of range");
        }

        return (unsigned int)value;
    }

    bool
    readBool(const YAML::Node& node,
             const std::string& context)
    {
        const std::string text = scalarText(node, context, "true or false");
        if ((text == "true") || (text == "True") || (text == "TRUE")) {
            return true;
        }
        if ((text == "false") || (text == "False") || (text == "FALSE")) {
            return false;
        }
        fail(node, context, "expected true or false, found \"" + text + "\"");
    }

    std::string
    readString(const YAML::Node& node,
               const std::string& context)
    {
        if (node && node.IsNull()) {
            return std::string();
        }

        return scalarText(node, context, "a string");
    }

    std::vector<std::string>
    readStringVector(const YAML::Node& node,
                     const std::string& context)
    {
        std::vector<std::string> values;
        if (node && node.IsNull()) {
            return values;
        }
        if (!node || !node.IsSequence()) {
            fail(node, context, "expected a list of strings");
        }
        values.reserve(node.size());
        for (std::size_t i = 0; i < node.size(); ++i) {
            values.push_back(readString(node[i], context));
        }

        return values;
    }

    std::list<std::string>
    readStringList(const YAML::Node& node,
                   const std::string& context)
    {
        std::vector<std::string> values = readStringVector(node, context);

        return std::list<std::string>(values.begin(), values.end());
    }

    std::vector<double>
    readDoubleVector(const YAML::Node& node,
                     const std::string& context,
                     std::size_t expectedSize)
    {
        if (!node || !node.IsSequence() || (node.size() != expectedSize)) {
            fail(node, context, "expected a list of " + std::to_string(expectedSize) + " numbers");
        }
        std::vector<double> values(expectedSize);
        for (std::size_t i = 0; i < expectedSize; ++i) {
            values[i] = readDouble(node[i], context);
        }

        return values;
    }

    std::string
    readKey(const YAML::Node& key,
            const std::string& context)
    {
        if (!key || !key.IsScalar()) {
            fail(key, context, "expected a plain key");
        }

        return key.Scalar();
    }

    void
    warn(YamlWarnings* warnings,
         const std::string& message)
    {
        if (warnings) {
            warnings->push_back(message);
        }
    }

    MapReader::MapReader(const YAML::Node& node,
                         std::string context,
                         YamlWarnings* warnings)
        : _context(std::move(context))
        , _warnings(warnings)
    {
        if (!node || node.IsNull()) {
            return;
        }
        if (!node.IsMap()) {
            fail(node, _context, "expected a map");
        }
        for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
            std::string key = readKey(it->first, _context);
            if (!_index.emplace(key, _entries.size()).second) {
                fail(it->first, _context, "duplicate key \"" + key + "\"");
            }
            _entries.push_back(Entry { std::move(key), it->second, false });
        }
    }

    YAML::Node
    MapReader::get(std::string_view key)
    {
        auto it = _index.find(key);
        if (it == _index.end()) {
            return YAML::Node(YAML::NodeType::Undefined);
        }
        Entry& entry = _entries[it->second];
        entry.used = true;

        return entry.value;
    }

    bool
    MapReader::has(std::string_view key) const
    {
        return _index.find(key) != _index.end();
    }

    std::vector<std::pair<std::string, YAML::Node>>
    MapReader::entries()
    {
        std::vector<std::pair<std::string, YAML::Node>> result;
        result.reserve(_entries.size());
        for (Entry& entry : _entries) {
            entry.used = true;
            result.emplace_back(entry.key, entry.value);
        }

        return result;
    }

    void
    MapReader::finish()
    {
        for (const Entry& entry : _entries) {
            if (!entry.used) {
                warn(_warnings, _context + ": unknown key \"" + entry.key + "\" ignored");
            }
        }
    }

    void
    checkDocumentFormat(MapReader& reader,
                        std::string_view format,
                        int maxVersion,
                        const std::string& what)
    {
        YAML::Node formatNode = reader.get("format");
        if (!formatNode || !formatNode.IsScalar() || (formatNode.Scalar() != format)) {
            throw YamlError("this is not " + what + ": \"format: " + std::string(format) + "\" is missing");
        }
        const int version = readInt(reader.get("formatVersion"), reader.context() + ".formatVersion");
        if (version > maxVersion) {
            throw YamlNewerFormatError("This was saved by a newer Natron (" + std::string(format) + " format version " + std::to_string(version) + "; this version reads up to " + std::to_string(maxVersion) + ")");
        }
        if (version < 1) {
            fail(reader.get("formatVersion"), reader.context() + ".formatVersion", "invalid format version " + std::to_string(version));
        }
    }

    void
    emitDocumentFormat(YAML::Emitter& em,
                       std::string_view format,
                       int version)
    {
        emitKey(em, "format");
        em << YAML::Value << std::string(format);
        emitKey(em, "formatVersion");
        em << YAML::Value;
        emitInt(em, version);
    }

} // namespace YamlDetail

} // namespace Serialization
NATRON_NAMESPACE_EXIT
