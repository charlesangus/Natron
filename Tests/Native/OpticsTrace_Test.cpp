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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Optics/Coating.h"
#include "Engine/Optics/Glass.h"
#include "Engine/Optics/Paraxial.h"
#include "Engine/Optics/Prescription.h"
#include "Engine/Optics/Trace.h"
#include "Engine/Optics/ZemaxParser.h"

using namespace Natron::Optics;

namespace {

const char* const kTriplet = NATRON_TESTS_FIXTURES_DIR "/optics/cooke-triplet-50mm-f4.zmx";
const char* const kZoom = NATRON_TESTS_FIXTURES_DIR "/optics/aspheric-zoom-position2.zmx";

Surface
surface(SurfaceType type,
        double radius,
        double spacing,
        double semiDiameter,
        double nd = 1.,
        double vd = 0.)
{
    Surface s;
    s.type = type;
    s.curvature = radius == 0. ? 0. : 1. / radius;
    s.spacing = spacing;
    s.semiDiameter = semiDiameter;
    s.nd = nd;
    s.vd = vd;

    return s;
}

// A stop 1 mm before an equiconvex lens: R 100, 1 mm thick, n 1.5, so a thin-lens focal length
// of 1 / ((n - 1) (1 / R1 - 1 / R2)) = 100 mm.
Prescription
biconvex()
{
    Prescription lens;
    lens.surfaces.push_back(surface(SurfaceType::Stop, 0., 1., 10.));
    lens.surfaces.push_back(surface(SurfaceType::Standard, 100., 1., 12., 1.5, 60.));
    lens.surfaces.push_back(surface(SurfaceType::Standard, -100., 100., 12.));

    return lens;
}

Prescription
load(const char* path)
{
    Prescription lens;
    LensFileError error;
    if (!loadLensFile(path, &lens, &error)) {
        ADD_FAILURE() << error.text();
    }

    return lens;
}

// Where a meridional ray in x-z crosses the axis.
double
axisCrossingZ(const Ray& ray)
{
    return ray.position.z - ray.position.x * ray.direction.z / ray.direction.x;
}

const GlassCatalogue kNoCatalogue;

} // namespace

TEST(OpticsTrace, ParaxialRayMeetsTheThinLensFocus)
{
    const TraceSystem system(biconvex(), kNoCatalogue, std::string(), 0., { kWavelengthDNm });
    const TraceResult r = system.traceSequential(Ray { Vec3 { 0.05, 0., -1. }, Vec3 { 0., 0., 1. } }, 0);
    ASSERT_TRUE(r.valid);
    // From the lens's mid-plane, z = 1.5.
    EXPECT_NEAR(axisCrossingZ(r.ray) - 1.5, 100., 0.5);
}

TEST(OpticsTrace, OnAxisRaysPassEveryBuiltinUndeviated)
{
    for (const std::string& id : builtinLensIds()) {
        Prescription lens;
        ASSERT_TRUE(builtinLens(id, &lens));
        const TraceSystem system(lens, kNoCatalogue, std::string(), 0., { 450., 550., 650. });
        for (int w = 0; w < 3; ++w) {
            const TraceResult r = system.traceSequential(system.rayAtFrontSurface(0., 0., Vec3 { 0., 0., 1. }), w);
            ASSERT_TRUE(r.valid) << id;
            EXPECT_NEAR(r.ray.position.x, 0., 1e-12) << id;
            EXPECT_NEAR(r.ray.position.y, 0., 1e-12) << id;
            EXPECT_NEAR(r.ray.direction.z, 1., 1e-12) << id;
            EXPECT_LT(r.rrel, 1e-12) << id;
        }
    }
}

TEST(OpticsTrace, AsphereNewtonSolveConverges)
{
    const Prescription lens = load(kZoom);
    const Surface& s = lens.surfaces[0];
    ASSERT_EQ(s.type, SurfaceType::EvenAsphere);
    const TraceSystem system(lens, kNoCatalogue, std::string(), 0., { kWavelengthDNm });
    for (double h = 0.; h <= 11.; h += 1.) {
        const Ray ray { Vec3 { h, 0.5 * h, -2. }, Vec3 { 0.05, -0.02, 1. } };
        SurfaceHit hit;
        ASSERT_TRUE(system.intersect(0, ray, &hit)) << h;
        const double r = std::hypot(hit.position.x, hit.position.y);
        const double c = s.curvature;
        double sag = c * r * r / (1. + std::sqrt(1. - (1. + s.conic) * c * c * r * r));
        for (int i = 0; i < kMaxAsphericCoefficients; ++i) {
            sag += s.coefficients[i] * std::pow(r, 2 * i + 2);
        }
        EXPECT_NEAR(hit.position.z, sag, 1e-6) << h;
    }
    const TraceResult r = system.traceSequential(system.rayAtFrontSurface(1., 0.5, Vec3 { 0., 0., 1. }), 0);
    EXPECT_TRUE(r.valid);
}

TEST(OpticsTrace, UseAsphericOffTracesTheVertexSphere)
{
    const Prescription lens = load(kZoom);
    TraceSystem system(lens, kNoCatalogue, std::string(), 0., { kWavelengthDNm });
    const Ray ray { Vec3 { 10., 0., -2. }, Vec3 { 0., 0., 1. } };
    SurfaceHit aspheric, spherical;
    ASSERT_TRUE(system.intersect(0, ray, &aspheric));
    system.setUseAspheric(false);
    ASSERT_TRUE(system.intersect(0, ray, &spherical));
    const double radius = 1. / lens.surfaces[0].curvature;
    EXPECT_NEAR(spherical.position.z, radius - std::sqrt(radius * radius - 100.), 1e-9);
    EXPECT_GT(std::fabs(aspheric.position.z - spherical.position.z), 1e-3);
}

TEST(OpticsTrace, TenSurfaceLensGhostPairs)
{
    Prescription lens;
    for (int i = 0; i < 10; ++i) {
        lens.surfaces.push_back(surface(i == 4 ? SurfaceType::Stop : SurfaceType::Standard, 50., 2., 10.));
    }
    const std::vector<GhostPath> ghosts = enumerateGhosts(lens);
    // 4 surfaces before the stop and 5 after: C(4, 2) + C(5, 2).
    EXPECT_EQ(ghosts.size(), 16u);
    for (const GhostPath& g : ghosts) {
        EXPECT_LT(g.bounce2, g.bounce1);
        EXPECT_NE(g.bounce1, 4);
        EXPECT_NE(g.bounce2, 4);
        EXPECT_EQ(g.bounce1 < 4, g.bounce2 < 4);
    }
}

TEST(OpticsTrace, RayPastTheClearApertureHasRrelAboveOne)
{
    const Prescription lens = load(kTriplet);
    const TraceSystem system(lens, kNoCatalogue, std::string(), 0., { kWavelengthDNm });
    const double semi = lens.surfaces[0].semiDiameter;
    const TraceResult outside = system.traceSequential(system.rayAtFrontSurface(1.05 * semi, 0., Vec3 { 0., 0., 1. }), 0);
    EXPECT_GT(outside.rrel, 1.);
    const TraceResult inside = system.traceSequential(system.rayAtFrontSurface(0.1 * semi, 0., Vec3 { 0., 0., 1. }), 0);
    ASSERT_TRUE(inside.valid);
    EXPECT_LT(inside.rrel, 1.);
    EXPECT_LT(std::hypot(inside.stopX, inside.stopY), 1.);
}

TEST(OpticsTrace, ImagePlaneIsTheFilesImageDistance)
{
    const Prescription lens = load(kTriplet);
    const TraceSystem system(lens, kNoCatalogue, std::string(), 0., { kWavelengthDNm });
    EXPECT_DOUBLE_EQ(system.imagePlaneZ(), lens.layout().back().vertexZ + 41.2372);
}

TEST(OpticsTrace, TotalInternalReflectionInvalidates)
{
    // A ray parallel to the axis at height 10 meets the R -12 exit surface at asin(10 / 12) = 56
    // degrees, past the 41.8 degree critical angle of n 1.5.
    Prescription lens;
    lens.surfaces.push_back(surface(SurfaceType::Stop, 0., 1., 12.));
    lens.surfaces.push_back(surface(SurfaceType::Standard, 0., 5., 12., 1.5, 60.));
    lens.surfaces.push_back(surface(SurfaceType::Standard, -12., 10., 12.));
    const TraceSystem system(lens, kNoCatalogue, std::string(), 0., { kWavelengthDNm });
    EXPECT_FALSE(system.traceSequential(Ray { Vec3 { 10., 0., -1. }, Vec3 { 0., 0., 1. } }, 0).valid);
    EXPECT_TRUE(system.traceSequential(Ray { Vec3 { 2., 0., -1. }, Vec3 { 0., 0., 1. } }, 0).valid);
}

TEST(OpticsCoating, UncoatedNormalIncidenceIsFresnel)
{
    const double n1 = 1., n2 = 1.62;
    const double expected = std::pow((n1 - n2) / (n1 + n2), 2.);
    EXPECT_NEAR(surfaceReflectance(Coating(), 0., 550., n1, n2), expected, 1e-6);
    EXPECT_NEAR(surfaceReflectance(Coating(), 0., 550., n2, n1), expected, 1e-6);
}

TEST(OpticsCoating, QuarterWaveCoatingReflectsLessThanUncoated)
{
    const Coating mgf2 { 550., 1.38 };
    const double coated = surfaceReflectance(mgf2, 0., 550., 1., 1.52);
    const double uncoated = surfaceReflectance(Coating(), 0., 550., 1., 1.52);
    EXPECT_LT(coated, uncoated);
    EXPECT_GT(coated, 0.);
    // Away from its design wavelength the film cancels less.
    EXPECT_LT(coated, surfaceReflectance(mgf2, 0., 400., 1., 1.52));
}

TEST(OpticsCoating, GenerationIsSeededAndSkipsTheStop)
{
    const Prescription lens = load(kTriplet);
    const Range wavelengths { 400., 700. };
    const Range iors { 1.38, 1.9 };
    const std::vector<Coating> a = generateCoatings(lens, 7, wavelengths, iors);
    const std::vector<Coating> b = generateCoatings(lens, 7, wavelengths, iors);
    const std::vector<Coating> c = generateCoatings(lens, 8, wavelengths, iors);
    ASSERT_EQ(a.size(), lens.surfaces.size());
    bool differs = false;
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].wavelengthNm, b[i].wavelengthNm);
        EXPECT_EQ(a[i].ior, b[i].ior);
        differs = differs || a[i].ior != c[i].ior;
        if (lens.surfaces[i].type == SurfaceType::Stop) {
            EXPECT_FALSE(a[i].present());
            continue;
        }
        // Every other triplet surface separates air and glass.
        ASSERT_TRUE(a[i].present()) << i;
        EXPECT_GE(a[i].wavelengthNm, 400.);
        EXPECT_LE(a[i].wavelengthNm, 700.);
        EXPECT_EQ(a[i].wavelengthNm, std::round(a[i].wavelengthNm));
        EXPECT_GE(a[i].ior, 1.38);
        EXPECT_LT(a[i].ior, 1.9);
    }
    EXPECT_TRUE(differs);
}

TEST(OpticsCoating, SameMediumSurfacesGetNone)
{
    Prescription lens;
    lens.surfaces.push_back(surface(SurfaceType::Standard, 50., 2., 10., 1.6, 40.));
    lens.surfaces.push_back(surface(SurfaceType::Standard, -50., 2., 10., 1.5, 60.));
    lens.surfaces.push_back(surface(SurfaceType::Stop, 0., 2., 10.));
    lens.surfaces.push_back(surface(SurfaceType::Standard, 0., 2., 10.));
    const std::vector<Coating> c = generateCoatings(lens, 1, Range { 500., 600. }, Range { 1.38, 1.6 });
    EXPECT_TRUE(c[0].present());
    EXPECT_FALSE(c[1].present());
    EXPECT_FALSE(c[3].present());
}

TEST(OpticsCoating, GhostCarriesTheProductOfItsReflectances)
{
    const TraceSystem system(biconvex(), kNoCatalogue, std::string(), 0., { kWavelengthDNm });
    const std::vector<GhostPath> ghosts = enumerateGhosts(system.prescription());
    ASSERT_EQ(ghosts.size(), 1u);
    EXPECT_EQ(ghosts[0], (GhostPath { 2, 1 }));
    const TraceResult r = system.traceGhost(Ray { Vec3 { 0., 0., -1. }, Vec3 { 0., 0., 1. } }, 0, 2, 1);
    ASSERT_TRUE(r.valid);
    const double n = system.ior(1, 0);
    const double single = std::pow((n - 1.) / (n + 1.), 2.);
    EXPECT_NEAR(r.reflectance, single * single, 1e-9);
    EXPECT_NEAR(r.ray.direction.z, 1., 1e-12);
    EXPECT_DOUBLE_EQ(system.traceSequential(Ray { Vec3 { 0., 0., -1. }, Vec3 { 0., 0., 1. } }, 0).reflectance, 1.);
}

TEST(OpticsParaxial, DoubleGaussFocalLengthMatchesItsCitation)
{
    Prescription lens;
    ASSERT_TRUE(builtinLens("dgauss50", &lens));
    ParaxialFacts f;
    std::string error;
    ASSERT_TRUE(paraxialFacts(lens, &f, &error)) << error;
    EXPECT_NEAR(f.effectiveFocalLength, 50., 0.5);
    EXPECT_GT(f.backFocalDistance, 0.);
}

TEST(OpticsParaxial, FNumberMatchesTheFileWithinFivePercent)
{
    const Prescription lens = load(kTriplet);
    ASSERT_DOUBLE_EQ(lens.fNumber, 4.);
    ParaxialFacts f;
    std::string error;
    ASSERT_TRUE(paraxialFacts(lens, &f, &error)) << error;
    EXPECT_NEAR(f.paraxialFNumber, lens.fNumber, 0.05 * lens.fNumber);
    EXPECT_DOUBLE_EQ(f.fNumber, 4.);
    EXPECT_NEAR(f.effectiveFocalLength, 50., 0.5);
}

TEST(OpticsParaxial, FocusingFollowsNewton)
{
    const Prescription lens = load(kTriplet);
    ParaxialFacts f;
    std::string error;
    ASSERT_TRUE(paraxialFacts(lens, &f, &error)) << error;
    double imageZ = 0.;
    ASSERT_TRUE(focusTo(lens, 5000., &imageZ, &error)) << error;
    // x x' = -f^2, x from the front focal point to the object, x' from the back focal point.
    const double x = (f.entrancePupilZ - 5000.) - f.frontFocusZ;
    const double expected = -f.effectiveFocalLength * f.effectiveFocalLength / x;
    const double shift = imageZ - f.backFocusZ;
    EXPECT_GT(shift, 0.);
    EXPECT_NEAR(shift, expected, 0.01 * expected);
}

TEST(OpticsParaxial, ThinLensFacts)
{
    ParaxialFacts f;
    std::string error;
    ASSERT_TRUE(paraxialFacts(biconvex(), &f, &error)) << error;
    // The thick-lens formula: 1 / f = (n - 1) (2 / R - (n - 1) t / (n R^2)).
    const double n = 1.5, R = 100., t = 1.;
    EXPECT_NEAR(f.effectiveFocalLength, 1. / ((n - 1.) * (2. / R - (n - 1.) * t / (n * R * R))), 1e-9);
    // The stop is the first surface, so it is its own entrance pupil.
    EXPECT_NEAR(f.entrancePupilZ, 0., 1e-12);
    EXPECT_NEAR(f.entrancePupilSemiDiameter, 10., 1e-12);
}

TEST(OpticsParaxial, FocusingInsideTheFocalLengthFails)
{
    double z = 0.;
    std::string error;
    EXPECT_FALSE(focusTo(biconvex(), 50., &z, &error));
    EXPECT_FALSE(error.empty());
}
