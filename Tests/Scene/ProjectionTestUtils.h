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

#ifndef NATRON_TESTS_SCENE_PROJECTIONTESTUTILS_H
#define NATRON_TESTS_SCENE_PROJECTIONTESTUTILS_H

// The USDProject3D scenes and checks the delegate suites share.

#include <algorithm>
#include <cmath>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <pxr/usd/usdGeom/basisCurves.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDEmissiveMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "RenderSettingsTestUtils.h"
#include "Tests/Native/NativeGolden.h"
#include "ofxNatron.h"

NATRON_NAMESPACE_ENTER

namespace ProjectionTestUtils {
// The delegate scene: a 16 x 8 card at the origin seen by /Cam from (0, 0, 10) through a lens
// whose frame is as wide as twice the distance, so the card's plane shows 20 x 10 units over the
// kProjectionWidth x kProjectionHeight format. A checker plate projects onto it from /Projector.
const int kProjectionWidth = 128;
const int kProjectionHeight = 64;
const double kProjectionBox = 16.;
const double kProjectionCameraZ = 10.;
const double kProjectionPlaneWidth = 20.;
const double kProjectionCardWidth = 16.;
const double kProjectionCardHeight = 8.;
const double kProjectionUnitsPerPixel = kProjectionPlaneWidth / kProjectionWidth;
// A delegate's pixel filter spreads a checker edge over about two render pixels.
const double kProjectionEdgeMargin = 2.5;
const char* const kProjectionGoldenGroup = "USDRender";
// Room for a host's arithmetic differing in its last bits; a lost face, a wrong colour transform
// or a shifted plate is far past these.
const float kProjectionChannelTolerance = 0.08f;
const double kProjectionDifferingFraction = 0.01;
const double kProjectionMeanTolerance = 0.005;

// The card's interior, a render pixel or more from its edges.
const RectI kProjectionCardInterior(16, 9, 112, 55);

// The render format, the plate's checker squares and the projector's distance; the card and the
// render camera are the same in every scene.
struct ProjectionScene {
    int width;
    int height;
    double box;
    double projectorZ;
};

inline ProjectionScene
delegateScene()
{
    return ProjectionScene { kProjectionWidth, kProjectionHeight, kProjectionBox, kProjectionCameraZ };
}

// The scene Storm's references were recorded from: 200 x 100, 25 pixel squares, the projector at
// half the render camera's distance.
inline ProjectionScene
stormScene()
{
    return ProjectionScene { 200, 100, 25., kProjectionCameraZ / 2. };
}

// Against Storm's references, on pixels away from the card's and the checker's edges: what the
// two renderers may differ by where both see the plate's flat colour.
const double kStormMeanTolerance = 0.0002;
const double kStormP99Tolerance = 0.0005;
const float kStormOverThreshold = 0.01f;
const double kStormOverFraction = 0.0003;
const float kStormMaxToleranceCycles = 0.01f;
const float kStormMaxToleranceMoonRay = 0.04f;
// Plate pixels each side of a checker edge left out of that comparison.
const double kStormEdgeMargin = 5.;

enum ProjectedEnum {
    eProjectedNone = 0,
    eProjectedRed,
    eProjectedBlue,
    eProjectedTransparent,
    eProjectedBlack
};

struct ProjectionCheck {
    int sure = 0;
    int wrong = 0;
    int red = 0;
    int blue = 0;
};

inline ProjectedEnum
classify(const RenderedPlane& plane,
         int x,
         int y)
{
    const std::size_t i = ((std::size_t)y * (std::size_t)plane.window.width() + (std::size_t)x) * 4;
    const float r = plane.pixels[i], g = plane.pixels[i + 1], b = plane.pixels[i + 2], a = plane.pixels[i + 3];
    if ((a < 0.05f) && (std::max(r, std::max(g, b)) < 0.05f)) {
        return eProjectedTransparent;
    }
    if (a < 0.9f) {
        return eProjectedNone;
    }
    if (std::max(r, std::max(g, b)) < 0.05f) {
        return eProjectedBlack;
    }
    if ((r > 0.7f) && (b < 0.25f) && (g < 0.25f)) {
        return eProjectedRed;
    }
    if ((b > 0.7f) && (r < 0.25f) && (g < 0.25f)) {
        return eProjectedBlue;
    }

    return eProjectedNone;
}

/**
 * Compares every card pixel away from an edge with the projection of the plate from a camera on
 * the z axis whose frame spans frameWidth units of the card's plane, offset by projectorX. The
 * card sits cardShift along x; referenceShift is how far it moved since the projection was held.
 * mirrored is for a card flipped along x after the projection was fixed on it, which shows the
 * plate the projector put at -x.
 **/
inline ProjectionCheck
checkProjection(const RenderedPlane& plane,
                double frameWidth,
                double projectorX,
                double cardShift = 0.,
                double referenceShift = 0.,
                bool mirrored = false)
{
    ProjectionCheck check;
    const double platePerUnit = kProjectionWidth / frameWidth;
    const double margin = kProjectionEdgeMargin * kProjectionUnitsPerPixel * platePerUnit;
    const double cardMargin = kProjectionEdgeMargin * kProjectionUnitsPerPixel;
    for (int y = 0; y < kProjectionHeight; ++y) {
        for (int x = 0; x < kProjectionWidth; ++x) {
            const double X = (x + 0.5 - kProjectionWidth / 2.) * kProjectionUnitsPerPixel;
            const double Y = (y + 0.5 - kProjectionHeight / 2.) * kProjectionUnitsPerPixel;
            if ((std::fabs(X - cardShift) > kProjectionCardWidth / 2. - cardMargin) || (std::fabs(Y) > kProjectionCardHeight / 2. - cardMargin)) {
                continue;
            }
            const double held = X - referenceShift;
            const double u = kProjectionWidth / 2. + ((mirrored ? -held : held) - projectorX) * platePerUnit;
            const double v = kProjectionHeight / 2. + Y * platePerUnit;
            ProjectedEnum expected = eProjectedNone;
            if ((u < -margin) || (u > kProjectionWidth + margin) || (v < -margin) || (v > kProjectionHeight + margin)) {
                expected = eProjectedTransparent;
            } else if ((u > margin) && (u < kProjectionWidth - margin) && (v > margin) && (v < kProjectionHeight - margin)) {
                const double bu = (u - kProjectionWidth / 2.) / kProjectionBox;
                const double bv = (v - kProjectionHeight / 2.) / kProjectionBox;
                const double edge = margin / kProjectionBox;
                if ((std::fabs(bu - std::round(bu)) >= edge) && (std::fabs(bv - std::round(bv)) >= edge)) {
                    const long sum = (long)std::floor(bu) + (long)std::floor(bv);
                    expected = ((sum % 2) == 0) ? eProjectedRed : eProjectedBlue;
                }
            }
            if (expected == eProjectedNone) {
                continue;
            }
            const ProjectedEnum seen = classify(plane, x, y);
            ++check.sure;
            check.wrong += (seen != expected) ? 1 : 0;
            check.red += (seen == eProjectedRed) ? 1 : 0;
            check.blue += (seen == eProjectedBlue) ? 1 : 0;
        }
    }

    return check;
}

// How many pixels of the region are of the given class.
inline int
countIn(const RenderedPlane& plane,
        const RectI& region,
        ProjectedEnum what)
{
    int count = 0;
    for (int y = region.y1; y < region.y2; ++y) {
        for (int x = region.x1; x < region.x2; ++x) {
            count += (classify(plane, x, y) == what) ? 1 : 0;
        }
    }

    return count;
}

// The mean RGBA of the region.
inline std::vector<double>
meanIn(const RenderedPlane& plane,
       const RectI& region)
{
    std::vector<double> mean(4, 0.);
    for (int y = region.y1; y < region.y2; ++y) {
        for (int x = region.x1; x < region.x2; ++x) {
            const std::size_t i = ((std::size_t)y * (std::size_t)plane.window.width() + (std::size_t)x) * 4;
            for (int k = 0; k < 4; ++k) {
                mean[k] += plane.pixels[i + k] / (double)region.area();
            }
        }
    }

    return mean;
}

// The crop's RGBA, rows bottom first as in the plane.
inline std::vector<float>
cropPixels(const RenderedPlane& rgba,
           const RectI& crop)
{
    std::vector<float> pixels;
    pixels.reserve((std::size_t)crop.width() * (std::size_t)crop.height() * 4);
    for (int y = crop.y1; y < crop.y2; ++y) {
        const std::size_t row = (std::size_t)(y - rgba.window.y1) * (std::size_t)rgba.window.width();
        for (int x = crop.x1; x < crop.x2; ++x) {
            const std::size_t i = (row + (std::size_t)(x - rgba.window.x1)) * 4;
            pixels.insert(pixels.end(), rgba.pixels.begin() + i, rgba.pixels.begin() + i + 4);
        }
    }

    return pixels;
}

// The committed reference goldenCase of the crop's size, or empty after a gtest failure.
inline std::vector<float>
readReference(const char* goldenCase,
              const RectI& crop)
{
    const std::string path = goldenReferenceDir() + "/" + kProjectionGoldenGroup + "/" + goldenCase + ".exr";
    std::unique_ptr<OIIO::ImageInput> in = OIIO::ImageInput::open(path);
    if (!in) {
        ADD_FAILURE() << goldenCase << ": no golden reference " << path << " (" << OIIO::geterror() << "). Record it in the published build image "
                      << "by running this test with NATRON_GOLDEN_RECORD_DIR set and commit the file.";

        return std::vector<float>();
    }
    const OIIO::ImageSpec& spec = in->spec();
    if ((spec.width != crop.width()) || (spec.height != crop.height()) || (spec.nchannels != 4)) {
        ADD_FAILURE() << path << " is " << spec.width << " x " << spec.height << " x " << spec.nchannels << ", not " << crop.width() << " x " << crop.height() << " RGBA";

        return std::vector<float>();
    }
    std::vector<float> reference((std::size_t)crop.width() * (std::size_t)crop.height() * 4);
    if (!in->read_image(0, 0, 0, 4, OIIO::TypeDesc::FLOAT, reference.data())) {
        ADD_FAILURE() << in->geterror();

        return std::vector<float>();
    }
    in->close();

    return reference;
}

// Records the crop as a half-float, zip compressed EXR under NATRON_GOLDEN_RECORD_DIR, or compares
// it with the committed one within the golden tolerances.
inline void
matchGoldenCrop(const RenderedPlane& rgba,
                const RectI& crop,
                const char* goldenCase)
{
    ASSERT_EQ(4u, rgba.channels.size());
    ASSERT_LE(crop.width(), 256);
    ASSERT_LE(crop.height(), 256);
    const std::vector<float> pixels = cropPixels(rgba, crop);

    const std::string recordDir = goldenRecordDir();
    if (!recordDir.empty()) {
        const std::string name = std::string(kProjectionGoldenGroup) + "/" + goldenCase + ".exr";
        const QString path = QDir(QString::fromStdString(recordDir)).filePath(QString::fromStdString(name));
        ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath())) << path.toStdString();
        std::unique_ptr<OIIO::ImageOutput> out = OIIO::ImageOutput::create(path.toStdString());
        ASSERT_TRUE(out != nullptr) << OIIO::geterror();
        OIIO::ImageSpec spec(crop.width(), crop.height(), 4, OIIO::TypeDesc::HALF);
        spec.attribute("compression", "zip");
        ASSERT_TRUE(out->open(path.toStdString(), spec)) << out->geterror();
        EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
        EXPECT_TRUE(out->close()) << out->geterror();
        std::cout << "[ golden ] recorded " << path.toStdString() << std::endl;

        return;
    }

    const std::vector<float> reference = readReference(goldenCase, crop);
    if (reference.empty()) {
        return;
    }
    std::size_t differing = 0;
    double sum = 0.;
    float worst = 0.f;
    for (std::size_t p = 0; p < pixels.size() / 4; ++p) {
        bool differs = false;
        for (int k = 0; k < 4; ++k) {
            const float d = std::fabs(pixels[p * 4 + k] - reference[p * 4 + k]);
            sum += d;
            worst = std::max(worst, d);
            differs = differs || !(d <= kProjectionChannelTolerance);
        }
        differing += differs ? 1 : 0;
    }
    EXPECT_LE((double)differing, kProjectionDifferingFraction * (double)(pixels.size() / 4)) << goldenCase << ": worst difference " << worst;
    EXPECT_LE(sum / (double)pixels.size(), kProjectionMeanTolerance) << goldenCase << ": worst difference " << worst;
} // matchGoldenCrop

/**
 * Compares the crop of a render of stormScene(), the card cardShift along x and the plate with it,
 * with Storm's reference stormCase on the pixels away from the card's edges and the checker's: the
 * delegates and Storm must agree on the plate wherever its colour is flat.
 **/
inline void
matchStormReference(const RenderedPlane& rgba,
                    const RectI& crop,
                    const char* stormCase,
                    double cardShift,
                    float maxTolerance)
{
    const ProjectionScene scene = stormScene();
    ASSERT_EQ(4u, rgba.channels.size());
    ASSERT_EQ(scene.width, rgba.window.width());
    const std::vector<float> reference = readReference(stormCase, crop);
    if (reference.empty()) {
        return;
    }
    const std::vector<float> pixels = cropPixels(rgba, crop);
    const double unitsPerPixel = kProjectionPlaneWidth / scene.width;
    const double platePerUnit = scene.width / (2. * scene.projectorZ);
    const double cardMargin = kProjectionEdgeMargin * unitsPerPixel;
    std::vector<float> worstPerPixel;
    double sum = 0.;
    std::size_t channels = 0;
    for (std::size_t p = 0; p < pixels.size() / 4; ++p) {
        const int x = crop.x1 + (int)(p % (std::size_t)crop.width());
        const int y = crop.y1 + (int)(p / (std::size_t)crop.width());
        const double X = (x + 0.5 - scene.width / 2.) * unitsPerPixel - cardShift;
        const double Y = (y + 0.5 - scene.height / 2.) * unitsPerPixel;
        if ((std::fabs(X) > kProjectionCardWidth / 2. - cardMargin) || (std::fabs(Y) > kProjectionCardHeight / 2. - cardMargin)) {
            continue;
        }
        // The format holds whole squares each side of its centre, so its edges are among these.
        const double bu = X * platePerUnit / scene.box;
        const double bv = Y * platePerUnit / scene.box;
        if (std::min(std::fabs(bu - std::round(bu)), std::fabs(bv - std::round(bv))) * scene.box <= kStormEdgeMargin) {
            continue;
        }
        float worst = 0.f;
        for (int k = 0; k < 4; ++k) {
            const float d = std::fabs(pixels[p * 4 + k] - reference[p * 4 + k]);
            sum += d;
            worst = std::max(worst, d);
        }
        channels += 4;
        worstPerPixel.push_back(worst);
    }
    ASSERT_GT(worstPerPixel.size(), 1000u) << stormCase << ": too few interior pixels";
    std::sort(worstPerPixel.begin(), worstPerPixel.end());
    const float p99 = worstPerPixel[(worstPerPixel.size() * 99) / 100];
    const std::size_t over = (std::size_t)(worstPerPixel.end() - std::upper_bound(worstPerPixel.begin(), worstPerPixel.end(), kStormOverThreshold));
    EXPECT_LE(sum / (double)channels, kStormMeanTolerance) << stormCase;
    EXPECT_LE(p99, kStormP99Tolerance) << stormCase;
    EXPECT_LE(worstPerPixel.back(), maxTolerance) << stormCase;
    EXPECT_LE((double)over, kStormOverFraction * (double)worstPerPixel.size()) << stormCase << ": " << over << " of " << worstPerPixel.size() << " pixels over " << kStormOverThreshold;
} // matchStormReference

#define EXPECT_PROJECTS(check)                                                                   \
    do {                                                                                         \
        const NATRON_NAMESPACE::ProjectionTestUtils::ProjectionCheck c_ = (check);               \
        EXPECT_GT(c_.sure, 1000) << "too few card pixels away from an edge";                     \
        EXPECT_LE(c_.wrong, c_.sure / 100) << c_.wrong << " of " << c_.sure << " pixels differ"; \
        EXPECT_GT(c_.red, 0);                                                                    \
        EXPECT_GT(c_.blue, 0);                                                                   \
    } while (0)

/**
 * The projection fixture over a delegate suite's BASE, which provides the static knob<KNOB>() and
 * selectRenderer() helpers; rendererId() names the delegate the graphs render with.
 **/
template <class BASE>
class ProjectionFixture
    : public BASE {
protected:
    virtual const char* rendererId() const = 0;

    virtual void SetUp() OVERRIDE
    {
        BASE::SetUp();
        setFormat(delegateScene());
    }

    virtual void TearDown() OVERRIDE
    {
        LiveTextureStore::instance().clear();
        BASE::TearDown();
    }

    struct Graph {
        NodePtr plate;
        NodePtr projector;
        NodePtr projectorCamera;
        NodePtr card;
        NodePtr merge;
        NodePtr render;
    };

    void setFormat(const ProjectionScene& scene)
    {
        _scene = scene;
        ProjectPtr project = this->getApp()->getProject();
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, scene.width, scene.height, "projection2to1", 1.));
    }

    static bool setString(const NodePtr& node,
                          const char* name,
                          const char* value)
    {
        KnobString* k = BASE::template knob<KnobString>(node, name);
        if (k) {
            k->setValue(std::string(value));
        }

        return k != nullptr;
    }

    static bool setTriple(const NodePtr& node,
                          const char* name,
                          double x,
                          double y,
                          double z)
    {
        KnobDouble* k = BASE::template knob<KnobDouble>(node, name);
        if (k) {
            k->setValue(x, ViewSpec::all(), 0);
            k->setValue(y, ViewSpec::all(), 1);
            k->setValue(z, ViewSpec::all(), 2);
        }

        return k != nullptr;
    }

    static bool setLens(const NodePtr& camera,
                        double horizontal,
                        double vertical)
    {
        KnobDouble* focal = BASE::template knob<KnobDouble>(camera, kUSDCameraParamFocalLength);
        KnobDouble* h = BASE::template knob<KnobDouble>(camera, kUSDCameraParamHorizontalAperture);
        KnobDouble* v = BASE::template knob<KnobDouble>(camera, kUSDCameraParamVerticalAperture);
        if (!focal || !h || !v) {
            return false;
        }
        focal->setValue(18.);
        h->setValue(horizontal);
        v->setValue(vertical);

        return true;
    }

    // The scene, the delegate's by default, with the projector on the render camera's axis; its
    // graph is empty after a gtest failure.
    Graph createProjectionGraph(const ProjectionScene& scene = delegateScene())
    {
        Graph graph;
        setFormat(scene);
        NodePtr checker = this->createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        NodePtr projectorCamera = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr project3D = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
        NodePtr card = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        NodePtr camera = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr merge = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr settings = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        NodePtr render = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!checker || !projectorCamera || !project3D || !card || !camera || !merge || !settings || !render) {
            ADD_FAILURE() << "a node is not registered";

            return graph;
        }

        KnobDouble* boxSize = BASE::template knob<KnobDouble>(checker, kCheckerBoardParamBoxSize);
        KnobDouble* centerlineWidth = BASE::template knob<KnobDouble>(checker, kCheckerBoardParamCenterLineWidth);
        KnobDouble* lineWidth = BASE::template knob<KnobDouble>(checker, kCheckerBoardParamLineWidth);
        KnobChoice* renderer = BASE::template knob<KnobChoice>(render, kUSDRenderParamRenderer);
        if (!boxSize || !centerlineWidth || !lineWidth || !renderer) {
            return graph;
        }
        boxSize->setValue(scene.box, ViewSpec::all(), 0);
        boxSize->setValue(scene.box, ViewSpec::all(), 1);
        centerlineWidth->setValue(0.);
        lineWidth->setValue(0.);
        const char* const colours[4] = { kCheckerBoardParamColor0, kCheckerBoardParamColor1, kCheckerBoardParamColor2, kCheckerBoardParamColor3 };
        for (int c = 0; c < 4; ++c) {
            KnobColor* colour = BASE::template knob<KnobColor>(checker, colours[c]);
            if (!colour) {
                return graph;
            }
            // Colours 0 and 2 fill the squares whose indices sum to an even number.
            const bool red = (c % 2) == 0;
            colour->setValue(red ? 0.9 : 0.05, ViewSpec::all(), 0);
            colour->setValue(0.05, ViewSpec::all(), 1);
            colour->setValue(red ? 0.05 : 0.9, ViewSpec::all(), 2);
            colour->setValue(1., ViewSpec::all(), 3);
        }

        bool ok = setLens(projectorCamera, 36., 18.) && setLens(camera, 36., 18.);
        ok = ok && setString(projectorCamera, kSceneParamPrimPath, "/Projector");
        ok = ok && setTriple(projectorCamera, kSceneParamTranslate, 0., 0., scene.projectorZ);
        ok = ok && setString(card, kSceneParamPrimPath, "/Card");
        ok = ok && setTriple(card, kSceneParamScale, kProjectionCardWidth, kProjectionCardHeight, 1.);
        ok = ok && setString(camera, kSceneParamPrimPath, "/Cam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., kProjectionCameraZ);
        if (!ok) {
            return graph;
        }
        if (!BASE::selectRenderer(renderer, rendererId())) {
            ADD_FAILURE() << "the Renderer menu has no " << rendererId();

            return graph;
        }

        this->connectNodes(checker, project3D, kUSDProject3DInputImage, true);
        this->connectNodes(projectorCamera, project3D, kUSDProject3DInputCamera, true);
        this->connectNodes(project3D, card, 0, true);
        this->connectNodes(card, merge, 0, true);
        this->connectNodes(camera, merge, 1, true);
        this->connectNodes(merge, settings, 0, true);
        this->connectNodes(settings, render, 0, true);
        if (!RenderSettingsTestUtils::configure(settings, "/Cam", scene.width, scene.height)) {
            return graph;
        }
        graph.plate = checker;
        graph.projector = project3D;
        graph.projectorCamera = projectorCamera;
        graph.card = card;
        graph.merge = merge;
        graph.render = render;

        return graph;
    } // createProjectionGraph

    /**
     * Writes to file a /Card of linear curves, not a mesh, covering the card's 16 x 8 units with
     * ribbons 0.35 wide every 0.25, carried shift along x from frame 1 to frame 11.
     **/
    static bool writeCurvesCard(const QString& file,
                                double shift)
    {
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateNew(file.toStdString());
        if (!stage) {
            ADD_FAILURE() << "could not create " << file.toStdString();

            return false;
        }
        PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(stage, PXR_NS::SdfPath("/Card"));
        const int rows = 33;
        PXR_NS::VtVec3fArray points;
        for (int row = 0; row < rows; ++row) {
            const float y = (float)(-kProjectionCardHeight / 2. + row * kProjectionCardHeight / (rows - 1));
            points.push_back(PXR_NS::GfVec3f((float)(-kProjectionCardWidth / 2.), y, 0.f));
            points.push_back(PXR_NS::GfVec3f((float)(kProjectionCardWidth / 2.), y, 0.f));
        }
        curves.CreateTypeAttr().Set(PXR_NS::UsdGeomTokens->linear);
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(rows, 2));
        curves.CreatePointsAttr().Set(points);
        curves.CreateWidthsAttr().Set(PXR_NS::VtFloatArray(1, 0.35f));
        curves.SetWidthsInterpolation(PXR_NS::UsdGeomTokens->constant);
        PXR_NS::UsdGeomXformOp translate = curves.AddTranslateOp();
        translate.Set(PXR_NS::GfVec3d(0.), PXR_NS::UsdTimeCode(1.));
        translate.Set(PXR_NS::GfVec3d(shift, 0., 0.), PXR_NS::UsdTimeCode(11.));

        return stage->GetRootLayer()->Save();
    }

    // Puts the curves of file in place of the card, the projection bound to them by an assign it
    // returns, null after a gtest failure.
    NodePtr replaceCardWithCurves(const Graph& graph,
                                  const QString& file)
    {
        NodePtr reader = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr assign = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
        KnobFile* fileKnob = reader ? BASE::template knob<KnobFile>(reader, kUSDReadParamFile) : nullptr;
        if (!fileKnob || !assign || !setString(assign, kUSDAssignMaterialParamPrimPattern, "/Card")) {
            ADD_FAILURE() << "no USDRead or USDAssignMaterial";

            return NodePtr();
        }
        fileKnob->setValue(file.toStdString());
        graph.card->disconnectInput(0);
        graph.merge->disconnectInput(0);
        this->connectNodes(reader, assign, kUSDAssignMaterialInputScene, true);
        this->connectNodes(graph.projector, assign, kUSDAssignMaterialInputMaterial, true);
        this->connectNodes(assign, graph.merge, 0, true);

        return assign;
    }

    // Off the main thread, which is where the plate's live texture is prepared from.
    bool renderProjection(const NodePtr& render,
                          double frame,
                          RenderedPlane* rgba) const
    {
        bool rendered = false;
        std::string error;
        std::vector<RenderedPlane> planes;
        const int width = _scene.width;
        const int height = _scene.height;

        appPTR->clearAllCaches();
        std::thread worker([&]() {
            rendered = renderNodePlanesDirect(render, frame, ViewIdx(0), 0, RectI(0, 0, width, height),
                                              std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error, false);
        });
        worker.join();
        appPTR->clearAllCaches();
        EXPECT_TRUE(rendered) << error;
        if (!rendered || (planes.size() != 1u) || (planes[0].channels.size() != 4u) || (planes[0].window.width() != width) || (planes[0].window.height() != height)) {
            ADD_FAILURE() << "no " << width << " x " << height << " RGBA plane";

            return false;
        }
        *rgba = planes[0];

        return true;
    }

    static std::string messageOf(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }

    // Linear curves, not a mesh, so no reference points: held at frame 1 in Geometry space, the
    // plate rides on the projection's frame alone as the curves' transform carries them 2 units by
    // frame 11.
    void expectHeldCurvesCarryThePlate()
    {
        double first = 0., last = 0.;
        this->getApp()->getFrameRange(&first, &last);
        ASSERT_LE(first, 1.);
        ASSERT_GE(last, 11.);
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const double shift = 2.;
        const QString file = tmp.path() + QString::fromUtf8("/curves.usda");
        ASSERT_TRUE(writeCurvesCard(file, shift));
        const Graph graph = createProjectionGraph();
        ASSERT_TRUE(bool(graph.render));
        KnobChoice* space = BASE::template knob<KnobChoice>(graph.projector, kUSDProject3DParamProjectionSpace);
        KnobBool* useHold = BASE::template knob<KnobBool>(graph.projector, kUSDProject3DParamUseFrameHold);
        KnobDouble* hold = BASE::template knob<KnobDouble>(graph.projector, kUSDProject3DParamFrameHold);
        ASSERT_TRUE(space && useHold && hold);
        space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
        useHold->setValue(true);
        hold->setValue(1.);
        const NodePtr assign = replaceCardWithCurves(graph, file);
        ASSERT_TRUE(bool(assign));

        RenderedPlane rgba;
        ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
        EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0.));
        ASSERT_TRUE(renderProjection(graph.render, 11., &rgba));
        EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0., shift, shift));
        EXPECT_FALSE(assign->hasPersistentMessage()) << messageOf(assign);
    }

    // Curves have no faces, so whichever way they face the projector they show the plate.
    void expectCurvesShowThePlateOnEitherSide()
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString file = tmp.path() + QString::fromUtf8("/curves.usda");
        ASSERT_TRUE(writeCurvesCard(file, 0.));
        const Graph graph = createProjectionGraph();
        ASSERT_TRUE(bool(graph.render));
        KnobChoice* projectOn = BASE::template knob<KnobChoice>(graph.projector, kUSDProject3DParamProjectOn);
        KnobChoice* outside = BASE::template knob<KnobChoice>(graph.projector, kUSDProject3DParamOutside);
        ASSERT_TRUE(projectOn && outside);
        outside->setValue((int)USDProject3D::eOutsideBlack);
        ASSERT_TRUE(bool(replaceCardWithCurves(graph, file)));

        RenderedPlane rgba;
        for (int side : { (int)USDProject3D::eProjectOnFront, (int)USDProject3D::eProjectOnBack }) {
            SCOPED_TRACE(side);
            projectOn->setValue(side);
            ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
            EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0.));
        }
    }

    // In Geometry space the projection is fixed where the card node places the card, so a
    // transform after it carries the plate along, and a mirror there shows the plate mirrored on
    // the face that looked at the projector, still under Project On Front.
    void expectATransformAfterTheCardCarriesThePlate()
    {
        const Graph graph = createProjectionGraph();
        ASSERT_TRUE(bool(graph.render));
        KnobChoice* space = BASE::template knob<KnobChoice>(graph.projector, kUSDProject3DParamProjectionSpace);
        KnobChoice* projectOn = BASE::template knob<KnobChoice>(graph.projector, kUSDProject3DParamProjectOn);
        KnobChoice* outside = BASE::template knob<KnobChoice>(graph.projector, kUSDProject3DParamOutside);
        NodePtr edit = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
        ASSERT_TRUE(space && projectOn && outside && edit);
        ASSERT_TRUE(setString(edit, kUSDEditTransformParamPrimPattern, "/Card"));
        space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
        graph.merge->disconnectInput(0);
        this->connectNodes(graph.card, edit, 0, true);
        this->connectNodes(edit, graph.merge, 0, true);

        // An appended transform applies inside the card's own scale, so the card moves shift units
        // of the plane when the edit translates it by shift over the card's width.
        const double shift = 2.;
        ASSERT_TRUE(setTriple(edit, kSceneParamTranslate, shift / kProjectionCardWidth, 0., 0.));
        RenderedPlane rgba;
        ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
        EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0., shift, shift));
        EXPECT_FALSE(edit->hasPersistentMessage()) << messageOf(edit);

        ASSERT_TRUE(setTriple(edit, kSceneParamTranslate, 0., 0., 0.));
        ASSERT_TRUE(setTriple(edit, kSceneParamScale, -1., 1., 1.));
        outside->setValue((int)USDProject3D::eOutsideBlack);
        projectOn->setValue((int)USDProject3D::eProjectOnFront);
        ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
        EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0., 0., 0., true));
        projectOn->setValue((int)USDProject3D::eProjectOnBack);
        ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
        EXPECT_EQ(kProjectionCardInterior.area(), countIn(rgba, kProjectionCardInterior, eProjectedBlack));
    }

    // The plate of Storm's World reference from Storm's scene: what Storm shows on the card.
    void expectStormsWorldPlate(float maxTolerance)
    {
        const Graph graph = createProjectionGraph(stormScene());
        ASSERT_TRUE(bool(graph.render));
        RenderedPlane rgba;
        ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
        matchStormReference(rgba, RectI(16, 6, 184, 94), "StormProject3DWorld", 0., maxTolerance);
    }

    // Held at frame 1 in Geometry space, the plate rides 2 units with the card by frame 11 as
    // Storm shows it.
    void expectStormsHeldPlate(float maxTolerance)
    {
        double first = 0., last = 0.;
        this->getApp()->getFrameRange(&first, &last);
        ASSERT_LE(first, 1.);
        ASSERT_GE(last, 11.);
        const Graph graph = createProjectionGraph(stormScene());
        ASSERT_TRUE(bool(graph.render));
        KnobChoice* space = BASE::template knob<KnobChoice>(graph.projector, kUSDProject3DParamProjectionSpace);
        KnobBool* useHold = BASE::template knob<KnobBool>(graph.projector, kUSDProject3DParamUseFrameHold);
        KnobDouble* hold = BASE::template knob<KnobDouble>(graph.projector, kUSDProject3DParamFrameHold);
        KnobDouble* translate = BASE::template knob<KnobDouble>(graph.card, kSceneParamTranslate);
        ASSERT_TRUE(space && useHold && hold && translate);
        space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
        useHold->setValue(true);
        hold->setValue(1.);
        const double shift = 2.;
        translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
        translate->setValueAtTime(11., shift, ViewSpec::all(), 0);
        RenderedPlane rgba;
        ASSERT_TRUE(renderProjection(graph.render, 11., &rgba));
        matchStormReference(rgba, RectI(36, 6, 200, 94), "StormProject3DGeometryHeld", shift, maxTolerance);
    }

    // A plate reaching 50 pixels beyond its format on every side, cropped to it, as Storm shows it.
    void expectStormsOverscanPlate(float maxTolerance)
    {
        const ProjectionScene scene = stormScene();
        const Graph graph = createProjectionGraph(scene);
        ASSERT_TRUE(bool(graph.render));
        KnobChoice* extent = BASE::template knob<KnobChoice>(graph.plate, kNativeGeneratorParamExtent);
        KnobDouble* bottomLeft = BASE::template knob<KnobDouble>(graph.plate, kNativeGeneratorParamBottomLeft);
        KnobDouble* size = BASE::template knob<KnobDouble>(graph.plate, kNativeGeneratorParamSize);
        KnobBool* crop = BASE::template knob<KnobBool>(graph.projector, kUSDProject3DParamCrop);
        ASSERT_TRUE(extent && bottomLeft && size && crop);
        extent->setValue((int)ExtentKnobs::eExtentSize);
        bottomLeft->setValue(-50., ViewSpec::all(), 0);
        bottomLeft->setValue(-25., ViewSpec::all(), 1);
        size->setValue(scene.width + 100., ViewSpec::all(), 0);
        size->setValue(scene.height + 50., ViewSpec::all(), 1);
        crop->setValue(true);
        RenderedPlane rgba;
        ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
        matchStormReference(rgba, RectI(16, 6, 184, 94), "StormProject3DOverscan", 0., maxTolerance);
    }

    // A plate of straight red 0.4 at half alpha over a blue card behind: the plate's opacity is
    // its alpha, and its colour is not darkened by it a second time.
    void expectAHalfAlphaPlateOverTheBackground()
    {
        const Graph graph = createProjectionGraph();
        ASSERT_TRUE(bool(graph.render));
        NodePtr constant = this->createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        NodePtr backMaterial = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDEMISSIVEMATERIAL));
        NodePtr backCard = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        ASSERT_TRUE(constant && backMaterial && backCard);
        KnobColor* colour = BASE::template knob<KnobColor>(constant, kConstantParamColor);
        KnobColor* blue = BASE::template knob<KnobColor>(backMaterial, kUSDEmissiveMaterialParamEmissiveColor);
        ASSERT_TRUE(colour && blue);
        const double premultiplied[4] = { 0.2, 0., 0., 0.5 };
        for (int c = 0; c < 4; ++c) {
            colour->setValue(premultiplied[c], ViewSpec::all(), c);
        }
        blue->setValue(0., ViewSpec::all(), 0);
        blue->setValue(0., ViewSpec::all(), 1);
        blue->setValue(1., ViewSpec::all(), 2);
        ASSERT_TRUE(setString(backMaterial, kSceneParamPrimPath, "/Blue"));
        ASSERT_TRUE(setString(backCard, kSceneParamPrimPath, "/Back"));
        ASSERT_TRUE(setTriple(backCard, kSceneParamTranslate, 0., 0., -1.));
        ASSERT_TRUE(setTriple(backCard, kSceneParamScale, kProjectionCardWidth, kProjectionCardHeight, 1.));
        graph.projector->disconnectInput(kUSDProject3DInputImage);
        this->connectNodes(constant, graph.projector, kUSDProject3DInputImage, true);
        this->connectNodes(backMaterial, backCard, 0, true);
        this->connectNodes(backCard, graph.merge, 2, true);

        RenderedPlane rgba;
        ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
        const RectI centre(kProjectionWidth / 2 - 20, kProjectionHeight / 2 - 10, kProjectionWidth / 2 + 20, kProjectionHeight / 2 + 10);
        const std::vector<double> mean = meanIn(rgba, centre);
        const double expected[4] = { premultiplied[0], 0., 1. - premultiplied[3], 1. };
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(expected[c], mean[c], 0.03) << "channel " << c;
        }
    }

private:
    ProjectionScene _scene = delegateScene();
};
} // namespace ProjectionTestUtils

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_SCENE_PROJECTIONTESTUTILS_H
