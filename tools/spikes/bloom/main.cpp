// BloomSpikeBench: see README.md.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "spikes.h"

using namespace bloomspike;

namespace {

void
usage()
{
    std::printf(
        "BloomSpikeBench [options]\n"
        "  --cpu-only             spike 1 only (CPU quality, seams, timing)\n"
        "  --skip-quality         skip the CPU quality table\n"
        "  --skip-seam            skip the seam test\n"
        "  --skip-cpu-timing      skip the CPU timing table\n"
        "  --skip-gpu-check       skip the GPU against CPU check\n"
        "  --skip-gpu-timing      skip the GPU timing table\n"
        "  --sizes a,b,..         quality sizes (default 3,25,100,400)\n"
        "  --counts a,b,..        quality counts (default 1,3,5,8)\n"
        "  --timing-sizes a,b,..  CPU and GPU timing sizes (default 50,200)\n"
        "  --timing-count n       CPU and GPU timing count (default 5)\n"
        "  --variants a,b,..      of 1331+bilinear, 1331+bicubic, box+bilinear, box+bicubic (default all)\n"
        "  --sigma-min s          smallest blur sigma, in level pixels, at a decimated level (default 2.5)\n"
        "  --ratio r              bloom ratio (default 2)\n"
        "  --threads n            CPU threads for 'all' (default: hardware concurrency)\n"
        "  --iters n              timed runs per measurement (default 5)\n"
        "  --budget-ms ms         per-stage budget before a CPU reference stage is extrapolated (default 1000)\n"
        "  --quality-size WxH     quality image (default 960x540)\n"
        "  --gpu-res a,b,..|none  timing resolutions: hd, uhd or WxH; none skips all timing (default hd,uhd)\n"
        "  --out dir              CSV directory, appended to (default build/spikes/bloom)\n");
}

std::vector<std::string>
split(const char* s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

} // namespace

int
main(int argc, char** argv)
{
    Config cfg;
    bool cpuOnly = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--cpu-only")) {
            cpuOnly = true;
        } else if (!std::strcmp(a, "--skip-quality")) {
            cfg.quality = false;
        } else if (!std::strcmp(a, "--skip-seam")) {
            cfg.seam = false;
        } else if (!std::strcmp(a, "--skip-cpu-timing")) {
            cfg.cpuTiming = false;
        } else if (!std::strcmp(a, "--skip-gpu-check")) {
            cfg.gpuCheck = false;
        } else if (!std::strcmp(a, "--skip-gpu-timing")) {
            cfg.gpuTiming = false;
        } else if (!std::strcmp(a, "--sizes")) {
            cfg.sizes.clear();
            for (const std::string& s : split(value())) {
                cfg.sizes.push_back(std::atof(s.c_str()));
            }
        } else if (!std::strcmp(a, "--counts")) {
            cfg.counts.clear();
            for (const std::string& s : split(value())) {
                cfg.counts.push_back(std::max(1, std::atoi(s.c_str())));
            }
        } else if (!std::strcmp(a, "--timing-sizes")) {
            cfg.timingSizes.clear();
            for (const std::string& s : split(value())) {
                cfg.timingSizes.push_back(std::atof(s.c_str()));
            }
        } else if (!std::strcmp(a, "--timing-count")) {
            cfg.timingCount = std::max(1, std::atoi(value()));
        } else if (!std::strcmp(a, "--variants")) {
            cfg.variants.clear();
            for (const std::string& s : split(value())) {
                bool found = false;
                for (const Variant& v : allVariants()) {
                    if (variantName(v) == s) {
                        cfg.variants.push_back(v);
                        found = true;
                    }
                }
                if (!found) {
                    std::fprintf(stderr, "unknown variant %s\n", s.c_str());
                    return 2;
                }
            }
        } else if (!std::strcmp(a, "--sigma-min")) {
            cfg.sigmaMin = std::atof(value());
        } else if (!std::strcmp(a, "--ratio")) {
            cfg.ratio = std::atof(value());
        } else if (!std::strcmp(a, "--threads")) {
            cfg.threads = unsigned(std::max(1, std::atoi(value())));
        } else if (!std::strcmp(a, "--iters")) {
            cfg.iters = std::max(1, std::atoi(value()));
        } else if (!std::strcmp(a, "--budget-ms")) {
            cfg.budgetMs = std::atof(value());
        } else if (!std::strcmp(a, "--quality-size")) {
            if (std::sscanf(value(), "%dx%d", &cfg.qualityWidth, &cfg.qualityHeight) != 2) {
                std::fprintf(stderr, "--quality-size takes WxH\n");
                return 2;
            }
        } else if (!std::strcmp(a, "--gpu-res")) {
            const char* v = value();
            cfg.resolutions.clear();
            if (std::strcmp(v, "none") != 0) {
                cfg.resolutions = split(v);
            }
        } else if (!std::strcmp(a, "--out")) {
            cfg.out = value();
        } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a);
            usage();
            return 2;
        }
    }
    if (cfg.resolutions.empty()) {
        cfg.cpuTiming = false;
        cfg.gpuTiming = false;
    }

    runCpuSpike(cfg);
    bool ok = true;
    if (!cpuOnly && (cfg.gpuCheck || cfg.gpuTiming)) {
        ok = runGpuSpike(cfg);
    }
    std::printf("\nCSV appended under %s\n", cfg.out.c_str());
    return ok ? 0 : 1;
}
