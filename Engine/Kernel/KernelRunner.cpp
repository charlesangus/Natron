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

#include "KernelRunner.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string_view>

#include <QThreadPool>

#include "Engine/Noise.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

std::size_t
KernelArrayValue::length() const
{
    if (type == KernelBaseType::eString) {
        return strings.size();
    }
    if (type == KernelBaseType::eInt) {
        return ints.size();
    }
    const int n = std::max(1, kernelComponentCount(type));
    return floats.size() / std::size_t(n);
}

void
KernelArrayValue::clear()
{
    floats.clear();
    ints.clear();
    strings.clear();
}

KernelRegisters::KernelRegisters(const KernelProgram& program)
    : _f(std::size_t(std::max(program.floatRegisters, 0)) * kKernelLaneCount, 0.f)
    , _i(std::size_t(std::max(program.intRegisters, 1)) * kKernelLaneCount, 0)
    , _s(std::size_t(std::max(program.stringRegisters, 0)) * kKernelLaneCount)
    , _a(std::size_t(std::max(program.arrayRegisters, 0)) * kKernelLaneCount)
{
}

std::optional<std::int32_t>
kernelConstantInt(const KernelProgram& program, KernelOperand operand)
{
    if (operand.bank != KernelBank::eInt) {
        return std::nullopt;
    }
    for (std::uint32_t k = 0; k < program.prologueSize && k < program.instructions.size(); ++k) {
        const KernelInstruction& in = program.instructions[k];
        if (in.op == KernelOp::eIConst && in.d == operand.reg) {
            return in.imm;
        }
    }
    return std::nullopt;
}

std::optional<float>
kernelConstantFloat(const KernelProgram& program, KernelOperand operand)
{
    if (operand.bank != KernelBank::eFloat) {
        return std::nullopt;
    }
    for (std::uint32_t k = 0; k < program.prologueSize && k < program.instructions.size(); ++k) {
        const KernelInstruction& in = program.instructions[k];
        if (in.op == KernelOp::eFConst && in.d == operand.reg) {
            return in.fimm;
        }
    }
    return std::nullopt;
}

const std::string*
kernelConstantString(const KernelProgram& program, KernelOperand operand)
{
    if (operand.bank != KernelBank::eString) {
        return nullptr;
    }
    for (std::uint32_t k = 0; k < program.prologueSize && k < program.instructions.size(); ++k) {
        const KernelInstruction& in = program.instructions[k];
        if (in.op == KernelOp::eSConst && in.d == operand.reg && in.imm >= 0
            && std::size_t(in.imm) < program.strings.size()) {
            return &program.strings[std::size_t(in.imm)];
        }
    }
    return nullptr;
}

namespace {

struct LoopAborted { };

// Integer ops wrap like two's complement hardware instead of being undefined on overflow.
inline std::int32_t
wrapAdd(std::int32_t a, std::int32_t b)
{
    return std::int32_t(std::uint32_t(a) + std::uint32_t(b));
}

inline std::int32_t
wrapSub(std::int32_t a, std::int32_t b)
{
    return std::int32_t(std::uint32_t(a) - std::uint32_t(b));
}

inline std::int32_t
wrapMul(std::int32_t a, std::int32_t b)
{
    return std::int32_t(std::uint32_t(a) * std::uint32_t(b));
}

inline std::int32_t
safeDiv(std::int32_t a, std::int32_t b)
{
    if (b == 0) {
        return 0;
    }
    if (b == -1) {
        return wrapSub(0, a);
    }
    return a / b;
}

inline std::int32_t
safeMod(std::int32_t a, std::int32_t b)
{
    if (b == 0 || b == -1) {
        return 0;
    }
    return a % b;
}

inline std::int32_t
floatToInt(float v)
{
    if (std::isnan(v)) {
        return 0;
    }
    if (v >= 2147483648.f) {
        return INT_MAX;
    }
    if (v <= -2147483648.f) {
        return INT_MIN;
    }
    return std::int32_t(v);
}

inline float
fsign(float v)
{
    return v > 0.f ? 1.f : (v < 0.f ? -1.f : 0.f);
}

inline std::uint32_t
hash32(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

inline std::uint32_t
floatBits(float v)
{
    if (v == 0.f) {
        v = 0.f;
    }
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

inline float
unitFloat(std::uint32_t h)
{
    return float(h >> 8) * (1.f / 16777216.f);
}

// ---------------------------------------------------------------------------------------------
// Matrices, row-major

double
det3(const double* m)
{
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

double
det4(const double* m)
{
    double sub[9];
    double d = 0.;
    for (int c = 0; c < 4; ++c) {
        int k = 0;
        for (int r = 1; r < 4; ++r) {
            for (int cc = 0; cc < 4; ++cc) {
                if (cc != c) {
                    sub[k++] = m[r * 4 + cc];
                }
            }
        }
        const double term = m[c] * det3(sub);
        d += (c & 1) ? -term : term;
    }
    return d;
}

// Gauss-Jordan with partial pivoting; a singular matrix gives all zeros.
void
invertMatrix(const double* in, double* out, int n)
{
    double a[4][8];
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            a[r][c] = in[r * n + c];
            a[r][c + n] = r == c ? 1. : 0.;
        }
    }
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (a[pivot][col] == 0.) {
            std::fill(out, out + n * n, 0.);
            return;
        }
        if (pivot != col) {
            for (int c = 0; c < 2 * n; ++c) {
                std::swap(a[pivot][c], a[col][c]);
            }
        }
        const double inv = 1. / a[col][col];
        for (int c = 0; c < 2 * n; ++c) {
            a[col][c] *= inv;
        }
        for (int r = 0; r < n; ++r) {
            if (r == col || a[r][col] == 0.) {
                continue;
            }
            const double f = a[r][col];
            for (int c = 0; c < 2 * n; ++c) {
                a[r][c] -= f * a[col][c];
            }
        }
    }
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            out[r * n + c] = a[r][c + n];
        }
    }
}

// ---------------------------------------------------------------------------------------------
// sprintf

// Bounds a field width or precision, which would otherwise let one conversion allocate gigabytes.
constexpr long kMaxFieldWidth = 1024;

template <class... Args>
std::string
formatted(const std::string& format, Args... args)
{
    const int n = std::snprintf(nullptr, 0, format.c_str(), args...);
    if (n <= 0) {
        return std::string();
    }
    std::string out(std::size_t(n) + 1, '\0');
    std::snprintf(out.data(), out.size(), format.c_str(), args...);
    out.resize(std::size_t(n));
    return out;
}

std::string
operandText(const KernelRegisters& regs, KernelOperand o, int lane, char conversion, const std::string& spec)
{
    switch (conversion) {
    case 'd':
    case 'i':
    case 'x':
    case 'X':
    case 'o':
    case 'u':
    case 'c': {
        long long v = 0;
        if (o.bank == KernelBank::eInt) {
            v = regs.i(o.reg)[lane];
        } else if (o.bank == KernelBank::eFloat) {
            v = floatToInt(regs.f(o.reg)[lane]);
        } else if (o.bank == KernelBank::eString) {
            v = std::strtoll(regs.s(o.reg)[lane].c_str(), nullptr, 10);
        }
        if (conversion == 'c') {
            return formatted(spec + 'c', int(v));
        }
        return formatted(spec + "ll" + conversion, v);
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G': {
        double v = 0.;
        if (o.bank == KernelBank::eFloat) {
            v = regs.f(o.reg)[lane];
        } else if (o.bank == KernelBank::eInt) {
            v = regs.i(o.reg)[lane];
        } else if (o.bank == KernelBank::eString) {
            v = std::strtod(regs.s(o.reg)[lane].c_str(), nullptr);
        }
        return formatted(spec + conversion, v);
    }
    default: {
        std::string text;
        if (o.bank == KernelBank::eString) {
            text = regs.s(o.reg)[lane];
        } else if (o.bank == KernelBank::eInt) {
            text = std::to_string(regs.i(o.reg)[lane]);
        } else if (o.bank == KernelBank::eFloat) {
            text = formatted("%g", double(regs.f(o.reg)[lane]));
        }
        if (spec.size() > 1) {
            return formatted(spec + 's', text.c_str());
        }
        return text;
    }
    }
}

// Each conversion takes the next operand, so a vector argument fills as many conversions as it has
// components.
std::string
formatLane(const KernelRegisters& regs, const std::string& format, std::span<const KernelOperand> args, int lane)
{
    const auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    // strchr also matches the terminator, so an embedded NUL must be ruled out first.
    const auto isOneOf = [](char c, const char* set) { return c != '\0' && std::strchr(set, c) != nullptr; };
    std::string out;
    std::size_t next = 0;
    for (std::size_t k = 0; k < format.size(); ++k) {
        const char ch = format[k];
        if (ch != '%') {
            out += ch;
            continue;
        }
        if (k + 1 < format.size() && format[k + 1] == '%') {
            out += '%';
            ++k;
            continue;
        }
        std::size_t j = k + 1;
        const auto number = [&]() {
            long v = 0;
            while (j < format.size() && isDigit(format[j])) {
                v = std::min(kMaxFieldWidth, v * 10 + (format[j] - '0'));
                ++j;
            }
            return v;
        };
        std::string spec = "%";
        while (j < format.size() && isOneOf(format[j], "-+ #0")) {
            spec += format[j++];
        }
        if (j < format.size() && isDigit(format[j])) {
            spec += std::to_string(number());
        }
        if (j < format.size() && format[j] == '.') {
            ++j;
            spec += '.';
            spec += std::to_string(number());
        }
        while (j < format.size() && isOneOf(format[j], "hlLqjzt")) {
            ++j;
        }
        if (j >= format.size()) {
            out += format.substr(k);
            break;
        }
        const char conversion = format[j];
        if (!isOneOf(conversion, "diouxXcfFeEgGs")) {
            out += format.substr(k, j - k + 1);
            k = j;
            continue;
        }
        if (next < args.size()) {
            out += operandText(regs, args[next++], lane, conversion, spec);
        }
        k = j;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// The machine

struct Machine {
    const KernelProgram& program;
    KernelRegisters regs;
    KernelRunWorker& worker;
    const std::function<bool()>& aborted;
    const std::atomic<bool>& stop;
    std::uint32_t backJumps = 0;
    std::vector<int> lanes;

    Machine(const KernelProgram& p, KernelRunWorker& w, const std::function<bool()>& abortedFn, const std::atomic<bool>& stopFlag)
        : program(p)
        , regs(p)
        , worker(w)
        , aborted(abortedFn)
        , stop(stopFlag)
    {
        lanes.reserve(kKernelLaneCount);
    }

    // The active lanes of a call, so an expensive builtin only runs where the mask is set.
    void activeLanes(const std::int32_t* mask, int n)
    {
        lanes.clear();
        for (int l = 0; l < n; ++l) {
            if (mask[l]) {
                lanes.push_back(l);
            }
        }
    }
};

float*
fop(Machine& m, std::span<const KernelOperand> ops, std::size_t k)
{
    return m.regs.f(ops[k].reg);
}

void
callCore(Machine& m, const KernelInstruction& in, const KernelProgramFunction& fn, int n)
{
    const std::int32_t* mask = m.regs.i(in.a);
    const std::span<const KernelOperand> all(m.program.operands.data() + in.operandBegin, in.operandCount);
    const std::size_t resultCount = std::size_t(std::max(in.b, 0));
    if (resultCount > all.size()) {
        throw KernelRunError("malformed call instruction");
    }
    const std::span<const KernelOperand> res = all.subspan(0, resultCount);
    const std::span<const KernelOperand> args = all.subspan(resultCount);
    const auto need = [&](std::size_t r, std::size_t a) {
        if (res.size() < r || args.size() < a) {
            throw KernelRunError("malformed call to '" + fn.signature.name + "'");
        }
    };
    m.activeLanes(mask, n);
    const std::vector<int>& lanes = m.lanes;
    switch (KernelCoreFunction(fn.signature.id)) {
    case KernelCoreFunction::eNoiseFloat: {
        need(1, 1);
        float* d = fop(m, res, 0);
        const float* x = fop(m, args, 0);
        for (int l : lanes) {
            const double p = x[l];
            double v = 0.;
            Noise<1, 1, double>(&p, &v);
            d[l] = float(0.5 + 0.5 * v);
        }
        break;
    }
    case KernelCoreFunction::eNoiseVector: {
        need(1, 3);
        float* d = fop(m, res, 0);
        const float* x = fop(m, args, 0);
        const float* y = fop(m, args, 1);
        const float* z = fop(m, args, 2);
        for (int l : lanes) {
            const double p[3] = { x[l], y[l], z[l] };
            double v = 0.;
            Noise<3, 1, double>(p, &v);
            d[l] = float(0.5 + 0.5 * v);
        }
        break;
    }
    case KernelCoreFunction::eNoiseVectorToVector: {
        need(3, 3);
        float* d[3] = { fop(m, res, 0), fop(m, res, 1), fop(m, res, 2) };
        const float* x = fop(m, args, 0);
        const float* y = fop(m, args, 1);
        const float* z = fop(m, args, 2);
        for (int l : lanes) {
            const double p[3] = { x[l], y[l], z[l] };
            double v[3] = { 0., 0., 0. };
            Noise<3, 3, double>(p, v);
            for (int c = 0; c < 3; ++c) {
                d[c][l] = float(0.5 + 0.5 * v[c]);
            }
        }
        break;
    }
    case KernelCoreFunction::eRandFloat:
    case KernelCoreFunction::eRandFloatToVector:
    case KernelCoreFunction::eRandVector:
    case KernelCoreFunction::eRandVectorToVector: {
        const KernelCoreFunction id = KernelCoreFunction(fn.signature.id);
        const bool vectorSeed = id == KernelCoreFunction::eRandVector || id == KernelCoreFunction::eRandVectorToVector;
        const bool vectorResult = id == KernelCoreFunction::eRandFloatToVector || id == KernelCoreFunction::eRandVectorToVector;
        const std::size_t seeds = vectorSeed ? 3 : 1;
        const std::size_t outs = vectorResult ? 3 : 1;
        need(outs, seeds);
        for (int l : lanes) {
            std::uint32_t h = 0x2545f491u;
            for (std::size_t s = 0; s < seeds; ++s) {
                h = hash32(h ^ floatBits(fop(m, args, s)[l]));
            }
            for (std::size_t c = 0; c < outs; ++c) {
                fop(m, res, c)[l] = unitFloat(hash32(h + std::uint32_t(c) * 0x9e3779b9u));
            }
        }
        break;
    }
    case KernelCoreFunction::eSprintf: {
        need(1, 1);
        std::string* d = m.regs.s(res[0].reg);
        const std::string* format = m.regs.s(args[0].reg);
        for (int l : lanes) {
            d[l] = formatLane(m.regs, format[l], args.subspan(1), l);
        }
        break;
    }
    case KernelCoreFunction::eStrlen: {
        need(1, 1);
        std::int32_t* d = m.regs.i(res[0].reg);
        const std::string* s = m.regs.s(args[0].reg);
        for (int l : lanes) {
            d[l] = std::int32_t(std::min<std::size_t>(s[l].size(), INT_MAX));
        }
        break;
    }
    case KernelCoreFunction::eItoa: {
        need(1, 1);
        std::string* d = m.regs.s(res[0].reg);
        const std::int32_t* v = m.regs.i(args[0].reg);
        for (int l : lanes) {
            d[l] = std::to_string(v[l]);
        }
        break;
    }
    case KernelCoreFunction::eAtoi: {
        need(1, 1);
        std::int32_t* d = m.regs.i(res[0].reg);
        const std::string* s = m.regs.s(args[0].reg);
        for (int l : lanes) {
            const long long v = std::strtoll(s[l].c_str(), nullptr, 10);
            d[l] = std::int32_t(std::clamp<long long>(v, INT_MIN, INT_MAX));
        }
        break;
    }
    case KernelCoreFunction::eAtof: {
        need(1, 1);
        float* d = m.regs.f(res[0].reg);
        const std::string* s = m.regs.s(args[0].reg);
        for (int l : lanes) {
            d[l] = float(std::strtod(s[l].c_str(), nullptr));
        }
        break;
    }
    case KernelCoreFunction::eStartsWith:
    case KernelCoreFunction::eEndsWith: {
        need(1, 2);
        const bool starts = KernelCoreFunction(fn.signature.id) == KernelCoreFunction::eStartsWith;
        std::int32_t* d = m.regs.i(res[0].reg);
        const std::string* s = m.regs.s(args[0].reg);
        const std::string* t = m.regs.s(args[1].reg);
        for (int l : lanes) {
            const std::string_view sv(s[l]);
            d[l] = starts ? sv.starts_with(t[l]) : sv.ends_with(t[l]);
        }
        break;
    }
    case KernelCoreFunction::eInvert3:
    case KernelCoreFunction::eInvert4:
    case KernelCoreFunction::eDeterminant3:
    case KernelCoreFunction::eDeterminant4: {
        const KernelCoreFunction id = KernelCoreFunction(fn.signature.id);
        const int dim = (id == KernelCoreFunction::eInvert3 || id == KernelCoreFunction::eDeterminant3) ? 3 : 4;
        const bool invert = id == KernelCoreFunction::eInvert3 || id == KernelCoreFunction::eInvert4;
        const std::size_t size = std::size_t(dim * dim);
        need(invert ? size : 1, size);
        for (int l : lanes) {
            double mat[16];
            for (std::size_t c = 0; c < size; ++c) {
                mat[c] = fop(m, args, c)[l];
            }
            if (invert) {
                double inv[16];
                invertMatrix(mat, inv, dim);
                for (std::size_t c = 0; c < size; ++c) {
                    fop(m, res, c)[l] = float(inv[c]);
                }
            } else {
                fop(m, res, 0)[l] = float(dim == 3 ? det3(mat) : det4(mat));
            }
        }
        break;
    }
    case KernelCoreFunction::eQuaternionAxisAngle: {
        need(4, 4);
        for (int l : lanes) {
            const double angle = fop(m, args, 0)[l];
            double axis[3] = { fop(m, args, 1)[l], fop(m, args, 2)[l], fop(m, args, 3)[l] };
            const double len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            double q[4] = { 0., 0., 0., 1. };
            if (len > 0.) {
                const double s = std::sin(angle * 0.5) / len;
                q[0] = axis[0] * s;
                q[1] = axis[1] * s;
                q[2] = axis[2] * s;
                q[3] = std::cos(angle * 0.5);
            }
            for (std::size_t c = 0; c < 4; ++c) {
                fop(m, res, c)[l] = float(q[c]);
            }
        }
        break;
    }
    case KernelCoreFunction::eQuaternionMultiply: {
        need(4, 8);
        for (int l : lanes) {
            double a[4];
            double b[4];
            for (std::size_t c = 0; c < 4; ++c) {
                a[c] = fop(m, args, c)[l];
                b[c] = fop(m, args, 4 + c)[l];
            }
            const double q[4] = {
                a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
            };
            for (std::size_t c = 0; c < 4; ++c) {
                fop(m, res, c)[l] = float(q[c]);
            }
        }
        break;
    }
    case KernelCoreFunction::eQuaternionRotate: {
        need(3, 7);
        for (int l : lanes) {
            const double qx = fop(m, args, 0)[l];
            const double qy = fop(m, args, 1)[l];
            const double qz = fop(m, args, 2)[l];
            const double qw = fop(m, args, 3)[l];
            const double vx = fop(m, args, 4)[l];
            const double vy = fop(m, args, 5)[l];
            const double vz = fop(m, args, 6)[l];
            const double tx = 2. * (qy * vz - qz * vy);
            const double ty = 2. * (qz * vx - qx * vz);
            const double tz = 2. * (qx * vy - qy * vx);
            fop(m, res, 0)[l] = float(vx + qw * tx + (qy * tz - qz * ty));
            fop(m, res, 1)[l] = float(vy + qw * ty + (qz * tx - qx * tz));
            fop(m, res, 2)[l] = float(vz + qw * tz + (qx * ty - qy * tx));
        }
        break;
    }
    default:
        throw KernelRunError("unknown core function '" + fn.signature.name + "'");
    }
}

// ---------------------------------------------------------------------------------------------
// Arrays

int
elementComponents(KernelBaseType t)
{
    return std::max(1, kernelComponentCount(t));
}

void
setArrayType(KernelArrayValue& v, KernelBaseType t)
{
    if (v.type != t) {
        v.clear();
        v.type = t;
    }
}

void
resizeArray(KernelArrayValue& v, std::size_t length)
{
    if (v.type == KernelBaseType::eString) {
        v.strings.resize(length);
    } else if (v.type == KernelBaseType::eInt) {
        v.ints.resize(length, 0);
    } else {
        v.floats.resize(length * std::size_t(elementComponents(v.type)), 0.f);
    }
}

// A negative index counts from the end; -1 when out of range.
std::int64_t
arrayIndex(std::int64_t index, std::size_t length)
{
    if (index < 0) {
        index += std::int64_t(length);
    }
    return index;
}

void
arrayGet(Machine& m, const KernelInstruction& in, int n)
{
    const KernelBaseType t = KernelBaseType(in.imm);
    const int comps = elementComponents(t);
    const KernelOperand* ops = m.program.operands.data() + in.operandBegin;
    const KernelArrayValue* arr = m.regs.a(in.a);
    const std::int32_t* idx = m.regs.i(in.b);
    for (int l = 0; l < n; ++l) {
        const KernelArrayValue& v = arr[l];
        const std::size_t len = v.type == t ? v.length() : 0;
        const std::int64_t k = arrayIndex(idx[l], len);
        const bool inside = k >= 0 && std::size_t(k) < len;
        for (std::uint32_t c = 0; c < in.operandCount; ++c) {
            const KernelOperand o = ops[c];
            switch (o.bank) {
            case KernelBank::eFloat:
                m.regs.f(o.reg)[l] = inside && int(c) < comps ? v.floats[std::size_t(k) * std::size_t(comps) + c] : 0.f;
                break;
            case KernelBank::eInt:
                m.regs.i(o.reg)[l] = inside ? v.ints[std::size_t(k)] : 0;
                break;
            case KernelBank::eString:
                m.regs.s(o.reg)[l] = inside ? v.strings[std::size_t(k)] : std::string();
                break;
            case KernelBank::eArray:
                break;
            }
        }
    }
}

void
storeElement(Machine& m, KernelArrayValue& v, std::size_t k, const KernelOperand* ops, std::uint32_t count, int l)
{
    const int comps = elementComponents(v.type);
    for (std::uint32_t c = 0; c < count; ++c) {
        const KernelOperand o = ops[c];
        switch (o.bank) {
        case KernelBank::eFloat:
            if (int(c) < comps && v.type != KernelBaseType::eInt && v.type != KernelBaseType::eString) {
                v.floats[k * std::size_t(comps) + c] = m.regs.f(o.reg)[l];
            }
            break;
        case KernelBank::eInt:
            if (v.type == KernelBaseType::eInt) {
                v.ints[k] = m.regs.i(o.reg)[l];
            }
            break;
        case KernelBank::eString:
            if (v.type == KernelBaseType::eString) {
                v.strings[k] = m.regs.s(o.reg)[l];
            }
            break;
        case KernelBank::eArray:
            break;
        }
    }
}

void
arraySet(Machine& m, const KernelInstruction& in, int n)
{
    const KernelBaseType t = KernelBaseType(in.imm);
    const KernelOperand* ops = m.program.operands.data() + in.operandBegin;
    const std::int32_t* mask = m.regs.i(in.a);
    KernelArrayValue* arr = m.regs.a(in.b);
    const std::int32_t* idx = m.regs.i(in.c);
    for (int l = 0; l < n; ++l) {
        if (!mask[l]) {
            continue;
        }
        KernelArrayValue& v = arr[l];
        setArrayType(v, t);
        const std::size_t len = v.length();
        const std::int64_t k = arrayIndex(idx[l], len);
        if (k < 0) {
            continue;
        }
        if (std::size_t(k) >= len) {
            resizeArray(v, std::size_t(k) + 1);
        }
        storeElement(m, v, std::size_t(k), ops, in.operandCount, l);
    }
}

void
arrayPush(Machine& m, const KernelInstruction& in, int n)
{
    const KernelBaseType t = KernelBaseType(in.imm);
    const KernelOperand* ops = m.program.operands.data() + in.operandBegin;
    const std::int32_t* mask = m.regs.i(in.a);
    KernelArrayValue* arr = m.regs.a(in.b);
    for (int l = 0; l < n; ++l) {
        if (!mask[l]) {
            continue;
        }
        KernelArrayValue& v = arr[l];
        setArrayType(v, t);
        const std::size_t len = v.length();
        resizeArray(v, len + 1);
        storeElement(m, v, len, ops, in.operandCount, l);
    }
}

void
arrayResize(Machine& m, const KernelInstruction& in, int n)
{
    const KernelBaseType t = KernelBaseType(in.imm);
    const std::int32_t* mask = m.regs.i(in.a);
    KernelArrayValue* arr = m.regs.a(in.b);
    const std::int32_t* size = m.regs.i(in.c);
    for (int l = 0; l < n; ++l) {
        if (!mask[l]) {
            continue;
        }
        setArrayType(arr[l], t);
        resizeArray(arr[l], std::size_t(std::max(size[l], 0)));
    }
}

[[noreturn]] void
throwStopped()
{
    throw LoopAborted();
}

void
pollInLoop(Machine& m)
{
    if ((++m.backJumps & 1023u) != 0) {
        return;
    }
    if (m.stop.load(std::memory_order_relaxed) || (m.aborted && m.aborted())) {
        throwStopped();
    }
}

#define KERNEL_F1(EXPR)               \
    {                                 \
        float* d = R.f(in.d);         \
        const float* pa = R.f(in.a);  \
        for (int l = 0; l < n; ++l) { \
            const float x = pa[l];    \
            d[l] = (EXPR);            \
        }                             \
        break;                        \
    }

#define KERNEL_F2(EXPR)               \
    {                                 \
        float* d = R.f(in.d);         \
        const float* pa = R.f(in.a);  \
        const float* pb = R.f(in.b);  \
        for (int l = 0; l < n; ++l) { \
            const float x = pa[l];    \
            const float y = pb[l];    \
            d[l] = (EXPR);            \
        }                             \
        break;                        \
    }

#define KERNEL_FCMP(EXPR)             \
    {                                 \
        std::int32_t* d = R.i(in.d);  \
        const float* pa = R.f(in.a);  \
        const float* pb = R.f(in.b);  \
        for (int l = 0; l < n; ++l) { \
            const float x = pa[l];    \
            const float y = pb[l];    \
            d[l] = (EXPR) ? 1 : 0;    \
        }                             \
        break;                        \
    }

#define KERNEL_I1(EXPR)                     \
    {                                       \
        std::int32_t* d = R.i(in.d);        \
        const std::int32_t* pa = R.i(in.a); \
        for (int l = 0; l < n; ++l) {       \
            const std::int32_t x = pa[l];   \
            d[l] = (EXPR);                  \
        }                                   \
        break;                              \
    }

#define KERNEL_I2(EXPR)                     \
    {                                       \
        std::int32_t* d = R.i(in.d);        \
        const std::int32_t* pa = R.i(in.a); \
        const std::int32_t* pb = R.i(in.b); \
        for (int l = 0; l < n; ++l) {       \
            const std::int32_t x = pa[l];   \
            const std::int32_t y = pb[l];   \
            d[l] = (EXPR);                  \
        }                                   \
        break;                              \
    }

// Instructions [begin, end) over lanes [0, n). Written once and instantiated for the baseline and
// the x86-64-v3 entry points below; it must stay free of fused multiply-adds (see eFMulAdd).
[[gnu::always_inline]] inline void
executeImpl(Machine& m, std::uint32_t begin, std::uint32_t end, int n)
{
    const std::vector<KernelInstruction>& code = m.program.instructions;
    KernelRegisters& R = m.regs;
    std::uint32_t pc = begin;
    while (pc < end) {
        const KernelInstruction& in = code[pc];
        ++pc;
        switch (in.op) {
        case KernelOp::eFConst: {
            float* d = R.f(in.d);
            std::fill(d, d + n, in.fimm);
            break;
        }
        case KernelOp::eIConst: {
            std::int32_t* d = R.i(in.d);
            std::fill(d, d + n, in.imm);
            break;
        }
        case KernelOp::eSConst: {
            std::string* d = R.s(in.d);
            const std::string& v = m.program.strings.at(std::size_t(in.imm));
            for (int l = 0; l < n; ++l) {
                d[l] = v;
            }
            break;
        }
        case KernelOp::eFMov:
            if (in.d != in.a) {
                std::memmove(R.f(in.d), R.f(in.a), std::size_t(n) * sizeof(float));
            }
            break;
        case KernelOp::eIMov:
            if (in.d != in.a) {
                std::memmove(R.i(in.d), R.i(in.a), std::size_t(n) * sizeof(std::int32_t));
            }
            break;
        case KernelOp::eSMov:
            if (in.d != in.a) {
                std::copy(R.s(in.a), R.s(in.a) + n, R.s(in.d));
            }
            break;
        case KernelOp::eACopy:
            if (in.d != in.a) {
                std::copy(R.a(in.a), R.a(in.a) + n, R.a(in.d));
            }
            break;
        case KernelOp::eFSel: {
            float* d = R.f(in.d);
            const std::int32_t* c = R.i(in.a);
            const float* pb = R.f(in.b);
            const float* pc2 = R.f(in.c);
            for (int l = 0; l < n; ++l) {
                d[l] = c[l] ? pb[l] : pc2[l];
            }
            break;
        }
        case KernelOp::eISel: {
            std::int32_t* d = R.i(in.d);
            const std::int32_t* c = R.i(in.a);
            const std::int32_t* pb = R.i(in.b);
            const std::int32_t* pc2 = R.i(in.c);
            for (int l = 0; l < n; ++l) {
                d[l] = c[l] ? pb[l] : pc2[l];
            }
            break;
        }
        case KernelOp::eSSel: {
            std::string* d = R.s(in.d);
            const std::int32_t* c = R.i(in.a);
            const std::string* pb = R.s(in.b);
            const std::string* pc2 = R.s(in.c);
            for (int l = 0; l < n; ++l) {
                const std::string& src = c[l] ? pb[l] : pc2[l];
                if (&src != &d[l]) {
                    d[l] = src;
                }
            }
            break;
        }
        case KernelOp::eASel: {
            KernelArrayValue* d = R.a(in.d);
            const std::int32_t* c = R.i(in.a);
            const KernelArrayValue* pb = R.a(in.b);
            const KernelArrayValue* pc2 = R.a(in.c);
            for (int l = 0; l < n; ++l) {
                const KernelArrayValue& src = c[l] ? pb[l] : pc2[l];
                if (&src != &d[l]) {
                    d[l] = src;
                }
            }
            break;
        }
        case KernelOp::eFAdd:
            KERNEL_F2(x + y)
        case KernelOp::eFSub:
            KERNEL_F2(x - y)
        case KernelOp::eFMul:
            KERNEL_F2(x * y)
        case KernelOp::eFDiv:
            KERNEL_F2(x / y)
        case KernelOp::eFMod:
            KERNEL_F2(std::fmod(x, y))
        case KernelOp::eFMulAdd: {
            float* d = R.f(in.d);
            const float* pa = R.f(in.a);
            const float* pb = R.f(in.b);
            const float* pc2 = R.f(in.c);
            for (int l = 0; l < n; ++l) {
                const float product = pa[l] * pb[l];
                d[l] = product + pc2[l];
            }
            break;
        }
        case KernelOp::eFNeg:
            KERNEL_F1(-x)
        case KernelOp::eFMin:
            KERNEL_F2(std::fmin(x, y))
        case KernelOp::eFMax:
            KERNEL_F2(std::fmax(x, y))
        case KernelOp::eFPow:
            KERNEL_F2(std::pow(x, y))
        case KernelOp::eFAtan2:
            KERNEL_F2(std::atan2(x, y))
        case KernelOp::eFAbs:
            KERNEL_F1(std::fabs(x))
        case KernelOp::eFFloor:
            KERNEL_F1(std::floor(x))
        case KernelOp::eFCeil:
            KERNEL_F1(std::ceil(x))
        case KernelOp::eFRound:
            KERNEL_F1(std::round(x))
        case KernelOp::eFTrunc:
            KERNEL_F1(std::trunc(x))
        case KernelOp::eFFrac:
            KERNEL_F1(x - std::floor(x))
        case KernelOp::eFSign:
            KERNEL_F1(fsign(x))
        case KernelOp::eFSqrt:
            KERNEL_F1(std::sqrt(x))
        case KernelOp::eFExp:
            KERNEL_F1(std::exp(x))
        case KernelOp::eFLog:
            KERNEL_F1(std::log(x))
        case KernelOp::eFLog10:
            KERNEL_F1(std::log10(x))
        case KernelOp::eFSin:
            KERNEL_F1(std::sin(x))
        case KernelOp::eFCos:
            KERNEL_F1(std::cos(x))
        case KernelOp::eFTan:
            KERNEL_F1(std::tan(x))
        case KernelOp::eFAsin:
            KERNEL_F1(std::asin(x))
        case KernelOp::eFAcos:
            KERNEL_F1(std::acos(x))
        case KernelOp::eFAtan:
            KERNEL_F1(std::atan(x))
        case KernelOp::eFLt:
            KERNEL_FCMP(x < y)
        case KernelOp::eFLe:
            KERNEL_FCMP(x <= y)
        case KernelOp::eFGt:
            KERNEL_FCMP(x > y)
        case KernelOp::eFGe:
            KERNEL_FCMP(x >= y)
        case KernelOp::eFEq:
            KERNEL_FCMP(x == y)
        case KernelOp::eFNe:
            KERNEL_FCMP(x != y)
        case KernelOp::eIAdd:
            KERNEL_I2(wrapAdd(x, y))
        case KernelOp::eISub:
            KERNEL_I2(wrapSub(x, y))
        case KernelOp::eIMul:
            KERNEL_I2(wrapMul(x, y))
        case KernelOp::eIDiv:
            KERNEL_I2(safeDiv(x, y))
        case KernelOp::eIMod:
            KERNEL_I2(safeMod(x, y))
        case KernelOp::eINeg:
            KERNEL_I1(wrapSub(0, x))
        case KernelOp::eIAbs:
            KERNEL_I1(x < 0 ? wrapSub(0, x) : x)
        case KernelOp::eIMin:
            KERNEL_I2(std::min(x, y))
        case KernelOp::eIMax:
            KERNEL_I2(std::max(x, y))
        case KernelOp::eIBitAnd:
            KERNEL_I2(x & y)
        case KernelOp::eIBitOr:
            KERNEL_I2(x | y)
        case KernelOp::eIBitXor:
            KERNEL_I2(x ^ y)
        case KernelOp::eIBitNot:
            KERNEL_I1(~x)
        case KernelOp::eIShl:
            KERNEL_I2(std::int32_t(std::uint32_t(x) << (std::uint32_t(y) & 31u)))
        case KernelOp::eIShr:
            KERNEL_I2(x >> (std::uint32_t(y) & 31u))
        case KernelOp::eILt:
            KERNEL_I2(x < y ? 1 : 0)
        case KernelOp::eILe:
            KERNEL_I2(x <= y ? 1 : 0)
        case KernelOp::eIGt:
            KERNEL_I2(x > y ? 1 : 0)
        case KernelOp::eIGe:
            KERNEL_I2(x >= y ? 1 : 0)
        case KernelOp::eIEq:
            KERNEL_I2(x == y ? 1 : 0)
        case KernelOp::eINe:
            KERNEL_I2(x != y ? 1 : 0)
        case KernelOp::eIBool:
            KERNEL_I1(x != 0 ? 1 : 0)
        case KernelOp::eINot:
            KERNEL_I1(x == 0 ? 1 : 0)
        case KernelOp::eIAnd:
            KERNEL_I2((x != 0 && y != 0) ? 1 : 0)
        case KernelOp::eIOr:
            KERNEL_I2((x != 0 || y != 0) ? 1 : 0)
        case KernelOp::eIAndNot:
            KERNEL_I2((x != 0 && y == 0) ? 1 : 0)
        case KernelOp::eI2F: {
            float* d = R.f(in.d);
            const std::int32_t* pa = R.i(in.a);
            for (int l = 0; l < n; ++l) {
                d[l] = float(pa[l]);
            }
            break;
        }
        case KernelOp::eF2I: {
            std::int32_t* d = R.i(in.d);
            const float* pa = R.f(in.a);
            for (int l = 0; l < n; ++l) {
                d[l] = floatToInt(pa[l]);
            }
            break;
        }
        case KernelOp::eSEq:
        case KernelOp::eSNe: {
            const bool eq = in.op == KernelOp::eSEq;
            std::int32_t* d = R.i(in.d);
            const std::string* pa = R.s(in.a);
            const std::string* pb = R.s(in.b);
            for (int l = 0; l < n; ++l) {
                d[l] = (pa[l] == pb[l]) == eq ? 1 : 0;
            }
            break;
        }
        case KernelOp::eSConcat: {
            std::string* d = R.s(in.d);
            const std::string* pa = R.s(in.a);
            const std::string* pb = R.s(in.b);
            for (int l = 0; l < n; ++l) {
                std::string joined = pa[l];
                joined += pb[l];
                d[l] = std::move(joined);
            }
            break;
        }
        case KernelOp::eANew: {
            KernelArrayValue* d = R.a(in.d);
            for (int l = 0; l < n; ++l) {
                d[l].clear();
                d[l].type = KernelBaseType(in.imm);
            }
            break;
        }
        case KernelOp::eALen: {
            std::int32_t* d = R.i(in.d);
            const KernelArrayValue* pa = R.a(in.a);
            for (int l = 0; l < n; ++l) {
                d[l] = std::int32_t(std::min<std::size_t>(pa[l].length(), INT_MAX));
            }
            break;
        }
        case KernelOp::eAGet:
            arrayGet(m, in, n);
            break;
        case KernelOp::eASet:
            arraySet(m, in, n);
            break;
        case KernelOp::eAPush:
            arrayPush(m, in, n);
            break;
        case KernelOp::eAResize:
            arrayResize(m, in, n);
            break;
        case KernelOp::eJump:
            if (std::uint32_t(in.imm) < pc) {
                pollInLoop(m);
            }
            pc = std::uint32_t(in.imm);
            break;
        case KernelOp::eJumpIfNone: {
            const std::int32_t* c = R.i(in.a);
            bool none = true;
            for (int l = 0; l < n && none; ++l) {
                none = c[l] == 0;
            }
            if (none) {
                if (std::uint32_t(in.imm) < pc) {
                    pollInLoop(m);
                }
                pc = std::uint32_t(in.imm);
            }
            break;
        }
        case KernelOp::eCall: {
            if (in.imm < 0 || std::size_t(in.imm) >= m.program.functions.size()) {
                throw KernelRunError("call to an unknown function");
            }
            const KernelProgramFunction& fn = m.program.functions[std::size_t(in.imm)];
            const std::int32_t* mask = R.i(in.a);
            bool any = false;
            for (int l = 0; l < n && !any; ++l) {
                any = mask[l] != 0;
            }
            if (!any) {
                break;
            }
            if (fn.provider == KernelFunctionProvider::eCore) {
                callCore(m, in, fn, n);
            } else {
                const std::span<const KernelOperand> all(m.program.operands.data() + in.operandBegin, in.operandCount);
                const std::size_t resultCount = std::min<std::size_t>(std::size_t(std::max(in.b, 0)), all.size());
                const KernelCall call { m.program, pc - 1, fn, R, mask, n, all.subspan(0, resultCount), all.subspan(resultCount) };
                m.worker.call(call);
            }
            break;
        }
        }
    }
}

#undef KERNEL_F1
#undef KERNEL_F2
#undef KERNEL_FCMP
#undef KERNEL_I1
#undef KERNEL_I2

void
executeBaseline(Machine& m, std::uint32_t begin, std::uint32_t end, int n)
{
    executeImpl(m, begin, end, n);
}

// x86-64-v3 without FMA, so eFMulAdd and every other float op round exactly as the baseline does.
[[gnu::target("avx,avx2,bmi,bmi2,f16c,popcnt")]] void
executeV3(Machine& m, std::uint32_t begin, std::uint32_t end, int n)
{
    executeImpl(m, begin, end, n);
}

typedef void (*ExecuteFn)(Machine&, std::uint32_t, std::uint32_t, int);

ExecuteFn
selectExecute()
{
    static const ExecuteFn fn = []() -> ExecuteFn {
        __builtin_cpu_init();
        const bool v3 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2")
            && __builtin_cpu_supports("f16c") && __builtin_cpu_supports("popcnt");
        return v3 ? &executeV3 : &executeBaseline;
    }();
    return fn;
}

int
defaultThreadCount()
{
    int threads = AppTLS::currentThreadBudget();
    if (threads <= 0) {
        threads = QThreadPool::globalInstance()->maxThreadCount();
    }
    return std::max(1, threads);
}

} // namespace

KernelRunResult
kernelRun(const KernelProgram& program, KernelRunContext& context, const KernelRunOptions& options)
{
    KernelRunResult result;
    const std::size_t items = context.itemCount();
    if (items == 0) {
        return result;
    }
    const int threads = int(std::min<std::size_t>(std::size_t(options.threads > 0 ? options.threads : defaultThreadCount()), items));
    const ExecuteFn execute = selectExecute();

    std::atomic<std::size_t> nextItem { 0 };
    std::atomic<bool> stop { false };
    std::atomic<bool> aborted { false };
    std::atomic<std::uint64_t> batches { 0 };
    std::mutex errorMutex;
    std::string error;

    const auto fail = [&](const std::string& message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (error.empty()) {
            error = message.empty() ? std::string("kernel run failed") : message;
        }
        stop.store(true);
    };

    const std::function<void(int)> body = [&](int) {
        if (stop.load()) {
            return;
        }
        try {
            std::unique_ptr<KernelRunWorker> worker = context.createWorker();
            Machine machine(program, *worker, options.aborted, stop);
            execute(machine, 0, program.prologueSize, kKernelLaneCount);
            const std::uint32_t end = std::uint32_t(program.instructions.size());
            for (;;) {
                const std::size_t item = nextItem.fetch_add(1);
                if (item >= items || stop.load()) {
                    return;
                }
                const std::size_t count = worker->beginItem(item);
                for (std::size_t first = 0; first < count; first += kKernelLaneCount) {
                    if (stop.load()) {
                        return;
                    }
                    if (options.aborted && options.aborted()) {
                        aborted.store(true);
                        stop.store(true);
                        return;
                    }
                    const int n = int(std::min<std::size_t>(kKernelLaneCount, count - first));
                    std::int32_t* entry = machine.regs.i(KernelProgram::kEntryMaskRegister);
                    std::fill(entry, entry + n, 1);
                    worker->gather(first, n, machine.regs);
                    execute(machine, program.prologueSize, end, n);
                    worker->scatter(first, n, machine.regs);
                    batches.fetch_add(1, std::memory_order_relaxed);
                }
                worker->endItem(item);
            }
        } catch (const LoopAborted&) {
            aborted.store(true);
            stop.store(true);
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail(std::string());
        }
    };

    parallelForOnGlobalPool(threads, threads, body);

    result.batches = batches.load();
    if (!error.empty()) {
        result.status = KernelRunStatus::eFailed;
        result.error = error;
    } else if (aborted.load() || stop.load()) {
        result.status = KernelRunStatus::eAborted;
    }
    return result;
}

NATRON_NAMESPACE_EXIT
