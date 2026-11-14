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

#ifndef Tests_Scene_UdimTilesTestEffect_h
#define Tests_Scene_UdimTilesTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <QString>
#include <QStringList>

#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/RenderContext.h"
#include "Engine/ViewIdx.h"

#define kUdimTilesTestPluginID "test.natron.built-in.LiveTextureUdimTiles"

NATRON_NAMESPACE_ENTER

/**
 * @brief A UDIM painter that renders: each tile is filled with its number less 1000 in red, over a
 * format of its own, and the tile's version moves only with the strokes painted on it.
 **/
class UdimTilesTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new UdimTilesTestEffect(n);
    }

    explicit UdimTilesTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    static RectI tileFormat(int tile)
    {
        switch (tile) {
        case 1001:
            return RectI(0, 0, 16, 16);
        case 1002:
            return RectI(0, 0, 8, 4);
        default:
            return RectI(0, 0, 4, 4);
        }
    }

    void paint(int tile)
    {
        {
            std::lock_guard<std::mutex> locker(_mutex);
            ++_strokes[tile];
        }
        getNode()->markHashDirty();
    }

    // Declares tiles instead of the default set at the frames the map names, and makes the
    // node vary over time, as a source whose tile coverage changes does. An empty list
    // declares nothing at its frame.
    void setTilesByFrame(const std::map<int, std::string>& tiles)
    {
        {
            std::lock_guard<std::mutex> locker(_mutex);
            _tilesByFrame = tiles;
        }
        getNode()->markHashDirty();
    }

    void setFailing(int tile,
                    bool failing)
    {
        std::lock_guard<std::mutex> locker(_mutex);
        if (failing) {
            _failing.insert(tile);
        } else {
            _failing.erase(tile);
        }
    }

    // A failing tile throws while its region is read, so its format is never known and no
    // render is asked of it.
    void setFormatFailing(int tile)
    {
        {
            std::lock_guard<std::mutex> locker(_mutex);
            _formatFailing.insert(tile);
        }
        getNode()->markHashDirty();
    }

    int renders(int tile) const
    {
        std::lock_guard<std::mutex> locker(_mutex);
        std::map<int, int>::const_iterator found = _renders.find(tile);

        return found == _renders.end() ? 0 : found->second;
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL
    {
        RenderContextReadSet reads;
        reads.add(RenderContextName::udimTile());

        return reads;
    }

    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL
    {
        std::lock_guard<std::mutex> locker(_mutex);
        h.appendU64(_strokes.size());
        for (std::map<int, int>::const_iterator it = _strokes.begin(); it != _strokes.end(); ++it) {
            h.appendI64(it->first);
            h.appendI64(it->second);
        }
        h.appendU64(_formatFailing.size());
        for (std::set<int>::const_iterator it = _formatFailing.begin(); it != _formatFailing.end(); ++it) {
            h.appendI64(*it);
        }
        h.appendU64(_tilesByFrame.size());
        for (std::map<int, std::string>::const_iterator it = _tilesByFrame.begin(); it != _tilesByFrame.end(); ++it) {
            h.appendI64(it->first);
            h.appendString(it->second);
        }
    }

    virtual U64 getUdimTileHash(int tile,
                                double /*time*/,
                                ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ContentHasher h;
        h.appendString(getNode()->getPluginID());
        h.appendI64(tile);
        std::lock_guard<std::mutex> locker(_mutex);
        std::map<int, int>::const_iterator found = _strokes.find(tile);
        h.appendI64(found == _strokes.end() ? 0 : found->second);

        return h.finalize();
    }

    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL
    {
        metadata.setOutputFormat(tileFormat(currentTile()));
        std::lock_guard<std::mutex> locker(_mutex);
        metadata.setIsFrameVarying(!_tilesByFrame.empty());

        return eStatusOK;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const int tile = currentTile();
        {
            std::lock_guard<std::mutex> locker(_mutex);
            if (_formatFailing.count(tile)) {
                throw std::runtime_error("the region of UDIM tile " + std::to_string(tile) + " cannot be read");
            }
        }
        const RectI format = tileFormat(tile);
        rod->x1 = format.x1;
        rod->y1 = format.y1;
        rod->x2 = format.x2;
        rod->y2 = format.y2;

        return eStatusOK;
    }

    virtual void addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE FINAL
    {
        comps->push_back(ImageLayerDesc::getRGBAComponents());
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

protected:
    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        std::string tiles("1001-1002,1011");
        {
            std::lock_guard<std::mutex> locker(_mutex);
            std::map<int, std::string>::const_iterator found = _tilesByFrame.find((int)std::floor(time));
            if (found != _tilesByFrame.end()) {
                tiles = found->second;
            }
        }
        ImageMetadata m;
        if (!tiles.empty()) {
            m.set(kNatronMetadataKeyUdimTiles, ImageMetadata::Value(tiles));
        }

        return m;
    }

private:
    int currentTile() const
    {
        const RenderContextPtr context = getCurrentRenderContext();
        const RenderContextValue* value = context ? context->find(RenderContextName::udimTile()) : nullptr;
        const std::int64_t* tile = value ? std::get_if<std::int64_t>(value) : nullptr;

        return tile ? (int)*tile : 0;
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const int tile = currentTile();
        {
            std::lock_guard<std::mutex> locker(_mutex);
            ++_renders[tile];
            if (_failing.count(tile)) {
                return eStatusFailed;
            }
        }
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image || (image->getBitDepth() != eImageBitDepthFloat) || (image->getComponentsCount() != 4)) {
                return eStatusFailed;
            }
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    pixel[0] = (float)(tile - 1000);
                    pixel[1] = 0.f;
                    pixel[2] = 0.f;
                    pixel[3] = 1.f;
                }
            }
        }

        return eStatusOK;
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = kUdimTilesTestPluginID,
        .label = "Test UDIM Tiles",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    mutable std::mutex _mutex;
    std::map<int, int> _strokes;
    std::map<int, int> _renders;
    std::set<int> _failing;
    std::set<int> _formatFailing;
    std::map<int, std::string> _tilesByFrame;
};

inline void
registerUdimTilesTestEffectOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(UdimTilesTestEffect::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&UdimTilesTestEffect::BuildEffect));
        LibraryBinary* binary = new LibraryBinary(functions);

        std::list<std::string> grouping;
        node->getPluginGrouping(&grouping);
        QStringList qgrouping;
        for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
            qgrouping.push_back(QString::fromUtf8(it->c_str()));
        }
        Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                           QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
        p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    });
}

NATRON_NAMESPACE_EXIT

#endif // Tests_Scene_UdimTilesTestEffect_h
