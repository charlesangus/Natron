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

#ifndef Engine_Ai_Tensor_h
#define Engine_Ai_Tensor_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief An owning float32 tensor with its shape, in row-major order. Image tensors are NCHW, with
 * row 0 the top row of the image. Models that take or return float16 are converted at the
 * InferenceSession boundary, so callers only ever see float32.
 **/
class Tensor {
public:
    Tensor() = default;

    // Zero-filled. Every dimension must be at least 1.
    explicit Tensor(std::vector<int64_t> shape)
        : _shape(std::move(shape))
        , _data(elementCount(_shape), 0.f)
    {
    }

    Tensor(std::vector<int64_t> shape,
           std::vector<float> data)
        : _shape(std::move(shape))
        , _data(std::move(data))
    {
        if (_data.size() != elementCount(_shape)) {
            throw std::invalid_argument("Tensor: " + std::to_string(_data.size()) + " values do not fill shape " + shapeString(_shape));
        }
    }

    // NCHW, zero-filled.
    static Tensor image(int64_t batch,
                        int64_t channels,
                        int64_t height,
                        int64_t width)
    {
        return Tensor(std::vector<int64_t> { batch, channels, height, width });
    }

    static std::size_t elementCount(const std::vector<int64_t>& shape)
    {
        std::size_t count = 1;

        for (int64_t d : shape) {
            if (d < 1) {
                throw std::invalid_argument("Tensor: shape " + shapeString(shape) + " has a dimension below 1");
            }
            count *= (std::size_t)d;
        }

        return count;
    }

    static std::string shapeString(const std::vector<int64_t>& shape)
    {
        std::string s = "[";

        for (std::size_t i = 0; i < shape.size(); ++i) {
            s += (i ? "x" : "") + std::to_string(shape[i]);
        }

        return s + "]";
    }

    const std::vector<int64_t>& shape() const
    {
        return _shape;
    }

    std::size_t rank() const
    {
        return _shape.size();
    }

    int64_t dim(std::size_t i) const
    {
        return _shape.at(i);
    }

    std::size_t size() const
    {
        return _data.size();
    }

    bool empty() const
    {
        return _data.empty();
    }

    float* data()
    {
        return _data.data();
    }

    const float* data() const
    {
        return _data.data();
    }

    std::vector<float>& values()
    {
        return _data;
    }

    const std::vector<float>& values() const
    {
        return _data;
    }

    // NCHW element access. The tensor must be rank 4.
    float& at(int64_t n,
              int64_t c,
              int64_t y,
              int64_t x)
    {
        return _data[offset(n, c, y, x)];
    }

    float at(int64_t n,
             int64_t c,
             int64_t y,
             int64_t x) const
    {
        return _data[offset(n, c, y, x)];
    }

    // The start of one NCHW plane (a channel of one batch item). The tensor must be rank 4.
    float* plane(int64_t n,
                 int64_t c)
    {
        return _data.data() + offset(n, c, 0, 0);
    }

    const float* plane(int64_t n,
                       int64_t c) const
    {
        return _data.data() + offset(n, c, 0, 0);
    }

private:
    std::size_t offset(int64_t n,
                       int64_t c,
                       int64_t y,
                       int64_t x) const
    {
        return (std::size_t)(((n * _shape[1] + c) * _shape[2] + y) * _shape[3] + x);
    }

    std::vector<int64_t> _shape;
    std::vector<float> _data;
};

/**
 * @brief IEEE 754 binary16 to binary32, exact for every input including subnormals, infinities and
 * NaN.
 **/
inline float
halfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1fu;
    uint32_t mantissa = h & 0x3ffu;
    uint32_t bits;

    if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else if (exponent != 0) {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    } else if (mantissa == 0) {
        bits = sign;
    } else {
        // Subnormal: shift the leading 1 into the implicit bit, adjusting the exponent to match.
        exponent = 113u;
        while ((mantissa & 0x400u) == 0) {
            mantissa <<= 1;
            --exponent;
        }
        bits = sign | (exponent << 23) | ((mantissa & 0x3ffu) << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof f);

    return f;
}

/**
 * @brief IEEE 754 binary32 to binary16, rounding to nearest with ties to even. Values beyond the
 * half range become infinities, tiny ones subnormals or zero, and NaN stays NaN.
 **/
inline uint16_t
floatToHalf(float f)
{
    uint32_t bits;

    std::memcpy(&bits, &f, sizeof bits);
    const uint16_t sign = (uint16_t)((bits >> 16) & 0x8000u);
    const uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu) {
        return (uint16_t)(sign | 0x7c00u | (mantissa ? (0x200u | (mantissa >> 13)) : 0u));
    }
    const int halfExponent = (int)exponent - 127 + 15;
    if (halfExponent >= 0x1f) {
        return (uint16_t)(sign | 0x7c00u);
    }
    if (halfExponent <= 0) {
        if (halfExponent < -10) {
            return sign;
        }
        mantissa |= 0x800000u;
        const unsigned shift = (unsigned)(14 - halfExponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if ((rest > halfway) || ((rest == halfway) && (half & 1u))) {
            ++half;
        }

        return (uint16_t)(sign | half);
    }
    uint32_t half = ((uint32_t)halfExponent << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1fffu;
    // A carry out of the mantissa correctly bumps the exponent, up to infinity.
    if ((rest > 0x1000u) || ((rest == 0x1000u) && (half & 1u))) {
        ++half;
    }

    return (uint16_t)(sign | half);
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_Tensor_h
