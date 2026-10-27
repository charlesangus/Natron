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

/*
 * Ported from darktable master (commit fee19b421fdb), src/common/ai/segmentation.c, and the mask
 * refinement of src/develop/masks/object.c (_keep_seed_components, _find_peak_point,
 * _compute_bbox, _mask_iou). darktable is distributed under the GNU GPL, version 3 or (at your
 * option) any later version; this file is therefore GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2026 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Segmentation.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <set>

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

NATRON_NAMESPACE_ENTER

namespace {

const int kDefaultInputSize = 1024;
const int kDefaultPrevMaskSize = 256;

// ImageNet normalisation, which SAM expects outside the graph; SegNext bakes it in.
const float kImageMean[3] = { 123.675f, 116.28f, 103.53f };
const float kImageStd[3] = { 58.395f, 57.12f, 57.375f };

// Areas keep their soft edge down to this probability.
const float kAreaFloor = 0.05f;

int
attributeInt(const ModelConfig& config,
             const char* name,
             int fallback)
{
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(config.attributesJson));

    if (!document.isObject()) {
        return fallback;
    }
    const int value = document.object().value(QString::fromLatin1(name)).toInt(0);

    return (value > 0) ? value : fallback;
}

std::shared_ptr<InferenceSession>
openSession(const std::string& path,
            const AiSessionOptions& options,
            const std::shared_ptr<const OrtLoader>& loader)
{
    return loader ? InferenceSession::open(path, options, loader) : InferenceSession::open(path, options);
}

inline int
luma709(const uint8_t* p)
{
    return (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
}

inline float
bilinear4(float v00,
          float v01,
          float v10,
          float v11,
          float fx,
          float fy)
{
    return v00 * (1.f - fx) * (1.f - fy) + v01 * fx * (1.f - fy) + v10 * (1.f - fx) * fy + v11 * fx * fy;
}

/*
 * Crops the valid (unpadded) part of a decoder mask and upsamples it to the encoded image, sampling
 * on pixel centres. With a guide of the destination's size, the upsampling is joint-bilateral on
 * the guide's luma, so the soft edge follows the image's.
 */
void
cropResizeMask(const float* src,
               int srcW,
               int srcH,
               float* dst,
               int dstW,
               int dstH,
               float scale,
               bool applySigmoid,
               const uint8_t* guide,
               int guideW,
               int guideH,
               float sigmaRange)
{
    const int validW = std::max(1, std::min((int)(dstW * scale + 0.5f), srcW));
    const int validH = std::max(1, std::min((int)(dstH * scale + 0.5f), srcH));
    const bool jointBilateral = guide && (sigmaRange > 1e-4f) && (guideW == dstW) && (guideH == dstH);

    float rangeLut[256];
    if (jointBilateral) {
        const float inv2Sigma2 = 1.f / (2.f * sigmaRange * sigmaRange);
        for (int i = 0; i < 256; ++i) {
            const float d = (float)i / 255.f;
            rangeLut[i] = std::exp(-d * d * inv2Sigma2);
        }
    }
    const float invScale = (scale > 1e-6f) ? 1.f / scale : 0.f;

    for (int y = 0; y < dstH; ++y) {
        // The clamp keeps fy >= 0 and makes the cast a floor.
        const float sy = std::max(((float)y + 0.5f) * scale - 0.5f, 0.f);
        const int y0 = std::min((int)sy, validH - 1);
        const int y1 = std::min(y0 + 1, validH - 1);
        const float fy = sy - (float)y0;

        for (int x = 0; x < dstW; ++x) {
            const float sx = std::max(((float)x + 0.5f) * scale - 0.5f, 0.f);
            const int x0 = std::min((int)sx, validW - 1);
            const int x1 = std::min(x0 + 1, validW - 1);
            const float fx = sx - (float)x0;

            const float v00 = src[y0 * srcW + x0];
            const float v01 = src[y0 * srcW + x1];
            const float v10 = src[y1 * srcW + x0];
            const float v11 = src[y1 * srcW + x1];

            float value;
            if (!jointBilateral) {
                value = bilinear4(v00, v01, v10, v11, fx, fy);
            } else {
                const int lumaRef = luma709(&guide[((size_t)y * guideW + x) * 3]);
                // The guide pixels the four mask samples come from: the inverse of the mapping above.
                const int gx0 = std::min(std::max((int)(((float)x0 + 0.5f) * invScale - 0.5f + 0.5f), 0), guideW - 1);
                const int gx1 = std::min(std::max((int)(((float)x1 + 0.5f) * invScale - 0.5f + 0.5f), 0), guideW - 1);
                const int gy0 = std::min(std::max((int)(((float)y0 + 0.5f) * invScale - 0.5f + 0.5f), 0), guideH - 1);
                const int gy1 = std::min(std::max((int)(((float)y1 + 0.5f) * invScale - 0.5f + 0.5f), 0), guideH - 1);

                const float r00 = rangeLut[std::abs(lumaRef - luma709(&guide[((size_t)gy0 * guideW + gx0) * 3]))];
                const float r01 = rangeLut[std::abs(lumaRef - luma709(&guide[((size_t)gy0 * guideW + gx1) * 3]))];
                const float r10 = rangeLut[std::abs(lumaRef - luma709(&guide[((size_t)gy1 * guideW + gx0) * 3]))];
                const float r11 = rangeLut[std::abs(lumaRef - luma709(&guide[((size_t)gy1 * guideW + gx1) * 3]))];

                const float w00 = (1.f - fx) * (1.f - fy) * r00;
                const float w01 = fx * (1.f - fy) * r01;
                const float w10 = (1.f - fx) * fy * r10;
                const float w11 = fx * fy * r11;
                const float wsum = w00 + w01 + w10 + w11;

                value = (wsum > 1e-6f) ? (v00 * w00 + v01 * w01 + v10 * w10 + v11 * w11) / wsum
                                       : bilinear4(v00, v01, v10, v11, fx, fy);
            }
            if (applySigmoid) {
                value = 1.f / (1.f + std::exp(-value));
            }
            dst[(size_t)y * dstW + x] = value;
        }
    }
} // cropResizeMask

// Bilinear resampling of a whole plane onto another grid, on pixel centres.
void
resamplePlane(const float* src,
              int srcW,
              int srcH,
              float* dst,
              int dstW,
              int dstH)
{
    if ((srcW == dstW) && (srcH == dstH)) {
        std::memcpy(dst, src, sizeof(float) * (size_t)dstW * dstH);

        return;
    }
    const float sxScale = (float)srcW / (float)dstW;
    const float syScale = (float)srcH / (float)dstH;
    for (int y = 0; y < dstH; ++y) {
        const float sy = std::min(std::max(((float)y + 0.5f) * syScale - 0.5f, 0.f), (float)(srcH - 1));
        const int y0 = (int)sy;
        const int y1 = std::min(y0 + 1, srcH - 1);
        for (int x = 0; x < dstW; ++x) {
            const float sx = std::min(std::max(((float)x + 0.5f) * sxScale - 0.5f, 0.f), (float)(srcW - 1));
            const int x0 = (int)sx;
            const int x1 = std::min(x0 + 1, srcW - 1);
            dst[(size_t)y * dstW + x] = bilinear4(src[y0 * srcW + x0], src[y0 * srcW + x1],
                                                  src[y1 * srcW + x0], src[y1 * srcW + x1],
                                                  sx - (float)x0, sy - (float)y0);
        }
    }
}

float
maskIou(const std::vector<float>& a,
        const std::vector<float>& b,
        float threshold)
{
    std::size_t intersection = 0, unionCount = 0;
    const std::size_t n = std::min(a.size(), b.size());

    for (std::size_t i = 0; i < n; ++i) {
        const bool inA = a[i] > threshold;
        const bool inB = b[i] > threshold;
        intersection += (inA && inB) ? 1 : 0;
        unionCount += (inA || inB) ? 1 : 0;
    }

    return unionCount ? (float)intersection / (float)unionCount : 0.f;
}

/*
 * The deepest interior pixel of mask > threshold, away from the positive points. darktable uses an
 * exact Euclidean distance transform; a 3-4 chamfer is close enough to pick an interior point.
 */
bool
findPeakPoint(const std::vector<float>& mask,
              int w,
              int h,
              float threshold,
              const std::vector<SegmentationPoint>& exclude,
              float minSeparation,
              SegmentationPoint* out)
{
    const float kFar = 1e9f;
    std::vector<float> dist((size_t)w * h);

    for (std::size_t i = 0; i < dist.size(); ++i) {
        dist[i] = (mask[i] > threshold) ? kFar : 0.f;
    }
    // Outside the image counts as background.
    auto at = [&](int x, int y) {
        return ((x < 0) || (y < 0) || (x >= w) || (y >= h)) ? 0.f : dist[(size_t)y * w + x];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float& d = dist[(size_t)y * w + x];
            if (d == 0.f) {
                continue;
            }
            d = std::min({ d, at(x - 1, y) + 1.f, at(x, y - 1) + 1.f, at(x - 1, y - 1) + 1.4142f, at(x + 1, y - 1) + 1.4142f });
        }
    }
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) {
            float& d = dist[(size_t)y * w + x];
            if (d == 0.f) {
                continue;
            }
            d = std::min({ d, at(x + 1, y) + 1.f, at(x, y + 1) + 1.f, at(x + 1, y + 1) + 1.4142f, at(x - 1, y + 1) + 1.4142f });
        }
    }

    const float minSeparationSq = minSeparation * minSeparation;
    for (const SegmentationPoint& p : exclude) {
        if (p.label != SegmentationPoint::eLabelPositive) {
            continue;
        }
        const int x0 = std::max(0, (int)(p.x - minSeparation));
        const int x1 = std::min(w - 1, (int)(p.x + minSeparation));
        const int y0 = std::max(0, (int)(p.y - minSeparation));
        const int y1 = std::min(h - 1, (int)(p.y + minSeparation));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = (float)x - p.x;
                const float dy = (float)y - p.y;
                if (dx * dx + dy * dy < minSeparationSq) {
                    dist[(size_t)y * w + x] = 0.f;
                }
            }
        }
    }

    // Shallower peaks than this say nothing the mask does not.
    float best = 4.f;
    std::size_t bestIndex = (std::size_t)-1;
    for (std::size_t i = 0; i < dist.size(); ++i) {
        if (dist[i] > best) {
            best = dist[i];
            bestIndex = i;
        }
    }
    if (bestIndex == (std::size_t)-1) {
        return false;
    }
    out->x = (float)(bestIndex % (std::size_t)w);
    out->y = (float)(bestIndex / (std::size_t)w);
    out->label = SegmentationPoint::eLabelPositive;

    return true;
} // findPeakPoint

bool
computeBox(const std::vector<float>& mask,
           int w,
           int h,
           float threshold,
           float padding,
           SegmentationPoint* topLeft,
           SegmentationPoint* bottomRight)
{
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (mask[(size_t)y * w + x] > threshold) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX == INT_MIN) {
        return false;
    }
    const int padX = (int)((maxX - minX) * padding) + 1;
    const int padY = (int)((maxY - minY) * padding) + 1;
    topLeft->x = (float)std::min(std::max(minX - padX, 0), w - 1);
    topLeft->y = (float)std::min(std::max(minY - padY, 0), h - 1);
    topLeft->label = SegmentationPoint::eLabelBoxTopLeft;
    bottomRight->x = (float)std::min(std::max(maxX + padX, 0), w - 1);
    bottomRight->y = (float)std::min(std::max(maxY + padY, 0), h - 1);
    bottomRight->label = SegmentationPoint::eLabelBoxBottomRight;

    return true;
}

void
appendBoxes(const std::vector<SegmentationBox>& boxes,
            std::vector<SegmentationPoint>* points)
{
    for (const SegmentationBox& b : boxes) {
        SegmentationPoint tl, br;
        tl.x = std::min(b.x1, b.x2);
        tl.y = std::min(b.y1, b.y2);
        tl.label = SegmentationPoint::eLabelBoxTopLeft;
        br.x = std::max(b.x1, b.x2);
        br.y = std::max(b.y1, b.y2);
        br.label = SegmentationPoint::eLabelBoxBottomRight;
        points->push_back(tl);
        points->push_back(br);
    }
}

} // namespace

Segmenter::Arch
Segmenter::archFromConfig(const ModelConfig& config)
{
    if ((config.arch == "sam2") || (config.arch == "sam")) {
        return Arch::eSam;
    }
    if (config.arch == "segnext") {
        return Arch::eSegNext;
    }
    throw AiError("The model " + config.id + " has the architecture \"" + config.arch + "\", which is not a segmentation one.");
}

Segmenter::Segmenter(const std::string& encoderPath,
                     const std::string& decoderPath,
                     const ModelConfig& config,
                     const AiSessionOptions& encoderOptions,
                     const std::shared_ptr<const OrtLoader>& loader)
    : _arch(archFromConfig(config))
{
    _inputSize = attributeInt(config, "input_size", kDefaultInputSize);
    _encoder = openSession(encoderPath, encoderOptions, loader);

    // The decoder is light enough that a GPU costs more than it saves, and some decoder graphs
    // (SegNext's Concat->Reshape) break under ONNX Runtime's graph optimisation.
    AiSessionOptions decoderOptions;
    decoderOptions.device = eAiDeviceCpu;
    decoderOptions.threads = encoderOptions.threads;
    decoderOptions.optimization = AiGraphOptimization::eDisabled;
    _decoder = openSession(decoderPath, decoderOptions, loader);

    const std::vector<AiTensorInfo>& encOutputs = _encoder->outputs();
    const std::vector<AiTensorInfo>& decInputs = _decoder->inputs();
    const std::vector<AiTensorInfo>& decOutputs = _decoder->outputs();
    const std::size_t nEnc = encOutputs.size();
    const std::size_t nPrompt = (_arch == Arch::eSam) ? 4 : 3;
    if ((_encoder->inputs().size() != 1) || (nEnc == 0)) {
        throw AiError(encoderPath + " is not a segmentation encoder: it must take one image and return at least one tensor.");
    }
    if (decInputs.size() != nEnc + nPrompt) {
        throw AiError(decoderPath + " takes " + std::to_string(decInputs.size()) + " inputs where its encoder's "
                      + std::to_string(nEnc) + " outputs and " + std::to_string(nPrompt) + " prompt tensors were expected.");
    }
    if (decOutputs.size() < ((_arch == Arch::eSam) ? 2u : 1u)) {
        throw AiError(decoderPath + " has too few outputs for its architecture.");
    }

    // Encoder outputs may come in another order than the decoder takes them, and under longer
    // names ("image_embeddings" for "image_embed"): exact matches first, then substrings.
    std::vector<bool> used(nEnc, false);
    for (std::size_t di = 0; di < nEnc; ++di) {
        const std::string& decName = decInputs[di].name;
        int best = -1;
        for (std::size_t ei = 0; ei < nEnc; ++ei) {
            if (used[ei]) {
                continue;
            }
            const std::string& encName = encOutputs[ei].name;
            if (encName == decName) {
                best = (int)ei;
                break;
            }
            if ((best < 0) && ((encName.find(decName) != std::string::npos) || (decName.find(encName) != std::string::npos))) {
                best = (int)ei;
            }
        }
        if (best < 0) {
            best = (int)di;
        }
        used[best] = true;
        _routing.push_back(std::make_pair(encOutputs[best].name, decName));
    }
    _coordsInput = decInputs[nEnc].name;
    _labelsInput = decInputs[nEnc + 1].name;
    _maskInput = decInputs[nEnc + 2].name;
    if (_arch == Arch::eSam) {
        _hasMaskInput = decInputs[nEnc + 3].name;
    }

    // The mask feedback grid: what the decoder declares it takes, else what darktable infers.
    const std::vector<int64_t>& maskShape = decInputs[nEnc + 2].shape;
    if ((maskShape.size() == 4) && (maskShape[2] > 0)) {
        _prevMaskDim = (int)maskShape[2];
    } else if (_arch == Arch::eSam) {
        _prevMaskDim = attributeInt(config, "prev_mask_size", kDefaultPrevMaskSize);
        if ((decOutputs.size() >= 3) && (decOutputs[2].shape.size() == 4) && (decOutputs[2].shape[2] > 0)) {
            _prevMaskDim = (int)decOutputs[2].shape[2];
        }
    } else {
        const std::vector<int64_t>& outShape = decOutputs[0].shape;
        _prevMaskDim = ((outShape.size() == 4) && (outShape[2] > 0)) ? (int)outShape[2] : _inputSize;
    }
    _prevMask.assign((size_t)_prevMaskDim * _prevMaskDim, 0.f);
}

Segmenter::~Segmenter()
{
}

Tensor
Segmenter::preprocess(const uint8_t* rgb8,
                      int width,
                      int height,
                      float* outScale) const
{
    const int target = _inputSize;
    const float scale = (float)target / (float)std::max(width, height);
    const int newW = std::min((int)(width * scale + 0.5f), target);
    const int newH = std::min((int)(height * scale + 0.5f), target);
    const bool normalize = (_arch == Arch::eSam);

    *outScale = scale;
    // Zero padding right and below the image, which sits top-left.
    Tensor output = Tensor::image(1, 3, target, target);
    float* out = output.data();
    const std::size_t plane = (std::size_t)target * target;

    for (int y = 0; y < newH; ++y) {
        // Pixel-centre sampling: y / scale would shift the image by 0.5 / scale - 0.5 source pixels,
        // which the encoder bakes into the mask.
        const float srcY = std::max(((float)y + 0.5f) / scale - 0.5f, 0.f);
        const int y0 = std::min((int)srcY, height - 1);
        const int y1 = (y0 + 1 < height) ? y0 + 1 : y0;
        const float fy = srcY - (float)y0;
        for (int x = 0; x < newW; ++x) {
            const float srcX = std::max(((float)x + 0.5f) / scale - 0.5f, 0.f);
            const int x0 = std::min((int)srcX, width - 1);
            const int x1 = (x0 + 1 < width) ? x0 + 1 : x0;
            const float fx = srcX - (float)x0;
            for (int c = 0; c < 3; ++c) {
                const float value = bilinear4(rgb8[((size_t)y0 * width + x0) * 3 + c], rgb8[((size_t)y0 * width + x1) * 3 + c],
                                              rgb8[((size_t)y1 * width + x0) * 3 + c], rgb8[((size_t)y1 * width + x1) * 3 + c],
                                              fx, fy);
                out[c * plane + (size_t)y * target + x] = normalize ? (value - kImageMean[c]) / kImageStd[c] : value / 255.f;
            }
        }
    }

    return output;
}

bool
Segmenter::encode(const uint8_t* rgb8,
                  int width,
                  int height,
                  const SegmentationKey& key,
                  const AiAbortPredicate& abort)
{
    if (!rgb8 || (width <= 0) || (height <= 0)) {
        throw AiError("Segmenter::encode: empty image");
    }
    if (select(key)) {
        return true;
    }

    // The encoder runs unlocked: it can take seconds, and decodes of the current encoding may go on.
    std::shared_ptr<Encoding> encoding = std::make_shared<Encoding>();
    encoding->key = key;
    encoding->width = width;
    encoding->height = height;
    {
        std::map<std::string, Tensor> inputs;
        inputs[_encoder->inputs()[0].name] = preprocess(rgb8, width, height, &encoding->scale);
        encoding->outputs = _encoder->run(inputs, abort);
    }
    for (const std::pair<std::string, std::string>& route : _routing) {
        if (encoding->outputs.find(route.first) == encoding->outputs.end()) {
            throw AiError("The segmentation encoder did not return " + route.first + ".");
        }
    }
    encoding->rgb.assign(rgb8, rgb8 + (size_t)width * height * 3);

    std::lock_guard<std::mutex> lock(_mutex);
    ++_encoderRuns;
    for (std::list<std::shared_ptr<Encoding>>::iterator it = _cache.begin(); it != _cache.end(); ++it) {
        if ((*it)->key == key) {
            _cache.erase(it);
            break;
        }
    }
    _cache.push_front(encoding);
    while (_cache.size() > kCacheCapacity) {
        _cache.pop_back();
    }
    makeCurrent(_cache.begin());

    return false;
} // Segmenter::encode

void
Segmenter::makeCurrent(std::list<std::shared_ptr<Encoding>>::iterator it)
{
    std::shared_ptr<Encoding> encoding = *it;

    if (it != _cache.begin()) {
        _cache.erase(it);
        _cache.push_front(encoding);
    }
    if (_current != encoding) {
        _current = encoding;
        _hasPrevMask = false;
        std::fill(_prevMask.begin(), _prevMask.end(), 0.f);
    }
}

bool
Segmenter::select(const SegmentationKey& key)
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (std::list<std::shared_ptr<Encoding>>::iterator it = _cache.begin(); it != _cache.end(); ++it) {
        if ((*it)->key == key) {
            ++_cacheHits;
            makeCurrent(it);

            return true;
        }
    }

    return false;
}

bool
Segmenter::isCached(const SegmentationKey& key) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (const std::shared_ptr<Encoding>& encoding : _cache) {
        if (encoding->key == key) {
            return true;
        }
    }

    return false;
}

bool
Segmenter::hasEncoding() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return bool(_current);
}

SegmentationKey
Segmenter::currentKey() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _current ? _current->key : SegmentationKey();
}

int
Segmenter::encodedWidth() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _current ? _current->width : 0;
}

int
Segmenter::encodedHeight() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _current ? _current->height : 0;
}

std::size_t
Segmenter::encoderRuns() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _encoderRuns;
}

std::size_t
Segmenter::cacheHits() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _cacheHits;
}

void
Segmenter::resetRefinement()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _hasPrevMask = false;
    std::fill(_prevMask.begin(), _prevMask.end(), 0.f);
}

std::vector<float>
Segmenter::decode(const std::vector<SegmentationPoint>& points,
                  const std::vector<SegmentationBox>& boxes)
{
    if (!boxes.empty() && !supportsBoxes()) {
        throw AiError("This model does not take box prompts: use a SAM 2.1 model.");
    }
    std::vector<SegmentationPoint> all = points;
    appendBoxes(boxes, &all);

    std::lock_guard<std::mutex> lock(_mutex);

    return decodeLocked(all);
}

std::vector<float>
Segmenter::decodeLocked(const std::vector<SegmentationPoint>& points)
{
    if (!_current) {
        throw AiError("Segmenter::decode: no image is encoded");
    }
    if (points.empty()) {
        throw AiError("Segmenter::decode: no prompts");
    }
    const Encoding& enc = *_current;
    const bool sam = (_arch == Arch::eSam);

    // SAM's ONNX export wants a padding point (0, 0) labelled -1 after the prompts.
    const int64_t total = (int64_t)points.size() + (sam ? 1 : 0);
    Tensor coords(std::vector<int64_t> { 1, total, 2 });
    Tensor labels(std::vector<int64_t> { 1, total });
    for (std::size_t i = 0; i < points.size(); ++i) {
        coords.data()[i * 2] = points[i].x * enc.scale;
        coords.data()[i * 2 + 1] = points[i].y * enc.scale;
        labels.data()[i] = (float)points[i].label;
    }
    if (sam) {
        labels.data()[points.size()] = -1.f;
    }

    std::map<std::string, Tensor> inputs;
    for (const std::pair<std::string, std::string>& route : _routing) {
        inputs[route.second] = enc.outputs.at(route.first);
    }
    inputs[_coordsInput] = std::move(coords);
    inputs[_labelsInput] = std::move(labels);
    inputs[_maskInput] = Tensor(std::vector<int64_t> { 1, 1, _prevMaskDim, _prevMaskDim }, _prevMask);
    if (sam) {
        inputs[_hasMaskInput] = Tensor(std::vector<int64_t> { 1 }, std::vector<float> { _hasPrevMask ? 1.f : 0.f });
    }

    std::map<std::string, Tensor> outputs = _decoder->run(inputs);
    const std::vector<AiTensorInfo>& outInfo = _decoder->outputs();
    const Tensor& masks = outputs.at(outInfo[0].name);
    if ((masks.rank() != 4) || (masks.dim(1) < 1)) {
        throw AiError("The segmentation decoder returned a mask shaped " + Tensor::shapeString(masks.shape()) + ".");
    }
    const int nMasks = (int)masks.dim(1);
    const int decH = (int)masks.dim(2);
    const int decW = (int)masks.dim(3);
    const std::size_t perMask = (std::size_t)decH * decW;

    int best = 0;
    if (sam) {
        const Tensor& iou = outputs.at(outInfo[1].name);
        const int n = std::min(nMasks, (int)iou.size());
        for (int m = 1; m < n; ++m) {
            if (iou.data()[m] > iou.data()[best]) {
                best = m;
            }
        }
        // The low-resolution logits feed the next pass; a 256x256 decoder has none, and its masks
        // already are at that resolution.
        if (outInfo.size() >= 3) {
            const Tensor& lowRes = outputs.at(outInfo[2].name);
            if ((lowRes.rank() == 4) && (lowRes.dim(1) > best)) {
                const int lh = (int)lowRes.dim(2), lw = (int)lowRes.dim(3);
                resamplePlane(lowRes.data() + (size_t)best * lh * lw, lw, lh, _prevMask.data(), _prevMaskDim, _prevMaskDim);
            }
        } else {
            resamplePlane(masks.data() + best * perMask, decW, decH, _prevMask.data(), _prevMaskDim, _prevMaskDim);
        }
    } else {
        resamplePlane(masks.data(), decW, decH, _prevMask.data(), _prevMaskDim, _prevMaskDim);
    }
    _hasPrevMask = true;

    std::vector<float> result((size_t)enc.width * enc.height);
    const float maskScale = enc.scale * (float)decH / (float)_inputSize;
    // SegNext returns probabilities, SAM logits.
    cropResizeMask(masks.data() + best * perMask, decW, decH,
                   result.data(), enc.width, enc.height,
                   maskScale, sam,
                   enc.rgb.empty() ? nullptr : enc.rgb.data(), enc.width, enc.height,
                   0.1f);

    return result;
} // Segmenter::decodeLocked

std::vector<float>
Segmenter::segment(const std::vector<SegmentationPoint>& points,
                   const std::vector<SegmentationBox>& boxes,
                   float threshold,
                   int passes)
{
    if (!boxes.empty() && !supportsBoxes()) {
        throw AiError("This model does not take box prompts: use a SAM 2.1 model.");
    }
    std::vector<SegmentationPoint> prompts = points;
    appendBoxes(boxes, &prompts);

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_current) {
        throw AiError("Segmenter::segment: no image is encoded");
    }
    _hasPrevMask = false;
    std::fill(_prevMask.begin(), _prevMask.end(), 0.f);

    const int w = _current->width;
    const int h = _current->height;
    std::vector<SegmentationPoint> all = prompts;
    std::vector<float> mask;
    bool boxAdded = !boxes.empty();
    passes = std::max(1, std::min(passes, 3));

    for (int pass = 0; pass < passes; ++pass) {
        std::vector<float> next = decodeLocked(all);
        const bool settled = !mask.empty() && (maskIou(mask, next, threshold) > 0.99f);
        mask.swap(next);
        if (settled || (pass + 1 >= passes)) {
            break;
        }
        bool added = false;
        SegmentationPoint peak;
        if (findPeakPoint(mask, w, h, threshold, all, 8.f, &peak)) {
            all.push_back(peak);
            added = true;
        }
        if (supportsBoxes() && !boxAdded) {
            SegmentationPoint tl, br;
            if (computeBox(mask, w, h, threshold, 0.05f, &tl, &br)) {
                all.push_back(tl);
                all.push_back(br);
                boxAdded = true;
                added = true;
            }
        }
        if (!added) {
            break;
        }
    }
    keepSeedAreas(mask, w, h, threshold, prompts);

    return mask;
} // Segmenter::segment

void
Segmenter::warmUpDecoder()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_current) {
        return;
    }
    const std::vector<float> savedMask = _prevMask;
    const bool savedHas = _hasPrevMask;
    SegmentationPoint origin;
    try {
        (void)decodeLocked(std::vector<SegmentationPoint>(1, origin));
    } catch (const AiError& e) {
        InferenceSession::logWarning(std::string("Segmentation decoder warm-up failed: ") + e.what());
    }
    _prevMask = savedMask;
    _hasPrevMask = savedHas;
}

void
Segmenter::keepSeedAreas(std::vector<float>& mask,
                         int w,
                         int h,
                         float threshold,
                         const std::vector<SegmentationPoint>& points)
{
    const std::size_t npix = (std::size_t)w * h;

    if ((mask.size() != npix) || (npix == 0)) {
        return;
    }
    // 0: unassigned; positive: area label.
    std::vector<int> labels(npix, 0);
    std::vector<std::size_t> stack;
    stack.reserve(1024);
    int nLabels = 0;
    int bestLabel = 0;
    std::size_t bestArea = 0;

    for (std::size_t i = 0; i < npix; ++i) {
        if ((mask[i] <= threshold) || (labels[i] != 0)) {
            continue;
        }
        const int label = ++nLabels;
        std::size_t area = 0;
        labels[i] = label;
        stack.push_back(i);
        while (!stack.empty()) {
            const std::size_t p = stack.back();
            stack.pop_back();
            ++area;
            const int px = (int)(p % w), py = (int)(p / w);
            const std::size_t neighbours[4] = { p - w, p + w, p - 1, p + 1 };
            const bool valid[4] = { py > 0, py < h - 1, px > 0, px < w - 1 };
            for (int k = 0; k < 4; ++k) {
                if (valid[k] && (labels[neighbours[k]] == 0) && (mask[neighbours[k]] > threshold)) {
                    labels[neighbours[k]] = label;
                    stack.push_back(neighbours[k]);
                }
            }
        }
        if (area > bestArea) {
            bestArea = area;
            bestLabel = label;
        }
    }
    if (!bestLabel) {
        return;
    }

    // Soft edges grow from every core at once, breadth first, so an edge pixel goes to the nearest
    // core and growth never merges two areas.
    std::vector<std::size_t> queue;
    queue.reserve(npix / 4 + 1);
    for (std::size_t i = 0; i < npix; ++i) {
        if (labels[i] > 0) {
            queue.push_back(i);
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t p = queue[head];
        const int label = labels[p];
        const int px = (int)(p % w), py = (int)(p / w);
        const std::size_t neighbours[4] = { p - w, p + w, p - 1, p + 1 };
        const bool valid[4] = { py > 0, py < h - 1, px > 0, px < w - 1 };
        for (int k = 0; k < 4; ++k) {
            if (valid[k] && (labels[neighbours[k]] == 0) && (mask[neighbours[k]] > kAreaFloor)) {
                labels[neighbours[k]] = label;
                queue.push_back(neighbours[k]);
            }
        }
    }

    std::set<int> kept;
    for (const SegmentationPoint& p : points) {
        if (p.label != SegmentationPoint::eLabelPositive) {
            continue;
        }
        const int sx = std::min(std::max((int)std::lrint(p.x), 0), w - 1);
        const int sy = std::min(std::max((int)std::lrint(p.y), 0), h - 1);
        const int label = labels[(size_t)sy * w + sx];
        if (label > 0) {
            kept.insert(label);
        }
    }
    if (kept.empty()) {
        kept.insert(bestLabel);
    }
    for (std::size_t i = 0; i < npix; ++i) {
        if (!kept.count(labels[i])) {
            mask[i] = 0.f;
        }
    }
} // Segmenter::keepSeedAreas

NATRON_NAMESPACE_EXIT
