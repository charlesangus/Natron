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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/MetadataText.h"

NATRON_NAMESPACE_USING

TEST(MetadataTextTest, IntIsWrittenInDecimal)
{
    const ImageMetadata::Value value(-42);

    EXPECT_EQ("-42", MetadataText::formatValuePlain(value));
    EXPECT_EQ("-42", MetadataText::formatValueHtml(value));
}

TEST(MetadataTextTest, DoubleIsWrittenInShortestForm)
{
    EXPECT_EQ("0.1", MetadataText::formatValuePlain(ImageMetadata::Value(0.1)));
    EXPECT_EQ("2", MetadataText::formatValuePlain(ImageMetadata::Value(2.)));
    EXPECT_EQ("-0.25", MetadataText::formatValueHtml(ImageMetadata::Value(-0.25)));
}

TEST(MetadataTextTest, StringIsWrittenAsIs)
{
    const ImageMetadata::Value value(std::string("a, b"));

    EXPECT_EQ("a, b", MetadataText::formatValuePlain(value));
    EXPECT_EQ("a, b", MetadataText::formatValueHtml(value));
}

TEST(MetadataTextTest, IntVectorIsCommaSeparated)
{
    const ImageMetadata::Value value(std::vector<int>({ 0, 0, 1280, 720 }));

    EXPECT_EQ("0, 0, 1280, 720", MetadataText::formatValuePlain(value));
    EXPECT_EQ("0, 0, 1280, 720", MetadataText::formatValueHtml(value));
    EXPECT_EQ("", MetadataText::formatValuePlain(ImageMetadata::Value(std::vector<int>())));
}

TEST(MetadataTextTest, DoubleVectorIsCommaSeparated)
{
    const ImageMetadata::Value value(std::vector<double>({ 1., 0.5, 0.25 }));

    EXPECT_EQ("1, 0.5, 0.25", MetadataText::formatValuePlain(value));
    EXPECT_EQ("1, 0.5, 0.25", MetadataText::formatValueHtml(value));
}

TEST(MetadataTextTest, HtmlFormEscapesMarkupAndLineBreaks)
{
    const ImageMetadata::Value value(std::string("<b>\"a\" & b</b>\nnext"));

    EXPECT_EQ("<b>\"a\" & b</b>\nnext", MetadataText::formatValuePlain(value));
    EXPECT_EQ("&lt;b&gt;&quot;a&quot; &amp; b&lt;/b&gt;<br />next", MetadataText::formatValueHtml(value));
    EXPECT_EQ("x &lt; y", MetadataText::escapeHtml("x < y"));
}
