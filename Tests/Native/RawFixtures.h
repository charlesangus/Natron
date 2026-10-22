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

#ifndef NATRON_TESTS_NATIVE_RAWFIXTURES_H
#define NATRON_TESTS_NATIVE_RAWFIXTURES_H

#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

/**
 * The camera raw files in Tests/fixtures/raw (see its README), tracked by Git LFS.
 **/
struct RawFixture {
    const char* file;
    const char* camera;
    // 2 for Bayer, 6 for X-Trans, 0 for linear data.
    int period;
    int channels;

    std::string path() const
    {
        return std::string(NATRON_TESTS_FIXTURES_DIR "/raw/") + file;
    }
};

inline const std::array<RawFixture, 3>&
rawFixtures()
{
    static const std::array<RawFixture, 3> fixtures = { {
        { "canon-eos-5d-mark-iii.cr2", "Canon EOS 5D Mark III", 2, 1 },
        { "fujifilm-x-pro2-14bit-uncompressed.raf", "Fujifilm X-Pro2", 6, 1 },
        { "canon-eos-5d-mark-iii-linear-rgb.dng", "Canon EOS 5D Mark III (linear DNG)", 0, 3 },
    } };

    return fixtures;
}

inline const RawFixture&
bayerFixture()
{
    return rawFixtures()[0];
}

inline const RawFixture&
xtransFixture()
{
    return rawFixtures()[1];
}

inline const RawFixture&
linearFixture()
{
    return rawFixtures()[2];
}

inline std::vector<char>
readFixtureBytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);

    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline bool
writeFixtureBytes(const std::string& path,
                  const char* data,
                  std::size_t size)
{
    std::ofstream out(path, std::ios::binary);

    out.write(data, (std::streamsize)size);

    return bool(out);
}

#endif // NATRON_TESTS_NATIVE_RAWFIXTURES_H
