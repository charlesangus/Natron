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
 * Ported from realflare (https://github.com/beatreichenbach/realflare v1.0.3,
 * flare/adapters/database/parsers/zemax.py), GPL-3.0-or-later; this port is GPL-3.0-or-later.
 * Departures: unknown surface types, catalogue glass names, malformed numbers, surfaces without
 * DISZ and nonzero aspheric terms beyond the eighth are errors instead of being skipped or
 * truncated; the object surface is always dropped; PARM / XDAT are applied by the surface's final
 * type; and extended asphere terms are scaled by the XDAT 2 normalisation radius.
 */

#include "Engine/Optics/ZemaxParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#include "Engine/Optics/DatParser.h"

namespace Natron {
namespace Optics {

    namespace {

        bool
        fail(LensFileError* error,
             int line,
             const std::string& message)
        {
            if (error) {
                error->line = line;
                error->message = message;
            }

            return false;
        }

        void
        appendUtf8(unsigned cp,
                   std::string* out)
        {
            if (cp < 0x80) {
                out->push_back((char)cp);
            } else if (cp < 0x800) {
                out->push_back((char)(0xC0 | (cp >> 6)));
                out->push_back((char)(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out->push_back((char)(0xE0 | (cp >> 12)));
                out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out->push_back((char)(0x80 | (cp & 0x3F)));
            } else {
                out->push_back((char)(0xF0 | (cp >> 18)));
                out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out->push_back((char)(0x80 | (cp & 0x3F)));
            }
        }

        std::string
        decodeUtf16(const std::string& bytes,
                    size_t start,
                    bool littleEndian)
        {
            std::string out;
            out.reserve(bytes.size() / 2);
            const auto unit = [&](size_t i) {
                const unsigned a = (unsigned char)bytes[i], b = (unsigned char)bytes[i + 1];

                return littleEndian ? (a | (b << 8)) : ((a << 8) | b);
            };
            for (size_t i = start; i + 1 < bytes.size(); i += 2) {
                unsigned cp = unit(i);
                if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < bytes.size()) {
                    const unsigned low = unit(i + 2);
                    if (low >= 0xDC00 && low < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    }
                }
                appendUtf8(cp, &out);
            }

            return out;
        }

        // Some Zemax exports are UTF-16, with or without a byte order mark.
        std::string
        decodeText(const std::string& bytes)
        {
            const auto byte = [&](size_t i) { return (unsigned char)bytes[i]; };
            if (bytes.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) {
                return bytes.substr(3);
            }
            if (bytes.size() >= 2 && byte(0) == 0xFF && byte(1) == 0xFE) {
                return decodeUtf16(bytes, 2, true);
            }
            if (bytes.size() >= 2 && byte(0) == 0xFE && byte(1) == 0xFF) {
                return decodeUtf16(bytes, 2, false);
            }
            if (bytes.size() >= 2 && byte(0) != 0 && byte(1) == 0) {
                return decodeUtf16(bytes, 0, true);
            }
            if (bytes.size() >= 2 && byte(0) == 0 && byte(1) != 0) {
                return decodeUtf16(bytes, 0, false);
            }

            return bytes;
        }

        std::vector<std::string>
        split(const std::string& text)
        {
            std::istringstream in(text);
            std::vector<std::string> parts;
            std::string part;
            while (in >> part) {
                parts.push_back(part);
            }

            return parts;
        }

        bool
        parseNumber(std::string text,
                    double* out)
        {
            std::replace(text.begin(), text.end(), ',', '.');
            std::string lower = text;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (lower == "infinity" || lower == "inf") {
                *out = std::numeric_limits<double>::infinity();

                return true;
            }
            const char* begin = text.c_str();
            char* end = nullptr;
            *out = std::strtod(begin, &end);

            return end != begin && *end == '\0';
        }

        struct PendingSurface {
            int line = 0;
            int number = 0;
            Surface surface;
            bool typeSet = false;
            bool haveDisz = false;
            std::array<double, kMaxAsphericCoefficients> parm {};
            std::vector<std::pair<int, double>> xdat;
            std::vector<int> xdatLines;
        };

        bool
        surfaceTypeFromName(const std::string& name,
                            SurfaceType* out)
        {
            static const std::pair<const char*, SurfaceType> types[] = {
                { "STOP", SurfaceType::Stop },
                { "STANDARD", SurfaceType::Standard },
                { "EVENASPH", SurfaceType::EvenAsphere },
                { "XASPHERE", SurfaceType::ExtendedAsphere },
                { "XOSPHERE", SurfaceType::ExtendedOddAsphere },
                { "CYLINDER_X", SurfaceType::CylinderX },
                { "CYLINDER_Y", SurfaceType::CylinderY },
            };
            for (const auto& t : types) {
                if (name == t.first) {
                    *out = t.second;

                    return true;
                }
            }

            return false;
        }

        bool
        finishSurface(PendingSurface* p,
                      LensFileError* error)
        {
            const std::string which = "surface " + std::to_string(p->number);
            if (!p->haveDisz) {
                return fail(error, p->line, which + " has no DISZ (truncated file?)");
            }
            Surface& s = p->surface;
            s.coefficients.fill(0.);
            if (s.type == SurfaceType::EvenAsphere) {
                s.coefficients = p->parm;
            } else if (s.type == SurfaceType::ExtendedAsphere || s.type == SurfaceType::ExtendedOddAsphere) {
                const bool odd = s.type == SurfaceType::ExtendedOddAsphere;
                const int start = odd ? 3 : 4;
                double normRadius = 1.;
                for (const auto& [index, value] : p->xdat) {
                    if (index == 2 && value > 0.) {
                        normRadius = value;
                    }
                }
                for (size_t i = 0; i < p->xdat.size(); ++i) {
                    const int slot = p->xdat[i].first - start;
                    if (slot < 0) {
                        continue;
                    }
                    if (slot >= kMaxAsphericCoefficients) {
                        if (p->xdat[i].second != 0.) {
                            return fail(error, p->xdatLines[i],
                                        which + ": aspheric term " + std::to_string(slot + 1) + " is beyond the " + std::to_string(kMaxAsphericCoefficients) + " supported");
                        }
                        continue;
                    }
                    // Extended asphere terms are polynomials in r / normRadius.
                    const int power = odd ? slot + 1 : 2 * slot + 2;
                    s.coefficients[slot] = p->xdat[i].second / std::pow(normRadius, power);
                }
            }

            return true;
        }

    } // namespace

    bool
    parseZemax(const std::string& bytes,
               Prescription* out,
               LensFileError* error)
    {
        const std::string text = decodeText(bytes);
        Prescription lens;
        std::vector<PendingSurface> surfaces;
        PendingSurface* current = nullptr;
        std::string note;

        std::istringstream in(text);
        std::string raw;
        int lineNumber = 0;
        while (std::getline(in, raw)) {
            ++lineNumber;
            if (!raw.empty() && raw.back() == '\r') {
                raw.pop_back();
            }
            const size_t first = raw.find_first_not_of(" \t");
            if (first == std::string::npos) {
                continue;
            }
            const bool indented = first > 0;
            if (current && !indented) {
                if (!finishSurface(current, error)) {
                    return false;
                }
                current = nullptr;
            }
            const size_t keyEnd = raw.find_first_of(" \t", first);
            const std::string key = raw.substr(first, keyEnd == std::string::npos ? std::string::npos : keyEnd - first);
            std::string rest = keyEnd == std::string::npos ? std::string() : raw.substr(keyEnd);
            const size_t restStart = rest.find_first_not_of(" \t");
            rest = restStart == std::string::npos ? std::string() : rest.substr(restStart);
            const size_t restEnd = rest.find_last_not_of(" \t");
            rest.erase(restEnd == std::string::npos ? 0 : restEnd + 1);
            const std::vector<std::string> parts = split(rest);

            const auto number = [&](size_t index, const char* what, double* value) {
                if (index >= parts.size()) {
                    return fail(error, lineNumber, std::string(what) + " has no value");
                }
                if (!parseNumber(parts[index], value)) {
                    return fail(error, lineNumber, std::string(what) + ": not a number: \"" + parts[index] + "\"");
                }

                return true;
            };

            if (key == "SURF") {
                double n = 0.;
                if (!number(0, "SURF", &n)) {
                    return false;
                }
                surfaces.emplace_back();
                current = &surfaces.back();
                current->line = lineNumber;
                current->number = (int)n;
                continue;
            }
            if (!current) {
                if (key == "NAME") {
                    lens.name = rest;
                } else if (key == "NOTE") {
                    std::string t = rest;
                    const std::vector<std::string> words = split(t);
                    double ignored;
                    if (!words.empty() && parseNumber(words[0], &ignored)) {
                        const size_t after = t.find(words[0]) + words[0].size();
                        t = t.substr(after);
                        const size_t s = t.find_first_not_of(" \t");
                        t = s == std::string::npos ? std::string() : t.substr(s);
                    }
                    if (!t.empty()) {
                        note += (note.empty() ? "" : "\n") + t;
                    }
                } else if (key == "FNUM") {
                    if (!number(0, "FNUM", &lens.fNumber)) {
                        return false;
                    }
                }
                continue;
            }

            Surface& s = current->surface;
            if (key == "TYPE") {
                if (current->typeSet) {
                    continue;
                }
                SurfaceType type;
                if (!surfaceTypeFromName(parts.empty() ? std::string() : parts[0], &type)) {
                    return fail(error, lineNumber, "unsupported surface type \"" + rest + "\"");
                }
                s.type = type;
                current->typeSet = true;
            } else if (key == "STOP") {
                s.type = SurfaceType::Stop;
                current->typeSet = true;
            } else if (key == "CURV") {
                if (!number(0, "CURV", &s.curvature)) {
                    return false;
                }
            } else if (key == "DISZ") {
                if (!number(0, "DISZ", &s.spacing)) {
                    return false;
                }
                current->haveDisz = true;
            } else if (key == "GLAS") {
                if (!parts.empty() && parts[0] == "MIRROR") {
                    return fail(error, lineNumber, "mirror surfaces are not supported");
                }
                if (parts.size() < 4) {
                    return fail(error, lineNumber,
                                "glass \"" + rest + "\" is a catalogue name; only \"___BLANK 1 0 nd Vd\" glasses are read");
                }
                if (!number(3, "GLAS index", &s.nd)) {
                    return false;
                }
                s.vd = 60.;
                if (parts.size() >= 5 && !number(4, "GLAS Abbe number", &s.vd)) {
                    return false;
                }
            } else if (key == "DIAM") {
                if (!number(0, "DIAM", &s.semiDiameter)) {
                    return false;
                }
            } else if (key == "CONI") {
                if (!number(0, "CONI", &s.conic)) {
                    return false;
                }
            } else if (key == "PARM" || key == "XDAT") {
                double index = 0., value = 0.;
                if (!number(0, key.c_str(), &index) || !number(1, key.c_str(), &value)) {
                    return false;
                }
                const int i = (int)index;
                if (key == "PARM") {
                    if (i >= 1 && i <= kMaxAsphericCoefficients) {
                        current->parm[i - 1] = value;
                    }
                } else if (i >= 1) {
                    current->xdat.emplace_back(i, value);
                    current->xdatLines.push_back(lineNumber);
                }
            }
        }
        if (current && !finishSurface(current, error)) {
            return false;
        }

        std::vector<Surface> kept;
        for (const PendingSurface& p : surfaces) {
            if (p.number == 0 || std::isinf(p.surface.spacing)) {
                continue;
            }
            kept.push_back(p.surface);
        }
        if (kept.size() < 2) {
            return fail(error, lineNumber, "no lens surfaces before the image surface");
        }
        lens.imageDistance = kept[kept.size() - 2].spacing;
        kept.pop_back();
        lens.surfaces = kept;
        lens.source = note;
        if (lens.stopCount() != 1) {
            return fail(error, lineNumber, lens.stopCount() ? "more than one stop surface" : "no stop surface");
        }
        *out = lens;

        return true;
    }

    bool
    loadLensFile(const std::string& path,
                 Prescription* out,
                 LensFileError* error)
    {
        const std::filesystem::path p(path);
        std::ifstream file(p, std::ios::binary);
        if (!file) {
            return fail(error, 0, "cannot open " + path);
        }
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string extension = p.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        Prescription lens;
        bool ok;
        if (extension == ".zmx") {
            ok = parseZemax(bytes, &lens, error);
        } else if (extension == ".dat") {
            ok = parseDat(decodeText(bytes), &lens, error);
        } else {
            return fail(error, 0, "unknown lens file type \"" + extension + "\" (expected .zmx or .dat)");
        }
        if (!ok) {
            return false;
        }
        lens.id = p.stem().string();
        if (lens.name.empty()) {
            lens.name = lens.id;
        }
        *out = lens;

        return true;
    }

} // namespace Optics
} // namespace Natron
