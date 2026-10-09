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

#ifndef Engine_Nodes_Image_NativeGenerator_h
#define Engine_Nodes_Image_NativeGenerator_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/RectD.h"

#define kNativeGeneratorSourceInputLabel "Source"

NATRON_NAMESPACE_ENTER

/**
 * @brief The channels of an output row a generator kernel writes, and where each comes from in
 * the generator's RGBA colour: an alpha pixel takes A, an XY pixel R and G, an RGB pixel R, G and
 * B, the way the openfx-misc generators fill a 1-, 2-, 3- or 4-channel image. A one-channel plane
 * other than colour alpha is rendered into an RGBA image by the host and read back from R, so it
 * too takes the generator's first channel.
 **/
struct GeneratorRowChannels {
    int count;
    int dstIndex[4];
    int rgbaIndex[4];

    explicit GeneratorRowChannels(const RowIO& io)
        : count(0)
    {
        for (int c = 0; (c < io.nComps) && (c < 4); ++c) {
            const int bit = pixelKernelChannelBit(io.nComps, c);
            if (io.channels[bit]) {
                dstIndex[count] = c;
                rgbaIndex[count] = bit;
                ++count;
            }
        }
    }

    /**
     * @brief Writes the selected channels of rgba into the pixel at dst.
     **/
    void write(const float rgba[4],
               float* dst) const
    {
        for (int k = 0; k < count; ++k) {
            dst[dstIndex[k]] = rgba[rgbaIndex[k]];
        }
    }
};

/**
 * @brief Base for native image generators, reproducing the openfx-misc Generator helper
 * (SupportExt/ofxsGenerator) knob for knob: the extent choice (Format, Size, Project, Default),
 * recenter, reformat, the host-managed format choice with its secret size and pixel aspect ratio,
 * bottomLeft/size with normalised defaults (the project extent), interactive, hidpi and
 * frameRange; the region of definition, output pixel aspect ratio, output format and time domain
 * they imply; and the rectangle overlay in Size extent.
 *
 * The output layer is chosen by a target layer select with channel buttons; the selected channels
 * of that layer are written by the subclass's kernel, and every other channel passes through from
 * the optional Source input, or is zero without one. The output stream is always RGBA, as the
 * openfx-misc generators' outputComponents is pinned to RGBA under Natron.
 *
 * A subclass declares its Source input through describeSourceInput(), calls
 * initializeGeneratorKnobs() first in initializeKnobs(), and builds a kernel in makeKernel(). The
 * kernel receives the row's pixel position (x0, y) at the render scale, no source row (nSrc is 0)
 * and dst already holding the pass-through values; it writes only the channels in io.channels,
 * through GeneratorRowChannels.
 **/
class NativeGenerator
    : public NativeImageEffect {
public:
    typedef ExtentKnobs::ExtentEnum ExtentEnum;

    explicit NativeGenerator(NodePtr node);

    virtual ~NativeGenerator();

    virtual bool isGenerator() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief Only ofx/frame, ofx/framerate and ofx/pixelaspect: a generator has no source file, so
     * nothing a Source input carries is passed on.
     **/
    virtual ImageMetadata getOutputMetadata(double time, ViewIdx view) OVERRIDE WARN_UNUSED_RETURN;

    ExtentEnum getExtent() const WARN_UNUSED_RETURN;

    /**
     * @brief The region of definition the extent knobs give at (time, view), as the openfx-misc
     * GeneratorPlugin::getRegionOfDefinition() computes it. False for Default extent, which has
     * none of its own.
     **/
    bool getExtentRegionOfDefinition(double time,
                                     ViewIdx view,
                                     RectD* rod) const WARN_UNUSED_RETURN;

    /**
     * @brief The project's default format in canonical coordinates: its offset and size.
     **/
    RectD getProjectExtentRect() const WARN_UNUSED_RETURN;

protected:
    /**
     * @brief Adds the optional Source input every generator has, so the layers and channels it
     * does not write pass through from it.
     **/
    static void describeSourceInput(NativePluginDescription* desc);

    /**
     * @brief Declares the generator knobs on page, in the openfx-misc order, with Default the
     * initial extent.
     **/
    void initializeGeneratorKnobs(const KnobPagePtr& page);

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE;

    virtual void onKnobsLoaded() OVERRIDE;

    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    ExtentKnobs _extentKnobs;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_NativeGenerator_h
