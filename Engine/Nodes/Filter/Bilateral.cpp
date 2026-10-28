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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Bilateral.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
constexpr InputSpec kBilateralInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "Guide", .role = InputRole::Aux, .optional = true },
};

constexpr NodeSpec kBilateralNodeSpec = {
    .id = PLUGINID_NATRON_BILATERAL,
    .label = "Bilateral",
    .description = "Smooth the input while keeping its edges: each pixel is averaged with its neighbours, weighted by their distance and by how far their luminance is from its own.\n"
                   "\n"
                   "Luminance differences are measured in stops (log2), so Range Sigma treats 0.1 against 0.2 the same as 10 against 20, and scaling the input scales the output by the same factor. "
                   "The luminance is Rec.709 when R, G and B are all processed, else the mean of the processed channels; values below 2^-16, zero and negatives included, count as 2^-16.\n"
                   "\n"
                   "Bilateral mode weights each neighbour by exp(-d^2 / 2 size^2) * exp(-stops^2 / 2 rangeSigma^2), out to 3 * size pixels. "
                   "Guided mode is He et al.'s guided filter: a local linear fit of the image to the log-luminance over box means, with a regularisation of rangeSigma^2 stops^2. It costs the same at any size.\n"
                   "\n"
                   "With the Guide connected, the luminance compared is the Guide's (a joint, or cross, filter); without it, the image's own. Iterations filter the result again, with the Guide's luminance or the new result's.\n"
                   "\n"
                   "Beyond the edges of the image the window holds only the pixels inside. NaNs and infinities count as 0.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_BILATERAL,
    .icon = NATRON_IMAGES_PATH "NativeNodes/Bilateral.png",
    .inputs = kBilateralInputs,
};

constexpr ImagePipelineSpec kBilateralPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, false),
    .op = OpKind::Passes,
};

const int kSlotSource = 0;
const int kSlotKey = 1;
const int kSlotImageA = 2;
const int kSlotImageB = 3;
// Guided mode: the row means of the key, its square, the image and key * image, then a and b.
const int kSlotRowKey = 4;
const int kSlotRowKey2 = 5;
const int kSlotRowImage = 6;
const int kSlotRowProduct = 7;
const int kSlotA = 8;
const int kSlotB = 9;
const int kGuidedSlots = 10;

const int kMaxIterations = 10;
const double kMaxSize = 50.;

std::vector<float>
planeWeights(const PassScratch& scratch)
{
    std::bitset<4> bits;
    for (int bit : scratch.channelBits) {
        if ((bit >= 0) && (bit < 4)) {
            bits.set(bit);
        }
    }
    std::vector<float> weights((std::size_t)scratch.nPlanes, 0.f);
    for (int p = 0; p < scratch.nPlanes; ++p) {
        weights[p] = Bilateral::luminanceWeight(bits, scratch.channelBits[p]);
    }

    return weights;
}

std::vector<const float*>
constPlanes(const PassScratch& scratch,
            int slot)
{
    std::vector<const float*> planes((std::size_t)scratch.nPlanes);
    for (int p = 0; p < scratch.nPlanes; ++p) {
        planes[p] = scratch.plane(slot, p);
    }

    return planes;
}

std::vector<float*>
planes(const PassScratch& scratch,
       int slot)
{
    std::vector<float*> out((std::size_t)scratch.nPlanes);
    for (int p = 0; p < scratch.nPlanes; ++p) {
        out[p] = scratch.plane(slot, p);
    }

    return out;
}

// The key of every pixel from the image in slot `from`.
std::function<PassRowFn(const PassScratch&)>
imageKeyWorker(int from)
{
    return [from](const PassScratch& scratch) -> PassRowFn {
        const std::vector<float> weights = planeWeights(scratch);
        const std::vector<const float*> image = constPlanes(scratch, from);
        float* const key = scratch.plane(kSlotKey, 0);
        const int width = scratch.width;
        const int nPlanes = scratch.nPlanes;

        return [weights, image, key, width, nPlanes](int row) {
            const std::size_t o = (std::size_t)row * (std::size_t)width;
            for (int x = 0; x < width; ++x) {
                float l = 0.f;
                for (int p = 0; p < nPlanes; ++p) {
                    if (weights[p] != 0.f) {
                        l += weights[p] * Bilateral::finiteSample(image[p][o + x]);
                    }
                }
                key[o + x] = Bilateral::luminanceKey(l);
            }
        };
    };
}

// The key of every pixel from the Guide, 0 where it has no pixel.
std::function<PassRowFn(const PassScratch&)>
guideKeyWorker(const std::shared_ptr<const BilateralGuide>& guide)
{
    return [guide](const PassScratch& scratch) -> PassRowFn {
        const std::vector<float> weights = planeWeights(scratch);
        std::vector<int> comps((std::size_t)scratch.nPlanes, -1);
        if (guide) {
            for (int p = 0; p < scratch.nPlanes; ++p) {
                comps[p] = PassExec::channelIndexForBit(guide->nComps, scratch.channelBits[p]);
            }
        }
        float* const key = scratch.plane(kSlotKey, 0);
        const int width = scratch.width;
        const RectI rect = scratch.rect;
        const int nPlanes = scratch.nPlanes;

        return [guide, weights, comps, key, width, rect, nPlanes](int row) {
            float* out = key + (std::size_t)row * (std::size_t)width;
            const int y = rect.y1 + row;
            const bool rowInside = guide && (y >= guide->bounds.y1) && (y < guide->bounds.y2);
            for (int x = 0; x < width; ++x) {
                const int gx = rect.x1 + x;
                float l = 0.f;
                if (rowInside && (gx >= guide->bounds.x1) && (gx < guide->bounds.x2)) {
                    const float* px = guide->pixels.data() + ((std::size_t)(y - guide->bounds.y1) * (std::size_t)guide->bounds.width() + (std::size_t)(gx - guide->bounds.x1)) * (std::size_t)guide->nComps;
                    for (int p = 0; p < nPlanes; ++p) {
                        if ((weights[p] != 0.f) && (comps[p] >= 0)) {
                            l += weights[p] * Bilateral::finiteSample(px[comps[p]]);
                        }
                    }
                }
                out[x] = Bilateral::luminanceKey(l);
            }
        };
    };
}

// One Bilateral iteration from slot `from` into slot `to`: taps within the disc of the radius
// and inside the buffer, in rows from the top and left to right, as the GPU sums them.
std::function<PassRowFn(const PassScratch&)>
bilateralWorker(int radius,
                float invTwoSigmaS2,
                float invTwoSigmaR2,
                int from,
                int to)
{
    return [=](const PassScratch& scratch) -> PassRowFn {
        const std::vector<const float*> image = constPlanes(scratch, from);
        const std::vector<float*> result = planes(scratch, to);
        const float* const key = scratch.plane(kSlotKey, 0);
        const int width = scratch.width;
        const int height = scratch.height;
        const int nPlanes = scratch.nPlanes;
        std::vector<int> span((std::size_t)(2 * radius + 1), 0);
        for (int dy = -radius; dy <= radius; ++dy) {
            int s = 0;
            while ((s + 1) * (s + 1) + dy * dy <= radius * radius) {
                ++s;
            }
            span[dy + radius] = s;
        }
        std::shared_ptr<std::vector<float>> sums = std::make_shared<std::vector<float>>((std::size_t)nPlanes);

        return [=](int row) {
            float* acc = sums->data();
            const std::size_t o = (std::size_t)row * (std::size_t)width;
            for (int x = 0; x < width; ++x) {
                const float centre = key[o + x];
                float weightSum = 0.f;
                std::fill(acc, acc + nPlanes, 0.f);
                for (int dy = -radius; dy <= radius; ++dy) {
                    const int yy = row + dy;
                    if ((yy < 0) || (yy >= height)) {
                        continue;
                    }
                    const int s = span[dy + radius];
                    const int lo = std::max(-s, -x);
                    const int hi = std::min(s, width - 1 - x);
                    const std::size_t base = (std::size_t)yy * (std::size_t)width;
                    for (int dx = lo; dx <= hi; ++dx) {
                        const std::size_t i = base + (std::size_t)(x + dx);
                        const float dk = key[i] - centre;
                        const float e = (float)(dx * dx + dy * dy) * invTwoSigmaS2 + dk * dk * invTwoSigmaR2;
                        const float w = std::exp(-e);
                        weightSum += w;
                        for (int p = 0; p < nPlanes; ++p) {
                            acc[p] += w * Bilateral::finiteSample(image[p][i]);
                        }
                    }
                }
                for (int p = 0; p < nPlanes; ++p) {
                    result[p][o + x] = acc[p] / weightSum;
                }
            }
        };
    };
}

// Guided mode's first horizontal pass: the row means, over the samples inside the buffer, of the
// key, its square, the image from slot `from` and key * image.
std::function<PassRowFn(const PassScratch&)>
guidedRowsWorker(int radius,
                 int from)
{
    return [=](const PassScratch& scratch) -> PassRowFn {
        const std::vector<const float*> image = constPlanes(scratch, from);
        const float* const key = scratch.plane(kSlotKey, 0);
        float* const rowKey = scratch.plane(kSlotRowKey, 0);
        float* const rowKey2 = scratch.plane(kSlotRowKey2, 0);
        const std::vector<float*> rowImage = planes(scratch, kSlotRowImage);
        const std::vector<float*> rowProduct = planes(scratch, kSlotRowProduct);
        const int width = scratch.width;
        const int nPlanes = scratch.nPlanes;
        std::shared_ptr<std::vector<float>> sums = std::make_shared<std::vector<float>>((std::size_t)(2 * nPlanes));

        return [=](int row) {
            float* sImage = sums->data();
            float* sProduct = sImage + nPlanes;
            const std::size_t o = (std::size_t)row * (std::size_t)width;
            for (int x = 0; x < width; ++x) {
                const int lo = std::max(0, x - radius);
                const int hi = std::min(width - 1, x + radius);
                const float n = (float)(hi - lo + 1);
                float sKey = 0.f;
                float sKey2 = 0.f;
                std::fill(sImage, sImage + 2 * nPlanes, 0.f);
                for (int i = lo; i <= hi; ++i) {
                    const float k = key[o + i];
                    sKey += k;
                    sKey2 += k * k;
                    for (int p = 0; p < nPlanes; ++p) {
                        const float v = Bilateral::finiteSample(image[p][o + i]);
                        sImage[p] += v;
                        sProduct[p] += k * v;
                    }
                }
                rowKey[o + x] = sKey / n;
                rowKey2[o + x] = sKey2 / n;
                for (int p = 0; p < nPlanes; ++p) {
                    rowImage[p][o + x] = sImage[p] / n;
                    rowProduct[p][o + x] = sProduct[p] / n;
                }
            }
        };
    };
}

// The column sums over [lo, hi] of nPlanes planes, accumulated row by row from the top so each
// pixel sums in the GPU's order.
void
sumColumns(const std::vector<const float*>& in,
           int width,
           int lo,
           int hi,
           float* sums)
{
    const std::size_t w = (std::size_t)width;
    std::fill(sums, sums + in.size() * w, 0.f);
    for (int j = lo; j <= hi; ++j) {
        const std::size_t o = (std::size_t)j * w;
        for (std::size_t q = 0; q < in.size(); ++q) {
            const float* src = in[q] + o;
            float* dst = sums + q * w;
            for (std::size_t x = 0; x < w; ++x) {
                dst[x] += src[x];
            }
        }
    }
}

// Guided mode's first vertical pass: the box means, and from them the linear coefficients a and
// b of each plane against the key.
std::function<PassRowFn(const PassScratch&)>
guidedCoefficientsWorker(int radius,
                         float eps)
{
    return [=](const PassScratch& scratch) -> PassRowFn {
        std::vector<const float*> in;
        in.push_back(scratch.plane(kSlotRowKey, 0));
        in.push_back(scratch.plane(kSlotRowKey2, 0));
        for (int p = 0; p < scratch.nPlanes; ++p) {
            in.push_back(scratch.plane(kSlotRowImage, p));
        }
        for (int p = 0; p < scratch.nPlanes; ++p) {
            in.push_back(scratch.plane(kSlotRowProduct, p));
        }
        const std::vector<float*> a = planes(scratch, kSlotA);
        const std::vector<float*> b = planes(scratch, kSlotB);
        const int width = scratch.width;
        const int height = scratch.height;
        const int nPlanes = scratch.nPlanes;
        std::shared_ptr<std::vector<float>> sums = std::make_shared<std::vector<float>>(in.size() * (std::size_t)width);

        return [=](int row) {
            const int lo = std::max(0, row - radius);
            const int hi = std::min(height - 1, row + radius);
            const float n = (float)(hi - lo + 1);
            float* s = sums->data();
            sumColumns(in, width, lo, hi, s);
            const std::size_t w = (std::size_t)width;
            const std::size_t o = (std::size_t)row * w;
            for (int x = 0; x < width; ++x) {
                const float mKey = s[x] / n;
                const float mKey2 = s[w + x] / n;
                const float d = std::max(mKey2 - mKey * mKey, 0.f) + eps;
                for (int p = 0; p < nPlanes; ++p) {
                    const float mImage = s[(2 + p) * w + x] / n;
                    const float mProduct = s[(2 + nPlanes + p) * w + x] / n;
                    const float av = (mProduct - mKey * mImage) / d;
                    a[p][o + x] = av;
                    b[p][o + x] = mImage - av * mKey;
                }
            }
        };
    };
}

// Guided mode's second horizontal pass: the row means of a and b, into the row image and row
// product slots, which the first vertical pass is done reading.
std::function<PassRowFn(const PassScratch&)>
guidedBoxRowsWorker(int radius)
{
    return [=](const PassScratch& scratch) -> PassRowFn {
        std::vector<const float*> in = constPlanes(scratch, kSlotA);
        const std::vector<const float*> b = constPlanes(scratch, kSlotB);
        in.insert(in.end(), b.begin(), b.end());
        std::vector<float*> out = planes(scratch, kSlotRowImage);
        const std::vector<float*> rowB = planes(scratch, kSlotRowProduct);
        out.insert(out.end(), rowB.begin(), rowB.end());
        const int width = scratch.width;

        return [=](int row) {
            const std::size_t o = (std::size_t)row * (std::size_t)width;
            for (int x = 0; x < width; ++x) {
                const int lo = std::max(0, x - radius);
                const int hi = std::min(width - 1, x + radius);
                const float n = (float)(hi - lo + 1);
                for (std::size_t q = 0; q < in.size(); ++q) {
                    float s = 0.f;
                    for (int i = lo; i <= hi; ++i) {
                        s += in[q][o + i];
                    }
                    out[q][o + x] = s / n;
                }
            }
        };
    };
}

// Guided mode's second vertical pass: mean(a) * key + mean(b) into slot `to`.
std::function<PassRowFn(const PassScratch&)>
guidedApplyWorker(int radius,
                  int to)
{
    return [=](const PassScratch& scratch) -> PassRowFn {
        std::vector<const float*> in = constPlanes(scratch, kSlotRowImage);
        const std::vector<const float*> rowB = constPlanes(scratch, kSlotRowProduct);
        in.insert(in.end(), rowB.begin(), rowB.end());
        const std::vector<float*> result = planes(scratch, to);
        const float* const key = scratch.plane(kSlotKey, 0);
        const int width = scratch.width;
        const int height = scratch.height;
        const int nPlanes = scratch.nPlanes;
        std::shared_ptr<std::vector<float>> sums = std::make_shared<std::vector<float>>(in.size() * (std::size_t)width);

        return [=](int row) {
            const int lo = std::max(0, row - radius);
            const int hi = std::min(height - 1, row + radius);
            const float n = (float)(hi - lo + 1);
            float* s = sums->data();
            sumColumns(in, width, lo, hi, s);
            const std::size_t w = (std::size_t)width;
            const std::size_t o = (std::size_t)row * w;
            for (int x = 0; x < width; ++x) {
                const float k = key[o + x];
                for (int p = 0; p < nPlanes; ++p) {
                    const float ma = s[p * w + x] / n;
                    const float mb = s[(nPlanes + p) * w + x] / n;
                    result[p][o + x] = ma * k + mb;
                }
            }
        };
    };
}

Pass
gatherWithoutSpread(int haloX,
                    int haloY,
                    std::size_t taps,
                    std::function<PassRowFn(const PassScratch&)> worker)
{
    Pass pass = Pass::gather(haloX, haloY, false, std::move(worker));
    pass.growX = 0;
    pass.growY = 0;
    pass.tapsPerSample = taps;

    return pass;
}

#ifdef NATRON_HAVE_GPU
/// Mirrored by BilateralParams in Engine/Gpu/Kernels/bilateral.slang.
struct BilateralGpuParams {
    // The CPU's buffer: the window grown by the halo, clipped to the region of definition.
    int32_t domX1;
    int32_t domY1;
    int32_t domX2;
    int32_t domY2;
    // The pixels of the image read that exist, the first at srcOffset floats.
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    // Floats per pixel of the image read.
    uint32_t srcChannels;
    // The rect written, its first pixel at dstOffset floats.
    int32_t outX1;
    int32_t outY1;
    int32_t outX2;
    int32_t outY2;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t channels;
    int32_t radius;
    float invTwoSigmaS2;
    float invTwoSigmaR2;
    float eps;
    // Per channel of the filtered image: its luminance weight and the component of the image read.
    float weight0;
    float weight1;
    float weight2;
    float weight3;
    int32_t comp0;
    int32_t comp1;
    int32_t comp2;
    int32_t comp3;
    uint32_t padding0;
    uint32_t padding1;
};

static_assert(sizeof(BilateralGpuParams) == 128);
static_assert(offsetof(BilateralGpuParams, srcX1) == 16);
static_assert(offsetof(BilateralGpuParams, outX1) == 44);
static_assert(offsetof(BilateralGpuParams, channels) == 68);
static_assert(offsetof(BilateralGpuParams, weight0) == 88);
static_assert(offsetof(BilateralGpuParams, comp0) == 104);

// The float index of pixel (x, y) of image, or false when the image exceeds the kernels' 32-bit
// indices.
bool
gpuFloatIndex(const gpu::GpuBlurImage& image,
              int x,
              int y,
              int nComps,
              uint32_t* out)
{
    const uint64_t index = image.offsetFloats + (uint64_t)(y - image.bounds.y1) * image.strideFloats + (uint64_t)(x - image.bounds.x1) * nComps;
    const uint64_t end = image.offsetFloats + (uint64_t)(image.bounds.y2 - image.bounds.y1) * image.strideFloats;
    if ((index > UINT32_MAX) || (end > UINT32_MAX)) {
        return false;
    }
    *out = (uint32_t)index;

    return true;
}

void
setRect(const RectI& r,
        int32_t* x1,
        int32_t* y1,
        int32_t* x2,
        int32_t* y2)
{
    *x1 = r.x1;
    *y1 = r.y1;
    *x2 = std::max(r.x1, r.x2);
    *y2 = std::max(r.y1, r.y2);
}

void
setChannel(BilateralGpuParams& p,
           int c,
           float weight,
           int comp)
{
    switch (c) {
    case 0:
        p.weight0 = weight;
        p.comp0 = comp;
        break;
    case 1:
        p.weight1 = weight;
        p.comp1 = comp;
        break;
    case 2:
        p.weight2 = weight;
        p.comp2 = comp;
        break;
    default:
        p.weight3 = weight;
        p.comp3 = comp;
        break;
    }
}

std::array<uint32_t, 3>
threadsOver(const RectI& r)
{
    return { (uint32_t)r.width(), (uint32_t)r.height(), 1 };
}

/// The passes on the GPU: the source loaded over the domain, then every iteration over the
/// domain in device buffers, the last one writing io.filtered over io.output, zero outside the
/// domain as the CPU's window is.
StatusEnum
recordBilateralGpu(const PassGpuIO& io,
                   const BilateralParams& params,
                   const RenderScale& scale,
                   bool guided)
{
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& output = io.output;
    if (PassExec::isEmptyRect(output)) {
        return eStatusOK;
    }
    const int nc = io.nComps;
    if ((nc < 1) || (nc > 4) || !io.filtered.buffer) {
        return eStatusFailed;
    }
    const bool guidedMode = (params.mode == eBilateralModeGuided);
    const double sigma = Bilateral::pixelSigma(scale, params);

    BilateralGpuParams base = {};
    base.channels = (uint32_t)nc;
    base.radius = guidedMode ? Bilateral::guidedRadius(scale, params) : Bilateral::bilateralRadius(scale, params);
    base.invTwoSigmaS2 = (float)(1. / (2. * sigma * sigma));
    base.invTwoSigmaR2 = (float)(1. / (2. * params.rangeSigma * params.rangeSigma));
    base.eps = (float)(params.rangeSigma * params.rangeSigma);

    const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
    BilateralGpuParams finalParams = base;
    setRect(output, &finalParams.outX1, &finalParams.outY1, &finalParams.outX2, &finalParams.outY2);
    finalParams.dstStride = filtered.strideFloats;
    if (!gpuFloatIndex(filtered, output.x1, output.y1, nc, &finalParams.dstOffset)) {
        return eStatusFailed;
    }

    const RectI domain = io.domain;
    if (PassExec::isEmptyRect(domain)) {
        // Nothing to filter: the direct kernel writes zero over the whole output.
        gpu::GpuBuffer* placeholder = nullptr;
        if (!ctx.scratch(4 * sizeof(float), placeholder) || !placeholder) {
            return eStatusFailed;
        }
        BilateralGpuParams p = finalParams;
        p.domX1 = p.domX2 = output.x1;
        p.domY1 = p.domY2 = output.y1;

        return ctx.dispatch(gpu::GpuKernelId::bilateral_direct, { placeholder, placeholder, filtered.buffer }, p, threadsOver(output)) ? eStatusOK : eStatusFailed;
    }
    setRect(domain, &base.domX1, &base.domY1, &base.domX2, &base.domY2);
    setRect(domain, &finalParams.domX1, &finalParams.domY1, &finalParams.domX2, &finalParams.domY2);

    const uint64_t area = (uint64_t)domain.width() * (uint64_t)domain.height();
    const uint64_t pixelFloats = guidedMode ? (uint64_t)(2 + 2 * nc) : (uint64_t)nc;
    if ((area * pixelFloats > UINT32_MAX) || (area * pixelFloats * sizeof(float) > ctx.maxScratchBytes())) {
        return eStatusFailed;
    }
    const uint32_t imageStride = (uint32_t)(domain.width() * nc);
    gpu::GpuBuffer* current = nullptr;
    gpu::GpuBuffer* next = nullptr;
    gpu::GpuBuffer* key = nullptr;
    gpu::GpuBuffer* rows = nullptr;
    gpu::GpuBuffer* coefficients = nullptr;
    gpu::GpuBuffer* boxed = nullptr;
    if (!ctx.scratch(area * nc * sizeof(float), current) || !current || !ctx.scratch(area * sizeof(float), key) || !key) {
        return eStatusFailed;
    }
    if ((params.iterations > 1) && (!ctx.scratch(area * nc * sizeof(float), next) || !next)) {
        return eStatusFailed;
    }
    if (guidedMode) {
        if (!ctx.scratch(area * (2 + 2 * nc) * sizeof(float), rows) || !rows || !ctx.scratch(area * 2 * nc * sizeof(float), coefficients) || !coefficients
            || !ctx.scratch(area * 2 * nc * sizeof(float), boxed) || !boxed) {
            return eStatusFailed;
        }
    }

    // The source as the CPU's slot 0 holds it: zero where it has no pixel.
    {
        BilateralGpuParams p = base;
        const gpu::GpuBuffer* sourceBuffer = current;
        if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
            const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
            sourceBuffer = io.source.buffer;
            setRect(io.sourceValid, &p.srcX1, &p.srcY1, &p.srcX2, &p.srcY2);
            p.srcStride = source.strideFloats;
            p.srcChannels = (uint32_t)nc;
            if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nc, &p.srcOffset)) {
                return eStatusFailed;
            }
        } else {
            p.srcX2 = p.srcX1;
            p.srcY2 = p.srcY1;
        }
        setRect(domain, &p.outX1, &p.outY1, &p.outX2, &p.outY2);
        p.dstStride = imageStride;
        if (!ctx.dispatch(gpu::GpuKernelId::bilateral_load, { sourceBuffer, sourceBuffer, current }, p, threadsOver(domain))) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
    }

    // The luminance weights of the filtered image's channels, as planeWeights() gives the CPU's.
    float weights[4] = { 0.f, 0.f, 0.f, 0.f };
    for (int c = 0; c < nc; ++c) {
        const int bit = pixelKernelChannelBit(nc, c);
        weights[c] = io.channels[bit] ? Bilateral::luminanceWeight(io.channels, bit) : 0.f;
    }

    if (guided) {
        const GpuImageView guide = (io.inputs.size() > (std::size_t)kBilateralInputGuide) ? io.inputs[kBilateralInputGuide] : GpuImageView();
        BilateralGpuParams p = base;
        const gpu::GpuBuffer* guideBuffer = key;
        const bool usable = guide.buffer && (guide.nComps >= 1) && (guide.nComps <= 4) && !PassExec::isEmptyRect(guide.bounds);
        for (int c = 0; c < nc; ++c) {
            const int comp = usable ? PassExec::channelIndexForBit(guide.nComps, pixelKernelChannelBit(nc, c)) : -1;
            setChannel(p, c, weights[c], comp);
        }
        for (int c = nc; c < 4; ++c) {
            setChannel(p, c, 0.f, -1);
        }
        if (usable) {
            const gpu::GpuBlurImage g = PassExec::toGpuBlurImage(guide);
            guideBuffer = guide.buffer;
            setRect(guide.bounds, &p.srcX1, &p.srcY1, &p.srcX2, &p.srcY2);
            p.srcStride = g.strideFloats;
            p.srcChannels = (uint32_t)guide.nComps;
            if (!gpuFloatIndex(g, guide.bounds.x1, guide.bounds.y1, guide.nComps, &p.srcOffset)) {
                return eStatusFailed;
            }
        } else {
            p.srcX2 = p.srcX1;
            p.srcY2 = p.srcY1;
        }
        setRect(domain, &p.outX1, &p.outY1, &p.outX2, &p.outY2);
        if (!ctx.dispatch(gpu::GpuKernelId::bilateral_key, { guideBuffer, guideBuffer, key }, p, threadsOver(domain))) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
    }

    for (int i = 0; i < params.iterations; ++i) {
        const bool last = (i == params.iterations - 1);
        if (!guided) {
            BilateralGpuParams p = base;
            setRect(domain, &p.srcX1, &p.srcY1, &p.srcX2, &p.srcY2);
            p.srcStride = imageStride;
            p.srcChannels = (uint32_t)nc;
            for (int c = 0; c < 4; ++c) {
                setChannel(p, c, (c < nc) ? weights[c] : 0.f, (c < nc) ? c : -1);
            }
            setRect(domain, &p.outX1, &p.outY1, &p.outX2, &p.outY2);
            if (!ctx.dispatch(gpu::GpuKernelId::bilateral_key, { current, current, key }, p, threadsOver(domain))) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
        }

        BilateralGpuParams dst = base;
        const gpu::GpuBuffer* dstBuffer = next;
        RectI written = domain;
        if (last) {
            dst = finalParams;
            dstBuffer = filtered.buffer;
            written = output;
        } else {
            setRect(domain, &dst.outX1, &dst.outY1, &dst.outX2, &dst.outY2);
            dst.dstStride = imageStride;
        }

        if (!guidedMode) {
            const gpu::GpuKernelId kernel = (base.radius <= Bilateral::kBilateralGpuTiledMaxRadius) ? gpu::GpuKernelId::bilateral_tiled : gpu::GpuKernelId::bilateral_direct;
            if (!ctx.dispatch(kernel, { current, key, dstBuffer }, dst, threadsOver(written))) {
                return eStatusFailed;
            }
        } else {
            BilateralGpuParams p = base;
            setRect(domain, &p.outX1, &p.outY1, &p.outX2, &p.outY2);
            if (!ctx.dispatch(gpu::GpuKernelId::bilateral_guided_rows, { current, key, rows }, p, threadsOver(domain))) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
            if (!ctx.dispatch(gpu::GpuKernelId::bilateral_guided_coefficients, { rows, rows, coefficients }, p, threadsOver(domain))) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
            if (!ctx.dispatch(gpu::GpuKernelId::bilateral_guided_box_rows, { coefficients, coefficients, boxed }, p, threadsOver(domain))) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
            if (!ctx.dispatch(gpu::GpuKernelId::bilateral_guided_apply, { boxed, key, dstBuffer }, dst, threadsOver(written))) {
                return eStatusFailed;
            }
        }
        if (!last) {
            ctx.computeBarrier();
            std::swap(current, next);
        }
    }

    return eStatusOK;
} // recordBilateralGpu
#endif // NATRON_HAVE_GPU
} // anonymous namespace

Bilateral::Bilateral(NodePtr node)
    : StagedImageEffect(node)
    , _mode()
    , _size()
    , _rangeSigma()
    , _iterations()
{
}

Bilateral::~Bilateral()
{
}

const NodeSpec*
Bilateral::nodeSpec() const
{
    return staticNodeSpec<kBilateralNodeSpec>();
}

const ImagePipelineSpec&
Bilateral::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kBilateralPipeline, kBilateralNodeSpec>();
}

std::string
Bilateral::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kBilateralInputSource:
        return tr("The image to smooth.").toStdString();
    case kBilateralInputMask:
        return tr("The mask the smoothed result is mixed with the source through.").toStdString();
    case kBilateralInputGuide:
        return tr("The Guide: when connected, its luminance decides which neighbours are alike, in place of the source's.").toStdString();
    }

    return std::string();
}

void
Bilateral::addAcceptedComponents(int inputNb,
                                 std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

void
Bilateral::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobChoicePtr modeKnob = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
    modeKnob->setName(kBilateralParamMode);
    modeKnob->setHintToolTip(tr("How the neighbours are weighed."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kBilateralParamModeBilateral, tr("Bilateral").toStdString(), tr("Each neighbour within 3 * Size is weighed by a Gaussian of its distance and a Gaussian of its luminance difference in stops. The cost grows with the square of Size.").toStdString()));
        options.push_back(ChoiceOption(kBilateralParamModeGuided, tr("Guided").toStdString(), tr("He et al.'s guided filter: the image is fitted as a linear function of the log-luminance over boxes of about 1.22 * Size pixels a side from the centre, regularised by Range Sigma squared. The cost does not depend on the luminance and grows only linearly with Size.").toStdString()));
        modeKnob->populateChoices(options);
    }
    modeKnob->setDefaultValue((int)eBilateralModeBilateral);
    page->addKnob(modeKnob);
    _mode = modeKnob;

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Size"));
    sizeKnob->setName(kBilateralParamSize);
    sizeKnob->setHintToolTip(tr("The spatial standard deviation, in pixel units (>=0). 0 leaves the input unchanged."));
    sizeKnob->setSpatial(true);
    sizeKnob->setMinimum(0.);
    sizeKnob->setMaximum(kMaxSize);
    sizeKnob->setDisplayMinimum(0.);
    sizeKnob->setDisplayMaximum(20.);
    sizeKnob->setIncrement(0.1);
    sizeKnob->setDefaultValue(3.);
    page->addKnob(sizeKnob);
    _size = sizeKnob;

    KnobDoublePtr rangeSigmaKnob = AppManager::createKnob<KnobDouble>(this, tr("Range Sigma"));
    rangeSigmaKnob->setName(kBilateralParamRangeSigma);
    rangeSigmaKnob->setHintToolTip(tr("How different two luminances may be and still be averaged, in stops (log2). Neighbours a Range Sigma apart weigh exp(-1/2) of an equal one. Being in stops, it treats dark and bright areas alike. 0 leaves the input unchanged."));
    rangeSigmaKnob->setMinimum(0.);
    rangeSigmaKnob->setDisplayMinimum(0.);
    rangeSigmaKnob->setDisplayMaximum(4.);
    rangeSigmaKnob->setIncrement(0.05);
    rangeSigmaKnob->setDefaultValue(1.);
    page->addKnob(rangeSigmaKnob);
    _rangeSigma = rangeSigmaKnob;

    KnobIntPtr iterationsKnob = AppManager::createKnob<KnobInt>(this, tr("Iterations"));
    iterationsKnob->setName(kBilateralParamIterations);
    iterationsKnob->setHintToolTip(tr("How many times the filter runs, each on the previous result, comparing the Guide's luminance when it is connected and the previous result's otherwise."));
    iterationsKnob->setMinimum(1);
    iterationsKnob->setMaximum(kMaxIterations);
    iterationsKnob->setDisplayMinimum(1);
    iterationsKnob->setDisplayMaximum(5);
    iterationsKnob->setDefaultValue(1);
    page->addKnob(iterationsKnob);
    _iterations = iterationsKnob;

    addStageKnobs(page);
} // Bilateral::initializeKnobs

BilateralParams
Bilateral::getParams(double time,
                     ViewIdx view) const
{
    const KnobReader reader(time, view);
    BilateralParams params;

    params.mode = (reader.value(_mode) == (int)eBilateralModeGuided) ? eBilateralModeGuided : eBilateralModeBilateral;
    params.size = std::min(kMaxSize, std::max(0., (double)reader.value(_size)));
    params.rangeSigma = std::max(0., (double)reader.value(_rangeSigma));
    params.iterations = std::min(kMaxIterations, std::max(0, (int)reader.value(_iterations)));

    return params;
}

double
Bilateral::pixelSigma(const RenderScale& scale,
                      const BilateralParams& params)
{
    return std::max(0., params.size * scale.toOfxPointD().x);
}

int
Bilateral::bilateralRadius(const RenderScale& scale,
                           const BilateralParams& params)
{
    const double sigma = pixelSigma(scale, params);

    return (sigma > 0.) ? (int)std::ceil(3. * sigma) : 0;
}

int
Bilateral::guidedRadius(const RenderScale& scale,
                        const BilateralParams& params)
{
    const double sigma = pixelSigma(scale, params);

    return (sigma > 0.) ? std::max(1, (int)std::lround(sigma * std::sqrt(1.5))) : 0;
}

PassList
Bilateral::passes(const BilateralParams& params,
                  const RenderScale& scale,
                  bool guided,
                  const std::shared_ptr<const BilateralGuide>& guide)
{
    PassList list;
    const double sigma = pixelSigma(scale, params);
    if ((params.iterations <= 0) || !(sigma > 0.) || !(params.rangeSigma > 0.)) {
        return list;
    }
    const float invTwoSigmaS2 = (float)(1. / (2. * sigma * sigma));
    const float invTwoSigmaR2 = (float)(1. / (2. * params.rangeSigma * params.rangeSigma));
    const float eps = (float)(params.rangeSigma * params.rangeSigma);
    const bool guidedMode = (params.mode == eBilateralModeGuided);

    list.boundary = PassExec::Boundary::Zero;
    if (guided) {
        list.passes.push_back(Pass::point(false, guideKeyWorker(guide)));
    }
    int from = kSlotSource;
    for (int i = 0; i < params.iterations; ++i) {
        const int to = (i % 2 == 0) ? kSlotImageA : kSlotImageB;
        if (!guided) {
            list.passes.push_back(Pass::point(false, imageKeyWorker(from)));
        }
        if (!guidedMode) {
            const int radius = bilateralRadius(scale, params);
            const std::size_t side = (std::size_t)(2 * radius + 1);
            list.passes.push_back(gatherWithoutSpread(radius, radius, side * side, bilateralWorker(radius, invTwoSigmaS2, invTwoSigmaR2, from, to)));
        } else {
            const int radius = guidedRadius(scale, params);
            const std::size_t side = (std::size_t)(2 * radius + 1);
            list.passes.push_back(gatherWithoutSpread(radius, 0, side, guidedRowsWorker(radius, from)));
            list.passes.push_back(gatherWithoutSpread(0, radius, side, guidedCoefficientsWorker(radius, eps)));
            list.passes.push_back(gatherWithoutSpread(radius, 0, side, guidedBoxRowsWorker(radius)));
            list.passes.push_back(gatherWithoutSpread(0, radius, side, guidedApplyWorker(radius, to)));
        }
        from = to;
    }
    list.scratchSlots = guidedMode ? kGuidedSlots : ((params.iterations > 1) ? kSlotImageB + 1 : kSlotImageA + 1);
    list.resultSlot = from;

    return list;
} // Bilateral::passes

RectI
Bilateral::getSourceRoI(const RectI& rect,
                        const RenderScale& scale,
                        const BilateralParams& params)
{
    return passListSourceRoI(rect, passes(params, scale, false, std::shared_ptr<const BilateralGuide>()));
}

float
Bilateral::luminanceWeight(const std::bitset<4>& channels,
                           int bit)
{
    if ((bit < 0) || (bit > 3) || !channels[bit]) {
        return 0.f;
    }
    if (channels[0] && channels[1] && channels[2]) {
        static const float kRec709[4] = { 0.2126f, 0.7152f, 0.0722f, 0.f };

        return kRec709[bit];
    }

    return 1.f / (float)channels.count();
}

float
Bilateral::luminanceKey(float l)
{
    if (!(l > kKeyFloor)) {
        l = kKeyFloor;
    }
    if (l > FLT_MAX) {
        l = FLT_MAX;
    }

    return std::log2(l);
}

float
Bilateral::finiteSample(float v)
{
    uint32_t u;
    std::memcpy(&u, &v, sizeof(u));

    return ((u & 0x7f800000u) == 0x7f800000u) ? 0.f : v;
}

PassList
Bilateral::opPasses(double time,
                    ViewIdx view,
                    const RenderScale& scale) const
{
    const BilateralParams params = getParams(time, view);
    const bool guided = (bool)getInput(kBilateralInputGuide);
    PassList list = passes(params, scale, guided, std::shared_ptr<const BilateralGuide>());

#ifdef NATRON_HAVE_GPU
    if (!list.empty() && ((params.mode == eBilateralModeGuided) || (bilateralRadius(scale, params) <= kBilateralGpuMaxRadius))) {
        list.gpu = [params, scale, guided](const PassGpuIO& io) -> StatusEnum {
            return recordBilateralGpu(io, params, scale, guided);
        };
    }
#endif

    return list;
}

bool
Bilateral::readsGpuInput(int inputNb,
                         const ImageLayerDesc& /*plane*/,
                         double /*time*/,
                         ViewIdx /*view*/) const
{
    if (inputNb != kBilateralInputGuide) {
        return true;
    }

    return (bool)getInput(kBilateralInputGuide);
}

RectI
Bilateral::opPassesInputRoI(int inputNb,
                            const RectI& outputRect,
                            const RenderScale& scale) const
{
    if (inputNb != kBilateralInputGuide) {
        return outputRect;
    }

    return passListSourceRoI(outputRect, opPasses(getCurrentTime(), getCurrentView(), scale));
}

std::optional<GpuCostHint>
Bilateral::gpuCostHint(double time,
                       ViewIdx view,
                       unsigned int mipmapLevel) const
{
    const BilateralParams params = getParams(time, view);
    const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
    const int radius = (params.mode == eBilateralModeGuided) ? guidedRadius(scale, params) : bilateralRadius(scale, params);
    if ((radius <= 0) || (params.iterations <= 0)) {
        return std::nullopt;
    }
    GpuCostHint hint;
    hint.size = radius * params.iterations;

    return hint;
}

bool
Bilateral::isOpIdentity(double time,
                        const RenderScale& scale,
                        const RectI& /*roi*/,
                        ViewIdx view)
{
    return opPasses(time, view, scale).empty();
}

StatusEnum
Bilateral::readGuide(const RenderActionArgs& args,
                     const RectI& window,
                     std::shared_ptr<BilateralGuide>* guide)
{
    std::shared_ptr<BilateralGuide> result = std::make_shared<BilateralGuide>();
    *guide = result;

    ImageLayerDesc layer;
    if (!resolveInputPlaneForRender(kBilateralInputGuide, args.time, args.view, &layer, NULL)) {
        return eStatusOK;
    }
    RectI roiPixel;
    // Read in the layout of the plane being rendered, as the source is.
    const ImagePtr image = getImage(kBilateralInputGuide, args.time, args.mappedScale, args.view, NULL, &layer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
    if (!image) {
        return eStatusOK;
    }
    if (!PassExec::isFloatImage(image)) {
        return eStatusFailed;
    }
    RectI valid;
    if (!PassExec::intersectRects(image->getBounds(), window, &valid)) {
        return eStatusOK;
    }
    const int nComps = (int)image->getComponentsCount();
    if ((nComps < 1) || (nComps > 4)) {
        return eStatusOK;
    }
    const std::size_t rowFloats = (std::size_t)valid.width() * (std::size_t)nComps;
    result->bounds = valid;
    result->nComps = nComps;
    result->pixels.assign(rowFloats * (std::size_t)valid.height(), 0.f);
    Image::ReadAccess access(image.get());
    for (int y = valid.y1; y < valid.y2; ++y) {
        const float* row = (const float*)access.pixelAt(valid.x1, y);
        if (row) {
            std::memcpy(result->pixels.data() + (std::size_t)(y - valid.y1) * rowFloats, row, rowFloats * sizeof(float));
        }
    }

    return eStatusOK;
} // Bilateral::readGuide

StatusEnum
Bilateral::renderOp(const RenderActionArgs& args,
                    const StagePlan& plan)
{
    const BilateralParams params = getParams(args.time, args.view);

    if (!getInput(kBilateralInputGuide)) {
        return runPassesCpu(*this, plan, args, passes(params, args.mappedScale, false, std::shared_ptr<const BilateralGuide>()));
    }
    const PassList shape = passes(params, args.mappedScale, true, std::shared_ptr<const BilateralGuide>());
    if (shape.empty()) {
        return runPassesCpu(*this, plan, args, shape);
    }
    // The Guide over every pixel the passes may read.
    std::shared_ptr<BilateralGuide> guide;
    const StatusEnum status = readGuide(args, passListSourceRoI(args.roi, shape), &guide);
    if (status != eStatusOK) {
        return status;
    }

    return runPassesCpu(*this, plan, args, passes(params, args.mappedScale, true, guide));
}

void
Bilateral::getRegionsOfInterest(double time,
                                const RenderScale& scale,
                                const RectD& outputRoD,
                                const RectD& renderWindow,
                                ViewIdx view,
                                RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr guide = getInput(kBilateralInputGuide);
    if (!guide) {
        return;
    }
    // The Guide is read over the render window grown by the full halo, in the source's pixels.
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(kBilateralInputSource);
    const RectI rectPixel = PassExec::toPixelEnclosing(renderWindow, scale, par);
    const RectD guideRoI = PassExec::toCanonical(passListSourceRoI(rectPixel, opPasses(time, view, scale)), scale, par);
    // The Guide may be the same effect as the source or the mask, which then needs both regions.
    RoIMap::iterator found = ret->find(guide);
    if (found != ret->end()) {
        found->second.merge(guideRoI);
    } else {
        ret->insert(std::make_pair(guide, guideRoI));
    }
}

NATRON_NAMESPACE_EXIT
