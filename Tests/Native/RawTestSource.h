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

#ifndef NATRON_TESTS_NATIVE_RAWTESTSOURCE_H
#define NATRON_TESTS_NATIVE_RAWTESTSOURCE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/Plugin.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

#include "RenderBothWays.h"

#define kRawTestSourceID "test.natron.built-in.RawTestSource"

NATRON_NAMESPACE_ENTER

/**
 * @brief A test-only raw source: a mosaic (one channel) or linear data (several) whose pixel
 * values, region of definition and raw/ metadata a test sets, standing in for a Read of a camera
 * raw file until real files are decoded. Pixel (x, y) channel c is value(x, y, c) inside the
 * region and nothing outside it.
 **/
class RawTestSource
    : public NativeEffectBase {
public:
    using ValueFn = std::function<float(int x, int y, int c)>;

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new RawTestSource(n);
    }

    explicit RawTestSource(NodePtr n)
        : NativeEffectBase(n)
        , _mutex()
        , _rod(0, 0, 16, 16)
        , _nComps(1)
        , _value([](int, int, int) { return 0.f; })
        , _metadata()
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

    /**
     * @brief Sets everything the source outputs. A render that bypasses the cache sees the new
     * pixels; the metadata, the component count included, is derived again. Main thread only.
     **/
    void setRaw(const RectI& rod,
                int nComps,
                const ValueFn& value,
                const ImageMetadata& metadata)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _rod = rod;
            _nComps = nComps;
            _value = value;
            _metadata = metadata;
        }
        invalidateOutputMetadata();
        // The component count is in no hash, so the cached metadata of this node and of those
        // below it describe the old count.
        dropCachedMetadataDownstream();
        refreshMetadataSideEffects();
    }

    void setMetadata(const ImageMetadata& metadata)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _metadata = metadata;
        }
        invalidateOutputMetadata();
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        std::lock_guard<std::mutex> lock(_mutex);
        *rod = RectD(_rod.x1, _rod.y1, _rod.x2, _rod.y2);

        return eStatusOK;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kRawTestSourceID,
        .label = "Test Raw Source",
        .outputKind = eDataKindRaw,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }

    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        std::lock_guard<std::mutex> lock(_mutex);
        metadata.setNComps(-1, _nComps);
        metadata.setBitDepth(-1, eImageBitDepthFloat);
        metadata.setIsFrameVarying(false);

        return eStatusOK;
    }

    virtual ImageMetadata deriveOutputMetadata(double /*time*/,
                                               ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        std::lock_guard<std::mutex> lock(_mutex);

        return _metadata;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        RectI rod;
        ValueFn value;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            rod = _rod;
            value = _value;
        }
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image) {
                continue;
            }
            if (image->getBitDepth() != eImageBitDepthFloat) {
                return eStatusFailed;
            }
            const int nComps = (int)image->getComponentsCount();
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    const bool inside = (x >= rod.x1) && (x < rod.x2) && (y >= rod.y1) && (y < rod.y2);
                    for (int c = 0; c < nComps; ++c) {
                        pixel[c] = inside ? value(x, y, c) : 0.f;
                    }
                }
            }
        }

        return eStatusOK;
    }

    mutable std::mutex _mutex;
    RectI _rod;
    int _nComps;
    ValueFn _value;
    ImageMetadata _metadata;
};

/**
 * @brief Registers RawTestSource with the application's plug-ins, once per process.
 **/
inline void
registerRawTestSourceOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(RawTestSource::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;
        functions.insert(std::make_pair("BuildEffect", (void (*)())&RawTestSource::BuildEffect));
        LibraryBinary* binary = new LibraryBinary(functions);
        Plugin* p = appPTR->registerPlugin(QString(), QStringList(QString::fromUtf8("Other")), QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                           QString(), QStringList(), node->isReader(), node->isWriter(), binary, false, node->getMajorVersion(), node->getMinorVersion(), false);
        // Registered after AppManager::load(), so the label the script name is made from was
        // never assigned.
        p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
        p->setOpenGLRenderSupport(node->supportsOpenGLRender());
    });
}

/**
 * @brief raw/ metadata of a mosaic with pattern at origin (originX, originY) and the stage.
 **/
inline ImageMetadata
rawTestMetadata(const std::string& pattern,
                int originX,
                int originY,
                RawStageEnum stage)
{
    ImageMetadata metadata;
    const std::optional<RawLayout> layout = RawLayout::fromPattern(pattern, originX, originY);

    if (layout) {
        layout->writeTo(&metadata);
    }
    RawMetadata::setStage(&metadata, stage);

    return metadata;
}

/**
 * @brief Renders the colour layer of node over window. With byPassCache (the default) the render
 * bypasses the cache and empties every cache before and after, as renderNodePlanesDirect() does,
 * which purges the nodes too: a Read drops its decoded file. Without it the caches are left as
 * they are.
 **/
inline bool
renderRawTestPlane(const NodePtr& node,
                   const RectI& window,
                   RenderedPlane* plane,
                   std::string* error,
                   bool byPassCache = true)
{
    std::list<ImageLayerDesc> layers;

    node->getEffectInstance()->getPresentLayers(1., ViewIdx(0), -1, &layers);
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            std::list<ImageLayerDesc> colour(1, *it);
            std::vector<RenderedPlane> planes;
            if (!renderNodePlanesDirect(node, 1., ViewIdx(0), 0, window, colour, &planes, error, byPassCache) || (planes.size() != 1)) {
                return false;
            }
            *plane = planes[0];

            return true;
        }
    }
    if (error) {
        *error = "no colour layer";
    }

    return false;
}

/**
 * @brief Counts the whole-frame computations RawNodeSupport::noteFrameComputed() reports, by
 * name, from its construction to its destruction, which removes the hook.
 **/
class FrameComputedCounter {
public:
    FrameComputedCounter()
    {
        RawNodeSupport::setFrameComputedHookForTests([this](const char* what, U64) {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_counts[what];
        });
    }

    ~FrameComputedCounter()
    {
        RawNodeSupport::setFrameComputedHookForTests(std::function<void(const char*, U64)>());
    }

    FrameComputedCounter(const FrameComputedCounter&) = delete;
    FrameComputedCounter& operator=(const FrameComputedCounter&) = delete;

    std::map<std::string, int> counts() const
    {
        std::lock_guard<std::mutex> lock(_mutex);

        return _counts;
    }

    int count(const char* what) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const std::map<std::string, int>::const_iterator found = _counts.find(what);

        return (found == _counts.end()) ? 0 : found->second;
    }

private:
    mutable std::mutex _mutex;
    std::map<std::string, int> _counts;
};

/**
 * @brief Channel c of pixel (x, y) of plane.
 **/
inline float
rawTestPlaneValue(const RenderedPlane& plane,
                  int x,
                  int y,
                  int c)
{
    const std::size_t nComps = plane.channels.size();

    return plane.pixels[((std::size_t)(y - plane.window.y1) * (std::size_t)plane.window.width() + (std::size_t)(x - plane.window.x1)) * nComps + (std::size_t)c];
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVE_RAWTESTSOURCE_H
