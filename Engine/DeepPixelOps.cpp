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

#include "DeepPixelOps.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace {

// Returns 1 - (1 - alpha)^fraction for fraction in [0, 1], i.e. the alpha accumulated over the
// leading `fraction` portion of a volumetric sample's depth extent (OpenEXR "Interpreting Deep
// Pixels", the split-a-volume-sample formula). Evaluated as -expm1(fraction * log1p(-alpha))
// rather than 1 - pow(1 - alpha, fraction): forming (1 - alpha) directly loses precision for
// small alpha (log1p(-alpha) does not), and forming 1 - (something close to 1) loses precision
// for small results (expm1 does not). fraction == 0 or 1, and alpha <= 0 or >= 1, are handled
// directly to avoid a 0 * -inf indeterminate form when alpha == 1 makes log1p(-alpha) == -inf.
float
volumeAlphaOverFraction(float alpha,
                        float fraction)
{
    if (fraction <= 0.f) {
        return 0.f;
    }
    if (fraction >= 1.f) {
        return alpha;
    }
    if (alpha <= 0.f) {
        return 0.f;
    }
    if (alpha >= 1.f) {
        return 1.f;
    }

    return -std::expm1(fraction * std::log1p(-(double)alpha));
}

void
copySample(const DeepPixelView& src,
           int srcIndex,
           const MutableDeepPixelView& dst,
           int dstIndex)
{
    dst.z[dstIndex] = src.z[srcIndex];
    dst.zback[dstIndex] = src.zbackAt(srcIndex);
    for (int c = 0; c < src.numChannels; ++c) {
        dst.channels[c][dstIndex] = src.channels[c][srcIndex];
    }
}

bool
sameDepthRange(const DeepPixelView& pixel,
               int a,
               int b)
{
    return (pixel.z[a] == pixel.z[b]) && (pixel.zbackAt(a) == pixel.zbackAt(b));
}

// Number of tidy boundaries falling strictly inside a sample's extent, i.e. the number of cuts
// tidySamples() has to make in it.
int
countInteriorCuts(const std::vector<float>& boundaries,
                  float z,
                  float zback)
{
    const std::vector<float>::const_iterator first = std::upper_bound(boundaries.begin(), boundaries.end(), z);
    const std::vector<float>::const_iterator last = std::lower_bound(boundaries.begin(), boundaries.end(), zback);

    return (last > first) ? (int)(last - first) : 0;
}

} // namespace

void
DeepPixelScratch::reset(int numChannels,
                        int alphaChannelIndex)
{
    _alphaChannelIndex = alphaChannelIndex;
    _numSamples = 0;
    if (numChannels != _numChannels) {
        _numChannels = numChannels;
        rebuildStorage();
    }
}

void
DeepPixelScratch::setSampleCount(int numSamples)
{
    if (numSamples > _capacity) {
        _capacity = std::max(numSamples, 2 * _capacity);
        rebuildStorage();
    }
    _numSamples = numSamples;
}

void
DeepPixelScratch::rebuildStorage()
{
    _storage.resize((std::size_t)(2 + _numChannels) * (std::size_t)_capacity);
    _channelPtrs.resize(_numChannels);
    for (int c = 0; c < _numChannels; ++c) {
        _channelPtrs[c] = _storage.data() + (std::size_t)(2 + c) * (std::size_t)_capacity;
    }
}

MutableDeepPixelView
DeepPixelScratch::view()
{
    MutableDeepPixelView out;

    out.z = z();
    out.zback = zback();
    out.channels = _channelPtrs.data();
    out.numChannels = _numChannels;
    out.alphaChannelIndex = _alphaChannelIndex;
    out.numSamples = _numSamples;

    return out;
}

DeepPixelView
DeepPixelScratch::view() const
{
    DeepPixelView out;

    out.z = z();
    out.zback = zback();
    out.channels = _channelPtrs.data();
    out.numChannels = _numChannels;
    out.alphaChannelIndex = _alphaChannelIndex;
    out.numSamples = _numSamples;

    return out;
}

namespace DeepPixelOps {

void
sortSamplesByDepth(const DeepPixelView& pixel,
                   std::vector<int>& order)
{
    order.resize(pixel.numSamples);
    for (int i = 0; i < pixel.numSamples; ++i) {
        order[i] = i;
    }

    std::stable_sort(order.begin(), order.end(), [&pixel](int a, int b) {
        if (pixel.z[a] != pixel.z[b]) {
            return pixel.z[a] < pixel.z[b];
        }
        // Tie-break: a point sample sorts before a volumetric sample starting at the same depth.
        // Otherwise, preserve input order (the stable sort makes that possible).
        return pixel.isPointSample(a) && !pixel.isPointSample(b);
    });
}

void
splitVolumeSample(const DeepPixelView& pixel,
                  int sampleIndex,
                  float depth,
                  const MutableDeepPixelView& out,
                  int frontIndex,
                  int backIndex)
{
    const float front = pixel.z[sampleIndex];
    const float back = pixel.zbackAt(sampleIndex);
    const float extent = back - front;
    const float fraction = (extent > 0.f) ? ((depth - front) / extent) : 0.f;
    const float alpha = pixel.channels[pixel.alphaChannelIndex][sampleIndex];
    const float alphaFront = volumeAlphaOverFraction(alpha, fraction);
    const float alphaBack = volumeAlphaOverFraction(alpha, 1.f - fraction);
    const float scaleFront = (alpha > 0.f) ? (alphaFront / alpha) : 0.f;
    const float scaleBack = (alpha > 0.f) ? (alphaBack / alpha) : 0.f;

    out.z[frontIndex] = front;
    out.zback[frontIndex] = depth;
    out.z[backIndex] = depth;
    out.zback[backIndex] = back;
    for (int c = 0; c < pixel.numChannels; ++c) {
        // Read the source value before writing either piece: out is allowed to alias pixel with
        // frontIndex == sampleIndex.
        const float value = pixel.channels[c][sampleIndex];
        out.channels[c][frontIndex] = value * scaleFront;
        out.channels[c][backIndex] = value * scaleBack;
    }
}

void
mergeCoincidentSamples(const DeepPixelView& pixel,
                       int aIndex,
                       int bIndex,
                       const MutableDeepPixelView& out,
                       int outIndex)
{
    const float alphaA = pixel.channels[pixel.alphaChannelIndex][aIndex];
    const float alphaB = pixel.channels[pixel.alphaChannelIndex][bIndex];
    const float z = pixel.z[aIndex];
    const float zback = pixel.zbackAt(aIndex);

    out.z[outIndex] = z;
    out.zback[outIndex] = zback;
    for (int c = 0; c < pixel.numChannels; ++c) {
        out.channels[c][outIndex] = pixel.channels[c][aIndex] * (1.f - 0.5f * alphaB) + pixel.channels[c][bIndex] * (1.f - 0.5f * alphaA);
    }
}

void
tidySamples(const DeepPixelView& pixel,
            DeepPixelScratch* out,
            DeepTidyWorkspace* work)
{
    const int numSamples = pixel.numSamples;

    out->reset(pixel.numChannels, pixel.alphaChannelIndex);
    work->fragments.reset(pixel.numChannels, pixel.alphaChannelIndex);
    if (numSamples <= 0) {
        out->setSampleCount(0);

        return;
    }

    work->boundaries.clear();
    work->boundaries.reserve((std::size_t)numSamples * 2);
    for (int s = 0; s < numSamples; ++s) {
        work->boundaries.push_back(pixel.z[s]);
        work->boundaries.push_back(pixel.zbackAt(s));
    }
    std::sort(work->boundaries.begin(), work->boundaries.end());
    work->boundaries.erase(std::unique(work->boundaries.begin(), work->boundaries.end()), work->boundaries.end());

    sortSamplesByDepth(pixel, work->order);

    int fragmentCount = 0;
    for (int s = 0; s < numSamples; ++s) {
        fragmentCount += pixel.isPointSample(s) ? 1 : (1 + countInteriorCuts(work->boundaries, pixel.z[s], pixel.zbackAt(s)));
    }
    work->fragments.setSampleCount(fragmentCount);

    const MutableDeepPixelView fragments = work->fragments.view();
    int fragmentIndex = 0;
    for (int i = 0; i < numSamples; ++i) {
        const int s = work->order[i];

        copySample(pixel, s, fragments, fragmentIndex);
        if (!pixel.isPointSample(s)) {
            const float zback = pixel.zbackAt(s);
            const std::vector<float>::const_iterator end = std::lower_bound(work->boundaries.begin(), work->boundaries.end(), zback);
            for (std::vector<float>::const_iterator it = std::upper_bound(work->boundaries.begin(), work->boundaries.end(), pixel.z[s]); it != end; ++it) {
                // The remainder is split in place: its front piece stays where it is and the
                // rest of it moves on to the next slot.
                splitVolumeSample(fragments, fragmentIndex, *it, fragments, fragmentIndex, fragmentIndex + 1);
                ++fragmentIndex;
            }
        }
        ++fragmentIndex;
    }

    const DeepPixelView sortedFragments = work->fragments.view();
    sortSamplesByDepth(sortedFragments, work->order);

    int tidyCount = 0;
    for (int i = 0; i < fragmentCount;) {
        int j = i + 1;
        while ((j < fragmentCount) && sameDepthRange(sortedFragments, work->order[j], work->order[i])) {
            ++j;
        }
        ++tidyCount;
        i = j;
    }

    out->setSampleCount(tidyCount);

    const MutableDeepPixelView tidy = out->view();
    int tidyIndex = 0;
    for (int i = 0; i < fragmentCount;) {
        const int a = work->order[i];
        int j = i + 1;
        while ((j < fragmentCount) && sameDepthRange(sortedFragments, work->order[j], a)) {
            mergeCoincidentSamples(sortedFragments, a, work->order[j], fragments, a);
            ++j;
        }
        copySample(sortedFragments, a, tidy, tidyIndex);
        ++tidyIndex;
        i = j;
    }
}

void
flattenFrontToBack(const DeepPixelView& tidyPixel,
                   float* outChannels)
{
    // Transmittance is a running product over samples, but the per-channel accumulations want
    // contiguous inner loops, so the prefix has to be materialized. A fixed block keeps that off
    // the heap while still amortizing the per-channel loop overhead.
    const int kBlockSize = 64;
    float transmittances[kBlockSize];
    const float* alphas = tidyPixel.channels ? tidyPixel.channels[tidyPixel.alphaChannelIndex] : nullptr;
    float transmittance = 1.f;

    for (int c = 0; c < tidyPixel.numChannels; ++c) {
        outChannels[c] = 0.f;
    }

    for (int first = 0; first < tidyPixel.numSamples; first += kBlockSize) {
        const int count = std::min(kBlockSize, tidyPixel.numSamples - first);

        for (int s = 0; s < count; ++s) {
            transmittances[s] = transmittance;
            transmittance *= (1.f - alphas[first + s]);
        }

        for (int c = 0; c < tidyPixel.numChannels; ++c) {
            const float* values = tidyPixel.channels[c] + first;
            float accumulated = 0.f;
            for (int s = 0; s < count; ++s) {
                accumulated += transmittances[s] * values[s];
            }
            outChannels[c] += accumulated;
        }
    }
}

DeepSample
getSample(const DeepPixelView& pixel,
          int sampleIndex)
{
    std::vector<float> channels(pixel.numChannels);

    for (int c = 0; c < pixel.numChannels; ++c) {
        channels[c] = pixel.channels[c][sampleIndex];
    }

    return DeepSample(pixel.z[sampleIndex], pixel.zbackAt(sampleIndex), std::move(channels));
}

} // namespace DeepPixelOps

NATRON_NAMESPACE_EXIT
