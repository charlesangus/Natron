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

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <list>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/LogEntry.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kCGURI = "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kCustomConfigID = "Custom config";
const char* const kSRGBEncoded = "sRGB Encoded Rec.709 (sRGB)";

bool
contains(const std::vector<std::string>& v,
         const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}
}

TEST(ProjectColorManagement, StudioURIResolvesRolesAndDefaults)
{
    ProjectColorManagement cm;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cm.load(kStudioURI, std::string(), &error)) << error;
    EXPECT_EQ(std::string(kStudioURI), cm.getConfigSource());
    EXPECT_TRUE(cm.getConfigDirectory().empty());
    EXPECT_EQ("ACEScg", cm.resolveRoleOrName("scene_linear"));
    EXPECT_EQ("ACEScct", cm.resolveRoleOrName("compositing_log"));
    EXPECT_EQ("ACEScg", cm.resolveRoleOrName("ACEScg"));
    EXPECT_TRUE(cm.resolveRoleOrName("no such colourspace").empty());
    EXPECT_EQ("sRGB - Display", cm.getDefaultDisplay());
    EXPECT_EQ("ACES 2.0 - SDR 100 nits (Rec.709)", cm.getDefaultView(cm.getDefaultDisplay()));
    EXPECT_TRUE(contains(cm.getColorSpaces(), "ACEScg"));
    EXPECT_TRUE(contains(cm.getRoles(), "scene_linear"));
    EXPECT_TRUE(contains(cm.getDisplays(), "sRGB - Display"));
    EXPECT_TRUE(contains(cm.getViews("sRGB - Display"), "ACES 2.0 - SDR 100 nits (Rec.709)"));
    EXPECT_FALSE(cm.getLooks().empty());
}

TEST(ProjectColorManagement, CustomConfigResolvesByAbsoluteAndRelativePath)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString file = QDir(dir.path()).filePath(QString::fromUtf8("custom.ocio"));
    {
        std::ofstream out(file.toStdString());
        OCIO_NAMESPACE::Config::CreateRaw()->serialize(out);
    }

    ProjectColorManagement absolute;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, absolute.load(file.toStdString(), std::string(), &error)) << error;
    EXPECT_EQ(QDir(dir.path()).absolutePath().toStdString(), absolute.getConfigDirectory());
    EXPECT_TRUE(contains(absolute.getColorSpaces(), "raw"));

    ProjectColorManagement relative;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, relative.load("custom.ocio", dir.path().toStdString(), &error)) << error;
    EXPECT_EQ(absolute.getConfigSource(), relative.getConfigSource());
}

TEST(ProjectColorManagement, BadPathReturnsAnErrorAndKeepsThePreviousConfig)
{
    ProjectColorManagement cm;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cm.load(kStudioURI, std::string(), &error)) << error;

    error.clear();
    EXPECT_EQ(ProjectColorManagement::eLoadErrorNoSuchFile, cm.load("/no/such/dir/config.ocio", std::string(), &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(std::string(kStudioURI), cm.getConfigSource());

    error.clear();
    EXPECT_NE(ProjectColorManagement::eLoadErrorNone, cm.load("ocio://no-such-builtin-config", std::string(), &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(std::string(kStudioURI), cm.getConfigSource());
}

TEST(ProjectColorManagement, DisplayProcessorMatchesADirectlyBuiltOCIOProcessor)
{
    ProjectColorManagement cm;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cm.load(kStudioURI, std::string(), &error)) << error;
    const std::string display = cm.getDefaultDisplay();
    const std::string view = cm.getDefaultView(display);

    ProjectColorManagement::DisplayProcessorPtr dp = cm.getDisplayProcessor("ACEScg", display, view, std::string(), &error);
    ASSERT_TRUE(dp != nullptr) << error;
    ASSERT_TRUE(dp->cpu != nullptr);
    EXPECT_FALSE(dp->cacheID.empty());

    OCIO_NAMESPACE::DisplayViewTransformRcPtr transform = OCIO_NAMESPACE::DisplayViewTransform::Create();
    transform->setSrc("ACEScg");
    transform->setDisplay(display.c_str());
    transform->setView(view.c_str());
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr direct = cm.getConfig()->getProcessor(transform, OCIO_NAMESPACE::TRANSFORM_DIR_FORWARD)->getOptimizedCPUProcessor(OCIO_NAMESPACE::BIT_DEPTH_F32, OCIO_NAMESPACE::BIT_DEPTH_F32, OCIO_NAMESPACE::OPTIMIZATION_DEFAULT);

    float expected[3] = { 0.18f, 0.18f, 0.18f };
    float actual[3] = { 0.18f, 0.18f, 0.18f };
    direct->applyRGB(expected);
    dp->cpu->applyRGB(actual);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(expected[c], actual[c], 1e-5);
    }
}

TEST(ProjectColorManagement, EqualLookupsShareACacheID)
{
    ProjectColorManagement cm;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cm.load(kStudioURI, std::string(), &error)) << error;
    const std::string display = cm.getDefaultDisplay();
    const std::string view = cm.getDefaultView(display);

    ProjectColorManagement::DisplayProcessorPtr a = cm.getDisplayProcessor("ACEScg", display, view, std::string(), &error);
    ProjectColorManagement::DisplayProcessorPtr b = cm.getDisplayProcessor("ACEScg", display, view, std::string(), &error);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->cacheID, b->cacheID);
    EXPECT_EQ(a->cacheHash, b->cacheHash);

    ProjectColorManagement::DisplayProcessorPtr other = cm.getDisplayProcessor("ACEScct", display, view, std::string(), &error);
    ASSERT_TRUE(other != nullptr);
    EXPECT_NE(a->cacheID, other->cacheID);
}

TEST(ProjectColorManagement, ColorPickingRoundTripsThroughTheWorkingSpace)
{
    ProjectColorManagement cm;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cm.load(kStudioURI, std::string(), &error)) << error;
    cm.setWorkingSpace("ACEScg");

    float r = 0.18f, g = 0.18f, b = 0.18f;
    cm.workingToColorPicking(&r, &g, &b);
    EXPECT_NEAR(0.4613f, r, 1e-3);
    EXPECT_NEAR(0.4613f, g, 1e-3);
    EXPECT_NEAR(0.4613f, b, 1e-3);

    cm.colorPickingToWorking(&r, &g, &b);
    EXPECT_NEAR(0.18f, r, 1e-4);
    EXPECT_NEAR(0.18f, g, 1e-4);
    EXPECT_NEAR(0.18f, b, 1e-4);
}

TEST(ProjectColorManagement, BuiltinConfigOptionsListTheRegistryPlusCustom)
{
    const std::vector<ChoiceOption> options = ProjectColorManagement::builtinConfigOptions();
    ASSERT_EQ(9u, options.size());
    EXPECT_EQ("Custom config", options.back().id);
    int recommended = 0;
    for (std::size_t i = 0; i + 1 < options.size(); ++i) {
        EXPECT_EQ(0u, options[i].id.find("ocio://"));
        if (options[i].label.find("(recommended)") != std::string::npos) {
            ++recommended;
        }
    }
    EXPECT_GE(recommended, 1);
}

TEST(ProjectColorManagement, ConfigChangedCallbacksFireUntilRemoved)
{
    ProjectColorManagement cm;
    int calls = 0;
    const int id = cm.addConfigChangedCallback([&calls]() { ++calls; });
    cm.notifyConfigChanged();
    EXPECT_EQ(1, calls);
    cm.removeConfigChangedCallback(id);
    cm.notifyConfigChanged();
    EXPECT_EQ(1, calls);
}

namespace {
ProjectPtr
project()
{
    return appPTR->getTopLevelInstance()->getProject();
}

KnobChoicePtr
choiceKnob(const char* name)
{
    return project()->getKnobByNameAndType<KnobChoice>(name);
}

KnobFilePtr
configFileKnob()
{
    return project()->getKnobByNameAndType<KnobFile>("ocioConfigFile");
}

std::string
activeID(const char* name)
{
    KnobChoicePtr knob = choiceKnob(name);

    return knob ? knob->getActiveEntry().id : std::string("<missing>");
}

bool
hasEntry(const char* name,
         const std::string& id)
{
    const std::vector<ChoiceOption> entries = choiceKnob(name)->getEntries_mt_safe();

    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == id) {
            return true;
        }
    }

    return false;
}

std::string
firstEntryOtherThan(const char* name,
                    const std::string& id)
{
    const std::vector<ChoiceOption> entries = choiceKnob(name)->getEntries_mt_safe();

    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id != id) {
            return entries[i].id;
        }
    }

    return std::string();
}

std::list<QString>
colorFallbackWarnings()
{
    std::list<LogEntry> log;
    std::list<QString> ret;

    appPTR->getErrorLog_mt_safe(&log);
    for (std::list<LogEntry>::const_iterator it = log.begin(); it != log.end(); ++it) {
        if (it->message.contains(QString::fromUtf8("does not define some of the project's colorspaces"))) {
            ret.push_back(it->message);
        }
    }

    return ret;
}

QString
writeRawConfig(const QTemporaryDir& dir)
{
    const QString file = QDir(dir.path()).filePath(QString::fromUtf8("raw.ocio"));

    {
        std::ofstream out(file.toStdString());
        OCIO_NAMESPACE::Config::CreateRaw()->serialize(out);
    }

    return file;
}
} // namespace

class ProjectColorManagementPageTest
    : public testing::Test {
protected:
    virtual void SetUp()
    {
        project()->reset(false, true);
        appPTR->clearErrorLog_mt_safe();
    }

    virtual void TearDown()
    {
        project()->reset(false, true);
    }

    void saveResetAndLoad(const QTemporaryDir& tmp,
                          const char* fileName)
    {
        const QString dirPath = tmp.path() + QLatin1Char('/');
        QString saved;

        ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8(fileName), &saved));
        ASSERT_TRUE(QFile::exists(saved));
        project()->reset(false, true);
        ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8(fileName)));
    }
};

TEST_F(ProjectColorManagementPageTest, NewProjectUsesStudioACEScgAndTheFileAndViewerDefaults)
{
    ProjectColorManagement reference;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, reference.load(kStudioURI, std::string(), &error)) << error;

    EXPECT_EQ(std::string(kStudioURI), activeID("ocioConfig"));
    EXPECT_EQ(std::string(kStudioURI), project()->getOCIOConfigSource());
    EXPECT_EQ(std::string(kStudioURI), project()->getColorManagement()->getConfigSource());
    EXPECT_FALSE(configFileKnob()->isEnabled(0));

    EXPECT_EQ("ACEScg", project()->getWorkingColorSpace());
    EXPECT_EQ(kSRGBEncoded, project()->getFileColorSpace(eFileColorCategory8Bit));
    EXPECT_EQ(kSRGBEncoded, project()->getFileColorSpace(eFileColorCategory16Bit));
    EXPECT_EQ("ACEScct", project()->getFileColorSpace(eFileColorCategoryLog));
    EXPECT_EQ("ACEScg", project()->getFileColorSpace(eFileColorCategoryFloat));

    std::string display, view;
    project()->getDefaultDisplayView(&display, &view);
    EXPECT_EQ(reference.getDefaultDisplay(), display);
    EXPECT_EQ(reference.getDefaultView(display), view);
    EXPECT_EQ("sRGB - Display", display);
    EXPECT_EQ("ACES 2.0 - SDR 100 nits (Rec.709)", view);
}

TEST_F(ProjectColorManagementPageTest, EveryColorKnobRoundTripsThroughSaveAndLoad)
{
    choiceKnob("ocioConfig")->setValueFromID(kCGURI, 0);
    ASSERT_EQ(std::string(kCGURI), project()->getColorManagement()->getConfigSource());
    configFileKnob()->setValue("/no/such/dir/unused.ocio");

    const char* const colorKnobs[] = { "workingSpace", "colorSpace8Bit", "colorSpace16Bit", "colorSpaceLog", "colorSpaceFloat", "viewerDisplay" };
    std::vector<std::string> expected;
    for (std::size_t i = 0; i < sizeof(colorKnobs) / sizeof(colorKnobs[0]); ++i) {
        const std::string other = firstEntryOtherThan(colorKnobs[i], activeID(colorKnobs[i]));
        ASSERT_FALSE(other.empty()) << colorKnobs[i];
        choiceKnob(colorKnobs[i])->setValueFromID(other, 0);
        ASSERT_EQ(other, activeID(colorKnobs[i])) << colorKnobs[i];
        expected.push_back(other);
    }
    const std::string view = firstEntryOtherThan("viewerView", activeID("viewerView"));
    if (!view.empty()) {
        choiceKnob("viewerView")->setValueFromID(view, 0);
    }
    const std::string expectedView = activeID("viewerView");

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    saveResetAndLoad(tmp, "color-roundtrip.ntp");

    EXPECT_EQ(std::string(kCGURI), activeID("ocioConfig"));
    EXPECT_EQ(std::string(kCGURI), project()->getColorManagement()->getConfigSource());
    EXPECT_EQ("/no/such/dir/unused.ocio", configFileKnob()->getValue());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], activeID(colorKnobs[i])) << colorKnobs[i];
    }
    EXPECT_EQ(expectedView, activeID("viewerView"));
    EXPECT_EQ(expected[0], project()->getWorkingColorSpace());
}

TEST_F(ProjectColorManagementPageTest, SwitchingToCGRepopulatesKeepsSharedNamesAndFallsBackOnceForStudioOnlyNames)
{
    ProjectColorManagement studio, cg;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, studio.load(kStudioURI, std::string(), &error)) << error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cg.load(kCGURI, std::string(), &error)) << error;

    std::string studioOnly;
    const std::vector<std::string> studioSpaces = studio.getColorSpaces();
    const std::vector<std::string> cgSpaces = cg.getColorSpaces();
    for (std::size_t i = 0; i < studioSpaces.size() && studioOnly.empty(); ++i) {
        if (!contains(cgSpaces, studioSpaces[i])) {
            studioOnly = studioSpaces[i];
        }
    }
    ASSERT_FALSE(studioOnly.empty());

    choiceKnob("colorSpace8Bit")->setValueFromID(studioOnly, 0);
    ASSERT_EQ(studioOnly, activeID("colorSpace8Bit"));
    ASSERT_TRUE(colorFallbackWarnings().empty());

    choiceKnob("ocioConfig")->setValueFromID(kCGURI, 0);

    EXPECT_EQ(std::string(kCGURI), project()->getColorManagement()->getConfigSource());
    EXPECT_FALSE(hasEntry("colorSpace8Bit", studioOnly));
    EXPECT_EQ(cgSpaces.size(), choiceKnob("workingSpace")->getEntries_mt_safe().size());
    EXPECT_EQ("ACEScg", project()->getWorkingColorSpace());
    const std::string resolved = cg.resolveRoleOrName("texture_paint");
    ASSERT_FALSE(resolved.empty());
    EXPECT_EQ(resolved, project()->getFileColorSpace(eFileColorCategory8Bit));
    EXPECT_EQ(kSRGBEncoded, project()->getFileColorSpace(eFileColorCategory16Bit));

    const std::list<QString> warnings = colorFallbackWarnings();
    ASSERT_EQ(1u, warnings.size());
    EXPECT_TRUE(warnings.front().contains(QString::fromUtf8(studioOnly.c_str()))) << warnings.front().toStdString();
    EXPECT_TRUE(warnings.front().contains(QString::fromUtf8(resolved.c_str()))) << warnings.front().toStdString();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    saveResetAndLoad(tmp, "color-switch.ntp");

    EXPECT_EQ(std::string(kCGURI), project()->getOCIOConfigSource());
    EXPECT_EQ(resolved, project()->getFileColorSpace(eFileColorCategory8Bit));
    EXPECT_EQ("ACEScg", project()->getWorkingColorSpace());
}

TEST_F(ProjectColorManagementPageTest, CustomConfigFileListsItsColorSpaces)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString file = writeRawConfig(tmp);

    configFileKnob()->setValue(file.toStdString());
    choiceKnob("ocioConfig")->setValueFromID(kCustomConfigID, 0);

    EXPECT_TRUE(configFileKnob()->isEnabled(0));
    EXPECT_EQ(QFileInfo(file).canonicalFilePath().toStdString(), project()->getOCIOConfigSource());
    EXPECT_EQ(project()->getOCIOConfigSource(), project()->getColorManagement()->getConfigSource());
    EXPECT_TRUE(hasEntry("workingSpace", "raw"));
    EXPECT_TRUE(hasEntry("colorSpaceFloat", "raw"));
    EXPECT_EQ("raw", project()->getWorkingColorSpace());
}

TEST_F(ProjectColorManagementPageTest, ConfigFileEnabledStateIsRederivedAfterLoadWhateverWasSerialized)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString file = writeRawConfig(tmp);
    QString saved;

    configFileKnob()->setValue(file.toStdString());
    choiceKnob("ocioConfig")->setValueFromID(kCustomConfigID, 0);
    ASSERT_TRUE(configFileKnob()->isEnabled(0));
    configFileKnob()->setAllDimensionsEnabled(false);
    ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8("custom.ntp"), &saved));

    choiceKnob("ocioConfig")->setValueFromID(kStudioURI, 0);
    ASSERT_FALSE(configFileKnob()->isEnabled(0));
    configFileKnob()->setAllDimensionsEnabled(true);
    ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8("builtin.ntp"), &saved));

    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8("builtin.ntp")));
    EXPECT_EQ(std::string(kStudioURI), project()->getOCIOConfigSource());
    EXPECT_EQ(file.toStdString(), configFileKnob()->getValue());
    EXPECT_FALSE(configFileKnob()->isEnabled(0));
    EXPECT_TRUE(choiceKnob("ocioConfig")->isEnabled(0));

    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8("custom.ntp")));
    EXPECT_EQ(std::string(kCustomConfigID), activeID("ocioConfig"));
    EXPECT_TRUE(configFileKnob()->isEnabled(0));
    EXPECT_TRUE(hasEntry("workingSpace", "raw"));
}
