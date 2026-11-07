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

#include "DeepImageCacheEntry.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Engine/CacheDiskTier.h"

NATRON_NAMESPACE_ENTER

namespace {

// A larger table would be a forged file: no render names that many channels.
const std::uint32_t kMaxDiskChannels = 4096;
const std::uint32_t kMaxDiskChannelName = 4096;

// In pieces, so that a retraction or shutdown stops a large image between them.
bool
writeInChunks(CacheDiskSink& sink,
              const void* data,
              std::uint64_t bytes)
{
    const char* at = static_cast<const char*>(data);

    while (bytes > 0) {
        const std::size_t chunk = (std::size_t)std::min<std::uint64_t>(bytes, CacheDiskTier::kChunkBytes);
        if (!sink.write(at, chunk)) {
            return false;
        }
        at += chunk;
        bytes -= chunk;
    }

    return true;
}

bool
readInChunks(CacheDiskSource& source,
             void* data,
             std::uint64_t bytes)
{
    char* at = static_cast<char*>(data);

    while (bytes > 0) {
        const std::size_t chunk = (std::size_t)std::min<std::uint64_t>(bytes, CacheDiskTier::kChunkBytes);
        if (!source.read(at, chunk)) {
            return false;
        }
        at += chunk;
        bytes -= chunk;
    }

    return true;
}

} // anon namespace

DeepImageCacheEntry::DeepImageCacheEntry(const DeepImageKey& key,
                                         const DeepImageParamsPtr& params,
                                         const CacheAPI* cache)
    : CacheEntryHelper<char, DeepImageKey, DeepImageParams>(key, params, cache)
    , _header(std::make_shared<CacheEntryHeader>())
    , _deepImage(std::make_shared<DeepImage>(params->getBounds(), key.getRenderScale(), key.getView()))
    , _accountedSize(0)
    , _sizeAccounted(false)
{
    _header->key = std::make_shared<const DeepImageKey>(getKey());
    _header->bounds = params->getBounds();
}

DeepImageCacheEntry::~DeepImageCacheEntry()
{
#ifdef DEBUG
    assert(!_sizeAccounted || _accountedSize == _deepImage->getSizeInBytes());
#endif
    deallocate();
}

CacheDiskIdentity
DeepImageCacheEntry::diskIdentityOfKey(const DeepImageKey& key)
{
    CacheDiskByteWriter base;
    base.put<std::uint64_t>(key._nodeHashKey);
    base.put<double>(key._time);
    base.put<std::int32_t>(key._view);

    CacheDiskByteWriter variant;
    variant.put<std::uint32_t>(key._mipmapLevel);

    return CacheDiskTier::makeIdentity(eCacheDiskPayloadDeep, base.bytes(), variant.bytes());
}

std::string
DeepImageCacheEntry::diskParamsBytes(const DeepImageParams& params)
{
    CacheDiskByteWriter bytes;
    const RectI& bounds = params.getBounds();
    bytes.put<std::int32_t>(bounds.x1);
    bytes.put<std::int32_t>(bounds.y1);
    bytes.put<std::int32_t>(bounds.x2);
    bytes.put<std::int32_t>(bounds.y2);

    return bytes.bytes();
}

DeepImageParamsPtr
DeepImageCacheEntry::diskParamsFromBytes(const std::string& bytes,
                                         const RectI& bounds)
{
    CacheDiskByteReader reader(bytes);
    std::int32_t x1, y1, x2, y2;

    if (!reader.get(&x1) || !reader.get(&y1) || !reader.get(&x2) || !reader.get(&y2) || !reader.atEnd() || !(RectI(x1, y1, x2, y2) == bounds)) {
        return DeepImageParamsPtr();
    }

    return std::make_shared<DeepImageParams>(bounds);
}

bool
DeepImageCacheEntry::diskIdentity(CacheDiskIdentity* identity) const
{
    if (!getCacheAPI()) {
        return false;
    }
    *identity = diskIdentityOfKey(getKey());
    const std::string paramsBytes = diskParamsBytes(*_params);
    identity->paramsHash = CacheDiskTier::hashBytes(paramsBytes.data(), paramsBytes.size());

    return true;
}

bool
DeepImageCacheEntry::diskSnapshot(CacheDiskSnapshot* snapshot) const
{
    // Until then the sample table and channels are still being filled.
    if (!isFullyRendered() || !diskIdentity(&snapshot->identity)) {
        return false;
    }
    snapshot->paramsBytes = diskParamsBytes(*_params);

    QReadLocker k(&_entryLock);

    const SampleTable& table = _deepImage->getSampleTable();
    const std::size_t pixels = table.getPixelCount();
    if (pixels == 0) {
        return false;
    }
    std::uint64_t payload = sizeof(std::uint8_t) + sizeof(std::uint32_t);
    for (const std::pair<const std::string, DeepChannelBuffer>& channel : _deepImage->getChannels()) {
        payload += sizeof(std::uint32_t) + channel.first.size() + sizeof(std::uint64_t) + channel.second.size() * sizeof(float);
    }
    payload += table.getCounts().size() * sizeof(U32) + table.getOffsets().size() * sizeof(U64);

    snapshot->bounds = _deepImage->getBounds();
    snapshot->totalPixels = pixels;
    snapshot->renderedPixels = pixels;
    snapshot->payloadBytes = payload;
    snapshot->bitmapRuns.clear();

    return true;
}

bool
DeepImageCacheEntry::diskWritePayload(const CacheDiskSnapshot& /*snapshot*/,
                                      CacheDiskSink& sink) const
{
    CacheDiskByteWriter prelude;
    const std::map<std::string, DeepChannelBuffer>& channels = _deepImage->getChannels();

    prelude.put<std::uint8_t>(_deepImage->isTidy() ? 1 : 0);
    prelude.put<std::uint32_t>((std::uint32_t)channels.size());
    for (const std::pair<const std::string, DeepChannelBuffer>& channel : channels) {
        prelude.putText(channel.first);
        prelude.put<std::uint64_t>(channel.second.size());
    }

    const SampleTable& table = _deepImage->getSampleTable();
    if (!sink.write(prelude.bytes().data(), prelude.bytes().size()) || !writeInChunks(sink, table.getCounts().data(), table.getCounts().size() * sizeof(U32)) || !writeInChunks(sink, table.getOffsets().data(), table.getOffsets().size() * sizeof(U64))) {
        return false;
    }
    for (const std::pair<const std::string, DeepChannelBuffer>& channel : channels) {
        if (!writeInChunks(sink, channel.second.data(), channel.second.size() * sizeof(float))) {
            return false;
        }
    }

    return true;
}

bool
DeepImageCacheEntry::diskRestore(const CacheDiskFileInfo& info,
                                 CacheDiskSource& source)
{
    SampleTable& table = _deepImage->getSampleTableForWriting();
    const std::size_t pixels = table.getPixelCount();

    if ((pixels == 0) || (info.totalPixels != pixels) || !info.allRendered || (info.bitmapBytes != 0) || !(_deepImage->getBounds() == info.bounds) || (info.payloadBytes < (std::uint64_t)pixels * sizeof(U32))) {
        return false;
    }

    std::uint8_t tidy;
    std::uint32_t channelCount;
    if (!source.read(&tidy, sizeof(tidy)) || !source.read(&channelCount, sizeof(channelCount)) || (channelCount > kMaxDiskChannels)) {
        return false;
    }

    std::vector<std::string> names(channelCount);
    std::uint64_t sampleBytes = 0;
    std::uint64_t sampleCountOfChannels = 0;
    for (std::string& name : names) {
        std::uint32_t length;
        std::uint64_t samples;
        if (!source.read(&length, sizeof(length)) || (length > kMaxDiskChannelName)) {
            return false;
        }
        name.resize(length);
        if ((length > 0) && !source.read(&name[0], length)) {
            return false;
        }
        if (!source.read(&samples, sizeof(samples))) {
            return false;
        }
        if (&name != &names[0] && (samples != sampleCountOfChannels)) {
            return false;
        }
        sampleCountOfChannels = samples;
        if (samples > info.payloadBytes / sizeof(float)) {
            return false;
        }
        sampleBytes += samples * sizeof(float);
    }
    if (sampleBytes > info.payloadBytes) {
        return false;
    }

    std::vector<U32> counts(pixels);
    if (!readInChunks(source, counts.data(), counts.size() * sizeof(U32))) {
        return false;
    }
    for (std::size_t i = 0; i < pixels; ++i) {
        table.setCount(i, counts[i]);
    }
    table.recomputeOffsets();
    if ((channelCount > 0) && (table.getTotalSampleCount() != sampleCountOfChannels)) {
        return false;
    }

    std::vector<U64> offsets(table.getOffsets().size());
    if (!readInChunks(source, offsets.data(), offsets.size() * sizeof(U64)) || (offsets != table.getOffsets())) {
        return false;
    }
    for (const std::string& name : names) {
        DeepChannelBuffer& channel = _deepImage->getChannelForWriting(name);
        if (channel.size() != sampleCountOfChannels) {
            return false;
        }
        if ((channel.size() > 0) && !readInChunks(source, channel.dataForWriting(), channel.size() * sizeof(float))) {
            return false;
        }
    }
    _deepImage->setTidy(tidy != 0);
    allocateMemory();

    return true;
}

void
DeepImageCacheEntry::allocateMemory()
{
    std::size_t accounted;
    {
        QWriteLocker k(&_entryLock);
        if (_sizeAccounted) {
            return;
        }
        accounted = _deepImage->getSizeInBytes();
        _accountedSize = accounted;
        _sizeAccounted = true;
    }
    if (getCacheAPI()) {
        getCacheAPI()->notifyEntryAllocated(getTime(), accounted, eStorageModeRAM);
    }
}

void
DeepImageCacheEntry::deallocate()
{
    std::size_t accounted;
    {
        QWriteLocker k(&_entryLock);
        if (!_sizeAccounted) {
            return;
        }
        accounted = _accountedSize;
        _sizeAccounted = false;
    }
    if (getCacheAPI()) {
        getCacheAPI()->notifyEntryDestroyed(getTime(), accounted, eStorageModeRAM);
    }
}

NATRON_NAMESPACE_EXIT
