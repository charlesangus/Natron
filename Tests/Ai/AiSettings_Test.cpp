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

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

CLANG_DIAG_OFF(deprecated)
#include <QByteArray>
#include <QSettings>
#include <QString>
CLANG_DIAG_ON(deprecated)

#include "Engine/Ai/OrtLoader.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kAiKnobNames[] = { "aiEnabled", "aiDevice", "aiGpuIndex", "aiThreads", "aiOrtLibraryPath" };
const char* const kDeviceEnv = "NATRON_AI_DEVICE";

KnobPagePtr
findSettingsPage(const std::string& label)
{
    const KnobsVec& knobs = appPTR->getCurrentSettings()->getKnobs();
    for (const KnobIPtr& k : knobs) {
        KnobPagePtr page = std::dynamic_pointer_cast<KnobPage>(k);
        if (page && (page->getLabel() == label)) {
            return page;
        }
    }

    return KnobPagePtr();
}

// Puts NATRON_AI_DEVICE, the AI knobs and their saved values back as they were, however the test exits.
class AiSettingsRestorer {
public:
    AiSettingsRestorer()
        : _envWasSet(qEnvironmentVariableIsSet(kDeviceEnv))
        , _env(qgetenv(kDeviceEnv))
        , _saveSettings(appPTR->getCurrentSettings()->getSaveSettings())
    {
        qunsetenv(kDeviceEnv);
    }

    ~AiSettingsRestorer()
    {
        if (_envWasSet) {
            qputenv(kDeviceEnv, _env);
        } else {
            qunsetenv(kDeviceEnv);
        }
        SettingsPtr settings = appPTR->getCurrentSettings();
        {
            QSettings saved(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));
            for (const char* name : kAiKnobNames) {
                saved.remove(QString::fromUtf8(name));
            }
        }
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(false);
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValue(0);
        settings->getKnobByNameAndType<KnobInt>("aiGpuIndex")->setValue(0);
        settings->getKnobByNameAndType<KnobInt>("aiThreads")->setValue(0);
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string());
        settings->setSaveSettings(_saveSettings);
    }

    AiSettingsRestorer(const AiSettingsRestorer&) = delete;
    AiSettingsRestorer& operator=(const AiSettingsRestorer&) = delete;

private:
    bool _envWasSet;
    QByteArray _env;
    bool _saveSettings;
};

} // namespace

TEST(AiSettings, AiPageHoldsTheAiKnobs)
{
    const KnobPagePtr page = findSettingsPage("AI");
    ASSERT_NE(page, nullptr) << "no Preferences page labelled \"AI\"";

    std::vector<std::string> children;
    for (const KnobIPtr& child : page->getChildren()) {
        children.push_back(child->getName());
    }
    const std::vector<std::string> expected { "aiEnabled", "aiDevice", "aiGpuIndex", "aiThreads", "aiOrtLibraryPath", "aiDetectedProviders" };
    EXPECT_EQ(expected, children);

    KnobStringPtr detected = appPTR->getCurrentSettings()->getKnobByNameAndType<KnobString>("aiDetectedProviders");
    ASSERT_NE(detected, nullptr);
    EXPECT_TRUE(detected->isLabel());
    EXPECT_FALSE(detected->getIsPersistent());
}

TEST(AiSettings, DefaultsAreOffAutoAndZero)
{
    AiSettingsRestorer restore;
    SettingsPtr settings = appPTR->getCurrentSettings();

    EXPECT_FALSE(settings->isAiEnabled());
    EXPECT_EQ(eAiDeviceAuto, settings->getAiDevice());
    EXPECT_EQ(0, settings->getAiGpuIndex());
    EXPECT_EQ(0, settings->getAiThreads());
    EXPECT_EQ(std::string(), settings->getAiOrtLibraryPath());

    KnobChoicePtr device = settings->getKnobByNameAndType<KnobChoice>("aiDevice");
    ASSERT_NE(device, nullptr);
    std::vector<std::string> ids;
    for (const ChoiceOption& entry : device->getEntries_mt_safe()) {
        ids.push_back(entry.id);
    }
    const std::vector<std::string> expected { "auto", "cpu", "cuda", "migraphx", "rocm", "openvino" };
    EXPECT_EQ(expected, ids);
}

TEST(AiSettings, DisabledAiLoadsNothingAndSaysSo)
{
    AiSettingsRestorer restore;
    SettingsPtr settings = appPTR->getCurrentSettings();

    ASSERT_FALSE(settings->isAiEnabled());
    const std::shared_ptr<const OrtLoader> ort = OrtLoader::process();
    EXPECT_FALSE(ort->available());
    EXPECT_EQ(std::string(OrtLoader::disabledReason()), ort->unavailableReason());

    settings->refreshAiDetectedProviders();
    EXPECT_EQ(std::string(OrtLoader::disabledReason()), settings->getKnobByNameAndType<KnobString>("aiDetectedProviders")->getValue());
}

TEST(AiSettings, EnvironmentOverridesTheDeviceChoice)
{
    EXPECT_EQ(eAiDeviceCuda, Settings::resolveAiDevice(eAiDeviceAuto, "cuda"));
    EXPECT_EQ(eAiDeviceMigraphx, Settings::resolveAiDevice(eAiDeviceCpu, "MIGraphX"));
    EXPECT_EQ(eAiDeviceAuto, Settings::resolveAiDevice(eAiDeviceRocm, "auto"));
    EXPECT_EQ(eAiDeviceOpenvino, Settings::resolveAiDevice(eAiDeviceAuto, "OPENVINO"));
    EXPECT_EQ(eAiDeviceRocm, Settings::resolveAiDevice(eAiDeviceRocm, nullptr));
    // A value that names no entry leaves the setting in force.
    EXPECT_EQ(eAiDeviceCpu, Settings::resolveAiDevice(eAiDeviceCpu, "tensorrt"));
    EXPECT_EQ(eAiDeviceCpu, Settings::resolveAiDevice(eAiDeviceCpu, ""));

    AiSettingsRestorer restore;
    SettingsPtr settings = appPTR->getCurrentSettings();
    settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValue((int)eAiDeviceCuda);
    EXPECT_EQ(eAiDeviceCuda, settings->getAiDevice());
    qputenv(kDeviceEnv, QByteArray("cpu"));
    EXPECT_EQ(eAiDeviceCpu, settings->getAiDevice());
    qputenv(kDeviceEnv, QByteArray("bogus"));
    EXPECT_EQ(eAiDeviceCuda, settings->getAiDevice());
}

TEST(AiSettings, ValuesSurviveASaveAndRestore)
{
    AiSettingsRestorer restore;
    SettingsPtr settings = appPTR->getCurrentSettings();
    settings->setSaveSettings(true);

    KnobBoolPtr enabled = settings->getKnobByNameAndType<KnobBool>("aiEnabled");
    KnobChoicePtr device = settings->getKnobByNameAndType<KnobChoice>("aiDevice");
    KnobIntPtr gpuIndex = settings->getKnobByNameAndType<KnobInt>("aiGpuIndex");
    KnobIntPtr threads = settings->getKnobByNameAndType<KnobInt>("aiThreads");
    KnobPathPtr libraryPath = settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath");
    ASSERT_NE(enabled, nullptr);
    ASSERT_NE(device, nullptr);
    ASSERT_NE(gpuIndex, nullptr);
    ASSERT_NE(threads, nullptr);
    ASSERT_NE(libraryPath, nullptr);
    const std::vector<KnobI*> knobs { enabled.get(), device.get(), gpuIndex.get(), threads.get(), libraryPath.get() };

    // A library path that does not exist, so enabling AI here loads nothing.
    const std::string bogus = "/nonexistent/natron-ai-settings-test/onnxruntime";
    libraryPath->setValue(bogus);
    enabled->setValue(true);
    device->setValue((int)eAiDeviceMigraphx);
    gpuIndex->setValue(2);
    threads->setValue(6);
    settings->saveSettings(knobs, false, false);
    {
        QSettings saved(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));
        EXPECT_EQ(QString::fromUtf8("migraphx"), saved.value(QString::fromUtf8("aiDevice")).toString()) << "choices save by id";
    }

    enabled->setValue(false);
    device->setValue(0);
    gpuIndex->setValue(0);
    threads->setValue(0);
    libraryPath->setValue(std::string());
    settings->restoreKnobsFromSettings(knobs);

    EXPECT_TRUE(settings->isAiEnabled());
    EXPECT_EQ(eAiDeviceMigraphx, settings->getAiDevice());
    EXPECT_EQ(2, settings->getAiGpuIndex());
    EXPECT_EQ(6, settings->getAiThreads());
    EXPECT_EQ(bogus, settings->getAiOrtLibraryPath());
}
