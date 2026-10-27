#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "pyramid.h"

namespace bloomspike {

struct Config {
    int qualityWidth = 960;
    int qualityHeight = 540;
    int seamWidth = 32768;
    int seamHeight = 64;
    int seamWindow = 1024;
    int seamOffset = 37;
    std::vector<double> sizes { 3., 25., 100., 400. };
    std::vector<int> counts { 1, 3, 5, 8 };
    std::vector<double> seamSizes { 3., 25., 100. };
    std::vector<int> seamCounts { 5, 8 };
    std::vector<double> timingSizes { 50., 200. };
    int timingCount = 5;
    std::vector<std::string> resolutions { "hd", "uhd" };
    std::vector<Variant> variants = allVariants();
    double ratio = 2.;
    double sigmaMin = 2.5;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    int iters = 5;
    double budgetMs = 1000.;
    std::string out = "build/spikes/bloom";
    bool quality = true;
    bool seam = true;
    bool cpuTiming = true;
    bool gpu = true;
    bool gpuCheck = true;
    bool gpuTiming = true;
};

enum class RefKind { fir,
                     iir };

bool resolutionSize(const std::string& name, int& width, int& height);

// "point", "edge" or "plate", over [0, width) x [0, height).
Image makeInput(const std::string& name, int width, int height, int nc);

// Blur i of the bloom over the whole image, black outside, for i < count, unscaled.
std::vector<Image> referenceBlurs(const Image& src, RefKind kind, const BloomParams& p, int count, Pool& pool);

struct ErrorStats {
    double peak = 0.;
    // |got - ref| over the reference's peak |ref|.
    double maxRel = 0.;
    double rmsRel = 0.;
    // max |got - ref| / (|ref| + 1e-3 * peak).
    double pxRelMax = 0.;
    double maxAbs = 0.;
};

ErrorStats compareImages(const Image& ref, const Image& got);

std::string levelList(const PyramidPlan& plan, int count);

// Appends to <out>/<name>, writing the header first when the file is new.
FILE* openCsv(const Config& cfg, const char* name, const char* header);

void runCpuSpike(const Config& cfg);
bool runGpuSpike(const Config& cfg);

} // namespace bloomspike
