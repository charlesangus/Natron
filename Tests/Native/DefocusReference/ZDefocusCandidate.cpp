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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ZDefocusCandidate.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

#include "Engine/AppInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/ZDefocus.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#include "NativeParity.h"
#include "RenderBothWays.h"

NATRON_NAMESPACE_ENTER

namespace {

std::mutex&
harnessImageMutex()
{
    static std::mutex m;

    return m;
}

std::shared_ptr<const DefocusReference::PinholeImage>&
harnessImageSlot()
{
    static std::shared_ptr<const DefocusReference::PinholeImage> image;

    return image;
}

std::shared_ptr<const DefocusReference::PinholeImage>
harnessImage()
{
    std::lock_guard<std::mutex> lock(harnessImageMutex());

    return harnessImageSlot();
}

int
setHarnessImage(const DefocusReference::PinholeImage& image)
{
    static int generation = 0;
    std::lock_guard<std::mutex> lock(harnessImageMutex());

    harnessImageSlot() = std::make_shared<const DefocusReference::PinholeImage>(image);

    return ++generation;
}

const double kRenderTime = 1.;

} // namespace

ImageLayerDesc
defocusHarnessDepthLayer()
{
    std::vector<std::string> rgb;

    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");

    return ImageLayerDesc(kDefocusHarnessDepthLayerID, rgb);
}

DefocusHarnessSourceTestEffect::DefocusHarnessSourceTestEffect(NodePtr n)
    : NativeEffectBase(n)
    , _generation()
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

void
DefocusHarnessSourceTestEffect::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
DefocusHarnessSourceTestEffect::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
    KnobIntPtr generation = createKnob<KnobInt>(std::string("Generation"));

    generation->setName(kDefocusHarnessSourceParamGeneration);
    generation->setAnimationEnabled(false);
    generation->setDefaultValue(0);
    generation->setIsMetadataSlave(true);
    page->addKnob(generation);
    _generation = generation;
}

StatusEnum
DefocusHarnessSourceTestEffect::getRegionOfDefinition(U64 /*hash*/,
                                                      double /*time*/,
                                                      const RenderScale& /*scale*/,
                                                      ViewIdx /*view*/,
                                                      RectD* rod)
{
    const std::shared_ptr<const DefocusReference::PinholeImage> image = harnessImage();

    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = image ? image->width : 0;
    rod->y2 = image ? image->height : 0;

    return eStatusOK;
}

StatusEnum
DefocusHarnessSourceTestEffect::getPreferredMetadataAt(double /*time*/,
                                                       ViewIdx /*view*/,
                                                       NodeMetadata& metadata)
{
    const std::shared_ptr<const DefocusReference::PinholeImage> image = harnessImage();

    metadata.setEdgeMode(EdgeMode::Stretch);
    metadata.setNComps(-1, 4);
    metadata.setBitDepth(-1, eImageBitDepthFloat);
    metadata.setPixelAspectRatio(-1, 1.);
    if (image) {
        metadata.setOutputFormat(RectI(0, 0, image->width, image->height));
    }

    return eStatusOK;
}

void
DefocusHarnessSourceTestEffect::getComponentsNeededAndProduced(double time,
                                                               ViewIdx view,
                                                               EffectInstance::ComponentsNeededMap* comps,
                                                               double* passThroughTime,
                                                               int* passThroughView,
                                                               int* passThroughInputNb)
{
    std::list<ImageLayerDesc>& produced = (*comps)[-1];

    produced.clear();
    produced.push_back(defocusHarnessDepthLayer());
    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInputNb = -1;
}

StatusEnum
DefocusHarnessSourceTestEffect::render(const RenderActionArgs& args)
{
    const std::shared_ptr<const DefocusReference::PinholeImage> source = harnessImage();
    const int step = 1 << args.mappedScale.toMipmapLevel();

    if (!source) {
        return eStatusFailed;
    }
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImageLayerDesc& layer = it->first;
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        const bool isColor = layer.isColorLayer();
        const bool isDepth = !isColor && (layer.getLayerID() == kDefocusHarnessDepthLayerID);
        if (!isColor && !isDepth) {
            image->fillZero(args.roi);
            continue;
        }
        const int nComps = static_cast<int>(image->getComponentsCount());
        Image::WriteAccess access(image.get());
        for (int y = args.roi.y1; y < args.roi.y2; ++y) {
            const int sy = std::min(source->height - 1, std::max(0, y * step + step / 2));
            for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                const int sx = std::min(source->width - 1, std::max(0, x * step + step / 2));
                const std::size_t i = (std::size_t)sy * source->width + sx;
                float* pixel = reinterpret_cast<float*>(access.pixelAt(x, y));
                if (!pixel) {
                    return eStatusFailed;
                }
                for (int c = 0; c < nComps; ++c) {
                    if (isDepth) {
                        pixel[c] = (c == 0) ? source->depth[i] : 0.f;
                    } else if (nComps == 1) {
                        pixel[c] = source->rgba[i * 4 + 3];
                    } else {
                        pixel[c] = source->rgba[i * 4 + std::min(c, 3)];
                    }
                }
            }
        }
    }

    return eStatusOK;
} // DefocusHarnessSourceTestEffect::render

ZDefocusCandidate::ZDefocusCandidate(const AppInstancePtr& app)
    : _app(app)
    , _source()
    , _zdefocus()
{
}

bool
ZDefocusCandidate::prepare(std::string* error)
{
    const auto fail = [error](const std::string& why) {
        if (error) {
            *error = why;
        }

        return false;
    };

    if (!_source) {
        _source = createNodeAtMajor(_app, kTestPluginIDDefocusHarnessSource, -1);
        _zdefocus = createNodeAtMajor(_app, PLUGINID_NATRON_ZDEFOCUS, PLUGIN_MAJOR_NATRON_ZDEFOCUS);
        if (!_source || !_zdefocus) {
            return fail("could not create the harness nodes");
        }
        if (!_app->getProject()->connectNodes(kZDefocusInputSource, _source, _zdefocus)) {
            return fail("could not connect the harness source to ZDefocus");
        }
        KnobChannelSelectPtr depth = std::dynamic_pointer_cast<KnobChannelSelect>(_zdefocus->getKnobByName(kZDefocusParamDepth));
        if (!depth) {
            return fail("ZDefocus has no depth channel knob");
        }
        depth->set(std::string(kDefocusHarnessDepthLayerID) + ".R");
        if (!setKnobValue(_zdefocus, kZDefocusParamDepthUnits, std::string(kZDefocusParamDepthUnitsDistance))
            || !setKnobValue(_zdefocus, kZDefocusParamMode, std::string(kZDefocusParamModeLens))
            || !setKnobValue(_zdefocus, kZDefocusParamSceneUnit, std::string(kZDefocusParamSceneUnitMetres))
            || !setKnobValue(_zdefocus, kEdgeModeParam, std::string("stretch"))
            || !setKnobValue(_zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputResult))) {
            return fail("could not set up ZDefocus");
        }
    }

    return true;
} // ZDefocusCandidate::prepare

bool
ZDefocusCandidate::render(const DefocusReference::Camera& camera,
                          const DefocusReference::PinholeImage& pinhole,
                          int layers,
                          std::vector<float>* rgba,
                          std::string* error)
{
    if (!prepare(error)) {
        return false;
    }
    const auto fail = [error](const std::string& why) {
        if (error) {
            *error = why;
        }

        return false;
    };

    if (camera.millimetresPerUnit != 1000.) {
        return fail("the ZDefocus candidate takes scenes in metres");
    }

    // Max Size spaces the slices, so it is the largest diameter the image holds, not the cap.
    double maxDiameter = 1.;
    for (float d : pinhole.depth) {
        if (d > 0.f) {
            maxDiameter = std::max(maxDiameter, std::abs(camera.cocDiameter(d)));
        }
    }
    const int generation = setHarnessImage(pinhole);
    if (!setKnobValues(_source, kDefocusHarnessSourceParamGeneration, { (double)generation })
        || !setKnobValues(_zdefocus, kZDefocusParamFocalPlane, { camera.focusDistance })
        || !setKnobValues(_zdefocus, kZDefocusParamFocalLength, { camera.focalLength })
        || !setKnobValues(_zdefocus, kZDefocusParamFStop, { camera.fStop })
        || !setKnobValues(_zdefocus, kZDefocusParamFilmBackWidth, { camera.filmBackWidth })
        || !setKnobValues(_zdefocus, kZDefocusParamMaxSize, { std::min(500., std::ceil(maxDiameter)) })
        || !setKnobValues(_zdefocus, kZDefocusParamLayers, { (double)layers })) {
        return fail("could not set the ZDefocus lens");
    }

    std::list<ImageLayerDesc> planesWanted;
    std::vector<RenderedPlane> planes;
    std::string renderError;
    const RectI window(0, 0, pinhole.width, pinhole.height);
    planesWanted.push_back(ImageLayerDesc::getRGBAComponents());
    if (!renderNodePlanesDirect(_zdefocus, kRenderTime, ViewIdx(0), 0, window, planesWanted, &planes, &renderError)) {
        return fail(renderError);
    }
    if ((planes.size() != 1) || (planes[0].channels.size() != 4) || (planes[0].window != window)) {
        return fail("ZDefocus did not render one RGBA plane over the image");
    }
    *rgba = planes[0].pixels;

    return true;
} // ZDefocusCandidate::render

NATRON_NAMESPACE_EXIT
