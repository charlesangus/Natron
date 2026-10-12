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

#include "ProjectXml.h"

#include <cctype>
#include <climits>
#include <cstddef>
#include <vector>

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    // Same result as QString::toInt() on the part, which the old reader used: surrounding
    // whitespace is ignored and anything else that is not a base-10 int reads as 0.
    int
    legacyVersionPartToInt(const std::string& part)
    {
        std::size_t begin = 0;
        std::size_t end = part.size();

        while ((begin < end) && std::isspace((unsigned char)part[begin])) {
            ++begin;
        }
        while ((end > begin) && std::isspace((unsigned char)part[end - 1])) {
            --end;
        }
        bool isNegative = false;
        if ((begin < end) && ((part[begin] == '+') || (part[begin] == '-'))) {
            isNegative = (part[begin] == '-');
            ++begin;
        }
        if (begin == end) {
            return 0;
        }
        long long value = 0;
        for (std::size_t i = begin; i < end; ++i) {
            if (!std::isdigit((unsigned char)part[i])) {
                return 0;
            }
            value = value * 10 + (part[i] - '0');
            if (value > (long long)INT_MAX + 1) {
                return 0;
            }
        }
        if (isNegative) {
            value = -value;
        }
        if ((value > INT_MAX) || (value < INT_MIN)) {
            return 0;
        }

        return (int)value;
    }

} // anonymous namespace

bool
parseLegacyNatronVersionString(const std::string& natronVersion,
                               int* versionMajor,
                               int* versionMinor,
                               int* versionRevision)
{
    const std::string versionPrefix(NATRON_APPLICATION_NAME " v");
    std::size_t versionStart = natronVersion.find(versionPrefix);

    if (versionStart == std::string::npos) {
        return false;
    }
    versionStart += versionPrefix.size();
    const std::size_t versionEnd = natronVersion.find(" from git branch");
    if (versionEnd == std::string::npos) {
        return false;
    }

    std::string versionText;
    for (std::size_t i = versionStart; i < versionEnd; ++i) {
        versionText.push_back(natronVersion[i]);
    }

    std::vector<std::string> parts(1);
    for (std::size_t i = 0; i < versionText.size(); ++i) {
        if (versionText[i] == '.') {
            parts.push_back(std::string());
        } else {
            parts.back().push_back(versionText[i]);
        }
    }
    if (parts.size() != 3) {
        return false;
    }
    *versionMajor = legacyVersionPartToInt(parts[0]);
    *versionMinor = legacyVersionPartToInt(parts[1]);
    *versionRevision = legacyVersionPartToInt(parts[2]);

    return true;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT

// explicit template instantiations

namespace boost {
namespace serialization {

    template void serialize<boost::archive::xml_iarchive>(boost::archive::xml_iarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::LayerRegistryEntrySerialization& entry,
                                                          const unsigned int version);
    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::LayerRegistryEntrySerialization& entry,
                                                          const unsigned int version);

    template void serialize<boost::archive::xml_iarchive>(boost::archive::xml_iarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::ProjectSerialization& project,
                                                          const unsigned int version);
    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::ProjectSerialization& project,
                                                          const unsigned int version);

} // namespace serialization
} // namespace boost
