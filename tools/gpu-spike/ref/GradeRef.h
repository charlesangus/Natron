#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

// Double-precision transcription of GradeKernel in Engine/Nodes/Color/Grade.cpp,
// which sits in an anonymous namespace and cannot be linked from here.
namespace graderef {

struct Channel
{
    double a;
    double b;
    double gamma;
};

inline double grade(double v, const Channel& ch)
{
    const double x = (ch.a * v) + ch.b;

    if (ch.gamma <= 0) {
        if (x < 1.)
            return 0.;
        if (x == 1.)
            return 1.;
        return std::numeric_limits<double>::infinity();
    }
    if (ch.gamma == 1.)
        return x;
    if (x <= 0)
        return x;
    return std::pow(x, 1. / ch.gamma);
}

inline double invgrade(double v, const Channel& ch)
{
    if ((ch.gamma != 1.) && (v > 0))
        v = std::pow(v, ch.gamma);
    v = v - ch.b;
    if (ch.a != 0)
        v /= ch.a;
    return v;
}

inline double apply(double v, const Channel& ch, bool reverse, bool clampBlack, bool clampWhite)
{
    v = reverse ? invgrade(v, ch) : grade(v, ch);
    if (clampBlack)
        v = (std::max)(0., v);
    if (clampWhite)
        v = (std::min)(1., v);
    return v;
}

} // namespace graderef
