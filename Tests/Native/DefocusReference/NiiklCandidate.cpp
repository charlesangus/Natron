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

#include "NiiklCandidate.h"

#include <cmath>
#include <cstddef>

#include "niikl/DefocusCore.h"

namespace DefocusReference {

namespace {

    defocus::ConstImageView
    viewOf(const std::vector<float>& data,
           int width,
           int height,
           int channels)
    {
        defocus::ConstImageView v;

        v.data = data.data();
        v.bounds = defocus::Rect(0, 0, width, height);
        v.channels = channels;
        v.rowStride = (std::ptrdiff_t)width * channels;

        return v;
    }

} // namespace

std::string
describe(const NiiklSettings& settings)
{
    std::string name;

    switch (settings.edgeMode) {
    case NiiklEdgeMode::Off:
        name = "off";
        break;
    case NiiklEdgeMode::Snap:
        name = "snap";
        break;
    case NiiklEdgeMode::Unmix:
        name = "unmix";
        break;
    }
    name += settings.fill ? "-fill" : "-nofill";
    name += settings.splitOcclusionEdges ? "-split" : "-fast";
    if (settings.positionNormal) {
        name += "-pn";
    }

    return name;
}

bool
runNiiklDefocus(const Camera& camera,
                const PinholeImage& pinhole,
                const NiiklSettings& settings,
                std::vector<float>* rgba,
                std::string* error)
{
    const int w = pinhole.width;
    const int h = pinhole.height;
    defocus::Inputs in;
    defocus::Settings s;

    if ((w <= 0) || (h <= 0) || (pinhole.rgba.size() < (std::size_t)w * h * 4) || (pinhole.depth.size() < (std::size_t)w * h)) {
        if (error) {
            *error = "empty pinhole image";
        }

        return false;
    }
    in.source = viewOf(pinhole.rgba, w, h, 4);
    in.depth = viewOf(pinhole.depth, w, h, 1);
    if (settings.positionNormal) {
        in.position = viewOf(pinhole.position, w, h, 3);
        in.positionSpace = defocus::eSpaceWorld;
        in.normal = viewOf(pinhole.normal, w, h, 3);
        in.normalSpace = defocus::eSpaceWorld;
        // World space is camera space here; the engine reads it through this camera.
        s.camera.valid = true;
        s.camera.focalPx = camera.imageDistance() * camera.pixelsPerMillimetre();
        s.camera.centreX = w / 2.;
        s.camera.centreY = h / 2.;
    }

    s.depthConvention = defocus::eDepthPlanarZ;
    s.zeroIsInfinity = true;
    s.worldToMm = camera.millimetresPerUnit;
    s.cocMode = defocus::eCocCamera;
    s.focalLengthMm = camera.focalLength;
    s.fNumber = camera.fStop;
    s.focusDistance = camera.focusDistance;
    s.filmbackWidthMm = camera.filmBackWidth;
    s.formatWidthPx = w;
    s.renderScale = 1.;
    s.maxRadiusPx = 1024.;
    s.opticalCentreX = w / 2.;
    s.opticalCentreY = h / 2.;
    s.fieldRadiusPx = 0.5 * std::sqrt((double)w * w + (double)h * h);
    s.edgeMode = (settings.edgeMode == NiiklEdgeMode::Off) ? defocus::eEdgeOff
                                                           : ((settings.edgeMode == NiiklEdgeMode::Snap) ? defocus::eEdgeMinMax : defocus::eEdgeUnmix);
    s.fillBehind = settings.fill;
    s.splitOcclusionEdges = settings.splitOcclusionEdges;
    s.border = defocus::eBorderRepeat;
    s.output = defocus::eOutputResult;
    s.useGpu = false;

    rgba->assign((std::size_t)w * h * 4, 0.f);
    defocus::ImageView out;
    out.data = rgba->data();
    out.bounds = defocus::Rect(0, 0, w, h);
    out.channels = 4;
    out.rowStride = (std::ptrdiff_t)w * 4;
    defocus::Stats stats;
    if (!defocus::render(in, s, out, nullptr, &stats, []() { return false; })) {
        if (error) {
            *error = "the scatter engine did not render";
        }

        return false;
    }

    return true;
} // runNiiklDefocus

} // namespace DefocusReference
