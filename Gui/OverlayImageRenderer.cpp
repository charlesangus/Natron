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

#include "OverlayImageRenderer.h"

#include <algorithm>
#include <cstddef>

#include <QDebug>

#include "Global/GLIncludes.h"

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/OverlayDrawList.h"

NATRON_NAMESPACE_ENTER

namespace {
// glGetError() returns GL_INVALID_OPERATION forever without a current context.
constexpr int kMaxGLErrorsDrained = 16;
}

std::vector<std::vector<unsigned char>>
OverlayImageRenderer::buildMipChain(const std::vector<unsigned char>& base,
                                    int width,
                                    int height)
{
    std::vector<std::vector<unsigned char>> chain;

    chain.push_back(base);
    while (width > 1 || height > 1) {
        // GL wants each level at floor(size / 2). A rounded-up odd size leaves the texture incomplete,
        // and an incomplete texture samples as alpha 1: the whole tile floods with its tint. So the
        // last row and column of an odd level fold into the texels before them.
        const int w = std::max(1, width / 2);
        const int h = std::max(1, height / 2);
        const std::vector<unsigned char>& src = chain.back();
        std::vector<unsigned char> dst((std::size_t)w * h, 0);
        for (int y = 0; y < height; ++y) {
            const int dy = std::min(y / 2, h - 1);
            for (int x = 0; x < width; ++x) {
                const int dx = std::min(x / 2, w - 1);
                unsigned char& texel = dst[(std::size_t)dy * w + dx];
                texel = std::max(texel, src[(std::size_t)y * width + x]);
            }
        }
        chain.push_back(std::move(dst));
        width = w;
        height = h;
    }

    return chain;
}

OverlayImageRenderer::OverlayImageRenderer()
{
}

OverlayImageRenderer::~OverlayImageRenderer()
{
}

void
OverlayImageRenderer::releaseGLResources()
{
    for (auto& it : _textures) {
        for (const Chunk& c : it.second.chunks) {
            GLuint id = c.texture;
            glDeleteTextures(1, &id);
        }
    }
    _textures.clear();
    _nodes.clear();
}

OverlayImageRenderer::TileTextures&
OverlayImageRenderer::texturesFor(const std::shared_ptr<const OverlayImageTile>& tile)
{
    auto found = _textures.find(tile.get());
    if (found != _textures.end()) {
        return found->second;
    }

    TileTextures& entry = _textures[tile.get()];
    entry.tile = tile;
    entry.used = true;

    const int width = tile->bounds.width();
    const int height = tile->bounds.height();
    if (width <= 0 || height <= 0 || tile->values.size() < (std::size_t)width * height) {
        return entry;
    }

    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    maxSize = std::max<GLint>(maxSize, 64);

    GLint savedAlignment = 4;
    GLint savedBinding = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedAlignment);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedBinding);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    for (int y0 = 0; y0 < height; y0 += maxSize) {
        for (int x0 = 0; x0 < width; x0 += maxSize) {
            const int cw = std::min<int>(maxSize, width - x0);
            const int ch = std::min<int>(maxSize, height - y0);
            std::vector<unsigned char> bytes((std::size_t)cw * ch);
            for (int y = 0; y < ch; ++y) {
                const unsigned char* row = tile->values.data() + (std::size_t)(y0 + y) * width + x0;
                std::copy(row, row + cw, bytes.begin() + (std::ptrdiff_t)y * cw);
            }
            const std::vector<std::vector<unsigned char>> chain = buildMipChain(bytes, cw, ch);

            for (int i = 0; (i < kMaxGLErrorsDrained) && (glGetError() != GL_NO_ERROR); ++i) {
            }

            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            int lw = cw;
            int lh = ch;
            for (std::size_t level = 0; level < chain.size(); ++level) {
                glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_ALPHA8, lw, lh, 0, GL_ALPHA, GL_UNSIGNED_BYTE, chain[level].data());
                lw = std::max(1, lw / 2);
                lh = std::max(1, lh / 2);
            }
            // A texture without storage samples as alpha 1 and floods the tile with its tint. The
            // chunk stays missing rather than retrying the allocation on every paint.
            const GLenum uploadError = glGetError();
            if (uploadError != GL_NO_ERROR) {
                qDebug() << "Overlay image texture upload failed, GL error" << uploadError;
                glDeleteTextures(1, &id);
                continue;
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)chain.size() - 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            entry.chunks.push_back(Chunk { id, x0, y0, x0 + cw, y0 + ch });
        }
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, savedAlignment);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedBinding);

    return entry;
}

void
OverlayImageRenderer::drawFrame(const OverlayImageFrame& frame,
                                const OverlayDrawState& state)
{
    for (const OverlayImagePlane& plane : frame.planes) {
        const float r = plane.tint.nodeColor ? state.nodeR : plane.tint.r;
        const float g = plane.tint.nodeColor ? state.nodeG : plane.tint.g;
        const float b = plane.tint.nodeColor ? state.nodeB : plane.tint.b;
        glColor4f(r, g, b, plane.tint.a);

        for (const std::shared_ptr<const OverlayImageTile>& tile : plane.tiles) {
            TileTextures& textures = texturesFor(tile);
            textures.used = true;

            // A pixel of a tile at mipmap level m spans 2^m canonical pixels, and the pixel
            // aspect ratio stretches x only.
            const double scale = (double)(1u << tile->mipmapLevel);
            const double sx = scale * frame.pixelAspectRatio;
            for (const Chunk& c : textures.chunks) {
                const double x1 = (tile->bounds.x1 + c.x1) * sx;
                const double x2 = (tile->bounds.x1 + c.x2) * sx;
                const double y1 = (tile->bounds.y1 + c.y1) * scale;
                const double y2 = (tile->bounds.y1 + c.y2) * scale;
                glBindTexture(GL_TEXTURE_2D, c.texture);
                glBegin(GL_QUADS);
                glTexCoord2f(0.f, 0.f);
                glVertex2d(x1, y1);
                glTexCoord2f(1.f, 0.f);
                glVertex2d(x2, y1);
                glTexCoord2f(1.f, 1.f);
                glVertex2d(x2, y2);
                glTexCoord2f(0.f, 1.f);
                glVertex2d(x1, y2);
                glEnd();
            }
        }
    }
}

void
OverlayImageRenderer::releaseUnused()
{
    for (auto it = _textures.begin(); it != _textures.end();) {
        if (it->second.used) {
            it->second.used = false;
            ++it;
            continue;
        }
        for (const Chunk& c : it->second.chunks) {
            GLuint id = c.texture;
            glDeleteTextures(1, &id);
        }
        it = _textures.erase(it);
    }
}

void
OverlayImageRenderer::draw(const NodesList& nodes,
                           double time,
                           ViewIdx view)
{
    for (auto& it : _nodes) {
        it.second.seen = false;
    }

    struct Item {
        OverlayImageFrameConstPtr frame;
        OverlayDrawState state;
    };
    std::vector<Item> items;
    for (const NodePtr& node : nodes) {
        const EffectInstancePtr effect = node->getEffectInstance();
        if (!effect || !effect->isOverlayImageRequested()) {
            continue;
        }
        NodeEntry& entry = _nodes[node.get()];
        entry.seen = true;
        OverlayImageFrameConstPtr frame = effect->getOverlayImages(time, view);
        if (frame) {
            entry.held = frame;
        } else if (entry.held && node->isOverlayImagePassPending() && entry.held->request.time == time && entry.held->request.view == view) {
            frame = entry.held;
        }
        if (frame) {
            items.push_back(Item { frame, effect->getOverlayDrawState() });
        }
    }
    for (auto it = _nodes.begin(); it != _nodes.end();) {
        it = it->second.seen ? std::next(it) : _nodes.erase(it);
    }

    if (!items.empty()) {
        GLint savedBinding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedBinding);
        {
            GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_CURRENT_BIT | GL_TEXTURE_BIT);
            glActiveTexture(GL_TEXTURE0);
            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            for (const Item& item : items) {
                drawFrame(*item.frame, item.state);
            }
        }
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedBinding);
    }
    releaseUnused();
}

NATRON_NAMESPACE_EXIT
