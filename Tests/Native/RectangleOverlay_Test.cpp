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

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/HostOverlaySupport.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"

#define kRectangleOverlayPluginID "test.natron.built-in.RectangleOverlay"
#define kRectangleOverlayMinimalPluginID "test.natron.built-in.RectangleOverlayMinimal"

#define kRectangleParamBottomLeft "bottomLeft"
#define kRectangleParamSize "size"
#define kRectangleParamInteractive "interactive"
#define kRectangleParamEnable "rectangleInteractEnable"

NATRON_NAMESPACE_ENTER

template <bool WITH_OPTIONAL_KNOBS>
class RectangleOverlayTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new RectangleOverlayTestEffect(n);
    }

    explicit RectangleOverlayTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = WITH_OPTIONAL_KNOBS ? kRectangleOverlayPluginID : kRectangleOverlayMinimalPluginID;
        desc.label = WITH_OPTIONAL_KNOBS ? "Test Rectangle Overlay" : "Test Rectangle Overlay Minimal";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));

        KnobDoublePtr bottomLeft = createKnob<KnobDouble>(std::string("Bottom Left"), 2);
        bottomLeft->setName(kRectangleParamBottomLeft);
        page->addKnob(bottomLeft);

        KnobDoublePtr size = createKnob<KnobDouble>(std::string("Size"), 2);
        size->setName(kRectangleParamSize);
        size->setDefaultValue(100., 0);
        size->setDefaultValue(50., 1);
        page->addKnob(size);

        KnobBoolPtr interactive;
        KnobBoolPtr enable;
        if (WITH_OPTIONAL_KNOBS) {
            interactive = createKnob<KnobBool>(std::string("Interactive Update"));
            interactive->setName(kRectangleParamInteractive);
            page->addKnob(interactive);

            enable = createKnob<KnobBool>(std::string("Rectangle Interact Enable"));
            enable->setName(kRectangleParamEnable);
            enable->setDefaultValue(true);
            enable->setSecretByDefault(true);
            page->addKnob(enable);
        }

        NodePtr node = getNode();
        if (node) {
            node->addRectangleInteract(bottomLeft, size, interactive, enable);
        }
    }
};

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {

template <typename PLUGIN>
void
registerRectangleOverlayPlugin()
{
    EffectInstancePtr node(PLUGIN::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&PLUGIN::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made
    // from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));

    std::list<PluginActionShortcut> shortcuts;
    node->getPluginShortcuts(&shortcuts);
    p->setShorcuts(shortcuts);
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerRectangleOverlayPluginsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerRectangleOverlayPlugin<RectangleOverlayTestEffect<true>>();
        registerRectangleOverlayPlugin<RectangleOverlayTestEffect<false>>();
    });
}

HostOverlayKnobsRectanglePtr
onlyPendingRectangle(const NodePtr& node)
{
    std::list<HostOverlayKnobsPtr> pending = node->getPendingHostOverlays();

    if (pending.size() != 1) {
        return HostOverlayKnobsRectanglePtr();
    }

    return std::dynamic_pointer_cast<HostOverlayKnobsRectangle>(pending.front());
}

} // namespace

class RectangleOverlayTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRectangleOverlayPluginsOnce();
        BaseTest::SetUp();
    }
};

TEST_F(RectangleOverlayTest, GuiLessNodeStoresTheRectangleOverlayKnobs)
{
    NodePtr node = createNode(QString::fromUtf8(kRectangleOverlayPluginID));
    ASSERT_TRUE(bool(node));

    EXPECT_EQ(1u, node->getPendingHostOverlays().size());
    HostOverlayKnobsRectanglePtr knobs = onlyPendingRectangle(node);
    ASSERT_TRUE(bool(knobs));

    KnobIPtr bottomLeft = node->getKnobByName(kRectangleParamBottomLeft);
    KnobIPtr size = node->getKnobByName(kRectangleParamSize);
    KnobIPtr interactive = node->getKnobByName(kRectangleParamInteractive);
    KnobIPtr enable = node->getKnobByName(kRectangleParamEnable);
    ASSERT_TRUE(bool(bottomLeft));
    ASSERT_TRUE(bool(size));
    ASSERT_TRUE(bool(interactive));
    ASSERT_TRUE(bool(enable));

    EXPECT_EQ(bottomLeft, knobs->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationBottomLeft));
    EXPECT_EQ(size, knobs->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationSize));
    EXPECT_EQ(interactive, knobs->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationInteractive));
    EXPECT_EQ(enable, knobs->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationEnable));
    EXPECT_EQ(bottomLeft, knobs->getFirstKnob());
    EXPECT_TRUE(knobs->checkHostOverlayValid());
}

TEST_F(RectangleOverlayTest, OptionalInteractiveAndEnableKnobsMayBeOmitted)
{
    NodePtr node = createNode(QString::fromUtf8(kRectangleOverlayMinimalPluginID));
    ASSERT_TRUE(bool(node));

    HostOverlayKnobsRectanglePtr knobs = onlyPendingRectangle(node);
    ASSERT_TRUE(bool(knobs));

    EXPECT_EQ(node->getKnobByName(kRectangleParamBottomLeft), knobs->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationBottomLeft));
    EXPECT_EQ(node->getKnobByName(kRectangleParamSize), knobs->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationSize));
    EXPECT_FALSE(bool(knobs->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationInteractive)));
    EXPECT_FALSE(bool(knobs->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationEnable)));
    EXPECT_TRUE(knobs->checkHostOverlayValid());
}

TEST_F(RectangleOverlayTest, OverlayIsInvalidWithoutSizeOrWithAWrongKnobType)
{
    NodePtr node = createNode(QString::fromUtf8(kRectangleOverlayPluginID));
    ASSERT_TRUE(bool(node));

    KnobIPtr bottomLeft = node->getKnobByName(kRectangleParamBottomLeft);
    KnobIPtr interactive = node->getKnobByName(kRectangleParamInteractive);
    ASSERT_TRUE(bool(bottomLeft));
    ASSERT_TRUE(bool(interactive));

    HostOverlayKnobsRectangle missingSize;
    missingSize.addKnob(bottomLeft, HostOverlayKnobsRectangle::eKnobsEnumerationBottomLeft);
    EXPECT_FALSE(missingSize.checkHostOverlayValid());

    HostOverlayKnobsRectangle boolAsSize;
    boolAsSize.addKnob(bottomLeft, HostOverlayKnobsRectangle::eKnobsEnumerationBottomLeft);
    boolAsSize.addKnob(interactive, HostOverlayKnobsRectangle::eKnobsEnumerationSize);
    EXPECT_FALSE(boolAsSize.checkHostOverlayValid());
}
