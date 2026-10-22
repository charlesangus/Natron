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
#include <memory>
#include <string>
#include <vector>

#include "RawSpeed-API.h"
#include "decoders/NakedDecoder.h"

#include <gtest/gtest.h>

#include "Engine/Nodes/IO/RawspeedRuntime.h"

NATRON_NAMESPACE_USING

TEST(RawspeedLink, VersionNamesThePinnedCommit)
{
    EXPECT_EQ(40u, RawspeedRuntime::version().size()) << RawspeedRuntime::version();
}

// The build tree puts cameras.xml where the bundle does, beside the executables.
TEST(RawspeedLink, CameraDatabaseIsFoundAndParses)
{
    ASSERT_FALSE(RawspeedRuntime::camerasXmlPath().empty());

    std::string error;
    const rawspeed::CameraMetaData* metadata = RawspeedRuntime::cameraMetaData(&error);
    ASSERT_TRUE(metadata != nullptr) << error;
    EXPECT_TRUE(metadata->getCamera("Canon", "Canon EOS 5D Mark II") != nullptr);
    EXPECT_TRUE(metadata->getCamera("No Such Maker", "No Such Camera") == nullptr);
    EXPECT_EQ(metadata, RawspeedRuntime::cameraMetaData());
}

TEST(RawspeedLink, ConstructsARawDecoder)
{
    const std::vector<std::uint8_t> bytes(256, 0);
    const rawspeed::Buffer buffer(bytes.data(), static_cast<rawspeed::Buffer::size_type>(bytes.size()));

    std::unique_ptr<rawspeed::RawDecoder> decoder = std::make_unique<rawspeed::NakedDecoder>(buffer, nullptr);
    ASSERT_TRUE(bool(decoder));
    EXPECT_TRUE(decoder->getRootIFD() == nullptr);
}

// The parser is the entry point the Read uses; on bytes that are no raw file it reports through
// rawspeed's own exception rather than crashing.
TEST(RawspeedLink, ParserRejectsANonRawBuffer)
{
    const rawspeed::CameraMetaData* metadata = RawspeedRuntime::cameraMetaData();
    ASSERT_TRUE(metadata != nullptr);

    const std::vector<std::uint8_t> bytes(256, 0);
    const rawspeed::Buffer buffer(bytes.data(), static_cast<rawspeed::Buffer::size_type>(bytes.size()));
    rawspeed::RawParser parser(buffer);
    EXPECT_THROW(static_cast<void>(parser.getDecoder(metadata)), rawspeed::RawspeedException);
}
