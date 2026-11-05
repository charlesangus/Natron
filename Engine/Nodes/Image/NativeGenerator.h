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
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectD.h"

#define kNativeGeneratorSourceInputLabel "Source"

NATRON_NAMESPACE_ENTER

/**
 * @brief The channels of an output row a generator op writes, and where each comes from in the
 * generator's RGBA colour: an alpha pixel takes A, an XY pixel R and G, an RGB pixel R, G and
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
 * @brief What native image generators share, reproducing the openfx-misc Generator helper
 * (SupportExt/ofxsGenerator) knob for knob: the extent choice (Format, Size, Project, Default),
 * recenter, reformat, the host-managed format choice with its secret size and pixel aspect ratio,
 * bottomLeft/size with normalised defaults (the project extent) and frameRange; the region of
 * definition, output pixel aspect ratio, output format and time domain they imply; and the
 * rectangle overlay in Size extent.
 *
 * A generator has no input: Default extent is the project's, and the channels of the target
 * layer its op does not write are zero.
 **/
class NativeGenerator {
public:
    typedef ExtentKnobs::ExtentEnum ExtentEnum;

    explicit NativeGenerator(EffectInstance* effect);

    /**
     * @brief Declares the generator knobs on page, in the openfx-misc order, with Default the
     * initial extent. The generator knobs come before the op's.
     **/
    void createKnobs(const KnobPagePtr& page);

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

    /**
     * @brief The generator's region of definition at (time, view): the extent's, or the
     * project's for Default extent.
     **/
    RectD getRegionOfDefinition(double time,
                                ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The output pixel aspect ratio at time and view, 1 when the effect has none yet.
     **/
    double getPixelAspectRatio(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    bool knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason,
                     double time) WARN_UNUSED_RETURN;

    void onKnobsLoaded();

    void getFrameRange(double* first,
                       double* last) const;

    StatusEnum getPreferredMetadataAt(double time,
                                      ViewIdx view,
                                      NodeMetadata& metadata) const WARN_UNUSED_RETURN;

    /**
     * @brief Only ofx/framerate and ofx/pixelaspect. ofx/frame is omitted because it numbers an
     * image within a source and a generator has none.
     **/
    ImageMetadata deriveOutputMetadata(double time,
                                       ViewIdx view) const WARN_UNUSED_RETURN;

private:
    EffectInstance* _effect;
    ExtentKnobs _extentKnobs;
};

/**
 * @brief An image generator made of an Op with no input: an ImageNode over OP and SPEC that
 * holds a NativeGenerator and answers from it the region of definition, preferred metadata,
 * output metadata and frame range. Its knobs are the generator's, then the op's, on one
 * "Controls" page. An op with a `const NativeGenerator* generator` member gets it pointed at the
 * node's, so that bake() can read the region of definition and pixel aspect ratio.
 **/
template <typename OP, const NodeSpec& SPEC = OP::kNodeSpec>
class GeneratorNode
    : public ImageNode<OP, SPEC, OP::kPipeline> {
    static_assert(SPEC.inputs.empty(), "a generator has no input");
    static_assert(OP::kPipeline.stages.empty(), "a generator has no source for its stages to blend against");

    typedef ImageNode<OP, SPEC, OP::kPipeline> Base;

public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeneratorNode(node);
    }

    explicit GeneratorNode(NodePtr node)
        : Base(node)
        , _generator(this)
    {
        if constexpr (requires(OP& o, const NativeGenerator* g) { o.generator = g; }) {
            this->op().generator = &_generator;
        }
    }

    virtual ~GeneratorNode()
    {
    }

    virtual bool isGenerator() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double time,
                                             const RenderScale& /*scale*/,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *rod = _generator.getRegionOfDefinition(time, view);

        return eStatusOK;
    }

    const NativeGenerator& generator() const
    {
        return _generator;
    }

protected:
    virtual void initializeKnobs() OVERRIDE
    {
        const KnobPagePtr page = this->createControlsPage();

        _generator.createKnobs(page);
        this->op().createKnobs(static_cast<KnobHolder*>(this), page);
        this->addStageKnobs(page);
    }

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec /*view*/,
                             double time,
                             bool /*originatedFromMainThread*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _generator.knobChanged(k, reason, time);
    }

    virtual void onKnobsLoaded() OVERRIDE FINAL
    {
        _generator.onKnobsLoaded();
    }

    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL
    {
        _generator.getFrameRange(first, last);
    }

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _generator.getPreferredMetadataAt(time, view, metadata);
    }

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _generator.deriveOutputMetadata(time, view);
    }

private:
    NativeGenerator _generator;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_NativeGenerator_h
