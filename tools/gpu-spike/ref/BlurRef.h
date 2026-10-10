// Reference separable FIR Gaussian blur, double precision.
//
// The four rules any implementation must follow to match:
//   1. Sigma: the Blur node "size" is a diameter, sigma = size / 2.4.
//   2. Radius: R = ceil(3 * sigma); the kernel has 2R+1 taps at offsets -R..R.
//   3. Weights: w[k] = exp(-k^2 / (2 sigma^2)) evaluated in double, divided by
//      their sum so the taps sum to 1. Sigma <= 0 gives the identity kernel.
//   4. Boundary: Neumann clamps the sample position to the nearest edge pixel;
//      Dirichlet treats every out-of-image sample as zero. The kernel is not
//      renormalised at the edges.
// The blur runs along x then along y on interleaved channels, each channel
// independently.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace blurref {

inline int radiusForSigma(double sigma)
{
    return sigma > 0.0 ? static_cast<int>(std::ceil(3.0 * sigma)) : 0;
}

inline double sigmaForSize(double size) { return size / 2.4; }

inline std::vector<double> makeWeights(double sigma)
{
    const int r = radiusForSigma(sigma);
    std::vector<double> w(2 * r + 1, 0.0);
    if (r == 0) {
        w[0] = 1.0;
        return w;
    }
    double sum = 0.0;
    for (int k = -r; k <= r; ++k) {
        w[k + r] = std::exp(-double(k) * k / (2.0 * sigma * sigma));
        sum += w[k + r];
    }
    for (double& v : w)
        v /= sum;
    return w;
}

inline void blurAxis(const std::vector<double>& src, std::vector<double>& dst, int width, int height,
                     int channels, const std::vector<double>& w, bool vertical, bool neumann)
{
    const int r = static_cast<int>(w.size() / 2);
    const int len = vertical ? height : width;
    const int lines = vertical ? width : height;
    dst.assign(src.size(), 0.0);
    for (int line = 0; line < lines; ++line) {
        for (int p = 0; p < len; ++p) {
            for (int c = 0; c < channels; ++c) {
                double acc = 0.0;
                for (int k = -r; k <= r; ++k) {
                    int q = p + k;
                    if (q < 0 || q >= len) {
                        if (!neumann)
                            continue;
                        q = std::clamp(q, 0, len - 1);
                    }
                    const std::size_t px = vertical ? std::size_t(q) * width + line
                                                    : std::size_t(line) * width + q;
                    acc += w[k + r] * src[px * channels + c];
                }
                const std::size_t out = vertical ? std::size_t(p) * width + line
                                                 : std::size_t(line) * width + p;
                dst[out * channels + c] = acc;
            }
        }
    }
}

inline std::vector<double> blur(const std::vector<double>& src, int width, int height, int channels,
                                double sigma, bool neumann)
{
    const std::vector<double> w = makeWeights(sigma);
    std::vector<double> tmp, out;
    blurAxis(src, tmp, width, height, channels, w, false, neumann);
    blurAxis(tmp, out, width, height, channels, w, true, neumann);
    return out;
}

} // namespace blurref
