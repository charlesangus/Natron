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

#ifndef Engine_DeepImage_h
#define Engine_DeepImage_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Per-pixel sample counts and their prefix-sum offsets into a DeepImage's channel
 * buffers, mirroring OpenEXR's deep scanline layout. Building one is two-pass: set every
 * pixel's sample count (in any order, any thread), then call recomputeOffsets() once to turn
 * those counts into a prefix sum and learn the total sample count needed to allocate channel
 * buffers. A SampleTable is shared copy-on-write between DeepImages that have not changed the
 * sample structure (i.e. any op that only rewrites channel values, not sample counts).
 **/
class SampleTable {
public:
    explicit SampleTable(std::size_t pixelCount)
        : _counts(pixelCount, 0)
        , _offsets(pixelCount, 0)
        , _totalSampleCount(0)
    {
    }

    std::size_t getPixelCount() const
    {
        return _counts.size();
    }

    U32 getCount(std::size_t pixelIndex) const
    {
        return _counts.at(pixelIndex);
    }

    U64 getOffset(std::size_t pixelIndex) const
    {
        return _offsets.at(pixelIndex);
    }

    const std::vector<U32>& getCounts() const
    {
        return _counts;
    }

    const std::vector<U64>& getOffsets() const
    {
        return _offsets;
    }

    U64 getTotalSampleCount() const
    {
        return _totalSampleCount;
    }

    void setCount(std::size_t pixelIndex, U32 count)
    {
        _counts.at(pixelIndex) = count;
    }

    // Rebuilds offsets as the exclusive prefix sum of counts and refreshes the total sample
    // count. Must be called after setCount() and before the offsets/total are read.
    void recomputeOffsets();

    std::size_t getSizeInBytes() const
    {
        return _counts.size() * sizeof(U32) + _offsets.size() * sizeof(U64);
    }

private:
    std::vector<U32> _counts;
    std::vector<U64> _offsets;
    U64 _totalSampleCount;
};

/**
 * @brief A copy-on-write handle to one channel's contiguous float sample array (indexed by a
 * DeepImage's SampleTable offsets). Copying a DeepChannelBuffer is cheap and aliases the same
 * storage; dataForWriting() detaches (deep-copies) only if that storage is currently shared, so
 * a DeepImage produced by copying another one and calling getChannelForWriting() on a single
 * channel name shares every other channel's storage with its source until something else also
 * writes to them.
 **/
class DeepChannelBuffer {
public:
    DeepChannelBuffer() = default;

    std::size_t size() const
    {
        return _data ? _data->size() : 0;
    }

    bool empty() const
    {
        return size() == 0;
    }

    const float* data() const
    {
        return _data ? _data->data() : nullptr;
    }

    // Replaces this buffer with a freshly-allocated, zero-filled, uniquely-owned array of
    // sampleCount floats. Used to create a channel that did not exist yet.
    void allocate(std::size_t sampleCount);

    // Returns a writable pointer to this buffer's storage, copying it first if anything else
    // (another DeepImage's copy of this same channel) currently shares it. Returns nullptr if
    // the buffer has never been allocated -- callers must allocate() first.
    float* dataForWriting();

    bool sharesStorageWith(const DeepChannelBuffer& other) const
    {
        return _data && (_data.get() == other._data.get());
    }

    // True when nothing else (no other DeepImage's copy of this channel) currently references
    // this buffer's storage. Used by DeepImage::getUniquelyOwnedSizeInBytes() for diagnostics;
    // not by getSizeInBytes(), which must stay aliasing-blind (see DeepImage's class comment).
    bool isUniquelyOwned() const
    {
        return _data && (_data.use_count() == 1);
    }

    std::size_t getSizeInBytes() const
    {
        return size() * sizeof(float);
    }

private:
    std::shared_ptr<std::vector<float>> _data;
};

/**
 * @brief A deep pixel payload: a rectangle of pixels, each holding zero or more depth samples,
 * structured the way OpenEXR's deep layout is structured -- one shared SampleTable of
 * per-pixel counts and prefix-sum offsets, and a set of named per-channel contiguous float
 * arrays ("R", "G", "B", "A", "Z", "ZBack", and arbitrary AOVs) indexed by those offsets.
 *
 * DeepImage is copy-on-write at two levels: copying a DeepImage is a shallow copy (the
 * SampleTable and every DeepChannelBuffer alias their sources), and getSampleTableForWriting()
 * / DeepChannelBuffer::dataForWriting() each detach only the one part being mutated. This is
 * what lets a color-only op (e.g. a grade) copy an input DeepImage, call
 * getChannelForWriting("R")/("G")/("B"), and leave "A", "Z" and "ZBack" -- and the sample
 * table itself, since a color-only op does not change which samples exist -- aliased with the
 * input for the lifetime of both.
 *
 * Per-pixel tidiness (samples within a pixel sorted by depth and non-overlapping) is recorded
 * as a flag set by whichever node produced this DeepImage, not enforced or computed here:
 * tidying samples that are not already tidy is later work (DeepMerge and flatten's job).
 *
 * getSizeInBytes() sums the sample table and every channel buffer unconditionally, regardless of
 * what else aliases their storage, so it depends only on this DeepImage's declared shape (bounds
 * and which channels exist) and not on how many other DeepImages currently share it. This is
 * required by Cache: a cache entry's cost is recorded once at insert and re-queried once at
 * destroy to reverse that same bookkeeping, so the two calls must agree even though a cached
 * DeepImage is handed out live and its buffers' use counts rise and fall as renders reference it
 * in between. The tradeoff is over-counting COW-shared storage across distinct cache entries,
 * which is the safe direction to err: it evicts early rather than never evicting.
 * getUniquelyOwnedSizeInBytes() reports the aliasing-aware figure instead, for diagnostics only.
 **/
class DeepImage {
public:
    DeepImage(const RectI& bounds,
              const RenderScale& scale,
              ViewIdx view);

    const RectI& getBounds() const
    {
        return _bounds;
    }

    const RenderScale& getRenderScale() const
    {
        return _scale;
    }

    ViewIdx getView() const
    {
        return _view;
    }

    std::size_t getPixelCount() const
    {
        return _sampleTable->getPixelCount();
    }

    const SampleTable& getSampleTable() const
    {
        return *_sampleTable;
    }

    // Detaches (copies) the sample table first if anything else currently shares it.
    SampleTable& getSampleTableForWriting();

    bool sharesSampleTableWith(const DeepImage& other) const
    {
        return _sampleTable.get() == other._sampleTable.get();
    }

    bool hasChannel(const std::string& name) const
    {
        return _channels.find(name) != _channels.end();
    }

    const std::map<std::string, DeepChannelBuffer>& getChannels() const
    {
        return _channels;
    }

    // Returns nullptr if no channel by that name exists.
    const DeepChannelBuffer* getChannel(const std::string& name) const
    {
        std::map<std::string, DeepChannelBuffer>::const_iterator it = _channels.find(name);

        return (it == _channels.end()) ? nullptr : &it->second;
    }

    // Returns a channel ready to be written to: a newly-allocated, zero-filled,
    // uniquely-owned buffer sized to the sample table's total sample count if the channel did
    // not exist yet, or the existing buffer detached (copied) if it was shared.
    DeepChannelBuffer& getChannelForWriting(const std::string& name);

    // Aliases (or replaces) a channel with an existing buffer as-is, e.g. to explicitly share
    // one input's channel on a node combining several deep inputs.
    void setChannel(const std::string& name, const DeepChannelBuffer& buffer)
    {
        _channels[name] = buffer;
    }

    void removeChannel(const std::string& name)
    {
        _channels.erase(name);
    }

    bool sharesChannelStorageWith(const DeepImage& other, const std::string& name) const
    {
        const DeepChannelBuffer* mine = getChannel(name);
        const DeepChannelBuffer* theirs = other.getChannel(name);

        return mine && theirs && mine->sharesStorageWith(*theirs);
    }

    bool isTidy() const
    {
        return _tidy;
    }

    void setTidy(bool tidy)
    {
        _tidy = tidy;
    }

    std::size_t getSizeInBytes() const;

    std::size_t getUniquelyOwnedSizeInBytes() const;

private:
    RectI _bounds;
    RenderScale _scale;
    ViewIdx _view;
    SampleTablePtr _sampleTable;
    std::map<std::string, DeepChannelBuffer> _channels;
    bool _tidy;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_DeepImage_h
