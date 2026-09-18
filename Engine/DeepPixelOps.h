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

#ifndef Engine_DeepPixelOps_h
#define Engine_DeepPixelOps_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Non-owning structure-of-channels view of one pixel's deep samples, laid out the way a
 * DeepImage lays them out: for a pixel whose sample table gives offset o and count n, z is the
 * "Z" channel's data + o, zback is the "ZBack" channel's data + o, and channels[c] is the c-th
 * channel's data + o, each n contiguous floats. A caller reading an already-tidy DeepImage
 * therefore builds one of these and copies nothing.
 *
 * zback is nullptr when the source has no "ZBack" channel at all, i.e. every sample is a point
 * sample; use zbackAt() rather than indexing zback directly.
 *
 * Channel order is the caller's and fixed for a whole view (e.g. 0 = R, 1 = G, 2 = B, 3 = A,
 * 4 = an AOV); alphaChannelIndex says which entry is alpha, and lives in the view so that split,
 * merge and flatten cannot disagree about it. Every channel value is premultiplied by its own
 * sample's alpha, matching OpenEXR deep conventions -- that is what lets splitVolumeSample() and
 * mergeCoincidentSamples() scale every channel, alpha included, by one uniform factor instead of
 * special-casing alpha.
 **/
struct DeepPixelView {
    const float* z = nullptr;
    const float* zback = nullptr;
    const float* const* channels = nullptr;
    int numChannels = 0;
    int alphaChannelIndex = 0;
    int numSamples = 0;

    float zbackAt(int s) const
    {
        return zback ? zback[s] : z[s];
    }

    // A sample whose zback does not exceed its z is a point sample (OpenEXR's zero-thickness
    // hard-surface case); zback > z is a volumetric sample, a segment of constant density
    // between the two depths, per "Interpreting Deep Pixels".
    bool isPointSample(int s) const
    {
        return zbackAt(s) <= z[s];
    }
};

/**
 * @brief Writable counterpart of DeepPixelView, used for the destination of the ops below. Its
 * zback array must exist: an op that writes a destination sample writes both of its depths.
 **/
struct MutableDeepPixelView {
    float* z = nullptr;
    float* zback = nullptr;
    float* const* channels = nullptr;
    int numChannels = 0;
    int alphaChannelIndex = 0;
    int numSamples = 0;

    operator DeepPixelView() const
    {
        DeepPixelView view;

        view.z = z;
        view.zback = zback;
        view.channels = channels;
        view.numChannels = numChannels;
        view.alphaChannelIndex = alphaChannelIndex;
        view.numSamples = numSamples;

        return view;
    }
};

/**
 * @brief Caller-owned, reusable storage for one pixel's samples: a single heap block of
 * (2 + numChannels) * capacity floats laid out channel-major ([z | zback | ch0 | ch1 | ...]) so
 * that every channel stays contiguous and view() is just pointers into it. Capacity only ever
 * grows, so a node holding one of these per render thread and calling setSampleCount() once per
 * pixel allocates nothing in steady state.
 *
 * setSampleCount() sizes the block for the pixel about to be written; it does not append, and
 * sample data is not preserved across a capacity change.
 **/
class DeepPixelScratch {
public:
    DeepPixelScratch() = default;

    // Non-copyable: view() and channel() hand out pointers into _storage, which a copy or move
    // would leave pointing at the source's block.
    DeepPixelScratch(const DeepPixelScratch&) = delete;
    DeepPixelScratch& operator=(const DeepPixelScratch&) = delete;

    void reset(int numChannels, int alphaChannelIndex);

    void setSampleCount(int numSamples);

    int getSampleCount() const
    {
        return _numSamples;
    }

    int getNumChannels() const
    {
        return _numChannels;
    }

    int getCapacity() const
    {
        return _capacity;
    }

    float* z()
    {
        return _storage.data();
    }

    float* zback()
    {
        return _storage.data() + _capacity;
    }

    float* channel(int c)
    {
        return _channelPtrs[c];
    }

    const float* z() const
    {
        return _storage.data();
    }

    const float* zback() const
    {
        return _storage.data() + _capacity;
    }

    const float* channel(int c) const
    {
        return _channelPtrs[c];
    }

    MutableDeepPixelView view();

    DeepPixelView view() const;

private:
    void rebuildStorage();

    std::vector<float> _storage;
    std::vector<float*> _channelPtrs;
    int _numChannels = 0;
    int _alphaChannelIndex = 0;
    int _numSamples = 0;
    int _capacity = 0;
};

/**
 * @brief Reusable working state for tidySamples(), with the same lifetime rules as
 * DeepPixelScratch: one per render thread, reused across pixels, growing only.
 **/
struct DeepTidyWorkspace {
    std::vector<int> order;
    std::vector<float> boundaries;
    DeepPixelScratch fragments;
};

/**
 * @brief One deep sample materialized as its own object, for the pixel probe and tests where a
 * single allocation per sample is irrelevant. The ops below never take one; they work on views.
 **/
struct DeepSample {
    float z;
    float zback;
    std::vector<float> channels;

    DeepSample()
        : z(0.f)
        , zback(0.f)
        , channels()
    {
    }

    DeepSample(float z_,
               float zback_,
               std::vector<float> channels_)
        : z(z_)
        , zback(zback_)
        , channels(std::move(channels_))
    {
    }

    bool isPointSample() const
    {
        return zback <= z;
    }
};

namespace DeepPixelOps {

/**
 * @brief Fills order with pixel's sample indices in front-to-back order, leaving the sample data
 * where it is. Ties (equal z) put point samples before volumetric samples starting at the same
 * depth, and otherwise preserve input order (a stable sort): the OpenEXR document does not
 * mandate a tie-break, so this one is Natron's convention, not the document's.
 **/
void sortSamplesByDepth(const DeepPixelView& pixel,
                        std::vector<int>& order);

/**
 * @brief Splits one volumetric sample of pixel into two consecutive sub-samples at an absolute
 * depth strictly between its z and zback, writing them to slots frontIndex and backIndex of out,
 * such that flattening the two pieces in front-to-back order reproduces the original sample
 * exactly. Behavior is undefined if the sample is a point sample or depth is outside its extent;
 * callers only ever split volumetric samples at interior boundaries.
 *
 * Every channel, alpha included, is split by the same ratio (see the .cpp for the derivation),
 * which is what makes treating alpha as just another premultiplied channel correct.
 *
 * out may alias pixel with frontIndex == sampleIndex, which is how tidySamples() consumes a
 * volumetric sample cut by cut without a second buffer.
 **/
void splitVolumeSample(const DeepPixelView& pixel,
                       int sampleIndex,
                       float depth,
                       const MutableDeepPixelView& out,
                       int frontIndex,
                       int backIndex);

/**
 * @brief Combines two samples of pixel that occupy the exact same depth range (equal z and equal
 * zback, whether that range is a point or a volumetric segment) into one sample covering that
 * same range in slot outIndex of out, under Hillman's assumption that the two samples' contents
 * are randomly interspersed within it -- neither is known to be in front of the other, so this is
 * not the Porter-Duff "over" operator, which requires a front/back order.
 *
 * Every channel c (alpha included, by the same reasoning as splitVolumeSample()) combines as
 * `cab = cA * (1 - aB / 2) + cB * (1 - aA / 2)`, symmetric in A and B by construction, so the
 * result is independent of which of the two is aIndex -- the "volumetric merge commutativity" the
 * document guarantees for a single pair. It is not in general associative for three or more
 * overlapping samples merged pairwise in different orders (the pairwise formula's 50/50 tie-break
 * does not generalize exactly to an N-sample simultaneous overlap); tidySamples() merges
 * left-to-right within a group of aligned samples for a deterministic result.
 *
 * out may alias pixel with outIndex == aIndex, which is how tidySamples() accumulates a group.
 **/
void mergeCoincidentSamples(const DeepPixelView& pixel,
                            int aIndex,
                            int bIndex,
                            const MutableDeepPixelView& out,
                            int outIndex);

/**
 * @brief Writes into out the tidy (sorted, non-overlapping) equivalent of pixel: sorts
 * front-to-back, splits volumetric samples at every depth boundary introduced by another sample
 * overlapping them so all overlapping ranges become identically bounded, then merges each group
 * of identically bounded samples with mergeCoincidentSamples(). The result composites, via
 * flattenFrontToBack(), to the same result as pixel does in front-to-back order.
 *
 * out is resized as needed and takes pixel's channel count and alpha index. work is caller-owned
 * so that a render thread pays for its intermediates once rather than once per pixel.
 **/
void tidySamples(const DeepPixelView& pixel,
                 DeepPixelScratch* out,
                 DeepTidyWorkspace* work);

/**
 * @brief Composites an already-tidy (sorted, non-overlapping) pixel front-to-back into the
 * numChannels floats at outChannels, which are overwritten rather than accumulated into and may
 * point straight into a destination Image row. Uses the standard Porter-Duff "over" accumulation
 * generalized to deep's premultiplied channels: starting from full transmittance, each sample in
 * order contributes `channel * transmittance-so-far` and reduces transmittance by
 * `(1 - that sample's alpha)`. A pixel that is not tidy silently gives a wrong answer -- call
 * tidySamples() first unless the caller already knows it is tidy.
 **/
void flattenFrontToBack(const DeepPixelView& tidyPixel,
                        float* outChannels);

/**
 * @brief Copies one sample out of a view into its own object. For the pixel probe and tests only.
 **/
DeepSample getSample(const DeepPixelView& pixel,
                     int sampleIndex);

} // namespace DeepPixelOps

NATRON_NAMESPACE_EXIT

#endif // Engine_DeepPixelOps_h
