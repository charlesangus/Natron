#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "../ref/BlurRef.h"

// Cases and bounds shared by the CPU-twin and GPU blur tests.
namespace blurfix {

struct Size
{
    int w, h;
};

// Includes images smaller than the radius of every sigma in kSigmas, and odd sizes.
inline constexpr Size kSizes[] = {{1, 1}, {2, 3}, {5, 3}, {37, 23}, {129, 131}, {257, 5}, {4, 300}};
inline constexpr double kSigmas[] = {0.5, 3.0, 25.0, 100.0};
inline constexpr int kChannelCounts[] = {1, 3, 4};
inline constexpr double kTolerance = 2e-6;
inline constexpr uint32_t kGroup = 128;

// Mirrors BlurParams in blur.slang, which has no padding between members.
struct BlurParams
{
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t radius;
    uint32_t vertical;
    uint32_t neumann;
};

inline std::vector<float> makeImage(int width, int height, int channels, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> img(size_t(width) * height * channels);
    for (float& v : img)
        v = dist(rng);
    return img;
}

inline std::vector<double> reference(const std::vector<float>& img, int width, int height, int channels,
                                     double sigma, bool neumann)
{
    return blurref::blur(std::vector<double>(img.begin(), img.end()), width, height, channels, sigma, neumann);
}

} // namespace blurfix
