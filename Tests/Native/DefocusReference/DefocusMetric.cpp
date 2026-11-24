/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "DefocusMetric.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <OpenImageIO/imageio.h>

namespace DefocusReference {

namespace {

    double
    diameterOf(const Camera& camera,
               float depth)
    {
        return camera.cocDiameter((depth > 0.f) ? (double)depth : HUGE_VAL);
    }

    void
    markDisc(int width,
             int height,
             int cx,
             int cy,
             int radius,
             std::vector<unsigned char>* band)
    {
        for (int y = std::max(0, cy - radius); y <= std::min(height - 1, cy + radius); ++y) {
            for (int x = std::max(0, cx - radius); x <= std::min(width - 1, cx + radius); ++x) {
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius) {
                    (*band)[(std::size_t)y * width + x] = 1;
                }
            }
        }
    }

} // namespace

std::vector<unsigned char>
edgeBand(const Camera& camera,
         const std::vector<float>& depth)
{
    const int w = camera.width;
    const int h = camera.height;
    std::vector<unsigned char> band((std::size_t)w * h, 0);
    std::vector<double> diameter((std::size_t)w * h, 0.);

    for (std::size_t i = 0; i < diameter.size() && i < depth.size(); ++i) {
        diameter[i] = diameterOf(camera, depth[i]);
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double a = diameter[(std::size_t)y * w + x];
            const int neighbours[2][2] = { { x + 1, y }, { x, y + 1 } };
            for (const auto& n : neighbours) {
                if ((n[0] >= w) || (n[1] >= h)) {
                    continue;
                }
                const double b = diameter[(std::size_t)n[1] * w + n[0]];
                if (!(std::abs(a - b) > 1.)) {
                    continue;
                }
                const int radius = (int)std::ceil(std::max(std::abs(a), std::abs(b)) / 2.);
                markDisc(w, h, x, y, radius, &band);
                markDisc(w, h, n[0], n[1], radius, &band);
            }
        }
    }

    return band;
}

DefocusScore
scoreDefocus(int width,
             int height,
             const std::vector<float>& truth,
             const std::vector<float>& candidate,
             const std::vector<unsigned char>& band)
{
    DefocusScore score;
    const std::size_t n = (std::size_t)width * height;
    double sum = 0.;
    double edgeSum = 0.;

    if ((truth.size() < n * 4) || (candidate.size() < n * 4)) {
        score.rmse = score.edgeRmse = score.halo = score.hole = HUGE_VAL;

        return score;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const double alpha = std::min(1., std::max(0., (double)truth[i * 4 + 3]));
        double squared = 0.;
        for (int c = 0; c < 4; ++c) {
            const double d = (double)candidate[i * 4 + c] - truth[i * 4 + c];
            squared += d * d;
            if (c < 3) {
                score.halo += (1. - alpha) * std::max(0., d);
                score.hole += alpha * std::max(0., -d);
            }
        }
        sum += squared;
        if ((i < band.size()) && band[i]) {
            edgeSum += squared;
            ++score.edgePixels;
        }
    }
    score.rmse = std::sqrt(sum / (4. * n));
    score.edgeRmse = (score.edgePixels > 0) ? std::sqrt(edgeSum / (4. * score.edgePixels)) : 0.;
    score.halo /= n;
    score.hole /= n;

    return score;
}

std::vector<float>
differenceImage(const std::vector<float>& truth,
                const std::vector<float>& candidate)
{
    std::vector<float> d(std::min(truth.size(), candidate.size()));

    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = candidate[i] - truth[i];
        if (i % 4 == 3) {
            d[i] = std::abs(d[i]);
        }
    }

    return d;
}

void
edgeBandCentre(int width,
               int height,
               const std::vector<unsigned char>& band,
               int* x,
               int* y)
{
    double sx = 0., sy = 0.;
    std::size_t count = 0;

    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            const std::size_t k = (std::size_t)j * width + i;
            if ((k < band.size()) && band[k]) {
                sx += i;
                sy += j;
                ++count;
            }
        }
    }
    *x = count ? (int)(sx / count) : width / 2;
    *y = count ? (int)(sy / count) : height / 2;
}

bool
writeCrop(const std::string& path,
          int width,
          int height,
          const std::vector<float>& rgba,
          int centreX,
          int centreY,
          int size,
          std::string* error)
{
    const int cw = std::min(size, width);
    const int ch = std::min(size, height);
    const int x1 = std::max(0, std::min(width - cw, centreX - cw / 2));
    const int y1 = std::max(0, std::min(height - ch, centreY - ch / 2));

    if ((cw <= 0) || (ch <= 0) || (rgba.size() < (std::size_t)width * height * 4)) {
        if (error) {
            *error = "empty crop";
        }

        return false;
    }
    std::vector<float> pixels((std::size_t)cw * ch * 4);
    // OpenImageIO's first scanline is the top one; the image's first row is its bottom.
    for (int y = 0; y < ch; ++y) {
        const float* src = &rgba[((std::size_t)(y1 + ch - 1 - y) * width + x1) * 4];
        std::copy(src, src + (std::size_t)cw * 4, &pixels[(std::size_t)y * cw * 4]);
    }
    OIIO::ImageSpec spec(cw, ch, 4, OIIO::TypeDesc::HALF);
    spec.attribute("compression", "zip");
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    if (!out || !out->open(path, spec) || !out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) || !out->close()) {
        if (error) {
            *error = out ? out->geterror() : OIIO::geterror();
        }

        return false;
    }

    return true;
}

} // namespace DefocusReference
