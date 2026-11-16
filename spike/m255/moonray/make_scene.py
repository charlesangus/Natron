"""Writes scene.usda: the Cycles deep-probe scene, plus a MoonRay network on every material.

UsdPreviewSurface opacity in MoonRay drives refraction, not presence, so without the
DwaBaseMaterial network the cards would be opaque refractors and never give deep alpha below 1.
"""

import math
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade


def bind(stage, prim, name, colour, opacity):
    path = "/World/mtl/" + name
    mat = UsdShade.Material.Define(stage, path)
    surf = UsdShade.Shader.Define(stage, path + "/surface")
    surf.CreateIdAttr("UsdPreviewSurface")
    surf.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*colour))
    surf.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.6)
    surf.CreateInput("opacity", Sdf.ValueTypeNames.Float).Set(opacity)
    mat.CreateSurfaceOutput().ConnectToSource(surf.ConnectableAPI(), "surface")
    dwa = UsdShade.Shader.Define(stage, path + "/dwa")
    dwa.CreateIdAttr("DwaBaseMaterial")
    dwa.CreateInput("albedo", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*colour))
    dwa.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.6)
    dwa.CreateInput("presence", Sdf.ValueTypeNames.Float).Set(opacity)
    dwa.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    mat.CreateSurfaceOutput("moonray").ConnectToSource(dwa.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(prim).Bind(mat)


def add_card(stage, name, centre, half, colour, opacity):
    mesh = UsdGeom.Mesh.Define(stage, "/World/" + name)
    x, y, z = centre
    mesh.CreatePointsAttr([(x - half, y - half, z), (x + half, y - half, z),
                           (x + half, y + half, z), (x - half, y + half, z)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    mesh.CreateDoubleSidedAttr(True)
    bind(stage, mesh.GetPrim(), name, colour, opacity)


def add_sphere(stage, centre, radius):
    rings, segments = 24, 48
    points, counts, indices = [], [], []
    for r in range(rings + 1):
        theta = math.pi * r / rings
        for s in range(segments):
            phi = 2.0 * math.pi * s / segments
            points.append(Gf.Vec3f(centre) + radius * Gf.Vec3f(math.sin(theta) * math.cos(phi), math.cos(theta),
                                                               math.sin(theta) * math.sin(phi)))
    for r in range(rings):
        for s in range(segments):
            a, b = r * segments + s, r * segments + (s + 1) % segments
            counts.append(4)
            indices += [a, b, b + segments, a + segments]
    mesh = UsdGeom.Mesh.Define(stage, "/World/sphere")
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr(counts)
    mesh.CreateFaceVertexIndicesAttr(indices)
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    bind(stage, mesh.GetPrim(), "sphere", (0.8, 0.8, 0.75), 1.0)


def main(path):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.Xform.Define(stage, "/World")
    add_sphere(stage, (0.3, 0.0, -1.0), 1.0)
    add_card(stage, "cardNear", (-0.7, 0.4, 1.5), 1.0, (0.9, 0.15, 0.1), 0.5)
    add_card(stage, "cardMid", (0.5, -0.4, 0.6), 1.1, (0.1, 0.3, 0.9), 0.4)
    add_card(stage, "cardFar", (-0.4, -0.2, -2.6), 1.4, (0.1, 0.8, 0.2), 0.7)
    sun = UsdLux.DistantLight.Define(stage, "/World/sun")
    sun.CreateIntensityAttr(3.0)
    UsdGeom.XformCommonAPI(sun.GetPrim()).SetRotate(Gf.Vec3f(-35, 25, 0))
    lamp = UsdLux.SphereLight.Define(stage, "/World/lamp")
    lamp.CreateIntensityAttr(60.0)
    lamp.CreateRadiusAttr(0.3)
    UsdGeom.XformCommonAPI(lamp.GetPrim()).SetTranslate(Gf.Vec3d(1.5, 2.5, 4))
    cam = UsdGeom.Camera.Define(stage, "/World/cam")
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
    UsdGeom.XformCommonAPI(cam.GetPrim()).SetTranslate(Gf.Vec3d(0, 0, 8))
    stage.SetDefaultPrim(stage.GetPrimAtPath("/World"))
    stage.GetRootLayer().Save()


if __name__ == "__main__":
    main(sys.argv[1])
