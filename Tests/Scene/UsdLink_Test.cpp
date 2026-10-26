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

CLANG_DIAG_OFF(deprecated)
#include <QFile>
#include <QString>
#include <QTemporaryDir>
CLANG_DIAG_ON(deprecated)

#include "BaseTest.h"

#include "Engine/AppManager.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

// The binary links OpenUSD and its plug-ins resolve: a stage builds in memory.
TEST(UsdLinkTest, StageCreatedInMemoryHoldsAPrim)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
    ASSERT_TRUE(bool(stage));

    PXR_NS::UsdPrim prim = stage->DefinePrim(PXR_NS::SdfPath("/World"), PXR_NS::TfToken("Xform"));
    EXPECT_TRUE(prim.IsValid());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World")).IsValid());
}

// UsdStage::Open reads a .usda from disk through the usda file-format plug-in.
TEST(UsdLinkTest, StageOpensAUsdaFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("smoke.usda"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("#usda 1.0\n"
                   "(\n"
                   "    upAxis = \"Z\"\n"
                   ")\n"
                   "\n"
                   "def Xform \"World\"\n"
                   "{\n"
                   "    def Sphere \"Ball\"\n"
                   "    {\n"
                   "        double radius = 2\n"
                   "    }\n"
                   "}\n");
    }

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdPrim ball = stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball"));
    ASSERT_TRUE(ball.IsValid());
    EXPECT_EQ(PXR_NS::TfToken("Sphere"), ball.GetTypeName());

    double radius = 0.;
    EXPECT_TRUE(ball.GetAttribute(PXR_NS::TfToken("radius")).Get(&radius));
    EXPECT_EQ(2., radius);

    PXR_NS::TfToken upAxis;
    EXPECT_TRUE(stage->GetMetadata(PXR_NS::UsdGeomTokens->upAxis, &upAxis));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->z, upAxis);
}

// The file formats scene nodes read are all registered, Alembic through USD's usdAbc plug-in.
TEST(UsdLinkTest, SceneFileFormatsAreRegistered)
{
    const char* extensions[] = { "usd", "usda", "usdc", "usdz", "abc" };

    for (const char* extension : extensions) {
        EXPECT_TRUE(bool(PXR_NS::SdfFileFormat::FindByExtension(extension))) << extension;
    }
}

class UsdLinkPythonTest
    : public BaseTest {
};

// Natron's embedded Python imports the same USD the binary links: the bindings were built
// against this Python (cmake/NatronUsd.cmake checks the version).
TEST_F(UsdLinkPythonTest, EmbeddedPythonImportsPxr)
{
    const std::string script = "from pxr import Sdf, Usd\n"
                               "_usdLinkStage = Usd.Stage.CreateInMemory()\n"
                               "if not _usdLinkStage.DefinePrim('/World', 'Xform').IsValid():\n"
                               "    raise RuntimeError('pxr could not define a prim')\n"
                               "if Sdf.FileFormat.FindByExtension('usda') is None:\n"
                               "    raise RuntimeError('pxr has no usda file format')\n"
                               "del _usdLinkStage\n";
    std::string error, output;

    EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << error;
}
