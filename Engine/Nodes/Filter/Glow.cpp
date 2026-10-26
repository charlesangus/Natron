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

#include "Glow.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Filter/Bloom.h"
#include "Engine/Nodes/Filter/BloomKernels.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
const int kSlotSource = 0;
const int kSlotMap = 1;
const int kSlotGlow = 2;

enum ChannelKind : uint32_t {
    eChannelUnprocessed = 0,
    eChannelColour = 1,
    eChannelAlpha = 2,
};

/**
 * @brief What the map and the combination do with each channel of a pixel, by its bit in the
 * space of pixelKernelChannelBit(): its luma weight, its grade and its kind. The weights are the
 * working space's over the processed colour channels, renormalised to sum to 1, so that a grey
 * of value v has luminance v; with no processed colour channel, a processed alpha is its own
 * luminance.
 **/
struct ChannelSetup {
    float weight[4] = { 0.f, 0.f, 0.f, 0.f };
    // The tint times the brightness, the saturation (1 off the colour channels) and 1 / gamma.
    float gain[4] = { 1.f, 1.f, 1.f, 1.f };
    float saturation[4] = { 1.f, 1.f, 1.f, 1.f };
    float invGamma[4] = { 1.f, 1.f, 1.f, 1.f };
    ChannelKind kind[4] = { eChannelUnprocessed, eChannelUnprocessed, eChannelUnprocessed, eChannelUnprocessed };
    // For a colour channel, its bit (0 red, 1 green, 2 blue), which the hue reads; else -1.
    int colourBit[4] = { -1, -1, -1, -1 };
};

/// bits[i] is the bit of channel i, processed[i] whether it is processed.
ChannelSetup
channelSetup(const GlowParams& params,
             const int* bits,
             const bool* processed,
             int n)
{
    ChannelSetup s;
    double sum = 0.;
    int nColour = 0;
    int alpha = -1;

    for (int i = 0; i < n; ++i) {
        if (!processed[i]) {
            continue;
        }
        const int bit = std::min(std::max(bits[i], 0), 3);
        s.gain[i] = (float)(params.brightness * params.tint[bit]);
        s.invGamma[i] = (float)(1. / params.gamma[bit]);
        if (bit < 3) {
            s.kind[i] = eChannelColour;
            s.colourBit[i] = bit;
            s.saturation[i] = (float)params.saturation[bit];
            sum += params.luma[bit];
            ++nColour;
        } else {
            s.kind[i] = eChannelAlpha;
            alpha = i;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (s.kind[i] == eChannelColour) {
            s.weight[i] = (sum > 0.) ? (float)(params.luma[bits[i]] / sum) : (float)(1. / nColour);
        }
    }
    if (!nColour && (alpha >= 0)) {
        s.weight[alpha] = 1.f;
    }

    return s;
}

ChannelSetup
channelSetupOf(const GlowParams& params,
               const PassScratch& scratch)
{
    int bits[4] = { 0, 0, 0, 0 };
    bool processed[4] = { false, false, false, false };
    const int n = std::min(4, (int)scratch.channelBits.size());

    for (int p = 0; p < n; ++p) {
        bits[p] = scratch.channelBits[p];
        processed[p] = true;
    }

    return channelSetup(params, bits, processed, n);
}

/// 0 up to 0, a parabola of curvature 1 / k up to k, then x - k / 2: continuous with its slope.
inline float
softKnee(float x,
         float k)
{
    if (x <= 0.f) {
        return 0.f;
    }
    if ((k > 0.f) && (x < k)) {
        return x * x / (2.f * k);
    }

    return x - 0.5f * k;
}

/// The luminance l of a pixel and the map's scale c(l) / l, with c its kept luminance.
inline void
mapScale(const ChannelSetup& setup,
         const float* v,
         int n,
         float threshold,
         float knee,
         float* s,
         float* c)
{
    float l = 0.f;

    for (int i = 0; i < n; ++i) {
        l += setup.weight[i] * v[i];
    }
    *c = softKnee(l - threshold, knee);
    *s = ((l > 0.f) && (*c > 0.f)) ? (*c / l) : 0.f;
}

/// The hue of (r, g, b) in degrees, in [0, 360], as HSVTool's rgb_to_hsv computes it: 0 for a grey.
inline float
hueOf(float r,
      float g,
      float b)
{
    const float minv = std::min(std::min(r, g), b);
    const float maxv = std::max(std::max(r, g), b);
    const float delta = maxv - minv;

    if ((maxv == 0.f) || (delta == 0.f)) {
        return 0.f;
    }
    float h;
    if (r == maxv) {
        h = (g - b) / delta;
    } else if (g == maxv) {
        h = 2.f + (b - r) / delta;
    } else {
        h = 4.f + (r - g) / delta;
    }
    h *= 1.f / 6.f;
    if (h < 0.f) {
        h += 1.f;
    }

    return h * 360.f;
}

/// HSVTool's hue key for the range of the single picked hue and its 50 degree rolloff.
inline float
hueKey(float h,
       float pick)
{
    float d = std::fabs(h - pick);

    d = std::min(d, 360.f - d);

    return std::max(0.f, 1.f - d / 50.f);
}

/**
 * @brief What the map of the pixel v (n planes) is multiplied by: the mask, null reading 1, at
 * index of its plane, times the hue key with isolate hue. The hue reads the processed colour
 * channels, an unprocessed one reading 0.
 **/
inline float
mapWeight(const GlowParams& params,
          const ChannelSetup& setup,
          const float* v,
          int n,
          const float* mask,
          std::size_t index)
{
    float w = mask ? mask[index] : 1.f;

    if (params.isolateHue) {
        float rgb[3] = { 0.f, 0.f, 0.f };
        for (int i = 0; i < n; ++i) {
            if (setup.colourBit[i] >= 0) {
                rgb[setup.colourBit[i]] = v[i];
            }
        }
        w *= hueKey(hueOf(rgb[0], rgb[1], rgb[2]), (float)params.hue);
    }

    return w;
}

/// How the bloom's axes lie on the image.
enum class FrameKind {
    // The image's: no stretch, or a rotation by a multiple of 180 degrees.
    eAxes,
    // Swapped: a rotation by an odd multiple of 90 degrees.
    eSwapped,
    // Any other rotation, through a resampled frame.
    eRotated,
};

/**
 * @brief The frame a rotated bloom runs in: the image in square pixels (x times the pixel aspect
 * ratio) turned counter-clockwise by rotate about the pixel origin. A pixel position p (pixel
 * (x, y) is centred at (x + 0.5, y + 0.5)) lies at toFrame p in the frame, and a frame position f
 * at toImage f in the image; both are row-major 2x2 matrices.
 **/
struct Frame {
    FrameKind kind = FrameKind::eAxes;
    float toFrame[4] = { 1.f, 0.f, 0.f, 1.f };
    float toImage[4] = { 1.f, 0.f, 0.f, 1.f };
};

Frame
frameOf(const GlowParams& params,
        double par)
{
    Frame frame;

    if (params.stretch <= 0.) {
        return frame;
    }
    double angle = std::fmod(params.rotate, 360.);
    if (angle < 0.) {
        angle += 360.;
    }
    if ((angle == 0.) || (angle == 180.) || (angle == 360.)) {
        return frame;
    }
    if ((angle == 90.) || (angle == 270.)) {
        frame.kind = FrameKind::eSwapped;

        return frame;
    }
    if (!(par > 0.)) {
        par = 1.;
    }
    const double t = angle * M_PI / 180.;
    const double c = std::cos(t);
    const double s = std::sin(t);
    frame.kind = FrameKind::eRotated;
    frame.toFrame[0] = (float)(c * par);
    frame.toFrame[1] = (float)-s;
    frame.toFrame[2] = (float)(s * par);
    frame.toFrame[3] = (float)c;
    frame.toImage[0] = (float)(c / par);
    frame.toImage[1] = (float)(s / par);
    frame.toImage[2] = (float)-s;
    frame.toImage[3] = (float)c;

    return frame;
}

/// The frame pixels covering the image pixels of rect turned into the frame, grown by 2 pixels.
RectI
frameRect(const Frame& frame,
          const RectI& rect)
{
    if (PassExec::isEmptyRect(rect)) {
        return RectI(0, 0, 0, 0);
    }
    const double xs[2] = { (double)rect.x1, (double)rect.x2 };
    const double ys[2] = { (double)rect.y1, (double)rect.y2 };
    double x1 = DBL_MAX;
    double y1 = DBL_MAX;
    double x2 = -DBL_MAX;
    double y2 = -DBL_MAX;
    for (double x : xs) {
        for (double y : ys) {
            const double fx = (double)frame.toFrame[0] * x + (double)frame.toFrame[1] * y;
            const double fy = (double)frame.toFrame[2] * x + (double)frame.toFrame[3] * y;
            x1 = std::min(x1, fx);
            y1 = std::min(y1, fy);
            x2 = std::max(x2, fx);
            y2 = std::max(y2, fy);
        }
    }

    return RectI((int)std::floor(x1) - 2, (int)std::floor(y1) - 2, (int)std::ceil(x2) + 2, (int)std::ceil(y2) + 2);
}

/**
 * @brief How far, in image pixels, extents of x by y frame pixels reach: the frame offsets within
 * them, turned into the image.
 **/
void
imageReach(const Frame& frame,
           double x,
           double y,
           int* reachX,
           int* reachY)
{
    *reachX = (int)std::ceil(std::fabs((double)frame.toImage[0]) * x + std::fabs((double)frame.toImage[1]) * y);
    *reachY = (int)std::ceil(std::fabs((double)frame.toImage[2]) * x + std::fabs((double)frame.toImage[3]) * y);
}

/**
 * @brief The bilinear sample at position (px, py) of plane, width x height pixels from pixel
 * (x1, y1), pixel (x, y) centred at (x + 0.5, y + 0.5); zero outside. The weights and the pixels
 * read come from the absolute position alone, so any window over the same pixels gives the same
 * sample bit for bit.
 **/
inline float
bilinear(const float* plane,
         int x1,
         int y1,
         int width,
         int height,
         float px,
         float py)
{
    const float u = px - 0.5f;
    const float v = py - 0.5f;
    const float fx = std::floor(u);
    const float fy = std::floor(v);
    const int x0 = (int)fx - x1;
    const int y0 = (int)fy - y1;
    const float wx = u - fx;
    const float wy = v - fy;
    const auto at = [&](int x, int y) -> float {
        return ((x >= 0) && (y >= 0) && (x < width) && (y < height)) ? plane[(std::size_t)y * width + x] : 0.f;
    };

    return (1.f - wy) * ((1.f - wx) * at(x0, y0) + wx * at(x0 + 1, y0)) + wy * ((1.f - wx) * at(x0, y0 + 1) + wx * at(x0 + 1, y0 + 1));
}

/// The bloom's blurs in pixels at the render scale: size across the stretch, size * (1 - stretch)
/// along it, on the image's axes, swapped, or on the square pixels of the rotated frame.
BloomKernels::Settings
settingsFor(const GlowParams& params,
            const PassContext& context)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    const double par = context.pixelAspectRatio;
    const double across = params.size;
    const double down = params.size * (1. - params.stretch);
    BloomKernels::Settings s;

    switch (frameOf(params, par).kind) {
    case FrameKind::eAxes:
        s.sizeX = rs.x * (across / par);
        s.sizeY = rs.y * down;
        break;
    case FrameKind::eSwapped:
        s.sizeX = rs.x * (down / par);
        s.sizeY = rs.y * across;
        break;
    case FrameKind::eRotated:
        s.sizeX = rs.x * across;
        s.sizeY = rs.y * down;
        break;
    }
    s.ratio = params.ratio;
    s.count = params.count;
    s.filter = params.filter;

    return s;
}

/// The map of slot 0, its colour channels saturated, into slot 1, every row of every plane.
std::function<PassRowFn(const PassScratch&)>
mapWorker(const GlowParams& params)
{
    return [params](const PassScratch& scratch) -> PassRowFn {
        const ChannelSetup setup = channelSetupOf(params, scratch);

        return [&scratch, params, setup](int r) {
            const int n = std::min(4, scratch.nPlanes);
            const float threshold = (float)params.threshold;
            const float knee = (float)params.knee;
            const std::size_t row = (std::size_t)r * scratch.width;
            const float* in[4];
            float* out[4];
            for (int p = 0; p < n; ++p) {
                in[p] = scratch.plane(kSlotSource, p) + row;
                out[p] = scratch.plane(kSlotMap, p) + row;
            }
            for (int x = 0; x < scratch.width; ++x) {
                float v[4];
                for (int p = 0; p < n; ++p) {
                    v[p] = in[p][x];
                }
                float s;
                float c;
                mapScale(setup, v, n, threshold, knee, &s, &c);
                const float w = mapWeight(params, setup, v, n, scratch.mask, row + x);
                for (int p = 0; p < n; ++p) {
                    const float m = v[p] * s;
                    const float g = (setup.kind[p] == eChannelColour) ? (c + setup.saturation[p] * (m - c)) : m;
                    out[p][x] = g * w;
                }
            }
        };
    };
}

/// The output into slot 2 from the source, the map and, outside the preview, the bloom in slot 2:
/// the bloom, plus the map with add input, is gamma-corrected and multiplied by the gain.
std::function<PassRowFn(const PassScratch&)>
combineWorker(const GlowParams& params)
{
    return [params](const PassScratch& scratch) -> PassRowFn {
        const ChannelSetup setup = channelSetupOf(params, scratch);

        return [&scratch, params, setup](int r) {
            const int n = std::min(4, scratch.nPlanes);
            const float threshold = (float)params.threshold;
            const float knee = (float)params.knee;
            const std::size_t row = (std::size_t)r * scratch.width;
            const float* src[4];
            const float* map[4];
            float* out[4];
            for (int p = 0; p < n; ++p) {
                src[p] = scratch.plane(kSlotSource, p) + row;
                map[p] = scratch.plane(kSlotMap, p) + row;
                out[p] = scratch.plane(kSlotGlow, p) + row;
            }
            for (int x = 0; x < scratch.width; ++x) {
                if (params.preview) {
                    float v[4];
                    for (int p = 0; p < n; ++p) {
                        v[p] = src[p][x];
                    }
                    float s;
                    float c;
                    mapScale(setup, v, n, threshold, knee, &s, &c);
                    const float w = mapWeight(params, setup, v, n, scratch.mask, row + x);
                    for (int p = 0; p < n; ++p) {
                        out[p][x] = (v[p] * s) * w;
                    }
                    continue;
                }
                for (int p = 0; p < n; ++p) {
                    float glow = out[p][x];
                    if (params.addInput) {
                        glow += map[p][x];
                    }
                    if ((setup.invGamma[p] != 1.f) && (glow > 0.f)) {
                        glow = std::pow(glow, setup.invGamma[p]);
                    }
                    glow *= setup.gain[p];
                    const float a = src[p][x];
                    float v;
                    if (params.glowOnly) {
                        v = glow;
                    } else if (params.screen) {
                        v = ((a <= 1.f) || (glow <= 1.f)) ? (a + glow - a * glow) : std::max(a, glow);
                    } else {
                        v = a + glow;
                    }
                    if (setup.kind[p] == eChannelAlpha) {
                        v = std::min(std::max(v, 0.f), 1.f);
                    }
                    out[p][x] = v;
                }
            }
        };
    };
}

/**
 * @brief The scratch of a rotated bloom on the CPU: two slots (the map turned into the frame, and
 * its bloom) over frameRect() of the image scratch, with its planes and channels. It is rewritten
 * whole by every plane job, so a job of the same shape reuses it.
 **/
class FrameScratch {
public:
    explicit FrameScratch(const Frame& frame)
        : _frame(frame)
    {
    }

    const Frame& frame() const
    {
        return _frame;
    }

    const PassScratch& prepare(const PassScratch& image)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_ready && (_imageRect.x1 == image.rect.x1) && (_imageRect.y1 == image.rect.y1) && (_imageRect.x2 == image.rect.x2) && (_imageRect.y2 == image.rect.y2) && (_scratch.nPlanes == image.nPlanes)) {
            return _scratch;
        }
        _imageRect = image.rect;
        _scratch = PassScratch();
        _scratch.rect = frameRect(_frame, image.rect);
        _scratch.width = std::max(0, _scratch.rect.width());
        _scratch.height = std::max(0, _scratch.rect.height());
        _scratch.nPlanes = image.nPlanes;
        _scratch.planeSize = (std::size_t)_scratch.width * _scratch.height;
        _scratch.nComps = image.nComps;
        _scratch.channelBits = image.channelBits;
        const std::size_t slotSize = _scratch.planeSize * std::max(0, _scratch.nPlanes);
        _storage.assign(std::max<std::size_t>(1, 2 * slotSize), 0.f);
        _scratch.buffers = { _storage.data(), _storage.data() + slotSize };
        _ready = true;

        return _scratch;
    }

private:
    const Frame _frame;
    std::mutex _mutex;
    bool _ready = false;
    RectI _imageRect;
    PassScratch _scratch;
    std::vector<float> _storage;
};

using FrameScratchPtr = std::shared_ptr<FrameScratch>;

/// A worker over the lines of the frame scratch, made from the image scratch and the frame's.
using FrameRowFactory = std::function<PassRowFn(const PassScratch& image, const PassScratch& frame)>;

/**
 * @brief A per-plane pass over the frame scratch, run on the image scratch's lines: image line
 * (plane p, row r) runs the frame rows [itemsBegin(r), itemsBegin(r + 1)) of plane p, so every
 * frame line runs once.
 **/
Pass
framePass(const FrameScratchPtr& state,
          const FrameRowFactory& factory)
{
    return Pass::point(true, [state, factory](const PassScratch& image) -> PassRowFn {
        const PassScratch& frame = state->prepare(image);
        const PassRowFn row = factory(image, frame);

        return [state, row, &image, &frame](int line) {
            const int lines = image.height;
            const int p = line / lines;
            const int r = line % lines;
            const auto begin = [&](int i) {
                return (int)(((int64_t)i * frame.height) / lines);
            };
            const int end = begin(r + 1);
            for (int f = begin(r); f < end; ++f) {
                row(p * frame.height + f);
            }
        };
    });
}

/**
 * @brief The passes of a rotated bloom of slot kSlotMap into slot kSlotGlow: the map resampled
 * into the frame, its bloom there, and the bloom resampled back over the whole image scratch,
 * after an extent pass carrying the halo and the spread turned into the image.
 **/
void
appendRotatedPasses(const BloomKernels::PlanPtr& plan,
                    const Frame& rotation,
                    bool expandRoD,
                    std::vector<Pass>* passes)
{
    const FrameScratchPtr state = std::make_shared<FrameScratch>(rotation);
    std::vector<Pass> bloom;
    BloomKernels::appendPasses(plan, 0, 1, 0, 0, &bloom);

    // An output pixel reads the frame bloom 1 pixel about it, which reads the frame map a halo
    // further, which reads the image map 1 pixel about each of its pixels.
    const int haloX = BloomKernels::haloX(*plan);
    const int haloY = BloomKernels::haloY(*plan);
    Pass extent;
    imageReach(rotation, haloX + 1, haloY + 1, &extent.haloX, &extent.haloY);
    extent.haloX += 2;
    extent.haloY += 2;
    if (expandRoD) {
        imageReach(rotation, haloX, haloY, &extent.growX, &extent.growY);
        extent.growX += 1;
        extent.growY += 1;
    }
    if (!bloom.empty()) {
        extent.sigma = bloom.front().sigma;
        extent.recursive = bloom.front().recursive;
    }
    passes->push_back(extent);

    passes->push_back(framePass(state, [state](const PassScratch& image, const PassScratch& frame) -> PassRowFn {
        const Frame& f = state->frame();

        return [&image, &frame, f](int line) {
            const int p = line / frame.height;
            const int row = line % frame.height;
            const float* src = image.plane(kSlotMap, p);
            float* dst = frame.plane(0, p) + (std::size_t)row * frame.width;
            const float y = (float)(frame.rect.y1 + row) + 0.5f;
            for (int i = 0; i < frame.width; ++i) {
                const float x = (float)(frame.rect.x1 + i) + 0.5f;
                const float sx = f.toImage[0] * x + f.toImage[1] * y;
                const float sy = f.toImage[2] * x + f.toImage[3] * y;
                dst[i] = bilinear(src, image.rect.x1, image.rect.y1, image.width, image.height, sx, sy);
            }
        };
    }));
    for (const Pass& pass : bloom) {
        if (!pass.rowWorker) {
            continue;
        }
        const std::function<PassRowFn(const PassScratch&)> worker = pass.rowWorker;
        passes->push_back(framePass(state, [worker](const PassScratch& /*image*/, const PassScratch& frame) -> PassRowFn {
            return worker(frame);
        }));
    }
    passes->push_back(Pass::point(true, [state](const PassScratch& image) -> PassRowFn {
        const PassScratch& frame = state->prepare(image);
        const Frame& f = state->frame();

        return [state, &image, &frame, f](int line) {
            const int p = line / image.height;
            const int r = line % image.height;
            const float* acc = frame.plane(1, p);
            float* dst = image.plane(kSlotGlow, p) + (std::size_t)r * image.width;
            const float y = (float)(image.rect.y1 + r) + 0.5f;
            for (int i = 0; i < image.width; ++i) {
                const float x = (float)(image.rect.x1 + i) + 0.5f;
                const float fx = f.toFrame[0] * x + f.toFrame[1] * y;
                const float fy = f.toFrame[2] * x + f.toFrame[3] * y;
                dst[i] = bilinear(acc, frame.rect.x1, frame.rect.y1, frame.width, frame.height, fx, fy);
            }
        };
    }));
}

#ifdef NATRON_HAVE_GPU
// Mirrors GlowParams in Kernels/glow.slang, which has no padding between members.
struct GlowGpuParams {
    int32_t domX1;
    int32_t domY1;
    uint32_t domWidth;
    uint32_t domHeight;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t outOffset;
    uint32_t outStride;
    uint32_t accStride;
    uint32_t channels;
    uint32_t flags;
    float threshold;
    float knee;
    // The ChannelKind of channel c in bits 2c and 2c + 1.
    uint32_t kinds;
    float weight[4];
    float gain[4];
    float saturation[4];
    float invGamma[4];
};
static_assert(sizeof(GlowGpuParams) == 128);
static_assert(offsetof(GlowGpuParams, flags) == 48);
static_assert(offsetof(GlowGpuParams, kinds) == 60);
static_assert(offsetof(GlowGpuParams, weight) == 64);
static_assert(offsetof(GlowGpuParams, invGamma) == 112);

constexpr uint32_t kGlowGpuPreview = 1;
constexpr uint32_t kGlowGpuGlowOnly = 2;
constexpr uint32_t kGlowGpuScreen = 4;
constexpr uint32_t kGlowGpuAddInput = 8;
// The map is multiplied by the weights glow_weight wrote, bound as accBuf to glow_map and to the
// preview's glow_combine.
constexpr uint32_t kGlowGpuWeighted = 32;

// Mirrors FrameParams in Kernels/glow_frame.slang, which has no padding between members.
struct GlowFrameGpuParams {
    int32_t srcX1;
    int32_t srcY1;
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t srcStride;
    int32_t dstX1;
    int32_t dstY1;
    uint32_t dstWidth;
    uint32_t dstHeight;
    uint32_t channels;
    uint32_t flags;
    float m00;
    float m01;
    float m10;
    float m11;
    int32_t maskX1;
    int32_t maskY1;
    uint32_t maskWidth;
    uint32_t maskHeight;
    uint32_t maskOffset;
    uint32_t maskStride;
    uint32_t maskComps;
    uint32_t maskComponent;
    float hue;
    uint32_t colourBits;
};
static_assert(sizeof(GlowFrameGpuParams) == 100);
static_assert(offsetof(GlowFrameGpuParams, m00) == 44);
static_assert(offsetof(GlowFrameGpuParams, maskX1) == 60);
static_assert(offsetof(GlowFrameGpuParams, hue) == 92);

constexpr uint32_t kGlowFrameHue = 1;
constexpr uint32_t kGlowFrameMask = 2;
constexpr uint32_t kGlowFrameMaskZero = 4;

template <typename Params>
bool
dispatchGlow(gpu::GpuRecordContext& ctx,
             gpu::GpuKernelId kernel,
             std::initializer_list<const gpu::GpuBuffer*> buffers,
             const Params& p,
             uint32_t width,
             uint32_t height)
{
    if (!width || !height) {
        return true;
    }
    if (!ctx.dispatch(kernel, buffers, p, { width, height, 1 })) {
        return false;
    }
    ctx.computeBarrier();

    return true;
}

/**
 * @brief Records the weights the map is multiplied by, one float per pixel of io.domain, from the
 * filled source (io.nComps floats per pixel over io.domain) and the mask of io, into *weight.
 **/
StatusEnum
recordWeights(const GlowParams& params,
              const PassGpuIO& io,
              const ChannelSetup& setup,
              const gpu::GpuBuffer* fill,
              gpu::GpuBuffer** weight)
{
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& domain = io.domain;
    if (!BloomKernels::gpuScratch(ctx, domain, 1, weight)) {
        return eStatusFailed;
    }
    if (PassExec::isEmptyRect(domain)) {
        return eStatusOK;
    }

    GlowFrameGpuParams w {};
    w.srcX1 = domain.x1;
    w.srcY1 = domain.y1;
    w.srcWidth = (uint32_t)domain.width();
    w.srcHeight = (uint32_t)domain.height();
    w.srcStride = (uint32_t)(domain.width() * io.nComps);
    w.dstX1 = domain.x1;
    w.dstY1 = domain.y1;
    w.dstWidth = w.srcWidth;
    w.dstHeight = w.srcHeight;
    w.channels = (uint32_t)io.nComps;
    w.flags = params.isolateHue ? kGlowFrameHue : 0u;
    w.hue = (float)params.hue;
    for (int c = 0; c < 4; ++c) {
        const uint32_t bit = (setup.colourBit[c] >= 0) ? (uint32_t)setup.colourBit[c] : 3u;
        w.colourBits |= bit << (2 * c);
    }

    const gpu::GpuBuffer* maskBuffer = fill;
    if (io.maskApplied) {
        const GpuImageView& mask = io.mask;
        if (mask.buffer && (io.maskComponent >= 0) && !PassExec::isEmptyRect(mask.bounds)) {
            const uint64_t offset = mask.byteOffset / sizeof(float);
            if (((mask.byteOffset % sizeof(float)) != 0) || ((offset + (uint64_t)mask.bounds.height() * (uint64_t)mask.rowStrideFloats) > UINT32_MAX)) {
                return eStatusFailed;
            }
            w.flags |= kGlowFrameMask;
            w.maskX1 = mask.bounds.x1;
            w.maskY1 = mask.bounds.y1;
            w.maskWidth = (uint32_t)mask.bounds.width();
            w.maskHeight = (uint32_t)mask.bounds.height();
            w.maskOffset = (uint32_t)offset;
            w.maskStride = (uint32_t)mask.rowStrideFloats;
            w.maskComps = (uint32_t)mask.nComps;
            w.maskComponent = (uint32_t)io.maskComponent;
            maskBuffer = mask.buffer;
        } else {
            w.flags |= kGlowFrameMaskZero;
        }
    }

    return dispatchGlow(ctx, gpu::GpuKernelId::glow_weight, { fill, maskBuffer, *weight }, w, w.dstWidth, w.dstHeight) ? eStatusOK : eStatusFailed;
}

/// Records the bilinear resampling of src (nComps floats per pixel of srcRect) into dst over
/// dstRect: dst pixel position p reads src at m p, zero outside srcRect.
bool
recordResample(gpu::GpuRecordContext& ctx,
               const gpu::GpuBuffer* src,
               const RectI& srcRect,
               const gpu::GpuBuffer* dst,
               const RectI& dstRect,
               int nComps,
               const float* m)
{
    if (PassExec::isEmptyRect(dstRect)) {
        return true;
    }
    const bool emptySrc = PassExec::isEmptyRect(srcRect);
    GlowFrameGpuParams r {};
    r.srcX1 = srcRect.x1;
    r.srcY1 = srcRect.y1;
    r.srcWidth = emptySrc ? 0u : (uint32_t)srcRect.width();
    r.srcHeight = emptySrc ? 0u : (uint32_t)srcRect.height();
    r.srcStride = r.srcWidth * (uint32_t)nComps;
    r.dstX1 = dstRect.x1;
    r.dstY1 = dstRect.y1;
    r.dstWidth = (uint32_t)dstRect.width();
    r.dstHeight = (uint32_t)dstRect.height();
    r.channels = (uint32_t)nComps;
    r.m00 = m[0];
    r.m01 = m[1];
    r.m10 = m[2];
    r.m11 = m[3];

    return dispatchGlow(ctx, gpu::GpuKernelId::glow_resample, { src, src, dst }, r, r.dstWidth, r.dstHeight);
}

/**
 * @brief Records the bloom of map (over io.domain) into acc (over io.output) in the rotated frame:
 * the map resampled into the frame over frameRect(io.domain), its bloom over the whole of that, as
 * the CPU runs it (a recursive filter depends on where it starts), and that resampled back.
 **/
StatusEnum
recordRotatedBloom(const BloomKernels::Plan& plan,
                   const Frame& frame,
                   const PassGpuIO& io,
                   const gpu::GpuBuffer* map,
                   const gpu::GpuBuffer* acc)
{
    gpu::GpuRecordContext& ctx = io.context;
    const int nComps = io.nComps;
    const RectI q = frameRect(frame, io.domain);

    gpu::GpuBuffer* frameMap = nullptr;
    gpu::GpuBuffer* frameAcc = nullptr;
    if (!BloomKernels::gpuScratch(ctx, q, nComps, &frameMap) || !BloomKernels::gpuScratch(ctx, q, nComps, &frameAcc)) {
        return eStatusFailed;
    }
    if (!recordResample(ctx, map, io.domain, frameMap, q, nComps, frame.toImage)) {
        return eStatusFailed;
    }
    if (!PassExec::isEmptyRect(q)) {
        PassGpuIO frameIO = io;
        frameIO.domain = q;
        frameIO.rod = q;
        frameIO.output = q;
        frameIO.haloX = BloomKernels::haloX(plan);
        frameIO.haloY = BloomKernels::haloY(plan);
        if (BloomKernels::recordGpu(plan, frameIO, frameMap, BloomKernels::gpuScratchImage(frameAcc, q, nComps)) != eStatusOK) {
            return eStatusFailed;
        }
    }

    return recordResample(ctx, frameAcc, q, acc, io.output, nComps, frame.toFrame) ? eStatusOK : eStatusFailed;
}

PassGpuRecorder
glowGpuRecorder(const GlowParams& params,
                const BloomKernels::PlanPtr& plan,
                const Frame& frame)
{
    return [params, plan, frame](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuRecordContext& ctx = io.context;
        const int nComps = io.nComps;
        const RectI& domain = io.domain;
        const RectI& output = io.output;
        if ((nComps < 1) || (nComps > 4)) {
            return eStatusFailed;
        }

        int bits[4] = { 0, 0, 0, 0 };
        bool processed[4] = { false, false, false, false };
        for (int c = 0; c < nComps; ++c) {
            bits[c] = pixelKernelChannelBit(nComps, c);
            processed[c] = io.channels[bits[c]];
        }
        const ChannelSetup setup = channelSetup(params, bits, processed, nComps);

        GlowGpuParams p {};
        const bool emptyDomain = PassExec::isEmptyRect(domain);
        p.domX1 = domain.x1;
        p.domY1 = domain.y1;
        p.domWidth = emptyDomain ? 0u : (uint32_t)domain.width();
        p.domHeight = emptyDomain ? 0u : (uint32_t)domain.height();
        p.channels = (uint32_t)nComps;
        p.flags = (params.preview ? kGlowGpuPreview : 0u) | (params.glowOnly ? kGlowGpuGlowOnly : 0u) | (params.screen ? kGlowGpuScreen : 0u) | (params.addInput ? kGlowGpuAddInput : 0u);
        p.threshold = (float)params.threshold;
        p.knee = (float)params.knee;
        for (int c = 0; c < 4; ++c) {
            p.kinds |= (uint32_t)setup.kind[c] << (2 * c);
            p.weight[c] = setup.weight[c];
            p.gain[c] = setup.gain[c];
            p.saturation[c] = setup.saturation[c];
            p.invGamma[c] = setup.invGamma[c];
        }

        gpu::GpuBuffer* fill = nullptr;
        if (BloomKernels::recordFill(io, false, &fill) != eStatusOK) {
            return eStatusFailed;
        }
        gpu::GpuBuffer* weight = nullptr;
        if (params.isolateHue || io.maskApplied) {
            if (recordWeights(params, io, setup, fill, &weight) != eStatusOK) {
                return eStatusFailed;
            }
            p.flags |= kGlowGpuWeighted;
        }
        gpu::GpuBuffer* map = nullptr;
        if (!BloomKernels::gpuScratch(ctx, domain, nComps, &map)) {
            return eStatusFailed;
        }
        if (!dispatchGlow(ctx, gpu::GpuKernelId::glow_map, { fill, map, weight ? weight : fill, map }, p, p.domWidth, p.domHeight)) {
            return eStatusFailed;
        }
        if (PassExec::isEmptyRect(output)) {
            return eStatusOK;
        }

        gpu::GpuBuffer* acc = weight ? weight : map;
        if (!params.preview) {
            if (!BloomKernels::gpuScratch(ctx, output, nComps, &acc)) {
                return eStatusFailed;
            }
            if (frame.kind == FrameKind::eRotated) {
                if (recordRotatedBloom(*plan, frame, io, map, acc) != eStatusOK) {
                    return eStatusFailed;
                }
            } else if (BloomKernels::recordGpu(*plan, io, map, BloomKernels::gpuScratchImage(acc, output, nComps)) != eStatusOK) {
                return eStatusFailed;
            }
        }

        const gpu::GpuBlurImage dst = PassExec::toGpuBlurImage(io.filtered);
        p.outX1 = output.x1;
        p.outY1 = output.y1;
        p.outWidth = (uint32_t)output.width();
        p.outHeight = (uint32_t)output.height();
        p.outStride = dst.strideFloats;
        p.accStride = (uint32_t)(output.width() * nComps);
        if (!BloomKernels::gpuFloatIndex(dst, output.x1, output.y1, nComps, &p.outOffset)) {
            return eStatusFailed;
        }

        return dispatchGlow(ctx, gpu::GpuKernelId::glow_combine, { fill, map, acc, dst.buffer }, p, p.outWidth, p.outHeight) ? eStatusOK : eStatusFailed;
    };
}
#endif // NATRON_HAVE_GPU

/// The map of slot 0 into slot 1, its bloom into slot 2 outside the preview, and the output over
/// it, with black outside the image.
PassList
glowPasses(const GlowParams& params,
           const PassContext& context)
{
    const Frame frame = frameOf(params, context.pixelAspectRatio);
    const BloomKernels::PlanPtr plan = BloomKernels::makePlan(settingsFor(params, context), false);
    PassList list;

    list.boundary = PassExec::Boundary::Zero;
    list.scratchSlots = 3;
    list.resultSlot = kSlotGlow;
    // The mask multiplies the map, as the PyPlug's did, rather than mixing the output.
    list.opMask = true;
    list.passes.push_back(Pass::point(false, mapWorker(params)));
    if (!params.preview) {
        if (frame.kind == FrameKind::eRotated) {
            appendRotatedPasses(plan, frame, params.expandRoD, &list.passes);
        } else {
            const int growX = params.expandRoD ? BloomKernels::haloX(*plan) : 0;
            const int growY = params.expandRoD ? BloomKernels::haloY(*plan) : 0;
            BloomKernels::appendPasses(plan, kSlotMap, kSlotGlow, growX, growY, &list.passes);
        }
    }
    list.passes.push_back(Pass::point(false, combineWorker(params)));

#ifdef NATRON_HAVE_GPU
    list.gpu = glowGpuRecorder(params, plan, frame);
#endif

    return list;
}

KnobDoublePtr
addDoubleKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const QString& label,
              const QString& hint,
              double defaultValue,
              double minimum,
              double displayMinimum,
              double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setMinimum(minimum);
    knob->setDisplayMinimum(displayMinimum);
    knob->setDisplayMaximum(displayMaximum);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

/// A colour knob of dims channels, every one defaulting to 1, with no hard bounds.
KnobColorPtr
addColorKnob(KnobHolder* holder,
             const KnobPagePtr& page,
             const char* name,
             const QString& label,
             const QString& hint,
             int dims,
             double displayMinimum,
             double displayMaximum,
             ColorKnobRoleEnum role)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, dims);

    knob->setName(name);
    knob->setHintToolTip(hint);
    for (int d = 0; d < dims; ++d) {
        knob->setDefaultValue(1., d);
        knob->setMinimum(-DBL_MAX, d);
        knob->setMaximum(DBL_MAX, d);
        knob->setDisplayMinimum(displayMinimum, d);
        knob->setDisplayMaximum(displayMaximum, d);
    }
    knob->setColorRole(role);
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
addBoolKnob(KnobHolder* holder,
            const KnobPagePtr& page,
            const char* name,
            const QString& label,
            const QString& hint,
            bool newLine)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(false);
    knob->setAddNewLine(newLine);
    page->addKnob(knob);

    return knob;
}
} // anonymous namespace

void
GlowOp::createKnobs(KnobHolder* holderArg,
                    const KnobPagePtr& page)
{
    holder = holderArg;

    threshold = addDoubleKnob(holder, page, kGlowParamThreshold, EffectInstance::tr("Threshold"),
                              EffectInstance::tr("The luminance above which the image glows, in the working space's scene-linear units. Higher values keep only the brightest areas."), 0.25, 0., 0., 1.);
    knee = addDoubleKnob(holder, page, kGlowParamKnee, EffectInstance::tr("Knee"),
                         EffectInstance::tr("The width of the soft knee above the threshold, over which the glow fades in instead of starting abruptly. 0 is a hard threshold."), 0.5, 0., 0., 1.);
    isolateHue = addBoolKnob(holder, page, kGlowParamIsolateHue, EffectInstance::tr("Isolate Hue"),
                             EffectInstance::tr("Restrict the glow to the areas of the picked hue as well: the glow map is multiplied by how close each pixel's hue is to it, 1 at the picked hue and 0 from 50 degrees of hue away. Greys count as hue 0, red."), true);

    KnobColorPtr pickKnob = AppManager::createKnob<KnobColor>(holder, EffectInstance::tr("Pick Hue"), 3);
    pickKnob->setName(kGlowParamPickHue);
    pickKnob->setHintToolTip(EffectInstance::tr("Pick a colour in the viewer to isolate its hue. The glow is only applied to the areas of that hue when Isolate Hue is checked."));
    for (int d = 0; d < 3; ++d) {
        pickKnob->setDefaultValue(0., d);
        pickKnob->setMinimum(-DBL_MAX, d);
        pickKnob->setMaximum(DBL_MAX, d);
        pickKnob->setDisplayMinimum(0., d);
        pickKnob->setDisplayMaximum(1., d);
    }
    page->addKnob(pickKnob);
    pickHue = pickKnob;

    preview = addBoolKnob(holder, page, kGlowParamPreview, EffectInstance::tr("Glow Map Preview"),
                          EffectInstance::tr("Output the glow map, the highlights that glow, to adjust the threshold and the knee."), true);

    KnobDoublePtr sizeKnob = addDoubleKnob(holder, page, kGlowParamSize, EffectInstance::tr("Size"),
                                           EffectInstance::tr("Size (diameter) of the smallest blur of the glow, in pixel units. Each next blur is ratio times larger."), 3., 0., 0., 100.);
    sizeKnob->setMaximum(1000.);
    sizeKnob->setSpatial(true);
    size = sizeKnob;
    stretch = addDoubleKnob(holder, page, kGlowParamStretch, EffectInstance::tr("Stretch"),
                            EffectInstance::tr("Stretch the glow horizontally: 0 keeps it round, 1 blurs horizontally only."), 0., 0., 0., 1.);
    stretch.lock()->setMaximum(1.);
    rotate = addDoubleKnob(holder, page, kGlowParamRotate, EffectInstance::tr("Rotate"),
                           EffectInstance::tr("Rotate the stretched glow, in degrees: the stretch, horizontal at 0, turns clockwise by this angle. It has no effect when Stretch is 0."), 0., -DBL_MAX, -360., 360.);
    ratio = addDoubleKnob(holder, page, kGlowParamRatio, EffectInstance::tr("Ratio"),
                          EffectInstance::tr("How many times larger each blur is than the previous one. At 1 or less, there is a single blur."), 2., 1., 1., 4.);
    ratio.lock()->setMaximum(BloomKernels::kMaxRatio);

    KnobIntPtr countKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Count"));
    countKnob->setName(kGlowParamCount);
    countKnob->setHintToolTip(EffectInstance::tr("The number of blurs averaged into the glow. More give a wider, heavier tail."));
    countKnob->setMinimum(1);
    countKnob->setMaximum(BloomKernels::kMaxCount);
    countKnob->setDisplayMinimum(1);
    countKnob->setDisplayMaximum(10);
    countKnob->setDefaultValue(7);
    page->addKnob(countKnob);
    count = countKnob;

    filter = Bloom::createFilterKnob(holder, page, kGlowParamFilter);

    tint = addColorKnob(holder, page, kGlowParamTint, EffectInstance::tr("Tint"),
                        EffectInstance::tr("The gain of each channel of the glow, alpha included, applied after the gamma."),
                        4, 0., 4., eColorKnobRoleGain);

    brightness = addDoubleKnob(holder, page, kGlowParamBrightness, EffectInstance::tr("Brightness"),
                               EffectInstance::tr("The gain of the glow, on top of the tint."), 1., -DBL_MAX, 0., 4.);
    saturation = addColorKnob(holder, page, kGlowParamSaturation, EffectInstance::tr("Saturation"),
                              EffectInstance::tr("The saturation of each colour channel of the glow, about its luminance: 0 makes the channel the luminance, 1 keeps the source's colours."),
                              3, 0., 4., eColorKnobRoleGain);
    gamma = addColorKnob(holder, page, kGlowParamGamma, EffectInstance::tr("Gamma"),
                         EffectInstance::tr("The gamma of each channel of the glow, alpha included, applied after the bloom and before the tint. Values at or below 0 are not affected. A gamma below 0.01 is treated as 0.01."),
                         4, 0.2, 5., eColorKnobRolePower);

    screen = addBoolKnob(holder, page, kGlowParamScreen, EffectInstance::tr("Screen"),
                         EffectInstance::tr("Screen the glow over the input, which preserves its highlights, instead of adding it."), false);
    addInput = addBoolKnob(holder, page, kGlowParamAddInput, EffectInstance::tr("Add Input"),
                           EffectInstance::tr("Add the glow map itself to its bloom, which sharpens the core of the glow."), false);
    glowOnly = addBoolKnob(holder, page, kGlowParamGlowOnly, EffectInstance::tr("Glow Only"),
                           EffectInstance::tr("Output the glow alone, to add or screen it over the source later."), false);
    expandRoD = addBoolKnob(holder, page, kGlowParamExpandRoD, EffectInstance::tr("Expand RoD"),
                            EffectInstance::tr("Grow the region of definition by the bloom's reach, so the glow spills past the input's edges instead of being cropped. Large sizes and counts make it very large."), true);
} // GlowOp::createKnobs

GlowOp::Params
GlowOp::bake(const KnobReader& reader) const
{
    Params params;

    params.threshold = std::max(0., reader.value(threshold));
    params.knee = std::max(0., reader.value(knee));
    params.preview = reader.value(preview);
    params.size = reader.value(size);
    params.stretch = std::min(std::max(reader.value(stretch), 0.), 1.);
    params.rotate = reader.value(rotate);
    params.isolateHue = reader.value(isolateHue);
    double pick[3];
    for (int d = 0; d < 3; ++d) {
        pick[d] = reader.value(pickHue, d);
    }
    params.hue = hueOf((float)pick[0], (float)pick[1], (float)pick[2]);
    params.ratio = reader.value(ratio);
    params.count = reader.value(count);
    const int f = reader.value(filter);
    if ((f >= (int)BlurKernels::eFilterQuasiGaussian) && (f <= (int)BlurKernels::eFilterFIRGaussian)) {
        params.filter = (BlurKernels::Filter)f;
    }
    for (int d = 0; d < 4; ++d) {
        params.tint[d] = reader.value(tint, d);
        params.gamma[d] = std::max(0.01, reader.value(gamma, d));
    }
    for (int d = 0; d < 3; ++d) {
        params.saturation[d] = reader.value(saturation, d);
    }
    params.brightness = reader.value(brightness);
    params.screen = reader.value(screen);
    params.addInput = reader.value(addInput);
    params.glowOnly = reader.value(glowOnly);
    params.expandRoD = reader.value(expandRoD);

    const double* fallback = ColorMath::acesCgLumaWeights();
    for (int d = 0; d < 3; ++d) {
        params.luma[d] = fallback[d];
    }
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();
    if (cm) {
        cm->getWorkingLuma(params.luma);
    }

    return params;
} // GlowOp::bake

bool
GlowOp::isIdentity(const Params& /*params*/,
                   const PassContext& /*context*/) const
{
    return false;
}

PassList
GlowOp::passes(const Params& params,
               const PassContext& context) const
{
    return glowPasses(params, context);
}

Glow::Glow(NodePtr node)
    : ImageNode<GlowOp>(node)
{
}

Glow::~Glow()
{
}

RectI
Glow::getSourceRoI(const RectI& rect,
                   const RenderScale& scale,
                   const GlowParams& params)
{
    PassContext context;

    context.scale = scale;

    return passListSourceRoI(rect, glowPasses(params, context));
}

NATRON_NAMESPACE_EXIT
