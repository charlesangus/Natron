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

#include "RetouchRender.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#include "Engine/Nodes/Filter/EdgeAware/Wavelets.h"
#include "Engine/Nodes/Filter/RetouchAlgorithms.h"

NATRON_NAMESPACE_ENTER

namespace RetouchCore {
void
applyBrushPressure(BrushPressureEnum mode,
                   float pressure,
                   float minRadius,
                   BrushPoint* point)
{
    switch (mode) {
    case eBrushPressureSizeRelative:
        point->radius = std::max(minRadius, point->radius * pressure);
        break;
    case eBrushPressureHardnessAbsolute:
        point->hardness = std::max(kBrushHardnessMin, pressure);
        break;
    case eBrushPressureHardnessRelative:
        point->hardness = std::max(kBrushHardnessMin, point->hardness * pressure);
        break;
    case eBrushPressureOpacityAbsolute:
        point->density = std::max(kBrushDensityPressureMin, pressure);
        break;
    case eBrushPressureOpacityRelative:
        point->density = std::max(kBrushDensityPressureMin, point->density * pressure);
        break;
    case eBrushPressureOff:
        break;
    }
}

Region
brushBounds(const std::vector<BrushPoint>& points)
{
    Region r;
    if (points.empty()) {
        return r;
    }
    float x1 = points[0].x, y1 = points[0].y, x2 = x1, y2 = y1;
    for (const BrushPoint& p : points) {
        x1 = std::min(x1, p.x - p.radius);
        y1 = std::min(y1, p.y - p.radius);
        x2 = std::max(x2, p.x + p.radius);
        y2 = std::max(y2, p.y + p.radius);
    }
    r.x = (int)std::floor(x1);
    r.y = (int)std::floor(y1);
    r.width = (int)std::ceil(x2) - r.x;
    r.height = (int)std::ceil(y2) - r.y;

    return r;
}

namespace {
    inline float
    brushFalloff(float distance,
                 float radius,
                 float hardness,
                 float density)
    {
        if ((radius <= 0.f) || (distance >= radius)) {
            return 0.f;
        }
        const float u = distance / radius;
        if ((u <= hardness) || (hardness >= 1.f)) {
            return density;
        }

        return density * (1.f - u) / (1.f - hardness);
    }
} // anon namespace

void
rasterizeBrush(const std::vector<BrushPoint>& points,
               const Region& roi,
               std::vector<float>* mask)
{
    mask->assign((std::size_t)std::max(0, roi.width) * (std::size_t)std::max(0, roi.height), 0.f);
    accumulateBrush(points, roi, mask->data(), EdgeAware::serialFor);
} // rasterizeBrush

void
accumulateBrush(const std::vector<BrushPoint>& points,
                const Region& roi,
                float* mask,
                const EdgeAware::ParallelFor& parallel)
{
    if (points.empty() || (roi.width <= 0) || (roi.height <= 0)) {
        return;
    }
    const std::size_t w = (std::size_t)roi.width;
    // A single point is a round stamp; segments carry the parameters linearly between their ends.
    const std::size_t nSegments = points.size() > 1 ? points.size() - 1 : 1;
    const int kRows = 16;
    EdgeAware::forBlocks(parallel, roi.height, kRows, [&](int begin, int end) {
        for (std::size_t s = 0; s < nSegments; ++s) {
            const BrushPoint& a = points[s];
            const BrushPoint& b = points.size() > 1 ? points[s + 1] : points[s];
            const float maxRadius = std::max(a.radius, b.radius);
            const int px1 = std::max(roi.x, (int)std::floor(std::min(a.x, b.x) - maxRadius));
            const int py1 = std::max(roi.y + begin, (int)std::floor(std::min(a.y, b.y) - maxRadius));
            const int px2 = std::min(roi.x + roi.width, (int)std::ceil(std::max(a.x, b.x) + maxRadius) + 1);
            const int py2 = std::min(roi.y + end, (int)std::ceil(std::max(a.y, b.y) + maxRadius) + 1);
            const float ex = b.x - a.x;
            const float ey = b.y - a.y;
            const float len2 = ex * ex + ey * ey;
            for (int py = py1; py < py2; ++py) {
                float* row = mask + (std::size_t)(py - roi.y) * w;
                const float cy = (float)py + 0.5f;
                for (int px = px1; px < px2; ++px) {
                    const float cx = (float)px + 0.5f;
                    float t = 0.f;
                    if (len2 > 0.f) {
                        t = std::clamp(((cx - a.x) * ex + (cy - a.y) * ey) / len2, 0.f, 1.f);
                    }
                    const float qx = a.x + t * ex - cx;
                    const float qy = a.y + t * ey - cy;
                    const float d = std::sqrt(qx * qx + qy * qy);
                    const float radius = a.radius + t * (b.radius - a.radius);
                    const float hardness = a.hardness + t * (b.hardness - a.hardness);
                    const float density = a.density + t * (b.density - a.density);
                    const float v = brushFalloff(d, radius, hardness, density);
                    float& out = row[px - roi.x];
                    out = std::max(out, v);
                }
            }
        }
    });
} // accumulateBrush

void
KeyHash::add(std::uint64_t value)
{
    // splitmix64's finaliser over the running hash and the value.
    std::uint64_t z = _hash ^ (value + 0x9e3779b97f4a7c15ULL + (_hash << 6) + (_hash >> 2));
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    _hash = z ^ (z >> 31);
}

void
KeyHash::addFloat(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    add(bits);
}

void
KeyHash::addRegion(const Region& region)
{
    add((std::uint64_t)(std::uint32_t)region.x | ((std::uint64_t)(std::uint32_t)region.y << 32));
    add((std::uint64_t)(std::uint32_t)region.width | ((std::uint64_t)(std::uint32_t)region.height << 32));
}

void
KeyHash::addBytes(const void* data,
                  std::size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    // A multiply-xor pass over whole words, cheap enough for a full mask on every render.
    std::uint64_t h = 0xcbf29ce484222325ULL ^ size;
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t word;
        std::memcpy(&word, bytes + i, 8);
        h = (h ^ word) * 0x100000001b3ULL;
        h ^= h >> 29;
    }
    for (; i < size; ++i) {
        h = (h ^ bytes[i]) * 0x100000001b3ULL;
    }
    add(h);
}

PatchCache::PatchCache(std::size_t maxBytes)
    : _maxBytes(maxBytes)
{
}

std::size_t
PatchCache::bytesOf(const Patch& patch) const
{
    return sizeof(Patch) + patch.values.size() * sizeof(float);
}

std::shared_ptr<const PatchCache::Patch>
PatchCache::acquire(std::uint64_t key)
{
    std::unique_lock<std::mutex> lock(_mutex);
    for (;;) {
        std::unordered_map<std::uint64_t, Entry>::iterator found = _entries.find(key);
        if (found == _entries.end()) {
            _entries[key] = Entry();
            ++_misses;

            return std::shared_ptr<const Patch>();
        }
        if (!found->second.pending) {
            _recent.splice(_recent.begin(), _recent, found->second.recent);

            return found->second.patch;
        }
        _published.wait(lock);
    }
}

void
PatchCache::publish(std::uint64_t key,
                    const std::shared_ptr<const Patch>& patch)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::unordered_map<std::uint64_t, Entry>::iterator found = _entries.find(key);
        if ((found != _entries.end()) && found->second.pending) {
            const std::size_t bytes = patch ? bytesOf(*patch) : 0;
            if (!patch || (bytes > _maxBytes)) {
                _entries.erase(found);
            } else {
                found->second.patch = patch;
                found->second.pending = false;
                _recent.push_front(key);
                found->second.recent = _recent.begin();
                _bytes += bytes;
                while ((_bytes > _maxBytes) && !_recent.empty()) {
                    std::unordered_map<std::uint64_t, Entry>::iterator oldest = _entries.find(_recent.back());
                    _bytes -= bytesOf(*oldest->second.patch);
                    _recent.pop_back();
                    _entries.erase(oldest);
                }
            }
        }
    }
    _published.notify_all();
}

void
PatchCache::abandon(std::uint64_t key)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::unordered_map<std::uint64_t, Entry>::iterator found = _entries.find(key);
        if ((found != _entries.end()) && found->second.pending) {
            _entries.erase(found);
        }
    }
    _published.notify_all();
}

void
PatchCache::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (std::unordered_map<std::uint64_t, Entry>::iterator it = _entries.begin(); it != _entries.end();) {
        if (it->second.pending) {
            ++it;
        } else {
            it = _entries.erase(it);
        }
    }
    _recent.clear();
    _bytes = 0;
}

std::size_t
PatchCache::misses() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _misses;
}

int
returnLayerForOutput(OutputEnum output,
                     int numScales,
                     int currentScale)
{
    switch (output) {
    case eOutputCurrentScale:
        return std::max(0, std::min(currentScale, numScales + 1));
    case eOutputResidual:
        return numScales + 1;
    case eOutputResult:
    default:
        return 0;
    }
}

bool
shapeScaleProcessed(int shapeScale,
                    int numScales)
{
    return shapeScale >= 0 && shapeScale <= numScales + 1;
}

namespace {
    // The overlap of a and b, which may be empty.
    Region
    intersect(const Region& a,
              const Region& b)
    {
        Region r;
        r.x = std::max(a.x, b.x);
        r.y = std::max(a.y, b.y);
        r.width = std::max(0, std::min(a.x + a.width, b.x + b.width) - r.x);
        r.height = std::max(0, std::min(a.y + a.height, b.y + b.height) - r.y);

        return r;
    }

    bool
    isEmpty(const Region& r)
    {
        return (r.width <= 0) || (r.height <= 0);
    }

    // The bounds of a and b; an empty one adds nothing.
    Region
    unite(const Region& a,
          const Region& b)
    {
        if (isEmpty(a)) {
            return b;
        }
        if (isEmpty(b)) {
            return a;
        }
        Region r;
        r.x = std::min(a.x, b.x);
        r.y = std::min(a.y, b.y);
        r.width = std::max(a.x + a.width, b.x + b.width) - r.x;
        r.height = std::max(a.y + a.height, b.y + b.height) - r.y;

        return r;
    }

    Region
    grow(const Region& r,
         int by)
    {
        Region g = r;
        g.x -= by;
        g.y -= by;
        g.width += 2 * by;
        g.height += 2 * by;

        return g;
    }

    bool
    takesSource(AlgorithmEnum algorithm)
    {
        return (algorithm == eAlgorithmClone) || (algorithm == eAlgorithmHeal);
    }

    /*
     * retouchHeal() on one shape. A draft starts from the shape's warm start where that covers
     * the mask and stops after the draft's cycles; without a warm start it solves in full, as a
     * render that is not a draft always does, so a final render does not depend on the drafts.
     */
    // False for a draft solve.
    bool
    healShape(float* layer,
              const Region& layerRoi,
              const float* mask,
              const Region& maskRoi,
              int dx,
              int dy,
              int scale,
              const Shape& shape,
              const Params& params,
              const EdgeAware::ParallelFor& parallel)
    {
        HealWarmStart* warm = shape.warmStart.get();
        std::vector<float> guess;
        if (warm && (params.draftHealCycles > 0)) {
            std::lock_guard<std::mutex> lock(warm->mutex);
            const Region overlap = intersect(warm->region, maskRoi);
            if ((warm->renderScale == params.renderScale) && (warm->scale == scale) && !isEmpty(overlap)) {
                guess.assign((std::size_t)4 * maskRoi.width * maskRoi.height, std::numeric_limits<float>::quiet_NaN());
                for (int y = overlap.y; y < overlap.y + overlap.height; ++y) {
                    const float* from = &warm->difference[4 * ((std::size_t)(y - warm->region.y) * warm->region.width + (overlap.x - warm->region.x))];
                    std::copy(from, from + 4 * (std::size_t)overlap.width,
                              &guess[4 * ((std::size_t)(y - maskRoi.y) * maskRoi.width + (overlap.x - maskRoi.x))]);
                }
            }
        }
        const int cycles = guess.empty() ? params.maxHealIterations : std::min(params.draftHealCycles, params.maxHealIterations);
        std::vector<float> difference;
        retouchHeal(layer, layerRoi, mask, maskRoi, dx, dy, shape.settings.opacity, cycles, parallel, guess.empty() ? nullptr : guess.data(),
                    warm ? &difference : nullptr);
        if (warm) {
            std::lock_guard<std::mutex> lock(warm->mutex);
            warm->region = maskRoi;
            warm->renderScale = params.renderScale;
            warm->scale = scale;
            warm->difference.swap(difference);
        }

        return guess.empty();
    }
} // anon namespace

Region
processRegion(const Region& window,
              const std::vector<Shape>& shapes,
              const Params& params,
              int fullWidth,
              int fullHeight)
{
    Region full;
    full.width = fullWidth;
    full.height = fullHeight;
    Region region = intersect(window, full);
    if (isEmpty(region)) {
        return region;
    }
    EdgeAware::DwtParams dwt;
    dwt.width = fullWidth;
    dwt.height = fullHeight;
    dwt.previewScale = (params.renderScale > 0.f) ? params.renderScale : 1.f;
    const int scales = std::min(std::max(0, std::min(params.numScales, kMaxScales)), EdgeAware::dwtMaxScale(dwt));
    // buildRegionMask() moves a shape's area by a pixel and grows a heal's by another.
    const int maskReach = 2;
    // The hat filters of scales 1 to scales reach 1 + 2 + ... + 2^(scales - 1) pixels, and so
    // does a crop edge's mirroring, which leaves the layers that far inside it unlike the full
    // image's. A shape reaching into that margin is taken whole, so that every shape processed
    // reads only layers as the full image has them.
    const int margin = maskReach + ((scales > 0) ? (1 << scales) - 1 : 0);
    std::vector<bool> taken(shapes.size(), false);
    bool grew = true;
    while (grew) {
        grew = false;
        const Region reach = grow(region, margin);
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            const Shape& shape = shapes[i];
            if (taken[i] || !shapeScaleProcessed(shape.settings.scale, params.numScales)) {
                continue;
            }
            const Region area = grow(shape.maskRoi, maskReach);
            Region source = area;
            if (takesSource(shape.settings.algorithm)) {
                source.x -= (int)shape.dx;
                source.y -= (int)shape.dy;
            }
            // A shape only reads its source, so a source alone in the region changes none of its
            // pixels. The fixed point ignores shape order because scale-0 shapes run before every
            // band's, whatever their index.
            if (isEmpty(intersect(area, reach))) {
                continue;
            }
            taken[i] = true;
            region = unite(region, intersect(unite(area, source), full));
            grew = true;
        }
    }

    return intersect(grow(region, margin), full);
} // processRegion

namespace {
    // The scale processLayer() applies a shape of shapeScale on, or -1 when it applies it on none.
    int
    layerOfShape(int shapeScale,
                 int scales,
                 int numScales,
                 int returnLayer)
    {
        if ((shapeScale >= 0) && (shapeScale <= scales)) {
            return shapeScale;
        }
        const int residual = ((scales < numScales) && (returnLayer == 0)) ? numScales + 1 : scales + 1;

        return ((scales > 0) && (shapeScale == residual)) ? residual : -1;
    }

    std::uint64_t
    shapeContentKey(const Shape& shape)
    {
        const ShapeSettings& s = shape.settings;
        KeyHash h;
        h.addRegion(shape.maskRoi);
        h.addBytes(shape.mask.data(), shape.mask.size() * sizeof(float));
        h.add((std::uint64_t)s.algorithm);
        h.add((std::uint64_t)(std::int64_t)s.scale);
        h.add((std::uint64_t)s.blurType);
        h.addFloat(s.blurRadius);
        h.add((std::uint64_t)s.fillMode);
        for (float c : s.fillColor) {
            h.addFloat(c);
        }
        h.addFloat(s.fillBrightness);
        h.addFloat(s.opacity);
        h.addFloat(shape.dx);
        h.addFloat(shape.dy);

        return h.value();
    }

    std::vector<std::uint64_t>
    shapeContentKeys(const Params& params,
                     const std::vector<Shape>& shapes)
    {
        std::vector<std::uint64_t> keys;
        if (params.cache && (params.inputKey != 0)) {
            keys.reserve(shapes.size());
            for (const Shape& shape : shapes) {
                keys.push_back(shapeContentKey(shape));
            }
        }

        return keys;
    }

    // buildRegionMask() moves a shape's area by a pixel and grows a heal's by another.
    const int kMaskReach = 2;

    /*
     * The cache key of each shape's result: the input, the layer, the shape, and the keys of the
     * shapes before it whose changes can reach what it reads: on its own layer, through the
     * wavelet support from the image layer, and on the merged layers below it. Areas are clipped
     * to whole, the processed image, so a shape inside it keys alike in every crop.
     */
    class ShapeKeys {
    public:
        ShapeKeys(const Params& params,
                  const std::vector<Shape>& shapes,
                  const std::vector<std::uint64_t>& content,
                  int scales,
                  int returnLayer,
                  int mergeFromScale,
                  const Region& whole)
            : _params(params)
            , _content(content)
            , _scales(scales)
            , _returnLayer(returnLayer)
            , _mergeFromScale(mergeFromScale)
            , _keys(shapes.size(), 0)
        {
            for (const Shape& shape : shapes) {
                _layer.push_back(layerOfShape(shape.settings.scale, scales, params.numScales, returnLayer));
                const Region area = grow(shape.maskRoi, kMaskReach);
                Region read = area;
                if (takesSource(shape.settings.algorithm)) {
                    Region source = area;
                    source.x -= (int)shape.dx;
                    source.y -= (int)shape.dy;
                    read = unite(read, source);
                }
                _written.push_back(intersect(area, whole));
                _read.push_back(intersect(read, whole));
            }
        }

        std::uint64_t of(std::size_t i)
        {
            if (_keys[i] != 0) {
                return _keys[i];
            }
            const int layer = _layer[i];
            const int support = (1 << std::min(layer, _scales)) - 1;
            KeyHash h;
            h.add(_params.inputKey);
            h.add((std::uint64_t)(std::int64_t)layer);
            h.add((std::uint64_t)_scales);
            h.add((std::uint64_t)(std::int64_t)_params.numScales);
            h.add((std::uint64_t)(std::int64_t)_returnLayer);
            h.add((std::uint64_t)(std::int64_t)_mergeFromScale);
            h.addFloat(_params.renderScale);
            h.add((std::uint64_t)(std::int64_t)_params.maxHealIterations);
            h.add(_content[i]);
            h.addRegion(_read[i]);
            for (std::size_t j = 0; j < _keys.size(); ++j) {
                const int other = _layer[j];
                bool reaches = false;
                if ((j == i) || (other < 0)) {
                    continue;
                } else if (other == layer) {
                    reaches = (j < i) && !isEmpty(intersect(_written[j], _read[i]));
                } else if (other == 0) {
                    reaches = !isEmpty(intersect(grow(_written[j], support), _read[i]));
                } else if ((_mergeFromScale > 0) && (other >= _mergeFromScale) && (other < layer) && (layer <= _scales)) {
                    reaches = !isEmpty(intersect(_written[j], _read[i]));
                }
                if (reaches) {
                    h.add(of(j));
                }
            }
            _keys[i] = std::max<std::uint64_t>(h.value(), 1);

            return _keys[i];
        }

    private:
        const Params& _params;
        const std::vector<std::uint64_t>& _content;
        int _scales;
        int _returnLayer;
        int _mergeFromScale;
        std::vector<int> _layer;
        std::vector<Region> _written;
        std::vector<Region> _read;
        std::vector<std::uint64_t> _keys;
    };

    void
    copyArea(const float* from,
             const Region& fromRoi,
             float* to,
             const Region& toRoi,
             const Region& area)
    {
        const std::size_t rowSamples = 4 * (std::size_t)area.width;
        for (int y = area.y; y < area.y + area.height; ++y) {
            const float* src = from + 4 * ((std::size_t)(y - fromRoi.y) * fromRoi.width + (std::size_t)(area.x - fromRoi.x));
            float* dst = to + 4 * ((std::size_t)(y - toRoi.y) * toRoi.width + (std::size_t)(area.x - toRoi.x));
            std::copy(src, src + rowSamples, dst);
        }
    }

    void
    processLayerIn(float* layer,
                   int width,
                   int height,
                   int scale,
                   int scales,
                   int returnLayer,
                   int mergeFromScale,
                   const Params& params,
                   const std::vector<Shape>& shapes,
                   const std::vector<std::uint64_t>& contentKeys,
                   const Region& whole,
                   const EdgeAware::ParallelFor& parallel)
    {
        if ((mergeFromScale == 0) && (returnLayer > 0) && (scale != returnLayer) && (scale != 0)) {
            return;
        }
        if (scale > scales + 1) {
            return;
        }
        if ((scales < params.numScales) && (returnLayer == 0) && (scale == scales + 1)) {
            scale = params.numScales + 1;
        }

        Region layerRoi;
        layerRoi.x = params.originX;
        layerRoi.y = params.originY;
        layerRoi.width = width;
        layerRoi.height = height;
        const bool cached = params.cache && (params.inputKey != 0) && (contentKeys.size() == shapes.size());
        std::unique_ptr<ShapeKeys> keys;
        if (cached) {
            keys.reset(new ShapeKeys(params, shapes, contentKeys, scales, returnLayer, mergeFromScale, whole));
        }
        std::vector<float> mask;
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            const Shape& shape = shapes[i];
            const ShapeSettings& s = shape.settings;
            if (s.scale != scale) {
                continue;
            }
            int dx = 0;
            int dy = 0;
            if ((s.algorithm != eAlgorithmBlur) && (s.algorithm != eAlgorithmFill)) {
                dx = (int)shape.dx;
                dy = (int)shape.dy;
            }
            Region maskRoi;
            if (!buildRegionMask(shape, layerRoi, dx, dy, &mask, &maskRoi)) {
                continue;
            }
            const bool apply = (dx != 0 || dy != 0 || s.algorithm == eAlgorithmBlur || s.algorithm == eAlgorithmFill)
                && maskRoi.width > 2 && maskRoi.height > 2;
            if (!apply) {
                continue;
            }
            std::unique_ptr<CacheClaim> claim;
            if (cached) {
                KeyHash h;
                h.add(keys->of(i));
                h.addRegion(maskRoi);
                const std::shared_ptr<const PatchCache::Patch> hit = params.cache->acquire(h.value());
                if (hit) {
                    copyArea(hit->values.data(), hit->area, layer, layerRoi, maskRoi);
                    continue;
                }
                claim.reset(new CacheClaim(params.cache, h.value()));
            }
            bool storable = true;
            switch (s.algorithm) {
            case eAlgorithmClone:
                retouchClone(layer, layerRoi, mask.data(), maskRoi, dx, dy, s.opacity);
                break;
            case eAlgorithmHeal:
                storable = healShape(layer, layerRoi, mask.data(), maskRoi, dx, dy, scale, shape, params, parallel);
                break;
            case eAlgorithmBlur:
                retouchBlur(layer, layerRoi, mask.data(), maskRoi, s.opacity, s.blurType, s.blurRadius, params.renderScale, parallel);
                break;
            case eAlgorithmFill:
                retouchFill(layer, layerRoi, mask.data(), maskRoi, s.opacity, s.fillMode, s.fillColor, s.fillBrightness);
                break;
            }
            if (claim && storable && !(params.cancelled && params.cancelled())) {
                std::shared_ptr<PatchCache::Patch> patch = std::make_shared<PatchCache::Patch>();
                patch->area = maskRoi;
                patch->values.resize(4 * (std::size_t)maskRoi.width * (std::size_t)maskRoi.height);
                copyArea(layer, layerRoi, patch->values.data(), maskRoi, maskRoi);
                claim->publish(patch);
            }
        }
    } // processLayerIn
} // anon namespace

void
processLayer(float* layer,
             int width,
             int height,
             int scale,
             int scales,
             int returnLayer,
             int mergeFromScale,
             const Params& params,
             const std::vector<Shape>& shapes,
             const EdgeAware::ParallelFor& parallel)
{
    Region whole;
    whole.x = params.originX;
    whole.y = params.originY;
    whole.width = width;
    whole.height = height;
    processLayerIn(layer, width, height, scale, scales, returnLayer, mergeFromScale, params, shapes, shapeContentKeys(params, shapes), whole, parallel);
} // processLayer

namespace {
    /*
     * process() for the result, with no shape on the residual or a merged layer: the image after
     * the scale-0 shapes plus, for each detail scale with shapes, the change those shapes make to
     * the band. A band is decomposed only over its shapes' areas and the band's support around
     * them, where the crop's band equals the full image's, as the shapes read nothing farther out.
     */
    void
    processDetailAreas(float* image,
                       int width,
                       int height,
                       int scales,
                       const Params& params,
                       const std::vector<Shape>& shapes,
                       const std::vector<std::uint64_t>& contentKeys,
                       const EdgeAware::ParallelFor& parallel)
    {
        const std::size_t samples = 4 * (std::size_t)width * (std::size_t)height;
        EdgeAware::NonFiniteSamples nonFinite;
        nonFinite.take(image, samples);
        Region whole;
        whole.x = params.originX;
        whole.y = params.originY;
        whole.width = width;
        whole.height = height;
        processLayerIn(image, width, height, 0, scales, 0, params.mergeFromScale, params, shapes, contentKeys, whole, parallel);

        struct Change {
            Region area;
            std::vector<float> delta;
        };
        std::vector<Change> changes;
        for (int scale = 1; scale <= scales; ++scale) {
            Region reach;
            for (const Shape& shape : shapes) {
                const ShapeSettings& s = shape.settings;
                if (s.scale != scale) {
                    continue;
                }
                const Region area = grow(shape.maskRoi, kMaskReach);
                reach = unite(reach, area);
                if (takesSource(s.algorithm)) {
                    Region source = area;
                    source.x -= (int)shape.dx;
                    source.y -= (int)shape.dy;
                    reach = unite(reach, source);
                }
            }
            reach = intersect(reach, whole);
            if (isEmpty(reach)) {
                continue;
            }
            const Region crop = intersect(grow(reach, (1 << scale) - 1), whole);
            const std::size_t rowSamples = 4 * (std::size_t)crop.width;
            const std::size_t cropSamples = rowSamples * (std::size_t)crop.height;
            std::vector<float> input(cropSamples);
            for (int y = 0; y < crop.height; ++y) {
                const float* from = image + 4 * ((std::size_t)(crop.y - whole.y + y) * width + (std::size_t)(crop.x - whole.x));
                std::copy(from, from + rowSamples, &input[rowSamples * y]);
            }
            std::vector<float> band(cropSamples);
            EdgeAware::dwtDetailBand(input.data(), crop.width, crop.height, scale, band.data(), parallel);
            std::vector<float>& edited = input;
            edited = band;
            Params local = params;
            local.originX = crop.x;
            local.originY = crop.y;
            processLayerIn(edited.data(), crop.width, crop.height, scale, scales, 0, params.mergeFromScale, local, shapes, contentKeys, whole, parallel);
            for (std::size_t i = 0; i < cropSamples; ++i) {
                edited[i] -= band[i];
            }
            changes.push_back({ crop, std::move(edited) });
        }
        // Every band comes from the image before any band's change.
        for (const Change& change : changes) {
            const Region& crop = change.area;
            const std::size_t rowSamples = 4 * (std::size_t)crop.width;
            for (int y = 0; y < crop.height; ++y) {
                float* to = image + 4 * ((std::size_t)(crop.y - whole.y + y) * width + (std::size_t)(crop.x - whole.x));
                const float* delta = &change.delta[rowSamples * y];
                for (std::size_t i = 0; i < rowSamples; ++i) {
                    to[i] += delta[i];
                }
            }
        }
        nonFinite.restore(image);
    } // processDetailAreas
} // anon namespace

void
process(float* image,
        int width,
        int height,
        const Params& params,
        const std::vector<Shape>& shapes,
        const EdgeAware::ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    EdgeAware::DwtParams dwt;
    dwt.width = width;
    dwt.height = height;
    dwt.capWidth = params.fullWidth;
    dwt.capHeight = params.fullHeight;
    dwt.scales = std::max(0, std::min(params.numScales, kMaxScales));
    dwt.returnLayer = params.returnLayer;
    dwt.mergeFromScale = params.mergeFromScale;
    dwt.previewScale = params.renderScale;

    const std::vector<std::uint64_t> contentKeys = shapeContentKeys(params, shapes);
    if (params.returnLayer == 0) {
        EdgeAware::DwtParams capped = dwt;
        if (capped.previewScale <= 0.f) {
            capped.previewScale = 1.f;
        }
        const int scales = std::min(capped.scales, EdgeAware::dwtMaxScale(capped));
        // As processLayer() remaps it when the image caps the scales.
        const int residualScale = (scales < params.numScales) ? params.numScales + 1 : scales + 1;
        bool onDetails = false;
        bool onResidual = false;
        for (const Shape& shape : shapes) {
            const int s = shape.settings.scale;
            onDetails = onDetails || ((s >= 1) && (s <= scales));
            onResidual = onResidual || ((scales > 0) && (s == residualScale));
        }
        if (!onResidual && (!onDetails || (params.mergeFromScale == 0))) {
            processDetailAreas(image, width, height, scales, params, shapes, contentKeys, parallel);

            return;
        }
    }

    Region whole;
    whole.x = params.originX;
    whole.y = params.originY;
    whole.width = width;
    whole.height = height;
    const EdgeAware::DwtLayerFn layerFn = [&](float* layer, const EdgeAware::DwtParams& p, int scale) {
        processLayerIn(layer, p.width, p.height, scale, p.scales, p.returnLayer, p.mergeFromScale, params, shapes, contentKeys, whole, parallel);
    };
    EdgeAware::dwtDecompose(image, dwt, layerFn, parallel);

    if ((dwt.returnLayer > 0) && (dwt.returnLayer < dwt.scales + 1)) {
        adjustLevels(image, width, height, params.previewLevels, parallel);
    }
}
} // namespace RetouchCore

NATRON_NAMESPACE_EXIT
