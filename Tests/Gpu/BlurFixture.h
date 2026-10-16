#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "GpuBlur.h"
#include "ref/BlurRef.h"

// Cases and bounds shared by the CPU-twin and GPU blur tests.
namespace blurfix {

struct Size {
    int w, h;
};

// Includes images smaller than the radius of every sigma in kSigmas, and odd sizes.
inline constexpr Size kSizes[] = { { 1, 1 }, { 2, 3 }, { 5, 3 }, { 37, 23 }, { 129, 131 }, { 257, 5 }, { 4, 300 } };
inline constexpr double kSigmas[] = { 0.5, 3.0, 25.0, 100.0 };
inline constexpr int kChannelCounts[] = { 1, 3, 4 };
inline constexpr double kTolerance = 2e-6;
inline constexpr uint32_t kGroup = 128;

using BlurParams = gpu::GpuBlurParams;

// The kernel reads the symmetric weights of offsets 0 to radius only.
inline std::vector<float>
halfWeights(double sigma)
{
    const std::vector<double> w = blurref::makeWeights(sigma);
    return std::vector<float>(w.begin() + w.size() / 2, w.end());
}

inline std::vector<float>
makeImage(int width, int height, int channels, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> img(size_t(width) * height * channels);
    for (float& v : img)
        v = dist(rng);
    return img;
}

inline std::vector<double>
reference(const std::vector<float>& img, int width, int height, int channels,
          double sigma)
{
    return blurref::blur(std::vector<double>(img.begin(), img.end()), width, height, channels, sigma, false);
}

} // namespace blurfix
