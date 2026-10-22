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
#include <QSettings>
#include <QString>
CLANG_DIAG_ON(deprecated)

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/GPUContextPool.h"
#include "Engine/GpuDeviceList.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {

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

bool
pageHoldsKnob(const KnobPagePtr& page, const std::string& name)
{
    for (const KnobIPtr& child : page->getChildren()) {
        if (child->getName() == name) {
            return true;
        }
    }

    return false;
}

std::vector<GpuDeviceSummary>
fakeDevices()
{
    GpuDeviceSummary discrete;
    discrete.index = 0;
    discrete.name = "Fake Discrete";
    discrete.type = GpuDeviceType::Discrete;
    discrete.uuid[0] = 0xab;

    GpuDeviceSummary cpu;
    cpu.index = 1;
    cpu.name = "llvmpipe";
    cpu.type = GpuDeviceType::Cpu;
    cpu.uuid[15] = 0x01;

    return { discrete, cpu };
}

// Puts the device menu back to the startup state, with the real devices and "auto" selected, however the test exits.
class GpuDeviceMenuRestorer {
public:
    explicit GpuDeviceMenuRestorer(const KnobChoicePtr& knob)
        : _knob(knob)
    {
    }

    ~GpuDeviceMenuRestorer()
    {
        QSettings settings(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));
        settings.remove(QString::fromUtf8("gpuDevice"));
        appPTR->getCurrentSettings()->populateGpuDevices(enumerateGpuDevices());
        _knob->setValue(0);
    }

    GpuDeviceMenuRestorer(const GpuDeviceMenuRestorer&) = delete;
    GpuDeviceMenuRestorer& operator=(const GpuDeviceMenuRestorer&) = delete;

private:
    KnobChoicePtr _knob;
};

} // namespace

TEST(GpuPreferences, GpuPageHoldsTheGpuKnobsAndThreadingDoesNot)
{
    const KnobPagePtr gpu = findSettingsPage("GPU");
    ASSERT_NE(gpu, nullptr) << "no Preferences page labelled \"GPU\"";
    const KnobPagePtr threading = findSettingsPage("Threading");
    ASSERT_NE(threading, nullptr);

    for (const char* name : { "gpuComputeMode", "gpuDevice", "gpuMemoryBudgetMB", "gpuStatus" }) {
        EXPECT_TRUE(pageHoldsKnob(gpu, name)) << name << " is not on the GPU page";
        EXPECT_FALSE(pageHoldsKnob(threading, name)) << name << " is still on the Threading page";
    }

    KnobStringPtr status = appPTR->getCurrentSettings()->getKnobByNameAndType<KnobString>("gpuStatus");
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->isLabel());
    EXPECT_EQ(appPTR->getGPUContextPool()->getRendererStatus(), status->getValue());
}

TEST(GpuPreferences, GpuDeviceIsAChoiceOfAutoThenEachEnumeratedDevice)
{
    KnobChoicePtr device = appPTR->getCurrentSettings()->getKnobByNameAndType<KnobChoice>("gpuDevice");
    ASSERT_NE(device, nullptr) << "gpuDevice is not a choice";

    const std::vector<GpuDeviceSummary> devices = enumerateGpuDevices();
    const std::vector<ChoiceOption> entries = device->getEntries_mt_safe();
    ASSERT_EQ(devices.size() + 1, entries.size());
    EXPECT_EQ("auto", entries[0].id);
    for (std::size_t i = 0; i < devices.size(); ++i) {
        EXPECT_EQ(uuidHex(devices[i].uuid), entries[i + 1].id);
    }
    EXPECT_EQ(0, device->getValue());
    EXPECT_EQ("", appPTR->getCurrentSettings()->getGpuDevice()) << "\"auto\" must read as an empty device id";
}

TEST(GpuPreferences, DeviceEntriesAreLabelledWithNameAndTypeAndReadAsTheirUuid)
{
    KnobChoicePtr device = appPTR->getCurrentSettings()->getKnobByNameAndType<KnobChoice>("gpuDevice");
    ASSERT_NE(device, nullptr);
    GpuDeviceMenuRestorer restore(device);

    const std::vector<GpuDeviceSummary> fake = fakeDevices();
    appPTR->getCurrentSettings()->populateGpuDevices(fake);
    const std::vector<ChoiceOption> entries = device->getEntries_mt_safe();
    ASSERT_EQ(3u, entries.size());
    EXPECT_EQ("auto", entries[0].id);
    EXPECT_EQ("Fake Discrete (discrete)", entries[1].label);
    EXPECT_EQ("llvmpipe (CPU)", entries[2].label);

    device->setValue(2);
    EXPECT_EQ(uuidHex(fake[1].uuid), appPTR->getCurrentSettings()->getGpuDevice());
}

TEST(GpuPreferences, SavedDeviceIsRestoredByUuidAndAnOldStringValueIsIgnored)
{
    KnobChoicePtr device = appPTR->getCurrentSettings()->getKnobByNameAndType<KnobChoice>("gpuDevice");
    ASSERT_NE(device, nullptr);
    GpuDeviceMenuRestorer restore(device);

    const std::vector<GpuDeviceSummary> fake = fakeDevices();
    appPTR->getCurrentSettings()->populateGpuDevices(fake);
    const std::vector<KnobI*> knobs { device.get() };

    {
        QSettings settings(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));
        settings.setValue(QString::fromUtf8("gpuDevice"), QString::fromStdString(uuidHex(fake[1].uuid)));
    }
    appPTR->getCurrentSettings()->restoreKnobsFromSettings(knobs);
    EXPECT_EQ(2, device->getValue());

    // A value saved when gpuDevice was a free-text name or index matches no entry, so the knob keeps its value.
    device->setValue(0);
    for (const char* old : { "Radeon", "1", "" }) {
        {
            QSettings settings(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));
            settings.setValue(QString::fromUtf8("gpuDevice"), QString::fromUtf8(old));
        }
        appPTR->getCurrentSettings()->restoreKnobsFromSettings(knobs);
        EXPECT_EQ(0, device->getValue()) << "old value \"" << old << "\"";
        EXPECT_EQ("", appPTR->getCurrentSettings()->getGpuDevice()) << "old value \"" << old << "\"";
    }
}

TEST(GpuPreferences, NoOpenGLPreferenceKnobRemains)
{
    for (const char* name : { "enableOpenGLRendering", "maxOpenGLContexts", "chooseOpenGLRenderer", "activeOpenGLRenderer" }) {
        EXPECT_FALSE(appPTR->getCurrentSettings()->getKnobByName(name)) << name << " still exists";
    }
}
