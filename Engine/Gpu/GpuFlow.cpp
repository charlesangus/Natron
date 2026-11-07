#include "GpuFlow.h"

#include "GpuBuffer.h"
#include "GpuKernel.h"
#include "GpuKernelRegistry.h"
#include "GpuRecord.h"

#include <algorithm>
#include <string>
#include <utility>

namespace gpu {

namespace {

    struct FlowPyramidPush {
        uint32_t srcOffset;
        uint32_t srcRowStride;
        uint32_t srcPixelStride;
        int32_t srcX0;
        int32_t srcY0;
        int32_t srcWidth;
        int32_t srcHeight;
        uint32_t width;
        uint32_t height;
        uint32_t dstOffset;
        uint32_t dstPixelStride;
        uint32_t channels;
        uint32_t channelStride;
        uint32_t luminance;
        uint32_t matchingCurve;
        float w0;
        float w1;
        float w2;
        float w3;
        float value;
    };
    static_assert(sizeof(FlowPyramidPush) == 80);
    static_assert(sizeof(FlowPyramidPush) <= GpuKernel::kMaxPushConstantBytes);

    struct FlowDisPush {
        uint32_t fromOffset;
        uint32_t toOffset;
        uint32_t weightOffset;
        uint32_t flowOffset;
        uint32_t patchOffset;
        uint32_t coarseOffset;
        uint32_t width;
        uint32_t height;
        uint32_t coarseWidth;
        uint32_t coarseHeight;
        uint32_t channels;
        uint32_t hasWeight;
        uint32_t patchSize;
        uint32_t patchStride;
        uint32_t iterations;
        uint32_t patchesX;
        uint32_t patchesY;
        uint32_t lastX;
        uint32_t lastY;
    };
    static_assert(sizeof(FlowDisPush) == 76);
    static_assert(sizeof(FlowDisPush) <= GpuKernel::kMaxPushConstantBytes);

    struct FlowRefinePush {
        uint32_t fromOffset;
        uint32_t toOffset;
        uint32_t weightOffset;
        uint32_t width;
        uint32_t height;
        uint32_t channels;
        uint32_t hasWeight;
        uint32_t colour;
        float halfDelta;
        float halfGamma;
        float halfAlpha;
        float omega;
    };
    static_assert(sizeof(FlowRefinePush) == 48);
    static_assert(sizeof(FlowRefinePush) <= GpuKernel::kMaxPushConstantBytes);

    struct FlowTvL1Push {
        uint32_t fromOffset;
        uint32_t toOffset;
        uint32_t weightOffset;
        uint32_t width;
        uint32_t height;
        uint32_t channels;
        uint32_t hasWeight;
        float lambdaTheta;
        float theta;
        float dualStep;
    };
    static_assert(sizeof(FlowTvL1Push) == 40);
    static_assert(sizeof(FlowTvL1Push) <= GpuKernel::kMaxPushConstantBytes);

    struct FlowConfidencePush {
        uint32_t width;
        uint32_t height;
        uint32_t neighbours;
        uint32_t mode;
        float scale;
        float strength;
        uint32_t srcOffset;
        uint32_t srcRowStride;
        uint32_t srcPixelStride;
        int32_t srcX0;
        int32_t srcY0;
        int32_t srcWidth;
        int32_t srcHeight;
        uint32_t dstOffset;
        uint32_t dstRowStride;
        uint32_t dstComps;
        int32_t outX0;
        int32_t outY0;
        uint32_t hasA;
        uint32_t hasB;
    };
    static_assert(sizeof(FlowConfidencePush) == 80);
    static_assert(sizeof(FlowConfidencePush) <= GpuKernel::kMaxPushConstantBytes);

    constexpr uint32_t kPyramidValuesPerChannel = 3;
    constexpr uint32_t kFieldStride = 3;
    constexpr uint32_t kPatchStride = 2;
    constexpr uint32_t kRefineTermsPerChannel = 8;
    constexpr uint32_t kRefineIncrementStride = 2;
    constexpr uint32_t kRefineSystemStride = 6;
    constexpr uint32_t kTvL1TermsPerChannel = 4;
    constexpr uint32_t kTvL1SolutionStride = 2;
    constexpr uint32_t kTvL1DualStride = 4;
    constexpr int kTvL1AbortCheckIterations = 10;
    constexpr uint32_t kWeightInside = 0;
    constexpr uint32_t kWeightOutside = 1;
    constexpr uint32_t kOutputVectors = 0;
    constexpr uint32_t kOutputConfidence = 1;
    constexpr uint64_t kPlaceholderFloats = 4;

    GpuStatus invalid(const std::string& what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, "optical flow: " + what };
    }

    // The levels of FlowPyramid over format, packed at pixelStride floats a pixel; empty when they do not
    // fit the kernels' 32-bit indices.
    std::vector<GpuFlowLevel> layoutLevels(const GpuFlowFormat& format, uint32_t pixelStride, uint64_t* totalFloats)
    {
        std::vector<GpuFlowLevel> levels;
        uint32_t w = format.width;
        uint32_t h = format.height;
        uint64_t offset = 0;
        if ((w == 0) || (h == 0)) {
            return levels;
        }
        for (;;) {
            levels.push_back({ w, h, static_cast<uint32_t>(offset) });
            offset += uint64_t(w) * h * pixelStride;
            if (offset > UINT32_MAX) {
                levels.clear();
                return levels;
            }
            if ((levels.size() >= format.depth) || ((w <= 1) && (h <= 1))) {
                break;
            }
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        *totalFloats = offset;
        return levels;
    }

    template <typename Params>
    GpuStatus run(GpuRecordContext& ctx, GpuKernelId kernel, std::initializer_list<const GpuBuffer*> buffers, const Params& params, uint32_t x, uint32_t y)
    {
        GpuStatus status = ctx.dispatch(kernel, buffers, params, { x, y, 1u });
        if (status) {
            ctx.computeBarrier();
        }
        return status;
    }

    void setWeights(FlowPyramidPush& push, const std::array<float, 4>& weights)
    {
        push.w0 = weights[0];
        push.w1 = weights[1];
        push.w2 = weights[2];
        push.w3 = weights[3];
    }

    uint32_t weightOffsetAt(const GpuFlowPlanes* weights, size_t level)
    {
        return weights ? weights->levels[level].offsetFloats : 0u;
    }

    bool sameShape(const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights)
    {
        if (from.levels.empty() || (from.channels != to.channels) || (from.levels.size() != to.levels.size())) {
            return false;
        }
        for (size_t l = 0; l < from.levels.size(); ++l) {
            if ((from.levels[l].width != to.levels[l].width) || (from.levels[l].height != to.levels[l].height)) {
                return false;
            }
        }
        return !weights || (weights->levels.size() == from.levels.size());
    }

} // namespace

uint64_t
gpuFlowLargestBufferBytes(const GpuFlowFormat& format)
{
    uint64_t pyramidFloats = 0;
    const std::vector<GpuFlowLevel> levels = layoutLevels(format, format.channels * kPyramidValuesPerChannel, &pyramidFloats);
    const uint64_t termsFloats = uint64_t(format.width) * format.height * format.channels * kRefineTermsPerChannel;
    if (levels.empty() || (termsFloats > UINT32_MAX)) {
        return 0;
    }
    return std::max(pyramidFloats, termsFloats) * sizeof(float);
}

GpuFlow::GpuFlow(std::function<bool()> aborted)
    : aborted_(std::move(aborted))
{
}

GpuStatus
GpuFlow::checkAborted() const
{
    if (aborted_ && aborted_()) {
        return { VK_INCOMPLETE, "optical flow: aborted" };
    }
    return {};
}

GpuStatus
GpuFlow::acquire(GpuRecordContext& ctx, uint64_t floats, const GpuBuffer** out)
{
    const VkDeviceSize bytes = floats * sizeof(float);
    auto best = spare_.end();
    for (auto it = spare_.begin(); it != spare_.end(); ++it) {
        if (((*it)->size() >= bytes) && ((best == spare_.end()) || ((*it)->size() < (*best)->size()))) {
            best = it;
        }
    }
    if (best != spare_.end()) {
        *out = *best;
        spare_.erase(best);
        return {};
    }
    GpuBuffer* buffer = nullptr;
    GpuStatus status = ctx.scratch(bytes, buffer);
    if (status) {
        *out = buffer;
    }
    return status;
}

void
GpuFlow::release(const GpuBuffer* buffer)
{
    spare_.push_back(buffer);
}

GpuStatus
GpuFlow::placeholder(GpuRecordContext& ctx, const GpuBuffer** out)
{
    if (!placeholder_) {
        GpuBuffer* buffer = nullptr;
        GpuStatus status = ctx.scratch(kPlaceholderFloats * sizeof(float), buffer);
        if (!status) {
            return status;
        }
        placeholder_ = buffer;
    }
    *out = placeholder_;
    return {};
}

GpuStatus
GpuFlow::fill(GpuRecordContext& ctx, const GpuBuffer* buffer, uint32_t width, uint32_t height, uint32_t pixelStride)
{
    const GpuBuffer* unused = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (!status) {
        return status;
    }
    FlowPyramidPush push = {};
    push.width = width;
    push.height = height;
    push.dstPixelStride = pixelStride;
    return run(ctx, GpuKernelId::flow_fill, { unused, buffer }, push, width, height);
}

GpuStatus
GpuFlow::downsample(GpuRecordContext& ctx, const GpuBuffer* buffer, const GpuFlowLevel& upper, const GpuFlowLevel& lower, uint32_t channels,
                    uint32_t pixelStride, uint32_t channelStride, const std::array<float, 4>& weights)
{
    const GpuBuffer* rows = nullptr;
    GpuStatus status = acquire(ctx, uint64_t(upper.width) * upper.height * channels, &rows);
    if (!status) {
        return status;
    }
    FlowPyramidPush push = {};
    setWeights(push, weights);
    push.channels = channels;
    push.channelStride = channelStride;
    push.srcOffset = upper.offsetFloats;
    push.srcRowStride = upper.width * pixelStride;
    push.srcPixelStride = pixelStride;
    push.width = upper.width;
    push.height = upper.height;
    status = run(ctx, GpuKernelId::flow_blur_rows, { buffer, rows }, push, upper.width, upper.height);
    if (!status) {
        return status;
    }
    push.srcOffset = 0;
    push.srcWidth = static_cast<int32_t>(upper.width);
    push.srcHeight = static_cast<int32_t>(upper.height);
    push.width = lower.width;
    push.height = lower.height;
    push.dstOffset = lower.offsetFloats;
    push.dstPixelStride = pixelStride;
    status = run(ctx, GpuKernelId::flow_blur_columns, { rows, buffer }, push, lower.width, lower.height);
    release(rows);
    return status;
}

GpuStatus
GpuFlow::buildPyramid(GpuRecordContext& ctx, const GpuFlowImage& image, const GpuFlowFormat& format, GpuFlowPyramid* pyramid)
{
    if (!image.buffer || (format.channels == 0) || (format.luminance && (format.channels != 1))) {
        return invalid("a pyramid needs a source and a channel count that matches it");
    }
    const uint32_t stride = format.channels * kPyramidValuesPerChannel;
    uint64_t total = 0;
    std::vector<GpuFlowLevel> levels = layoutLevels(format, stride, &total);
    if (levels.empty()) {
        return invalid("the pyramid does not fit 32-bit indices");
    }
    const GpuBuffer* buffer = nullptr;
    const GpuBuffer* unused = nullptr;
    GpuStatus status = acquire(ctx, total, &buffer);
    if (status) {
        status = placeholder(ctx, &unused);
    }
    if (!status) {
        return status;
    }

    FlowPyramidPush extract = {};
    extract.srcOffset = image.offsetFloats;
    extract.srcRowStride = image.rowStrideFloats;
    extract.srcPixelStride = image.pixelStride;
    extract.srcX0 = image.x0;
    extract.srcY0 = image.y0;
    extract.srcWidth = image.width;
    extract.srcHeight = image.height;
    extract.width = format.width;
    extract.height = format.height;
    extract.dstPixelStride = stride;
    extract.channels = format.channels;
    extract.channelStride = kPyramidValuesPerChannel;
    extract.luminance = format.luminance ? 1u : 0u;
    extract.matchingCurve = format.matchingCurve ? 1u : 0u;
    status = run(ctx, GpuKernelId::flow_extract, { image.buffer, buffer }, extract, format.width, format.height);
    for (size_t l = 0; status && (l + 1 < levels.size()); ++l) {
        status = checkAborted();
        if (status) {
            status = downsample(ctx, buffer, levels[l], levels[l + 1], format.channels, stride, kPyramidValuesPerChannel, format.downsampleWeights);
        }
    }
    for (size_t l = 0; status && (l < levels.size()); ++l) {
        FlowPyramidPush push = {};
        push.width = levels[l].width;
        push.height = levels[l].height;
        push.dstOffset = levels[l].offsetFloats;
        push.dstPixelStride = stride;
        push.channels = format.channels;
        push.channelStride = kPyramidValuesPerChannel;
        status = run(ctx, GpuKernelId::flow_gradients, { unused, buffer }, push, levels[l].width, levels[l].height);
    }
    if (status) {
        pyramid->buffer = buffer;
        pyramid->channels = format.channels;
        pyramid->levels = std::move(levels);
    }
    return status;
}

GpuStatus
GpuFlow::buildMatte(GpuRecordContext& ctx, const GpuFlowImage& image, const GpuFlowFormat& format, GpuFlowMatte* matte)
{
    if (!image.buffer) {
        return invalid("a matte needs a source");
    }
    uint64_t total = 0;
    std::vector<GpuFlowLevel> levels = layoutLevels(format, 1, &total);
    if (levels.empty()) {
        return invalid("the matte does not fit 32-bit indices");
    }
    const GpuBuffer* unused = nullptr;
    const GpuBuffer* base = nullptr;
    const GpuBuffer* inside = nullptr;
    const GpuBuffer* outside = nullptr;
    const GpuBuffer* flags = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (status) {
        status = acquire(ctx, total, &base);
    }
    if (status) {
        status = acquire(ctx, total, &inside);
    }
    if (status) {
        status = acquire(ctx, total, &outside);
    }
    if (status) {
        status = acquire(ctx, 1, &flags);
    }
    if (status) {
        status = fill(ctx, flags, 1, 1, 1);
    }
    if (!status) {
        return status;
    }

    FlowConfidencePush extract = {};
    extract.width = format.width;
    extract.height = format.height;
    extract.srcOffset = image.offsetFloats;
    extract.srcRowStride = image.rowStrideFloats;
    extract.srcPixelStride = image.pixelStride;
    extract.srcX0 = image.x0;
    extract.srcY0 = image.y0;
    extract.srcWidth = image.width;
    extract.srcHeight = image.height;
    status = run(ctx, GpuKernelId::flow_matte_extract, { image.buffer, unused, unused, unused, unused, base, flags }, extract, format.width, format.height);
    for (size_t l = 0; status && (l + 1 < levels.size()); ++l) {
        status = downsample(ctx, base, levels[l], levels[l + 1], 1, 1, 1, format.downsampleWeights);
    }
    for (size_t l = 0; status && (l < levels.size()); ++l) {
        FlowConfidencePush push = {};
        push.width = levels[l].width;
        push.height = levels[l].height;
        push.srcOffset = levels[l].offsetFloats;
        push.dstOffset = levels[l].offsetFloats;
        push.mode = kWeightInside;
        status = run(ctx, GpuKernelId::flow_matte_weight, { base, unused, unused, unused, unused, inside, flags }, push, push.width, push.height);
        if (status) {
            push.mode = kWeightOutside;
            status = run(ctx, GpuKernelId::flow_matte_weight, { base, unused, unused, unused, unused, outside, flags }, push, push.width, push.height);
        }
    }
    if (status) {
        matte->matte = { base, levels };
        matte->inside = { inside, levels };
        matte->outside = { outside, levels };
        matte->flags = flags;
    }
    return status;
}

GpuStatus
GpuFlow::upsample(GpuRecordContext& ctx, const GpuFlowField& coarse, uint32_t width, uint32_t height, GpuFlowField* fine)
{
    const GpuBuffer* unused = nullptr;
    const GpuBuffer* buffer = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (status) {
        status = acquire(ctx, uint64_t(width) * height * kFieldStride, &buffer);
    }
    if (!status) {
        return status;
    }
    FlowDisPush push = {};
    push.width = width;
    push.height = height;
    push.coarseWidth = coarse.width;
    push.coarseHeight = coarse.height;
    status = run(ctx, GpuKernelId::flow_upsample, { unused, unused, unused, buffer, unused, coarse.buffer }, push, width, height);
    if (status) {
        *fine = { buffer, width, height };
    }
    return status;
}

GpuStatus
GpuFlow::refine(GpuRecordContext& ctx, const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights, size_t level,
                const GpuFlowDisParams& params, const GpuFlowField& flow)
{
    const uint32_t width = flow.width;
    const uint32_t height = flow.height;
    const uint64_t pixels = uint64_t(width) * height;
    const GpuBuffer* unused = nullptr;
    const GpuBuffer* terms = nullptr;
    const GpuBuffer* increment = nullptr;
    const GpuBuffer* system = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (status) {
        status = acquire(ctx, pixels * from.channels * kRefineTermsPerChannel, &terms);
    }
    if (status) {
        status = acquire(ctx, pixels * kRefineIncrementStride, &increment);
    }
    if (status) {
        status = acquire(ctx, pixels * kRefineSystemStride, &system);
    }
    if (status) {
        status = fill(ctx, increment, width, height, kRefineIncrementStride);
    }
    if (!status) {
        return status;
    }

    FlowRefinePush push = {};
    push.fromOffset = from.levels[level].offsetFloats;
    push.toOffset = to.levels[level].offsetFloats;
    push.weightOffset = weightOffsetAt(weights, level);
    push.width = width;
    push.height = height;
    push.channels = from.channels;
    push.hasWeight = weights ? 1u : 0u;
    push.halfDelta = 0.5f * params.delta / static_cast<float>(from.channels);
    push.halfGamma = 0.5f * params.gamma / static_cast<float>(from.channels);
    push.halfAlpha = 0.5f * params.alpha;
    push.omega = params.omega;
    const std::initializer_list<const GpuBuffer*> buffers = { from.buffer, to.buffer, weights ? weights->buffer : unused, flow.buffer, terms, increment, system };

    status = run(ctx, GpuKernelId::flow_refine_linearise, buffers, push, width, height);
    if (status) {
        status = run(ctx, GpuKernelId::flow_refine_derivatives, buffers, push, width, height);
    }
    for (int fixedPoint = 0; status && (fixedPoint < params.refineFixedPointIterations); ++fixedPoint) {
        status = checkAborted();
        if (status) {
            status = run(ctx, GpuKernelId::flow_refine_assemble, buffers, push, width, height);
        }
        for (int sor = 0; status && (sor < params.refineSorIterations); ++sor) {
            push.colour = 0;
            status = run(ctx, GpuKernelId::flow_refine_sor, buffers, push, (width + 1) / 2, height);
            if (status) {
                push.colour = 1;
                status = run(ctx, GpuKernelId::flow_refine_sor, buffers, push, (width + 1) / 2, height);
            }
        }
    }
    if (status) {
        status = run(ctx, GpuKernelId::flow_refine_apply, buffers, push, width, height);
    }
    release(terms);
    release(increment);
    release(system);
    return status;
}

GpuStatus
GpuFlow::solveDis(GpuRecordContext& ctx, const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights,
                  const GpuFlowDisParams& params, GpuFlowField* flow)
{
    if (!sameShape(from, to, weights)) {
        return invalid("the frames' pyramids differ");
    }
    const int coarsest = static_cast<int>(from.levels.size()) - 1;
    const int finest = std::min(params.finestLevel, coarsest);
    const GpuBuffer* unused = nullptr;
    GpuFlowField field { nullptr, from.levels[coarsest].width, from.levels[coarsest].height };
    const GpuBuffer* start = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (status) {
        status = acquire(ctx, uint64_t(field.width) * field.height * kFieldStride, &start);
    }
    if (status) {
        field.buffer = start;
        status = fill(ctx, field.buffer, field.width, field.height, kFieldStride);
    }
    for (int l = coarsest; status && (l >= finest); --l) {
        status = checkAborted();
        const GpuFlowLevel& source = from.levels[l];
        if (status && (l != coarsest)) {
            GpuFlowField fine;
            status = upsample(ctx, field, source.width, source.height, &fine);
            if (status) {
                release(field.buffer);
                field = fine;
            }
        }
        if (!status) {
            break;
        }
        const uint32_t patchSize = std::min({ static_cast<uint32_t>(std::max(params.patchSize, 1)), source.width, source.height });
        const uint32_t stride = static_cast<uint32_t>(std::max(params.patchStride, 1));
        const uint32_t lastX = source.width - patchSize;
        const uint32_t lastY = source.height - patchSize;
        FlowDisPush push = {};
        push.fromOffset = source.offsetFloats;
        push.toOffset = to.levels[l].offsetFloats;
        push.weightOffset = weightOffsetAt(weights, static_cast<size_t>(l));
        push.width = source.width;
        push.height = source.height;
        push.channels = from.channels;
        push.hasWeight = weights ? 1u : 0u;
        push.patchSize = patchSize;
        push.patchStride = stride;
        push.iterations = static_cast<uint32_t>(params.searchIterations);
        push.patchesX = (lastX + stride - 1) / stride + 1;
        push.patchesY = (lastY + stride - 1) / stride + 1;
        push.lastX = lastX;
        push.lastY = lastY;
        const GpuBuffer* patches = nullptr;
        status = acquire(ctx, uint64_t(push.patchesX) * push.patchesY * kPatchStride, &patches);
        if (!status) {
            break;
        }
        const std::initializer_list<const GpuBuffer*> buffers = { from.buffer, to.buffer, weights ? weights->buffer : unused, field.buffer, patches, unused };
        status = run(ctx, GpuKernelId::flow_dis_search, buffers, push, push.patchesX, push.patchesY);
        if (status) {
            status = checkAborted();
        }
        if (status) {
            status = run(ctx, GpuKernelId::flow_dis_densify, buffers, push, source.width, source.height);
        }
        release(patches);
        if (status && (params.refineFixedPointIterations > 0)) {
            status = refine(ctx, from, to, weights, static_cast<size_t>(l), params, field);
        }
    }
    for (int l = finest - 1; status && (l >= 0); --l) {
        GpuFlowField fine;
        status = upsample(ctx, field, from.levels[l].width, from.levels[l].height, &fine);
        if (status) {
            release(field.buffer);
            field = fine;
        }
    }
    if (status) {
        *flow = field;
    }
    return status;
}

GpuStatus
GpuFlow::solveTvL1Level(GpuRecordContext& ctx, const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights, size_t level,
                        const GpuFlowTvL1Params& params, GpuFlowField* flow)
{
    const uint32_t width = from.levels[level].width;
    const uint32_t height = from.levels[level].height;
    const uint64_t pixels = uint64_t(width) * height;
    const GpuBuffer* unused = nullptr;
    const GpuBuffer* terms = nullptr;
    const GpuBuffer* solution = nullptr;
    const GpuBuffer* dual = nullptr;
    const GpuBuffer* filtered = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (status) {
        status = acquire(ctx, pixels * from.channels * kTvL1TermsPerChannel, &terms);
    }
    if (status) {
        status = acquire(ctx, pixels * kTvL1SolutionStride, &solution);
    }
    if (status) {
        status = acquire(ctx, pixels * kTvL1DualStride, &dual);
    }
    if (status) {
        status = acquire(ctx, pixels * kFieldStride, &filtered);
    }
    if (status) {
        status = fill(ctx, dual, width, height, kTvL1DualStride);
    }
    if (!status) {
        return status;
    }

    FlowTvL1Push push = {};
    push.fromOffset = from.levels[level].offsetFloats;
    push.toOffset = to.levels[level].offsetFloats;
    push.weightOffset = weightOffsetAt(weights, level);
    push.width = width;
    push.height = height;
    push.channels = from.channels;
    push.hasWeight = weights ? 1u : 0u;
    push.lambdaTheta = params.lambda * params.theta / static_cast<float>(from.channels);
    push.theta = params.theta;
    push.dualStep = params.tau / params.theta;
    const GpuBuffer* weightBuffer = weights ? weights->buffer : unused;

    for (int warp = 0; status && (warp < params.warps); ++warp) {
        const std::initializer_list<const GpuBuffer*> buffers = { from.buffer, to.buffer, weightBuffer, flow->buffer, terms, solution, dual, filtered };
        status = run(ctx, GpuKernelId::flow_tvl1_linearise, buffers, push, width, height);
        for (int iteration = 0; status && (iteration < params.iterations); ++iteration) {
            if ((iteration % kTvL1AbortCheckIterations) == 0) {
                status = checkAborted();
            }
            if (status) {
                status = run(ctx, GpuKernelId::flow_tvl1_threshold, buffers, push, width, height);
            }
            if (status) {
                status = run(ctx, GpuKernelId::flow_tvl1_primal, buffers, push, width, height);
            }
            if (status) {
                status = run(ctx, GpuKernelId::flow_tvl1_dual, buffers, push, width, height);
            }
        }
        if (status) {
            status = run(ctx, GpuKernelId::flow_tvl1_median, buffers, push, width, height);
        }
        if (status) {
            std::swap(flow->buffer, filtered);
        }
    }
    release(terms);
    release(solution);
    release(dual);
    release(filtered);
    return status;
}

GpuStatus
GpuFlow::solveTvL1(GpuRecordContext& ctx, const GpuFlowPyramid& from, const GpuFlowPyramid& to, const GpuFlowPlanes* weights,
                   const GpuFlowTvL1Params& params, GpuFlowField* flow)
{
    if (!sameShape(from, to, weights)) {
        return invalid("the frames' pyramids differ");
    }
    const int coarsest = static_cast<int>(from.levels.size()) - 1;
    const int finest = std::min(params.finestLevel, coarsest);
    GpuFlowField field { nullptr, from.levels[coarsest].width, from.levels[coarsest].height };
    const GpuBuffer* start = nullptr;
    GpuStatus status = acquire(ctx, uint64_t(field.width) * field.height * kFieldStride, &start);
    if (status) {
        field.buffer = start;
        status = fill(ctx, field.buffer, field.width, field.height, kFieldStride);
    }
    for (int l = coarsest; status && (l >= finest); --l) {
        status = checkAborted();
        if (status && (l != coarsest)) {
            GpuFlowField fine;
            status = upsample(ctx, field, from.levels[l].width, from.levels[l].height, &fine);
            if (status) {
                release(field.buffer);
                field = fine;
            }
        }
        if (status) {
            status = solveTvL1Level(ctx, from, to, weights, static_cast<size_t>(l), params, &field);
        }
    }
    for (int l = finest - 1; status && (l >= 0); --l) {
        GpuFlowField fine;
        status = upsample(ctx, field, from.levels[l].width, from.levels[l].height, &fine);
        if (status) {
            release(field.buffer);
            field = fine;
        }
    }
    if (status) {
        *flow = field;
    }
    return status;
}

GpuStatus
GpuFlow::confidence(GpuRecordContext& ctx, const GpuFlowField& back, float sigma, const GpuFlowField& field)
{
    const GpuBuffer* unused = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (!status) {
        return status;
    }
    FlowConfidencePush push = {};
    push.width = field.width;
    push.height = field.height;
    push.scale = 1.f / (sigma * sigma);
    return run(ctx, GpuKernelId::flow_confidence, { back.buffer, unused, unused, unused, unused, field.buffer, unused }, push, field.width, field.height);
}

GpuStatus
GpuFlow::composite(GpuRecordContext& ctx, const GpuFlowField& inside, const GpuFlowField& outside, const GpuFlowMatte& matte, GpuFlowField* out)
{
    const GpuBuffer* unused = nullptr;
    const GpuBuffer* buffer = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (status) {
        status = acquire(ctx, uint64_t(outside.width) * outside.height * kFieldStride, &buffer);
    }
    if (!status) {
        return status;
    }
    FlowConfidencePush push = {};
    push.width = outside.width;
    push.height = outside.height;
    status = run(ctx, GpuKernelId::flow_composite, { inside.buffer, outside.buffer, matte.matte.buffer, unused, unused, buffer, matte.flags }, push, push.width, push.height);
    if (status) {
        *out = { buffer, outside.width, outside.height };
    }
    return status;
}

GpuStatus
GpuFlow::smoothTemporal(GpuRecordContext& ctx, const GpuFlowField& current, const std::vector<GpuFlowTemporalNeighbour>& neighbours,
                        float strength, GpuFlowField* out)
{
    if (neighbours.size() > 2) {
        return invalid("temporal smoothing takes at most two neighbours");
    }
    if (strength <= 0.f) {
        *out = current;
        return {};
    }
    const GpuBuffer* unused = nullptr;
    const GpuBuffer* buffer = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (status) {
        status = acquire(ctx, uint64_t(current.width) * current.height * kFieldStride, &buffer);
    }
    if (!status) {
        return status;
    }
    FlowConfidencePush push = {};
    push.width = current.width;
    push.height = current.height;
    push.neighbours = static_cast<uint32_t>(neighbours.size());
    push.strength = strength;
    const GpuBuffer* firstField = (neighbours.size() > 0) ? neighbours[0].field.buffer : unused;
    const GpuBuffer* firstThrough = (neighbours.size() > 0) ? neighbours[0].through.buffer : unused;
    const GpuBuffer* secondField = (neighbours.size() > 1) ? neighbours[1].field.buffer : unused;
    const GpuBuffer* secondThrough = (neighbours.size() > 1) ? neighbours[1].through.buffer : unused;
    status = run(ctx, GpuKernelId::flow_temporal, { firstField, firstThrough, secondField, secondThrough, current.buffer, buffer, unused }, push, push.width, push.height);
    if (status) {
        *out = { buffer, current.width, current.height };
    }
    return status;
}

GpuStatus
GpuFlow::writeOutput(GpuRecordContext& ctx, const Output& output, bool vectors, const GpuFlowField& first, const GpuFlowField& second)
{
    const GpuBuffer* unused = nullptr;
    GpuStatus status = placeholder(ctx, &unused);
    if (!status) {
        return status;
    }
    FlowConfidencePush push = {};
    push.width = output.windowWidth;
    push.height = output.windowHeight;
    push.mode = vectors ? kOutputVectors : kOutputConfidence;
    push.srcWidth = static_cast<int32_t>(output.width);
    push.srcHeight = static_cast<int32_t>(output.height);
    push.dstOffset = output.offsetFloats;
    push.dstRowStride = output.rowStrideFloats;
    push.dstComps = output.nComps;
    push.outX0 = output.x0;
    push.outY0 = output.y0;
    push.hasA = first.buffer ? 1u : 0u;
    push.hasB = second.buffer ? 1u : 0u;
    return run(ctx, GpuKernelId::flow_output, { first.buffer ? first.buffer : unused, second.buffer ? second.buffer : unused, unused, unused, unused, output.buffer, unused },
               push, output.width, output.height);
}

GpuFlowEstimate::GpuFlowEstimate(const GpuFlowEstimateParams& params, std::function<bool()> aborted)
    : params_(params)
    , flow_(std::move(aborted))
{
}

void
GpuFlowEstimate::setFrame(int slot, const GpuFlowImage& image, const GpuFlowImage* matte)
{
    Frame& frame = frames_[slot];
    frame.present = true;
    frame.image = image;
    frame.hasMatte = (matte != nullptr);
    if (matte) {
        frame.matteImage = *matte;
    }
}

GpuStatus
GpuFlowEstimate::built(GpuRecordContext& ctx, int slot)
{
    Frame& frame = frames_[slot];
    if (frame.built) {
        return {};
    }
    GpuStatus status = flow_.buildPyramid(ctx, frame.image, params_.format, &frame.pyramid);
    if (status && frame.hasMatte) {
        status = flow_.buildMatte(ctx, frame.matteImage, params_.format, &frame.matte);
    }
    frame.built = bool(status);
    return status;
}

GpuStatus
GpuFlowEstimate::raw(GpuRecordContext& ctx, int from, int to, int pass)
{
    if (raw_[from][to][pass].buffer) {
        return {};
    }
    GpuStatus status = built(ctx, from);
    if (status) {
        status = built(ctx, to);
    }
    if (!status) {
        return status;
    }
    const Frame& source = frames_[from];
    const GpuFlowPlanes* weights = nullptr;
    if (source.hasMatte) {
        weights = (pass == kPassInside) ? &source.matte.inside : &source.matte.outside;
    }
    if (params_.tvL1) {
        return flow_.solveTvL1(ctx, source.pyramid, frames_[to].pyramid, weights, params_.tvL1Params, &raw_[from][to][pass]);
    }
    return flow_.solveDis(ctx, source.pyramid, frames_[to].pyramid, weights, params_.dis, &raw_[from][to][pass]);
}

GpuStatus
GpuFlowEstimate::direction(GpuRecordContext& ctx, int from, int to, bool withConfidence, GpuFlowField* out)
{
    *out = GpuFlowField();
    if (!frames_[from].present || !frames_[to].present) {
        return {};
    }
    if (directed_[from][to].buffer && (directedConfident_[from][to] || !withConfidence)) {
        *out = directed_[from][to];
        return {};
    }
    const int passes = (frames_[from].hasMatte || frames_[to].hasMatte) ? int(kPassCount) : 1;
    for (int pass = 0; pass < passes; ++pass) {
        GpuStatus status = raw(ctx, from, to, pass);
        if (!status) {
            return status;
        }
        if (withConfidence && !rawConfident_[from][to][pass]) {
            status = raw(ctx, to, from, pass);
            if (status) {
                status = flow_.confidence(ctx, raw_[to][from][pass], params_.confidenceSigma, raw_[from][to][pass]);
            }
            if (!status) {
                return status;
            }
            rawConfident_[from][to][pass] = true;
        }
    }
    GpuFlowField result = raw_[from][to][kPassOutside];
    if (frames_[from].hasMatte) {
        GpuStatus status = flow_.composite(ctx, raw_[from][to][kPassInside], raw_[from][to][kPassOutside], frames_[from].matte, &result);
        if (!status) {
            return status;
        }
    }
    directed_[from][to] = result;
    directedConfident_[from][to] = withConfidence;
    *out = result;
    return {};
}

GpuStatus
GpuFlowEstimate::smoothed(GpuRecordContext& ctx, bool forward, float strength, GpuFlowField* out)
{
    GpuFlowField& cached = smoothed_[forward ? 0 : 1];
    if (cached.buffer) {
        *out = cached;
        return {};
    }
    *out = GpuFlowField();
    const int ahead = forward ? kCurrent + 1 : kCurrent - 1;
    const int beyond = forward ? kCurrent + 2 : kCurrent - 2;
    const int opposite = forward ? kCurrent - 1 : kCurrent + 1;
    if (!frames_[ahead].present) {
        return {};
    }
    GpuFlowField current;
    GpuStatus status = direction(ctx, kCurrent, ahead, true, &current);
    if (!status) {
        return status;
    }
    std::vector<GpuFlowTemporalNeighbour> neighbours;
    if (frames_[beyond].present) {
        GpuFlowTemporalNeighbour neighbour;
        neighbour.through = current;
        status = direction(ctx, ahead, beyond, true, &neighbour.field);
        if (!status) {
            return status;
        }
        neighbours.push_back(neighbour);
    }
    if (frames_[opposite].present) {
        GpuFlowTemporalNeighbour neighbour;
        status = direction(ctx, opposite, kCurrent, true, &neighbour.field);
        if (status) {
            status = direction(ctx, kCurrent, opposite, true, &neighbour.through);
        }
        if (!status) {
            return status;
        }
        neighbours.push_back(neighbour);
    }
    status = flow_.smoothTemporal(ctx, current, neighbours, strength, &cached);
    if (status) {
        *out = cached;
    }
    return status;
}

} // namespace gpu
