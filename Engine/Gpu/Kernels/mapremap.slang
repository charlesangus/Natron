// MapInputParams::remap() and mapGammaPow() of Engine/Nodes/Filter/MapInput.cpp, for the kernels
// of the variable-size filters. Every operation is the CPU's float operation in the same order,
// with its coefficients by their bits, precise so that none contracts, and the one division is
// rounded correctly in integer arithmetic, so the Map value matches the CPU's bit for bit.

// By its bits, as a comparison need not see a NaN.
bool mapRemapIsNan(float v)
{
    return (asuint(v) & 0x7fffffffu) > 0x7f800000u;
}

// a / b rounded to nearest even, as the CPU's float division is. Vulkan lets a float division be
// off by 2.5 ulp, and RADV divides by multiplying with a reciprocal, so the quotient is formed from
// the significands by shift and subtract instead. For a and b normal or zero, b nonzero, and a
// nonzero quotient that is normal, which mapGammaPow's (f - 1) / (f + 1) is: |f - 1| is 0 or at
// least 2^-24, and f + 1 is in [1.7, 2.5].
float mapDivide(float a, float b)
{
    if (a == 0.0)
        return asfloat((asuint(a) ^ asuint(b)) & 0x80000000u);
    const uint ab = asuint(a);
    const uint bb = asuint(b);
    const uint signBit = (ab ^ bb) & 0x80000000u;
    int e = int((ab >> 23) & 0xffu) - int((bb >> 23) & 0xffu);
    uint ma = (ab & 0x007fffffu) | 0x00800000u;
    const uint mb = (bb & 0x007fffffu) | 0x00800000u;
    // ma / mb in [1, 2), so that the quotient's first bit is its leading one.
    if (ma < mb) {
        ma <<= 1;
        e -= 1;
    }
    // 24 quotient bits; the remainder stays below 2 mb < 2^25.
    uint q = 0u;
    uint r = ma;
    for (int i = 0; i < 24; ++i) {
        q <<= 1;
        if (r >= mb) {
            r -= mb;
            q |= 1u;
        }
        r <<= 1;
    }
    // r is twice the remainder: above mb rounds up, equal to it is a tie that rounds to even.
    if (r > mb || (r == mb && (q & 1u) != 0u))
        q += 1u;
    if (q == 0x01000000u) {
        q >>= 1;
        e += 1;
    }
    return asfloat(signBit | (uint(e + 127) << 23) | (q & 0x007fffffu));
}

// mapGammaPow(): m^e for m in (0, 1) and e > 0.
float mapGammaPow(float m, float e)
{
    // The smallest normal float.
    if (m < asfloat(0x00800000u))
        return 0.0;
    // m = f * 2^k, with f in [sqrt(2)/2, sqrt(2)].
    const uint bits = asuint(m);
    int k = int(bits >> 23) - 127;
    float f = asfloat((bits & 0x007fffffu) | 0x3f800000u);
    if (f > asfloat(0x3fb504f3u)) {
        precise float halved = f * 0.5;
        f = halved;
        k += 1;
    }
    precise float num = f - 1.0;
    precise float den = f + 1.0;
    const float s = mapDivide(num, den);
    precise float s2 = s * s;
    // log2(f) = s * (c1 + c3 s^2 + c5 s^4 + c7 s^6 + c9 s^8).
    precise float p = asfloat(0x3ea4258au);
    precise float t3 = p * s2;
    p = t3 + asfloat(0x3ed30bb1u);
    precise float t2 = p * s2;
    p = t2 + asfloat(0x3f13bb63u);
    precise float t1 = p * s2;
    p = t1 + asfloat(0x3f76384fu);
    precise float t0 = p * s2;
    p = t0 + asfloat(0x4038aa3bu);
    precise float logF = p * s;
    precise float log2M = float(k) + logF;
    precise float y = e * log2M;
    if (!(y >= -125.0))
        return 0.0;
    // 2^y = 2^r * 2^n, with r in [-1/2, 1/2] and n in [-125, 0].
    precise float shifted = y + 0.5;
    const float n = floor(shifted);
    precise float r = y - n;
    precise float q = asfloat(0x377fe5feu);
    precise float u6 = q * r;
    q = u6 + asfloat(0x39218489u);
    precise float u5 = q * r;
    q = u5 + asfloat(0x3aaec3ffu);
    precise float u4 = q * r;
    q = u4 + asfloat(0x3c1d955bu);
    precise float u3 = q * r;
    q = u3 + asfloat(0x3d635847u);
    precise float u2 = q * r;
    q = u2 + asfloat(0x3e75fdf0u);
    precise float u1 = q * r;
    q = u1 + asfloat(0x3f317218u);
    precise float u0 = q * r;
    q = u0 + asfloat(0x3f800000u);
    const float scale = asfloat(uint(int(n) + 127) << 23);
    precise float result = q * scale;
    return (result < 1.0) ? result : 1.0;
}

// MapInputParams::remap(): the raw sample inverted when invert is set, clamped to [0, 1], raised
// to exponent and shaped by smoothstep when smooth is set.
float mapRemap(float raw, bool invert, bool smooth, float exponent)
{
    precise float flipped = 1.0 - raw;
    const float v = invert ? flipped : raw;
    // A NaN sample reads as black.
    if (mapRemapIsNan(v) || !(v > 0.0))
        return 0.0;
    if (v >= 1.0)
        return 1.0;
    float m = v;
    if (exponent != 1.0)
        m = mapGammaPow(m, exponent);
    if (smooth) {
        precise float square = m * m;
        precise float twice = m * 2.0;
        precise float slope = 3.0 - twice;
        precise float shaped = square * slope;
        m = shaped;
    }
    return m;
}
