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

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>

#include <gtest/gtest.h>

#include "Global/GLIncludes.h" // must precede qopengl.h
#include <QColor>
#include <QFont>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>

#include "Engine/AppManager.h"
#include "Gui/TextRenderer.h"

// Only AppManager is imported: the fixture shares its name with the class under test.
using NATRON_NAMESPACE::AppManager;

// A TextRenderer's glyph atlases are texture names of the context that was current when the text
// was drawn. These tests draw in several unshared Qt contexts and check, with glIsTexture in each,
// that every context gets its own atlases, that a context's death frees only its own, and that
// nothing is deleted in a context that does not own it.
//
// Like GuiThreadGLBinding, the tests need an X server with GLX; without one they print a skip line
// and pass, unless NATRON_TEST_REQUIRE_GL=1 turns the skip into a failure.

namespace {

bool
skipWithoutGL(const char* reason)
{
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");

    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << reason << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::cout << "[  SKIPPED ] TextRenderer: " << reason
              << "; run with DISPLAY set to an X server with GLX" << std::endl;

    return true;
}

class TextRenderer
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!appPTR->isOpenGLLoaded()) {
            _skip = skipWithoutGL("Natron did not load OpenGL");

            return;
        }
        QOpenGLContext probe;
        if (!probe.create()) {
            _skip = skipWithoutGL("Qt could not create an OpenGL context");

            return;
        }
        _surface.reset(new QOffscreenSurface);
        _surface->setFormat(probe.format());
        _surface->create();
        if (!_surface->isValid() || !probe.makeCurrent(_surface.get())) {
            _skip = skipWithoutGL("Qt could not make its OpenGL context current");

            return;
        }
        probe.doneCurrent();
    }

    void TearDown() override
    {
        if (QOpenGLContext* current = QOpenGLContext::currentContext()) {
            current->doneCurrent();
        }
        _surface.reset();
    }

    std::unique_ptr<QOpenGLContext> createContext()
    {
        std::unique_ptr<QOpenGLContext> context(new QOpenGLContext);
        context->setFormat(_surface->format());
        if (!context->create()) {
            ADD_FAILURE() << "creating a QOpenGLContext failed";

            return nullptr;
        }

        return context;
    }

    bool makeCurrent(QOpenGLContext* context)
    {
        return context->makeCurrent(_surface.get());
    }

    static void drawText(const NATRON_NAMESPACE::TextRenderer& renderer)
    {
        renderer.renderText(0.f, 0.f, 1.f, 1.f, QString::fromUtf8("Natron 0123"), QColor(255, 255, 255), QFont());
    }

    static void expectTextures(const std::list<unsigned int>& names,
                               bool exist,
                               const char* where)
    {
        for (unsigned int name : names) {
            EXPECT_EQ(exist, glIsTexture(name) == GL_TRUE) << "texture " << name << " " << where;
        }
    }

    bool _skip = false;
    std::unique_ptr<QOffscreenSurface> _surface;
};

} // namespace

TEST_F(TextRenderer, EachContextGetsItsOwnAtlases)
{
    if (_skip) {
        return;
    }
    std::unique_ptr<QOpenGLContext> first = createContext();
    std::unique_ptr<QOpenGLContext> second = createContext();
    ASSERT_TRUE(first && second);
    NATRON_NAMESPACE::TextRenderer renderer;

    ASSERT_TRUE(makeCurrent(first.get()));
    drawText(renderer);
    const std::list<unsigned int> firstNames = renderer.textureNamesOfCurrentContext();
    ASSERT_FALSE(firstNames.empty());
    expectTextures(firstNames, true, "drawn in the first context");

    ASSERT_TRUE(makeCurrent(second.get()));
    EXPECT_TRUE(renderer.textureNamesOfCurrentContext().empty()) << "the second context sees the first one's atlases";
    drawText(renderer);
    const std::list<unsigned int> secondNames = renderer.textureNamesOfCurrentContext();
    ASSERT_FALSE(secondNames.empty());
    expectTextures(secondNames, true, "drawn in the second context");

    ASSERT_TRUE(makeCurrent(first.get()));
    EXPECT_EQ(firstNames, renderer.textureNamesOfCurrentContext());
    expectTextures(firstNames, true, "of the first context after drawing in the second");
}

TEST_F(TextRenderer, DestroyedContextReleasesOnlyItsOwnAtlases)
{
    if (_skip) {
        return;
    }
    std::unique_ptr<QOpenGLContext> survivor = createContext();
    std::unique_ptr<QOpenGLContext> dying = createContext();
    ASSERT_TRUE(survivor && dying);
    NATRON_NAMESPACE::TextRenderer renderer;

    ASSERT_TRUE(makeCurrent(survivor.get()));
    drawText(renderer);
    const std::list<unsigned int> survivorNames = renderer.textureNamesOfCurrentContext();
    ASSERT_FALSE(survivorNames.empty());
    ASSERT_TRUE(makeCurrent(dying.get()));
    drawText(renderer);
    ASSERT_FALSE(renderer.textureNamesOfCurrentContext().empty());

    // The release has to borrow the dying context and then put back the one that was current.
    ASSERT_TRUE(makeCurrent(survivor.get()));
    dying.reset();
    EXPECT_EQ(survivor.get(), QOpenGLContext::currentContext()) << "releasing the dead context's atlases left another context current";
    EXPECT_EQ(survivorNames, renderer.textureNamesOfCurrentContext());
    expectTextures(survivorNames, true, "of the surviving context after the other one died");

    drawText(renderer);
    expectTextures(renderer.textureNamesOfCurrentContext(), true, "of the surviving context drawn again");

    std::unique_ptr<QOpenGLContext> third = createContext();
    ASSERT_TRUE(third != nullptr);
    ASSERT_TRUE(makeCurrent(third.get()));
    EXPECT_TRUE(renderer.textureNamesOfCurrentContext().empty()) << "a new context sees another context's atlases";
    drawText(renderer);
    const std::list<unsigned int> thirdNames = renderer.textureNamesOfCurrentContext();
    ASSERT_FALSE(thirdNames.empty());
    expectTextures(thirdNames, true, "drawn in a context created after one died");

    third->doneCurrent();
    third.reset();
    EXPECT_EQ(nullptr, QOpenGLContext::currentContext()) << "releasing the dead context's atlases left a context current";
}

TEST_F(TextRenderer, ReleaseCurrentContextDeletesItsAtlases)
{
    if (_skip) {
        return;
    }
    std::unique_ptr<QOpenGLContext> context = createContext();
    ASSERT_TRUE(context != nullptr);
    NATRON_NAMESPACE::TextRenderer renderer;

    ASSERT_TRUE(makeCurrent(context.get()));
    drawText(renderer);
    const std::list<unsigned int> names = renderer.textureNamesOfCurrentContext();
    ASSERT_FALSE(names.empty());

    renderer.releaseCurrentContext();
    EXPECT_TRUE(renderer.textureNamesOfCurrentContext().empty());
    expectTextures(names, false, "after releaseCurrentContext");

    drawText(renderer);
    const std::list<unsigned int> rebuilt = renderer.textureNamesOfCurrentContext();
    ASSERT_FALSE(rebuilt.empty());
    expectTextures(rebuilt, true, "rebuilt after releaseCurrentContext");
}

TEST_F(TextRenderer, DestructorNeverDeletesInAContextThatDoesNotOwnTheNames)
{
    if (_skip) {
        return;
    }
    std::unique_ptr<QOpenGLContext> owner = createContext();
    std::unique_ptr<QOpenGLContext> other = createContext();
    ASSERT_TRUE(owner && other);
    std::list<unsigned int> ownerNames;
    std::list<unsigned int> otherNames;
    GLuint bystander = 0;
    {
        NATRON_NAMESPACE::TextRenderer renderer;
        ASSERT_TRUE(makeCurrent(owner.get()));
        drawText(renderer);
        ownerNames = renderer.textureNamesOfCurrentContext();
        ASSERT_FALSE(ownerNames.empty());

        ASSERT_TRUE(makeCurrent(other.get()));
        drawText(renderer);
        otherNames = renderer.textureNamesOfCurrentContext();
        ASSERT_FALSE(otherNames.empty());

        // Unshared contexts hand out the same small names, so a texture of the other context's own
        // is likely to carry one of the owner's atlas names.
        glGenTextures(1, &bystander);
        glBindTexture(GL_TEXTURE_2D, bystander);
        glBindTexture(GL_TEXTURE_2D, 0);
        ASSERT_TRUE(glIsTexture(bystander) == GL_TRUE);
    }
    EXPECT_TRUE(glIsTexture(bystander) == GL_TRUE) << "the renderer deleted the owner's names in another context";
    expectTextures(otherNames, false, "of the context current when the renderer was destroyed");

    ASSERT_TRUE(makeCurrent(owner.get()));
    expectTextures(ownerNames, true, "of a context that was not current when the renderer was destroyed");
}
