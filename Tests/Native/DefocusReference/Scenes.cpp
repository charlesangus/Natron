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

#include "Scenes.h"

#include <cmath>

namespace DefocusReference {

namespace {

    const Vec3 kFacing(0., 0., 1.);
    const Vec3 kRight(1., 0., 0.);
    const Vec3 kUp(0., 1., 0.);

    // A card facing the camera over [x1, x2] x [y1, y2] at planar depth `depth`.
    void
    addFacingCard(AnalyticScene* scene,
                  double x1,
                  double y1,
                  double x2,
                  double y2,
                  double depth,
                  const Colour& colour)
    {
        scene->addCard(Vec3((x1 + x2) / 2., (y1 + y2) / 2., -depth), kFacing, kRight, (x2 - x1) / 2., (y2 - y1) / 2., colour);
    }

    // A wall at `depth` from x1 to x2 and up to `top`, with vertical stripes every `pitch` just in
    // front of it, so a blur of it shows.
    void
    addStripedWall(AnalyticScene* scene,
                   double depth,
                   double x1,
                   double x2,
                   double bottom,
                   double top,
                   double pitch,
                   const Colour& wall,
                   const Colour& stripe)
    {
        addFacingCard(scene, x1, bottom, x2, top, depth, wall);
        for (double x = x1 + pitch / 2.; x < x2; x += pitch) {
            addFacingCard(scene, x, bottom, x + pitch / 4., top, depth * 0.9975, stripe);
        }
    }

    // A grid of emissive points of `energy` at the full-size width, kept at the same value per pixel
    // at any width.
    void
    addPointGrid(AnalyticScene* scene,
                 double depth,
                 double x1,
                 double x2,
                 double y1,
                 double y2,
                 double pitch,
                 double energy)
    {
        const double scale = std::pow(scene->camera.width / 512., 2.);
        int i = 0;

        for (double y = y1; y <= y2 + 1e-9; y += pitch) {
            for (double x = x1; x <= x2 + 1e-9; x += pitch, ++i) {
                const float warm = (i % 3 == 0) ? 1.f : ((i % 3 == 1) ? 0.8f : 0.6f);
                scene->addPoint(Vec3(x, y, -depth), 0.5, Colour((float)(energy * scale * warm), (float)(energy * scale * 0.85), (float)(energy * scale * (1.4 - warm))));
            }
        }
    }

    Camera
    camera(int width,
           int height,
           double focusDistance,
           double fStop)
    {
        Camera c;

        c.width = width;
        c.height = height;
        c.focalLength = 50.;
        c.filmBackWidth = 36.;
        c.millimetresPerUnit = 1000.;
        c.focusDistance = focusDistance;
        c.fStop = fStop;

        return c;
    }

} // namespace

const std::vector<std::string>&
fixtureSceneNames()
{
    static const std::vector<std::string> names = {
        "sharpFgBlurBg", "blurFgSharpBg", "aliasedEdge", "grazingFloor", "highlightsBehindEdge", "eclipsedBokeh"
    };

    return names;
}

FixtureScene
makeFixtureScene(const std::string& name,
                 int width,
                 int height)
{
    FixtureScene f;
    AnalyticScene& s = f.scene;

    f.name = name;
    if (name == "sharpFgBlurBg") {
        s.camera = camera(width, height, 1., 1.4);
        addStripedWall(&s, 4., -2., 2., -1., 0.4, 0.4, Colour(0.15f, 0.3f, 0.6f), Colour(0.8f, 0.8f, 0.7f));
        s.addSphere(Vec3(0.45, -0.1, -2.5), 0.12, Colour(0.2f, 0.7f, 0.25f));
        addFacingCard(&s, -0.4, -0.3, 0., 0.1, 1., Colour(0.9f, 0.45f, 0.15f));
        f.nearDepth = 1.;
        f.farDepth = 4.;
    } else if (name == "blurFgSharpBg") {
        s.camera = camera(width, height, 3., 1.4);
        addStripedWall(&s, 3., -3., 3., -2., 0.35, 0.3, Colour(0.2f, 0.35f, 0.15f), Colour(0.85f, 0.8f, 0.6f));
        s.addDisc(Vec3(0.1, 0.04, -0.8), kFacing, 0.05, Colour(0.3f, 0.5f, 0.9f, 0.5f));
        addFacingCard(&s, -0.3, -0.3, -0.02, 0.06, 0.6, Colour(0.85f, 0.25f, 0.2f));
        f.nearDepth = 0.6;
        f.farDepth = 3.;
    } else if (name == "aliasedEdge") {
        s.camera = camera(width, height, 2., 1.4);
        f.pinhole.depthSamples = 4;
        addStripedWall(&s, 5., -4., 4., -3., 0.6, 0.5, Colour(0.1f, 0.2f, 0.5f), Colour(0.7f, 0.75f, 0.8f));
        const double angle = 20. * 3.14159265358979323846 / 180.;
        s.addCard(Vec3(-0.25, 0., -1.2), kFacing, Vec3(std::cos(angle), std::sin(angle), 0.), 0.25, 0.4, Colour(0.9f, 0.85f, 0.3f));
        f.nearDepth = 1.2;
        f.farDepth = 5.;
    } else if (name == "grazingFloor") {
        s.camera = camera(width, height, 1.5, 1.4);
        // The floor's v axis, up x right, points away from the camera.
        s.addCard(Vec3(0., -0.12, -6.), kUp, kRight, 3., 5.7, Colour(0.5f, 0.45f, 0.4f));
        for (double z = 0.75; z < 11.; z += 0.5) {
            s.addCard(Vec3(0., -0.1199, -z), kUp, kRight, 3., 0.08, Colour(0.9f, 0.9f, 0.85f));
        }
        s.addSphere(Vec3(0.15, -0.02, -2.2), 0.1, Colour(0.8f, 0.2f, 0.15f));
        f.nearDepth = 0.6;
        f.farDepth = 11.7;
    } else if (name == "highlightsBehindEdge") {
        s.camera = camera(width, height, 1., 1.4);
        addFacingCard(&s, -3., -3., 3., 0.6, 5., Colour(0.05f, 0.05f, 0.07f));
        addPointGrid(&s, 4.98, -0.6, 0.6, -0.4, 0.4, 0.15, 600.);
        addFacingCard(&s, -0.4, -0.3, -0.05, 0.3, 1., Colour(0.25f, 0.22f, 0.2f));
        f.nearDepth = 1.;
        f.farDepth = 5.;
    } else if (name == "eclipsedBokeh") {
        s.camera = camera(width, height, 2., 1.);
        addFacingCard(&s, -8., -8., 8., 1.5, 12., Colour(0.03f, 0.03f, 0.04f));
        addPointGrid(&s, 11.95, -3., 2., -1.8, 1.2, 0.6, 300.);
        addFacingCard(&s, -0.5, -0.3, -0.02, 0.3, 0.8, Colour(0.3f, 0.2f, 0.15f));
        f.nearDepth = 0.8;
        f.farDepth = 12.;
    }

    return f;
} // makeFixtureScene

} // namespace DefocusReference
