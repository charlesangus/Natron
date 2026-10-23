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

#ifndef Serialization_Yaml_YamlPrivate_h
#define Serialization_Yaml_YamlPrivate_h

// Shared by the Serialization/Yaml/*.cpp files only. yaml-cpp is a PRIVATE dependency of
// NatronSerialization, so no other target can include this header; the public API of the YAML
// codecs is in the other Serialization/Yaml/ headers and speaks std::string.

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "Serialization/Bundle/BlobTable.h"
#include "Serialization/CurveSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/TrackerSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace YamlDetail {

    /**
     * @brief Writes a map key, double-quoted when a plain scalar would read back as something other
     * than that string (a number, a boolean, null) or would need escaping.
     **/
    void emitKey(YAML::Emitter& em, std::string_view key);

    /**
     * @brief Writes a string value, always double-quoted, so a decoder tells it apart from a number
     * or a token without knowing the type.
     **/
    void emitString(YAML::Emitter& em, std::string_view value);

    void emitDouble(YAML::Emitter& em, double value);
    void emitFloat(YAML::Emitter& em, float value);
    void emitInt(YAML::Emitter& em, long long value);

    /**
     * @brief Writes key with a flow-style [a, b] or [r, g, b] value.
     **/
    void emitDoublePair(YAML::Emitter& em, std::string_view key, double a, double b);
    void emitFloatTriple(YAML::Emitter& em, std::string_view key, float r, float g, float b);
    void emitBool(YAML::Emitter& em, bool value);
    void emitStringList(YAML::Emitter& em, const std::vector<std::string>& values);
    void emitStringList(YAML::Emitter& em, const std::list<std::string>& values);

    /**
     * @brief The document text, throwing YamlError if the emitter reported an error.
     **/
    std::string documentText(const YAML::Emitter& em);

    /**
     * @brief Parses a document, turning a parse error into a YamlError naming what was parsed.
     **/
    YAML::Node loadDocument(std::string_view text, std::string_view what);

    std::string where(const YAML::Node& node);

    [[noreturn]] void fail(const YAML::Node& node, const std::string& context, const std::string& message);

    /**
     * @brief Whether node is a scalar written without quotes. Quoted scalars are strings; plain ones
     * may be numbers, booleans, null or tokens.
     **/
    bool isPlainScalar(const YAML::Node& node);

    double readDouble(const YAML::Node& node, const std::string& context);
    float readFloat(const YAML::Node& node, const std::string& context);
    int readInt(const YAML::Node& node, const std::string& context);
    unsigned int readUInt(const YAML::Node& node, const std::string& context);
    long long readInt64(const YAML::Node& node, const std::string& context);
    bool readBool(const YAML::Node& node, const std::string& context);
    std::string readString(const YAML::Node& node, const std::string& context);
    std::vector<std::string> readStringVector(const YAML::Node& node, const std::string& context);
    std::list<std::string> readStringList(const YAML::Node& node, const std::string& context);
    std::vector<double> readDoubleVector(const YAML::Node& node, const std::string& context, std::size_t expectedSize);
    std::vector<float> readFloatTriple(const YAML::Node& node, const std::string& context);

    /**
     * @brief Reads a map: gives each known key once, refuses duplicate keys, and on finish() reports
     * every key nobody asked for as an "ignored" warning, so files from a newer Natron still load.
     **/
    class MapReader {
    public:
        MapReader(const YAML::Node& node, std::string context, YamlWarnings* warnings);

        /**
         * @brief The value of key, or an undefined node (operator bool false) when it is absent.
         **/
        YAML::Node get(std::string_view key);

        bool has(std::string_view key) const;

        /**
         * @brief The keys in document order, each marked as read.
         **/
        std::vector<std::pair<std::string, YAML::Node>> entries();

        const std::string& context() const
        {
            return _context;
        }

        YamlWarnings* warnings() const
        {
            return _warnings;
        }

        void finish();

    private:
        struct Entry {
            std::string key;
            YAML::Node value;
            bool used;
        };

        struct KeyHash {
            using is_transparent = void;

            std::size_t operator()(std::string_view key) const
            {
                return std::hash<std::string_view>()(key);
            }
        };

        std::string _context;
        YamlWarnings* _warnings;
        std::vector<Entry> _entries;
        std::unordered_map<std::string, std::size_t, KeyHash, std::equal_to<>> _index;
    };

    std::string readKey(const YAML::Node& key, const std::string& context);

    void warn(YamlWarnings* warnings, const std::string& message);

    /**
     * @brief Checks the format and formatVersion keys of a document read with reader. Throws
     * YamlError when it names another format (what names the expected kind of document, e.g.
     * "a Natron preset"), YamlNewerFormatError for a version above maxVersion.
     **/
    void checkDocumentFormat(MapReader& reader, std::string_view format, int maxVersion, const std::string& what);
    void emitDocumentFormat(YAML::Emitter& em, std::string_view format, int version);

    void emitCurve(YAML::Emitter& em, const CurveSerialization& curve);
    CurveSerialization readCurve(const YAML::Node& node, const std::string& context);

    struct KnobEmitOptions {
        // When set and true for a dimension of a knob, that dimension's curve is held in a table of
        // the enclosing item and the knob only says so (`curve: table`).
        std::function<bool(const KnobSerialization& knob, std::size_t dimension)> curveInTable;
    };

    void emitKnobMap(YAML::Emitter& em, const std::list<KnobSerialization>& knobs, const KnobEmitOptions& options = KnobEmitOptions());
    void emitKnobValue(YAML::Emitter& em, const KnobSerialization& knob, const KnobEmitOptions& options = KnobEmitOptions());

    /**
     * @brief A dimension whose curve is held in a table of the enclosing item: the decoder leaves
     * an empty curve there for that item's decoder to fill.
     **/
    struct TabledCurveRef {
        std::string knobName;
        std::size_t dimension;
    };

    std::list<KnobSerialization> readKnobMap(const YAML::Node& node, const std::string& context, YamlWarnings* warnings, std::vector<TabledCurveRef>* tabledCurves = nullptr);
    KnobSerialization readKnob(const std::string& name, const YAML::Node& node, const std::string& context, YamlWarnings* warnings, std::vector<TabledCurveRef>* tabledCurves = nullptr);

    void emitGroupKnobChildren(YAML::Emitter& em, const std::list<GroupKnobChildSerialization>& children);
    void emitUserPages(YAML::Emitter& em, const std::list<GroupKnobSerialization>& pages);
    std::list<GroupKnobSerialization> readUserPages(const YAML::Node& node, const std::string& context, YamlWarnings* warnings);

    struct TableColumn {
        std::string name;
        BlobDType type;
        std::vector<double> f64;
        std::vector<std::int32_t> i32;
        std::vector<std::uint8_t> u8;

        std::size_t size() const;
    };

    struct Table {
        std::size_t rows = 0;
        std::vector<TableColumn> columns;

        void addF64(const std::string& name, std::vector<double> values);
        void addI32(const std::string& name, std::vector<std::int32_t> values);
        void addU8(const std::string& name, std::vector<std::uint8_t> values);

        const TableColumn* find(std::string_view name) const;

        // A column the table lacks reads as rows copies of defaultValue, so a newer writer can add
        // columns and an older one can leave them out.
        std::vector<double> f64(std::string_view name, double defaultValue = 0.) const;
        std::vector<std::int32_t> i32(std::string_view name, std::int32_t defaultValue = 0) const;
        std::vector<std::uint8_t> u8(std::string_view name, std::uint8_t defaultValue = 0) const;
    };

    /**
     * @brief Writes table inline when it is under the threshold, otherwise as a blob at path, which
     * sink either keeps as a member or hands back for base64 inlining.
     **/
    void emitTable(YAML::Emitter& em, const Table& table, const std::string& path, BlobSink& sink);

    /**
     * @brief Reads a table written by emitTable. A blob the source lacks gives a warning, an empty
     * table and *missing set.
     **/
    Table readTable(const YAML::Node& node, const BlobSource& source, const std::string& context, YamlWarnings* warnings, bool* missing = nullptr);

    /**
     * @brief The interpolation of a keyframe held in a table row, which has no room for derivatives:
     * free and broken keyframes are refused.
     **/
    int checkedInterpolation(std::uint8_t value, const YAML::Node& node, const std::string& context);
    bool isTabulableInterpolation(int interpolation);

    /**
     * @brief Makes path unique among usedPaths (appending -2, -3...) and records it.
     **/
    std::string uniqueBlobPath(const std::string& path, std::set<std::string>* usedPaths);

    std::string sanitizeBlobPathComponent(const std::string& name);

    struct BlobEncodeContext {
        BlobSink* sink;
        std::string nodeFullName;
        std::set<std::string>* usedPaths;
    };

    void emitRotoContext(YAML::Emitter& em, const RotoContextSerialization& roto, const BlobEncodeContext& blobs);
    RotoContextSerialization readRotoContext(const YAML::Node& node, const BlobSource& source, const std::string& context, YamlWarnings* warnings);

    void emitTrackerContext(YAML::Emitter& em, const TrackerContextSerialization& tracker, const BlobEncodeContext& blobs);
    TrackerContextSerialization readTrackerContext(const YAML::Node& node, const BlobSource& source, const std::string& context, YamlWarnings* warnings);

    /**
     * @brief Writes nodes as a map keyed by script name, sorted, each with its children nested.
     * parentFullName is the fully qualified name of the group holding them, empty at the top level.
     **/
    void emitNodeMap(YAML::Emitter& em, const std::list<NodeSerializationPtr>& nodes, const std::string& parentFullName, BlobSink& sink, std::set<std::string>* usedPaths);
    std::list<NodeSerializationPtr> readNodeMap(const YAML::Node& node, const BlobSource& source, const std::string& context, YamlWarnings* warnings);

} // namespace YamlDetail
} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_YamlPrivate_h
