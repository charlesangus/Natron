module mergeops;

// Merge operators on premultiplied pixels: A over B, with a and b their alphas. This module is the
// canonical source; Engine/Nodes/Merge/MergeOps.cpp is its CPU twin, kept in the same operand order,
// and Tests/Gpu/MergeOpsOracle_Test.cpp holds the two together. Operator indices follow the
// alphabetical list in MergeOps.h.
//
// The formulas are Nuke's, with the openfx-misc extras (hue, saturation, color, luminosity, freeze,
// grain-extract, grain-merge, pinlight, reflect). Where a formula clamps, flips sign, divides by
// zero or jumps for values above 1 or below 0, this module uses a continuous unbounded variant, so
// scene-linear values pass through. The deviations:
//
// | Operator       | Nuke / openfx-misc                         | Here                                         |
// |----------------|--------------------------------------------|----------------------------------------------|
// | color-burn     | 1 - min(1, (1-B)/A), A if A <= 0           | 1 - (1-B)/A, A if A <= 0 (no clamp)          |
// | color-dodge    | min(1, B/(1-A)), A if A >= 1               | B/(1-A), A if A >= 1 (no clamp)              |
// | divide         | A/B, 0 if A < 0 and B < 0                  | A/B, 0 if B == 0 (negatives: positive)       |
// | multiply       | AB, A if A < 0 and B < 0                   | AB (negatives: positive)                     |
// | screen         | A+B-AB if A or B <= 1, else max(A, B)      | A+B-AB if A and B <= 1, else max(A, B)       |
// | geometric      | 2AB/(A+B)                                  | 2AB/(A+B), 0 if A+B == 0                     |
// | hard-light     | multiply (negatives jump), screen          | plain multiply, screen as above              |
// | overlay        | multiply (negatives jump), screen          | plain multiply, screen as above              |
// | soft-light     | openfx-misc: SVG piecewise                 | Nuke: B(2A+B(1-AB)) if AB < 1, else 2AB      |
// | pinlight       | max(B, 2A-1) if A >= 0.5, else min(B, 2A)  | min(max(B, 2A-1), 2A)                        |
// | reflect        | min(1, A²/(1-B)), 1 if B >= 1              | A²/(1-B), B if B >= 1 (no clamp)             |
// | freeze         | max(0, 1 - sqrt(max(0, 1-A))/B), 0 if B<=0 | 1 - ssqrt(1-A)/B, 0 if B <= 0, ssqrt signed  |
// | hue, saturation, color, luminosity | unpremultiplied blend, colour clipped to [0, a] | premultiplied form, no clip      |
//
// Every guard that remains sits where the formula has a pole, so no finite choice is continuous.
// The conjoint-over and disjoint-over factors B(1 - a/b) and B(1-a)/b are Porter-Duff's, continuous
// where their branches meet; disjoint-over keeps A alone once a >= 1, where (1-a)/b would turn
// negative. In screen, both forms equal 1 on the edge of the unit square, so taking max(A, B) as
// soon as either side exceeds 1 joins them without the jump Nuke's "or" leaves where both cross 1.
//
// The four non-separable operators are the PDF blend modes, written on premultiplied values. The
// PDF form (1-a)B + (1-b)A + ab·Blend(A/a, B/b) is rewritten through SetLum's and SetSat's scaling,
// so it never divides by an alpha and stays continuous as an alpha goes to zero:
//   hue        SetLum(SetSat(A, a·Sat(B)), a·Lum(B))
//   saturation SetLum(SetSat(B, b·Sat(A)), a·Lum(B))
//   color      SetLum(b·A, a·Lum(B))
//   luminosity SetLum(a·B, b·Lum(A))
// Their fourth channel follows over, A + B(1-a). The colour clip is dropped, so HDR colours
// survive. Separable operators treat every channel alike, the fourth included.

public static const uint MERGE_ATOP = 0;
public static const uint MERGE_AVERAGE = 1;
public static const uint MERGE_COLOR = 2;
public static const uint MERGE_COLOR_BURN = 3;
public static const uint MERGE_COLOR_DODGE = 4;
public static const uint MERGE_CONJOINT_OVER = 5;
public static const uint MERGE_COPY = 6;
public static const uint MERGE_DIFFERENCE = 7;
public static const uint MERGE_DISJOINT_OVER = 8;
public static const uint MERGE_DIVIDE = 9;
public static const uint MERGE_EXCLUSION = 10;
public static const uint MERGE_FREEZE = 11;
public static const uint MERGE_FROM = 12;
public static const uint MERGE_GEOMETRIC = 13;
public static const uint MERGE_GRAIN_EXTRACT = 14;
public static const uint MERGE_GRAIN_MERGE = 15;
public static const uint MERGE_HARD_LIGHT = 16;
public static const uint MERGE_HUE = 17;
public static const uint MERGE_HYPOT = 18;
public static const uint MERGE_IN = 19;
public static const uint MERGE_LUMINOSITY = 20;
public static const uint MERGE_MASK = 21;
public static const uint MERGE_MATTE = 22;
public static const uint MERGE_MAX = 23;
public static const uint MERGE_MIN = 24;
public static const uint MERGE_MINUS = 25;
public static const uint MERGE_MULTIPLY = 26;
public static const uint MERGE_OUT = 27;
public static const uint MERGE_OVER = 28;
public static const uint MERGE_OVERLAY = 29;
public static const uint MERGE_PIN_LIGHT = 30;
public static const uint MERGE_PLUS = 31;
public static const uint MERGE_REFLECT = 32;
public static const uint MERGE_SATURATION = 33;
public static const uint MERGE_SCREEN = 34;
public static const uint MERGE_SOFT_LIGHT = 35;
public static const uint MERGE_STENCIL = 36;
public static const uint MERGE_UNDER = 37;
public static const uint MERGE_XOR = 38;
public static const uint MERGE_OPERATOR_COUNT = 39;

public bool mergeIsSeparable(uint op)
{
    return op != MERGE_HUE && op != MERGE_SATURATION && op != MERGE_COLOR && op != MERGE_LUMINOSITY;
}

// Selects spelled out so NaN lands on the same side as the CPU twin, which SPIR-V's FMax/FMin
// leave undefined.
float mergeMax(float x, float y)
{
    return (x < y) ? y : x;
}

float mergeMin(float x, float y)
{
    return (y < x) ? y : x;
}

float mergeScreen(float A, float B)
{
    if (A <= 1.0 && B <= 1.0)
        return A + B - A * B;
    return mergeMax(A, B);
}

float mergeHardLight(float A, float B)
{
    if (A < 0.5)
        return 2.0 * A * B;
    return mergeScreen(2.0 * A - 1.0, B);
}

float mergeSignedSqrt(float x)
{
    return (x < 0.0) ? -sqrt(-x) : sqrt(x);
}

public float mergeSeparable(uint op, float A, float a, float B, float b)
{
    switch (op)
    {
    case MERGE_ATOP:
        return A * b + B * (1.0 - a);
    case MERGE_AVERAGE:
        return (A + B) * 0.5;
    case MERGE_COLOR_BURN:
        return (A > 0.0) ? 1.0 - (1.0 - B) / A : A;
    case MERGE_COLOR_DODGE:
        return (A < 1.0) ? B / (1.0 - A) : A;
    case MERGE_CONJOINT_OVER:
        if (a >= b)
            return A;
        if (b <= 0.0)
            return A + B;
        return A + B * (1.0 - a / b);
    case MERGE_COPY:
        return A;
    case MERGE_DIFFERENCE:
        return abs(A - B);
    case MERGE_DISJOINT_OVER:
        if (a >= 1.0)
            return A;
        if (a + b <= 1.0)
            return A + B;
        return A + B * (1.0 - a) / b;
    case MERGE_DIVIDE:
        return (B != 0.0) ? A / B : 0.0;
    case MERGE_EXCLUSION:
        return A + B - 2.0 * A * B;
    case MERGE_FREEZE:
        return (B > 0.0) ? 1.0 - mergeSignedSqrt(1.0 - A) / B : 0.0;
    case MERGE_FROM:
        return B - A;
    case MERGE_GEOMETRIC:
    {
        const float s = A + B;
        return (s != 0.0) ? 2.0 * A * B / s : 0.0;
    }
    case MERGE_GRAIN_EXTRACT:
        return B - A + 0.5;
    case MERGE_GRAIN_MERGE:
        return B + A - 0.5;
    case MERGE_HARD_LIGHT:
        return mergeHardLight(A, B);
    case MERGE_HYPOT:
        return sqrt(A * A + B * B);
    case MERGE_IN:
        return A * b;
    case MERGE_MASK:
        return B * a;
    case MERGE_MATTE:
        return A * a + B * (1.0 - a);
    case MERGE_MAX:
        return mergeMax(A, B);
    case MERGE_MIN:
        return mergeMin(A, B);
    case MERGE_MINUS:
        return A - B;
    case MERGE_MULTIPLY:
        return A * B;
    case MERGE_OUT:
        return A * (1.0 - b);
    case MERGE_OVER:
        return A + B * (1.0 - a);
    case MERGE_OVERLAY:
        return mergeHardLight(B, A);
    case MERGE_PIN_LIGHT:
        return mergeMin(mergeMax(B, 2.0 * A - 1.0), 2.0 * A);
    case MERGE_PLUS:
        return A + B;
    case MERGE_REFLECT:
        return (B < 1.0) ? A * A / (1.0 - B) : B;
    case MERGE_SCREEN:
        return mergeScreen(A, B);
    case MERGE_SOFT_LIGHT:
    {
        const float ab = A * B;
        return (ab < 1.0) ? B * (2.0 * A + B * (1.0 - ab)) : 2.0 * ab;
    }
    case MERGE_STENCIL:
        return B * (1.0 - a);
    case MERGE_UNDER:
        return A * (1.0 - b) + B;
    case MERGE_XOR:
        return A * (1.0 - b) + B * (1.0 - a);
    default:
        return B;
    }
}

float mergeLum(float3 c)
{
    return c.x * 0.3 + c.y * 0.59 + c.z * 0.11;
}

float mergeSat(float3 c)
{
    return mergeMax(mergeMax(c.x, c.y), c.z) - mergeMin(mergeMin(c.x, c.y), c.z);
}

float3 mergeSetLum(float3 c, float l)
{
    const float d = l - mergeLum(c);
    return float3(c.x + d, c.y + d, c.z + d);
}

// The PDF's SetSat: the smallest channel goes to 0, the largest to s, the middle one in proportion.
float3 mergeSetSat(float3 c, float s)
{
    const float mn = mergeMin(mergeMin(c.x, c.y), c.z);
    const float t = mergeMax(mergeMax(c.x, c.y), c.z) - mn;
    if (!(t > 0.0))
        return float3(0.0, 0.0, 0.0);
    const float k = s / t;
    return float3((c.x - mn) * k, (c.y - mn) * k, (c.z - mn) * k);
}

public float3 mergeNonSeparable(uint op, float3 A, float a, float3 B, float b)
{
    float3 blend;
    switch (op)
    {
    case MERGE_HUE:
        blend = mergeSetLum(mergeSetSat(A, a * mergeSat(B)), a * mergeLum(B));
        break;
    case MERGE_SATURATION:
        blend = mergeSetLum(mergeSetSat(B, b * mergeSat(A)), a * mergeLum(B));
        break;
    case MERGE_COLOR:
        blend = mergeSetLum(A * b, a * mergeLum(B));
        break;
    default:
        blend = mergeSetLum(B * a, b * mergeLum(A));
        break;
    }
    const float ka = 1.0 - a;
    const float kb = 1.0 - b;
    return float3(A.x * kb + B.x * ka + blend.x,
                  A.y * kb + B.y * ka + blend.y,
                  A.z * kb + B.z * ka + blend.z);
}

// A pixel in channel-bit order, absent channels reading zero; a and b hold each channel's alpha.
// The non-separable operators blend the colour channels with the alphas of the first one.
public float4 mergePixel(uint op, float4 A, float4 a, float4 B, float4 b)
{
    if (mergeIsSeparable(op))
        return float4(mergeSeparable(op, A.x, a.x, B.x, b.x),
                      mergeSeparable(op, A.y, a.y, B.y, b.y),
                      mergeSeparable(op, A.z, a.z, B.z, b.z),
                      mergeSeparable(op, A.w, a.w, B.w, b.w));
    const float3 c = mergeNonSeparable(op, A.xyz, a.x, B.xyz, b.x);
    return float4(c, mergeSeparable(MERGE_OVER, A.w, a.w, B.w, b.w));
}
