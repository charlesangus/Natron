module gradeops;

// Selects spelled out so NaN lands on the same side as std::max/std::min in
// the engine kernel, which SPIR-V's FMax/FMin leave undefined.
public float grade(float v, float a, float b, float gamma, float invGamma)
{
    // One rounding of a*v+b, so cancellation near zero costs no extra error.
    float x = fma(a, v, b);

    if (gamma <= 0.0)
    {
        if (x < 1.0)
            return 0.0;
        else if (x == 1.0)
            return 1.0;
        return float(1.0 / 0.0);
    }
    if (gamma == 1.0)
        return x;
    if (x <= 0.0)
        return x;
    return pow(x, invGamma);
}

public float invGrade(float v, float a, float b, float gamma)
{
    // SPIR-V Pow is exp2(y * log2(x)), so pow(inf, 0) would be inf * 0 = NaN
    // where the engine's std::pow gives 1.
    if (gamma != 1.0 && v > 0.0)
        v = (gamma == 0.0) ? 1.0 : pow(v, gamma);
    v = v - b;
    if (a != 0.0)
        v /= a;
    return v;
}

public float gradeApply(float v, float a, float b, float gamma, float invGamma,
                 bool reverse, bool clampBlack, bool clampWhite)
{
    v = reverse ? invGrade(v, a, b, gamma) : grade(v, a, b, gamma, invGamma);
    if (clampBlack)
        v = (0.0 < v) ? v : 0.0;
    if (clampWhite)
        v = (v < 1.0) ? v : 1.0;
    return v;
}
