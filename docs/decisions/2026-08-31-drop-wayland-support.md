# Wayland support is dropped, not silently disabled

`NATRON_ENABLE_WAYLAND` and its `find_package(ECM NO_MODULE)` are removed, or
documented as unsupported on this image — the option does not survive as a
switch that quietly does nothing.

Today it is a no-op: `extra-cmake-modules` is not in `aswf/ci-vfxall`, EPEL is
unreachable from the build environment, and the `find_package` call is not
`REQUIRED`, so Wayland detection fails silently and `OSGLContext_wayland.cpp`
is never activated. Anyone reading `CMakeLists.txt` would reasonably conclude
the fork supports Wayland behind a flag. It does not.

M7 prototyped a from-source ECM build and confirmed it works, so this is a
choice about scope, not feasibility: carrying another from-source dependency to
enable a path nothing currently exercises is not worth it now. A silent no-op
is the worst of the three available states — worse than absent, because it
misleads.

Reversible: the M7 prototype is the starting point if Wayland becomes real work
later.
