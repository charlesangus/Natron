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

#include "NativeGolden.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <list>
#include <set>
#include <sstream>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kGoldenTargetLayerID = "depth";

#ifndef NATRON_GOLDEN_CONFIG
#define NATRON_GOLDEN_CONFIG "unknown"
#endif

// Optimisation level, FMA contraction and vectorisation move the last ulps of the same source, so
// a reference only binds bit for bit under the configuration that recorded it.
const ParityTolerance&
crossConfigTolerance()
{
    static const ParityTolerance tolerance = ParityTolerance::iir();

    return tolerance;
}

std::string
stampPath(const std::string& dir)
{
    return dir + "/STAMP";
}

bool
writeStamp(const std::string& dir)
{
    QDir().mkpath(QString::fromStdString(dir));
    QFile file(QString::fromStdString(stampPath(dir)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const std::string line = std::string(NATRON_GOLDEN_CONFIG) + "\n";

    return file.write(line.data(), static_cast<qint64>(line.size())) == static_cast<qint64>(line.size());
}

// The recording configuration, or empty when the stamp is missing.
std::string
readStamp(const std::string& dir)
{
    QFile file(QString::fromStdString(stampPath(dir)));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::string();
    }
    std::string line = QString::fromUtf8(file.readLine()).trimmed().toStdString();

    return line;
}

// True when the references were recorded under this build's configuration. The first mismatch in
// the process prints one line.
bool
referencesMatchConfig(const std::string& dir)
{
    static const bool strict = [&dir]() {
        const std::string recorded = readStamp(dir);
        if (recorded == NATRON_GOLDEN_CONFIG) {
            return true;
        }
        std::cout << "[ golden ] config mismatch: references recorded under \"" << (recorded.empty() ? std::string("no STAMP") : recorded)
                  << "\", running under \"" << NATRON_GOLDEN_CONFIG << "\"; comparing within tolerance instead of bit for bit" << std::endl;

        return false;
    }();

    return strict;
}

std::string
planeSuffix(const ImageLayerDesc& layer)
{
    return layer.isColorLayer() ? std::string() : layer.getLayerID();
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

// The plane suffixes of every reference recorded for `caseName` in `caseDir`.
std::set<std::string>
recordedPlanes(const std::string& caseDir,
               const std::string& caseName)
{
    std::set<std::string> planes;
    const QDir dir(QString::fromStdString(caseDir));
    const QStringList patterns = QStringList() << QString::fromStdString(caseName + ".f32") << QString::fromStdString(caseName + ".*.f32");
    const QStringList files = dir.entryList(patterns, QDir::Files);

    for (const QString& f : files) {
        const std::string file = f.toStdString();
        const std::string rest = file.substr(caseName.size(), file.size() - caseName.size() - 4);
        planes.insert(rest.empty() ? std::string() : rest.substr(1));
    }

    return planes;
}

bool
sameValue(float reference,
          float rendered)
{
    if (std::isnan(reference) || std::isnan(rendered)) {
        return std::isnan(reference) && std::isnan(rendered);
    }

    return std::bit_cast<std::uint32_t>(reference) == std::bit_cast<std::uint32_t>(rendered);
}

// Compares one plane, bit for bit when `strict` and within crossConfigTolerance() otherwise, and
// reports the first difference and the count.
bool
expectSamePlane(const std::string& what,
                const ParityF32Image& reference,
                const RenderedPlane& rendered,
                bool strict)
{
    const std::size_t nComps = rendered.channels.size();
    std::size_t mismatches = 0;
    std::size_t first = 0;
    double maxAbsDiff = 0.;

    for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
        const float a = reference.pixels[i];
        const float b = rendered.pixels[i];
        if (strict ? sameValue(a, b) : crossConfigTolerance().accepts(a, b)) {
            continue;
        }
        if (mismatches == 0) {
            first = i;
        }
        ++mismatches;
        if (std::isfinite(a) && std::isfinite(b)) {
            maxAbsDiff = std::max(maxAbsDiff, std::fabs(static_cast<double>(a) - static_cast<double>(b)));
        }
    }
    if (mismatches == 0) {
        return true;
    }
    const std::size_t pixel = first / nComps;
    std::ostringstream ss;
    ss.precision(9);
    ss << what << ": " << mismatches << " of " << reference.pixels.size() << " values differ from the golden reference" << (strict ? "" : " beyond the cross-config tolerance") << "; first at pixel ("
       << rendered.window.x1 + static_cast<int>(pixel % rendered.window.width()) << "," << rendered.window.y1 + static_cast<int>(pixel / rendered.window.width())
       << ") channel " << rendered.channels[first % nComps] << ": golden " << reference.pixels[first] << ", rendered " << rendered.pixels[first]
       << " (max abs diff " << maxAbsDiff << ")";
    ADD_FAILURE() << ss.str();

    return false;
}

NodePtr
makeSource(const AppInstancePtr& app,
           int originX,
           int originY)
{
    NodePtr source = createNodeAtMajor(app, kTestPluginIDParitySource, -1);

    if (source) {
        setParitySourceNoise(source, true);
        setParitySourceOrigin(source, originX, originY);
    }

    return source;
}

int
inputNamed(const NodePtr& node,
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
connectInput(const NodePtr& input,
             const NodePtr& node,
             const std::string& label)
{
    const int inputNb = label.empty() ? 0 : inputNamed(node, label);

    if (inputNb < 0) {
        ADD_FAILURE() << node->getPluginID() << " has no input labelled \"" << label << "\"";

        return false;
    }
    if (!NodeCollection::connectNodes(inputNb, input, node)) {
        ADD_FAILURE() << "cannot connect a source to input " << inputNb << " of " << node->getPluginID();

        return false;
    }

    return true;
}

void
resetGoldenProject(const AppInstancePtr& app)
{
    ProjectPtr project = app->getProject();

    project->reset(false, true);
    project->setAutoSetProjectFormatEnabled(true);
    project->setOrAddProjectFormat(Format(0, 0, kGoldenWidth, kGoldenHeight, "nativeGolden64x48", 1.));
}

// Points the node at its non-colour layer and returns that layer's ID, or empty after a gtest
// failure.
std::string
selectNonColorLayer(const GoldenGraph& graph,
                    const GoldenNodeSpec& spec)
{
    const NodePtr& node = graph.node;
    const KnobIPtr knob = node->getLayerKnob();
    KnobLayerSelectPtr layerSelect = std::dynamic_pointer_cast<KnobLayerSelect>(knob);

    if (layerSelect && node->isTargetLayerKnob(knob)) {
        layerSelect->setLayer(kGoldenTargetLayerID);

        return kGoldenTargetLayerID;
    }
    if (spec.generator) {
        ADD_FAILURE() << spec.name << " is a generator without a target layer knob";

        return std::string();
    }
    setParitySourceExtraLayer(graph.source, true);
    for (std::size_t i = 0; i < graph.inputs.size(); ++i) {
        setParitySourceExtraLayer(graph.inputs[i], true);
    }
    if (KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(knob)) {
        channelSet->setLayer(0, kParitySourceExtraLayerID, NULL);
    } else if (layerSelect) {
        layerSelect->setLayer(kParitySourceExtraLayerID);
    }

    return kParitySourceExtraLayerID;
}

// Builds the graph of one case in a reset project, or returns a graph without a node after a
// gtest failure.
GoldenGraph
buildGraph(const AppInstancePtr& app,
           const GoldenNodeSpec& spec,
           const GoldenVariant& variant)
{
    GoldenGraph graph;

    resetGoldenProject(app);
    NodePtr node = createNodeAtMajor(app, spec.pluginID, spec.major);
    if (!node) {
        return graph;
    }
    if (!spec.generator) {
        graph.source = makeSource(app, 0, 0);
        if (!graph.source || !connectInput(graph.source, node, std::string())) {
            return graph;
        }
        const int origins[3][2] = { { 8, 4 }, { -6, 10 }, { 12, 6 } };
        for (std::size_t i = 0; i < spec.extraInputs.size(); ++i) {
            NodePtr input = makeSource(app, origins[i % 3][0], origins[i % 3][1]);
            if (!input || !connectInput(input, node, spec.extraInputs[i])) {
                return graph;
            }
            graph.inputs.push_back(input);
        }
    }
    if (variant.mask) {
        if (spec.maskInput.empty()) {
            ADD_FAILURE() << spec.name << " / " << variant.name << " asks for a mask, the node has none";

            return graph;
        }
        graph.mask = makeSource(app, -5, 7);
        if (!graph.mask || !connectInput(graph.mask, node, spec.maskInput)) {
            return graph;
        }
        if (!setKnobValues(node, std::string("enableMask_") + spec.maskInput, std::vector<double>(1, 1.)) || !setGoldenChannelSelect(node, std::string("maskChannel_") + spec.maskInput, "rgba.A")) {
            return graph;
        }
    }
    graph.node = node;

    return graph;
}

std::string
scaleName(unsigned mipmapLevel)
{
    return (mipmapLevel == 0) ? "s1" : "s05";
}

} // namespace

std::string
goldenRecordDir()
{
    const char* env = std::getenv("NATRON_GOLDEN_RECORD_DIR");

    return (env && *env) ? std::string(env) : std::string();
}

std::string
goldenReferenceDir()
{
    return std::string(NATRON_TESTS_FIXTURES_DIR) + "/native-golden";
}

RectI
goldenWindow(unsigned mipmapLevel)
{
    return RectD(0., 0., kGoldenWidth, kGoldenHeight).toPixelEnclosing(mipmapLevel, 1.);
}

bool
setGoldenChannelSelect(const NodePtr& node,
                       const std::string& name,
                       const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    if (!select) {
        ADD_FAILURE() << (node ? node->getPluginID() : std::string("null node")) << " has no channel select \"" << name << "\"";

        return false;
    }
    select->set(value);

    return true;
}

bool
pressGoldenButton(const NodePtr& node,
                  const std::string& name)
{
    KnobButton* button = node ? dynamic_cast<KnobButton*>(node->getKnobByName(name).get()) : NULL;

    if (!button) {
        ADD_FAILURE() << (node ? node->getPluginID() : std::string("null node")) << " has no button \"" << name << "\"";

        return false;
    }
    button->trigger();

    return true;
}

bool
checkGolden(const NodePtr& node,
            const std::string& group,
            const std::string& caseName,
            unsigned mipmapLevel,
            double time)
{
    const RectI window = goldenWindow(mipmapLevel);
    std::list<ImageLayerDesc> layers;

    node->getEffectInstance()->getPresentLayers(time, ViewIdx(0), -1, &layers);
    if (layers.empty()) {
        ADD_FAILURE() << caseName << ": the node presents no plane";

        return false;
    }

    const std::string recordDir = goldenRecordDir();
    const std::string refDir = recordDir.empty() ? goldenReferenceDir() : recordDir;
    std::vector<ParityF32Image> references;
    if (recordDir.empty()) {
        std::vector<std::string> missing;
        for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
            const std::string path = parityReferencePath(refDir, group, caseName, 0, *it);
            if (!QFileInfo::exists(QString::fromStdString(path))) {
                missing.push_back(path);
                continue;
            }
            ParityF32Image image;
            std::string error;
            if (!readParityF32(path, &image, &error)) {
                ADD_FAILURE() << caseName << ": " << error;

                return false;
            }
            if ((image.width != window.width()) || (image.height != window.height()) || (image.nComps != it->getNumComponents())) {
                ADD_FAILURE() << path << " is " << image.width << "x" << image.height << "x" << image.nComps << ", the case renders " << window.width() << "x"
                              << window.height() << "x" << it->getNumComponents();

                return false;
            }
            references.push_back(image);
        }
        if (!missing.empty()) {
            ADD_FAILURE() << caseName << ": no golden reference " << joinStrings(missing) << ". Record the goldens of unchanged nodes by running this test with "
                          << "NATRON_GOLDEN_RECORD_DIR=" << refDir << " and commit the files.";

            return false;
        }
        std::set<std::string> stale = recordedPlanes(refDir + "/" + group, caseName);
        for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
            stale.erase(planeSuffix(*it));
        }
        if (!stale.empty()) {
            ADD_FAILURE() << caseName << ": the node no longer presents the recorded plane(s) "
                          << joinStrings(std::vector<std::string>(stale.begin(), stale.end()));

            return false;
        }
    }

    std::vector<RenderedPlane> planes;
    std::string error;
    if (!renderNodePlanesDirect(node, time, ViewIdx(0), mipmapLevel, window, layers, &planes, &error)) {
        ADD_FAILURE() << caseName << ": render failed: " << error;

        return false;
    }
    if (planes.size() != layers.size()) {
        ADD_FAILURE() << caseName << ": rendered " << planes.size() << " planes of " << layers.size();

        return false;
    }

    if (!recordDir.empty()) {
        bool ok = true;
        if (!writeStamp(recordDir)) {
            ADD_FAILURE() << caseName << ": cannot write " << stampPath(recordDir);
            ok = false;
        }
        for (std::size_t i = 0; i < planes.size(); ++i) {
            ParityF32Image image;
            image.width = planes[i].window.width();
            image.height = planes[i].window.height();
            image.nComps = static_cast<int>(planes[i].channels.size());
            image.pixels = planes[i].pixels;
            const std::string path = parityReferencePath(recordDir, group, caseName, 0, planes[i].layer);
            if (!writeParityF32(path, image, &error)) {
                ADD_FAILURE() << caseName << ": cannot record: " << error;
                ok = false;
            }
        }
        std::cout << "[ golden ] recorded " << group << "/" << caseName << " (" << planes.size() << " plane(s))" << std::endl;

        return ok;
    }

    const bool strict = referencesMatchConfig(refDir);
    bool ok = true;
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const std::string what = caseName + " plane " + planes[i].layer.getLayerID();
        if (static_cast<int>(planes[i].channels.size()) != references[i].nComps) {
            ADD_FAILURE() << what << " has " << planes[i].channels.size() << " channels, the golden reference " << references[i].nComps;
            ok = false;
            continue;
        }
        ok = expectSamePlane(what, references[i], planes[i], strict) && ok;
    }

    return ok;
} // checkGolden

void
runGoldenCases(const AppInstancePtr& app,
               const GoldenNodeSpec& spec,
               const std::vector<GoldenVariant>& variants)
{
    GpuPlacementOffGuard cpuOnly;

    for (std::size_t v = 0; v < variants.size(); ++v) {
        const GoldenVariant& variant = variants[v];
        for (int nonColor = 0; nonColor < 2; ++nonColor) {
            GoldenGraph graph = buildGraph(app, spec, variant);
            if (!graph.node) {
                ADD_FAILURE() << spec.name << " / " << variant.name << ": cannot build the graph";
                continue;
            }
            std::string layerName = "rgba";
            if (nonColor) {
                layerName = selectNonColorLayer(graph, spec);
                if (layerName.empty()) {
                    continue;
                }
            }
            if (variant.apply && !variant.apply(graph)) {
                ADD_FAILURE() << spec.name << " / " << variant.name << ": cannot apply the variant";
                continue;
            }
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                const std::string caseName = spec.name + "_" + variant.name + "_" + layerName + "_" + scaleName(mipmapLevel);
                checkGolden(graph.node, spec.name, caseName, mipmapLevel);
            }
        }
    }
    resetGoldenProject(app);
} // runGoldenCases

NATRON_NAMESPACE_EXIT
