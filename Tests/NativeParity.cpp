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

std::string
describeLayers(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> names;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        names.push_back(it->getLayerID() + "(" + std::to_string(it->getNumComponents()) + ")");
    }

    return "[" + joinStrings(names) + "]";
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
connectBoth(const ParityPair& pair,
            const NodePtr& input,
            const std::string& inputLabel)
{
    bool ok = true;
    const NodePtr nodes[2] = { pair.ofx, pair.native };

    for (int i = 0; i < 2; ++i) {
        if (!nodes[i]) {
            continue;
        }
        const int inputNb = inputLabel.empty() ? 0 : findInputByLabel(nodes[i], inputLabel);
        if (inputNb < 0) {
            ADD_FAILURE() << nodes[i]->getPluginID() << " has no input labelled \"" << inputLabel << "\"";
            ok = false;
            continue;
        }
        if (!NodeCollection::connectNodes(inputNb, input, nodes[i])) {
            ADD_FAILURE() << "cannot connect a parity source to input " << inputNb << " of " << nodes[i]->getPluginID();
            ok = false;
        }
    }

    return ok;
}

std::string
formatDouble(double v)
{
    std::ostringstream ss;

    ss.precision(17);
    ss << v;

    return ss.str();
}

// The defaults of `knob`, one string per dimension, so knobs of any value type compare alike.
std::vector<std::string>
knobDefaults(const KnobIPtr& knob)
{
    std::vector<std::string> out;
    const int dims = knob->getDimension();

    if (KnobChoice* choice = dynamic_cast<KnobChoice*>(knob.get())) {
        for (int d = 0; d < dims; ++d) {
            out.push_back(choice->getEntry(choice->getDefaultValue(d)).id);
        }
    } else if (Knob<double>* dbl = dynamic_cast<Knob<double>*>(knob.get())) {
        for (int d = 0; d < dims; ++d) {
            out.push_back(formatDouble(dbl->getDefaultValue(d)));
        }
    } else if (Knob<int>* integer = dynamic_cast<Knob<int>*>(knob.get())) {
        for (int d = 0; d < dims; ++d) {
            out.push_back(std::to_string(integer->getDefaultValue(d)));
        }
    } else if (Knob<bool>* boolean = dynamic_cast<Knob<bool>*>(knob.get())) {
        for (int d = 0; d < dims; ++d) {
            out.push_back(boolean->getDefaultValue(d) ? "true" : "false");
        }
    } else if (Knob<std::string>* str = dynamic_cast<Knob<std::string>*>(knob.get())) {
        for (int d = 0; d < dims; ++d) {
            out.push_back(str->getDefaultValue(d));
        }
    }

    return out;
}

bool
isOfxInternalKnobName(const std::string& name)
{
    static const char* const prefixes[] = { "NatronOfxParamProcess", "unPremultBy" };
    static const char* const names[] = { "premultChanged", "aChannelsChanged", "bChannelsChanged" };

    for (const char* prefix : prefixes) {
        if (name.compare(0, std::strlen(prefix), prefix) == 0) {
            return true;
        }
    }
    for (const char* n : names) {
        if (name == n) {
            return true;
        }
    }

    return false;
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
             const std::string& what,
             double time,
             unsigned mipmapLevel,
             const RectI& window,
             const std::list<ImageLayerDesc>& layers,
             std::vector<RenderedPlane>* planes,
             ParityResult* r)
{
    std::string error;

    if (!renderNodePlanesDirect(node, time, ViewIdx(0), mipmapLevel, window, layers, planes, &error)) {
        r->error = what + " render failed: " + error;

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
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

void
ParitySourceTestEffect::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

NativePluginDescription
ParitySourceTestEffect::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = kTestPluginIDParitySource;
    desc.label = "Test Parity Source";
    desc.description = "";
    desc.outputKind = eDataKindImage;

    return desc;
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

void
ParitySourceTestEffect::getOrigin(int* x,
                                  int* y) const
{
    KnobIntPtr origin = _origin.lock();

    *x = origin ? origin->getValue(0) : 0;
    *y = origin ? origin->getValue(1) : 0;
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
ParitySourceTestEffect::getPreferredMetadata(NodeMetadata& metadata)
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
                    pixel[c] = isColor ? paritySourceColorValue(colorChannel[c], cx, cy, args.time) : paritySourceExtraValue(c, cx, cy, args.time);
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
               int ofxMajor,
               int nativeMajor,
               const std::string& maskInputLabel)
{
    ParityPair pair;

    pair.id = id;
    pair.ofxMajor = ofxMajor;
    pair.nativeMajor = nativeMajor;
    pair.app = app;
    pair.source = createNodeAtMajor(app, kTestPluginIDParitySource, -1);
    if (!pair.source) {
        return pair;
    }
    if (isPluginMajorRegistered(id, ofxMajor)) {
        pair.ofx = createNodeAtMajor(app, id, ofxMajor);
    }
    pair.native = createNodeAtMajor(app, id, nativeMajor);
    if (!pair.native) {
        return pair;
    }
    connectBoth(pair, pair.source, std::string());
    if (!maskInputLabel.empty()) {
        pair.mask = connectParityInput(pair, maskInputLabel);
    }

    return pair;
}

NodePtr
connectParityInput(ParityPair& pair,
                   const std::string& inputLabel)
{
    NodePtr source = createNodeAtMajor(pair.app, kTestPluginIDParitySource, -1);

    if (!source || !connectBoth(pair, source, inputLabel)) {
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
    const std::vector<double> v(values);
    bool ok = setKnobValues(pair.native, name, v);

    if (pair.ofx) {
        ok = setKnobValues(pair.ofx, name, v) && ok;
    }

    return ok;
}

bool
setKnobOnBoth(const ParityPair& pair,
              const std::string& name,
              const std::string& value)
{
    bool ok = setKnobValue(pair.native, name, value);

    if (pair.ofx) {
        ok = setKnobValue(pair.ofx, name, value) && ok;
    }

    return ok;
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
    ss << (r.live ? "live" : "replay") << ", " << r.planesCompared << " plane(s)";
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

    std::vector<RenderedPlane> reference;
    std::vector<RenderedPlane> native;
    r.live = pair.live() && !options.forceReplay;
    if (r.live) {
        const std::list<ImageLayerDesc> ofxLayers = presentLayers(pair.ofx, time);
        bool sameSet = (ofxLayers.size() == nativeLayers.size());
        for (std::list<ImageLayerDesc>::const_iterator it = ofxLayers.begin(); sameSet && it != ofxLayers.end(); ++it) {
            sameSet = std::find(nativeLayers.begin(), nativeLayers.end(), *it) != nativeLayers.end();
        }
        if (!sameSet) {
            r.error = "planes differ: ofx " + describeLayers(ofxLayers) + ", native " + describeLayers(nativeLayers);

            return r;
        }
        if (!renderPlanes(pair.ofx, "ofx", time, mipmapLevel, window, ofxLayers, &reference, &r) || !renderPlanes(pair.native, "native", time, mipmapLevel, window, ofxLayers, &native, &r)) {
            return r;
        }
        const std::string recordDir = recordDirFor(options);
        if (record && !recordDir.empty()) {
            for (std::size_t i = 0; i < reference.size(); ++i) {
                ParityF32Image img;
                img.width = reference[i].window.width();
                img.height = reference[i].window.height();
                img.nComps = static_cast<int>(reference[i].channels.size());
                img.pixels = reference[i].pixels;
                std::string error;
                if (!writeParityF32(parityReferencePath(recordDir, pair.id, caseName, mipmapLevel, reference[i].layer), img, &error)) {
                    ADD_FAILURE() << "cannot record " << pair.id << " / " << caseName << ": " << error;
                }
            }
        }
    } else {
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
            r.error = "neither " + pair.id + " at major " + std::to_string(pair.ofxMajor) + " nor the reference(s) " + joinStrings(missing) + " are available";
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
        if (!renderPlanes(pair.native, "native", time, mipmapLevel, window, loaded, &native, &r)) {
            return r;
        }
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

std::vector<std::string>
knobParityProblems(const NodePtr& ofxNode,
                   const NodePtr& nativeNode,
                   const std::vector<std::string>& ignoredNames)
{
    std::vector<std::string> problems;

    if (!ofxNode || !nativeNode) {
        problems.push_back("a node is missing");

        return problems;
    }
    const std::vector<KnobIPtr>& knobs = ofxNode->getKnobs();
    for (std::vector<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        const KnobIPtr& knob = *it;
        if (!knob || knob->getIsSecret() || dynamic_cast<KnobPage*>(knob.get()) || dynamic_cast<KnobSeparator*>(knob.get())) {
            continue;
        }
        const std::string& name = knob->getName();
        if (isOfxInternalKnobName(name) || (std::find(ignoredNames.begin(), ignoredNames.end(), name) != ignoredNames.end())) {
            continue;
        }
        KnobIPtr other = nativeNode->getKnobByName(name);
        if (!other) {
            problems.push_back("\"" + name + "\" is missing on the native node");
            continue;
        }
        if (other->typeName() != knob->typeName()) {
            problems.push_back("\"" + name + "\" is a " + other->typeName() + ", the OFX knob a " + knob->typeName());
            continue;
        }
        if (other->getDimension() != knob->getDimension()) {
            problems.push_back("\"" + name + "\" has " + std::to_string(other->getDimension()) + " dimensions, the OFX knob " + std::to_string(knob->getDimension()));
            continue;
        }
        const std::vector<std::string> a = knobDefaults(knob);
        const std::vector<std::string> b = knobDefaults(other);
        if (a != b) {
            problems.push_back("\"" + name + "\" defaults to [" + joinStrings(b) + "], the OFX knob to [" + joinStrings(a) + "]");
        }
    }

    return problems;
}

void
expectKnobParity(const NodePtr& ofxNode,
                 const NodePtr& nativeNode,
                 const std::vector<std::string>& ignoredNames)
{
    const std::vector<std::string> problems = knobParityProblems(ofxNode, nativeNode, ignoredNames);

    for (std::size_t i = 0; i < problems.size(); ++i) {
        ADD_FAILURE() << "knob parity: " << problems[i];
    }
}

NATRON_NAMESPACE_EXIT
