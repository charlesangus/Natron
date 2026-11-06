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
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

#include "Engine/ContentHasher.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/RectI.h"

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

LiveTextureBufferConstPtr
renderSource(const LiveTextureSource& source,
             double frame,
             ViewIdx view,
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
    const bool rendered = renderNodeImageOverWindow(node, frame, view, ImageLayerDesc::getRGBAComponents(), window.window, &image, &rect);
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
        }
    }

    int numFailed = 0;
    for (const LiveTextureSource& source : sources) {
        const std::string uri = source.getUri(frame);
        {
            std::unique_lock<std::mutex> lock(_mutex);
            // Another consumer rendering the same texture: wait for it rather than render twice.
            _rendered.wait(lock, [&]() { return _rendering.find(uri) == _rendering.end(); });
            std::map<std::string, Entry>::iterator found = _entries.find(uri);
            if (found != _entries.end()) {
                _uses.splice(_uses.begin(), _uses, found->second.use);
                continue;
            }
            _rendering.insert(uri);
        }

        // On a thread of its own: a render resets the render state of the thread it runs on, and
        // the caller may be in the middle of a render of its own (USDRender).
        std::string failure;
        LiveTextureBufferConstPtr buffer;
        std::thread renderer([&]() {
            try {
                buffer = renderSource(source, source.getSourceFrame(frame), kLiveTextureView, &failure);
            } catch (const std::exception& e) {
                buffer.reset();
                failure = e.what();
            } catch (...) {
                buffer.reset();
                failure = "an unknown error occurred";
            }
        });
        renderer.join();
        if (buffer) {
            insert(uri, buffer);
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _rendering.erase(uri);
            if (buffer) {
                _failures.erase(uri);
            } else {
                // A varying source's every frame and every edit has a URI of its own, most of which
                // are never tried again, so erasing on success alone would not bound this.
                if ((_failures.size() >= kMaxFailures) && (_failures.find(uri) == _failures.end())) {
                    _failures.clear();
                }
                _failures[uri] = failure;
            }
        }
        _rendered.notify_all();

        if (!buffer) {
            ++numFailed;
            const NodePtr consumer = source.consumer.lock();
            if (consumer) {
                consumer->setPersistentMessage(eMessageTypeWarning,
                                               "The live texture " + uri + " shows the renderer's fallback: " + failure + ".");
            }
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
        _lastPrepared.swap(uris);
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
