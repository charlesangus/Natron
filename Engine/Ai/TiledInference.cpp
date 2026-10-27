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

#include "TiledInference.h"

#include <algorithm>
#include <cmath>
#include <string>

NATRON_NAMESPACE_ENTER

namespace {

// Mirror padding that does not repeat the edge pixel, as numpy's "reflect".
int
reflect(int i,
        int n)
{
    if (n <= 1) {
        return 0;
    }
    const int period = 2 * (n - 1);
    i %= period;

    return i < n ? i : period - i;
}

int
roundUp(int value,
        int multiple)
{
    return ((value + multiple - 1) / multiple) * multiple;
}

float
raisedCosine(double t)
{
    return (float)(0.5 - 0.5 * std::cos(M_PI * t));
}

// A tile's blend weight along one axis at output resolution: raised-cosine ramps across what it
// shares with each neighbour, so the two weights over a shared span sum to one.
std::vector<float>
axisWeights(const std::vector<int>& starts,
            std::size_t index,
            int size,
            int scale)
{
    const int left = index > 0 ? std::max(0, starts[index - 1] + size - starts[index]) * scale : 0;
    const int right = index + 1 < starts.size() ? std::max(0, starts[index] + size - starts[index + 1]) * scale : 0;
    const int length = size * scale;
    std::vector<float> weights(length, 1.f);

    for (int j = 0; j < length; ++j) {
        if (j < left) {
            weights[j] *= raisedCosine((j + 0.5) / left);
        }
        if (j >= length - right) {
            weights[j] *= raisedCosine((length - j - 0.5) / right);
        }
    }

    return weights;
}

// A model input's fixed height and width, or 0 where the model leaves them dynamic.
void
fixedInputSize(const InferenceSession& session,
               const AiTileSpec& spec,
               int* height,
               int* width)
{
    if (session.inputs().size() != 1) {
        throw AiError(session.modelPath() + ": tiled inference needs a model with one input, not " + std::to_string(session.inputs().size()));
    }
    const std::vector<int64_t>& shape = session.inputs().front().shape;
    if (shape.size() != 4) {
        throw AiError(session.modelPath() + ": tiled inference needs an NCHW input, not " + Tensor::shapeString(shape));
    }
    if ((shape[1] > 0) && (shape[1] != spec.inChannels)) {
        throw AiError(session.modelPath() + ": the model takes " + std::to_string(shape[1]) + " channels, not " + std::to_string(spec.inChannels));
    }
    *height = shape[2] > 0 ? (int)shape[2] : 0;
    *width = shape[3] > 0 ? (int)shape[3] : 0;
}

Tensor
runPass(const InferenceSession& session,
        const RectI& roi,
        const AiTileSpec& spec,
        int tileEdge,
        const AiTilePack& pack,
        const AiAbortPredicate& abort,
        int* tilesRun)
{
    int fixedHeight = 0;
    int fixedWidth = 0;

    fixedInputSize(session, spec, &fixedHeight, &fixedWidth);

    const int width = roi.width();
    const int height = roi.height();
    const int scale = spec.scale;
    const int tileWidth = std::min(fixedWidth > 0 ? std::min(tileEdge, fixedWidth) : tileEdge, width);
    const int tileHeight = std::min(fixedHeight > 0 ? std::min(tileEdge, fixedHeight) : tileEdge, height);
    const int overlapX = std::min(spec.overlap, tileWidth - 1);
    const int overlapY = std::min(spec.overlap, tileHeight - 1);
    const std::vector<int> xs = tileStarts(width, tileWidth, overlapX);
    const std::vector<int> ys = tileStarts(height, tileHeight, overlapY);
    const int paddedWidth = fixedWidth > 0 ? fixedWidth : roundUp(tileWidth, spec.multipleOf);
    const int paddedHeight = fixedHeight > 0 ? fixedHeight : roundUp(tileHeight, spec.multipleOf);
    const bool padding = (paddedWidth != tileWidth) || (paddedHeight != tileHeight);
    const std::vector<int64_t> expected { 1, spec.outChannels, (int64_t)paddedHeight * scale, (int64_t)paddedWidth * scale };

    const int outWidth = width * scale;
    Tensor result = Tensor::image(1, spec.outChannels, (int64_t)height * scale, outWidth);
    std::vector<float> weightSum(result.size() / spec.outChannels, 0.f);
    Tensor tile = Tensor::image(1, spec.inChannels, tileHeight, tileWidth);
    Tensor padded = padding ? Tensor::image(1, spec.inChannels, paddedHeight, paddedWidth) : Tensor();

    *tilesRun = 0;
    for (std::size_t yi = 0; yi < ys.size(); ++yi) {
        const std::vector<float> weightY = axisWeights(ys, yi, tileHeight, scale);
        for (std::size_t xi = 0; xi < xs.size(); ++xi) {
            if (abort && abort()) {
                throw AiAborted();
            }
            const int row = ys[yi];
            const int col = xs[xi];
            const RectI rect(roi.x1 + col, roi.y2 - row - tileHeight, roi.x1 + col + tileWidth, roi.y2 - row);
            std::fill(tile.values().begin(), tile.values().end(), 0.f);
            pack(rect, tile);
            if (padding) {
                for (int c = 0; c < spec.inChannels; ++c) {
                    for (int y = 0; y < paddedHeight; ++y) {
                        const float* src = tile.plane(0, c) + (std::size_t)reflect(y, tileHeight) * tileWidth;
                        float* dst = padded.plane(0, c) + (std::size_t)y * paddedWidth;
                        for (int x = 0; x < paddedWidth; ++x) {
                            dst[x] = src[reflect(x, tileWidth)];
                        }
                    }
                }
            }

            const Tensor out = session.run(padding ? padded : tile, abort);
            if (out.shape() != expected) {
                throw AiError(session.modelPath() + ": a tile came back " + Tensor::shapeString(out.shape()) + ", not " + Tensor::shapeString(expected));
            }

            // A running weighted mean: a pixel every tile agrees on keeps its exact value.
            const std::vector<float> weightX = axisWeights(xs, xi, tileWidth, scale);
            const int outTileWidth = tileWidth * scale;
            const int outPaddedWidth = paddedWidth * scale;
            for (int y = 0; y < tileHeight * scale; ++y) {
                const std::size_t rowStart = (std::size_t)(row * scale + y) * outWidth + (std::size_t)col * scale;
                for (int x = 0; x < outTileWidth; ++x) {
                    const float w = weightY[y] * weightX[x];
                    float& sum = weightSum[rowStart + x];
                    sum += w;
                    const float f = sum > 0.f ? w / sum : 1.f;
                    for (int c = 0; c < spec.outChannels; ++c) {
                        float& dst = result.plane(0, c)[rowStart + x];
                        const float src = out.plane(0, c)[(std::size_t)y * outPaddedWidth + x];
                        dst += (src - dst) * f;
                    }
                }
            }
            ++*tilesRun;
        }
    }

    return result;
}

} // namespace

std::vector<int>
tileStarts(int length,
           int tile,
           int overlap)
{
    if (length <= tile) {
        return std::vector<int>(1, 0);
    }
    const int step = tile - overlap;
    const int count = std::max(2, (length - overlap + step - 1) / step);
    std::vector<int> starts(count);
    const long long span = length - tile;

    for (int i = 0; i < count; ++i) {
        starts[i] = (int)((i * span + (count - 1) / 2) / (count - 1));
    }

    return starts;
}

AiTiledRunInfo
runTiled(const std::shared_ptr<InferenceSession>& session,
         const RectI& roi,
         const AiTileSpec& spec,
         const AiTilePack& pack,
         const AiTileUnpack& unpack,
         const AiAbortPredicate& abort)
{
    if (!session) {
        throw AiError("tiled inference was given no session");
    }
    if ((spec.tile < 1) || (spec.overlap < 0) || (spec.overlap >= spec.tile) || (spec.multipleOf < 1) || (spec.scale < 1) || (spec.inChannels < 1) || (spec.outChannels < 1)) {
        throw AiError("invalid tile spec: tile " + std::to_string(spec.tile) + ", overlap " + std::to_string(spec.overlap) + ", multiple of " + std::to_string(spec.multipleOf) + ", scale " + std::to_string(spec.scale) + ", channels " + std::to_string(spec.inChannels) + " in " + std::to_string(spec.outChannels) + " out");
    }

    AiTiledRunInfo info;
    info.tile = spec.tile;
    if ((roi.width() <= 0) || (roi.height() <= 0)) {
        return info;
    }

    std::shared_ptr<InferenceSession> current = session;
    for (;;) {
        try {
            Tensor result = runPass(*current, roi, spec, info.tile, pack, abort, &info.tiles);
            const RectI outRoI(roi.x1 * spec.scale, roi.y1 * spec.scale, roi.x2 * spec.scale, roi.y2 * spec.scale);
            unpack(outRoI, result);

            return info;
        } catch (const AiOutOfMemory& e) {
            int fixedHeight = 0;
            int fixedWidth = 0;
            fixedInputSize(*current, spec, &fixedHeight, &fixedWidth);
            const int half = (info.tile / 2) / spec.multipleOf * spec.multipleOf;
            const int largestTile = std::max(std::min(info.tile, roi.width()), std::min(info.tile, roi.height()));
            const bool canHalve = !info.halved && (fixedHeight == 0) && (fixedWidth == 0) && (half >= spec.multipleOf) && (half > spec.overlap) && (half < largestTile);
            if (canHalve) {
                InferenceSession::logWarning(std::string(e.what()) + "; retrying with " + std::to_string(half) + " pixel tiles");
                info.halved = true;
                info.tile = half;
                continue;
            }
            if (current->isGpu()) {
                InferenceSession::logWarning(std::string(e.what()) + "; retrying on the CPU");
                AiSessionOptions cpu = current->requestedOptions();
                cpu.device = eAiDeviceCpu;
                current = InferenceSession::open(current->modelPath(), cpu, current->loader());
                info.cpuFallback = true;
                continue;
            }
            throw;
        }
    }
}

NATRON_NAMESPACE_EXIT
