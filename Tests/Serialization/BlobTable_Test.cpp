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

#include "Global/Macros.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/Bundle/BlobTable.h"

NATRON_NAMESPACE_USING
using Serialization::BlobColumn;
using Serialization::BlobDType;
using Serialization::BlobTableError;
using Serialization::BlobTableView;
using Serialization::BlobTableWriter;

namespace {

template <typename T>
std::vector<T>
ramp(std::size_t rows,
     double scale)
{
    std::vector<T> values(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        double value = static_cast<double>(i) * scale;
        if constexpr (std::is_integral_v<T>) {
            values[i] = static_cast<T>(static_cast<std::int64_t>(value));
        } else {
            values[i] = static_cast<T>(value);
        }
    }

    return values;
}

template <typename T>
void
expectColumn(const BlobTableView& view,
             const char* name,
             const std::vector<T>& expected)
{
    BlobColumn<T> column = view.column<T>(name);
    EXPECT_TRUE(column.present()) << name;
    ASSERT_EQ(column.size(), expected.size()) << name;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(column[i], expected[i]) << name << " row " << i;
    }
}

std::vector<std::byte>
smallTable()
{
    std::vector<double> time = { 1.0, 2.0, 3.0 };
    std::vector<float> x = { 0.5f, 1.5f, 2.5f };
    std::vector<std::int32_t> id = { -1, 0, 1 };
    std::vector<std::uint8_t> flags = { 0, 1, 255 };
    BlobTableWriter writer(3);
    writer.addColumn("time", time);
    writer.addColumn("x", x);
    writer.addColumn("id", id);
    writer.addColumn("flags", flags);

    return writer.serialize();
}

void
setU16(std::vector<std::byte>& bytes,
       std::size_t offset,
       std::uint16_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void
setU64(std::vector<std::byte>& bytes,
       std::size_t offset,
       std::uint64_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// Reads every column a view reports, so a view that wrongly accepted corrupt bytes trips the
// bounds checks below instead of passing silently.
void
touchAllColumns(const BlobTableView& view,
                std::span<const std::byte> blob)
{
    for (const std::string& name : view.columnNames()) {
        std::span<const std::byte> bytes;
        switch (*view.columnType(name)) {
        case BlobDType::eF64: {
            BlobColumn<double> column = view.column<double>(name);
            bytes = std::as_bytes(column.values());
            if (!column.zeroCopy()) {
                continue;
            }
            break;
        }
        case BlobDType::eF32: {
            BlobColumn<float> column = view.column<float>(name);
            bytes = std::as_bytes(column.values());
            if (!column.zeroCopy()) {
                continue;
            }
            break;
        }
        case BlobDType::eI32: {
            BlobColumn<std::int32_t> column = view.column<std::int32_t>(name);
            bytes = std::as_bytes(column.values());
            if (!column.zeroCopy()) {
                continue;
            }
            break;
        }
        case BlobDType::eU8: {
            BlobColumn<std::uint8_t> column = view.column<std::uint8_t>(name);
            bytes = std::as_bytes(column.values());
            break;
        }
        }
        ASSERT_GE(bytes.data(), blob.data()) << name;
        ASSERT_LE(bytes.data() + bytes.size(), blob.data() + blob.size()) << name;
    }
}
} // anon namespace

TEST(BlobTable, RoundTripsEveryDTypeOnAlignedColumns)
{
    const std::size_t rows = 1000;
    std::vector<double> time = ramp<double>(rows, 0.25);
    std::vector<float> x = ramp<float>(rows, -1.5);
    std::vector<std::int32_t> id = ramp<std::int32_t>(rows, -3);
    std::vector<std::uint8_t> flags = ramp<std::uint8_t>(rows, 1);

    BlobTableWriter writer(rows);
    writer.addColumn("time", time);
    writer.addColumn("x", x);
    writer.addColumn("id", id);
    writer.addColumn("flags", flags);
    std::vector<std::byte> blob = writer.serialize();

    BlobTableView view(blob);
    EXPECT_EQ(view.rows(), rows);
    EXPECT_EQ(view.columnNames(), (std::vector<std::string> { "time", "x", "id", "flags" }));
    EXPECT_EQ(view.columnType("time"), BlobDType::eF64);
    EXPECT_EQ(view.columnType("x"), BlobDType::eF32);
    EXPECT_EQ(view.columnType("id"), BlobDType::eI32);
    EXPECT_EQ(view.columnType("flags"), BlobDType::eU8);
    expectColumn(view, "time", time);
    expectColumn(view, "x", x);
    expectColumn(view, "id", id);
    expectColumn(view, "flags", flags);

    for (const char* name : { "time", "x", "id" }) {
        std::span<const std::byte> bytes;
        if (std::string(name) == "time") {
            BlobColumn<double> column = view.column<double>(name);
            EXPECT_TRUE(column.zeroCopy());
            bytes = std::as_bytes(column.values());
        } else if (std::string(name) == "x") {
            BlobColumn<float> column = view.column<float>(name);
            EXPECT_TRUE(column.zeroCopy());
            bytes = std::as_bytes(column.values());
        } else {
            BlobColumn<std::int32_t> column = view.column<std::int32_t>(name);
            EXPECT_TRUE(column.zeroCopy());
            bytes = std::as_bytes(column.values());
        }
        EXPECT_EQ(static_cast<std::size_t>(bytes.data() - blob.data()) % Serialization::kBlobTableColumnAlignment, 0u) << name;
    }
}

TEST(BlobTable, RoundTripsZeroRows)
{
    BlobTableWriter writer(0);
    writer.addColumn("time", std::vector<double>());
    writer.addColumn("flags", std::vector<std::uint8_t>());
    std::vector<std::byte> blob = writer.serialize();
    BlobTableView view(blob);
    EXPECT_EQ(view.rows(), 0u);
    EXPECT_EQ(view.columnNames().size(), 2u);
    EXPECT_EQ(view.column<double>("time").size(), 0u);
    EXPECT_TRUE(view.column<double>("time").present());
    EXPECT_EQ(view.column<float>("absent").size(), 0u);

    std::vector<std::byte> empty = BlobTableWriter(0).serialize();
    EXPECT_EQ(empty.size(), Serialization::kBlobTableHeaderSize);
    BlobTableView emptyView(empty);
    EXPECT_EQ(emptyView.rows(), 0u);
    EXPECT_TRUE(emptyView.columnNames().empty());
}

TEST(BlobTable, RoundTripsAMillionRows)
{
    const std::size_t rows = 1000000;
    std::vector<double> time = ramp<double>(rows, 1.0 / 24.0);
    std::vector<float> value = ramp<float>(rows, 0.001);
    BlobTableWriter writer(rows);
    writer.addColumn("time", time);
    writer.addColumn("value", value);
    std::vector<std::byte> blob = writer.serialize();
    EXPECT_GE(blob.size(), rows * (sizeof(double) + sizeof(float)));

    BlobTableView view(blob);
    EXPECT_EQ(view.rows(), rows);
    expectColumn(view, "time", time);
    expectColumn(view, "value", value);
}

TEST(BlobTable, MatchesColumnsByName)
{
    std::vector<std::byte> blob = smallTable();
    BlobTableView view(blob);

    BlobColumn<double> missing = view.column<double>("y", 7.5);
    EXPECT_FALSE(missing.present());
    ASSERT_EQ(missing.size(), 3u);
    for (double v : missing.values()) {
        EXPECT_EQ(v, 7.5);
    }
    EXPECT_FALSE(view.hasColumn("y"));

    expectColumn(view, "flags", std::vector<std::uint8_t> { 0, 1, 255 });
    expectColumn(view, "time", std::vector<double> { 1.0, 2.0, 3.0 });

    EXPECT_THROW(view.column<float>("time"), BlobTableError);
}

TEST(BlobTable, CopiesUnalignedColumns)
{
    std::vector<std::byte> blob = smallTable();
    std::vector<std::byte> shifted(blob.size() + 1);
    std::memcpy(shifted.data() + 1, blob.data(), blob.size());
    BlobTableView view(std::span<const std::byte>(shifted.data() + 1, blob.size()));

    BlobColumn<double> time = view.column<double>("time");
    EXPECT_FALSE(time.zeroCopy());
    expectColumn(view, "time", std::vector<double> { 1.0, 2.0, 3.0 });
    expectColumn(view, "x", std::vector<float> { 0.5f, 1.5f, 2.5f });
    expectColumn(view, "id", std::vector<std::int32_t> { -1, 0, 1 });
}

TEST(BlobTable, WriterRefusesInconsistentColumns)
{
    std::vector<double> three = { 1, 2, 3 };
    std::vector<double> two = { 1, 2 };
    BlobTableWriter writer(3);
    EXPECT_THROW(writer.addColumn("short", two), BlobTableError);
    EXPECT_THROW(writer.addColumn("", three), BlobTableError);
    writer.addColumn("time", three);
    EXPECT_THROW(writer.addColumn("time", three), BlobTableError);

    EXPECT_THROW(BlobTableWriter(5).serialize(), BlobTableError);
}

TEST(BlobTable, DTypeNamesRoundTrip)
{
    for (BlobDType type : { BlobDType::eF64, BlobDType::eF32, BlobDType::eI32, BlobDType::eU8 }) {
        EXPECT_EQ(Serialization::blobDTypeFromName(Serialization::blobDTypeName(type)), type);
    }
    EXPECT_FALSE(Serialization::blobDTypeFromName("f16").has_value());
}

TEST(BlobTable, RejectsCorruptHeadersAndDirectories)
{
    const std::vector<std::byte> original = smallTable();
    ASSERT_NO_THROW(BlobTableView { original });

    struct Corruption {
        const char* what;
        void (*apply)(std::vector<std::byte>&);
    };
    const Corruption corruptions[] = {
        { "bad magic", [](std::vector<std::byte>& b) { b[0] = std::byte { 'X' }; } },
        { "future version", [](std::vector<std::byte>& b) { setU16(b, 4, 2); } },
        { "more columns than the directory holds", [](std::vector<std::byte>& b) { setU16(b, 6, 60000); } },
        { "rows past the column data", [](std::vector<std::byte>& b) { setU64(b, 8, 4); } },
        { "rows that overflow a byte count", [](std::vector<std::byte>& b) { setU64(b, 8, std::numeric_limits<std::uint64_t>::max() / 2); } },
        { "directory past the end", [](std::vector<std::byte>& b) { setU64(b, 16, b.size() + 1); } },
        { "directory inside the header", [](std::vector<std::byte>& b) { setU64(b, 16, 8); } },
        { "directory at the very end", [](std::vector<std::byte>& b) { setU64(b, 16, b.size()); } },
        { "column offset past the end", [](std::vector<std::byte>& b) { setU64(b, 64, b.size()); } },
        { "column offset huge", [](std::vector<std::byte>& b) { setU64(b, 64, std::numeric_limits<std::uint64_t>::max()); } },
        { "column name past the end", [](std::vector<std::byte>& b) { setU16(b, 72, 65535); } },
        { "empty column name", [](std::vector<std::byte>& b) { setU16(b, 72, 0); } },
        { "unknown dtype", [](std::vector<std::byte>& b) { b[74] = std::byte { 9 }; } },
        { "rows without columns", [](std::vector<std::byte>& b) { setU16(b, 6, 0); } },
    };
    for (const Corruption& corruption : corruptions) {
        std::vector<std::byte> bytes = original;
        corruption.apply(bytes);
        EXPECT_THROW(BlobTableView { bytes }, BlobTableError) << corruption.what;
    }

    std::vector<double> values = { 1, 2 };
    BlobTableWriter writer(2);
    writer.addColumn("aa", values);
    writer.addColumn("ab", values);
    std::vector<std::byte> duplicate = writer.serialize();
    std::size_t second = 64 + 24;
    ASSERT_EQ(std::to_integer<char>(duplicate[second + 17]), 'b');
    duplicate[second + 17] = std::byte { 'a' };
    EXPECT_THROW(BlobTableView { duplicate }, BlobTableError);
}

TEST(BlobTable, SurvivesEveryTruncationAndBitFlip)
{
    const std::vector<std::byte> original = smallTable();

    for (std::size_t size = 0; size < original.size(); ++size) {
        std::vector<std::byte> bytes(original.begin(), original.begin() + size);
        try {
            BlobTableView view(bytes);
            touchAllColumns(view, bytes);
        } catch (const BlobTableError&) {
        }
    }

    for (std::size_t i = 0; i < original.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<std::byte> bytes = original;
            bytes[i] ^= static_cast<std::byte>(1 << bit);
            try {
                BlobTableView view(bytes);
                touchAllColumns(view, bytes);
            } catch (const BlobTableError&) {
            }
        }
    }
}
