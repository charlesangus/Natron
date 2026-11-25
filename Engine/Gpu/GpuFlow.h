#pragma once

#include "GpuDevice.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace gpu {

class GpuBuffer;
class GpuRecordContext;

// A frame or matte in a buffer. x0 and y0 place its first pixel relative to the solved window; what lies
// outside it reads as 0. offsetFloats is that pixel's first component, or for a matte its channel.
struct GpuFlowImage {
    const GpuBuffer* buffer = nullptr;
    uint32_t offsetFloats = 0;
    uint32_t rowStrideFloats = 0;
    uint32_t pixelStride = 0;
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct GpuFlowLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t offsetFloats = 0;
};

// One buffer for a whole frame: per pixel and channel the matched value and its two gradients.
struct GpuFlowPyramid {
    const GpuBuffer* buffer = nullptr;
    uint32_t channels = 0;
    std::vector<GpuFlowLevel> levels;
};

// One float per pixel of each level.
struct GpuFlowPlanes {
    const GpuBuffer* buffer = nullptr;
    std::vector<GpuFlowLevel> levels;
};

// u, v and confidence per pixel.
struct GpuFlowField {
    const GpuBuffer* buffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

// The matte's pyramid and the data weights of the two passes. Whether it separates anything is only known
// on the device, so flags carries the answer to the kernels that depend on it.
struct GpuFlowMatte {
    GpuFlowPlanes matte;
    GpuFlowPlanes inside;
    GpuFlowPlanes outside;
    const GpuBuffer* flags = nullptr;
};

struct GpuFlowFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t channels = 1;
    // Reads three components into the one channel matched; otherwise channels components are read as they are.
    bool luminance = false;
    bool matchingCurve = false;
    // Centre, then one to three pixels away.
    std::array<float, 4> downsampleWeights {};
};

struct GpuFlowDisParams {
    int finestLevel = 1;
    int patchSize = 8;
    int patchStride = 3;
    int searchIterations = 25;
    int refineFixedPointIterations = 5;
    int refineSorIterations = 5;
    float alpha = 20.f;
    float gamma = 10.f;
    float delta = 5.f;
    float omega = 1.6f;
};

struct GpuFlowTvL1Params {
    int finestLevel = 0;
    int warps = 5;
    int iterations = 30;
    float lambda = 0.15f;
    float theta = 0.3f;
    float tau = 0.25f;
};

struct GpuFlowTemporalNeighbour {
    GpuFlowField field;
    GpuFlowField through;
};

// Bytes of the largest single buffer the solve binds, the frame's pyramid: 0 when its indices do not fit 32
// bits.
uint64_t gpuFlowLargestBufferBytes(const GpuFlowFormat& format);

/**
 * @brief Records the stages of the classical flow solvers. Every stage's writes are ordered before the next
 * stage's reads, so the results may be used at once. A stage that fails leaves what it was given usable,
 * and a refused scratch request shows as GpuRecordContext::ranOutOfMemory().
 *
 * The contexts passed must all stay alive until the last one's work is done, as buffers move between them.
 * aborted, when given, is asked between dispatch batches.
 **/
class GpuFlow {
public:
    explicit GpuFlow(std::function<bool()> aborted = std::function<bool()>());

    GpuStatus buildPyramid(GpuRecordContext& ctx, const GpuFlowImage& image, const GpuFlowFormat& format, GpuFlowPyramid* pyramid);

    GpuStatus buildMatte(GpuRecordContext& ctx, const GpuFlowImage& image, const GpuFlowFormat& format, GpuFlowMatte* matte);

    // Null weights weigh every pixel 1.
    GpuStatus solveDis(GpuRecordContext& ctx, const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights,
                       const GpuFlowDisParams& params, GpuFlowField* flow);

    GpuStatus solveTvL1(GpuRecordContext& ctx, const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights,
                        const GpuFlowTvL1Params& params, GpuFlowField* flow);

    // Writes field's confidence from how far back along it leaves a pixel from where it began.
    GpuStatus confidence(GpuRecordContext& ctx, const GpuFlowField& back, float sigma, const GpuFlowField& field);

    GpuStatus composite(GpuRecordContext& ctx, const GpuFlowField& inside, const GpuFlowField& outside, const GpuFlowMatte& matte, GpuFlowField* out);

    // out is current itself at a strength of 0, where a neighbour counts for nothing.
    GpuStatus smoothTemporal(GpuRecordContext& ctx, const GpuFlowField& current, const std::vector<GpuFlowTemporalNeighbour>& neighbours,
                             float strength, GpuFlowField* out);

    // Writes a rect of an output plane. x0 and y0 place the rect in the solved window. vectors takes first's
    // u and v; otherwise the components are first's confidence and second's. A missing field reads as 0, as
    // does whatever lies outside the window; components past the second are 0.
    struct Output {
        const GpuBuffer* buffer = nullptr;
        uint32_t offsetFloats = 0;
        uint32_t rowStrideFloats = 0;
        uint32_t nComps = 0;
        int32_t x0 = 0;
        int32_t y0 = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t windowWidth = 0;
        uint32_t windowHeight = 0;
    };
    GpuStatus writeOutput(GpuRecordContext& ctx, const Output& output, bool vectors, const GpuFlowField& first, const GpuFlowField& second);

private:
    GpuStatus acquire(GpuRecordContext& ctx, uint64_t floats, const GpuBuffer** out);
    void release(const GpuBuffer* buffer);
    GpuStatus placeholder(GpuRecordContext& ctx, const GpuBuffer** out);
    GpuStatus fill(GpuRecordContext& ctx, const GpuBuffer* buffer, uint32_t width, uint32_t height, uint32_t pixelStride);
    GpuStatus downsample(GpuRecordContext& ctx, const GpuBuffer* buffer, const GpuFlowLevel& upper, const GpuFlowLevel& lower, uint32_t channels,
                         uint32_t pixelStride, uint32_t channelStride, const std::array<float, 4>& weights);
    GpuStatus upsample(GpuRecordContext& ctx, const GpuFlowField& coarse, uint32_t width, uint32_t height, GpuFlowField* fine);
    GpuStatus refine(GpuRecordContext& ctx, const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights, size_t level,
                     const GpuFlowDisParams& params, const GpuFlowField& flow);
    GpuStatus solveTvL1Level(GpuRecordContext& ctx, const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights, size_t level,
                             const GpuFlowTvL1Params& params, GpuFlowField* flow);
    GpuStatus checkAborted() const;

    std::function<bool()> aborted_;
    // Free for reuse: the barrier after every stage orders a later use after the earlier ones.
    std::vector<const GpuBuffer*> spare_;
    const GpuBuffer* placeholder_ = nullptr;
};

struct GpuFlowEstimateParams {
    GpuFlowFormat format;
    bool tvL1 = false;
    GpuFlowDisParams dis;
    GpuFlowTvL1Params tvL1Params;
    float confidenceSigma = 2.f;
};

/**
 * @brief What estimateFlow() and estimateFlowSmoothed() do around the solvers, solving each field the first
 * time it is asked for. Frames are slots, with the current one at kCurrent. Recording happens before any
 * result can be read back, so a frame with a matte always solves its two passes; a matte that separates
 * nothing weighs both alike and the composite keeps the outside field, which is what solving once gives.
 **/
class GpuFlowEstimate {
public:
    static constexpr int kSlots = 5;
    static constexpr int kCurrent = 2;

    GpuFlowEstimate(const GpuFlowEstimateParams& params, std::function<bool()> aborted);

    void setFrame(int slot, const GpuFlowImage& image, const GpuFlowImage* matte);

    // The flow between adjacent slots; an empty field when either frame is missing.
    GpuStatus direction(GpuRecordContext& ctx, int from, int to, bool withConfidence, GpuFlowField* out);

    // The current frame's flow to its next (forward) or previous frame, temporally smoothed.
    GpuStatus smoothed(GpuRecordContext& ctx, bool forward, float strength, GpuFlowField* out);

    GpuFlow& flow() { return flow_; }

private:
    struct Frame {
        bool present = false;
        GpuFlowImage image;
        bool hasMatte = false;
        GpuFlowImage matteImage;
        bool built = false;
        GpuFlowPyramid pyramid;
        GpuFlowMatte matte;
    };
    enum Pass {
        kPassOutside = 0,
        kPassInside = 1,
        kPassCount = 2
    };

    GpuStatus built(GpuRecordContext& ctx, int slot);
    GpuStatus raw(GpuRecordContext& ctx, int from, int to, int pass);

    GpuFlowEstimateParams params_;
    GpuFlow flow_;
    Frame frames_[kSlots];
    GpuFlowField raw_[kSlots][kSlots][kPassCount];
    bool rawConfident_[kSlots][kSlots][kPassCount] = {};
    GpuFlowField directed_[kSlots][kSlots];
    bool directedConfident_[kSlots][kSlots] = {};
    GpuFlowField smoothed_[2];
};

} // namespace gpu
