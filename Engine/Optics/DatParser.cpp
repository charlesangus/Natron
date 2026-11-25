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
 * Ported from Niik-l's LensTrace (niikl/RB-2.6 Engine/Dev/PostFX/LensTrace.cpp,
 * GPL-2.0-or-later), parseLensFile().
 */

#include "Engine/Optics/DatParser.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

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

        bool
        parseNumber(const std::string& token,
                    double* out)
        {
            const char* begin = token.c_str();
            char* end = nullptr;
            *out = std::strtod(begin, &end);

            return end != begin && *end == '\0' && std::isfinite(*out);
        }

        // Shortest text that reads back to the same double.
        std::string
        number(double v)
        {
            char buf[32];
            for (int precision = 1; precision <= 17; ++precision) {
                std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
                if (std::strtod(buf, nullptr) == v) {
                    break;
                }
            }

            return buf;
        }

    } // namespace

    bool
    parseDat(const std::string& text,
             Prescription* out,
             LensFileError* error)
    {
        Prescription p;
        std::istringstream in(text);
        std::string line;
        int lineNumber = 0;
        int stopLine = 0;
        while (std::getline(in, line)) {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const size_t hash = line.find('#');
            if (hash != std::string::npos) {
                if (p.name.empty()) {
                    std::string comment = line.substr(hash + 1);
                    const size_t first = comment.find_first_not_of(" \t");
                    comment.erase(0, first == std::string::npos ? comment.size() : first);
                    const size_t last = comment.find_last_not_of(" \t");
                    comment.erase(last == std::string::npos ? 0 : last + 1);
                    if (!comment.empty() && comment.find_first_of("0123456789") != 0) {
                        p.name = comment;
                    }
                }
                line.erase(hash);
            }
            std::istringstream tokens(line);
            std::vector<double> v;
            std::string token;
            while (tokens >> token) {
                double x;
                if (!parseNumber(token, &x)) {
                    return fail(error, lineNumber, "not a finite number: \"" + token + "\"");
                }
                v.push_back(x);
            }
            if (v.empty()) {
                continue;
            }
            if (v.size() < 4 || v.size() > 5) {
                return fail(error, lineNumber, "a row needs radius, thickness, index, aperture and an optional Abbe number");
            }
            Surface s;
            const bool stop = v[0] == 0. && v[2] <= 0.;
            if (stop) {
                if (stopLine) {
                    return fail(error, lineNumber, "a second stop; the stop is at line " + std::to_string(stopLine));
                }
                stopLine = lineNumber;
                if (!(v[3] > 0.)) {
                    return fail(error, lineNumber, "the stop needs an aperture above 0");
                }
            }
            s.type = stop ? SurfaceType::Stop : SurfaceType::Standard;
            s.curvature = v[0] == 0. ? 0. : 1. / v[0];
            s.spacing = v[1];
            s.nd = v[2] <= 0. ? 1. : v[2];
            s.semiDiameter = 0.5 * v[3];
            s.vd = v.size() == 5 && v[4] > 0. ? v[4] : 0.;
            p.surfaces.push_back(s);
        }
        if (p.surfaces.size() < 2) {
            return fail(error, lineNumber, "a lens needs at least two surfaces");
        }
        if (!stopLine) {
            return fail(error, lineNumber, "no stop (a row with radius 0 and index 0)");
        }
        *out = p;

        return true;
    }

    std::string
    formatDat(const Prescription& lens)
    {
        std::string out;
        if (!lens.name.empty()) {
            out += "# " + lens.name + "\n";
        }
        out += "# radius thickness index aperture abbe\n";
        for (const Surface& s : lens.surfaces) {
            const bool stop = s.type == SurfaceType::Stop;
            out += number(stop ? 0. : s.radius()) + " " + number(s.spacing) + " " + number(stop ? 0. : s.nd) + " " + number(2. * s.semiDiameter);
            if (s.vd > 0.) {
                out += " " + number(s.vd);
            }
            out += "\n";
        }

        return out;
    }

} // namespace Optics
} // namespace Natron
