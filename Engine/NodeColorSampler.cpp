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

#include "NodeColorSampler.h"

#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QThread>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/TLSHolder.h"
#include "Engine/ThreadPool.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {

template <typename PIX, int maxValue>
void
accumulate(const Image& image,
           const RectI& rect,
           const std::vector<int>& sourceIndex,
           const int slot[4],
           double sums[4])
{
    Image::ReadAccess access(&image);

    for (int y = rect.y1; y < rect.y2; ++y) {
        for (int x = rect.x1; x < rect.x2; ++x) {
            const PIX* pix = reinterpret_cast<const PIX*>(access.pixelAt(x, y));
            if (!pix) {
                continue;
            }
            for (std::size_t c = 0; c < sourceIndex.size(); ++c) {
                if (sourceIndex[c] >= 0) {
                    sums[slot[c]] += pix[sourceIndex[c]] * (1. / maxValue);
                }
            }
        }
    }
}

bool
isMainThread()
{
    QCoreApplication* app = QCoreApplication::instance();

    return app && (QThread::currentThread() == app->thread());
}

bool
renderLayer(const NodePtr& node,
            const AbortableRenderInfoPtr& abortInfo,
            double time,
            ViewIdx view,
            unsigned int mipmapLevel,
            const RectI& pixelRect,
            const ImageLayerDesc& layer,
            RectI* roi,
            ImagePtr* image)
{
    EffectInstancePtr effect = node->getEffectInstance();
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             view,
                                             true /*isRenderUserInteraction*/,
                                             false /*isSequential*/,
                                             abortInfo,
                                             node,
                                             0 /*textureIndex*/,
                                             node->getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             RenderStatsPtr());

    const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
    RectD rod;
    bool isProjectFormat = false;
    // The hash and layout the args snapshotted, so an edit made while sampling cannot pair one
    // state's region with another's layout.
    if (effect->getRegionOfDefinition_public(effect->getRenderHash(), time, scale, view, &rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    const NodeMetadataConstPtr renderMetadata = effect->getRenderMetadata(time, view);
    const double par = renderMetadata->getPixelAspectRatio(-1);
    *roi = pixelRect.intersect(rod.toPixelEnclosing(mipmapLevel, par));
    if (roi->isNull()) {
        return false;
    }

    FrameRequestMap request;
    if (EffectInstance::computeRequestPass(time, view, mipmapLevel, roi->toCanonical(mipmapLevel, par, rod), node, request) == eStatusFailed) {
        return false;
    }
    frameRenderArgs.updateNodesRequest(request);

    // The viewer renders at the effect's own depth, so asking for the same depth finds its image.
    ImageBitDepthEnum depth = renderMetadata->getBitDepth(-1);
    if ((depth != eImageBitDepthByte) && (depth != eImageBitDepthShort)) {
        depth = eImageBitDepthFloat;
    }

    std::list<ImageLayerDesc> components;
    components.push_back(layer);
    EffectInstance::RenderRoIArgs args(time,
                                       scale,
                                       mipmapLevel,
                                       view,
                                       false /*byPassCache*/,
                                       *roi,
                                       rod,
                                       components,
                                       depth,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       time);
    std::map<ImageLayerDesc, ImagePtr> layers;
    if ((effect->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) || layers.empty()) {
        return false;
    }
    *image = layers.begin()->second;

    return bool(*image);
} // renderLayer

} // namespace

ImagePtr
renderNodeImage(const NodePtr& node,
                double time,
                ViewIdx view,
                unsigned int mipmapLevel,
                const RectI& pixelRect,
                const ImageLayerDesc& layer,
                RectI* renderedRect)
{
    if (!node || !node->getEffectInstance() || !node->isActivated() || node->isNodeDisabled(time) || pixelRect.isNull()) {
        return ImagePtr();
    }
    if (node->hasMandatoryInputDisconnected()) {
        return ImagePtr();
    }
    switch (node->getEffectiveOutputDataKind()) {
    case eDataKindDeep:
    // Raw values are camera-native, per-photosite on a mosaic: no RGBA reading of them means anything.
    case eDataKindRaw:
        return ImagePtr();
    case eDataKindImage:
    case eDataKindScene:
    case eDataKindPolymorphic:
        break;
    }

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    AbortableThread* isAbortable = dynamic_cast<AbortableThread*>(QThread::currentThread());
    if (isAbortable) {
        isAbortable->setAbortInfo(true /*isRenderUserInteraction*/, abortInfo, node->getEffectInstance());
    }

    RectI roi;
    ImagePtr image;
    const bool rendered = renderLayer(node, abortInfo, time, view, mipmapLevel, pixelRect, layer, &roi, &image);

    // The main thread's TLS also holds the state of whatever action is running on it, so only a
    // worker thread, which owns nothing else, is cleaned up as the tracker does.
    if (!isMainThread()) {
        appPTR->getAppTLS()->cleanupTLSForThread();
    }

    if (!rendered) {
        return ImagePtr();
    }
    if (renderedRect) {
        *renderedRect = roi;
    }

    return image;
} // renderNodeImage

bool
sampleNodeColor(const NodePtr& node,
                double time,
                ViewIdx view,
                unsigned int mipmapLevel,
                const RectI& pixelRect,
                const ImageLayerDesc& layer,
                float rgba[4])
{
    RectI roi;
    const ImagePtr image = renderNodeImage(node, time, view, mipmapLevel, pixelRect, layer, &roi);
    if (!image) {
        return false;
    }
    const RectI rect = roi.intersect(image->getBounds());
    if (rect.isNull()) {
        return false;
    }

    const std::vector<std::string>& wanted = layer.getChannels();
    const std::vector<std::string>& present = image->getComponents().getChannels();
    const std::size_t nComps = std::min<std::size_t>(present.size(), image->getComponentsCount());
    std::vector<int> sourceIndex(std::min<std::size_t>(wanted.size(), 4), -1);
    int slot[4] = { 0, 1, 2, 3 };
    bool hasAlpha = false;
    for (std::size_t c = 0; c < sourceIndex.size(); ++c) {
        if (wanted[c] == "A") {
            slot[c] = 3;
        }
        for (std::size_t i = 0; i < nComps; ++i) {
            if (present[i] == wanted[c]) {
                sourceIndex[c] = (int)i;
                break;
            }
        }
        if ((slot[c] == 3) && (sourceIndex[c] >= 0)) {
            hasAlpha = true;
        }
    }

    double sums[4] = { 0., 0., 0., 0. };
    switch (image->getBitDepth()) {
    case eImageBitDepthByte:
        accumulate<unsigned char, 255>(*image, rect, sourceIndex, slot, sums);
        break;
    case eImageBitDepthShort:
        accumulate<unsigned short, 65535>(*image, rect, sourceIndex, slot, sums);
        break;
    case eImageBitDepthFloat:
        accumulate<float, 1>(*image, rect, sourceIndex, slot, sums);
        break;
    default:
        return false;
    }

    const double area = (double)rect.width() * rect.height();
    for (int i = 0; i < 4; ++i) {
        rgba[i] = (float)(sums[i] / area);
    }
    if (!hasAlpha) {
        rgba[3] = 1.f;
    }

    return true;
} // sampleNodeColor

NATRON_NAMESPACE_EXIT
