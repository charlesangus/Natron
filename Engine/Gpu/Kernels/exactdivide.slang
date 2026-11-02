module exactdivide;

// Division rounded once, as the CPU divides. Vulkan only bounds a float quotient within 2.5 ulp, and
// RADV multiplies by an approximate reciprocal, so a kernel that must match the CPU bit for bit
// divides through divideRounded. A module with no bindings.

static const uint kDivideSignBit = 0x80000000u;
static const float kDivideNumeratorMin = asfloat(0x19800000u); // 2^-76
static const float kDivideOperandMin = asfloat(0x0D800000u); // 2^-100
static const float kDivideOperandMax = asfloat(0x71800000u); // 2^100

// a * b as p + e exactly, by Dekker's split: fma() need not round once (lavapipe's does not).
public void
exactProduct(float a, float b, out float p, out float e)
{
    precise float pr = a * b;
    precise float ca = 4097.0f * a;
    precise float ah = ca - (ca - a);
    precise float al = a - ah;
    precise float cb = 4097.0f * b;
    precise float bh = cb - (cb - b);
    precise float bl = b - bh;
    precise float er = ((ah * bh - pr) + ah * bl + al * bh) + al * bl;
    p = pr;
    e = er;
}

// One correction step makes the device quotient faithful, so its remainder is exact; the remainder
// against half a gap to each neighbour then picks the rounded quotient. Every step after the two
// device divisions is an exact add, sub or mul while no split overflows and no product's low part
// falls below FLT_MIN, which a device may flush: the operand bounds keep both clear. Outside them,
// and for zeros, infinities and NaNs, the device quotient is returned as it is.
public float
divideRounded(float a, float b)
{
    precise float q0 = a / b;
    const float magnitudeA = abs(a);
    const float magnitudeB = abs(b);
    const float magnitudeQ = abs(q0);
    if (!(magnitudeA >= kDivideNumeratorMin && magnitudeA <= kDivideOperandMax
          && magnitudeB >= kDivideOperandMin && magnitudeB <= kDivideOperandMax
          && magnitudeQ >= kDivideOperandMin && magnitudeQ <= kDivideOperandMax)) {
        return q0;
    }
    float p;
    float e;
    exactProduct(magnitudeQ, magnitudeB, p, e);
    precise float r0 = (magnitudeA - p) - e;
    precise float c0 = r0 / magnitudeB;
    precise float q1 = magnitudeQ + c0;
    exactProduct(q1, magnitudeB, p, e);
    precise float r = (magnitudeA - p) - e;
    const float up = asfloat(asuint(q1) + 1u);
    const float down = asfloat(asuint(q1) - 1u);
    precise float gapUp = up - q1;
    precise float gapDown = q1 - down;
    precise float halfUp = 0.5f * magnitudeB * gapUp;
    precise float halfDown = 0.5f * magnitudeB * gapDown;
    float q = q1;
    if (r > halfUp) {
        q = up;
    } else if (r < -halfDown) {
        q = down;
    }
    return asfloat(asuint(q) | ((asuint(a) ^ asuint(b)) & kDivideSignBit));
}
