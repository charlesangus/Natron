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

#include "LiveTextureStore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectI.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

namespace {
// The header the asset bytes start with; the pixels follow at kHeaderSize, float-aligned.
const char kMagic[8] = { 'N', 'T', 'L', 'I', 'V', 'E', 'T', 'X' };
const std::size_t kHeaderSize = 16;

// Enough for a few HD sequences' worth of frames held for scrubbing, or one 8K texture.
const std::size_t kDefaultByteBudget = std::size_t(2) << 30;

template <typename PIX>
void
copyRow(const Image::ReadAccess& access,
        const RectI& rect,
        int y,
        int numComps,
        float scale,
        float* row)
{
    for (int x = rect.x1; x < rect.x2; ++x, row += 4) {
        const PIX* pix = reinterpret_cast<const PIX*>(access.pixelAt(x, y));
        if (!pix) {
            row[0] = row[1] = row[2] = row[3] = 0.f;
            continue;
        }
        switch (numComps) {
        case 1:
            // An alpha-only image: its one channel is the first, which scalar inputs read.
            row[0] = row[1] = row[2] = row[3] = pix[0] * scale;
            break;
        case 2:
            row[0] = pix[0] * scale;
            row[1] = pix[1] * scale;
            row[2] = 0.f;
            row[3] = 1.f;
            break;
        case 3:
            row[0] = pix[0] * scale;
            row[1] = pix[1] * scale;
            row[2] = pix[2] * scale;
            row[3] = 1.f;
            break;
        default: {
            // Natron's images are premultiplied, a UsdUVTexture's are not: Storm multiplies the
            // colour by the alpha the material's opacity reads, so it is divided out here.
            const float alpha = pix[3] * scale;
            // The reciprocal of a subnormal alpha is infinite, and would turn a zero colour to NaN.
            const float unpremult = (alpha >= std::numeric_limits<float>::min()) ? (1.f / alpha) : 1.f;
            row[0] = pix[0] * scale * unpremult;
            row[1] = pix[1] * scale * unpremult;
            row[2] = pix[2] * scale * unpremult;
            row[3] = alpha;
            break;
        }
        }
    }
}

/**
 * Straight colour is undefined where the alpha is 0, and Storm's bilinear and mipmap filtering
 * blends it into the edge texels it averages with: left black, it darkens every soft or
 * antialiased edge. Texels with no alpha within kTransparentFillBand texels (counting diagonal
 * steps as one, as a mip level's square footprint does) of one with an alpha take the colour of
 * their neighbours one step nearer, weighted by alpha next to the covered texels, in time linear in
 * the texels. Farther texels, texels with an alpha, and every alpha are left as rendered.
 **/
void
fillTransparentColour(LiveTextureBuffer& buffer)
{
    const int width = buffer.getWidth();
    const int height = buffer.getHeight();
    const unsigned char unreached = std::numeric_limits<unsigned char>::max();
    std::vector<unsigned char> distance(std::size_t(width) * std::size_t(height), unreached);
    bool anyTransparent = false;

    for (int y = 0; y < height; ++y) {
        const float* row = buffer.getRow(y);
        for (int x = 0; x < width; ++x) {
            if (row[4 * x + 3] > 0.f) {
                distance[std::size_t(y) * std::size_t(width) + std::size_t(x)] = 0;
            } else {
                anyTransparent = true;
            }
        }
    }
    if (!anyTransparent) {
        return;
    }

    const auto texelAt = [&](std::size_t i) {
        return buffer.getRow(int(i / std::size_t(width))) + 4 * (i % std::size_t(width));
    };
    std::vector<std::size_t> frontier;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(width) + std::size_t(x);
            if (distance[i] != 0) {
                continue;
            }
            bool bordersTransparent = false;
            for (int ny = std::max(y - 1, 0); ny <= std::min(y + 1, height - 1) && !bordersTransparent; ++ny) {
                for (int nx = std::max(x - 1, 0); nx <= std::min(x + 1, width - 1); ++nx) {
                    if (distance[std::size_t(ny) * std::size_t(width) + std::size_t(nx)] == unreached) {
                        bordersTransparent = true;
                        break;
                    }
                }
            }
            if (bordersTransparent) {
                frontier.push_back(i);
            }
        }
    }

    std::vector<std::size_t> next;
    for (int d = 1; (d <= LiveTextureBuffer::kTransparentFillBand) && !frontier.empty(); ++d) {
        next.clear();
        for (std::size_t i : frontier) {
            const int x = int(i % std::size_t(width));
            const int y = int(i / std::size_t(width));
            for (int ny = std::max(y - 1, 0); ny <= std::min(y + 1, height - 1); ++ny) {
                for (int nx = std::max(x - 1, 0); nx <= std::min(x + 1, width - 1); ++nx) {
                    const std::size_t n = std::size_t(ny) * std::size_t(width) + std::size_t(nx);
                    if (distance[n] == unreached) {
                        distance[n] = (unsigned char)d;
                        next.push_back(n);
                    }
                }
            }
        }
        for (std::size_t n : next) {
            const int x = int(n % std::size_t(width));
            const int y = int(n / std::size_t(width));
            float sum[4] = { 0.f, 0.f, 0.f, 0.f };
            for (int ny = std::max(y - 1, 0); ny <= std::min(y + 1, height - 1); ++ny) {
                for (int nx = std::max(x - 1, 0); nx <= std::min(x + 1, width - 1); ++nx) {
                    const std::size_t i = std::size_t(ny) * std::size_t(width) + std::size_t(nx);
                    if (distance[i] != d - 1) {
                        continue;
                    }
                    const float* nearer = texelAt(i);
                    const float weight = (d == 1) ? nearer[3] : 1.f;
                    sum[0] += nearer[0] * weight;
                    sum[1] += nearer[1] * weight;
                    sum[2] += nearer[2] * weight;
                    sum[3] += weight;
                }
            }
            if (sum[3] > 0.f) {
                float* texel = texelAt(n);
                texel[0] = sum[0] / sum[3];
                texel[1] = sum[1] / sum[3];
                texel[2] = sum[2] / sum[3];
            }
        }
        frontier.swap(next);
    }
} // fillTransparentColour

// The URI names no view and the layers that hold it are not per view, so a texture has one.
const ViewIdx kLiveTextureView(0);

// Failure messages kept for getFailure().
const std::size_t kMaxFailures = 256;

/**
 * The texture of source at frame, or of its UDIM tile when tile is not 0. format receives the
 * format the texture was to span when it could be told, so a stand-in can be of its size. A tile
 * whose format cannot be told sets untiledFormat to the source's format outside any tile, when
 * that can be told.
 **/
LiveTextureBufferConstPtr
renderSource(const LiveTextureSource& source,
             double frame,
             ViewIdx view,
             int tile,
             RectI* format,
             RectI* untiledFormat,
             std::string* failure)
{
    NodePtr node = source.node.lock();

    if (!node || !node->isActivated()) {
        *failure = "the node " + source.nodeName + " that feeds it no longer exists";

        return LiveTextureBufferConstPtr();
    }
    // A disabled node shows its pass-through input, as the engine renders it, but the render
    // below declines a disabled node. A source authored while the node was enabled can be
    // rendered after it is disabled, and disabling can be animated.
    while (node->isNodeDisabled(frame)) {
        node = node->getPreferredInputNode();
        if (!node || !node->isActivated()) {
            *failure = "the node " + source.nodeName + " that feeds it is disabled and passes no input through";

            return LiveTextureBufferConstPtr();
        }
    }
    char frameText[32];
    std::snprintf(frameText, sizeof(frameText), "%g", frame);
    const std::string what = node->getFullyQualifiedName() + (tile ? " for UDIM tile " + std::to_string(tile) : std::string()) + " at frame " + frameText;
    const RenderContextPtr context = tile ? RenderContext::Builder().set(RenderContextName::udimTile(), RenderContextValue(std::int64_t(tile))).build() : RenderContextPtr();
    LiveTextureWindow window;
    if (tile) {
        // A tile spans its format exactly: texture coordinates pick the tile, so no transform can
        // map them into an overscan. The format is the one the source has under the tile.
        bool hasFormat;
        {
            ActionContextScope underTile(true, context);
            hasFormat = getNodeImageWindow(node, frame, view, &window.format, &window.window);
        }
        if (!hasFormat) {
            LiveTextureWindow untiled;
            if (getNodeImageWindow(node, frame, view, &untiled.format, &untiled.window)) {
                *untiledFormat = untiled.format;
            }
            *failure = what + " has no format";

            return LiveTextureBufferConstPtr();
        }
        window.window = window.format;
    } else {
        // The texture holds the whole window its layer authored texture coordinates for: the
        // format with the overscan around it, transparent black where the image does not reach.
        window = source.getWindow(frame);
        if (window.isNull() && !getNodeImageWindow(node, frame, view, &window.format, &window.window)) {
            *failure = what + " could not be rendered";

            return LiveTextureBufferConstPtr();
        }
    }
    *format = window.format;
    RectI rect;
    ImagePtr image;
    const bool rendered = renderNodeImageOverWindow(node, frame, view, ImageLayerDesc::getRGBAComponents(), window.window, &image, &rect, context);
    if (!rendered || (image && rect.isNull())) {
        *failure = what + " could not be rendered";

        return LiveTextureBufferConstPtr();
    }
    const std::shared_ptr<LiveTextureBuffer> buffer = LiveTextureBuffer::create(window.window.width(), window.window.height());
    if (!buffer) {
        *failure = "the image of " + what + " is too large to hold";

        return LiveTextureBufferConstPtr();
    }
    if (!image) {
        return buffer;
    }
    const int numComps = (int)image->getComponentsCount();
    Image::ReadAccess access(image.get());
    for (int y = rect.y1; y < rect.y2; ++y) {
        float* row = buffer->getRow(y - window.window.y1) + std::size_t(rect.x1 - window.window.x1) * 4;
        switch (image->getBitDepth()) {
        case eImageBitDepthByte:
            copyRow<unsigned char>(access, rect, y, numComps, 1.f / 255.f, row);
            break;
        case eImageBitDepthShort:
            copyRow<unsigned short>(access, rect, y, numComps, 1.f / 65535.f, row);
            break;
        case eImageBitDepthFloat:
            copyRow<float>(access, rect, y, numComps, 1.f, row);
            break;
        default:
            *failure = "the image of " + what + " has a bit depth live textures do not read";

            return LiveTextureBufferConstPtr();
        }
    }
    // Only RGBA colour is unpremultiplied, and an alpha-only image's colour is its alpha.
    if (numComps >= 4) {
        fillTransparentColour(*buffer);
    }

    return buffer;
} // renderSource

// Storm reads every tile of a set at the size of the first, so a stand-in for that one keeps the
// size the tile would have had, when it is known.
LiveTextureBufferConstPtr
makeMagenta(const RectI& format)
{
    std::shared_ptr<LiveTextureBuffer> buffer = format.isNull() ? std::shared_ptr<LiveTextureBuffer>() : LiveTextureBuffer::create(format.width(), format.height());

    if (!buffer) {
        buffer = LiveTextureBuffer::create(1, 1);
    }
    if (!buffer) {
        return buffer;
    }
    for (int y = 0; y < buffer->getHeight(); ++y) {
        float* row = buffer->getRow(y);
        for (int x = 0; x < buffer->getWidth(); ++x, row += 4) {
            row[0] = 1.f;
            row[1] = 0.f;
            row[2] = 1.f;
            row[3] = 1.f;
        }
    }

    return buffer;
}

/**
 * One texture prepare() makes: a source's single texture, tile 0, or one tile of its UDIM set.
 **/
struct PrepareJob {
    const LiveTextureSource* source = nullptr;
    std::string uri;
    std::string setUri;
    int tile = 0;
};

std::vector<PrepareJob>
makeJobs(const LiveTextureSourceList& sources,
         double frame)
{
    std::vector<PrepareJob> jobs;

    for (const LiveTextureSource& source : sources) {
        PrepareJob job;
        job.source = &source;
        job.setUri = source.getUri(frame);
        if (!source.isUdim()) {
            job.uri = job.setUri;
            jobs.push_back(job);
            continue;
        }
        for (int tile : source.getTiles(frame)) {
            job.tile = tile;
            job.uri = source.getTileUri(frame, tile);
            jobs.push_back(job);
        }
    }

    return jobs;
}

/**
 * What the tile of job shows at frame, by source, layer, tile, frame and tile version, 0 when its
 * source no longer exists. Walks the graph upstream of the source.
 **/
U64
makeTileKey(const PrepareJob& job,
            double frame)
{
    const LiveTextureSource& source = *job.source;
    const NodePtr node = source.node.lock();

    if (!node) {
        return 0;
    }
    const double sourceFrame = source.getSourceFrame(frame);
    ContentHasher h;
    h.appendString(source.nodeName);
    h.appendString(source.layer);
    h.appendI64(job.tile);
    // A frame-varying image changes with the frame where its version does not.
    h.appendDouble(source.varying ? sourceFrame : 0.);
    h.appendU64(udimTileVersion(node, job.tile, sourceFrame, kLiveTextureView));

    return h.finalize();
}

std::string
joinTiles(const std::vector<int>& tiles)
{
    std::string text;

    for (int tile : tiles) {
        text += (text.empty() ? "" : ", ") + std::to_string(tile);
    }

    return text;
}
} // anonymous namespace

std::shared_ptr<LiveTextureBuffer>
LiveTextureBuffer::create(int width,
                          int height)
{
    if ((width <= 0) || (height <= 0)) {
        return std::shared_ptr<LiveTextureBuffer>();
    }
    const std::size_t numFloats = std::size_t(width) * std::size_t(height) * 4;
    if (numFloats > (std::numeric_limits<std::size_t>::max() - kHeaderSize) / sizeof(float)) {
        return std::shared_ptr<LiveTextureBuffer>();
    }
    const std::size_t byteSize = kHeaderSize + numFloats * sizeof(float);
    char* bytes = static_cast<char*>(::operator new(byteSize, std::nothrow));
    if (!bytes) {
        return std::shared_ptr<LiveTextureBuffer>();
    }
    std::memcpy(bytes, kMagic, sizeof(kMagic));
    const std::int32_t size[2] = { width, height };
    std::memcpy(bytes + sizeof(kMagic), size, sizeof(size));

    std::shared_ptr<LiveTextureBuffer> buffer(new LiveTextureBuffer());
    buffer->_bytes = std::shared_ptr<const char>(bytes, [](const char* p) { ::operator delete(const_cast<char*>(p)); });
    buffer->_byteSize = byteSize;
    buffer->_pixels = reinterpret_cast<float*>(bytes + kHeaderSize);
    buffer->_width = width;
    buffer->_height = height;
    std::fill(buffer->_pixels, buffer->_pixels + numFloats, 0.f);

    return buffer;
}

float*
LiveTextureBuffer::getRow(int y)
{
    return _pixels + std::size_t(y) * std::size_t(_width) * 4;
}

const float*
LiveTextureBuffer::getRow(int y) const
{
    return _pixels + std::size_t(y) * std::size_t(_width) * 4;
}

bool
LiveTextureBuffer::parse(const char* bytes,
                         std::size_t size,
                         int* width,
                         int* height,
                         const float** pixels)
{
    if (!bytes || (size < kHeaderSize) || (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0)) {
        return false;
    }
    std::int32_t dims[2];
    std::memcpy(dims, bytes + sizeof(kMagic), sizeof(dims));
    if ((dims[0] <= 0) || (dims[1] <= 0)
        || ((size - kHeaderSize) / sizeof(float) / 4 / std::size_t(dims[0]) < std::size_t(dims[1]))) {
        return false;
    }
    *width = dims[0];
    *height = dims[1];
    *pixels = reinterpret_cast<const float*>(bytes + kHeaderSize);

    return true;
}

double
LiveTextureSource::getSourceFrame(double frame) const
{
    const double first = std::ceil(firstFrame);
    const double last = std::max(first, std::floor(lastFrame));

    return std::min(last, std::max(first, std::floor(frame)));
}

U64
LiveTextureSource::getVersion(double frame) const
{
    if (!varying && tiles.empty()) {
        return contentHash;
    }
    ContentHasher h;
    h.appendU64(contentHash);
    if (varying) {
        h.appendDouble(getSourceFrame(frame));
    }
    // Storm keeps a set it has loaded under its URI, so a set of other tiles needs another.
    const std::vector<int>& tilesAtFrame = getTiles(frame);
    h.appendU64(tilesAtFrame.size());
    for (int tile : tilesAtFrame) {
        h.appendI64(tile);
    }

    return h.finalize();
}

std::string
LiveTextureSource::getUri(double frame) const
{
    return makeUri(nodeName, tiles.empty() ? layer : layer + "." kLiveTextureUdimToken, getVersion(frame));
}

std::string
LiveTextureSource::getTileUri(double frame,
                              int tile) const
{
    return makeUri(nodeName, layer + "." + std::to_string(tile), getVersion(frame));
}

const std::vector<int>&
LiveTextureSource::getTiles(double frame) const
{
    const double index = getRecordedIndex(frame);

    if ((index < 0.) || (index >= (double)frameTiles.size())) {
        return tiles;
    }

    return frameTiles[(std::size_t)index];
}

std::vector<int>
LiveTextureSource::getTilesBeyondUsd(double frame) const
{
    const std::vector<int>& tilesAtFrame = getTiles(frame);

    return std::vector<int>(std::upper_bound(tilesAtFrame.begin(), tilesAtFrame.end(), kLastUsdUdimTile), tilesAtFrame.end());
}

std::string
LiveTextureSource::getTilesBeyondUsdWarning(double frame) const
{
    const std::vector<int> beyond = getTilesBeyondUsd(frame);

    if (beyond.empty()) {
        return std::string();
    }

    return "The UDIM tiles " + joinTiles(beyond) + " of the live texture " + getUri(frame) + " are above " + std::to_string(kLastUsdUdimTile)
        + ", the last tile USD looks for, and do not show.";
}

double
LiveTextureSource::getRecordedIndex(double frame) const
{
    return varying ? getSourceFrame(frame) - std::ceil(firstFrame) : 0.;
}

LiveTextureWindow
LiveTextureSource::getWindow(double frame) const
{
    const double index = getRecordedIndex(frame);

    if ((index < 0.) || (index >= (double)windows.size())) {
        return LiveTextureWindow();
    }

    return windows[(std::size_t)index];
}

void
LiveTextureWindow::getUvTransform(double scale[2],
                                  double translation[2]) const
{
    if (isNull()) {
        scale[0] = scale[1] = 1.;
        translation[0] = translation[1] = 0.;

        return;
    }
    scale[0] = (double)format.width() / (double)window.width();
    scale[1] = (double)format.height() / (double)window.height();
    translation[0] = (double)(format.x1 - window.x1) / (double)window.width();
    translation[1] = (double)(format.y1 - window.y1) / (double)window.height();
}

std::string
LiveTextureSource::makeUri(const std::string& nodeName,
                           const std::string& layer,
                           U64 version)
{
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)version);

    return std::string(kLiveTextureScheme "://") + nodeName + "/" + layer + "?v=" + hex;
}

LiveTextureStore::LiveTextureStore()
    : _byteBudget(kDefaultByteBudget)
{
}

LiveTextureStore&
LiveTextureStore::instance()
{
    static LiveTextureStore store;

    return store;
}

void
LiveTextureStore::insert(const std::string& uri,
                         const LiveTextureBufferConstPtr& buffer)
{
    if (!buffer) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    insertLocked(uri, buffer, false);
    _failures.erase(uri);
    evictLocked();
}

void
LiveTextureStore::insertLocked(const std::string& uri,
                               const LiveTextureBufferConstPtr& buffer,
                               bool fallback)
{
    std::map<std::string, Entry>::iterator found = _entries.find(uri);
    if (found != _entries.end()) {
        eraseLocked(found);
    }
    _uses.push_front(uri);
    Entry& entry = _entries[uri];
    entry.buffer = buffer;
    entry.use = _uses.begin();
    entry.fallback = fallback;
    if (++_bufferUses[buffer.get()] == 1) {
        _byteSize += buffer->getByteSize();
    }
}

void
LiveTextureStore::eraseLocked(std::map<std::string, Entry>::iterator entry)
{
    std::map<const LiveTextureBuffer*, int>::iterator uses = _bufferUses.find(entry->second.buffer.get());
    if ((uses != _bufferUses.end()) && (--uses->second == 0)) {
        _byteSize -= entry->second.buffer->getByteSize();
        _bufferUses.erase(uses);
    }
    _uses.erase(entry->second.use);
    _entries.erase(entry);
}

void
LiveTextureStore::setFailureLocked(const std::string& uri,
                                   const std::string& failure)
{
    if (failure.empty()) {
        _failures.erase(uri);

        return;
    }
    // A varying source's every frame and every edit has a URI of its own, most of which are never
    // tried again, so erasing on success alone would not bound this.
    if ((_failures.size() >= kMaxFailures) && (_failures.find(uri) == _failures.end())) {
        _failures.clear();
    }
    _failures[uri] = failure;
}

LiveTextureBufferConstPtr
LiveTextureStore::find(const std::string& uri)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<std::string, Entry>::iterator found = _entries.find(uri);
    if (found == _entries.end()) {
        return LiveTextureBufferConstPtr();
    }
    _uses.splice(_uses.begin(), _uses, found->second.use);

    return found->second.buffer;
}

bool
LiveTextureStore::contains(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _entries.find(uri) != _entries.end();
}

void
LiveTextureStore::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _entries.clear();
    _uses.clear();
    _failures.clear();
    _lastPrepared.clear();
    _bufferUses.clear();
    _tiles.clear();
    _byteSize = 0;
}

void
LiveTextureStore::setByteBudget(std::size_t bytes)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _byteBudget = bytes;
    evictLocked();
}

std::size_t
LiveTextureStore::getByteBudget() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _byteBudget;
}

std::size_t
LiveTextureStore::getByteSize() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _byteSize;
}

void
LiveTextureStore::evictLocked()
{
    // The most recent texture stays even over budget: it is the one about to be read. So do the
    // ones a frame on screen or being prepared reads: going over budget costs memory, while
    // dropping one shows Storm's fallback with nothing to say why.
    std::list<std::string>::iterator use = _uses.end();
    while ((_byteSize > _byteBudget) && (use != _uses.begin())) {
        --use;
        if (use == _uses.begin()) {
            break;
        }
        if ((_pinned.find(*use) != _pinned.end()) || (_lastPrepared.find(*use) != _lastPrepared.end())) {
            continue;
        }
        const std::list<std::string>::iterator next = std::next(use);
        std::map<std::string, Entry>::iterator victim = _entries.find(*use);
        if (victim != _entries.end()) {
            eraseLocked(victim);
        } else {
            _uses.erase(use);
        }
        use = next;
    }
}

std::string
LiveTextureStore::getFailure(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<std::string, std::string>::const_iterator found = _failures.find(uri);

    return (found == _failures.end()) ? std::string() : found->second;
}

int
LiveTextureStore::prepare(const LiveTextureSourceList& sources,
                          double frame)
{
    // Consumer warnings, joined and set once, so one texture's does not hide another's.
    std::map<NodePtr, std::vector<std::string>> warnings;
    const auto warn = [&warnings](const LiveTextureSource& source,
                                  const std::string& message) {
        const NodePtr consumer = source.consumer.lock();
        if (!consumer) {
            return;
        }
        std::vector<std::string>& messages = warnings[consumer];
        if (std::find(messages.begin(), messages.end(), message) == messages.end()) {
            messages.push_back(message);
        }
    };
    const auto flushWarnings = [&warnings]() {
        for (const std::pair<const NodePtr, std::vector<std::string>>& messages : warnings) {
            std::string text;
            for (const std::string& message : messages.second) {
                text += (text.empty() ? "" : "\n") + message;
            }
            messages.first->setPersistentMessage(eMessageTypeWarning, text);
        }
    };

    // A render on the main thread would block the UI and could deadlock against it.
    QCoreApplication* app = QCoreApplication::instance();
    if (app && (QThread::currentThread() == app->thread())) {
        // Warn rather than leave the consumer silently showing the fallback.
        std::map<const LiveTextureSource*, std::string> missingBySource;
        int numMissing = 0;
        for (const PrepareJob& job : makeJobs(sources, frame)) {
            if (contains(job.uri)) {
                continue;
            }
            ++numMissing;
            std::string& uris = missingBySource[job.source];
            uris += (uris.empty() ? "" : ", ") + job.uri;
        }
        std::map<NodePtr, std::string> missingByConsumer;
        for (const std::pair<const LiveTextureSource* const, std::string>& missing : missingBySource) {
            const NodePtr consumer = missing.first->consumer.lock();
            if (consumer) {
                std::string& uris = missingByConsumer[consumer];
                uris += (uris.empty() ? "" : ", ") + missing.second;
            }
        }
        for (const std::pair<const NodePtr, std::string>& missing : missingByConsumer) {
            missing.first->setPersistentMessage(eMessageTypeWarning,
                                                "The live textures " + missing.second + " are not rendered on the main thread and show the renderer's fallback.");
        }

        return numMissing;
    }

    if (sources.empty()) {
        return 0;
    }

    const std::vector<PrepareJob> jobs = makeJobs(sources, frame);
    std::set<std::string> uris;
    for (const PrepareJob& job : jobs) {
        uris.insert(job.uri);
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const std::string& uri : uris) {
            ++_pinned[uri];
        }
    }

    int numFailed = 0;
    // The failures of each UDIM set's tiles, which getFailure() gives for the set's URI.
    std::map<std::string, std::string> setFailures;
    // The warnings of the textures that failed in this call, which are neither rendered nor
    // counted again for another source naming them.
    std::map<std::string, std::string> failedHere;
    // A format of each UDIM set's tiles, for the stand-in of a tile whose own is not known.
    std::map<std::string, RectI> setFormats;
    const auto noteSetFormat = [&setFormats](const PrepareJob& job,
                                             const LiveTextureBufferConstPtr& buffer) {
        if (job.tile && buffer) {
            setFormats.insert(std::make_pair(job.setUri, RectI(0, 0, buffer->getWidth(), buffer->getHeight())));
        }
    };
    // Failed tiles whose format is not known, stood in for once every tile of their set is tried.
    struct UnsizedTile {
        std::string uri;
        std::string setUri;
        RectI untiledFormat;
    };
    std::vector<UnsizedTile> unsizedTiles;
    for (const PrepareJob& job : jobs) {
        const LiveTextureSource& source = *job.source;
        if (job.tile) {
            setFailures.insert(std::make_pair(job.setUri, std::string()));
        }
        const std::map<std::string, std::string>::const_iterator failedEarlier = failedHere.find(job.uri);
        if (failedEarlier != failedHere.end()) {
            warn(source, failedEarlier->second);
            continue;
        }
        // Computed only for a tile the store lacks, as it walks the whole graph upstream.
        U64 tileKey = 0;
        if (job.tile) {
            bool stored;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                const std::map<std::string, Entry>::const_iterator found = _entries.find(job.uri);
                stored = (found != _entries.end()) && !found->second.fallback;
            }
            if (!stored) {
                tileKey = makeTileKey(job, frame);
            }
        }
        {
            std::unique_lock<std::mutex> lock(_mutex);
            // Another consumer rendering the same texture: wait for it rather than render twice.
            _rendered.wait(lock, [&]() { return _rendering.find(job.uri) == _rendering.end(); });
            std::map<std::string, Entry>::iterator found = _entries.find(job.uri);
            if ((found != _entries.end()) && !found->second.fallback) {
                _uses.splice(_uses.begin(), _uses, found->second.use);
                noteSetFormat(job, found->second.buffer);
                continue;
            }
            if (tileKey) {
                std::map<U64, std::weak_ptr<const LiveTextureBuffer>>::iterator held = _tiles.find(tileKey);
                const LiveTextureBufferConstPtr buffer = (held == _tiles.end()) ? LiveTextureBufferConstPtr() : held->second.lock();
                if (buffer) {
                    insertLocked(job.uri, buffer, false);
                    _failures.erase(job.uri);
                    noteSetFormat(job, buffer);
                    continue;
                }
            }
            _rendering.insert(job.uri);
        }

        // On a thread of its own: a render resets the render state of the thread it runs on, and
        // the caller may be in the middle of a render of its own (USDRender).
        std::string failure;
        RectI format;
        RectI untiledFormat;
        LiveTextureBufferConstPtr buffer;
        std::thread renderer([&]() {
            try {
                buffer = renderSource(source, source.getSourceFrame(frame), kLiveTextureView, job.tile, &format, &untiledFormat, &failure);
            } catch (const std::exception& e) {
                buffer.reset();
                failure = e.what();
            } catch (...) {
                buffer.reset();
                failure = "an unknown error occurred";
            }
        });
        renderer.join();
        if (job.tile && !format.isNull()) {
            setFormats.insert(std::make_pair(job.setUri, format));
        }
        noteSetFormat(job, buffer);
        const bool unsized = !buffer && job.tile && format.isNull();
        const LiveTextureBufferConstPtr magenta = (!buffer && job.tile && !unsized) ? makeMagenta(format) : LiveTextureBufferConstPtr();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _rendering.erase(job.uri);
            ++_renderCounts[job.tile];
            if (buffer) {
                insertLocked(job.uri, buffer, false);
                if (tileKey) {
                    _tiles[tileKey] = buffer;
                }
            } else if (magenta) {
                insertLocked(job.uri, magenta, true);
            }
            setFailureLocked(job.uri, buffer ? std::string() : failure);
            evictLocked();
        }
        _rendered.notify_all();

        if (!buffer) {
            ++numFailed;
            std::string message;
            if (job.tile) {
                std::string& tileFailures = setFailures[job.setUri];
                tileFailures += (tileFailures.empty() ? "" : "; ") + failure;
                message = "The UDIM tile " + std::to_string(job.tile) + " of the live texture " + job.setUri + " shows magenta: " + failure + ".";
                if (unsized) {
                    UnsizedTile pending;
                    pending.uri = job.uri;
                    pending.setUri = job.setUri;
                    pending.untiledFormat = untiledFormat;
                    unsizedTiles.push_back(pending);
                }
            } else {
                message = "The live texture " + job.uri + " shows the renderer's fallback: " + failure + ".";
            }
            failedHere[job.uri] = message;
            warn(source, message);
        }
    }
    // Storm sizes a whole set from its lowest tile, so a 1x1 stand-in there would shrink every
    // other tile to a texel.
    for (const UnsizedTile& pending : unsizedTiles) {
        const std::map<std::string, RectI>::const_iterator known = setFormats.find(pending.setUri);
        const LiveTextureBufferConstPtr magenta = makeMagenta((known != setFormats.end()) ? known->second : pending.untiledFormat);
        if (!magenta) {
            continue;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        const std::map<std::string, Entry>::const_iterator found = _entries.find(pending.uri);
        if ((found == _entries.end()) || found->second.fallback) {
            insertLocked(pending.uri, magenta, true);
            evictLocked();
        }
    }
    for (const LiveTextureSource& source : sources) {
        const std::string beyond = source.getTilesBeyondUsdWarning(frame);
        if (!beyond.empty()) {
            warn(source, beyond);
        }
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const std::pair<const std::string, std::string>& set : setFailures) {
            setFailureLocked(set.first, set.second);
        }
        for (const std::string& uri : uris) {
            std::map<std::string, int>::iterator pin = _pinned.find(uri);
            if (--pin->second == 0) {
                _pinned.erase(pin);
            }
        }
        _lastPrepared.swap(uris);
        evictLocked();
        for (std::map<U64, std::weak_ptr<const LiveTextureBuffer>>::iterator it = _tiles.begin(); it != _tiles.end();) {
            if (it->second.expired()) {
                it = _tiles.erase(it);
            } else {
                ++it;
            }
        }
    }
    flushWarnings();

    return numFailed;
} // LiveTextureStore::prepare

std::map<int, std::size_t>
LiveTextureStore::getRenderCounts() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _renderCounts;
}

namespace SceneCaches {
void
releaseLiveTextures()
{
    LiveTextureStore::instance().clear();
}
} // namespace SceneCaches

NATRON_NAMESPACE_EXIT
