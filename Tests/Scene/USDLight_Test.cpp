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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <string>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/StageCache.h"
#include "Engine/Nodes/Scene/USDLight.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class USDLightTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        StageCache::instance().clear();

        BaseTest::TearDown();
    }

    // The light prim at the node's default path, "/" and its script name, of its composed stage;
    // invalid after a failed check.
    static PXR_NS::UsdPrim composeLight(const NodePtr& node,
                                        PXR_NS::UsdStageRefPtr* stage)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;
        *stage = StageCache::instance().getStage(payload);
        if (!*stage) {
            ADD_FAILURE() << "no stage";

            return PXR_NS::UsdPrim();
        }

        return (*stage)->GetPrimAtPath(PXR_NS::SdfPath("/" + node->getScriptName_mt_safe()));
    }

    static void setTexture(const NodePtr& light,
                           const std::string& path)
    {
        KnobFile* textureFile = dynamic_cast<KnobFile*>(light->getKnobByName(kUSDLightParamTextureFile).get());

        ASSERT_TRUE(textureFile != NULL);
        textureFile->setValue(path);
    }

    static void setType(const NodePtr& light,
                        const char* typeID)
    {
        KnobChoice* type = dynamic_cast<KnobChoice*>(light->getKnobByName(kUSDLightParamType).get());

        ASSERT_TRUE(type != NULL);
        type->setValueFromID(typeID, 0);
    }
};

TEST_F(USDLightTest, EachTypeAuthorsItsUsdLuxPrimWithTheSharedInputs)
{
    NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
    ASSERT_TRUE(bool(light));
    EXPECT_TRUE(light->getEffectInstance()->producesSceneData());
    KnobColor* color = dynamic_cast<KnobColor*>(light->getKnobByName(kUSDLightParamColor).get());
    KnobDouble* intensity = dynamic_cast<KnobDouble*>(light->getKnobByName(kUSDLightParamIntensity).get());
    KnobDouble* exposure = dynamic_cast<KnobDouble*>(light->getKnobByName(kUSDLightParamExposure).get());
    ASSERT_TRUE(color && intensity && exposure);
    // Working-space values outside [0, 1] are written unchanged.
    color->setValue(0.25, ViewSpec::all(), 0);
    color->setValue(0.5, ViewSpec::all(), 1);
    color->setValue(2., ViewSpec::all(), 2);
    intensity->setValue(3.);
    exposure->setValue(1.5);

    struct Case {
        const char* id;
        const char* typeName;
        const char* sizeAttribute;
        float size;
    };
    const Case cases[] = {
        { "distant", "DistantLight", "inputs:angle", 0.53f },
        { "sphere", "SphereLight", "inputs:radius", 0.5f },
        { "rect", "RectLight", "inputs:width", 1.f },
        { "dome", "DomeLight", NULL, 0.f },
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.id);
        setType(light, c.id);
        PXR_NS::UsdStageRefPtr stage;
        const PXR_NS::UsdPrim prim = composeLight(light, &stage);
        ASSERT_TRUE(prim.IsValid());
        EXPECT_EQ(PXR_NS::TfToken(c.typeName), prim.GetTypeName());

        const PXR_NS::UsdLuxLightAPI lightAPI(prim);
        PXR_NS::GfVec3f colorValue;
        ASSERT_TRUE(lightAPI.GetColorAttr().Get(&colorValue));
        EXPECT_FLOAT_EQ(0.25f, colorValue[0]);
        EXPECT_FLOAT_EQ(0.5f, colorValue[1]);
        EXPECT_FLOAT_EQ(2.f, colorValue[2]);
        float value = 0.f;
        ASSERT_TRUE(lightAPI.GetIntensityAttr().Get(&value));
        EXPECT_FLOAT_EQ(3.f, value);
        ASSERT_TRUE(lightAPI.GetExposureAttr().Get(&value));
        EXPECT_FLOAT_EQ(1.5f, value);
        EXPECT_TRUE(prim.GetAttribute(PXR_NS::TfToken("inputs:intensity")).HasAuthoredValue());
        if (c.sizeAttribute) {
            const PXR_NS::UsdAttribute size = prim.GetAttribute(PXR_NS::TfToken(c.sizeAttribute));
            ASSERT_TRUE(size.HasAuthoredValue());
            ASSERT_TRUE(size.Get(&value));
            EXPECT_FLOAT_EQ(c.size, value);
        }
        // No texture, no warning.
        EXPECT_FALSE(light->hasPersistentMessage());
    }
}

TEST_F(USDLightTest, AMissingDomeTextureIsAuthoredWithAWarning)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string missing = dir.filePath(QString::fromUtf8("missing_sky.exr")).toStdString();
    NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
    ASSERT_TRUE(bool(light));
    setType(light, "dome");
    setTexture(light, missing);

    PXR_NS::UsdStageRefPtr stage;
    const PXR_NS::UsdPrim prim = composeLight(light, &stage);
    ASSERT_TRUE(prim.IsValid());
    EXPECT_EQ(PXR_NS::TfToken("DomeLight"), prim.GetTypeName());
    PXR_NS::SdfAssetPath texture;
    ASSERT_TRUE(PXR_NS::UsdLuxDomeLight(prim).GetTextureFileAttr().Get(&texture));
    EXPECT_EQ(missing, texture.GetAssetPath());
    EXPECT_TRUE(prim.GetAttribute(PXR_NS::TfToken("inputs:texture:file")).HasAuthoredValue());

    ASSERT_TRUE(light->hasPersistentMessage());
    QString message;
    int type = 0;
    light->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromStdString(missing))) << message.toStdString();

    // A texture that resolves clears the warning.
    const std::string present = dir.filePath(QString::fromUtf8("present_sky.exr")).toStdString();
    {
        QFile f(QString::fromStdString(present));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("not read");
    }
    setTexture(light, present);
    const PXR_NS::UsdPrim resolved = composeLight(light, &stage);
    ASSERT_TRUE(resolved.IsValid());
    ASSERT_TRUE(PXR_NS::UsdLuxDomeLight(resolved).GetTextureFileAttr().Get(&texture));
    EXPECT_EQ(present, texture.GetAssetPath());
    EXPECT_FALSE(light->hasPersistentMessage());

    // So does a type that has no texture.
    setTexture(light, missing);
    (void)composeLight(light, &stage);
    EXPECT_TRUE(light->hasPersistentMessage());
    setType(light, "sphere");
    (void)composeLight(light, &stage);
    EXPECT_FALSE(light->hasPersistentMessage());
}
