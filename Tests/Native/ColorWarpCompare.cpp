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

// Renders synthetic charts through every colour warp space and kernel with the same pin sets and
// prints comparison metrics. It is a tool, not a check, so it is disabled by default; run it with
//   Tests --gtest_also_run_disabled_tests --gtest_filter='DISABLED_ColorWarpCompare.*'
// Output goes to NATRON_COLORWARP_COMPARE_DIR, or to build/colorwarp-compare in the source tree.

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Engine/Nodes/Color/ColorWarp/ColorWarpField.h"

NATRON_NAMESPACE_USING

namespace {
const double kRec709ToXYZ[9] = {
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041
};

const int kChartWidth = 512;
const int kCheckerRows = 192;
const int kRampRows = 256;
const int kSweepRows = 128;
const int kPinRows = 32;
const int kChartHeight = kCheckerRows + kRampRows + kSweepRows + kPinRows;
const double kRampLowStop = -7.;
const double kRampHighStop = 7.;

struct Image {
    int width = 0;
    int height = 0;
    std::vector<double> rgb;

    Image(int w, int h)
        : width(w)
        , height(h)
        , rgb(static_cast<std::size_t>(w) * h * 3, 0.)
    {
    }

    double* at(int x, int y) { return &rgb[(static_cast<std::size_t>(y) * width + x) * 3]; }
    const double* at(int x, int y) const { return &rgb[(static_cast<std::size_t>(y) * width + x) * 3]; }
};

double
srgbToLinear(double v)
{
    return (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double
linearToSrgb(double v)
{
    v = std::min(1., std::max(0., v));

    return (v <= 0.0031308) ? v * 12.92 : 1.055 * std::pow(v, 1. / 2.4) - 0.055;
}

double
luminance(const double rgb[3])
{
    return kRec709ToXYZ[3] * rgb[0] + kRec709ToXYZ[4] * rgb[1] + kRec709ToXYZ[5] * rgb[2];
}

// The 24 ColorChecker patches as published sRGB 8-bit values.
const int kCheckerSrgb[24][3] = {
    { 115, 82, 68 }, { 194, 150, 130 }, { 98, 122, 157 }, { 87, 108, 67 }, { 133, 128, 177 }, { 103, 189, 170 }, { 214, 126, 44 }, { 80, 91, 166 }, { 193, 90, 99 }, { 94, 60, 108 }, { 157, 188, 64 }, { 224, 163, 46 }, { 56, 61, 150 }, { 70, 148, 73 }, { 175, 54, 60 }, { 231, 199, 31 }, { 187, 86, 149 }, { 8, 133, 161 }, { 243, 243, 242 }, { 200, 200, 200 }, { 160, 160, 160 }, { 122, 122, 121 }, { 85, 85, 85 }, { 52, 52, 52 }
};

struct PinSet {
    std::string name;
    std::vector<ColorWarpPin> pins;
};

ColorWarpPin
pin(double sr, double sg, double sb, double tr, double tg, double tb)
{
    ColorWarpPin p;
    p.source[0] = sr;
    p.source[1] = sg;
    p.source[2] = sb;
    p.target[0] = tr;
    p.target[1] = tg;
    p.target[2] = tb;

    return p;
}

std::vector<PinSet>
pinSets()
{
    std::vector<PinSet> sets;

    const double hueSource[3] = { 0.40, 0.10, 0.06 };
    double hsv[3], hueTarget[3];
    colorWarpRgbToHsv(hueSource, hsv);
    hsv[0] += 35.;
    colorWarpHsvToRgb(hsv, hueTarget);
    sets.push_back({ "hue", { pin(hueSource[0], hueSource[1], hueSource[2], hueTarget[0], hueTarget[1], hueTarget[2]) } });

    sets.push_back({ "skin", {
                                 pin(0.54, 0.31, 0.22, 0.58, 0.30, 0.17),
                                 pin(0.30, 0.30, 0.30, 0.30, 0.30, 0.30),
                             } });

    sets.push_back({ "look", {
                                 pin(0.54, 0.31, 0.22, 0.58, 0.30, 0.17),
                                 pin(0.10, 0.20, 0.45, 0.06, 0.24, 0.36),
                                 pin(0.10, 0.25, 0.06, 0.16, 0.25, 0.05),
                                 pin(0.18, 0.18, 0.18, 0.18, 0.18, 0.18),
                                 pin(0.80, 0.80, 0.80, 0.80, 0.80, 0.80),
                             } });

    return sets;
}

Image
makeChart(const std::vector<ColorWarpPin>& pins)
{
    Image img(kChartWidth, kChartHeight);

    for (int y = 0; y < kCheckerRows; ++y) {
        const int row = y * 4 / kCheckerRows;
        for (int x = 0; x < kChartWidth; ++x) {
            const int col = x * 6 / kChartWidth;
            const int* s = kCheckerSrgb[row * 6 + col];
            double* p = img.at(x, y);
            for (int c = 0; c < 3; ++c) {
                p[c] = srgbToLinear(s[c] / 255.);
            }
        }
    }

    const double rampHues[6] = { 0., 60., 120., 180., 240., 300. };
    std::vector<std::vector<double>> rampBases;
    for (double h : rampHues) {
        const double hsv[3] = { h, 0.8, 1. };
        std::vector<double> rgb(3);
        colorWarpHsvToRgb(hsv, rgb.data());
        rampBases.push_back(rgb);
    }
    rampBases.push_back({ 0.54, 0.31, 0.22 });
    rampBases.push_back({ 1., 1., 1. });
    const int rampHeight = kRampRows / static_cast<int>(rampBases.size());
    for (int y = 0; y < kRampRows; ++y) {
        const std::vector<double>& base = rampBases[std::min<int>(y / rampHeight, static_cast<int>(rampBases.size()) - 1)];
        const double norm = 0.18 / luminance(base.data());
        for (int x = 0; x < kChartWidth; ++x) {
            const double stop = kRampLowStop + (kRampHighStop - kRampLowStop) * (x + 0.5) / kChartWidth;
            const double k = norm * std::exp2(stop);
            double* p = img.at(x, kCheckerRows + y);
            for (int c = 0; c < 3; ++c) {
                p[c] = base[c] * k;
            }
        }
    }

    for (int y = 0; y < kSweepRows; ++y) {
        const double sat = static_cast<double>(y) / (kSweepRows - 1);
        for (int x = 0; x < kChartWidth; ++x) {
            const double hsv[3] = { 360. * (x + 0.5) / kChartWidth, sat, 0.45 };
            colorWarpHsvToRgb(hsv, img.at(x, kCheckerRows + kRampRows + y));
        }
    }

    const int n = std::max<int>(1, static_cast<int>(pins.size()));
    for (int y = 0; y < kPinRows; ++y) {
        for (int x = 0; x < kChartWidth; ++x) {
            const int i = std::min(n - 1, x * n / kChartWidth);
            double* p = img.at(x, kCheckerRows + kRampRows + kSweepRows + y);
            for (int c = 0; c < 3; ++c) {
                p[c] = pins.empty() ? 0. : pins[i].source[c];
            }
        }
    }

    return img;
}

Image
applyField(const ColorWarpField& field, const Image& src)
{
    Image out(src.width, src.height);

    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            field.apply(src.at(x, y), out.at(x, y));
        }
    }

    return out;
}

double
changedFraction(const Image& a, const Image& b)
{
    const std::size_t pixels = static_cast<std::size_t>(a.width) * a.height;
    std::size_t changed = 0;

    for (std::size_t i = 0; i < pixels; ++i) {
        double diff = 0., mag = 1e-6;
        for (int c = 0; c < 3; ++c) {
            diff = std::max(diff, std::fabs(b.rgb[i * 3 + c] - a.rgb[i * 3 + c]));
            mag = std::max(mag, std::fabs(a.rgb[i * 3 + c]));
        }
        if (diff > 0.01 * mag) {
            ++changed;
        }
    }

    return static_cast<double>(changed) / pixels;
}

// Falloff units differ per space, so a fixed number would compare different localities. Each
// space's reach is instead the one at which the single-hue pin, under the Gaussian kernel,
// changes kCalibratedChangedFraction of the chart; every pin set then uses that reach.
const double kCalibratedChangedFraction = 0.15;

double
calibrateReach(ColorWarpSpaceEnum space, const ColorWarpSpaceBasis& basis, const PinSet& set, const Image& chart)
{
    double lo = std::log(1e-4), hi = std::log(1e3);

    for (int iter = 0; iter < 30; ++iter) {
        const double mid = 0.5 * (lo + hi);
        std::vector<ColorWarpPin> pins = set.pins;
        for (ColorWarpPin& p : pins) {
            p.falloff = std::exp(mid);
        }
        ColorWarpField field;
        field.prepare(pins, space, basis, eColorWarpKernelGaussian);
        if (changedFraction(chart, applyField(field, chart)) < kCalibratedChangedFraction) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    return std::exp(0.5 * (lo + hi));
}

uint16_t
floatToHalf(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;

    if (((bits >> 23) & 0xffu) == 0xffu) {
        return static_cast<uint16_t>(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    }
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7c00u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const int shift = 14 - exponent;
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if ((rest > halfway) || ((rest == halfway) && (half & 1u))) {
            ++half;
        }

        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1fffu;
    if ((rest > 0x1000u) || ((rest == 0x1000u) && (half & 1u))) {
        ++half;
    }

    return static_cast<uint16_t>(sign | half);
}

template <typename T>
void
put(std::string& buf, T v)
{
    char bytes[sizeof(T)];
    std::memcpy(bytes, &v, sizeof(T));
    buf.append(bytes, sizeof(T));
}

void
attribute(std::string& header, const char* name, const char* type, const std::string& value)
{
    header.append(name, std::strlen(name) + 1);
    header.append(type, std::strlen(type) + 1);
    put<int32_t>(header, static_cast<int32_t>(value.size()));
    header += value;
}

// Single-part, scanline, uncompressed, half-float B, G, R: the smallest EXR any reader accepts.
bool
writeExr(const std::string& path, const Image& img)
{
    std::string header;
    put<uint32_t>(header, 20000630u);
    put<uint32_t>(header, 2u);

    std::string channels;
    for (const char* name : { "B", "G", "R" }) {
        channels.append(name, 2);
        put<int32_t>(channels, 1);
        channels.append(4, '\0');
        put<int32_t>(channels, 1);
        put<int32_t>(channels, 1);
    }
    channels.push_back('\0');
    attribute(header, "channels", "chlist", channels);
    attribute(header, "compression", "compression", std::string(1, '\0'));
    std::string box;
    put<int32_t>(box, 0);
    put<int32_t>(box, 0);
    put<int32_t>(box, img.width - 1);
    put<int32_t>(box, img.height - 1);
    attribute(header, "dataWindow", "box2i", box);
    attribute(header, "displayWindow", "box2i", box);
    attribute(header, "lineOrder", "lineOrder", std::string(1, '\0'));
    std::string one;
    put<float>(one, 1.f);
    attribute(header, "pixelAspectRatio", "float", one);
    std::string centre;
    put<float>(centre, 0.f);
    put<float>(centre, 0.f);
    attribute(header, "screenWindowCenter", "v2f", centre);
    attribute(header, "screenWindowWidth", "float", one);
    header.push_back('\0');

    const int32_t lineBytes = img.width * 3 * 2;
    const uint64_t chunkBytes = 8 + static_cast<uint64_t>(lineBytes);
    const uint64_t firstChunk = header.size() + 8ull * img.height;
    for (int y = 0; y < img.height; ++y) {
        put<uint64_t>(header, firstChunk + chunkBytes * y);
    }
    for (int y = 0; y < img.height; ++y) {
        put<int32_t>(header, y);
        put<int32_t>(header, lineBytes);
        for (int c = 2; c >= 0; --c) {
            for (int x = 0; x < img.width; ++x) {
                put<uint16_t>(header, floatToHalf(static_cast<float>(img.at(x, y)[c])));
            }
        }
    }
    std::ofstream out(path, std::ios::binary);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));

    return static_cast<bool>(out);
}

uint32_t
crc32(const std::string& data, std::size_t from)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    uint32_t c = 0xffffffffu;
    for (std::size_t i = from; i < data.size(); ++i) {
        c = table[(c ^ static_cast<uint8_t>(data[i])) & 0xffu] ^ (c >> 8);
    }

    return c ^ 0xffffffffu;
}

void
putBigEndian(std::string& buf, uint32_t v)
{
    buf.push_back(static_cast<char>((v >> 24) & 0xffu));
    buf.push_back(static_cast<char>((v >> 16) & 0xffu));
    buf.push_back(static_cast<char>((v >> 8) & 0xffu));
    buf.push_back(static_cast<char>(v & 0xffu));
}

void
pngChunk(std::string& png, const char* type, const std::string& data)
{
    putBigEndian(png, static_cast<uint32_t>(data.size()));
    const std::size_t start = png.size();
    png.append(type, 4);
    png += data;
    putBigEndian(png, crc32(png, start));
}

// A display preview only: the brightest channel goes through an extended Reinhard curve whose
// white is the top of the HDR ramps, the others keep their ratio to it, then sRGB. The same curve
// is used for every image, so they compare with each other; the EXRs carry the real values.
void
toDisplay(const double rgb[3], uint8_t out[3])
{
    const double white = 0.18 * std::exp2(kRampHighStop);
    const double m = std::max(rgb[0], std::max(rgb[1], rgb[2]));
    const double scale = (m > 0.) ? (m * (1. + m / (white * white)) / (1. + m)) / m : 0.;

    for (int c = 0; c < 3; ++c) {
        out[c] = static_cast<uint8_t>(std::lround(255. * linearToSrgb(std::max(0., rgb[c]) * scale * 1.6)));
    }
}

// Uncompressed (stored-deflate) RGB8, so no zlib is needed; the files are a few MB at most.
bool
writePng(const std::string& path, const std::vector<const Image*>& panels)
{
    const int gap = 8;
    int width = 0;
    int height = 0;
    for (const Image* p : panels) {
        width += p->width;
        height = std::max(height, p->height);
    }
    width += gap * static_cast<int>(panels.size() - 1);

    std::string raw;
    raw.reserve(static_cast<std::size_t>(height) * (width * 3 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back('\0');
        for (std::size_t i = 0; i < panels.size(); ++i) {
            const Image& p = *panels[i];
            for (int x = 0; x < p.width; ++x) {
                uint8_t px[3] = { 0, 0, 0 };
                if (y < p.height) {
                    toDisplay(p.at(x, y), px);
                }
                raw.append(reinterpret_cast<const char*>(px), 3);
            }
            if (i + 1 < panels.size()) {
                raw.append(static_cast<std::size_t>(gap) * 3, '\x40');
            }
        }
    }

    std::string zlib;
    zlib.push_back('\x78');
    zlib.push_back('\x01');
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;) {
        const std::size_t len = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = (pos + len) >= raw.size();
        zlib.push_back(last ? '\x01' : '\x00');
        zlib.push_back(static_cast<char>(len & 0xffu));
        zlib.push_back(static_cast<char>((len >> 8) & 0xffu));
        zlib.push_back(static_cast<char>(~len & 0xffu));
        zlib.push_back(static_cast<char>((~len >> 8) & 0xffu));
        zlib.append(raw, pos, len);
        pos += len;
        if (last) {
            break;
        }
    }
    uint32_t a = 1, b = 0;
    for (unsigned char ch : raw) {
        a = (a + ch) % 65521u;
        b = (b + a) % 65521u;
    }
    putBigEndian(zlib, (b << 16) | a);

    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    putBigEndian(ihdr, static_cast<uint32_t>(width));
    putBigEndian(ihdr, static_cast<uint32_t>(height));
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    pngChunk(png, "IHDR", ihdr);
    pngChunk(png, "IDAT", zlib);
    pngChunk(png, "IEND", std::string());

    std::ofstream out(path, std::ios::binary);
    out.write(png.data(), static_cast<std::streamsize>(png.size()));

    return static_cast<bool>(out);
}

struct Metrics {
    double maxGradLog = 0.;
    double meanGradLog = 0.;
    double maxGradLin = 0.;
    double changedFraction = 0.;
    double neutralDrift = 0.;
    double holdError = 0.;
    double maxOut = 0.;
    int negativesIntroduced = 0;
    int nonFinite = 0;
    std::vector<double> hueByStop;
    std::vector<double> satRatioByStop;
};

double
frobeniusOffIdentity(const double j[3][3])
{
    double acc = 0.;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const double v = j[r][c] - ((r == c) ? 1. : 0.);
            acc += v * v;
        }
    }

    return std::sqrt(acc);
}

// The warp's Jacobian, minus identity, by central differences on a lattice. In the log variant
// both input and output are measured in log-encoded RGB (stops), a common ruler across spaces.
void
gradientMetric(const ColorWarpField& field, bool logLattice, double& maxOut, double& meanOut)
{
    const int n = 17;
    const double lo = logLattice ? colorWarpLogEncode(0.002) : 0.;
    const double hi = logLattice ? colorWarpLogEncode(16.) : 1.;
    const double h = (hi - lo) / 256.;
    auto eval = [&](const double e[3], double o[3]) {
        double rgb[3];
        for (int c = 0; c < 3; ++c) {
            rgb[c] = logLattice ? colorWarpLogDecode(e[c]) : e[c];
        }
        double w[3];
        field.apply(rgb, w);
        for (int c = 0; c < 3; ++c) {
            o[c] = logLattice ? colorWarpLogEncode(w[c]) : w[c];
        }
    };
    double maxG = 0., sumG = 0.;
    int count = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            for (int k = 0; k < n; ++k) {
                const double p[3] = { lo + (hi - lo) * i / (n - 1), lo + (hi - lo) * j / (n - 1), lo + (hi - lo) * k / (n - 1) };
                double jac[3][3];
                for (int axis = 0; axis < 3; ++axis) {
                    double a[3] = { p[0], p[1], p[2] }, b[3] = { p[0], p[1], p[2] };
                    a[axis] += h;
                    b[axis] -= h;
                    double fa[3], fb[3];
                    eval(a, fa);
                    eval(b, fb);
                    for (int c = 0; c < 3; ++c) {
                        jac[c][axis] = (fa[c] - fb[c]) / (2. * h);
                    }
                }
                const double g = frobeniusOffIdentity(jac);
                if (std::isfinite(g)) {
                    maxG = std::max(maxG, g);
                    sumG += g;
                    ++count;
                }
            }
        }
    }
    maxOut = maxG;
    meanOut = count ? sumG / count : 0.;
}

double
wrapDegrees(double d)
{
    d = std::fmod(d, 360.);
    if (d > 180.) {
        d -= 360.;
    } else if (d <= -180.) {
        d += 360.;
    }

    return d;
}

Metrics
measure(const ColorWarpField& field, const Image& chart, const Image& warped, const PinSet& set)
{
    Metrics m;

    gradientMetric(field, true, m.maxGradLog, m.meanGradLog);
    double unusedMean = 0.;
    gradientMetric(field, false, m.maxGradLin, unusedMean);

    m.changedFraction = changedFraction(chart, warped);
    const std::size_t pixels = static_cast<std::size_t>(chart.width) * chart.height;
    for (std::size_t i = 0; i < pixels; ++i) {
        const double* a = &chart.rgb[i * 3];
        const double* b = &warped.rgb[i * 3];
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(b[c])) {
                ++m.nonFinite;
            }
            if ((a[c] >= 0.) && (b[c] < -1e-9)) {
                ++m.negativesIntroduced;
            }
            m.maxOut = std::max(m.maxOut, b[c]);
        }
    }

    const int rampHeight = kRampRows / 8;
    const int neutralY = kCheckerRows + 7 * rampHeight + rampHeight / 2;
    for (int x = 0; x < warped.width; ++x) {
        const double* p = warped.at(x, neutralY);
        const double hi = std::max(p[0], std::max(p[1], p[2]));
        const double lo = std::min(p[0], std::min(p[1], p[2]));
        if (hi > 0.) {
            m.neutralDrift = std::max(m.neutralDrift, (hi - lo) / hi);
        }
    }

    for (const ColorWarpPin& pin : set.pins) {
        const bool hold = (pin.source[0] == pin.target[0]) && (pin.source[1] == pin.target[1]) && (pin.source[2] == pin.target[2]);
        if (!hold) {
            continue;
        }
        double out[3];
        field.apply(pin.source, out);
        for (int c = 0; c < 3; ++c) {
            m.holdError = std::max(m.holdError, std::fabs(out[c] - pin.source[c]));
        }
    }

    const ColorWarpPin& moved = set.pins.front();
    double srcHsv[3], tgtHsv[3];
    colorWarpRgbToHsv(moved.source, srcHsv);
    colorWarpRgbToHsv(moved.target, tgtHsv);
    for (int stop = -6; stop <= 6; stop += 2) {
        const double k = std::exp2(stop);
        const double in[3] = { moved.source[0] * k, moved.source[1] * k, moved.source[2] * k };
        double out[3], inHsv[3], outHsv[3];
        field.apply(in, out);
        colorWarpRgbToHsv(in, inHsv);
        colorWarpRgbToHsv(out, outHsv);
        m.hueByStop.push_back(wrapDegrees(outHsv[0] - inHsv[0]));
        m.satRatioByStop.push_back((inHsv[1] > 0.) ? outHsv[1] / inHsv[1] : 0.);
    }

    return m;
}

std::string
outputDir()
{
    if (const char* env = std::getenv("NATRON_COLORWARP_COMPARE_DIR")) {
        return env;
    }

    return std::string(NATRON_TESTS_ENGINE_DIR) + "/../build/colorwarp-compare";
}
} // anon namespace

TEST(DISABLED_ColorWarpCompare, WriteImagesAndMetrics)
{
    const std::string dir = outputDir();
    std::filesystem::create_directories(dir);
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);

    const Image plain = makeChart({});
    ASSERT_TRUE(writeExr(dir + "/chart_original.exr", plain));
    ASSERT_TRUE(writePng(dir + "/chart_original.png", { &plain }));

    const std::vector<PinSet> sets = pinSets();
    double reach[kColorWarpSpaceCount];
    {
        const Image hueChart = makeChart(sets.front().pins);
        std::ostringstream reaches;
        for (int s = 0; s < kColorWarpSpaceCount; ++s) {
            reach[s] = calibrateReach(static_cast<ColorWarpSpaceEnum>(s), basis, sets.front(), hueChart);
            reaches << colorWarpSpaceName(static_cast<ColorWarpSpaceEnum>(s)) << "\t" << reach[s] << "\n";
        }
        std::ofstream(dir + "/reach.tsv") << reaches.str();
        std::printf("calibrated reach (falloff, warp-space units):\n%s", reaches.str().c_str());
    }

    std::ostringstream table;
    table << "set\tspace\tkernel\tmaxGradLog\tmeanGradLog\tmaxGradLin\tchanged\tneutralDrift\tholdError\tmaxOut\tnegIntroduced\tnonFinite"
          << "\thueShiftByStop(-6..+6)\tsatRatioByStop(-6..+6)\n";
    int written = 0;
    for (const PinSet& set : sets) {
        const Image chart = makeChart(set.pins);
        for (int s = 0; s < kColorWarpSpaceCount; ++s) {
            const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
            std::vector<ColorWarpPin> pins = set.pins;
            for (ColorWarpPin& p : pins) {
                p.falloff = reach[s];
            }
            for (int k = 0; k < kColorWarpKernelCount; ++k) {
                const ColorWarpKernelEnum kernel = static_cast<ColorWarpKernelEnum>(k);
                ColorWarpField field;
                field.prepare(pins, space, basis, kernel);
                const Image warped = applyField(field, chart);
                const std::string stem = dir + "/" + set.name + "_" + colorWarpSpaceName(space) + "_" + colorWarpKernelName(kernel);
                EXPECT_TRUE(writeExr(stem + ".exr", warped));
                EXPECT_TRUE(writePng(stem + ".png", { &chart, &warped }));
                ++written;

                const Metrics m = measure(field, chart, warped, set);
                EXPECT_EQ(m.nonFinite, 0) << stem;
                char line[512];
                std::snprintf(line, sizeof(line), "%s\t%s\t%s\t%.3f\t%.4f\t%.3f\t%.3f\t%.4f\t%.2e\t%.2f\t%d\t%d\t",
                              set.name.c_str(), colorWarpSpaceName(space), colorWarpKernelName(kernel),
                              m.maxGradLog, m.meanGradLog, m.maxGradLin, m.changedFraction, m.neutralDrift, m.holdError,
                              m.maxOut, m.negativesIntroduced, m.nonFinite);
                table << line;
                for (std::size_t i = 0; i < m.hueByStop.size(); ++i) {
                    std::snprintf(line, sizeof(line), "%s%.1f", i ? "," : "", m.hueByStop[i]);
                    table << line;
                }
                table << "\t";
                for (std::size_t i = 0; i < m.satRatioByStop.size(); ++i) {
                    std::snprintf(line, sizeof(line), "%s%.2f", i ? "," : "", m.satRatioByStop[i]);
                    table << line;
                }
                table << "\n";
            }
        }
    }
    EXPECT_EQ(written, 3 * kColorWarpSpaceCount * kColorWarpKernelCount);
    std::ofstream(dir + "/metrics.tsv") << table.str();
    std::printf("%s", table.str().c_str());
}
