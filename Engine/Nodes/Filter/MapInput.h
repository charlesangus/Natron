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

#ifndef Engine_Nodes_Filter_MapInput_h
#define Engine_Nodes_Filter_MapInput_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#define kMapInputLabel "Map"
#define kMapInputParamChannel "mapChannel"
#define kMapInputParamInvert "mapInvert"
#define kMapInputParamResponse "mapResponse"
#define kMapInputParamResponseLinear "linear"
#define kMapInputParamResponseSmooth "smooth"
#define kMapInputParamGamma "mapGamma"
#define kMapInputParamSizeAtBlack "sizeAtBlack"

// The largest Size and Size At Black of a variable-size filter. Radii and halos are derived from
// sizes held to it, which keeps their integer conversions and the box radius search finite.
#define kMapInputMaxSize 1000.

NATRON_NAMESPACE_ENTER

class KnobReader;

/**
 * @brief The Map input of a variable-size filter. It is an Aux input, so the host's mask and mix
 * never blend it: the node reads it itself, through MapInput.
 **/
inline constexpr InputSpec kMapInputSpec = {
    .label = kMapInputLabel,
    .role = InputRole::Aux,
    .optional = true,
};

/**
 * @brief The Map knobs at one time and view. remap() turns a raw Map sample into the Map value,
 * in [0,1]: inverted first when invert is set, clamped to [0,1], raised to the power exponent,
 * 1 / Map Gamma, and then shaped by smoothstep when smooth is set. Exponent 1 without smooth
 * leaves the clamped sample as it is. sizeAt() turns a Map value into a size, sizeAtBlack at 0
 * and the node's own size at 1.
 **/
struct MapInputParams {
    bool invert = false;
    bool smooth = false;
    float exponent = 1.f;
    double sizeAtBlack[2] = { 0., 0. };

    float remap(float raw) const WARN_UNUSED_RETURN;

    double sizeAt(float map, int dimension, double sizeAtWhite) const WARN_UNUSED_RETURN;
};

struct MapRange {
    float min = 0.f;
    float max = 0.f;
};

/**
 * @brief The Map values of a pixel window. A pixel outside the window reads outside(), the value
 * the Map has beyond its image.
 **/
class MapPlane {
public:
    MapPlane() = default;

    void reset(const RectI& window, float fill);

    const RectI& window() const
    {
        return _window;
    }

    float outside() const
    {
        return _outside;
    }

    float at(int x, int y) const WARN_UNUSED_RETURN;

    float* row(int y) WARN_UNUSED_RETURN;

    const float* row(int y) const WARN_UNUSED_RETURN;

    /**
     * @brief The smallest and largest Map value in rect. The part of rect outside the window
     * counts as outside(); an empty rect gives outside() for both.
     **/
    MapRange tileRange(const RectI& rect) const WARN_UNUSED_RETURN;

private:
    RectI _window;
    float _outside = 0.f;
    std::vector<float> _values;
};

/**
 * @brief m^exponent for m in (0,1) and exponent > 0, from additions, multiplications, one division
 * and exact bit operations in a fixed order, so that the GPU kernels, which repeat them in
 * Engine/Gpu/Kernels/mapremap.slang, compute the same float. Within about 1e-7 of the exact power;
 * a result below 2^-125 is 0.
 **/
float mapGammaPow(float m, float exponent) WARN_UNUSED_RETURN;

/**
 * @brief The Map convention shared by the variable-size filters: an optional Aux input named Map,
 * a channel of any of its layers, invert, a response curve and gamma, and a size at black that
 * pairs with the node's own size, the size at white. A node holds one MapInput, calls
 * createSizeAtBlackKnob() just before adding its size knob and createKnobs() after it from
 * initializeKnobs(), declareLayerKnob() once its node exists, reports getInputLayersRead() for
 * the Map input, bakes the knobs with bake() and reads the Map for its render window with read().
 **/
class MapInput {
public:
    /**
     * @brief Adds Size At Black to page. sizeDimensions is the dimension count of the node's size
     * knob, which it matches.
     **/
    void createSizeAtBlackKnob(KnobHolder* holder, const KnobPagePtr& page, int sizeDimensions);

    /**
     * @brief Adds the Map's channel, invert, response and gamma knobs to page.
     **/
    void createKnobs(KnobHolder* holder, const KnobPagePtr& page);

    void declareLayerKnob(const NodePtr& node, int inputNb) const;

    MapInputParams bake(const KnobReader& reader) const WARN_UNUSED_RETURN;

    /**
     * @brief The layer and the channel within it the channel knob selects among the Map input's
     * layers, false when the input is disconnected, the knob is None or the layer is absent.
     **/
    bool resolveChannel(EffectInstance* effect,
                        int inputNb,
                        double time,
                        ViewIdx view,
                        ImageLayerDesc* layer,
                        int* channel) const WARN_UNUSED_RETURN;

    bool getInputLayersRead(EffectInstance* effect,
                            int inputNb,
                            double time,
                            ViewIdx view,
                            std::list<ImageLayerDesc>* layers) const WARN_UNUSED_RETURN;

    /**
     * @brief The Map values over window. A disconnected Map reads 1 everywhere, so the node runs
     * at its full size. A connected one reads a black pixel, remapped and inverted like any
     * other, outside its image, and everywhere when the channel does not resolve. Fails on a Map
     * image that is not float.
     * Runs on the calling thread, which must not be a pool thread: it fetches and locks the image.
     **/
    StatusEnum read(EffectInstance* effect,
                    int inputNb,
                    const MapInputParams& params,
                    double time,
                    ViewIdx view,
                    const RenderScale& scale,
                    const RectI& window,
                    MapPlane* plane) const WARN_UNUSED_RETURN;

    /// A size held to [0, kMapInputMaxSize]; NaN is 0.
    static double clampSize(double size) WARN_UNUSED_RETURN;

    /**
     * @brief Fills plane over window with channel of a float image, remapped by params; a null
     * image, or a pixel outside its bounds, reads params.remap(0), as a black pixel inside it does.
     **/
    static void fill(const Image* image,
                     int channel,
                     const MapInputParams& params,
                     const RectI& window,
                     MapPlane* plane);

private:
    KnobChannelSelectWPtr _channel;
    KnobBoolWPtr _invert;
    KnobChoiceWPtr _response;
    KnobDoubleWPtr _gamma;
    KnobDoubleWPtr _sizeAtBlack;
    int _sizeDimensions = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_MapInput_h
