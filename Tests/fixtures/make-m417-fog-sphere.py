#!/usr/bin/env python3
# Regenerates the OpenVDB fixture m417_vdb_primitive.py reads:
#
#   python3 Tests/fixtures/make-m417-fog-sphere.py Tests/fixtures
#
# m417-fog-sphere.vdb: a fog volume sphere of radius 1 centred at the origin, in voxels of 1/8, with
# one float grid named "density": 1 inside, falling smoothly to 0 over the outer fifth of the radius.
# Floats are saved as half and the file is zip-compressed, a few tens of KB.
#
# The image has no OpenVDB Python module, so this compiles a small writer against the image's
# libopenvdb and runs it. Run it inside the dev container (tools/ci/local/devshell.sh).
import os
import shutil
import subprocess
import sys
import tempfile

NAME = "m417-fog-sphere.vdb"

WRITER = r"""
#include <cmath>

#include <openvdb/openvdb.h>

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    const double voxel = 1.0 / 8.0;
    const double radius = 1.0;
    const int half = (int)std::ceil(radius / voxel) + 1;

    openvdb::initialize();
    openvdb::FloatGrid::Ptr density = openvdb::FloatGrid::create(0.f);
    openvdb::FloatGrid::Accessor d = density->getAccessor();
    for (int i = -half; i <= half; ++i) {
        for (int j = -half; j <= half; ++j) {
            for (int k = -half; k <= half; ++k) {
                const double r = std::sqrt((double)(i * i + j * j + k * k)) * voxel;
                if (r >= radius) {
                    continue;
                }
                const double edge = 0.8 * radius;
                double falloff = 1.0;
                if (r > edge) {
                    const double s = (r - edge) / (radius - edge);
                    falloff = 1.0 - s * s * (3.0 - 2.0 * s);
                }
                d.setValue(openvdb::Coord(i, j, k), (float)falloff);
            }
        }
    }
    density->setName("density");
    density->setGridClass(openvdb::GRID_FOG_VOLUME);
    density->setTransform(openvdb::math::Transform::createLinearTransform(voxel));
    density->setSaveFloatAsHalf(true);
    density->pruneGrid();
    openvdb::GridPtrVec grids;
    grids.push_back(density);
    openvdb::io::File file(argv[1]);
    file.setCompression(openvdb::io::COMPRESS_ZIP | openvdb::io::COMPRESS_ACTIVE_MASK);
    file.write(grids);
    file.close();
    return 0;
}
"""


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
        path = os.path.join(directory, NAME)
        subprocess.check_call([binary, path])
        print("%s: %d bytes" % (path, os.path.getsize(path)))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make-m417-fog-sphere.py <output directory>")
    main(sys.argv[1])
