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

#include <string>
#include <vector>

#if (!defined(Q_MOC_RUN) && !defined(SBK_RUN)) || defined(SBK2_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/version.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on
#endif

#define IMAGELAYERDESC_SERIALIZATION_RENAMES_TAGS 3
#define IMAGELAYERDESC_SERIALIZATION_VERSION IMAGELAYERDESC_SERIALIZATION_RENAMES_TAGS

#include "Engine/ChoiceOption.h"
#include "Engine/EngineFwd.h"
#include <nuke/fnOfxExtensions.h>

#define kNatronColorLayerID kFnOfxImagePlaneColour
#define kNatronColorLayerLabel "Color"

#define kNatronBackwardMotionVectorsLayerID kFnOfxImagePlaneBackwardMotionVector
#define kNatronBackwardMotionVectorsLayerLabel "Backward"

#define kNatronForwardMotionVectorsLayerID kFnOfxImagePlaneForwardMotionVector
#define kNatronForwardMotionVectorsLayerLabel "Forward"

#define kNatronDisparityLeftLayerID kFnOfxImagePlaneStereoDisparityLeft
#define kNatronDisparityLeftLayerLabel "DisparityLeft"

#define kNatronDisparityRightLayerID kFnOfxImagePlaneStereoDisparityRight
#define kNatronDisparityRightLayerLabel "DisparityRight"

#define kNatronDisparityComponentsLabel "Disparity"
#define kNatronMotionComponentsLabel "Motion"

NATRON_NAMESPACE_ENTER

class ImageLayerDesc {
public:
    ImageLayerDesc();

    ImageLayerDesc(const std::string& layerID,
                   const std::string& layerLabel,
                   const std::string& channelsLabel,
                   const std::vector<std::string>& channels);

    ImageLayerDesc(const std::string& layerID,
                   const std::string& layerLabel,
                   const std::string& channelsLabel,
                   const char** chanels,
                   int count);

    ImageLayerDesc(const ImageLayerDesc& other);

    ImageLayerDesc& operator=(const ImageLayerDesc& other);

    ~ImageLayerDesc();

    // Is it Alpha, RGB or RGBA
    bool isColorLayer() const;

    static bool isColorLayer(const std::string& layerID);

    /**
     * @brief Returns the number of channels in this layer.
     **/
    int getNumComponents() const;

    /**
     * @brief Returns the layer unique identifier. This should be used to compare ImageLayerDesc together.
     * This is not supposed to be used for display purpose, use getLayerLabel() instead.
     **/
    const std::string& getLayerID() const;

    /**
     * @brief Returns the layer label.
     * This is what is used to display to the user.
     **/
    const std::string& getLayerLabel() const;

    /**
     * @brief Returns the channels composing this layer.
     **/
    const std::vector<std::string>& getChannels() const;

    /**
     * @brief Returns a label used to better represent the type of components used by this layer.
     * e.g: "Motion" can be used to better label "XY" component types.
     **/
    const std::string& getChannelsLabel() const;

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
     * @brief Maps the given OpenFX plane to a ImageLayerDesc.
     * @param ofxPlane Can be

     *  kFnOfxImagePlaneBackwardMotionVector
     *  kFnOfxImagePlaneForwardMotionVector
     *  kFnOfxImagePlaneStereoDisparityLeft
     *  kFnOfxImagePlaneStereoDisparityRight
     *  Or any plane encoded in the format specified by the Natron multi-plane extension.
     * @note kFnOfxImagePlaneColour carries no channel count, so the color layer must go through
     * mapNCompsToColorLayer instead.
     * @return getNoneComponents() when ofxPlane cannot be decoded.
     **/
    static ImageLayerDesc mapOFXPlaneStringToLayer(const std::string& ofxPlane);

    /**
     * @brief Maps OpenFX components string to a layer, optionnally also to a paired layer in the case of disparity/motion vectors.
     * @param ofxComponents Must be a string between
     * kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha, kNatronOfxImageComponentXY, kOfxImageComponentNone
     * or kFnOfxImageComponentStereoDisparity or kFnOfxImageComponentMotionVectors
     * Or any plane encoded in the format specified by the Natron multi-plane extension.
     **/
    static void mapOFXComponentsTypeStringToLayers(const std::string& ofxComponents, ImageLayerDesc* layer, ImageLayerDesc* pairedLayer);

    /**
     * @brief Does the inverse of mapOFXPlaneStringToLayer, except that it can also be used for
     * the color layer.
     **/
    static std::string mapLayerToOFXPlaneString(const ImageLayerDesc& layer);

    /**
     * @brief Returns an OpenFX encoded string representing the components type of the layer.
     * @returns One of the following strings:
     * kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha, kNatronOfxImageComponentXY, kOfxImageComponentNone
     * or kFnOfxImageComponentStereoDisparity or kFnOfxImageComponentMotionVectors
     * Or any plane encoded in the format specified by the Natron multi-plane extension.
     **/
    static std::string mapLayerToOFXComponentsTypeString(const ImageLayerDesc& layer);

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

    template <class Archive>
    void save(Archive& ar, const unsigned int version) const;

    template <class Archive>
    void load(Archive& ar, const unsigned int version);

private:
    std::string _layerID, _layerLabel;
    std::vector<std::string> _channels;
    std::string _channelsLabel;

    friend class boost::serialization::access;

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};

NATRON_NAMESPACE_EXIT

BOOST_CLASS_VERSION(NATRON_NAMESPACE::ImageLayerDesc, IMAGELAYERDESC_SERIALIZATION_VERSION)

#endif // IMAGECOMPONENTS_H
