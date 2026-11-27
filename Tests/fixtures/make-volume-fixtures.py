#!/usr/bin/env python3
# Regenerates the OpenVDB fixtures VolumeFields_Test.cpp and VolumeRender_Test.cpp read:
#
#   python3 Tests/fixtures/make-volume-fixtures.py Tests/fixtures
#
# volume-sphere.0001.vdb and volume-sphere.0002.vdb: a sphere of radius 0.5 centred at x = -0.625 on
# frame 1 and x = +0.625 on frame 2 (y = z = 0), in 32^3 voxels of 1/24 around its centre, with
# three grids:
#   density      float, 4 inside, falling smoothly to 0 over the outer fifth of the radius
#   temperature  float, 1 at the centre, 0 at the surface
#   velocity     vec3s, (30, 0, 0) units per second wherever density is set: the 1.25 units the
#                sphere moves between the frames at 24 frames per second
# Floats are saved as half and the files are zip-compressed, a few tens of KB each.
#
# The image has no OpenVDB Python module, so this compiles a small writer against the image's
# libopenvdb and runs it. Run it inside the dev container (tools/ci/local/devshell.sh).
import os
import shutil
import subprocess
import sys
import tempfile

WRITER = r"""
#include <cmath>
#include <cstdlib>

#include <openvdb/openvdb.h>

int main(int argc, char** argv)
{
    if (argc != 3) {
        return 2;
    }
    const double centreX = std::atof(argv[2]);
    const double voxel = 1.0 / 24.0;
    const double radius = 0.5;
    const double speed = 30.0;
    const int half = 16;

    openvdb::initialize();
    openvdb::FloatGrid::Ptr density = openvdb::FloatGrid::create(0.f);
    openvdb::FloatGrid::Ptr temperature = openvdb::FloatGrid::create(0.f);
    openvdb::Vec3SGrid::Ptr velocity = openvdb::Vec3SGrid::create(openvdb::Vec3s(0.f));
    openvdb::FloatGrid::Accessor d = density->getAccessor();
    openvdb::FloatGrid::Accessor t = temperature->getAccessor();
    openvdb::Vec3SGrid::Accessor v = velocity->getAccessor();
    const int ci = (int)std::lround(centreX / voxel);
    for (int i = ci - half; i < ci + half; ++i) {
        for (int j = -half; j < half; ++j) {
            for (int k = -half; k < half; ++k) {
                const double x = i * voxel - centreX;
                const double y = j * voxel;
                const double z = k * voxel;
                const double r = std::sqrt(x * x + y * y + z * z);
                if (r >= radius) {
                    continue;
                }
                const double edge = 0.8 * radius;
                double falloff = 1.0;
                if (r > edge) {
                    const double s = (r - edge) / (radius - edge);
                    falloff = 1.0 - s * s * (3.0 - 2.0 * s);
                }
                const openvdb::Coord c(i, j, k);
                d.setValue(c, (float)(4.0 * falloff));
                t.setValue(c, (float)(1.0 - r / radius));
                v.setValue(c, openvdb::Vec3s((float)speed, 0.f, 0.f));
            }
        }
    }
    openvdb::GridPtrVec grids;
    density->setName("density");
    density->setGridClass(openvdb::GRID_FOG_VOLUME);
    temperature->setName("temperature");
    temperature->setGridClass(openvdb::GRID_FOG_VOLUME);
    velocity->setName("velocity");
    velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    for (openvdb::GridBase::Ptr grid : { openvdb::GridBase::Ptr(density), openvdb::GridBase::Ptr(temperature), openvdb::GridBase::Ptr(velocity) }) {
        grid->setTransform(openvdb::math::Transform::createLinearTransform(voxel));
        grid->setSaveFloatAsHalf(true);
        grid->pruneGrid();
        grids.push_back(grid);
    }
    openvdb::io::File file(argv[1]);
    file.setCompression(openvdb::io::COMPRESS_ZIP | openvdb::io::COMPRESS_ACTIVE_MASK);
    file.write(grids);
    file.close();

    return 0;
}
"""

FRAMES = ((1, -0.625), (2, 0.625))


def main(directory):
    prefix = os.environ.get("NATRON_OPENVDB_PREFIX", "/usr/local")
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        raise RuntimeError("no C++ compiler on PATH")
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "writer.cpp")
        binary = os.path.join(work, "writer")
        with open(source, "w") as f:
            f.write(WRITER)
        subprocess.check_call([
            compiler, "-std=c++17", "-O1", source, "-o", binary,
            "-I" + os.path.join(prefix, "include"),
            "-L" + os.path.join(prefix, "lib"),
            "-Wl,-rpath," + os.path.join(prefix, "lib"),
            "-lopenvdb", "-ltbb", "-lImath-3_2",
        ])
        for frame, centre in FRAMES:
            path = os.path.join(directory, "volume-sphere.%04d.vdb" % frame)
            subprocess.check_call([binary, path, repr(centre)])
            print("%s: %d bytes" % (path, os.path.getsize(path)))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make-volume-fixtures.py <output directory>")
    main(sys.argv[1])
