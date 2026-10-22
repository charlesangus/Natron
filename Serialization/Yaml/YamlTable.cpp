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

#include <cstring>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace YamlDetail {

    std::size_t
    TableColumn::size() const
    {
        switch (type) {
        case BlobDType::eF64:
            return f64.size();
        case BlobDType::eI32:
            return i32.size();
        case BlobDType::eU8:
            return u8.size();
        case BlobDType::eF32:
            break;
        }

        return 0;
    }

    void
    Table::addF64(const std::string& name,
                  std::vector<double> values)
    {
        columns.push_back(TableColumn { name, BlobDType::eF64, std::move(values), {}, {} });
    }

    void
    Table::addI32(const std::string& name,
                  std::vector<std::int32_t> values)
    {
        columns.push_back(TableColumn { name, BlobDType::eI32, {}, std::move(values), {} });
    }

    void
    Table::addU8(const std::string& name,
                 std::vector<std::uint8_t> values)
    {
        columns.push_back(TableColumn { name, BlobDType::eU8, {}, {}, std::move(values) });
    }

    const TableColumn*
    Table::find(std::string_view name) const
    {
        for (const TableColumn& column : columns) {
            if (column.name == name) {
                return &column;
            }
        }

        return nullptr;
    }

    std::vector<double>
    Table::f64(std::string_view name,
               double defaultValue) const
    {
        const TableColumn* column = find(name);
        if (!column || (column->type != BlobDType::eF64)) {
            return std::vector<double>(rows, defaultValue);
        }

        return column->f64;
    }

    std::vector<std::int32_t>
    Table::i32(std::string_view name,
               std::int32_t defaultValue) const
    {
        const TableColumn* column = find(name);
        if (!column || (column->type != BlobDType::eI32)) {
            return std::vector<std::int32_t>(rows, defaultValue);
        }

        return column->i32;
    }

    std::vector<std::uint8_t>
    Table::u8(std::string_view name,
              std::uint8_t defaultValue) const
    {
        const TableColumn* column = find(name);
        if (!column || (column->type != BlobDType::eU8)) {
            return std::vector<std::uint8_t>(rows, defaultValue);
        }

        return column->u8;
    }

    namespace {

        void
        checkTable(const Table& table)
        {
            for (const TableColumn& column : table.columns) {
                if (column.size() != table.rows) {
                    throw YamlError("the table column \"" + column.name + "\" has " + std::to_string(column.size()) + " rows instead of " + std::to_string(table.rows));
                }
            }
        }

        void
        emitColumnList(YAML::Emitter& em,
                       const Table& table)
        {
            em << YAML::Flow << YAML::BeginSeq;
            for (const TableColumn& column : table.columns) {
                em << column.name + ":" + std::string(blobDTypeName(column.type));
            }
            em << YAML::EndSeq;
        }

        std::vector<std::byte>
        serializeTable(const Table& table)
        {
            BlobTableWriter writer(table.rows);
            for (const TableColumn& column : table.columns) {
                switch (column.type) {
                case BlobDType::eF64:
                    writer.addColumn(column.name, column.f64);
                    break;
                case BlobDType::eI32:
                    writer.addColumn(column.name, column.i32);
                    break;
                case BlobDType::eU8:
                    writer.addColumn(column.name, column.u8);
                    break;
                case BlobDType::eF32:
                    break;
                }
            }

            return writer.serialize();
        }

        void
        readColumnsFromBlob(std::span<const std::byte> bytes,
                            Table* table,
                            const YAML::Node& node,
                            const std::string& context)
        {
            try {
                BlobTableView view(bytes);
                if (view.rows() != table->rows) {
                    fail(node, context, "the blob holds " + std::to_string(view.rows()) + " rows, the document says " + std::to_string(table->rows));
                }
                for (TableColumn& column : table->columns) {
                    switch (column.type) {
                    case BlobDType::eF64: {
                        BlobColumn<double> values = view.column<double>(column.name);
                        column.f64.assign(values.values().begin(), values.values().end());
                        break;
                    }
                    case BlobDType::eI32: {
                        BlobColumn<std::int32_t> values = view.column<std::int32_t>(column.name);
                        column.i32.assign(values.values().begin(), values.values().end());
                        break;
                    }
                    case BlobDType::eU8: {
                        BlobColumn<std::uint8_t> values = view.column<std::uint8_t>(column.name);
                        column.u8.assign(values.values().begin(), values.values().end());
                        break;
                    }
                    case BlobDType::eF32:
                        break;
                    }
                }
            } catch (const BlobTableError& e) {
                fail(node, context, std::string("unreadable blob table: ") + e.what());
            }
        }

    } // namespace

    void
    emitTable(YAML::Emitter& em,
              const Table& table,
              const std::string& path,
              BlobSink& sink)
    {
        checkTable(table);
        em << YAML::Flow << YAML::BeginMap;
        if (table.rows < kInlineTableRowThreshold) {
            emitKey(em, "rows");
            em << YAML::Value;
            emitInt(em, (long long)table.rows);
            emitKey(em, "columns");
            em << YAML::Value;
            emitColumnList(em, table);
            emitKey(em, "values");
            em << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (std::size_t row = 0; row < table.rows; ++row) {
                for (const TableColumn& column : table.columns) {
                    switch (column.type) {
                    case BlobDType::eF64:
                        emitDouble(em, column.f64[row]);
                        break;
                    case BlobDType::eI32:
                        emitInt(em, column.i32[row]);
                        break;
                    case BlobDType::eU8:
                        emitInt(em, column.u8[row]);
                        break;
                    case BlobDType::eF32:
                        break;
                    }
                }
            }
            em << YAML::EndSeq;
            em << YAML::EndMap;

            return;
        }

        std::vector<std::byte> bytes = serializeTable(table);
        const bool stored = sink.addMember(path, bytes);
        if (stored) {
            emitKey(em, "blob");
            em << YAML::Value << path;
        }
        emitKey(em, "rows");
        em << YAML::Value;
        emitInt(em, (long long)table.rows);
        emitKey(em, "columns");
        em << YAML::Value;
        emitColumnList(em, table);
        if (!stored) {
            emitKey(em, "data");
            em << YAML::Value << YAML::Binary(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        }
        em << YAML::EndMap;
    }

    Table
    readTable(const YAML::Node& node,
              const BlobSource& source,
              const std::string& context,
              YamlWarnings* warnings,
              bool* missing)
    {
        if (missing) {
            *missing = false;
        }
        Table table;
        MapReader reader(node, context, warnings);
        const long long rows = readInt64(reader.get("rows"), context + ".rows");
        if (rows < 0) {
            fail(node, context, "a table cannot have a negative row count");
        }
        table.rows = (std::size_t)rows;

        YAML::Node columns = reader.get("columns");
        if (!columns || !columns.IsSequence()) {
            fail(columns ? columns : node, context + ".columns", "expected a list of name:type columns");
        }
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const std::string descriptor = readString(columns[i], context + ".columns");
            const std::size_t colon = descriptor.rfind(':');
            std::optional<BlobDType> type = (colon == std::string::npos) ? std::nullopt : blobDTypeFromName(descriptor.substr(colon + 1));
            if (!type || (*type == BlobDType::eF32)) {
                fail(columns[i], context + ".columns", "\"" + descriptor + "\" is not a name:f64, name:i32 or name:u8 column");
            }
            const std::string name = descriptor.substr(0, colon);
            if (table.find(name)) {
                fail(columns[i], context + ".columns", "the column \"" + name + "\" is listed twice");
            }
            table.columns.push_back(TableColumn { name, *type, {}, {}, {} });
        }

        if (YAML::Node values = reader.get("values")) {
            if (!values.IsSequence() || (values.size() != table.rows * table.columns.size())) {
                fail(values, context + ".values", "expected " + std::to_string(table.rows * table.columns.size()) + " values (" + std::to_string(table.rows) + " rows of " + std::to_string(table.columns.size()) + " columns)");
            }
            std::size_t index = 0;
            for (std::size_t row = 0; row < table.rows; ++row) {
                for (TableColumn& column : table.columns) {
                    const YAML::Node value = values[index++];
                    switch (column.type) {
                    case BlobDType::eF64:
                        column.f64.push_back(readDouble(value, context + ".values"));
                        break;
                    case BlobDType::eI32:
                        column.i32.push_back(readInt(value, context + ".values"));
                        break;
                    case BlobDType::eU8: {
                        const int byte = readInt(value, context + ".values");
                        if ((byte < 0) || (byte > 255)) {
                            fail(value, context + ".values", "the u8 column \"" + column.name + "\" holds " + std::to_string(byte));
                        }
                        column.u8.push_back((std::uint8_t)byte);
                        break;
                    }
                    case BlobDType::eF32:
                        break;
                    }
                }
            }
        } else if (YAML::Node blob = reader.get("blob")) {
            const std::string path = readString(blob, context + ".blob");
            std::optional<std::span<const std::byte>> bytes = source.findMember(path);
            if (!bytes) {
                warn(warnings, context + ": the blob member \"" + path + "\" is missing; its data is left empty");
                if (missing) {
                    *missing = true;
                }
                table.rows = 0;
                table.columns.clear();
            } else {
                readColumnsFromBlob(*bytes, &table, node, context);
            }
        } else if (YAML::Node data = reader.get("data")) {
            YAML::Binary binary;
            try {
                binary = data.as<YAML::Binary>();
            } catch (const YAML::Exception& e) {
                fail(data, context + ".data", std::string("expected base64 table data: ") + e.what());
            }
            std::vector<std::byte> bytes(binary.size());
            if (!bytes.empty()) {
                std::memcpy(bytes.data(), binary.data(), bytes.size());
            }
            readColumnsFromBlob(bytes, &table, node, context);
        } else {
            fail(node, context, "a table needs values, a blob path or data");
        }
        reader.finish();

        return table;
    }

    int
    checkedInterpolation(std::uint8_t value,
                         const YAML::Node& node,
                         const std::string& context)
    {
        if (value > kYamlKeyframeNone) {
            fail(node, context, "unknown interpolation " + std::to_string(value) + " in an interp column");
        }
        if (keyframeStoresDerivatives(value)) {
            fail(node, context, "a table cannot hold free or broken keyframes, whose derivatives it does not store");
        }

        return value;
    }

    bool
    isTabulableInterpolation(int interpolation)
    {
        return (interpolation >= 0) && (interpolation <= kYamlKeyframeNone) && !keyframeStoresDerivatives(interpolation);
    }

    std::string
    sanitizeBlobPathComponent(const std::string& name)
    {
        std::string result;
        result.reserve(name.size());
        for (char c : name) {
            const bool keep = ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-') || (c == '.');
            result.push_back(keep ? c : '_');
        }
        if (result.empty() || (result.find_first_not_of('.') == std::string::npos)) {
            result.insert(result.begin(), '_');
        }

        return result;
    }

    std::string
    uniqueBlobPath(const std::string& path,
                   std::set<std::string>* usedPaths)
    {
        std::string candidate = path;
        if (usedPaths) {
            const std::size_t extension = path.rfind(".bin");
            for (int suffix = 2; usedPaths->count(candidate); ++suffix) {
                candidate = path.substr(0, extension) + "-" + std::to_string(suffix) + path.substr(extension);
            }
            usedPaths->insert(candidate);
        }

        return candidate;
    }

} // namespace YamlDetail
} // namespace Serialization
NATRON_NAMESPACE_EXIT
