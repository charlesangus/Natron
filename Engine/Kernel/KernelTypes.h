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

#ifndef Engine_Kernel_KernelTypes_h
#define Engine_Kernel_KernelTypes_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The value types of the kernel language. Vectors and matrices are float tuples; a matrix is
 * stored row-major and a vector multiplies it as a row vector.
 **/
enum class KernelBaseType : std::uint8_t {
    eVoid,
    eInt,
    eFloat,
    eVector2,
    eVector,
    eVector4,
    eMatrix3,
    eMatrix,
    eString,
};

struct KernelType {
    KernelBaseType base = KernelBaseType::eVoid;
    bool array = false;

    constexpr KernelType() = default;
    constexpr KernelType(KernelBaseType b, bool isArray = false)
        : base(b)
        , array(isArray)
    {
    }

    constexpr bool operator==(const KernelType&) const = default;

    constexpr KernelType element() const { return KernelType(base); }
    constexpr bool isVoid() const { return base == KernelBaseType::eVoid; }
    constexpr bool is(KernelBaseType b) const { return !array && base == b; }
};

constexpr int
kernelComponentCount(KernelBaseType t)
{
    switch (t) {
    case KernelBaseType::eVoid:
        return 0;
    case KernelBaseType::eVector2:
        return 2;
    case KernelBaseType::eVector:
        return 3;
    case KernelBaseType::eVector4:
        return 4;
    case KernelBaseType::eMatrix3:
        return 9;
    case KernelBaseType::eMatrix:
        return 16;
    default:
        return 1;
    }
}

constexpr bool
kernelIsFloatTuple(KernelBaseType t)
{
    return t == KernelBaseType::eFloat || t == KernelBaseType::eVector2 || t == KernelBaseType::eVector
        || t == KernelBaseType::eVector4 || t == KernelBaseType::eMatrix3 || t == KernelBaseType::eMatrix;
}

constexpr bool
kernelIsVector(KernelBaseType t)
{
    return t == KernelBaseType::eVector2 || t == KernelBaseType::eVector || t == KernelBaseType::eVector4;
}

constexpr bool
kernelIsMatrix(KernelBaseType t)
{
    return t == KernelBaseType::eMatrix3 || t == KernelBaseType::eMatrix;
}

inline const char*
kernelBaseTypeName(KernelBaseType t)
{
    switch (t) {
    case KernelBaseType::eVoid:
        return "void";
    case KernelBaseType::eInt:
        return "int";
    case KernelBaseType::eFloat:
        return "float";
    case KernelBaseType::eVector2:
        return "vector2";
    case KernelBaseType::eVector:
        return "vector";
    case KernelBaseType::eVector4:
        return "vector4";
    case KernelBaseType::eMatrix3:
        return "matrix3";
    case KernelBaseType::eMatrix:
        return "matrix";
    case KernelBaseType::eString:
        return "string";
    }
    return "?";
}

inline std::string
kernelTypeName(KernelType t)
{
    std::string name = kernelBaseTypeName(t.base);
    if (t.array) {
        name += "[]";
    }
    return name;
}

/**
 * @brief Register banks. Every register holds one value per lane of a batch. Ints, masks and
 * booleans (0 or 1) live in the int bank; float tuples take one float register per component;
 * strings and arrays are per-lane handles whose storage the runner owns.
 **/
enum class KernelBank : std::uint8_t {
    eFloat,
    eInt,
    eString,
    eArray,
};

constexpr KernelBank
kernelBankOf(KernelType t)
{
    if (t.array) {
        return KernelBank::eArray;
    }
    if (t.base == KernelBaseType::eInt) {
        return KernelBank::eInt;
    }
    if (t.base == KernelBaseType::eString) {
        return KernelBank::eString;
    }
    return KernelBank::eFloat;
}

/**
 * @brief The batched interpreter's instruction set. Each instruction runs across every lane of the
 * batch. Operand fields are register numbers in the bank the opcode implies; d is the destination.
 * Control flow never diverges per lane: conditional code runs under an int mask register, and only
 * writes (selects), array updates and calls honour the mask. A comparison or logical op yields 0 or 1.
 **/
enum class KernelOp : std::uint8_t {
    // F[d] = fimm, I[d] = imm, S[d] = program.strings[imm]
    eFConst,
    eIConst,
    eSConst,
    // d = a. eACopy copies the array value, arrays being values in the language.
    eFMov,
    eIMov,
    eSMov,
    eACopy,
    // d = I[a] ? b : c
    eFSel,
    eISel,
    eSSel,
    eASel,

    eFAdd,
    eFSub,
    eFMul,
    eFDiv,
    eFMod, // fmod
    eFMulAdd, // a * b + c, rounded after the multiply as well, so results match unfused C++
    eFNeg,
    eFMin,
    eFMax,
    eFPow,
    eFAtan2, // atan2(a, b)
    eFAbs,
    eFFloor,
    eFCeil,
    eFRound,
    eFTrunc,
    eFFrac, // a - floor(a)
    eFSign,
    eFSqrt,
    eFExp,
    eFLog,
    eFLog10,
    eFSin,
    eFCos,
    eFTan,
    eFAsin,
    eFAcos,
    eFAtan,
    // I[d] = F[a] op F[b]
    eFLt,
    eFLe,
    eFGt,
    eFGe,
    eFEq,
    eFNe,

    // Integer division and modulo by zero yield 0.
    eIAdd,
    eISub,
    eIMul,
    eIDiv,
    eIMod,
    eINeg,
    eIAbs,
    eIMin,
    eIMax,
    eIBitAnd,
    eIBitOr,
    eIBitXor,
    eIBitNot,
    eIShl,
    eIShr,
    eILt,
    eILe,
    eIGt,
    eIGe,
    eIEq,
    eINe,
    eIBool, // a != 0
    eINot, // a == 0
    eIAnd, // a && b
    eIOr, // a || b
    eIAndNot, // a && !b

    eI2F,
    eF2I, // truncates toward zero

    // I[d] = S[a] op S[b]; S[d] = S[a] + S[b]
    eSEq,
    eSNe,
    eSConcat,

    // Arrays. imm is the element KernelBaseType; element values are operand lists (one operand per
    // component). A read out of range yields zeros and a negative index counts from the end.
    eANew, // A[d] = empty
    eALen, // I[d] = length of A[a]
    eAGet, // operands = element of A[a] at I[b]
    eASet, // under mask I[a]: A[b] at I[c] = operands, growing the array as needed
    eAPush, // under mask I[a]: append operands to A[b]
    eAResize, // under mask I[a]: resize A[b] to I[c], new elements zero

    eJump, // to imm
    eJumpIfNone, // to imm when I[a] is 0 on every lane

    // Call program.functions[imm] under mask I[a]. The first b operands receive the results, one per
    // component, and the rest are the arguments.
    eCall,
};

/**
 * @brief Functions the language core provides beyond plain opcodes. The runner implements them.
 **/
enum class KernelCoreFunction : std::int32_t {
    eNoiseFloat, // noise(float) -> float
    eNoiseVector, // noise(vector) -> float
    eNoiseVectorToVector, // noise(vector) -> vector
    eRandFloat, // rand(float) -> float
    eRandVector, // rand(vector) -> float
    eRandFloatToVector, // rand(float) -> vector
    eRandVectorToVector, // rand(vector) -> vector
    eSprintf, // sprintf(string format, ...)
    eStrlen,
    eItoa,
    eAtoi,
    eAtof,
    eStartsWith,
    eEndsWith,
    eInvert3,
    eInvert4,
    eDeterminant3,
    eDeterminant4,
    eQuaternionAxisAngle, // quaternion(float angle, vector axis) -> vector4
    eQuaternionMultiply, // qmultiply(vector4, vector4) -> vector4
    eQuaternionRotate, // qrotate(vector4, vector) -> vector
    // The FastNoise2 family over a vector4 position. A vector result is three fields with their own
    // seeds. The fractal forms also take int octaves, float lacunarity and float gain.
    ePerlinNoise4, // pnoise(vector4) -> float
    ePerlinNoise4ToVector, // pnoise(vector4) -> vector
    eSimplexNoise4, // xnoise(vector4) -> float
    eSimplexNoise4ToVector, // xnoise(vector4) -> vector
    eValueNoise4, // valuenoise(vector4) -> float
    eValueNoise4ToVector, // valuenoise(vector4) -> vector
    eWorleyNoise4, // wnoise(vector4) -> float
    eWorleyNoise4ToVector, // wnoise(vector4) -> vector
    eFbmNoise4, // onoise(vector4, int, float, float) -> float
    eFbmNoise4ToVector, // onoise(vector4, int, float, float) -> vector
    eTurbulenceNoise4, // anoise(vector4, int, float, float) -> float
    eTurbulenceNoise4ToVector, // anoise(vector4, int, float, float) -> vector
    eCurlNoise4, // curlnoise(vector4, int, float, float) -> vector
};

struct KernelOperand {
    KernelBank bank = KernelBank::eFloat;
    std::int32_t reg = 0;
};

struct KernelInstruction {
    KernelOp op = KernelOp::eFMov;
    std::int32_t d = 0;
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    std::int32_t imm = 0;
    float fimm = 0.f;
    std::uint32_t operandBegin = 0;
    std::uint32_t operandCount = 0;
};

enum class KernelVariableKind : std::uint8_t {
    eAttribute,
    eBuiltin,
};

/**
 * @brief What a binding provider resolved a name to. id and domain are the provider's own: the
 * front end never interprets them and hands them back to the runner through the program.
 **/
struct KernelVariable {
    KernelVariableKind kind = KernelVariableKind::eAttribute;
    std::int32_t id = -1;
    std::int32_t domain = 0;
    KernelType type;
    bool readOnly = false;
};

/**
 * @brief A bound variable as the program uses it. The runner fills registers before running a batch
 * when needsGather is set, and scatters them back after it when written is set.
 **/
struct KernelProgramBinding {
    std::string name;
    std::int32_t input = 0;
    // The type the registers hold; variable.type is what the provider stores, which may differ
    // (an int attribute read through f@), in which case the runner converts.
    KernelType type;
    KernelVariable variable;
    std::vector<std::int32_t> registers;
    bool read = false;
    bool written = false;
    // A read, a write under a mask, or a write to part of the tuple: lanes keep their old value.
    bool needsGather = false;
};

enum class KernelFunctionProvider : std::uint8_t {
    eCore,
    eBinding,
};

struct KernelParameter {
    KernelType type;
    // The argument must be a literal, such as an attribute name a provider resolves when it binds.
    bool constant = false;
};

struct KernelFunctionSignature {
    std::string name;
    KernelType result;
    std::vector<KernelParameter> params;
    // Extra arguments of any non-void type follow the declared ones.
    bool variadic = false;
    bool sideEffects = false;
    std::int32_t id = -1;
};

struct KernelProgramFunction {
    KernelFunctionProvider provider = KernelFunctionProvider::eCore;
    KernelFunctionSignature signature;
};

/**
 * @brief A compiled kernel. Instructions [0, prologueSize) only load constants and may run once per
 * thread; the rest run once per batch. Int register kEntryMaskRegister holds 1 on the lanes the
 * runner fills with elements and 0 on the batch's unused tail; the runner sets it before the body.
 **/
struct KernelProgram {
    static constexpr std::int32_t kEntryMaskRegister = 0;

    std::vector<KernelInstruction> instructions;
    std::vector<KernelOperand> operands;
    std::uint32_t prologueSize = 0;
    std::int32_t floatRegisters = 0;
    std::int32_t intRegisters = 1;
    std::int32_t stringRegisters = 0;
    std::int32_t arrayRegisters = 0;
    std::vector<std::string> strings;
    std::vector<KernelProgramBinding> bindings;
    std::vector<KernelProgramFunction> functions;

    const KernelProgramBinding* findBinding(const std::string& name, std::int32_t input = 0) const
    {
        for (const KernelProgramBinding& b : bindings) {
            if (b.name == name && b.input == input) {
                return &b;
            }
        }
        return nullptr;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Kernel_KernelTypes_h
