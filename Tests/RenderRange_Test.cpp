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

#include <map>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <SequenceParsing.h>

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

bool
readExrFirstPixelRed(const std::string& path,
                     float* outValue,
                     std::string* error)
{
    FlatExrImage image;

    if (!readFlatExr(path, &image, error)) {
        return false;
    }
    if (image.channelIndex("R") < 0) {
        *error = "no R channel found";

        return false;
    }
    *outValue = image.at(image.x1, image.y1, "R");

    return true;
}

} // namespace

// Renders Constant -> WriteOIIO over a frame range with the Constant's colour keyframed to
// a distinct, exactly-known value per frame, then reads every rendered frame back and checks
// its pixel against the value keyframed for that frame. This catches both a scheduler that
// silently stops dispatching frames (a missing file) and a writer that numbers or overwrites
// frames wrongly (two frames carrying the same value), and it is exact rather than
// approximate because the source image is synthesized at stated values rather than sampled
// from a generator whose output can only be checked approximately.
//
// It does not exercise reader-side frame mapping: there is no reader in this graph.
TEST_F(BaseTest, RenderFrameRangeProducesDistinctFramePixels)
{
    NodePtr generator = createNode(QString::fromUtf8(PLUGINID_OFX_CONSTANT));
    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(generator) && bool(writer));

    connectNodes(generator, writer, 0, true);

    Format f(0, 0, 8, 8, "renderRangeFormat", 1.);
    generator->getApp()->getProject()->setOrAddProjectFormat(f);

    KnobColor* color = dynamic_cast<KnobColor*>(generator->getKnobByName("color").get());
    ASSERT_TRUE(color != NULL);

    const int firstFrame = 1;
    const int lastFrame = 5;
    std::map<int, double> valueForFrame;
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const double value = frame * 0.1;
        valueForFrame[frame] = value;
        for (int dimension = 0; dimension < 3; ++dimension) {
            color->setValueAtTime(frame, value, ViewSpec::all(), dimension);
        }
        color->setValueAtTime(frame, 1., ViewSpec::all(), 3); // alpha=1: keeps premult/unpremult a no-op
    }

    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
    ASSERT_TRUE(bitDepth != NULL);
    bitDepth->setValueFromID("32f", 0);

    KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
    ASSERT_TRUE(compression != NULL);
    compression->setValueFromID("none", 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pattern = (tmp.path() + QLatin1String("/render.####.exr")).toStdString();
    writer->setOutputFilesForWriter(pattern);

    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, firstFrame, lastFrame, 1, false));
    getApp()->startWritersRendering(false, works);

    const std::vector<std::string>& viewNames = getApp()->getProject()->getProjectViewNames();

    std::set<float> observedValues;
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0);
        const QString qPath = QString::fromStdString(path);
        ASSERT_TRUE(QFile::exists(qPath)) << "frame " << frame << " was not rendered: " << path;

        float pixel = 0.f;
        std::string error;
        ASSERT_TRUE(readExrFirstPixelRed(path, &pixel, &error)) << "frame " << frame << ": " << error;

        EXPECT_NEAR(valueForFrame[frame], pixel, 1e-4) << "frame " << frame << " has the wrong pixel value";
        EXPECT_TRUE(observedValues.insert(pixel).second)
            << "frame " << frame << " carries a pixel value (" << pixel << ") already seen on an earlier frame";

        QFile::remove(qPath);
    }
} // TEST_F(BaseTest, RenderFrameRangeProducesDistinctFramePixels)
