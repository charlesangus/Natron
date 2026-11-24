/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "Global/Macros.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QString>

#include "BaseTest.h"

#include "DefocusReference/AnalyticScene.h"
#include "DefocusReference/DefocusMetric.h"
#include "DefocusReference/NiiklCandidate.h"
#include "DefocusReference/Scenes.h"
#include "DefocusReference/ZDefocusCandidate.h"

NATRON_NAMESPACE_USING

using namespace DefocusReference;

namespace {

const std::uint64_t kSeed = 1;
const int kCropSize = 256;

// One candidate configuration: ZDefocus at a layer count, or the scatter engine at its settings.
struct CandidateConfig {
    std::string engine;
    std::string name;
    int layers = 0;
    NiiklSettings niikl;
};

std::vector<CandidateConfig>
candidateConfigs()
{
    std::vector<CandidateConfig> configs;

    for (int layers : { 8, 16, 32 }) {
        CandidateConfig c;
        c.engine = "zdefocus";
        c.name = "layers" + std::to_string(layers);
        c.layers = layers;
        configs.push_back(c);
    }
    for (NiiklEdgeMode edge : { NiiklEdgeMode::Unmix, NiiklEdgeMode::Snap, NiiklEdgeMode::Off }) {
        for (bool fill : { true, false }) {
            for (bool positionNormal : { false, true }) {
                CandidateConfig c;
                c.engine = "niikl";
                c.niikl.edgeMode = edge;
                c.niikl.fill = fill;
                c.niikl.positionNormal = positionNormal;
                c.name = describe(c.niikl);
                configs.push_back(c);
            }
        }
    }
    CandidateConfig fast;
    fast.engine = "niikl";
    fast.niikl.splitOcclusionEdges = false;
    fast.name = describe(fast.niikl);
    configs.push_back(fast);

    return configs;
}

struct HarnessRow {
    std::string scene;
    std::string engine;
    std::string config;
    DefocusScore score;
    double seconds = 0.;
};

class DefocusHarnessTest
    : public BaseTest {
protected:
    /**
     * Scores every candidate configuration on every scene at width x height against a thin-lens
     * truth of `samples` per pixel. With a non-empty `reportDir`, writes each scene's truth and
     * each candidate's result and difference there as crops around the edge band.
     **/
    std::vector<HarnessRow> run(int width,
                                int height,
                                int samples,
                                const std::string& reportDir)
    {
        std::vector<HarnessRow> rows;
        ZDefocusCandidate zdefocus(getApp());
        const std::vector<CandidateConfig> configs = candidateConfigs();
        std::string prepareError;

        // The nodes are made here so that no timed render pays for creating them.
        EXPECT_TRUE(zdefocus.prepare(&prepareError)) << prepareError;

        for (const std::string& name : fixtureSceneNames()) {
            const FixtureScene f = makeFixtureScene(name, width, height);
            const PinholeImage pinhole = f.scene.renderPinhole(f.pinhole);
            const std::vector<float> truth = f.scene.renderThinLens(samples, kSeed);
            const std::vector<unsigned char> band = edgeBand(f.scene.camera, pinhole.depth);
            int cx, cy;
            edgeBandCentre(width, height, band, &cx, &cy);
            std::string error;
            if (!reportDir.empty()) {
                EXPECT_TRUE(writeCrop(reportDir + "/" + name + "-truth.exr", width, height, truth, cx, cy, kCropSize, &error)) << error;
                EXPECT_TRUE(writeCrop(reportDir + "/" + name + "-pinhole.exr", width, height, pinhole.rgba, cx, cy, kCropSize, &error)) << error;
            }
            for (const CandidateConfig& c : configs) {
                std::vector<float> result;
                const auto start = std::chrono::steady_clock::now();
                const bool ok = (c.engine == "zdefocus") ? zdefocus.render(f.scene.camera, pinhole, c.layers, &result, &error)
                                                         : runNiiklDefocus(f.scene.camera, pinhole, c.niikl, &result, &error);
                const auto end = std::chrono::steady_clock::now();
                EXPECT_TRUE(ok) << name << " " << c.engine << " " << c.name << ": " << error;
                if (!ok) {
                    continue;
                }
                HarnessRow row;
                row.scene = name;
                row.engine = c.engine;
                row.config = c.name;
                row.score = scoreDefocus(width, height, truth, result, band);
                row.seconds = std::chrono::duration<double>(end - start).count();
                rows.push_back(row);
                if (!reportDir.empty()) {
                    const std::string stem = reportDir + "/" + name + "-" + c.engine + "-" + c.name;
                    EXPECT_TRUE(writeCrop(stem + ".exr", width, height, result, cx, cy, kCropSize, &error)) << error;
                    EXPECT_TRUE(writeCrop(stem + "-diff.exr", width, height, differenceImage(truth, result), cx, cy, kCropSize, &error)) << error;
                }
            }
        }

        return rows;
    }
};

std::string
formatNumber(double v)
{
    char buffer[32];

    std::snprintf(buffer, sizeof(buffer), "%.5g", v);

    return buffer;
}

void
writeCsv(const std::string& path,
         const std::vector<HarnessRow>& rows)
{
    std::ofstream out(path);

    out << "scene,engine,config,rmse,edge_rmse,halo,hole,edge_pixels,seconds\n";
    for (const HarnessRow& r : rows) {
        out << r.scene << "," << r.engine << "," << r.config << "," << formatNumber(r.score.rmse) << "," << formatNumber(r.score.edgeRmse) << ","
            << formatNumber(r.score.halo) << "," << formatNumber(r.score.hole) << "," << r.score.edgePixels << "," << formatNumber(r.seconds) << "\n";
    }
}

// A table per scene, then the edge RMSE each scatter configuration gains from position and normal.
void
writeMarkdown(const std::string& path,
              const std::vector<HarnessRow>& rows,
              int width,
              int height,
              int samples)
{
    std::ofstream out(path);
    std::map<std::string, double> edgeByKey;

    out << "# Defocus harness results\n\n";
    out << width << "x" << height << ", " << samples << " thin-lens samples per pixel, seed " << kSeed << ".\n";
    for (const std::string& scene : fixtureSceneNames()) {
        out << "\n## " << scene << "\n\n";
        out << "| Engine | Config | RMSE | Edge RMSE | Halo | Hole | Seconds |\n";
        out << "|---|---|---|---|---|---|---|\n";
        for (const HarnessRow& r : rows) {
            if (r.scene != scene) {
                continue;
            }
            out << "| " << r.engine << " | " << r.config << " | " << formatNumber(r.score.rmse) << " | " << formatNumber(r.score.edgeRmse) << " | "
                << formatNumber(r.score.halo) << " | " << formatNumber(r.score.hole) << " | " << formatNumber(r.seconds) << " |\n";
            edgeByKey[r.scene + "/" + r.config] = r.score.edgeRmse;
        }
    }
    out << "\n## Position and normal\n\n";
    out << "Edge RMSE with position and normal minus without; negative means they help.\n\n";
    out << "| Scene | Config | Without | With | Difference |\n";
    out << "|---|---|---|---|---|\n";
    for (const std::string& scene : fixtureSceneNames()) {
        for (const CandidateConfig& c : candidateConfigs()) {
            if ((c.engine != "niikl") || !c.niikl.positionNormal) {
                continue;
            }
            NiiklSettings without = c.niikl;
            without.positionNormal = false;
            const auto a = edgeByKey.find(scene + "/" + describe(without));
            const auto b = edgeByKey.find(scene + "/" + c.name);
            if ((a == edgeByKey.end()) || (b == edgeByKey.end())) {
                continue;
            }
            out << "| " << scene << " | " << describe(without) << " | " << formatNumber(a->second) << " | " << formatNumber(b->second) << " | "
                << formatNumber(b->second - a->second) << " |\n";
        }
    }
}

void
expectFinite(const std::vector<HarnessRow>& rows)
{
    for (const HarnessRow& r : rows) {
        const std::string what = r.scene + " " + r.engine + " " + r.config;
        EXPECT_TRUE(std::isfinite(r.score.rmse)) << what;
        EXPECT_TRUE(std::isfinite(r.score.edgeRmse)) << what;
        EXPECT_TRUE(std::isfinite(r.score.halo)) << what;
        EXPECT_TRUE(std::isfinite(r.score.hole)) << what;
        EXPECT_GT(r.score.edgePixels, 0) << what;
    }
}

} // namespace

// Every scene through every candidate configuration, small and noisy, gives finite scores.
TEST_F(DefocusHarnessTest, Smoke)
{
    const std::vector<HarnessRow> rows = run(64, 36, 16, std::string());

    EXPECT_EQ(fixtureSceneNames().size() * candidateConfigs().size(), rows.size());
    expectFinite(rows);
}

// With NATRON_DEFOCUS_HARNESS_REPORT set to a directory, the full-size run, its CSV, its tables
// and its crops; otherwise nothing.
TEST_F(DefocusHarnessTest, FullReport)
{
    const char* dir = std::getenv("NATRON_DEFOCUS_HARNESS_REPORT");

    if (!dir || !*dir) {
        return;
    }
    const std::string reportDir(dir);
    ASSERT_TRUE(QDir().mkpath(QString::fromUtf8(dir))) << reportDir;
    const int width = 512;
    const int height = 288;
    const int samples = 256;
    const std::vector<HarnessRow> rows = run(width, height, samples, reportDir);

    expectFinite(rows);
    writeCsv(reportDir + "/results.csv", rows);
    writeMarkdown(reportDir + "/results.md", rows, width, height, samples);
}
