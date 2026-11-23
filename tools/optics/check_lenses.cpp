/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

/*
 * The parse check behind tools/optics/convert_lenses.py: loads a lens archive through
 * Optics::LensDatabase, the way Natron does, and prints one "EXCLUDE\t<id>\t<reason>" line per lens
 * that fails to parse, has no paraxial focus, or parses differently from its original OpticsDatabase
 * file. Build it from the repository root (Engine/Optics has no Natron or Qt dependency):
 *
 *   g++ -std=c++17 -O1 -I. tools/optics/check_lenses.cpp Engine/Optics/LensDatabase.cpp \
 *       Engine/Optics/ZemaxParser.cpp Engine/Optics/DatParser.cpp Engine/Optics/Prescription.cpp \
 *       Engine/Optics/BuiltinLenses.cpp Engine/Optics/Paraxial.cpp -lz -o <output>
 *
 * usage: check_lenses <lenses.zmx.gz> <OpticsDatabase checkout>
 */

#include <cstdio>
#include <filesystem>
#include <string>

#include "Engine/Optics/LensDatabase.h"
#include "Engine/Optics/ZemaxParser.h"

using namespace Natron::Optics;

namespace {

bool
sameSurface(const Surface& a,
            const Surface& b)
{
    return a.type == b.type && a.curvature == b.curvature && a.spacing == b.spacing && a.semiDiameter == b.semiDiameter && a.conic == b.conic && a.coefficients == b.coefficients && a.nd == b.nd && a.vd == b.vd;
}

std::string
difference(const Prescription& archived,
           const Prescription& original)
{
    if (archived.surfaces.size() != original.surfaces.size()) {
        return "surface count " + std::to_string(archived.surfaces.size()) + " != " + std::to_string(original.surfaces.size());
    }
    for (size_t i = 0; i < archived.surfaces.size(); ++i) {
        if (!sameSurface(archived.surfaces[i], original.surfaces[i])) {
            return "surface " + std::to_string(i) + " differs";
        }
    }
    if (archived.imageDistance != original.imageDistance) {
        return "image distance differs";
    }
    if (archived.fNumber != original.fNumber) {
        return "FNUM differs";
    }
    if (archived.source != original.source) {
        return "NOTE differs";
    }
    // loadLensFile names an unnamed lens after its file stem.
    if (archived.name != original.name && original.name != original.id) {
        return "NAME differs";
    }

    return std::string();
}

void
exclude(const std::string& id,
        const std::string& reason)
{
    std::printf("EXCLUDE\t%s\t%s\n", id.c_str(), reason.c_str());
}

} // namespace

int
main(int argc,
     char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: check_lenses <lenses.zmx.gz> <OpticsDatabase checkout>\n");

        return 2;
    }
    LensDatabase database(argv[1]);
    std::string error;
    if (!database.load(&error)) {
        std::fprintf(stderr, "check_lenses: %s\n", error.c_str());

        return 2;
    }
    for (const std::string& failure : database.loadFailures()) {
        const size_t colon = failure.find(": ");
        exclude(failure.substr(0, colon), colon == std::string::npos ? failure : failure.substr(colon + 2));
    }
    int checked = 0;
    for (const LensInfo& info : database.lenses()) {
        if (info.sourcePath.empty()) {
            continue;
        }
        Prescription archived;
        if (!database.prescription(info.id, &archived, &error)) {
            exclude(info.id, error);
            continue;
        }
        Prescription original;
        LensFileError originalError;
        const std::string path = (std::filesystem::path(argv[2]) / info.sourcePath).string();
        if (!loadLensFile(path, &original, &originalError)) {
            exclude(info.id, "the original does not parse: " + originalError.text());
            continue;
        }
        const std::string diff = difference(archived, original);
        if (!diff.empty()) {
            exclude(info.id, "parses differently from the original: " + diff);
            continue;
        }
        ++checked;
    }
    std::fprintf(stderr, "check_lenses: %d archive lenses match their originals\n", checked);

    return 0;
}
