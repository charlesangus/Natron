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

#include "KernelGpuCodegen.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include "Global/Macros.h"

// The gradient tables of Engine/Noise.cpp, which the GPU noise must use too.
namespace {
#include "Engine/NoiseTables.h"
}

NATRON_NAMESPACE_ENTER

namespace {

// Float helpers that reproduce what the interpreter computes with <cmath> where GLSL leaves a case
// undefined or rounds differently: division by zero, fmod's truncation, fmin and fmax with NaN,
// pow of a negative base, atan2 at the origin, round half away from zero and the saturating
// float to int conversion. Vulkan bounds the native exp, pow, sin, cos and tan only loosely
// (sin and cos absolutely, inside [-pi, pi]), so those are reduced to a small range first. Integer division and remainder go through unsigned operands because
// some drivers get the signed remainder of a negative dividend wrong.
const char* const kBasePrelude = R"GLSL(
#define kNaN uintBitsToFloat(0x7fc00000u)

bool kSignBit(float v)
{
    return (floatBitsToUint(v) & 0x80000000u) != 0u;
}

float kFDiv(float x, float y)
{
    if (y == 0.0) {
        if (isnan(x) || x == 0.0) {
            return kNaN;
        }
        return uintBitsToFloat(((floatBitsToUint(x) ^ floatBitsToUint(y)) & 0x80000000u) | 0x7f800000u);
    }
    return x / y;
}

float kFMod(float x, float y)
{
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0) {
        return kNaN;
    }
    if (isinf(y)) {
        return x;
    }
    precise float ax = abs(x);
    precise float ay = abs(y);
    precise float r = fma(-ay, trunc(ax / ay), ax);
    if (r < 0.0) {
        r += ay;
    }
    if (r >= ay) {
        r -= ay;
    }
    return kSignBit(x) ? -r : r;
}

float kFMin(float x, float y)
{
    return isnan(x) ? y : (isnan(y) ? x : min(x, y));
}

float kFMax(float x, float y)
{
    return isnan(x) ? y : (isnan(y) ? x : max(x, y));
}

bool kOddInteger(float y)
{
    return abs(y) < 16777216.0 && trunc(y) == y && (int(abs(y)) & 1) == 1;
}

// 2^k * m without the intermediate overflow of one ldexp when k is near the exponent range's ends.
float kScale(float m, int k)
{
    return ldexp(ldexp(m, k / 2), k - k / 2);
}

float kExp2(float t)
{
    if (isnan(t)) {
        return t;
    }
    if (t >= 128.0) {
        return uintBitsToFloat(0x7f800000u);
    }
    if (t < -150.0) {
        return 0.0;
    }
    precise float k = roundEven(t);
    return kScale(exp2(t - k), int(k));
}

float kExp(float x)
{
    if (isnan(x)) {
        return x;
    }
    if (x > 88.72284) {
        return uintBitsToFloat(0x7f800000u);
    }
    if (x < -103.97208) {
        return 0.0;
    }
    // ln 2 in two parts, the first short enough that k times it is exact for every k in range.
    precise float k = roundEven(x * 1.442695041);
    precise float r = (x - k * 0.693145751953125) - k * 1.428606765330187e-06;
    return kScale(exp2(r * 1.442695041), int(k));
}

float kLog10(float x)
{
    float r = log2(x) * 0.30102999566398120;
    float n = round(r);
    return abs(r - n) <= 4.8e-7 * max(abs(n), 1.0) ? n : r;
}

// x - k * pi/2 for the nearest integer k, returning k mod 4. pi/2 is split Cody-Waite style into
// five 7-bit parts and a rounded sixth: for |k| below 2^17 every k * part is exact and each
// subtraction cancels exactly, so the remainder keeps full precision up to |x| of about 2e5.
int kReduceHalfPi(float x, out float r)
{
    precise float k = roundEven(x * 0.63661977236758134);
    precise float t = x - k * 1.5625;
    t = t - k * 0.0081787109375;
    t = t - k * 0.00011730194091796875;
    t = t - k * 3.129243850708008e-07;
    t = t - k * 9.89530235528946e-10;
    t = t - k * 2.5633440682570896e-12;
    r = t;
    return int(k) & 3;
}

float kSinPoly(float r)
{
    float z = r * r;
    return ((-1.9515295891e-4 * z + 8.3321608736e-3) * z - 1.6666654611e-1) * z * r + r;
}

float kCosPoly(float r)
{
    float z = r * r;
    return ((2.443315711809948e-5 * z - 1.388731625493765e-3) * z + 4.166664568298827e-2) * z * z - 0.5 * z + 1.0;
}

float kSin(float x)
{
    float r;
    int q = kReduceHalfPi(x, r);
    float v = (q & 1) == 0 ? kSinPoly(r) : kCosPoly(r);
    return (q & 2) == 0 ? v : -v;
}

float kCos(float x)
{
    float r;
    int q = kReduceHalfPi(x, r);
    float v = (q & 1) == 0 ? kCosPoly(r) : kSinPoly(r);
    return ((q + 1) & 2) == 0 ? v : -v;
}

float kTan(float x)
{
    float r;
    int q = kReduceHalfPi(x, r);
    float s = kSinPoly(r);
    float c = kCosPoly(r);
    return (q & 1) == 0 ? s / c : -c / s;
}

float kPow(float x, float y)
{
    if (y == 0.0 || x == 1.0) {
        return 1.0;
    }
    if (isnan(x) || isnan(y)) {
        return kNaN;
    }
    if (x == 0.0) {
        bool odd = kOddInteger(y);
        if (y > 0.0) {
            return odd ? x : 0.0;
        }
        float inf = uintBitsToFloat(0x7f800000u);
        return (odd && kSignBit(x)) ? -inf : inf;
    }
    if (x < 0.0) {
        if (isinf(y)) {
            return x == -1.0 ? 1.0 : kExp2(y * log2(-x));
        }
        if (trunc(y) != y) {
            return kNaN;
        }
        float r = kExp2(y * log2(-x));
        return kOddInteger(y) ? -r : r;
    }
    return kExp2(y * log2(x));
}

float kAtan2(float y, float x)
{
    if (x == 0.0 && y == 0.0) {
        float r = kSignBit(x) ? 3.14159265358979 : 0.0;
        return kSignBit(y) ? -r : r;
    }
    return atan(y, x);
}

float kRound(float x)
{
    float t = trunc(x);
    if (abs(x - t) >= 0.5) {
        t += x < 0.0 ? -1.0 : 1.0;
    }
    return t;
}

float kFSign(float x)
{
    return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0);
}

int kF2I(float x)
{
    if (isnan(x)) {
        return 0;
    }
    if (x >= 2147483648.0) {
        return 2147483647;
    }
    if (x <= -2147483648.0) {
        return int(0x80000000u);
    }
    return int(x);
}

uint kMagnitude(int v)
{
    return v < 0 ? uint(-v) : uint(v);
}

int kIDiv(int a, int b)
{
    if (b == 0) {
        return 0;
    }
    if (b == -1) {
        return -a;
    }
    uint q = kMagnitude(a) / kMagnitude(b);
    return (a < 0) != (b < 0) ? -int(q) : int(q);
}

int kIMod(int a, int b)
{
    if (b == 0 || b == -1) {
        return 0;
    }
    uint r = kMagnitude(a) % kMagnitude(b);
    return a < 0 ? -int(r) : int(r);
}
)GLSL";

const char* const kRandPrelude = R"GLSL(
uint kHash32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint kFloatBits(float v)
{
    return v == 0.0 ? 0u : floatBitsToUint(v);
}

float kUnitFloat(uint h)
{
    return float(h >> 8) * (1.0 / 16777216.0);
}
)GLSL";

// Perlin noise as Engine/Noise.cpp computes it, in float: the lattice index and the weights are
// exact, so only the gradient products and the interpolation round differently from the double
// original.
const char* const kNoiseFunctions = R"GLSL(
uint kNoiseTemper(uint s)
{
    s ^= s >> 11;
    s ^= (s << 7) & 0x9d2c5680u;
    s ^= (s << 15) & 0xefc60000u;
    s ^= s >> 18;
    return (((s & 0xff0000u) >> 4) + (s & 0xffu)) & 0xffu;
}

float kSCurve(float t)
{
    return t * t * t * (t * (6.0 * t - 15.0) + 10.0);
}

float kNoise1(float x)
{
    float f = floor(x);
    int i = int(f);
    float w0 = x - f;
    float w1 = w0 - 1.0;
    float v0 = kNoiseG1[kNoiseTemper(uint(i) + 1013904223u)] * w0;
    float v1 = kNoiseG1[kNoiseTemper(uint(i + 1) + 1013904223u)] * w1;
    float a = kSCurve(w0);
    return (1.0 - a) * v0 + a * v1;
}

float kNoise3(vec3 x, int shift)
{
    vec3 f = floor(x);
    ivec3 i = ivec3(f) + ivec3(shift);
    vec3 w0 = x - f;
    vec3 w1 = w0 - vec3(1.0);
    float v[8];
    for (int c = 0; c < 8; ++c) {
        ivec3 o = ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        ivec3 l = i + o;
        uint s = 0u;
        s = s * 1664525u + uint(l.x) + 1013904223u;
        s = s * 1664525u + uint(l.y) + 1013904223u;
        s = s * 1664525u + uint(l.z) + 1013904223u;
        vec3 g = kNoiseG3[kNoiseTemper(s)];
        vec3 w = mix(w0, w1, bvec3(o));
        v[c] = g.x * w.x + g.y * w.y + g.z * w.z;
    }
    vec3 a = vec3(kSCurve(w0.x), kSCurve(w0.y), kSCurve(w0.z));
    v[0] = (1.0 - a.x) * v[0] + a.x * v[1];
    v[2] = (1.0 - a.x) * v[2] + a.x * v[3];
    v[4] = (1.0 - a.x) * v[4] + a.x * v[5];
    v[6] = (1.0 - a.x) * v[6] + a.x * v[7];
    v[0] = (1.0 - a.y) * v[0] + a.y * v[2];
    v[4] = (1.0 - a.y) * v[4] + a.y * v[6];
    return (1.0 - a.z) * v[0] + a.z * v[4];
}
)GLSL";

// Gauss-Jordan with partial pivoting as the interpreter does it, in float; a singular matrix gives
// zeros. N, NN and N2 are replaced by the size, its square and twice it.
const char* const kInvertTemplate = R"GLSL(
void kInvertN(in float m[NN], out float r[NN])
{
    float a[N][N2];
    for (int row = 0; row < N; ++row) {
        for (int c = 0; c < N; ++c) {
            a[row][c] = m[row * N + c];
            a[row][c + N] = row == c ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < N; ++col) {
        int pivot = col;
        for (int row = col + 1; row < N; ++row) {
            if (abs(a[row][col]) > abs(a[pivot][col])) {
                pivot = row;
            }
        }
        if (a[pivot][col] == 0.0) {
            for (int k = 0; k < NN; ++k) {
                r[k] = 0.0;
            }
            return;
        }
        if (pivot != col) {
            for (int c = 0; c < N2; ++c) {
                float t = a[pivot][c];
                a[pivot][c] = a[col][c];
                a[col][c] = t;
            }
        }
        float inv = 1.0 / a[col][col];
        for (int c = 0; c < N2; ++c) {
            a[col][c] *= inv;
        }
        for (int row = 0; row < N; ++row) {
            if (row == col || a[row][col] == 0.0) {
                continue;
            }
            float f = a[row][col];
            for (int c = 0; c < N2; ++c) {
                a[row][c] -= f * a[col][c];
            }
        }
    }
    for (int row = 0; row < N; ++row) {
        for (int c = 0; c < N; ++c) {
            r[row * N + c] = a[row][c + N];
        }
    }
}
)GLSL";

const char* const kDet3Prelude = R"GLSL(
float kDet3(in float m[9])
{
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}
)GLSL";

const char* const kDet4Prelude = R"GLSL(
float kDet4(in float m[16])
{
    float d = 0.0;
    for (int c = 0; c < 4; ++c) {
        float sub[9];
        int k = 0;
        for (int r = 1; r < 4; ++r) {
            for (int cc = 0; cc < 4; ++cc) {
                if (cc != c) {
                    sub[k++] = m[r * 4 + cc];
                }
            }
        }
        float term = m[c] * kDet3(sub);
        d += (c & 1) != 0 ? -term : term;
    }
    return d;
}
)GLSL";

const char* const kQuaternionPrelude = R"GLSL(
vec4 kQuaternion(float angle, vec3 axis)
{
    float len = sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (!(len > 0.0)) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    float s = sin(angle * 0.5) / len;
    return vec4(axis * s, cos(angle * 0.5));
}

vec4 kQMultiply(vec4 a, vec4 b)
{
    return vec4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

vec3 kQRotate(vec4 q, vec3 v)
{
    vec3 t = 2.0 * vec3(q.y * v.z - q.z * v.y, q.z * v.x - q.x * v.z, q.x * v.y - q.y * v.x);
    return vec3(v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
                v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
                v.z + q.w * t.z + (q.x * t.y - q.y * t.x));
}
)GLSL";

std::string
replaceAll(std::string text, std::string_view from, std::string_view to)
{
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

std::string
invertFunction(int n)
{
    std::string text = kInvertTemplate;
    text = replaceAll(std::move(text), "kInvertN", "kInvert" + std::to_string(n));
    text = replaceAll(std::move(text), "NN", std::to_string(n * n));
    text = replaceAll(std::move(text), "N2", std::to_string(2 * n));
    // Every remaining N is the size: the template uses no other capital N.
    return replaceAll(std::move(text), "N", std::to_string(n));
}

// Nine significant digits give back the same float; a non-finite value has no decimal literal.
std::string
floatLiteral(float v)
{
    char buffer[40];
    if (std::isfinite(v)) {
        std::snprintf(buffer, sizeof buffer, "%.8e", double(v));
        return buffer;
    }
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    std::snprintf(buffer, sizeof buffer, "uintBitsToFloat(0x%08xu)", bits);
    return buffer;
}

std::string
intLiteral(std::int32_t v)
{
    if (v == INT_MIN) {
        return "int(0x80000000u)";
    }
    return std::to_string(v);
}

std::string
noiseTables()
{
    std::string out = "const float kNoiseG1[256] = float[256](";
    for (int k = 0; k < 256; ++k) {
        out += (k ? ", " : "") + floatLiteral(float(NOISE_TABLES<1>::g[k][0]));
    }
    out += ");\nconst vec3 kNoiseG3[256] = vec3[256](";
    for (int k = 0; k < 256; ++k) {
        out += k ? ", vec3(" : "vec3(";
        for (int c = 0; c < 3; ++c) {
            out += (c ? ", " : "") + floatLiteral(float(NOISE_TABLES<3>::g[k][c]));
        }
        out += ")";
    }
    out += ");\n";
    return out;
}

std::string
fr(std::int32_t reg)
{
    return "f" + std::to_string(reg);
}

std::string
ir(std::int32_t reg)
{
    return "i" + std::to_string(reg);
}

std::string
operandName(const KernelOperand& o)
{
    return o.bank == KernelBank::eInt ? ir(o.reg) : fr(o.reg);
}

std::string
bindingName(const KernelProgramBinding& b)
{
    if (b.input != 0) {
        return "@opinput" + std::to_string(b.input) + "_" + b.name;
    }
    return "@" + b.name;
}

class Generator {
public:
    Generator(const KernelProgram& program, const KernelGpuTarget& target, KernelGpuPlan& plan)
        : _program(program)
        , _target(target)
        , _plan(plan)
    {
    }

    void run();

private:
    bool fail(const std::string& reason, const std::string& name = std::string())
    {
        if (_plan.cpuOnlyReason.empty()) {
            _plan.cpuOnlyReason = reason;
            _plan.cpuOnlyName = name;
        }
        return false;
    }

    bool validF(std::int32_t reg) const
    {
        return reg >= 0 && reg < _program.floatRegisters;
    }

    bool validI(std::int32_t reg) const
    {
        return reg >= 0 && reg < _program.intRegisters;
    }

    bool validOperand(const KernelOperand& o) const
    {
        return (o.bank == KernelBank::eFloat && validF(o.reg)) || (o.bank == KernelBank::eInt && validI(o.reg));
    }

    bool malformed()
    {
        return fail("the compiled program is malformed");
    }

    bool addBindings();
    bool emitInstruction(std::uint32_t index, std::string& out);
    bool emitCall(std::uint32_t index, const KernelInstruction& in, std::string& out);
    bool emitCoreCall(const KernelProgramFunction& fn, std::span<const KernelOperand> res,
                      std::span<const KernelOperand> args, std::string& out);
    bool emitBody(std::string& out);
    std::string assemble(const std::string& prologue, const std::string& body) const;

    const KernelProgram& _program;
    const KernelGpuTarget& _target;
    KernelGpuPlan& _plan;
    bool _noise = false;
    bool _rand = false;
    bool _invert3 = false;
    bool _invert4 = false;
    bool _det3 = false;
    bool _det4 = false;
    bool _quaternion = false;
    bool _jumpsBack = false;
};

bool
Generator::addBindings()
{
    std::map<std::pair<KernelBank, std::int32_t>, std::size_t> columnOf;
    for (std::size_t bi = 0; bi < _program.bindings.size(); ++bi) {
        const KernelProgramBinding& b = _program.bindings[bi];
        std::string why;
        const KernelGpuForm form = _target.bindingForm(b, &why);
        const KernelBank bank = kernelBankOf(b.type);
        if (form == KernelGpuForm::eUnsupported) {
            return fail(bindingName(b) + ": " + (why.empty() ? std::string("has no GPU form") : why), bindingName(b));
        }
        if (bank != KernelBank::eFloat && bank != KernelBank::eInt) {
            return fail(bindingName(b) + ": " + kernelTypeName(b.type) + " values have no GPU form", bindingName(b));
        }
        for (std::size_t k = 0; k < b.registers.size(); ++k) {
            const KernelOperand o { bank, b.registers[k] };
            if (!validOperand(o)) {
                return malformed();
            }
            if (form == KernelGpuForm::eUniform) {
                if (b.written) {
                    return fail(bindingName(b) + ": a uniform value cannot be written on the GPU", bindingName(b));
                }
                KernelGpuUniform u;
                u.operand = o;
                u.binding = std::int32_t(bi);
                u.result = std::uint32_t(k);
                _plan.uniforms.push_back(u);
                continue;
            }
            const auto key = std::make_pair(bank, o.reg);
            auto found = columnOf.find(key);
            if (found == columnOf.end()) {
                found = columnOf.emplace(key, _plan.columns.size()).first;
                KernelGpuColumn c;
                c.bank = bank;
                c.reg = o.reg;
                _plan.columns.push_back(c);
            }
            KernelGpuColumn& c = _plan.columns[found->second];
            c.load = c.load || b.needsGather;
            c.store = c.store || b.written;
        }
    }
    return true;
}

bool
Generator::emitCoreCall(const KernelProgramFunction& fn, std::span<const KernelOperand> res,
                        std::span<const KernelOperand> args, std::string& out)
{
    const std::string& name = fn.signature.name;
    const auto need = [&](std::size_t r, std::size_t a) {
        if (res.size() < r || args.size() < a) {
            return false;
        }
        for (std::size_t k = 0; k < r; ++k) {
            if (res[k].bank != KernelBank::eFloat || !validF(res[k].reg)) {
                return false;
            }
        }
        for (std::size_t k = 0; k < a; ++k) {
            if (args[k].bank != KernelBank::eFloat || !validF(args[k].reg)) {
                return false;
            }
        }
        return true;
    };
    const auto argList = [&](std::size_t begin, std::size_t count) {
        std::string s;
        for (std::size_t k = 0; k < count; ++k) {
            s += (k ? ", " : "") + fr(args[begin + k].reg);
        }
        return s;
    };
    const KernelCoreFunction id = KernelCoreFunction(fn.signature.id);
    switch (id) {
    case KernelCoreFunction::eNoiseFloat:
        if (!need(1, 1)) {
            return malformed();
        }
        _noise = true;
        out += fr(res[0].reg) + " = 0.5 + 0.5 * kNoise1(" + fr(args[0].reg) + ");\n";
        return true;
    case KernelCoreFunction::eNoiseVector:
        if (!need(1, 3)) {
            return malformed();
        }
        _noise = true;
        out += fr(res[0].reg) + " = 0.5 + 0.5 * kNoise3(vec3(" + argList(0, 3) + "), 0);\n";
        return true;
    case KernelCoreFunction::eNoiseVectorToVector:
        if (!need(3, 3)) {
            return malformed();
        }
        _noise = true;
        out += "{ vec3 p = vec3(" + argList(0, 3) + ");\n";
        for (int c = 0; c < 3; ++c) {
            out += fr(res[std::size_t(c)].reg) + " = 0.5 + 0.5 * kNoise3(p, " + std::to_string(1000 * c) + ");\n";
        }
        out += "}\n";
        return true;
    case KernelCoreFunction::eRandFloat:
    case KernelCoreFunction::eRandVector:
    case KernelCoreFunction::eRandFloatToVector:
    case KernelCoreFunction::eRandVectorToVector: {
        const bool vectorSeed = id == KernelCoreFunction::eRandVector || id == KernelCoreFunction::eRandVectorToVector;
        const bool vectorResult = id == KernelCoreFunction::eRandFloatToVector || id == KernelCoreFunction::eRandVectorToVector;
        const std::size_t seeds = vectorSeed ? 3 : 1;
        const std::size_t outs = vectorResult ? 3 : 1;
        if (!need(outs, seeds)) {
            return malformed();
        }
        _rand = true;
        out += "{ uint h = 0x2545f491u;\n";
        for (std::size_t s = 0; s < seeds; ++s) {
            out += "h = kHash32(h ^ kFloatBits(" + fr(args[s].reg) + "));\n";
        }
        for (std::size_t c = 0; c < outs; ++c) {
            char add[24];
            std::snprintf(add, sizeof add, "0x%08xu", unsigned(std::uint32_t(c) * 0x9e3779b9u));
            out += fr(res[c].reg) + " = kUnitFloat(kHash32(h + " + add + "));\n";
        }
        out += "}\n";
        return true;
    }
    case KernelCoreFunction::eInvert3:
    case KernelCoreFunction::eInvert4:
    case KernelCoreFunction::eDeterminant3:
    case KernelCoreFunction::eDeterminant4: {
        const int dim = (id == KernelCoreFunction::eInvert3 || id == KernelCoreFunction::eDeterminant3) ? 3 : 4;
        const bool invert = id == KernelCoreFunction::eInvert3 || id == KernelCoreFunction::eInvert4;
        const std::size_t size = std::size_t(dim * dim);
        if (!need(invert ? size : 1, size)) {
            return malformed();
        }
        const std::string n = std::to_string(size);
        out += "{ float m[" + n + "] = float[" + n + "](" + argList(0, size) + ");\n";
        if (invert) {
            (dim == 3 ? _invert3 : _invert4) = true;
            out += "float r[" + n + "];\nkInvert" + std::to_string(dim) + "(m, r);\n";
            for (std::size_t c = 0; c < size; ++c) {
                out += fr(res[c].reg) + " = r[" + std::to_string(c) + "];\n";
            }
        } else {
            _det3 = true;
            _det4 = _det4 || dim == 4;
            out += fr(res[0].reg) + " = kDet" + std::to_string(dim) + "(m);\n";
        }
        out += "}\n";
        return true;
    }
    case KernelCoreFunction::eQuaternionAxisAngle:
        if (!need(4, 4)) {
            return malformed();
        }
        _quaternion = true;
        out += "{ vec4 q = kQuaternion(" + fr(args[0].reg) + ", vec3(" + argList(1, 3) + "));\n";
        for (int c = 0; c < 4; ++c) {
            out += fr(res[std::size_t(c)].reg) + " = q[" + std::to_string(c) + "];\n";
        }
        out += "}\n";
        return true;
    case KernelCoreFunction::eQuaternionMultiply:
        if (!need(4, 8)) {
            return malformed();
        }
        _quaternion = true;
        out += "{ vec4 q = kQMultiply(vec4(" + argList(0, 4) + "), vec4(" + argList(4, 4) + "));\n";
        for (int c = 0; c < 4; ++c) {
            out += fr(res[std::size_t(c)].reg) + " = q[" + std::to_string(c) + "];\n";
        }
        out += "}\n";
        return true;
    case KernelCoreFunction::eQuaternionRotate:
        if (!need(3, 7)) {
            return malformed();
        }
        _quaternion = true;
        out += "{ vec3 v = kQRotate(vec4(" + argList(0, 4) + "), vec3(" + argList(4, 3) + "));\n";
        for (int c = 0; c < 3; ++c) {
            out += fr(res[std::size_t(c)].reg) + " = v[" + std::to_string(c) + "];\n";
        }
        out += "}\n";
        return true;
    case KernelCoreFunction::eSprintf:
    case KernelCoreFunction::eStrlen:
    case KernelCoreFunction::eItoa:
    case KernelCoreFunction::eAtoi:
    case KernelCoreFunction::eAtof:
    case KernelCoreFunction::eStartsWith:
    case KernelCoreFunction::eEndsWith:
        break;
    }
    return fail(name + "() has no GPU form", name);
}

bool
Generator::emitCall(std::uint32_t index, const KernelInstruction& in, std::string& out)
{
    if (in.imm < 0 || std::size_t(in.imm) >= _program.functions.size() || !validI(in.a)) {
        return malformed();
    }
    const std::size_t end = std::size_t(in.operandBegin) + in.operandCount;
    const std::size_t resultCount = std::size_t(std::max(in.b, 0));
    if (end > _program.operands.size() || resultCount > in.operandCount) {
        return malformed();
    }
    const std::span<const KernelOperand> all(_program.operands.data() + in.operandBegin, in.operandCount);
    const std::span<const KernelOperand> res = all.subspan(0, resultCount);
    const std::span<const KernelOperand> args = all.subspan(resultCount);
    const KernelProgramFunction& fn = _program.functions[std::size_t(in.imm)];
    std::string call;
    if (fn.provider == KernelFunctionProvider::eCore) {
        if (!emitCoreCall(fn, res, args, call)) {
            return false;
        }
    } else {
        if (_target.functionForm(fn) != KernelGpuForm::eUniform) {
            return fail(fn.signature.name + "() has no GPU form", fn.signature.name);
        }
        for (std::size_t k = 0; k < res.size(); ++k) {
            if (!validOperand(res[k])) {
                return fail(fn.signature.name + "() returns a value with no GPU form", fn.signature.name);
            }
            KernelGpuUniform u;
            u.operand = res[k];
            u.instruction = std::int32_t(index);
            u.result = std::uint32_t(k);
            const std::string word = "uni.w[" + std::to_string(_plan.uniforms.size()) + "]";
            call += operandName(res[k]) + " = " + (res[k].bank == KernelBank::eInt ? "int(" + word + ")" : "uintBitsToFloat(" + word + ")") + ";\n";
            _plan.uniforms.push_back(u);
        }
    }
    out += "if (" + ir(in.a) + " != 0) {\n" + call + "}\n";
    return true;
}

bool
Generator::emitInstruction(std::uint32_t index, std::string& out)
{
    const KernelInstruction& in = _program.instructions[index];
    const auto f1 = [&](const std::string& expr) {
        if (!validF(in.d) || !validF(in.a)) {
            return malformed();
        }
        out += fr(in.d) + " = " + replaceAll(expr, "$a", fr(in.a)) + ";\n";
        return true;
    };
    const auto f2 = [&](const std::string& expr) {
        if (!validF(in.d) || !validF(in.a) || !validF(in.b)) {
            return malformed();
        }
        out += fr(in.d) + " = " + replaceAll(replaceAll(expr, "$a", fr(in.a)), "$b", fr(in.b)) + ";\n";
        return true;
    };
    const auto fcmp = [&](const char* op) {
        if (!validI(in.d) || !validF(in.a) || !validF(in.b)) {
            return malformed();
        }
        out += ir(in.d) + " = (" + fr(in.a) + " " + op + " " + fr(in.b) + ") ? 1 : 0;\n";
        return true;
    };
    const auto i1 = [&](const std::string& expr) {
        if (!validI(in.d) || !validI(in.a)) {
            return malformed();
        }
        out += ir(in.d) + " = " + replaceAll(expr, "$a", ir(in.a)) + ";\n";
        return true;
    };
    const auto i2 = [&](const std::string& expr) {
        if (!validI(in.d) || !validI(in.a) || !validI(in.b)) {
            return malformed();
        }
        out += ir(in.d) + " = " + replaceAll(replaceAll(expr, "$a", ir(in.a)), "$b", ir(in.b)) + ";\n";
        return true;
    };
    switch (in.op) {
    case KernelOp::eFConst:
        if (!validF(in.d)) {
            return malformed();
        }
        out += fr(in.d) + " = " + floatLiteral(in.fimm) + ";\n";
        return true;
    case KernelOp::eIConst:
        if (!validI(in.d)) {
            return malformed();
        }
        out += ir(in.d) + " = " + intLiteral(in.imm) + ";\n";
        return true;
    case KernelOp::eSConst:
        // A string literal only feeds string operations, which have no GPU form, or the name
        // argument of a call the host evaluates.
        return true;
    case KernelOp::eFMov:
        return f1("$a");
    case KernelOp::eIMov:
        return i1("$a");
    case KernelOp::eFSel:
        if (!validF(in.d) || !validI(in.a) || !validF(in.b) || !validF(in.c)) {
            return malformed();
        }
        out += fr(in.d) + " = (" + ir(in.a) + " != 0) ? " + fr(in.b) + " : " + fr(in.c) + ";\n";
        return true;
    case KernelOp::eISel:
        if (!validI(in.d) || !validI(in.a) || !validI(in.b) || !validI(in.c)) {
            return malformed();
        }
        out += ir(in.d) + " = (" + ir(in.a) + " != 0) ? " + ir(in.b) + " : " + ir(in.c) + ";\n";
        return true;
    case KernelOp::eFAdd:
        return f2("$a + $b");
    case KernelOp::eFSub:
        return f2("$a - $b");
    case KernelOp::eFMul:
        return f2("$a * $b");
    case KernelOp::eFDiv:
        return f2("kFDiv($a, $b)");
    case KernelOp::eFMod:
        return f2("kFMod($a, $b)");
    case KernelOp::eFMulAdd:
        if (!validF(in.d) || !validF(in.a) || !validF(in.b) || !validF(in.c)) {
            return malformed();
        }
        // The registers are precise, so the product is rounded before the add, as on the CPU.
        out += "{ precise float t = " + fr(in.a) + " * " + fr(in.b) + "; " + fr(in.d) + " = t + " + fr(in.c) + "; }\n";
        return true;
    case KernelOp::eFNeg:
        return f1("-$a");
    case KernelOp::eFMin:
        return f2("kFMin($a, $b)");
    case KernelOp::eFMax:
        return f2("kFMax($a, $b)");
    case KernelOp::eFPow:
        return f2("kPow($a, $b)");
    case KernelOp::eFAtan2:
        return f2("kAtan2($a, $b)");
    case KernelOp::eFAbs:
        return f1("abs($a)");
    case KernelOp::eFFloor:
        return f1("floor($a)");
    case KernelOp::eFCeil:
        return f1("ceil($a)");
    case KernelOp::eFRound:
        return f1("kRound($a)");
    case KernelOp::eFTrunc:
        return f1("trunc($a)");
    case KernelOp::eFFrac:
        return f1("$a - floor($a)");
    case KernelOp::eFSign:
        return f1("kFSign($a)");
    case KernelOp::eFSqrt:
        return f1("sqrt($a)");
    case KernelOp::eFExp:
        return f1("kExp($a)");
    case KernelOp::eFLog:
        return f1("log2($a) * 0.69314718055994531");
    case KernelOp::eFLog10:
        return f1("kLog10($a)");
    case KernelOp::eFSin:
        return f1("kSin($a)");
    case KernelOp::eFCos:
        return f1("kCos($a)");
    case KernelOp::eFTan:
        return f1("kTan($a)");
    case KernelOp::eFAsin:
        return f1("asin($a)");
    case KernelOp::eFAcos:
        return f1("acos($a)");
    case KernelOp::eFAtan:
        return f1("atan($a)");
    case KernelOp::eFLt:
        return fcmp("<");
    case KernelOp::eFLe:
        return fcmp("<=");
    case KernelOp::eFGt:
        return fcmp(">");
    case KernelOp::eFGe:
        return fcmp(">=");
    case KernelOp::eFEq:
        return fcmp("==");
    case KernelOp::eFNe:
        return fcmp("!=");
    case KernelOp::eIAdd:
        return i2("$a + $b");
    case KernelOp::eISub:
        return i2("$a - $b");
    case KernelOp::eIMul:
        return i2("$a * $b");
    case KernelOp::eIDiv:
        return i2("kIDiv($a, $b)");
    case KernelOp::eIMod:
        return i2("kIMod($a, $b)");
    case KernelOp::eINeg:
        return i1("-$a");
    case KernelOp::eIAbs:
        return i1("($a < 0) ? -$a : $a");
    case KernelOp::eIMin:
        return i2("min($a, $b)");
    case KernelOp::eIMax:
        return i2("max($a, $b)");
    case KernelOp::eIBitAnd:
        return i2("$a & $b");
    case KernelOp::eIBitOr:
        return i2("$a | $b");
    case KernelOp::eIBitXor:
        return i2("$a ^ $b");
    case KernelOp::eIBitNot:
        return i1("~$a");
    case KernelOp::eIShl:
        return i2("int(uint($a) << (uint($b) & 31u))");
    case KernelOp::eIShr:
        return i2("$a >> (uint($b) & 31u)");
    case KernelOp::eILt:
        return i2("($a < $b) ? 1 : 0");
    case KernelOp::eILe:
        return i2("($a <= $b) ? 1 : 0");
    case KernelOp::eIGt:
        return i2("($a > $b) ? 1 : 0");
    case KernelOp::eIGe:
        return i2("($a >= $b) ? 1 : 0");
    case KernelOp::eIEq:
        return i2("($a == $b) ? 1 : 0");
    case KernelOp::eINe:
        return i2("($a != $b) ? 1 : 0");
    case KernelOp::eIBool:
        return i1("($a != 0) ? 1 : 0");
    case KernelOp::eINot:
        return i1("($a == 0) ? 1 : 0");
    case KernelOp::eIAnd:
        return i2("($a != 0 && $b != 0) ? 1 : 0");
    case KernelOp::eIOr:
        return i2("($a != 0 || $b != 0) ? 1 : 0");
    case KernelOp::eIAndNot:
        return i2("($a != 0 && $b == 0) ? 1 : 0");
    case KernelOp::eI2F:
        if (!validF(in.d) || !validI(in.a)) {
            return malformed();
        }
        out += fr(in.d) + " = float(" + ir(in.a) + ");\n";
        return true;
    case KernelOp::eF2I:
        if (!validI(in.d) || !validF(in.a)) {
            return malformed();
        }
        out += ir(in.d) + " = kF2I(" + fr(in.a) + ");\n";
        return true;
    case KernelOp::eSMov:
    case KernelOp::eSSel:
    case KernelOp::eSEq:
    case KernelOp::eSNe:
    case KernelOp::eSConcat:
        return fail("string values have no GPU form", "string");
    case KernelOp::eACopy:
    case KernelOp::eASel:
    case KernelOp::eANew:
    case KernelOp::eALen:
    case KernelOp::eAGet:
    case KernelOp::eASet:
    case KernelOp::eAPush:
    case KernelOp::eAResize:
        return fail("array values have no GPU form", "array");
    case KernelOp::eCall:
        return emitCall(index, in, out);
    case KernelOp::eJump:
    case KernelOp::eJumpIfNone:
        // Only emitBody places jumps.
        return malformed();
    }
    return malformed();
}

bool
Generator::emitBody(std::string& out)
{
    const std::uint32_t begin = _program.prologueSize;
    const std::uint32_t end = std::uint32_t(_program.instructions.size());
    std::set<std::uint32_t> leaders;
    leaders.insert(begin);
    for (std::uint32_t k = begin; k < end; ++k) {
        const KernelInstruction& in = _program.instructions[k];
        if (in.op != KernelOp::eJump && in.op != KernelOp::eJumpIfNone) {
            continue;
        }
        if (in.imm < std::int32_t(begin) || std::uint32_t(in.imm) > end) {
            return malformed();
        }
        if (in.op == KernelOp::eJumpIfNone && !validI(in.a)) {
            return malformed();
        }
        leaders.insert(std::uint32_t(in.imm));
        leaders.insert(k + 1);
        _jumpsBack = _jumpsBack || std::uint32_t(in.imm) <= k;
    }
    leaders.erase(end);
    const auto jumpTo = [&](std::uint32_t from, std::uint32_t target) {
        std::string s;
        if (target <= from) {
            s += "if (++jumps > push.jumpBudget) { status.runaway = 1u; return; }\n";
        }
        s += "pc = " + std::to_string(target) + ";\n";
        return s;
    };
    out += "int pc = " + std::to_string(begin) + ";\nuint jumps = 0u;\nwhile (pc >= 0) {\nswitch (pc) {\n";
    for (auto it = leaders.begin(); it != leaders.end(); ++it) {
        const std::uint32_t first = *it;
        auto nextIt = std::next(it);
        const std::uint32_t last = nextIt == leaders.end() ? end : *nextIt;
        out += "case " + std::to_string(first) + ": {\n";
        bool transferred = false;
        for (std::uint32_t k = first; k < last; ++k) {
            const KernelInstruction& in = _program.instructions[k];
            if (in.op == KernelOp::eJump) {
                out += jumpTo(k, std::uint32_t(in.imm));
                transferred = true;
            } else if (in.op == KernelOp::eJumpIfNone) {
                out += "if (" + ir(in.a) + " == 0) {\n" + jumpTo(k, std::uint32_t(in.imm)) + "} else {\npc = " + std::to_string(k + 1) + ";\n}\n";
                transferred = true;
            } else if (!emitInstruction(k, out)) {
                return false;
            }
        }
        if (!transferred) {
            out += "pc = " + std::to_string(last) + ";\n";
        }
        out += "break;\n}\n";
    }
    out += "default:\npc = -1;\nbreak;\n}\n}\n";
    return true;
}

std::string
Generator::assemble(const std::string& prologue, const std::string& body) const
{
    std::string s = "#version 460\n"
                    "layout(local_size_x = "
        + std::to_string(kKernelGpuGroupSize)
        + ") in;\n"
          "layout(std430, set = 0, binding = 0) buffer Data { uint w[]; } data;\n"
          "layout(std430, set = 0, binding = 1) readonly buffer Uniforms { uint w[]; } uni;\n"
          "layout(std430, set = 0, binding = 2) buffer Status { uint runaway; } status;\n"
          "layout(push_constant) uniform Push { uint first; uint count; uint stride; uint jumpBudget; } push;\n";
    s += kBasePrelude;
    if (_rand) {
        s += kRandPrelude;
    }
    if (_noise) {
        s += noiseTables();
        s += kNoiseFunctions;
    }
    if (_invert3) {
        s += invertFunction(3);
    }
    if (_invert4) {
        s += invertFunction(4);
    }
    if (_det3) {
        s += kDet3Prelude;
    }
    if (_det4) {
        s += kDet4Prelude;
    }
    if (_quaternion) {
        s += kQuaternionPrelude;
    }
    s += "\nvoid main()\n{\nuint e = push.first + gl_GlobalInvocationID.x;\nif (e >= push.count) {\nreturn;\n}\n";
    for (std::int32_t r = 0; r < _program.floatRegisters; ++r) {
        s += "precise float " + fr(r) + " = 0.0;\n";
    }
    for (std::int32_t r = 0; r < _program.intRegisters; ++r) {
        s += "int " + ir(r) + " = " + (r == KernelProgram::kEntryMaskRegister ? "1" : "0") + ";\n";
    }
    s += prologue;
    for (std::size_t c = 0; c < _plan.columns.size(); ++c) {
        const KernelGpuColumn& col = _plan.columns[c];
        if (!col.load) {
            continue;
        }
        const std::string word = "data.w[" + std::to_string(c) + "u * push.stride + e]";
        s += col.bank == KernelBank::eInt ? ir(col.reg) + " = int(" + word + ");\n" : fr(col.reg) + " = uintBitsToFloat(" + word + ");\n";
    }
    for (std::size_t k = 0; k < _plan.uniforms.size(); ++k) {
        const KernelGpuUniform& u = _plan.uniforms[k];
        if (u.binding < 0) {
            continue;
        }
        const std::string word = "uni.w[" + std::to_string(k) + "]";
        s += u.operand.bank == KernelBank::eInt ? ir(u.operand.reg) + " = int(" + word + ");\n" : fr(u.operand.reg) + " = uintBitsToFloat(" + word + ");\n";
    }
    s += body;
    for (std::size_t c = 0; c < _plan.columns.size(); ++c) {
        const KernelGpuColumn& col = _plan.columns[c];
        if (!col.store) {
            continue;
        }
        const std::string word = "data.w[" + std::to_string(c) + "u * push.stride + e]";
        s += word + " = " + (col.bank == KernelBank::eInt ? "uint(" + ir(col.reg) + ")" : "floatBitsToUint(" + fr(col.reg) + ")") + ";\n";
    }
    s += "}\n";
    return s;
}

void
Generator::run()
{
    if (_program.prologueSize > _program.instructions.size() || _program.intRegisters < 1) {
        malformed();
        return;
    }
    if (!addBindings()) {
        return;
    }
    std::string prologue;
    for (std::uint32_t k = 0; k < _program.prologueSize; ++k) {
        if (!emitInstruction(k, prologue)) {
            return;
        }
    }
    std::string body;
    if (!emitBody(body)) {
        return;
    }
    _plan.source = assemble(prologue, body);
}

} // namespace

KernelGpuPlan
kernelGpuPlan(const KernelProgram& program, const KernelGpuTarget& target)
{
    KernelGpuPlan plan;
    Generator(program, target, plan).run();
    if (!plan.runsOnGpu()) {
        plan.source.clear();
        plan.columns.clear();
        plan.uniforms.clear();
    }
    return plan;
}

NATRON_NAMESPACE_EXIT
