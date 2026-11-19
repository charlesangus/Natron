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
 * Ported in part from realflare (https://github.com/beatreichenbach/realflare v1.0.3,
 * GPL-3.0-or-later), with the sources named in Glass.h; this port is GPL-3.0-or-later.
 */

#include "Engine/Optics/Glass.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <system_error>

namespace Natron {
namespace Optics {

    namespace {

        // Enough JSON for glass.json: objects, arrays, strings, numbers, true, false and null.
        struct JsonValue {
            enum class Kind {
                Null,
                Bool,
                Number,
                String,
                Array,
                Object,
            };
            Kind kind = Kind::Null;
            bool boolean = false;
            double number = 0.;
            std::string string;
            std::vector<JsonValue> array;
            std::map<std::string, JsonValue> object;

            const JsonValue* member(const char* key) const
            {
                const auto it = object.find(key);

                return it == object.end() ? nullptr : &it->second;
            }
        };

        class JsonReader {
        public:
            explicit JsonReader(const std::string& text)
                : _text(text)
            {
            }

            bool read(JsonValue* out, std::string* error)
            {
                if (!value(out, 0)) {
                    *error = "glass.json: " + _error + " at offset " + std::to_string(_pos);

                    return false;
                }
                skipSpace();
                if (_pos != _text.size()) {
                    *error = "glass.json: trailing characters at offset " + std::to_string(_pos);

                    return false;
                }

                return true;
            }

        private:
            const std::string& _text;
            size_t _pos = 0;
            std::string _error;

            bool fail(const char* message)
            {
                _error = message;

                return false;
            }

            void skipSpace()
            {
                while (_pos < _text.size() && (_text[_pos] == ' ' || _text[_pos] == '\t' || _text[_pos] == '\n' || _text[_pos] == '\r')) {
                    ++_pos;
                }
            }

            bool literal(const char* word)
            {
                const size_t n = std::char_traits<char>::length(word);
                if (_text.compare(_pos, n, word) != 0) {
                    return fail("unexpected token");
                }
                _pos += n;

                return true;
            }

            bool string(std::string* out)
            {
                ++_pos;
                while (_pos < _text.size()) {
                    const char c = _text[_pos++];
                    if (c == '"') {
                        return true;
                    }
                    if (c != '\\') {
                        out->push_back(c);
                        continue;
                    }
                    if (_pos >= _text.size()) {
                        break;
                    }
                    const char e = _text[_pos++];
                    switch (e) {
                    case '"':
                    case '\\':
                    case '/':
                        out->push_back(e);
                        break;
                    case 'b':
                        out->push_back('\b');
                        break;
                    case 'f':
                        out->push_back('\f');
                        break;
                    case 'n':
                        out->push_back('\n');
                        break;
                    case 'r':
                        out->push_back('\r');
                        break;
                    case 't':
                        out->push_back('\t');
                        break;
                    case 'u': {
                        if (_pos + 4 > _text.size()) {
                            return fail("truncated \\u escape");
                        }
                        const unsigned cp = (unsigned)std::strtoul(_text.substr(_pos, 4).c_str(), nullptr, 16);
                        _pos += 4;
                        if (cp < 0x80) {
                            out->push_back((char)cp);
                        } else if (cp < 0x800) {
                            out->push_back((char)(0xC0 | (cp >> 6)));
                            out->push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            out->push_back((char)(0xE0 | (cp >> 12)));
                            out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out->push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default:
                        return fail("bad string escape");
                    }
                }

                return fail("unterminated string");
            }

            bool value(JsonValue* out, int depth)
            {
                if (depth > 64) {
                    return fail("nesting too deep");
                }
                skipSpace();
                if (_pos >= _text.size()) {
                    return fail("unexpected end");
                }
                const char c = _text[_pos];
                if (c == '{') {
                    out->kind = JsonValue::Kind::Object;
                    ++_pos;
                    skipSpace();
                    if (_pos < _text.size() && _text[_pos] == '}') {
                        ++_pos;

                        return true;
                    }
                    for (;;) {
                        skipSpace();
                        if (_pos >= _text.size() || _text[_pos] != '"') {
                            return fail("expected a member name");
                        }
                        std::string key;
                        if (!string(&key)) {
                            return false;
                        }
                        skipSpace();
                        if (_pos >= _text.size() || _text[_pos] != ':') {
                            return fail("expected ':'");
                        }
                        ++_pos;
                        if (!value(&out->object[key], depth + 1)) {
                            return false;
                        }
                        skipSpace();
                        if (_pos < _text.size() && _text[_pos] == ',') {
                            ++_pos;
                            continue;
                        }
                        if (_pos < _text.size() && _text[_pos] == '}') {
                            ++_pos;

                            return true;
                        }

                        return fail("expected ',' or '}'");
                    }
                }
                if (c == '[') {
                    out->kind = JsonValue::Kind::Array;
                    ++_pos;
                    skipSpace();
                    if (_pos < _text.size() && _text[_pos] == ']') {
                        ++_pos;

                        return true;
                    }
                    for (;;) {
                        out->array.emplace_back();
                        if (!value(&out->array.back(), depth + 1)) {
                            return false;
                        }
                        skipSpace();
                        if (_pos < _text.size() && _text[_pos] == ',') {
                            ++_pos;
                            continue;
                        }
                        if (_pos < _text.size() && _text[_pos] == ']') {
                            ++_pos;

                            return true;
                        }

                        return fail("expected ',' or ']'");
                    }
                }
                if (c == '"') {
                    out->kind = JsonValue::Kind::String;

                    return string(&out->string);
                }
                if (c == 't' || c == 'f') {
                    out->kind = JsonValue::Kind::Bool;
                    out->boolean = c == 't';

                    return literal(c == 't' ? "true" : "false");
                }
                if (c == 'n') {
                    return literal("null");
                }
                const char* begin = _text.c_str() + _pos;
                char* end = nullptr;
                out->number = std::strtod(begin, &end);
                if (end == begin) {
                    return fail("unexpected character");
                }
                out->kind = JsonValue::Kind::Number;
                _pos += (size_t)(end - begin);

                return true;
            }
        };

        bool
        numberMember(const JsonValue& object,
                     const char* key,
                     double* out)
        {
            const JsonValue* v = object.member(key);
            if (!v || v->kind != JsonValue::Kind::Number) {
                return false;
            }
            *out = v->number;

            return true;
        }

        bool
        stringMember(const JsonValue& object,
                     const char* key,
                     std::string* out)
        {
            const JsonValue* v = object.member(key);
            if (!v || v->kind != JsonValue::Kind::String) {
                return false;
            }
            *out = v->string;

            return true;
        }

    } // namespace

    double
    Glass::ior(double wavelengthNm) const
    {
        if (coefficients.empty()) {
            return nd;
        }
        const double w = wavelengthNm * 1e-3;
        const double w2 = w * w;
        double sum = coefficients[0];
        for (size_t i = 1; i + 1 < coefficients.size(); i += 2) {
            const double b = coefficients[i], c = coefficients[i + 1];
            if (formula == 1) {
                sum += b * w2 / (w2 - c * c);
            } else if (formula == 2) {
                sum += b * w2 / (w2 - c);
            } else {
                sum += b * std::pow(w, c);
            }
        }
        const double n2 = formula == 3 ? sum : sum + 1.;

        return n2 > 0. ? std::sqrt(n2) : nd;
    }

    std::string
    GlassCatalogue::defaultPath()
    {
        std::error_code ec;
        const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) {
            return std::string();
        }

        return (exe.parent_path() / ".." / "Resources" / "optics" / "glass.json").lexically_normal().string();
    }

    bool
    GlassCatalogue::load(const std::string& path,
                         std::string* error)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            *error = "cannot open the glass catalogue " + path;

            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        return parse(text, error);
    }

    bool
    GlassCatalogue::parse(const std::string& text,
                          std::string* error)
    {
        JsonValue root;
        if (!JsonReader(text).read(&root, error)) {
            return false;
        }
        const JsonValue* list = root.kind == JsonValue::Kind::Object ? root.member("glasses") : nullptr;
        if (!list || list->kind != JsonValue::Kind::Array) {
            *error = "glass.json: no \"glasses\" array";

            return false;
        }
        std::vector<Glass> glasses;
        glasses.reserve(list->array.size());
        for (size_t i = 0; i < list->array.size(); ++i) {
            const JsonValue& entry = list->array[i];
            Glass g;
            double formula = 0.;
            const JsonValue* coefficients = entry.member("coefficients");
            const bool ok = entry.kind == JsonValue::Kind::Object && stringMember(entry, "vendor", &g.vendor) && stringMember(entry, "name", &g.name) && numberMember(entry, "nd", &g.nd) && numberMember(entry, "vd", &g.vd) && numberMember(entry, "formula", &formula) && coefficients && coefficients->kind == JsonValue::Kind::Array;
            g.formula = (int)formula;
            if (!ok || g.formula < 1 || g.formula > 3 || coefficients->array.size() % 2 != 1) {
                *error = "glass.json: glass " + std::to_string(i) + " is malformed";

                return false;
            }
            for (const JsonValue& c : coefficients->array) {
                if (c.kind != JsonValue::Kind::Number) {
                    *error = "glass.json: glass " + std::to_string(i) + " has a non-numeric coefficient";

                    return false;
                }
                g.coefficients.push_back(c.number);
            }
            glasses.push_back(std::move(g));
        }
        std::stable_sort(glasses.begin(), glasses.end(), [](const Glass& a, const Glass& b) {
            return a.vendor != b.vendor ? a.vendor < b.vendor : a.name < b.name;
        });
        _glasses = std::move(glasses);

        return true;
    }

    std::vector<std::string>
    GlassCatalogue::vendors() const
    {
        std::vector<std::string> out;
        for (const Glass& g : _glasses) {
            if (out.empty() || out.back() != g.vendor) {
                out.push_back(g.vendor);
            }
        }

        return out;
    }

    const Glass*
    GlassCatalogue::find(const std::string& vendor,
                         const std::string& name) const
    {
        for (const Glass& g : _glasses) {
            if (g.vendor == vendor && g.name == name) {
                return &g;
            }
        }

        return nullptr;
    }

    const Glass*
    GlassCatalogue::nearest(const std::string& vendor,
                            double nd,
                            double vd,
                            double* distance) const
    {
        const Glass* best = nullptr;
        double bestScore = std::numeric_limits<double>::infinity();
        for (const Glass& g : _glasses) {
            if (g.vendor != vendor) {
                continue;
            }
            const double en = 1. - g.nd / nd, ev = 1. - g.vd / vd;
            const double score = en * en + ev * ev;
            if (score < bestScore) {
                bestScore = score;
                best = &g;
            }
        }
        if (distance) {
            *distance = best ? std::sqrt(bestScore) : std::numeric_limits<double>::infinity();
        }

        return best;
    }

    double
    abbeFor(double nd)
    {
        return std::max(22., std::min(66., 64. - 135. * (nd - 1.517)));
    }

    double
    cauchyIor(double nd,
              double vd,
              double wavelengthNm)
    {
        if (nd <= 1. || vd <= 0.) {
            return nd <= 0. ? 1. : nd;
        }
        const double ld = 0.5876, lF = 0.4861, lC = 0.6563, l = wavelengthNm * 1e-3;
        const double b = ((nd - 1.) / vd) / (1. / (lF * lF) - 1. / (lC * lC));

        return nd + b * (1. / (l * l) - 1. / (ld * ld));
    }

    double
    Dispersion::ior(double wavelengthNm) const
    {
        if (glass) {
            return glass->ior(wavelengthNm) + glassOffset;
        }

        return nd <= 1. ? 1. : cauchyIor(nd, vd, wavelengthNm);
    }

    Dispersion
    matchGlass(const GlassCatalogue& catalogue,
               const std::string& vendor,
               double nd,
               double vd,
               double abbeOffset,
               double tolerance)
    {
        Dispersion d;
        if (nd <= 1.) {
            return d;
        }
        // An offset can push Vd to zero or below, where neither the score nor Cauchy is defined.
        const double target = std::max(1., (vd > 0. ? vd : abbeFor(nd)) + abbeOffset);
        d.nd = nd;
        d.vd = target;
        double distance = 0.;
        const Glass* g = catalogue.nearest(vendor, nd, target, &distance);
        if (g && distance <= tolerance) {
            d.glass = *g;
            d.glassOffset = nd - g->ior(kWavelengthDNm);
        }

        return d;
    }

} // namespace Optics
} // namespace Natron
