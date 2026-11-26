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

#ifndef IMAGECOMPONENTS_H
#define IMAGECOMPONENTS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <string>
#include <vector>

#if (!defined(Q_MOC_RUN) && !defined(SBK_RUN)) || defined(SBK2_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/version.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on
#endif

#define IMAGELAYERDESC_SERIALIZATION_RENAMES_TAGS 3
#define IMAGELAYERDESC_SERIALIZATION_LAYER_NAMES 4
#define IMAGELAYERDESC_SERIALIZATION_VERSION IMAGELAYERDESC_SERIALIZATION_LAYER_NAMES

#include "Engine/ChoiceOption.h"
#include "Engine/EngineFwd.h"

// The colour storage plane shares its ID with the rgba view; isColorStorage() tells them apart.
#define kNatronColorLayerID "rgba"

// A colour view is (the colour storage plane, a channel mask): rgba/rgb/alpha are always
// present on any colour storage, xy only on the 2-channel XY storage layout.
#define kNatronColorViewRGBA "rgba"
#define kNatronColorViewRGB "rgb"
#define kNatronColorViewAlpha "alpha"
#define kNatronColorViewXY "xy"

#define kNatronBackwardMotionVectorsLayerID "backward"
#define kNatronForwardMotionVectorsLayerID "forward"
#define kNatronDisparityLeftLayerID "disparityL"
#define kNatronDisparityRightLayerID "disparityR"

NATRON_NAMESPACE_ENTER

class ImageLayerDesc {
public:
    ImageLayerDesc();

    /**
     * @brief A desc whose ID is kNatronColorLayerID is the colour storage plane; the colour views
     * come from getColorView() only.
     **/
    ImageLayerDesc(const std::string& layerID,
                   const std::vector<std::string>& channels);

    ImageLayerDesc(const std::string& layerID,
                   const char** channels,
                   int count);

    ImageLayerDesc(const ImageLayerDesc& other);

    ImageLayerDesc& operator=(const ImageLayerDesc& other);

    ~ImageLayerDesc();

    // Is it Alpha, RGB or RGBA, or a colour view of one of these (see isColorViewID())
    bool isColorLayer() const;

    static bool isColorLayer(const std::string& layerID);

    /**
     * @brief The colour storage plane an image carries, as opposed to a colour view of it, though
     * the rgba view shares its ID.
     **/
    bool isColorStorage() const;

    /**
     * @brief Returns the number of channels in this layer.
     **/
    int getNumComponents() const;

    /**
     * @brief The layer's name: what menus show, files store and projects save.
     **/
    const std::string& getLayerID() const;

    /**
     * @brief The layer name for text built from a desc that may be the colour storage plane:
     * the storage plane is named after the colour view whose channels it carries (see
     * colorViewForNComps()). Any other layer, colour views included, returns getLayerID().
     **/
    std::string getUserFacingLabel() const;

    /**
     * @brief Returns the channels composing this layer.
     **/
    const std::vector<std::string>& getChannels() const;

    bool operator==(const ImageLayerDesc& other) const;

    bool operator!=(const ImageLayerDesc& other) const
    {
        return !(*this == other);
    }

    // For std::map
    bool operator<(const ImageLayerDesc& other) const;

    operator bool() const
    {
        return getNumComponents() > 0;
    }

    bool operator!() const
    {
        return getNumComponents() == 0;
    }

    ChoiceOption getLayerOption() const;
    ChoiceOption getChannelOption(int channelIndex) const;

    /**
     * @brief Maps the given nComps to the color layer
     **/
    static const ImageLayerDesc& mapNCompsToColorLayer(int nComps);

    /**
     * @brief Find a layer equivalent to this layer in the other layers container.
     * ITERATOR must be either a std::vector<ImageLayerDesc>::iterator or std::list<ImageLayerDesc>::iterator
     **/
    template <typename ITERATOR>
    static ITERATOR findEquivalentLayer(const ImageLayerDesc& layer, ITERATOR begin, ITERATOR end)
    {
        bool isColor = layer.isColorLayer();

        ITERATOR foundExistingColorMatch = end;
        ITERATOR foundExistingComponents = end;

        for (ITERATOR it = begin; it != end; ++it) {
            if (it->isColorLayer() && isColor) {
                foundExistingColorMatch = it;
            } else {
                if (*it == layer) {
                    foundExistingComponents = it;
                    break;
                }
            }
        } // for each output components

        if (foundExistingComponents != end) {
            return foundExistingComponents;
        } else if (foundExistingColorMatch != end) {
            return foundExistingColorMatch;
        } else {
            return end;
        }
    } // findEquivalentLayer

    /*
     * These are default presets image components
     */
    static const ImageLayerDesc& getNoneComponents();
    static const ImageLayerDesc& getRGBAComponents();
    static const ImageLayerDesc& getRGBComponents();
    static const ImageLayerDesc& getAlphaComponents();
    static const ImageLayerDesc& getBackwardMotionComponents();
    static const ImageLayerDesc& getForwardMotionComponents();
    static const ImageLayerDesc& getDisparityLeftComponents();
    static const ImageLayerDesc& getDisparityRightComponents();
    static const ImageLayerDesc& getXYComponents();

    static bool isColorViewID(const std::string& layerID);

    /**
     * @brief The view's own ID and channel names; getNoneComponents() for any other ID.
     **/
    static const ImageLayerDesc& getColorView(const std::string& viewID);

    /**
     * @brief Colour channels are std::bitset<4> bits R=0, G=1, B=2, A=3 whatever the layout,
     * so a single-channel view or storage occupies bit 3 (see ResolvedLayer::channelBit in
     * KnobChannelSet.h).
     **/
    static std::bitset<4> colorViewMask(const std::string& viewID);

    /**
     * @brief -1 past the view's channels.
     **/
    static int colorViewChannelBit(const std::string& viewID, int index);

    /**
     * @brief -1 when viewID has no channel on bit.
     **/
    static int colorViewChannelIndex(const std::string& viewID, int bit);

    static std::bitset<4> colorStorageBits(const ImageLayerDesc& storage);

    /**
     * @brief Tries Alpha, XY, RGB, then RGBA; the None layout when bits is empty.
     **/
    static ImageLayerDesc narrowestColorStorageCovering(const std::bitset<4>& bits);

    /**
     * @brief rgba, rgb and alpha are presented by every colour storage, whatever its layout, since
     * a channel the storage lacks reads as zero; xy only by the 2-channel XY layout. Does nothing
     * if storage is not the colour layer.
     **/
    static void presentColorViews(const ImageLayerDesc& storage, std::vector<std::string>* views);

    /**
     * @brief rowChannels has ChannelSetRow::channels' meaning: empty selects every channel of
     * viewID. *zeroBits is the subset of *bits that storage does not carry and so reads as zero.
     **/
    static void resolveColorView(const std::string& viewID,
                                 const ImageLayerDesc& storage,
                                 const std::vector<std::string>& rowChannels,
                                 std::bitset<4>* bits,
                                 std::bitset<4>* zeroBits);

    /**
     * @brief 1 -> alpha, 2 -> xy, 3 -> rgb, 4 -> rgba.
     **/
    static const ImageLayerDesc& colorViewForNComps(int nComps);

    /**
     * @brief Replaces the colour storage entry with every view it presents, in the order rgba,
     * rgb, alpha, (xy).
     **/
    static void expandColorViews(std::list<ImageLayerDesc>* storageList);

    /**
     * @brief Replaces the colour storage entry with the one view matching its layout, so each
     * colour channel is listed once; an unknown layout becomes rgba.
     **/
    static void collapseColorToLayoutView(std::list<ImageLayerDesc>* storageList);

    template <class Archive>
    void save(Archive& ar, const unsigned int version) const;

    template <class Archive>
    void load(Archive& ar, const unsigned int version);

private:
    struct ColorViewTag { };

    ImageLayerDesc(ColorViewTag, const std::string& viewID, const std::vector<std::string>& channels);

    std::string _layerID;
    std::vector<std::string> _channels;
    bool _colorStorage;

    friend class boost::serialization::access;

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};

NATRON_NAMESPACE_EXIT

BOOST_CLASS_VERSION(NATRON_NAMESPACE::ImageLayerDesc, IMAGELAYERDESC_SERIALIZATION_VERSION)

#endif // IMAGECOMPONENTS_H
