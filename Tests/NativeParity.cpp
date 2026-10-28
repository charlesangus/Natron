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

#include "NativeParity.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/NodeGroup.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

static_assert(std::endian::native == std::endian::little, "parity references are stored little-endian and read by memcpy");

NATRON_NAMESPACE_ENTER

namespace {

const char kParityMagic[4] = { 'N', 'P', 'A', 'R' };

int
floorDiv(int a,
         int b)
{
    const int q = a / b;

    return ((a % b) != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

void
fail(std::string* error,
     const std::string& message)
{
    if (error) {
        *error = message;
    }
}

std::string
joinStrings(const std::vector<std::string>& items)
{
    std::string out;

    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += items[i];
    }

    return out;
}

std::list<ImageLayerDesc>
presentLayers(const NodePtr& node,
              double time)
{
    std::list<ImageLayerDesc> layers;

    node->getEffectInstance()->getPresentLayers(time, ViewIdx(0), -1, &layers);

    return layers;
}

int
findInputByLabel(const NodePtr& node,
                 const std::string& label)
{
    const int n = node->getNInputs();

    for (int i = 0; i < n; ++i) {
        if (node->getInputLabel(i) == label) {
            return i;
        }
    }

    return -1;
}

bool
connectToNode(const ParityPair& pair,
              const NodePtr& input,
              const std::string& inputLabel)
{
    const int inputNb = inputLabel.empty() ? 0 : findInputByLabel(pair.native, inputLabel);

    if (inputNb < 0) {
        ADD_FAILURE() << pair.native->getPluginID() << " has no input labelled \"" << inputLabel << "\"";

        return false;
    }
    if (!NodeCollection::connectNodes(inputNb, input, pair.native)) {
        ADD_FAILURE() << "cannot connect a parity source to input " << inputNb << " of " << pair.native->getPluginID();

        return false;
    }

    return true;
}

std::string
recordDirFor(const ParityOptions& options)
{
    if (!options.recordDir.empty()) {
        return options.recordDir;
    }
    const char* env = std::getenv("NATRON_PARITY_RECORD_DIR");

    return env ? std::string(env) : std::string();
}

std::string
referenceDirFor(const ParityOptions& options)
{
    if (!options.referenceDir.empty()) {
        return options.referenceDir;
    }

    return std::string(NATRON_TESTS_FIXTURES_DIR) + "/native-parity";
}

std::string
caseStem(const std::string& caseName,
         unsigned mipmapLevel)
{
    return (mipmapLevel > 0) ? caseName + ".mip" + std::to_string(mipmapLevel) : caseName;
}

bool
isMipSuffix(const std::string& s)
{
    if ((s.size() < 4) || (s.compare(0, 3, "mip") != 0)) {
        return false;
    }
    std::size_t i = 3;
    while ((i < s.size()) && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
    }

    return (i > 3) && ((i == s.size()) || (s[i] == '.'));
}

// The layer part of every reference recorded for (caseName, mipmapLevel) in `caseDir`: empty for
// the colour plane, the layer ID otherwise.
std::set<std::string>
recordedPlaneNames(const std::string& caseDir,
                   const std::string& caseName,
                   unsigned mipmapLevel)
{
    std::set<std::string> names;
    const std::string stem = caseStem(caseName, mipmapLevel);
    QDir dir(QString::fromStdString(caseDir));
    const QStringList files = dir.entryList(QStringList() << QString::fromStdString(stem + "*.f32"), QDir::Files);

    for (const QString& f : files) {
        const std::string file = f.toStdString();
        const std::string rest = file.substr(stem.size(), file.size() - stem.size() - 4);
        if (rest.empty()) {
            names.insert(std::string());
        } else if ((rest[0] == '.') && !(mipmapLevel == 0 && isMipSuffix(rest.substr(1)))) {
            names.insert(rest.substr(1));
        }
    }

    return names;
}

void
compareOnePlane(const RenderedPlane& reference,
                const RenderedPlane& native,
                const ParityTolerance& tolerance,
                ParityResult* r)
{
    const std::size_t nComps = native.channels.size();

    for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
        const float a = reference.pixels[i];
        const float b = native.pixels[i];
        if (std::isfinite(a) && std::isfinite(b)) {
            r->maxAbsDiff = std::max(r->maxAbsDiff, std::fabs(static_cast<double>(a) - static_cast<double>(b)));
        }
        if (tolerance.accepts(a, b)) {
            continue;
        }
        if (!r->ok) {
            continue;
        }
        r->ok = false;
        const std::size_t pixel = i / nComps;
        r->plane = native.layer.getLayerID();
        r->channel = native.channels[i % nComps];
        r->x = native.window.x1 + static_cast<int>(pixel % native.window.width());
        r->y = native.window.y1 + static_cast<int>(pixel / native.window.width());
        r->reference = a;
        r->native = b;
    }
}

bool
renderPlanes(const NodePtr& node,
             double time,
             unsigned mipmapLevel,
             const RectI& window,
             const std::list<ImageLayerDesc>& layers,
             std::vector<RenderedPlane>* planes,
             ParityResult* r)
{
    std::string error;

    if (!renderNodePlanesDirect(node, time, ViewIdx(0), mipmapLevel, window, layers, planes, &error)) {
        r->error = "render failed: " + error;

        return false;
    }

    return true;
}

} // namespace

float
paritySourceColorValue(int channel,
                       int cx,
                       int cy,
                       double time)
{
    const double u = (cx + 0.5) / kParitySourceWidth;
    const double v = (cy + 0.5) / kParitySourceHeight;
    const double offset = 0.01 * time;

    switch (channel) {
    case 0:
        return static_cast<float>(-0.25 + 2. * u + 0.15 * std::sin(6.1 * v + 0.3) + offset);
    case 1:
        return static_cast<float>(-0.25 + 2. * v + 0.15 * std::sin(4.3 * u + 1.1) + offset);
    case 2:
        return static_cast<float>(-0.25 + (u + v) + 0.15 * std::sin(9.7 * u * v + 2.) + offset);
    default: {
        const int band = ((floorDiv(cy, 4) % 3) + 3) % 3;
        if (band == 0) {
            return 0.f;
        }
        if (band == 1) {
            return 1.f;
        }

        return static_cast<float>(std::fmod(u + offset, 1.));
    }
    }
}

float
paritySourceExtraValue(int channel,
                       int cx,
                       int cy,
                       double time)
{
    const double u = (cx + 0.5) / kParitySourceWidth;
    const double v = (cy + 0.5) / kParitySourceHeight;

    return static_cast<float>(0.5 + 0.75 * std::sin(3. * u + 5. * v + 1.7 * channel) + 0.01 * time);
}

float
paritySourceNoise(int channel,
                  int cx,
                  int cy)
{
    const std::uint32_t kSeed = 0x9e3779b9u;
    std::uint32_t h = (static_cast<std::uint32_t>(cx) * 0x8da6b343u) ^ (static_cast<std::uint32_t>(cy) * 0xd8163841u) ^ (static_cast<std::uint32_t>(channel) * 0xcb1ab31fu) ^ kSeed;

    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;

    // 24 bits scaled by 2^-24 and offset by 0.5 are exact in float, so the noise is the same on every platform.
    return static_cast<float>(h >> 8) * (1.f / 16777216.f) - 0.5f;
}

ImageLayerDesc
paritySourceExtraLayer()
{
    std::vector<std::string> rgb;

    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");

    return ImageLayerDesc(kParitySourceExtraLayerID, kParitySourceExtraLayerID, "RGB", rgb);
}

ParitySourceTestEffect::ParitySourceTestEffect(NodePtr n)
    : NativeEffectBase(n)
    , _components()
    , _extraLayer()
    , _origin()
    , _noise()
    , _nonFinite()
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

void
ParitySourceTestEffect::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
ParitySourceTestEffect::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));

    KnobChoicePtr components = createKnob<KnobChoice>(std::string("Components"));
    components->setName(kParitySourceParamComponents);
    components->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("rgba"));
        options.push_back(ChoiceOption("rgb"));
        options.push_back(ChoiceOption("alpha"));
        components->populateChoices(options);
    }
    components->setDefaultValue(0);
    components->setIsMetadataSlave(true);
    page->addKnob(components);
    _components = components;

    KnobBoolPtr extraLayer = createKnob<KnobBool>(std::string("Extra Layer"));
    extraLayer->setName(kParitySourceParamExtraLayer);
    extraLayer->setAnimationEnabled(false);
    extraLayer->setDefaultValue(false);
    extraLayer->setIsMetadataSlave(true);
    page->addKnob(extraLayer);
    _extraLayer = extraLayer;

    KnobIntPtr origin = createKnob<KnobInt>(std::string("Origin"), 2);
    origin->setName(kParitySourceParamOrigin);
    origin->setAnimationEnabled(false);
    origin->setDefaultValue(0, 0);
    origin->setDefaultValue(0, 1);
    page->addKnob(origin);
    _origin = origin;

    KnobBoolPtr noise = createKnob<KnobBool>(std::string("Noise"));
    noise->setName(kParitySourceParamNoise);
    noise->setAnimationEnabled(false);
    noise->setDefaultValue(false);
    page->addKnob(noise);
    _noise = noise;

    KnobIntPtr nonFinite = createKnob<KnobInt>(std::string("Non-Finite"), 4);
    nonFinite->setName(kParitySourceParamNonFinite);
    nonFinite->setAnimationEnabled(false);
    for (int i = 0; i < 4; ++i) {
        nonFinite->setDefaultValue(0, i);
    }
    page->addKnob(nonFinite);
    _nonFinite = nonFinite;
}

int
ParitySourceTestEffect::colorNComps() const
{
    KnobChoicePtr components = _components.lock();
    const int index = components ? components->getValue() : 0;

    switch (index) {
    case 1:
        return 3;
    case 2:
        return 1;
    default:
        return 4;
    }
}

bool
ParitySourceTestEffect::hasExtraLayer() const
{
    KnobBoolPtr extraLayer = _extraLayer.lock();

    return extraLayer ? extraLayer->getValue() : false;
}

bool
ParitySourceTestEffect::hasNoise() const
{
    KnobBoolPtr noise = _noise.lock();

    return noise ? noise->getValue() : false;
}

void
ParitySourceTestEffect::getOrigin(int* x,
                                  int* y) const
{
    KnobIntPtr origin = _origin.lock();

    *x = origin ? origin->getValue(0) : 0;
    *y = origin ? origin->getValue(1) : 0;
}

RectI
ParitySourceTestEffect::getNonFinite() const
{
    KnobIntPtr nonFinite = _nonFinite.lock();

    if (!nonFinite) {
        return RectI();
    }

    return RectI(nonFinite->getValue(0), nonFinite->getValue(1), nonFinite->getValue(2), nonFinite->getValue(3));
}

StatusEnum
ParitySourceTestEffect::getRegionOfDefinition(U64 /*hash*/,
                                              double /*time*/,
                                              const RenderScale& /*scale*/,
                                              ViewIdx /*view*/,
                                              RectD* rod)
{
    int ox, oy;
    getOrigin(&ox, &oy);
    rod->x1 = ox;
    rod->y1 = oy;
    rod->x2 = ox + kParitySourceWidth;
    rod->y2 = oy + kParitySourceHeight;

    return eStatusOK;
}

StatusEnum
ParitySourceTestEffect::getPreferredMetadataAt(double /*time*/,
                                               ViewIdx /*view*/,
                                               NodeMetadata& metadata)
{
    metadata.setNComps(-1, colorNComps());
    metadata.setBitDepth(-1, eImageBitDepthFloat);
    metadata.setIsFrameVarying(true);

    return eStatusOK;
}

void
ParitySourceTestEffect::getComponentsNeededAndProduced(double time,
                                                       ViewIdx view,
                                                       EffectInstance::ComponentsNeededMap* comps,
                                                       double* passThroughTime,
                                                       int* passThroughView,
                                                       int* passThroughInputNb)
{
    std::list<ImageLayerDesc>& produced = (*comps)[-1];

    produced.clear();
    if (hasExtraLayer()) {
        produced.push_back(paritySourceExtraLayer());
    }
    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInputNb = -1;
}

StatusEnum
ParitySourceTestEffect::render(const RenderActionArgs& args)
{
    const unsigned mipmapLevel = args.mappedScale.toMipmapLevel();
    const int step = 1 << mipmapLevel;
    const bool noise = hasNoise();
    const RectI nonFinite = getNonFinite();
    const float nonFiniteValues[3] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
    int ox, oy;
    getOrigin(&ox, &oy);

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
        const bool isExtra = !isColor && (layer.getLayerID() == kParitySourceExtraLayerID);
        if (!isColor && !isExtra) {
            image->fillZero(args.roi);
            continue;
        }
        const int nComps = static_cast<int>(image->getComponentsCount());
        int colorChannel[4] = { 0, 1, 2, 3 };
        if (isColor && (nComps == 1)) {
            colorChannel[0] = 3;
        }
        Image::WriteAccess access(image.get());
        for (int y = args.roi.y1; y < args.roi.y2; ++y) {
            const int cy = y * step + step / 2 - oy;
            for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                const int cx = x * step + step / 2 - ox;
                float* pixel = reinterpret_cast<float*>(access.pixelAt(x, y));
                if (!pixel) {
                    return eStatusFailed;
                }
                for (int c = 0; c < nComps; ++c) {
                    const int channel = isColor ? colorChannel[c] : c;
                    float v = isColor ? paritySourceColorValue(channel, cx, cy, args.time) : paritySourceExtraValue(channel, cx, cy, args.time);
                    if (noise) {
                        const float n = 0.1f * paritySourceNoise(isColor ? channel : 4 + channel, cx, cy);
                        if (!isColor || (channel < 3)) {
                            v += n;
                        } else if ((v != 0.f) && (v != 1.f)) {
                            v = std::min(1.f, std::max(0.f, v + n));
                        }
                    }
                    if (isColor && (channel < 3) && nonFinite.contains(cx, cy)) {
                        v = nonFiniteValues[channel];
                    }
                    pixel[c] = v;
                }
            }
        }
    }

    return eStatusOK;
} // ParitySourceTestEffect::render

bool
ParityTolerance::accepts(float reference,
                         float native) const
{
    if (std::isnan(reference) || std::isnan(native)) {
        return std::isnan(reference) && std::isnan(native);
    }
    if (reference == native) {
        return true;
    }
    const double diff = std::fabs(static_cast<double>(reference) - static_cast<double>(native));
    const double bound = absolute + relative * std::max(1., std::fabs(static_cast<double>(reference)));

    return diff <= bound;
}

bool
isPluginMajorRegistered(const std::string& id,
                        int major)
{
    try {
        Plugin* p = appPTR->getPluginBinary(QString::fromStdString(id), major, -1, false);

        return p && (p->getMajorVersion() == major);
    } catch (const std::exception&) {
        return false;
    }
}

NodePtr
createNodeAtMajor(const AppInstancePtr& app,
                  const std::string& id,
                  int major)
{
    if (!app) {
        ADD_FAILURE() << "no app to create " << id << " in";

        return NodePtr();
    }
    if ((major >= 0) && !isPluginMajorRegistered(id, major)) {
        ADD_FAILURE() << id << " is not registered at major " << major;

        return NodePtr();
    }
    CreateNodeArgs args(id, app->getProject());
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, major, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);
    NodePtr node = app->createNode(args);
    if (!node) {
        ADD_FAILURE() << "cannot create " << id << " at major " << major;

        return NodePtr();
    }
    if ((major >= 0) && (!node->getPlugin() || (node->getPlugin()->getMajorVersion() != major))) {
        ADD_FAILURE() << id << " was created at major " << (node->getPlugin() ? node->getPlugin()->getMajorVersion() : -1) << ", not " << major;
    }

    return node;
}

ParityPair
makeParityPair(const AppInstancePtr& app,
               const std::string& id,
               int nativeMajor,
               const std::string& maskInputLabel)
{
    ParityPair pair;

    pair.id = id;
    pair.nativeMajor = nativeMajor;
    pair.app = app;
    pair.source = createNodeAtMajor(app, kTestPluginIDParitySource, -1);
    if (!pair.source) {
        return pair;
    }
    pair.native = createNodeAtMajor(app, id, nativeMajor);
    if (!pair.native) {
        return pair;
    }
    connectToNode(pair, pair.source, std::string());
    if (!maskInputLabel.empty()) {
        pair.mask = connectParityInput(pair, maskInputLabel);
    }

    return pair;
}

NodePtr
connectParityInput(ParityPair& pair,
                   const std::string& inputLabel)
{
    if (!pair.native) {
        ADD_FAILURE() << "no node to connect " << inputLabel << " to";

        return NodePtr();
    }
    NodePtr source = createNodeAtMajor(pair.app, kTestPluginIDParitySource, -1);

    if (!source || !connectToNode(pair, source, inputLabel)) {
        return NodePtr();
    }

    return source;
}

bool
setKnobValues(const NodePtr& node,
              const std::string& name,
              const std::vector<double>& values)
{
    KnobIPtr knob = node ? node->getKnobByName(name) : KnobIPtr();

    if (!knob) {
        ADD_FAILURE() << (node ? node->getPluginID() : std::string("null node")) << " has no knob \"" << name << "\"";

        return false;
    }
    const int dims = knob->getDimension();
    if (values.empty() || ((values.size() != 1) && (static_cast<int>(values.size()) != dims))) {
        ADD_FAILURE() << "knob \"" << name << "\" has " << dims << " dimensions, " << values.size() << " values given";

        return false;
    }
    Knob<double>* dbl = dynamic_cast<Knob<double>*>(knob.get());
    Knob<int>* integer = dynamic_cast<Knob<int>*>(knob.get());
    Knob<bool>* boolean = dynamic_cast<Knob<bool>*>(knob.get());
    if (!dbl && !integer && !boolean) {
        ADD_FAILURE() << "knob \"" << name << "\" of type " << knob->typeName() << " does not take numbers";

        return false;
    }
    knob->beginChanges();
    for (int d = 0; d < dims; ++d) {
        const double v = (values.size() == 1) ? values[0] : values[d];
        if (dbl) {
            dbl->setValue(v, ViewSpec::all(), d);
        } else if (integer) {
            integer->setValue(static_cast<int>(std::lround(v)), ViewSpec::all(), d);
        } else {
            boolean->setValue(v != 0., ViewSpec::all(), d);
        }
    }
    knob->endChanges();

    return true;
}

bool
setKnobValue(const NodePtr& node,
             const std::string& name,
             const std::string& value)
{
    KnobIPtr knob = node ? node->getKnobByName(name) : KnobIPtr();

    if (!knob) {
        ADD_FAILURE() << (node ? node->getPluginID() : std::string("null node")) << " has no knob \"" << name << "\"";

        return false;
    }
    if (KnobChoice* choice = dynamic_cast<KnobChoice*>(knob.get())) {
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == value) {
                choice->setValueFromID(value, 0);

                return true;
            }
        }
        ADD_FAILURE() << "choice \"" << name << "\" has no option \"" << value << "\"";

        return false;
    }
    if (Knob<std::string>* str = dynamic_cast<Knob<std::string>*>(knob.get())) {
        str->setValue(value);

        return true;
    }
    ADD_FAILURE() << "knob \"" << name << "\" of type " << knob->typeName() << " does not take a string";

    return false;
}

bool
setKnobOnBoth(const ParityPair& pair,
              const std::string& name,
              std::initializer_list<double> values)
{
    return setKnobValues(pair.native, name, std::vector<double>(values));
}

bool
setKnobOnBoth(const ParityPair& pair,
              const std::string& name,
              const std::string& value)
{
    return setKnobValue(pair.native, name, value);
}

void
setParitySourceComponents(const NodePtr& source,
                          const std::string& componentsID)
{
    setKnobValue(source, kParitySourceParamComponents, componentsID);
}

void
setParitySourceExtraLayer(const NodePtr& source,
                          bool extraLayer)
{
    setKnobValues(source, kParitySourceParamExtraLayer, std::vector<double>(1, extraLayer ? 1. : 0.));
}

void
setParitySourceNoise(const NodePtr& source,
                     bool noise)
{
    setKnobValues(source, kParitySourceParamNoise, std::vector<double>(1, noise ? 1. : 0.));
}

void
setParitySourceNonFinite(const NodePtr& source,
                         const RectI& rect)
{
    setKnobValues(source, kParitySourceParamNonFinite, { (double)rect.x1, (double)rect.y1, (double)rect.x2, (double)rect.y2 });
}

void
setParitySourceOrigin(const NodePtr& source,
                      int x,
                      int y)
{
    std::vector<double> origin;

    origin.push_back(x);
    origin.push_back(y);
    setKnobValues(source, kParitySourceParamOrigin, origin);
}

RectI
paritySourceWindow(const NodePtr& source,
                   double /*time*/,
                   unsigned mipmapLevel)
{
    KnobIntPtr origin = source ? std::dynamic_pointer_cast<KnobInt>(source->getKnobByName(kParitySourceParamOrigin)) : KnobIntPtr();
    const int ox = origin ? origin->getValue(0) : 0;
    const int oy = origin ? origin->getValue(1) : 0;
    const RectD rod(ox, oy, ox + kParitySourceWidth, oy + kParitySourceHeight);

    return rod.toPixelEnclosing(mipmapLevel, 1.);
}

std::string
parityReferencePath(const std::string& dir,
                    const std::string& id,
                    const std::string& caseName,
                    unsigned mipmapLevel,
                    const ImageLayerDesc& layer)
{
    std::string path = dir + "/" + id + "/" + caseStem(caseName, mipmapLevel);

    if (!layer.isColorLayer()) {
        path += "." + layer.getLayerID();
    }

    return path + ".f32";
}

bool
writeParityF32(const std::string& path,
               const ParityF32Image& image,
               std::string* error)
{
    const std::size_t expected = static_cast<std::size_t>(image.width) * image.height * image.nComps;

    if ((image.width <= 0) || (image.height <= 0) || (image.nComps <= 0) || (image.pixels.size() != expected)) {
        fail(error, "inconsistent image for " + path);

        return false;
    }
    const QString dir = QFileInfo(QString::fromStdString(path)).absolutePath();
    if (!QDir().mkpath(dir)) {
        fail(error, "cannot create " + dir.toStdString());

        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        fail(error, "cannot open " + path + " for writing");

        return false;
    }
    const std::int32_t header[3] = { image.width, image.height, image.nComps };
    out.write(kParityMagic, sizeof(kParityMagic));
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(image.pixels.data()), static_cast<std::streamsize>(expected * sizeof(float)));
    out.close();
    if (!out) {
        fail(error, "cannot write " + path);

        return false;
    }

    return true;
}

bool
readParityF32(const std::string& path,
              ParityF32Image* image,
              std::string* error)
{
    std::ifstream in(path, std::ios::binary);

    if (!in) {
        fail(error, "cannot open " + path);

        return false;
    }
    char magic[4];
    std::int32_t header[3];
    in.read(magic, sizeof(magic));
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || (std::memcmp(magic, kParityMagic, sizeof(magic)) != 0)) {
        fail(error, path + " is not a parity reference");

        return false;
    }
    if ((header[0] <= 0) || (header[1] <= 0) || (header[2] <= 0) || (header[2] > 4)) {
        fail(error, path + " has an invalid header");

        return false;
    }
    const std::size_t count = static_cast<std::size_t>(header[0]) * header[1] * header[2];
    std::vector<float> pixels(count);
    in.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(count * sizeof(float)));
    if (!in) {
        fail(error, path + " is truncated");

        return false;
    }
    in.peek();
    if (!in.eof()) {
        fail(error, path + " has trailing data");

        return false;
    }
    image->width = header[0];
    image->height = header[1];
    image->nComps = header[2];
    image->pixels.swap(pixels);

    return true;
}

std::string
describe(const ParityResult& r)
{
    std::ostringstream ss;

    ss.precision(9);
    ss << (r.recorded ? "recorded" : "replay") << ", " << r.planesCompared << " plane(s)";
    if (r.recorded) {
        return ss.str();
    }
    if (r.ok) {
        ss << ": match (max abs diff " << r.maxAbsDiff << ")";

        return ss.str();
    }
    if (!r.plane.empty()) {
        ss << ": plane " << r.plane << " pixel (" << r.x << "," << r.y << ") channel " << r.channel
           << " reference=" << r.reference << " native=" << r.native << " (max abs diff " << r.maxAbsDiff << ")";
    }
    if (!r.error.empty()) {
        ss << " [" << r.error << "]";
    }

    return ss.str();
}

ParityResult
compareParity(const ParityPair& pair,
              const std::string& caseName,
              const RectI& roi,
              unsigned mipmapLevel,
              const ParityTolerance& tolerance,
              bool record,
              const ParityOptions& options)
{
    ParityResult r;

    if (!pair.native || !pair.source) {
        r.error = "the pair has no native node or no source";
        ADD_FAILURE() << pair.id << " / " << caseName << ": " << r.error;

        return r;
    }
    const double time = options.time;
    const RectI window = roi.isNull() ? paritySourceWindow(pair.source, time, mipmapLevel) : roi;
    const std::list<ImageLayerDesc> nativeLayers = presentLayers(pair.native, time);

    const std::string recordDir = recordDirFor(options);
    if (record && !recordDir.empty()) {
        std::vector<RenderedPlane> recorded;
        if (!renderPlanes(pair.native, time, mipmapLevel, window, nativeLayers, &recorded, &r)) {
            return r;
        }
        for (std::size_t i = 0; i < recorded.size(); ++i) {
            ParityF32Image img;
            img.width = recorded[i].window.width();
            img.height = recorded[i].window.height();
            img.nComps = static_cast<int>(recorded[i].channels.size());
            img.pixels = recorded[i].pixels;
            std::string error;
            if (!writeParityF32(parityReferencePath(recordDir, pair.id, caseName, mipmapLevel, recorded[i].layer), img, &error)) {
                r.error = "cannot record " + pair.id + " / " + caseName + ": " + error;
                ADD_FAILURE() << r.error;

                return r;
            }
        }
        r.ok = true;
        r.recorded = true;
        r.planesCompared = static_cast<int>(recorded.size());

        return r;
    }

    std::vector<RenderedPlane> reference;
    std::vector<RenderedPlane> native;
    const std::string refDir = referenceDirFor(options);
    const std::string caseDir = refDir + "/" + pair.id;
    std::vector<std::string> missing;
    std::list<ImageLayerDesc> loaded;
    for (std::list<ImageLayerDesc>::const_iterator it = nativeLayers.begin(); it != nativeLayers.end(); ++it) {
        const std::string path = parityReferencePath(refDir, pair.id, caseName, mipmapLevel, *it);
        if (!QFileInfo::exists(QString::fromStdString(path))) {
            missing.push_back(path);
            continue;
        }
        ParityF32Image img;
        std::string error;
        if (!readParityF32(path, &img, &error)) {
            r.error = error;

            return r;
        }
        if ((img.width != window.width()) || (img.height != window.height()) || (img.nComps != it->getNumComponents())) {
            std::ostringstream ss;
            ss << path << " is " << img.width << "x" << img.height << "x" << img.nComps << ", the case renders "
               << window.width() << "x" << window.height() << "x" << it->getNumComponents();
            r.error = ss.str();

            return r;
        }
        RenderedPlane plane;
        plane.layer = *it;
        plane.window = window;
        plane.channels = it->getChannels();
        plane.pixels.swap(img.pixels);
        reference.push_back(plane);
        loaded.push_back(*it);
    }
    if (!missing.empty()) {
        r.error = pair.id + " / " + caseName + " has no recorded reference " + joinStrings(missing);
        ADD_FAILURE() << r.error;

        return r;
    }
    std::set<std::string> recorded = recordedPlaneNames(caseDir, caseName, mipmapLevel);
    for (std::list<ImageLayerDesc>::const_iterator it = nativeLayers.begin(); it != nativeLayers.end(); ++it) {
        recorded.erase(it->isColorLayer() ? std::string() : it->getLayerID());
    }
    if (!recorded.empty()) {
        r.error = "the native node does not present the recorded plane(s) " + joinStrings(std::vector<std::string>(recorded.begin(), recorded.end()));

        return r;
    }
    if (!renderPlanes(pair.native, time, mipmapLevel, window, loaded, &native, &r)) {
        return r;
    }

    r.ok = true;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (reference[i].channels.size() != native[i].channels.size()) {
            r.ok = false;
            r.error = "plane " + native[i].layer.getLayerID() + " has " + std::to_string(native[i].channels.size()) + " channels, the reference "
                + std::to_string(reference[i].channels.size());

            return r;
        }
        compareOnePlane(reference[i], native[i], tolerance, &r);
        ++r.planesCompared;
    }

    return r;
} // compareParity

NATRON_NAMESPACE_EXIT
