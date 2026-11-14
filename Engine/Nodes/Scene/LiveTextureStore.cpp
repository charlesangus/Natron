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
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/ContentHasher.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Scene/LiveTextureOiio/LiveTextureFile.h"
#include "Engine/Nodes/Scene/LiveTextureSharedMemory.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
using NatronLiveTexture::kHeaderSize;

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

std::string
fallbackWarning(const std::string& uri,
                const std::string& failure)
{
    return "The live texture " + uri + " shows the missing-texture colour, magenta: " + failure + ".";
}

// natron://<node>/<layer>?v=<hash>, whose reader knows the node by the name it gave it.
std::string
nodeNameOfUri(const std::string& uri)
{
    const std::size_t start = std::strlen(kLiveTextureScheme "://");
    const std::size_t end = uri.rfind('/');

    return ((end == std::string::npos) || (end <= start)) ? uri : uri.substr(start, end - start);
}

std::string
megabytes(std::size_t bytes)
{
    char text[64];

    std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024. * 1024.));

    return text;
}

std::string
noRoomMessage(const std::string& uri,
              std::size_t needed,
              std::size_t available)
{
    return "The live texture " + nodeNameOfUri(uri) + " needs " + megabytes(needed) + " of shared memory (" + LiveTextureSharedMemory::directory()
        + ") to reach the renderer and only " + megabytes(available) + " is free: the render stops. Free space or make the image smaller to render it";
}

LiveTextureBufferConstPtr
renderSource(const LiveTextureSource& source,
             double frame,
             ViewIdx view,
             const AbortableRenderInfoPtr& abortInfo,
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
    // The texture holds the whole window its layer authored texture coordinates for: the format
    // with the overscan around it, transparent black where the image does not reach.
    char frameText[32];
    std::snprintf(frameText, sizeof(frameText), "%g", frame);
    LiveTextureWindow window = source.getWindow(frame);
    if (window.isNull() && !getNodeImageWindow(node, frame, view, &window.format, &window.window)) {
        *failure = node->getFullyQualifiedName() + " could not be rendered at frame " + frameText;

        return LiveTextureBufferConstPtr();
    }
    RectI rect;
    ImagePtr image;
    const bool rendered = renderNodeImageOverWindow(node, frame, view, ImageLayerDesc::getRGBAComponents(), window.window, &image, &rect, abortInfo);
    if (!rendered || (image && rect.isNull())) {
        *failure = node->getFullyQualifiedName() + " could not be rendered at frame " + frameText;

        return LiveTextureBufferConstPtr();
    }
    const std::shared_ptr<LiveTextureBuffer> buffer = LiveTextureBuffer::create(window.window.width(), window.window.height());
    if (!buffer) {
        *failure = "the image of " + node->getFullyQualifiedName() + " at frame " + frameText + " is too large to hold";

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
            *failure = "the image of " + node->getFullyQualifiedName() + " has a bit depth live textures do not read";

            return LiveTextureBufferConstPtr();
        }
    }
    // Only RGBA colour is unpremultiplied, and an alpha-only image's colour is its alpha.
    if (numComps >= 4) {
        fillTransparentColour(*buffer);
    }

    return buffer;
} // renderSource
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
    NatronLiveTexture::writeHeader(bytes, width, height);

    std::shared_ptr<LiveTextureBuffer> buffer(new LiveTextureBuffer());
    buffer->_bytes = std::shared_ptr<const char>(bytes, [](const char* p) { ::operator delete(const_cast<char*>(p)); });
    buffer->_byteSize = byteSize;
    buffer->_pixels = reinterpret_cast<float*>(bytes + kHeaderSize);
    buffer->_width = width;
    buffer->_height = height;
    std::fill(buffer->_pixels, buffer->_pixels + numFloats, 0.f);

    return buffer;
}

std::shared_ptr<const LiveTextureBuffer>
LiveTextureBuffer::fromFile(const std::shared_ptr<const char>& bytes,
                            std::size_t byteSize,
                            int width,
                            int height,
                            const std::string& filePath)
{
    std::shared_ptr<LiveTextureBuffer> buffer(new LiveTextureBuffer());

    buffer->_bytes = bytes;
    buffer->_byteSize = byteSize;
    // Only the const accessors can reach it, through the const pointer returned.
    buffer->_pixels = reinterpret_cast<float*>(const_cast<char*>(bytes.get()) + kHeaderSize);
    buffer->_width = width;
    buffer->_height = height;
    buffer->_filePath = filePath;

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
    int parsedWidth = 0;
    int parsedHeight = 0;

    if (!NatronLiveTexture::readHeader(bytes, size, &parsedWidth, &parsedHeight)
        || ((size - kHeaderSize) / sizeof(float) / 4 / std::size_t(parsedWidth) < std::size_t(parsedHeight))) {
        return false;
    }
    *width = parsedWidth;
    *height = parsedHeight;
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

std::string
LiveTextureSource::getUri(double frame) const
{
    if (!varying) {
        return makeUri(nodeName, layer, contentHash);
    }
    ContentHasher h;
    h.appendU64(contentHash);
    h.appendDouble(getSourceFrame(frame));

    return makeUri(nodeName, layer, h.finalize());
}

LiveTextureWindow
LiveTextureSource::getWindow(double frame) const
{
    const double index = varying ? getSourceFrame(frame) - std::ceil(firstFrame) : 0.;

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
    std::map<std::string, Entry>::iterator found = _entries.find(uri);
    if (found != _entries.end()) {
        _byteSize -= found->second.buffer->getByteSize();
        _uses.erase(found->second.use);
        _entries.erase(found);
    }
    _uses.push_front(uri);
    _noRoom.erase(uri);
    Entry& entry = _entries[uri];
    entry.buffer = buffer;
    entry.use = _uses.begin();
    _byteSize += buffer->getByteSize();
    _failures.erase(uri);
    evictLocked();
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
    _noRoom.clear();
    _lastPrepared.clear();
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
        std::map<std::string, Entry>::iterator victim = _entries.find(*use);
        if (victim != _entries.end()) {
            _byteSize -= victim->second.buffer->getByteSize();
            _entries.erase(victim);
        }
        use = _uses.erase(use);
    }
}

void
LiveTextureStore::recordFailureLocked(const std::string& uri,
                                      const std::string& failure)
{
    // A varying source's every frame and every edit has a URI of its own, most of which are never
    // tried again, so erasing on success alone would not bound this.
    if ((_failures.size() >= kMaxFailures) && (_failures.find(uri) == _failures.end())) {
        _failures.clear();
    }
    _failures[uri] = failure;
}

void
LiveTextureStore::addConsumerLocked(const std::string& uri,
                                    const NodeWPtr& consumer)
{
    const std::map<std::string, Entry>::iterator found = _entries.find(uri);
    const NodePtr node = consumer.lock();

    if ((found == _entries.end()) || !node) {
        return;
    }
    std::vector<NodeWPtr>& consumers = found->second.consumers;
    const bool known = std::any_of(consumers.begin(), consumers.end(), [&node](const NodeWPtr& other) { return other.lock() == node; });
    if (!known) {
        consumers.push_back(consumer);
    }
}

// Returned rather than destroyed here: removing a large file from tmpfs frees its pages one by one,
// which no other texture's lookup should wait for.
std::vector<LiveTextureBufferConstPtr>
LiveTextureStore::releaseUnusedFilesLocked(const std::string& keptUri,
                                           std::size_t bytes)
{
    std::vector<LiveTextureBufferConstPtr> released;
    std::size_t freed = 0;
    std::list<std::string>::iterator use = _uses.end();

    while ((freed < bytes) && (use != _uses.begin())) {
        --use;
        const std::map<std::string, Entry>::iterator victim = _entries.find(*use);
        // A delegate's reference would keep the pages however many entries let go of them.
        if ((*use == keptUri) || (_pinned.find(*use) != _pinned.end()) || (_lastPrepared.find(*use) != _lastPrepared.end())
            || (victim == _entries.end()) || victim->second.buffer->getFilePath().empty() || (victim->second.buffer.use_count() > 1)) {
            continue;
        }
        freed += victim->second.buffer->getByteSize();
        _byteSize -= victim->second.buffer->getByteSize();
        released.push_back(victim->second.buffer);
        _entries.erase(victim);
        use = _uses.erase(use);
    }

    return released;
}

namespace {
std::function<bool()>&
exportAborted()
{
    thread_local std::function<bool()> aborted;

    return aborted;
}

const std::chrono::milliseconds kAbortPoll(10);
} // anonymous namespace

LiveTextureStore::ExportAbortScope::ExportAbortScope(const std::function<bool()>& aborted)
    : _outer(exportAborted())
{
    const std::function<bool()> outer = _outer;

    exportAborted() = outer ? std::function<bool()>([outer, aborted]() { return aborted() || outer(); }) : aborted;
}

LiveTextureStore::ExportAbortScope::~ExportAbortScope()
{
    exportAborted() = _outer;
}

LiveTextureBufferConstPtr
LiveTextureStore::exportToSharedMemory(const std::string& uri)
{
    // Ends the export on every way out, so callers waiting on the uri do not wait forever.
    class Exporting {
    public:
        Exporting(LiveTextureStore& store,
                  const std::string& uri)
            : _store(store)
            , _uri(uri)
        {
        }

        ~Exporting()
        {
            if (_held) {
                {
                    std::lock_guard<std::mutex> lock(_store._mutex);
                    _store._exporting.erase(_uri);
                }
                _store._rendered.notify_all();
            }
        }

        Exporting(const Exporting&) = delete;
        Exporting& operator=(const Exporting&) = delete;

        void beginLocked()
        {
            _store._exporting.insert(_uri);
            _held = true;
        }

        void endLocked()
        {
            _store._exporting.erase(_uri);
            _held = false;
        }

    private:
        LiveTextureStore& _store;
        const std::string& _uri;
        bool _held = false;
    };

    const std::function<bool()> aborted = exportAborted();
    Exporting exporting(*this, uri);
    LiveTextureBufferConstPtr source;
    LiveTextureBufferConstPtr replaced;
    bool warned = false;
    {
        std::unique_lock<std::mutex> lock(_mutex);
        const auto idle = [&]() { return (_rendering.find(uri) == _rendering.end()) && (_exporting.find(uri) == _exporting.end()); };
        while (!idle()) {
            if (!aborted) {
                _rendered.wait(lock, idle);
            } else if (aborted()) {
                return LiveTextureBufferConstPtr();
            } else {
                _rendered.wait_for(lock, kAbortPoll);
            }
        }
        const std::map<std::string, Entry>::iterator found = _entries.find(uri);
        const std::map<std::string, std::weak_ptr<const LiveTextureBuffer>>::iterator file = _exported.find(uri);
        const LiveTextureBufferConstPtr alive = (file == _exported.end()) ? LiveTextureBufferConstPtr() : file->second.lock();
        if (found != _entries.end()) {
            _uses.splice(_uses.begin(), _uses, found->second.use);
        }
        if (alive) {
            if (found != _entries.end()) {
                replaced = found->second.buffer;
                _byteSize += alive->getByteSize() - replaced->getByteSize();
                found->second.buffer = alive;
                evictLocked();
            }

            return alive;
        }
        if (found == _entries.end()) {
            if (_failures.find(uri) == _failures.end()) {
                recordFailureLocked(uri, "the image of " + nodeNameOfUri(uri) + " was not rendered for this renderer");
            }

            return LiveTextureBufferConstPtr();
        }
        // Tried again every time: another texture letting go of its file can make room for this one.
        warned = _failures.find(uri) != _failures.end();
        exporting.beginLocked();
        source = found->second.buffer;
    }

    LiveTextureSharedMemory::Export file;
    int errorNumber = 0;
    std::string what;
    const auto write = [&]() {
        return LiveTextureSharedMemory::exportImage(uri, source->getWidth(), source->getHeight(), source->getRow(0), &file, &errorNumber, &what, aborted);
    };
    bool exported = write();
    if (!exported && (errorNumber == ENOSPC)) {
        std::vector<LiveTextureBufferConstPtr> released;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            released = releaseUnusedFilesLocked(uri, NatronLiveTexture::fileSize(source->getWidth(), source->getHeight()));
        }
        if (!released.empty()) {
            released.clear();
            exported = write();
        }
    }
    if (!exported && (errorNumber == ECANCELED)) {
        // Not a failure of the texture: the next render tries again.
        return LiveTextureBufferConstPtr();
    }

    LiveTextureBufferConstPtr result;
    std::string failure;
    bool noRoom = false;
    std::vector<NodeWPtr> consumers;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        exporting.endLocked();
        const std::map<std::string, Entry>::iterator found = _entries.find(uri);
        if (exported) {
            result = LiveTextureBuffer::fromFile(file.bytes, file.byteSize, source->getWidth(), source->getHeight(), file.path);
            for (std::map<std::string, std::weak_ptr<const LiveTextureBuffer>>::iterator it = _exported.begin(); it != _exported.end();) {
                it = it->second.expired() ? _exported.erase(it) : std::next(it);
            }
            _exported[uri] = result;
            _failures.erase(uri);
            _noRoom.erase(uri);
            if (found != _entries.end()) {
                replaced = found->second.buffer;
                _byteSize += result->getByteSize() - replaced->getByteSize();
                found->second.buffer = result;
                evictLocked();
            }
        } else {
            if (errorNumber == ENOSPC) {
                failure = noRoomMessage(uri, NatronLiveTexture::fileSize(source->getWidth(), source->getHeight()), LiveTextureSharedMemory::availableBytes());
                _noRoom[uri] = failure;
                noRoom = true;
            } else {
                failure = "the image of " + nodeNameOfUri(uri) + " does not fit in shared memory (" + what + ": " + std::strerror(errorNumber) + ")";
            }
            recordFailureLocked(uri, failure);
            if ((found != _entries.end()) && !warned) {
                consumers = found->second.consumers;
            }
        }
    }
    _rendered.notify_all();

    for (const NodeWPtr& weak : consumers) {
        if (const NodePtr consumer = weak.lock()) {
            consumer->setPersistentMessage(eMessageTypeWarning, noRoom ? failure : fallbackWarning(uri, failure));
        }
    }

    return result;
} // LiveTextureStore::exportToSharedMemory

std::string
LiveTextureStore::getSourceOfFile(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (const std::pair<const std::string, std::weak_ptr<const LiveTextureBuffer>>& exported : _exported) {
        const LiveTextureBufferConstPtr buffer = exported.second.lock();
        if (buffer && (buffer->getFilePath() == path)) {
            return nodeNameOfUri(exported.first);
        }
    }

    return std::string();
}

std::string
LiveTextureStore::getNoRoom(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const std::map<std::string, std::string>::const_iterator found = _noRoom.find(uri);

    return (found == _noRoom.end()) ? std::string() : found->second;
}

std::string
LiveTextureStore::getAnyNoRoom() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    // A note about a texture no recent render asked for belongs to a scene that has moved on.
    for (const std::pair<const std::string, std::string>& note : _noRoom) {
        if ((_lastPrepared.find(note.first) != _lastPrepared.end()) || (_pinned.find(note.first) != _pinned.end())) {
            return note.second;
        }
    }

    return std::string();
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
                          double frame,
                          const std::function<bool()>& aborted)
{
    // A render on the main thread would block the UI and could deadlock against it.
    QCoreApplication* app = QCoreApplication::instance();
    if (app && (QThread::currentThread() == app->thread())) {
        // Warn rather than leave the consumer silently showing the fallback.
        std::map<NodePtr, std::string> missingByConsumer;
        int numMissing = 0;
        for (const LiveTextureSource& source : sources) {
            const std::string uri = source.getUri(frame);
            if (contains(uri)) {
                continue;
            }
            ++numMissing;
            const NodePtr consumer = source.consumer.lock();
            if (consumer) {
                std::string& uris = missingByConsumer[consumer];
                uris += (uris.empty() ? "" : ", ") + uri;
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

    std::set<std::string> uris;
    for (const LiveTextureSource& source : sources) {
        uris.insert(source.getUri(frame));
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const std::string& uri : uris) {
            ++_pinned[uri];
            _noRoom.erase(uri);
        }
    }

    const auto isAborted = [&aborted]() { return aborted && aborted(); };
    int numFailed = 0;
    bool gaveUp = false;
    for (const LiveTextureSource& source : sources) {
        const std::string uri = source.getUri(frame);
        {
            std::unique_lock<std::mutex> lock(_mutex);
            // Another consumer rendering the same texture: wait for it rather than render twice.
            const auto idle = [&]() { return _rendering.find(uri) == _rendering.end(); };
            while (!idle() && !isAborted()) {
                _rendered.wait_for(lock, kAbortPoll);
            }
            if (!idle()) {
                gaveUp = true;
                break;
            }
            std::map<std::string, Entry>::iterator found = _entries.find(uri);
            if (found != _entries.end()) {
                _uses.splice(_uses.begin(), _uses, found->second.use);
                addConsumerLocked(uri, source.consumer);
                continue;
            }
            _rendering.insert(uri);
        }

        // On a thread of its own: a render resets the render state of the thread it runs on, and
        // the caller may be in the middle of a render of its own (USDRender).
        std::string failure;
        LiveTextureBufferConstPtr buffer;
        const AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        std::promise<void> finished;
        std::future<void> done = finished.get_future();
        std::thread renderer([&]() {
            try {
                buffer = renderSource(source, source.getSourceFrame(frame), kLiveTextureView, abortInfo, &failure);
            } catch (const std::exception& e) {
                buffer.reset();
                failure = e.what();
            } catch (...) {
                buffer.reset();
                failure = "an unknown error occurred";
            }
            finished.set_value();
        });
        // aborted reads the caller's render state, so it is asked here, on the caller's thread.
        while (aborted && (done.wait_for(kAbortPoll) != std::future_status::ready)) {
            if (!abortInfo->isAborted() && aborted()) {
                abortInfo->setAborted();
            }
        }
        renderer.join();
        // A render stopped half way says nothing about the texture, so it is neither kept nor
        // recorded as failed, and the next prepare() renders it again.
        const bool stopped = abortInfo->isAborted();
        if (stopped) {
            buffer.reset();
        }
        if (buffer) {
            insert(uri, buffer);
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _rendering.erase(uri);
            if (buffer) {
                _failures.erase(uri);
                addConsumerLocked(uri, source.consumer);
            } else if (!stopped) {
                recordFailureLocked(uri, failure);
            }
        }
        _rendered.notify_all();

        if (!buffer && !stopped) {
            ++numFailed;
            const NodePtr consumer = source.consumer.lock();
            if (consumer) {
                consumer->setPersistentMessage(eMessageTypeWarning, fallbackWarning(uri, failure));
            }
        }
        if (stopped || isAborted()) {
            gaveUp = true;
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const std::string& uri : uris) {
            std::map<std::string, int>::iterator pin = _pinned.find(uri);
            if (--pin->second == 0) {
                _pinned.erase(pin);
            }
        }
        // An abandoned prepare() leaves the last finished one's textures held for the frame on screen.
        if (!gaveUp) {
            _lastPrepared.swap(uris);
        }
        evictLocked();
    }

    return numFailed;
} // LiveTextureStore::prepare

namespace SceneCaches {
void
releaseLiveTextures()
{
    LiveTextureStore::instance().clear();
}
} // namespace SceneCaches

NATRON_NAMESPACE_EXIT
