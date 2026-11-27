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

#include "NodeOverlayRenderer.h"

#include <algorithm>
#include <cstdint>

#include "Global/GLIncludes.h"

#include "Engine/OverlayDrawList.h"
#include "Engine/OverlaySupport.h"

NATRON_NAMESPACE_ENTER

namespace {

struct Rgba {
    float r, g, b, a;
};

Rgba
resolveColor(const OverlayColor& c,
             const OverlayDrawState& state)
{
    if (c.nodeColor) {
        return Rgba { state.nodeR, state.nodeG, state.nodeB, c.a };
    }

    return Rgba { c.r, c.g, c.b, c.a };
}

class Tessellator {
public:
    Tessellator(std::vector<float>* positions,
                std::vector<float>* colors,
                std::vector<float>* shadowColors,
                std::vector<NodeOverlayRenderer::Batch>* batches)
        : _positions(positions)
        , _colors(colors)
        , _shadowColors(shadowColors)
        , _batches(batches)
    {
    }

    void begin(unsigned int mode,
               float width,
               bool smooth,
               bool shadow)
    {
        const int first = (int)(_positions->size() / 2);
        if (!_batches->empty()) {
            NodeOverlayRenderer::Batch& last = _batches->back();
            if ((last.mode == mode) && (last.width == width) && (last.smooth == smooth) && (last.shadow == shadow) && (last.first + last.count == first)) {
                return;
            }
        }
        _batches->push_back(NodeOverlayRenderer::Batch { mode, first, 0, width, smooth, shadow });
    }

    void vertex(const OverlayDrawList::Point& p,
                const Rgba& c)
    {
        _positions->push_back((float)p.x);
        _positions->push_back((float)p.y);
        _colors->push_back(c.r);
        _colors->push_back(c.g);
        _colors->push_back(c.b);
        _colors->push_back(c.a);
        _shadowColors->push_back(0.f);
        _shadowColors->push_back(0.f);
        _shadowColors->push_back(0.f);
        _shadowColors->push_back(c.a);
        ++_batches->back().count;
    }

private:
    std::vector<float>* _positions;
    std::vector<float>* _colors;
    std::vector<float>* _shadowColors;
    std::vector<NodeOverlayRenderer::Batch>* _batches;
};

} // anon namespace

NodeOverlayRenderer::NodeOverlayRenderer()
    : _vbo(0)
    , _uploads(0)
    , _uploaded(false)
    , _uploadedBuildId(0)
    , _uploadedState()
    , _uploadedScaleDependent(false)
    , _uploadedHasText(false)
    , _uploadedPixelScaleX(0.)
    , _uploadedPixelScaleY(0.)
    , _uploadedPositionBytes(0)
    , _uploadedColorBytes(0)
{
}

NodeOverlayRenderer::~NodeOverlayRenderer()
{
}

void
NodeOverlayRenderer::releaseGLResources()
{
    if (_vbo) {
        GLuint id = _vbo;
        glDeleteBuffers(1, &id);
        _vbo = 0;
    }
    _uploaded = false;
}

void
NodeOverlayRenderer::tessellate(const OverlayDrawList& list,
                                const OverlayDrawState& state,
                                double pixelScaleX,
                                double pixelScaleY,
                                std::vector<float>* positions,
                                std::vector<float>* colors,
                                std::vector<float>* shadowColors,
                                std::vector<Batch>* batches)
{
    positions->clear();
    colors->clear();
    shadowColors->clear();
    batches->clear();

    Tessellator t(positions, colors, shadowColors, batches);
    const std::vector<OverlayDrawList::Point>& pts = list.points();
    const std::vector<OverlayDrawList::Primitive>& primitives = list.primitives();
    std::vector<std::size_t> triangles;
    std::vector<OverlayDrawList::Point> segments;
    std::vector<OverlayDrawList::Point> filled;

    for (std::size_t index : list.drawOrder()) {
        const OverlayDrawList::Primitive& p = primitives[index];
        const Rgba color = resolveColor(p.color, state);
        switch (p.type) {
        case OverlayDrawList::ePrimitiveTypePoints:
            t.begin(GL_POINTS, p.width, p.smooth, p.shadow);
            for (std::size_t i = p.first; i < p.first + p.count; ++i) {
                t.vertex(pts[i], color);
            }
            break;
        case OverlayDrawList::ePrimitiveTypeLines:
        case OverlayDrawList::ePrimitiveTypePolyline:
        case OverlayDrawList::ePrimitiveTypeStroke:
        case OverlayDrawList::ePrimitiveTypeAnchoredShape:
            segments.clear();
            filled.clear();
            OverlayDrawList::expandGeometry(p, list, pixelScaleX, pixelScaleY, &segments, &filled);
            if (!segments.empty()) {
                t.begin(GL_LINES, p.width, p.smooth, p.shadow);
                for (const OverlayDrawList::Point& v : segments) {
                    t.vertex(v, color);
                }
            }
            if (!filled.empty()) {
                t.begin(GL_TRIANGLES, 0.f, false, p.shadow);
                for (const OverlayDrawList::Point& v : filled) {
                    t.vertex(v, color);
                }
            }
            if ((p.type == OverlayDrawList::ePrimitiveTypeStroke) && (p.count > 0)) {
                // Wide GL lines have square ends and gaps at the joins; a round point the line's
                // width on every vertex rounds both.
                t.begin(GL_POINTS, p.width, true, p.shadow);
                for (std::size_t i = p.first; i < p.first + p.count; ++i) {
                    t.vertex(pts[i], color);
                }
            }
            break;
        case OverlayDrawList::ePrimitiveTypePolygon:
            triangles.clear();
            if (OverlayDrawList::triangulate(pts, p.first, p.count, &triangles) && !triangles.empty()) {
                t.begin(GL_TRIANGLES, 0.f, false, p.shadow);
                for (std::size_t i : triangles) {
                    t.vertex(pts[i], color);
                }
            }
            break;
        case OverlayDrawList::ePrimitiveTypeHandle: {
            Rgba c = color;
            if ((p.handleId == state.pressedHandle) || (p.handleId == state.hoveredHandle)) {
                c.r = 0.f;
                c.g = 1.f;
                c.b = 0.f;
            }
            t.begin(GL_POINTS, p.width, p.handleShape == OverlayDrawList::eHandleShapeCircle, p.shadow);
            t.vertex(pts[p.first], c);
            break;
        }
        case OverlayDrawList::ePrimitiveTypeText:
            break;
        }
    }
} // NodeOverlayRenderer::tessellate

bool
NodeOverlayRenderer::isUploaded(const OverlayDrawList& list,
                                const OverlayDrawState& state,
                                double pixelScaleX,
                                double pixelScaleY) const
{
    const bool sameScale = !_uploadedScaleDependent || ((_uploadedPixelScaleX == pixelScaleX) && (_uploadedPixelScaleY == pixelScaleY));

    return _uploaded && _vbo && (_uploadedBuildId == list.buildId()) && sameScale && (_uploadedState.hoveredHandle == state.hoveredHandle) && (_uploadedState.pressedHandle == state.pressedHandle) && (_uploadedState.nodeR == state.nodeR) && (_uploadedState.nodeG == state.nodeG) && (_uploadedState.nodeB == state.nodeB);
}

void
NodeOverlayRenderer::draw(const OverlayDrawList& list,
                          const OverlayDrawState& state,
                          OverlaySupport* viewport)
{
    if (list.empty() || !viewport) {
        return;
    }

    double pixelScaleX, pixelScaleY;
    viewport->getPixelScale(pixelScaleX, pixelScaleY);
    double viewportW, viewportH;
    viewport->getViewportSize(viewportW, viewportH);
    double screenPixelRatio = 1.;
#ifdef OFX_EXTENSIONS_NATRON
    screenPixelRatio = viewport->getScreenPixelRatio();
#endif

    GLdouble projection[16];
    glGetDoublev(GL_PROJECTION_MATRIX, projection);
    // How far to move GL_PROJECTION to shift the drawing by exactly one screen pixel.
    const double shadowX = (projection[0] * viewportW != 0.) ? 2. / (projection[0] * viewportW) : 0.;
    const double shadowY = (projection[5] * viewportH != 0.) ? 2. / (projection[5] * viewportH) : 0.;

    glPushAttrib(GL_ENABLE_BIT | GL_LINE_BIT | GL_POINT_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_HINT_BIT);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);

    glDisable(GL_LINE_STIPPLE);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (!isUploaded(list, state, pixelScaleX, pixelScaleY)) {
        tessellate(list, state, pixelScaleX, pixelScaleY, &_positions, &_colors, &_shadowColors, &_batches);
        _uploadedPositionBytes = _positions.size() * sizeof(float);
        _uploadedColorBytes = _colors.size() * sizeof(float);
        if (!_positions.empty()) {
            if (!_vbo) {
                GLuint id = 0;
                glGenBuffers(1, &id);
                _vbo = id;
            }
            glBindBuffer(GL_ARRAY_BUFFER, _vbo);
            glBufferData(GL_ARRAY_BUFFER, _uploadedPositionBytes + 2 * _uploadedColorBytes, 0, GL_DYNAMIC_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0, _uploadedPositionBytes, _positions.data());
            glBufferSubData(GL_ARRAY_BUFFER, _uploadedPositionBytes, _uploadedColorBytes, _colors.data());
            glBufferSubData(GL_ARRAY_BUFFER, _uploadedPositionBytes + _uploadedColorBytes, _uploadedColorBytes, _shadowColors.data());
            ++_uploads;
        }
        _uploaded = true;
        _uploadedBuildId = list.buildId();
        _uploadedState = state;
        _uploadedScaleDependent = std::any_of(list.primitives().begin(), list.primitives().end(), OverlayDrawList::dependsOnPixelScale);
        _uploadedHasText = std::any_of(list.primitives().begin(), list.primitives().end(), [](const OverlayDrawList::Primitive& p) {
            return p.type == OverlayDrawList::ePrimitiveTypeText;
        });
        _uploadedPixelScaleX = pixelScaleX;
        _uploadedPixelScaleY = pixelScaleY;
    }

    const bool hasVertices = (_uploadedPositionBytes > 0) && _vbo;
    if (hasVertices) {
        glBindBuffer(GL_ARRAY_BUFFER, _vbo);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, (const GLvoid*)0);
    }

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);

    const std::vector<OverlayDrawList::Point>& pts = list.points();
    const std::vector<OverlayDrawList::Primitive>& primitives = list.primitives();
    // Overlays drawn as many small lists pay this sort on every redraw, though most hold no text.
    std::vector<std::size_t> order;
    if (_uploadedHasText) {
        order = list.drawOrder();
    }

    for (int l = 0; l < 2; ++l) {
        const bool shadowPass = (l == 0);
        glMatrixMode(GL_PROJECTION);
        if (shadowPass) {
            glTranslated(shadowX, -shadowY, 0);
        } else {
            glTranslated(-shadowX, shadowY, 0);
        }
        glMatrixMode(GL_MODELVIEW);

        if (hasVertices) {
            const std::uintptr_t colorOffset = _uploadedPositionBytes + (shadowPass ? _uploadedColorBytes : 0);
            glColorPointer(4, GL_FLOAT, 0, (const GLvoid*)colorOffset);
            for (const Batch& b : _batches) {
                if ((shadowPass && !b.shadow) || (b.count == 0)) {
                    continue;
                }
                if (b.mode == GL_POINTS) {
                    if (b.smooth) {
                        glEnable(GL_POINT_SMOOTH);
                    } else {
                        glDisable(GL_POINT_SMOOTH);
                    }
                    glPointSize(b.width * (float)screenPixelRatio);
                } else if (b.mode == GL_LINES) {
                    if (b.smooth) {
                        glEnable(GL_LINE_SMOOTH);
                    } else {
                        glDisable(GL_LINE_SMOOTH);
                    }
                    glLineWidth(b.width * (float)screenPixelRatio);
                }
                glDrawArrays(b.mode, b.first, b.count);
            }
        }

        for (std::size_t index : order) {
            const OverlayDrawList::Primitive& p = primitives[index];
            if ((p.type != OverlayDrawList::ePrimitiveTypeText) || (shadowPass && !p.shadow)) {
                continue;
            }
            const Rgba c = resolveColor(p.color, state);
            const double x = pts[p.first].x + p.textOffsetX * pixelScaleX;
            const double y = pts[p.first].y + p.textOffsetY * pixelScaleY;
            if (shadowPass) {
                viewport->renderText(x, y, p.text, 0., 0., 0.);
            } else {
                viewport->renderText(x, y, p.text, c.r, c.g, c.b);
            }
        }
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    if (hasVertices) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    glPopClientAttrib();
    glPopAttrib();
    glCheckError();
} // NodeOverlayRenderer::draw

NATRON_NAMESPACE_EXIT
