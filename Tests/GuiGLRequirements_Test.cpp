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

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QSurfaceFormat>

#include "Gui/GuiGLRequirements.h"

NATRON_NAMESPACE_USING

namespace {
GuiGLRequirements::ContextInfo
context(int major,
        int minor,
        QSurfaceFormat::OpenGLContextProfile profile)
{
    GuiGLRequirements::ContextInfo info;
    info.created = true;
    info.majorVersion = major;
    info.minorVersion = minor;
    info.profile = profile;

    return info;
}
} // namespace

TEST(GuiGLRequirements, AcceptsCompatibility45AndLater)
{
    EXPECT_TRUE(GuiGLRequirements::judge(context(4, 5, QSurfaceFormat::CompatibilityProfile)).accepted);
    EXPECT_TRUE(GuiGLRequirements::judge(context(4, 6, QSurfaceFormat::CompatibilityProfile)).accepted);
    EXPECT_TRUE(GuiGLRequirements::judge(context(5, 0, QSurfaceFormat::CompatibilityProfile)).accepted);
    EXPECT_TRUE(GuiGLRequirements::judge(context(4, 6, QSurfaceFormat::CompatibilityProfile)).message.isEmpty());
}

TEST(GuiGLRequirements, RefusesOldVersionsNamingWhatWasObtained)
{
    GuiGLRequirements::ContextInfo info = context(2, 1, QSurfaceFormat::NoProfile);
    info.versionString = QString::fromUtf8("2.1 Mesa 25.0.0");
    info.renderer = QString::fromUtf8("llvmpipe (LLVM 21.1.0, 256 bits)");
    const GuiGLRequirements::Verdict verdict = GuiGLRequirements::judge(info);

    EXPECT_FALSE(verdict.accepted);
    EXPECT_TRUE(verdict.message.contains(QString::fromUtf8("OpenGL 4.5 compatibility-profile")));
    EXPECT_TRUE(verdict.message.contains(QString::fromUtf8("gave OpenGL 2.1 (no profile)")));
    EXPECT_TRUE(verdict.message.contains(QString::fromUtf8("GL_VERSION: 2.1 Mesa 25.0.0")));
    EXPECT_TRUE(verdict.message.contains(QString::fromUtf8("GL_RENDERER: llvmpipe")));

    EXPECT_FALSE(GuiGLRequirements::judge(context(3, 3, QSurfaceFormat::CompatibilityProfile)).accepted);
    EXPECT_FALSE(GuiGLRequirements::judge(context(4, 4, QSurfaceFormat::CompatibilityProfile)).accepted);
}

TEST(GuiGLRequirements, RefusesCoreAndProfilelessContexts)
{
    const GuiGLRequirements::Verdict core = GuiGLRequirements::judge(context(4, 6, QSurfaceFormat::CoreProfile));
    EXPECT_FALSE(core.accepted);
    EXPECT_TRUE(core.message.contains(QString::fromUtf8("gave OpenGL 4.6 (core profile)")));

    EXPECT_FALSE(GuiGLRequirements::judge(context(4, 6, QSurfaceFormat::NoProfile)).accepted);
}

TEST(GuiGLRequirements, RefusesWhenNoContextCouldBeCreated)
{
    const GuiGLRequirements::Verdict verdict = GuiGLRequirements::judge(GuiGLRequirements::ContextInfo());
    EXPECT_FALSE(verdict.accepted);
    EXPECT_TRUE(verdict.message.contains(QString::fromUtf8("could not create one")));
}

TEST(GuiGLRequirements, RequestsCompatibility45KeepingTheBaseFormat)
{
    QSurfaceFormat base;
    base.setDepthBufferSize(24);
    base.setStencilBufferSize(8);
    QString warning;
    const QSurfaceFormat format = GuiGLRequirements::requestedFormat(base, QByteArray(), QByteArray(), &warning);

    EXPECT_EQ(4, format.majorVersion());
    EXPECT_EQ(5, format.minorVersion());
    EXPECT_EQ(QSurfaceFormat::CompatibilityProfile, format.profile());
    EXPECT_EQ(QSurfaceFormat::OpenGL, format.renderableType());
    EXPECT_EQ(24, format.depthBufferSize());
    EXPECT_EQ(8, format.stencilBufferSize());
    EXPECT_TRUE(warning.isEmpty());
}

TEST(GuiGLRequirements, OverridesForceTheRequest)
{
    QString warning;
    const QSurfaceFormat old = GuiGLRequirements::requestedFormat(QSurfaceFormat(), QByteArray("2.1"), QByteArray(), &warning);
    EXPECT_EQ(2, old.majorVersion());
    EXPECT_EQ(1, old.minorVersion());
    EXPECT_EQ(QSurfaceFormat::CompatibilityProfile, old.profile());

    const QSurfaceFormat core = GuiGLRequirements::requestedFormat(QSurfaceFormat(), QByteArray(), QByteArray("Core"), &warning);
    EXPECT_EQ(4, core.majorVersion());
    EXPECT_EQ(5, core.minorVersion());
    EXPECT_EQ(QSurfaceFormat::CoreProfile, core.profile());
    EXPECT_TRUE(warning.isEmpty());
}

TEST(GuiGLRequirements, IgnoresMalformedOverrides)
{
    QString warning;
    const QSurfaceFormat format = GuiGLRequirements::requestedFormat(QSurfaceFormat(), QByteArray("four"), QByteArray("es"), &warning);
    EXPECT_EQ(4, format.majorVersion());
    EXPECT_EQ(5, format.minorVersion());
    EXPECT_EQ(QSurfaceFormat::CompatibilityProfile, format.profile());
    EXPECT_TRUE(warning.contains(QString::fromUtf8(GuiGLRequirements::kRequestVersionEnv)));
    EXPECT_TRUE(warning.contains(QString::fromUtf8(GuiGLRequirements::kRequestProfileEnv)));
}
