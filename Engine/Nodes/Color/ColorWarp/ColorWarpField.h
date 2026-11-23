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

#ifndef NATRON_ENGINE_COLORWARPFIELD_H
#define NATRON_ENGINE_COLORWARPFIELD_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/Nodes/Color/ColorWarp/ColorWarpSpace.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One warp pin, in working-space RGB. Its displacement is
 * strength * (warp(target) - warp(source)) in the field's warp space. A pin whose displacement
 * is zero (target equal to source, or strength 0) is a hold pin: it keeps its full weight and
 * pulls nearby colours back towards identity.
 *
 * falloff is the pin's reach in warp-space units, so its meaning changes with the space. Only
 * the Gaussian kernel reads it; a pin with a falloff that is not positive has no Gaussian
 * influence except on its exact source colour.
 **/
struct ColorWarpPin {
    double source[3] = { 0., 0., 0. };
    double target[3] = { 0., 0., 0. };
    double strength = 1.;
    double falloff = 1.;
    bool enabled = true;
};

/**
 * @brief How pin displacements spread through the colour volume.
 *
 * - Gaussian: D(x) = sum(w_i d_i) / (1 + sum(w_i)), w_i = exp(-r^2 / 2 s^2) * s^2 / r^2,
 *   s = falloff / 2, r the warp-space distance to pin i, measured with the lightness component
 *   divided by the field's lightness falloff (see ColorWarpField::prepare()). The 1 is an identity weight, so colours
 *   far from every pin are unchanged (exactly, past kColorWarpGaussianCutoff falloffs). The
 *   1 / r^2 factor makes the blend pass exactly through every pin, hold pins included; it also
 *   flattens the field's gradient to zero at each pin.
 * - RbfLinear, RbfCubic: polyharmonic splines, phi(r) = -r and phi(r) = r^3, with an affine
 *   part, solved per displacement axis and regularised by adding the regularisation to the
 *   system's diagonal. Exact through every pin up to that regularisation, smooth everywhere,
 *   but global: they ignore falloff, and with no hold pins one moved pin shifts every colour.
 **/
enum ColorWarpKernelEnum {
    eColorWarpKernelGaussian,
    eColorWarpKernelRbfLinear,
    eColorWarpKernelRbfCubic
};

inline constexpr int kColorWarpKernelCount = 3;

const char* colorWarpKernelName(ColorWarpKernelEnum kernel);

inline constexpr double kColorWarpGaussianCutoff = 4.;
inline constexpr double kColorWarpDefaultRegularisation = 1e-6;
inline constexpr double kColorWarpDefaultLightnessFalloff = 1.;

/**
 * @brief A prepared warp field. prepare() does all the per-change work; apply() evaluates one
 * colour and is safe to call from many threads at once.
 *
 * Pins whose sources coincide in warp space are merged before anything else: if any of them
 * moves, the merged pin carries the mean of the moving pins' displacements and the hold pins
 * among them are dropped (a moved pin wins at its own source); otherwise they become one hold
 * pin. The merged falloff is the mean of the contributing positive falloffs, or 0 when none is
 * positive.
 *
 * The evaluated form is plain arrays so a GPU kernel can evaluate the same thing:
 * - centres: 3 per pin, warp space (Gaussian) or normalised as (x - origin) * invScale (RBF);
 * - coefficients: 3 per pin, the displacement (Gaussian) or the per-axis spline weight (RBF);
 * - invTwoSigma2: per pin, 1 / (2 s^2) (Gaussian only);
 * - lightnessAxis and lightnessStretch: the Gaussian's r^2 is |x - c|^2 + stretch * ((x - c) . axis)^2;
 * - affine: 12 values, the RBF's constant term per axis then its 3x3 slope matrix (row = axis)
 *   in normalised coordinates;
 * - the space conversion: basis()'s matrices, plus the OKLab matrices and log-toe constants
 *   ColorWarpSpace.h exposes.
 **/
class ColorWarpField {
public:
    ColorWarpField();

    /**
     * @brief lightnessFalloff scales the Gaussian's reach along the space's neutral axis
     * (colorWarpLightnessAxis()) relative to its reach across it, so 2 lets a pin reach twice as
     * far in lightness as in chroma. 1 is a round reach; one that is not positive and finite
     * reads as 1. The RBF kernels ignore it, as they ignore falloff.
     **/
    void prepare(const std::vector<ColorWarpPin>& pins,
                 ColorWarpSpaceEnum space,
                 const ColorWarpSpaceBasis& basis,
                 ColorWarpKernelEnum kernel,
                 double regularisation = kColorWarpDefaultRegularisation,
                 double lightnessFalloff = kColorWarpDefaultLightnessFalloff);

    /**
     * @brief Returns rgb itself, bit-exact, when the field is an identity, when rgb is not finite,
     * and wherever the evaluated displacement is exactly zero.
     **/
    void apply(const double rgb[3], double out[3]) const;

    /**
     * @brief The displacement at a warp-space position, for callers that already work there.
     **/
    void displacementAt(const double warp[3], double d[3]) const;

    bool isIdentity() const { return _pinCount == 0; }
    int pinCount() const { return _pinCount; }
    ColorWarpSpaceEnum space() const { return _space; }
    ColorWarpKernelEnum kernel() const { return _kernel; }
    const ColorWarpSpaceBasis& basis() const { return _basis; }
    const std::vector<double>& centres() const { return _centres; }
    const std::vector<double>& coefficients() const { return _coefficients; }
    const std::vector<double>& invTwoSigma2() const { return _invTwoSigma2; }
    const double* lightnessAxis() const { return _lightnessAxis; }
    double lightnessStretch() const { return _lightnessStretch; }
    const double* affine() const { return _affine; }
    const double* origin() const { return _origin; }
    double invScale() const { return _invScale; }

private:
    void prepareGaussian(const std::vector<double>& centres, const std::vector<double>& displacements, const std::vector<double>& falloffs);
    void prepareRbf(const std::vector<double>& centres, const std::vector<double>& displacements, double regularisation);

    ColorWarpSpaceEnum _space;
    ColorWarpKernelEnum _kernel;
    ColorWarpSpaceBasis _basis;
    int _pinCount;
    std::vector<double> _centres;
    std::vector<double> _coefficients;
    std::vector<double> _invTwoSigma2;
    double _lightnessAxis[3];
    double _lightnessStretch;
    double _affine[12];
    double _origin[3];
    double _invScale;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_COLORWARPFIELD_H
