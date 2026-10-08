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

#ifndef NATRON_TESTS_NATIVEPARITY_H
#define NATRON_TESTS_NATIVEPARITY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <initializer_list>
#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"

#include "RenderBothWays.h"

#define kTestPluginIDParitySource "test.natron.built-in.ParitySource"
#define kParitySourceParamComponents "components"
#define kParitySourceParamExtraLayer "extraLayer"
#define kParitySourceParamOrigin "origin"
#define kParitySourceWidth 64
#define kParitySourceHeight 48
#define kParitySourceExtraLayerID "spec"

NATRON_NAMESPACE_ENTER

/**
 * @brief The value the parity source writes at canonical pixel (cx, cy), counted from its origin,
 * for `channel` of the colour plane (0..3 = R, G, B, A, whatever the plane's width) or, with
 * extraLayer, of the spec plane (0..2).
 *
 * R, G and B are ramps over [-0.25, 1.75] plus a sine term, so a node sees negatives and
 * super-whites. A alternates bands of four rows holding exact 0, exact 1 and a [0, 1] ramp, so
 * premultiplication edge cases are always present. Everything except the exact alpha bands is
 * offset by 0.01 * time.
 **/
float paritySourceColorValue(int channel, int cx, int cy, double time);
float paritySourceExtraValue(int channel, int cx, int cy, double time);

/**
 * @brief A deterministic multiplanar generator for parity tests: a 64x48 region of definition at
 * the `origin` knob, a colour plane chosen by `components` ("rgba", "rgb" or "alpha"), and, with
 * `extraLayer`, an RGB plane named "spec". It renders at any render scale by point-sampling the
 * full-resolution picture at each pixel's centre, so mipmap levels show the same image.
 **/
class ParitySourceTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new ParitySourceTestEffect(n);
    }

    explicit ParitySourceTestEffect(NodePtr n);

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    int colorNComps() const;
    bool hasExtraLayer() const;
    void getOrigin(int* x, int* y) const;

    KnobChoiceWPtr _components;
    KnobBoolWPtr _extraLayer;
    KnobIntWPtr _origin;
};

ImageLayerDesc paritySourceExtraLayer();

/**
 * @brief A per-value bound: |native - reference| <= absolute + relative * max(1, |reference|).
 * A pair of NaNs matches, and so do two equal infinities. The named classes are the ones every
 * per-node parity test picks from.
 **/
struct ParityTolerance {
    double absolute = 0.;
    double relative = 0.;

    static ParityTolerance exact() { return ParityTolerance(); }
    static ParityTolerance arithmetic() { return make(0., 1e-6); }
    static ParityTolerance transcendental() { return make(0., 1e-5); }
    static ParityTolerance resampling() { return make(1e-5, 0.); }
    static ParityTolerance iir() { return make(1e-4, 0.); }

    static ParityTolerance make(double absolute_,
                                double relative_)
    {
        ParityTolerance t;

        t.absolute = absolute_;
        t.relative = relative_;

        return t;
    }

    bool accepts(float reference, float native) const;
};

/**
 * @brief One plugin ID instantiated twice on the same parity source: `ofx` pinned to the exact
 * major `ofxMajor`, and `native` pinned to `nativeMajor`. `ofx` is null when that exact major is
 * not registered (the OFX plugin was retired), in which case compareParity() replays recorded
 * references instead. `mask` is the shared mask source when one was asked for.
 **/
struct ParityPair {
    std::string id;
    int ofxMajor = -1;
    int nativeMajor = -1;
    AppInstancePtr app;
    NodePtr source;
    NodePtr mask;
    NodePtr ofx;
    NodePtr native;

    bool live() const
    {
        return bool(ofx);
    }
};

/**
 * @brief Whether a plugin is registered under `id` with exactly `major`. The engine's own lookup
 * falls back to the closest major above, which would silently hand back the native node.
 **/
bool isPluginMajorRegistered(const std::string& id, int major);

/**
 * @brief Creates a parity source and connects it to input 0 of both nodes. With a non-empty
 * `maskInputLabel`, also creates a second parity source and connects it to the input bearing that
 * label on both nodes. Each node is created at its exact major; a failure to do so, or a native
 * node that cannot be created, is reported with ADD_FAILURE() and leaves `native` null.
 **/
ParityPair makeParityPair(const AppInstancePtr& app,
                          const std::string& id,
                          int ofxMajor,
                          int nativeMajor,
                          const std::string& maskInputLabel = std::string());

/**
 * @brief Creates another parity source and connects it to the input labelled `inputLabel` on
 * both nodes of the pair. Returns it, or null after an ADD_FAILURE() when either node lacks the
 * input or a connection is refused.
 **/
NodePtr connectParityInput(ParityPair& pair, const std::string& inputLabel);

NodePtr createNodeAtMajor(const AppInstancePtr& app, const std::string& id, int major);

/**
 * @brief Sets knob `name` on both nodes of the pair (the OFX one only when live). For a
 * double/colour, int or bool knob, a single value is applied to every dimension, otherwise one
 * value per dimension. The string overload sets a choice by option ID and a string knob by value.
 * Returns false after an ADD_FAILURE() when a node lacks the knob or its type does not fit.
 **/
bool setKnobOnBoth(const ParityPair& pair, const std::string& name, std::initializer_list<double> values);
bool setKnobOnBoth(const ParityPair& pair, const std::string& name, const std::string& value);

bool setKnobValues(const NodePtr& node, const std::string& name, const std::vector<double>& values);
bool setKnobValue(const NodePtr& node, const std::string& name, const std::string& value);

void setParitySourceComponents(const NodePtr& source, const std::string& componentsID);
void setParitySourceExtraLayer(const NodePtr& source, bool extraLayer);
void setParitySourceOrigin(const NodePtr& source, int x, int y);

/**
 * @brief The source's region of definition in pixel coordinates at `mipmapLevel`.
 **/
RectI paritySourceWindow(const NodePtr& source, double time, unsigned mipmapLevel);

/**
 * @brief Where compareParity() reads and writes references. Empty strings select the defaults:
 * NATRON_PARITY_RECORD_DIR for recording, NATRON_TESTS_FIXTURES_DIR/native-parity for replay.
 * `forceReplay` compares against the references even while the OFX plugin is loadable.
 **/
struct ParityOptions {
    std::string recordDir;
    std::string referenceDir;
    bool forceReplay = false;
    double time = 1.;
};

struct ParityResult {
    bool ok = false;
    bool live = false;
    int planesCompared = 0;
    std::string plane;
    std::string channel;
    int x = 0;
    int y = 0;
    float reference = 0.f;
    float native = 0.f;
    double maxAbsDiff = 0.;
    std::string error;
};

std::string describe(const ParityResult& r);

/**
 * @brief Renders `pair.native` and compares every channel of every plane it presents against the
 * reference, over `roi` (pixel coordinates at `mipmapLevel`; a null rect means the source's
 * region of definition at that level).
 *
 * Live (the OFX node exists and forceReplay is off): the reference is `pair.ofx` rendered the
 * same way, and both nodes must present the same planes. With `record` and a record directory,
 * the OFX planes are also written as parity references.
 * Replay: the reference is the recorded file for each plane, and every recorded plane of the case
 * must be presented by the native node.
 * With neither, the call adds a gtest failure naming what is missing. A mismatch does not: it is
 * returned with the first differing value and the largest absolute difference.
 *
 * References live at <dir>/<id>/<stem>.f32 for the colour plane and <dir>/<id>/<stem>.<layer>.f32
 * for any other, with stem `caseName`, suffixed ".mip<N>" above mipmap 0 so one case can be
 * recorded at several levels.
 **/
ParityResult compareParity(const ParityPair& pair,
                           const std::string& caseName,
                           const RectI& roi,
                           unsigned mipmapLevel,
                           const ParityTolerance& tolerance,
                           bool record,
                           const ParityOptions& options = ParityOptions());

std::string parityReferencePath(const std::string& dir,
                                const std::string& id,
                                const std::string& caseName,
                                unsigned mipmapLevel,
                                const ImageLayerDesc& layer);

/**
 * @brief The reference file format: "NPAR", then width, height and nComps as little-endian
 * int32, then width * height * nComps little-endian float32, row-major from the bottom row,
 * interleaved.
 **/
struct ParityF32Image {
    int width = 0;
    int height = 0;
    int nComps = 0;
    std::vector<float> pixels;
};

bool writeParityF32(const std::string& path, const ParityF32Image& image, std::string* error = 0);
bool readParityF32(const std::string& path, ParityF32Image* image, std::string* error = 0);

/**
 * @brief Every problem that keeps `nativeNode` from being a drop-in for `ofxNode`'s parameters:
 * each user-visible OFX knob must exist on the native node with the same script name, knob type,
 * dimension and default per dimension, a choice's default compared by option ID. Secret knobs,
 * pages, separators, the OFX-internal names (NatronOfxParamProcess*, unPremultBy*,
 * premultChanged, aChannelsChanged, bChannelsChanged) and `ignoredNames` are skipped.
 **/
std::vector<std::string> knobParityProblems(const NodePtr& ofxNode,
                                            const NodePtr& nativeNode,
                                            const std::vector<std::string>& ignoredNames = std::vector<std::string>());

void expectKnobParity(const NodePtr& ofxNode,
                      const NodePtr& nativeNode,
                      const std::vector<std::string>& ignoredNames = std::vector<std::string>());

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVEPARITY_H
