#!/usr/bin/env python3
# Writes a deforming mesh, cubic and linear curves, points and a rigid moving card under one held
# Geometry projection, for comparing renders across frames:
#   reader  the surface reads each prim's reference points through a UsdPrimvarReader_float3
#   baked   each prim has st baked from the same reference points under a plain textured surface:
#           the reference for where the image should be
# usage: gen_sticky.py <out file> <plate image> <glslfx> <reader|baked>
import math
import os
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade, Vt

out_file, plate, shader_path, mode = sys.argv[1:5]
plate, shader_path = os.path.abspath(plate), os.path.abspath(shader_path)
aspect = 16.0 / 9.0
FRAME = "natronProjectionFrameHold1"
REFERENCE = "natronReferencePointsHold1"
NO_REFERENCE = 1e30
BARE_WIDTHS = os.environ.get("SPIKE_BARE_WIDTHS") == "1"

stage = Usd.Stage.CreateNew(out_file)
stage.SetStartTimeCode(1)
stage.SetEndTimeCode(2)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)


def camera(path, z):
    c = UsdGeom.Camera.Define(stage, path)
    c.CreateFocalLengthAttr(18.0)
    c.CreateHorizontalApertureAttr(36.0)
    c.CreateVerticalApertureAttr(36.0 / aspect)
    c.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
    c.AddTranslateOp().Set(Gf.Vec3d(0, 0, z))
    return c


projector = camera("/World/Projector", 6)
camera("/World/View", 9)
to_uv = Gf.Matrix4d(0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 1, 0, 0.5, 0.5, 0, 1)
frustum = projector.GetCamera(1).frustum
world_to_projector = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix() * to_uv


def plate_texture(path):
    texture = UsdShade.Shader.Define(stage, path + "/Plate")
    texture.CreateIdAttr("UsdUVTexture")
    texture.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(Sdf.AssetPath(plate))
    texture.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("black")
    texture.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("black")
    st = UsdShade.Shader.Define(stage, path + "/St")
    st.CreateIdAttr("UsdPrimvarReader_float2")
    st.CreateInput("varname", Sdf.ValueTypeNames.String).Set("st")
    texture.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(st.ConnectableAPI(), "result")
    return texture


def projection_material(path):
    result = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/Project3D")
    shader.SetSourceAsset(Sdf.AssetPath(shader_path), "glslfx")
    shader.CreateInput("worldToProjector", Sdf.ValueTypeNames.Matrix4d).Set(world_to_projector)
    shader.CreateInput("projectorOrigin", Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(0, 0, 6, 1))
    shader.CreateInput("projectionSpace", Sdf.ValueTypeNames.Int).Set(1)
    shader.CreateInput("crop", Sdf.ValueTypeNames.Int).Set(1)
    shader.CreateInput("projectOn", Sdf.ValueTypeNames.Int).Set(int(os.environ.get("SPIKE_PROJECT_ON", "0")))
    texture = plate_texture(path)
    texture.CreateOutput("rgba", Sdf.ValueTypeNames.Float4)
    shader.CreateInput("plate", Sdf.ValueTypeNames.Float4).ConnectToSource(texture.ConnectableAPI(), "rgba")
    frame = UsdShade.Shader.Define(stage, path + "/Frame")
    frame.CreateIdAttr("UsdPrimvarReader_matrix")
    frame.CreateInput("varname", Sdf.ValueTypeNames.String).Set(FRAME)
    frame.CreateInput("fallback", Sdf.ValueTypeNames.Matrix4d).Set(Gf.Matrix4d(1))
    frame.CreateOutput("result", Sdf.ValueTypeNames.Matrix4d)
    shader.CreateInput("geometryFrame", Sdf.ValueTypeNames.Matrix4d).ConnectToSource(frame.ConnectableAPI(), "result")
    points = UsdShade.Shader.Define(stage, path + "/Points")
    points.CreateIdAttr("UsdPrimvarReader_float3")
    points.CreateInput("varname", Sdf.ValueTypeNames.String).Set(REFERENCE)
    points.CreateInput("fallback", Sdf.ValueTypeNames.Float3).Set(Gf.Vec3f(NO_REFERENCE))
    points.CreateOutput("result", Sdf.ValueTypeNames.Float3)
    shader.CreateInput("referencePoints", Sdf.ValueTypeNames.Float3).ConnectToSource(points.ConnectableAPI(), "result")
    result.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    return result


def baked_material(path):
    result = UsdShade.Material.Define(stage, path)
    surface = UsdShade.Shader.Define(stage, path + "/Surface")
    surface.CreateIdAttr("UsdPreviewSurface")
    surface.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0))
    surface.CreateInput("specularColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0))
    surface.CreateInput("useSpecularWorkflow", Sdf.ValueTypeNames.Int).Set(1)
    texture = plate_texture(path)
    texture.CreateOutput("rgb", Sdf.ValueTypeNames.Float3)
    surface.CreateInput("emissiveColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(texture.ConnectableAPI(), "rgb")
    result.CreateSurfaceOutput().ConnectToSource(surface.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    return result


shared = projection_material("/World/Looks/Shared") if mode == "reader" else baked_material("/World/Looks/Baked")


def grid_points(cx, cy, size, n, frame):
    wave = 0.0 if frame == 1 else 0.6
    out = []
    for j in range(n + 1):
        for i in range(n + 1):
            u, v = i / n, j / n
            out.append(Gf.Vec3f(cx + size * (u - 0.5) + wave * v, cy + size * (v - 0.5) + wave * math.sin(u * 3.14), wave * u))
    return out


def author_reference(prim, points):
    if mode == "reader":
        UsdGeom.PrimvarsAPI(prim).CreatePrimvar(REFERENCE, Sdf.ValueTypeNames.Point3fArray, UsdGeom.Tokens.vertex).Set(Vt.Vec3fArray(points))
    else:
        uvs = []
        for p in points:
            c = Gf.Vec4d(p[0], p[1], p[2], 1) * world_to_projector
            uvs.append(Gf.Vec2f(c[0] / c[3], c[1] / c[3]))
        UsdGeom.PrimvarsAPI(prim).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex).Set(Vt.Vec2fArray(uvs))
    UsdShade.MaterialBindingAPI.Apply(prim).Bind(shared)


N = 12
mesh = UsdGeom.Mesh.Define(stage, "/World/Mesh")
mesh.CreateFaceVertexCountsAttr([4] * (N * N))
indices = []
for j in range(N):
    for i in range(N):
        a = j * (N + 1) + i
        indices += [a, a + 1, a + N + 2, a + N + 1]
mesh.CreateFaceVertexIndicesAttr(indices)
mesh.CreateDoubleSidedAttr(True)
mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.catmullClark if BARE_WIDTHS else UsdGeom.Tokens.none)
for frame in (1, 2):
    mesh.CreatePointsAttr().Set(grid_points(-3.5, 1.2, 2.5, N, frame), frame)
author_reference(mesh.GetPrim(), grid_points(-3.5, 1.2, 2.5, N, 1))


def curve_points(y, frame):
    wave = 0.0 if frame == 1 else 0.5
    return [Gf.Vec3f(-1.5 + 0.5 * i, y + wave * math.sin(i), wave * 0.2 * i) for i in range(7)]


for name, kind, basis, y in (("Linear", "linear", None, 1.8), ("Cubic", "cubic", "bspline", 0.6)):
    curves = UsdGeom.BasisCurves.Define(stage, "/World/" + name)
    curves.CreateTypeAttr(kind)
    if basis:
        curves.CreateBasisAttr(basis)
    curves.CreateCurveVertexCountsAttr([7])
    if not BARE_WIDTHS:
        curves.CreateWidthsAttr([0.35])
        curves.SetWidthsInterpolation(UsdGeom.Tokens.constant)
    for frame in (1, 2):
        curves.CreatePointsAttr().Set(curve_points(y, frame), frame)
    author_reference(curves.GetPrim(), curve_points(y, 1))

cloud = UsdGeom.Points.Define(stage, "/World/Cloud")
if not BARE_WIDTHS:
    cloud.CreateWidthsAttr([0.3])
    cloud.SetWidthsInterpolation(UsdGeom.Tokens.constant)


def cloud_points(frame):
    wave = 0.0 if frame == 1 else 0.5
    return [Gf.Vec3f(2.0 + 0.4 * i, 1.8 - 0.4 * j + wave * (i - 2) * 0.2, wave * 0.2) for j in range(5) for i in range(5)]


for frame in (1, 2):
    cloud.CreatePointsAttr().Set(cloud_points(frame), frame)
author_reference(cloud.GetPrim(), cloud_points(1))

# Rigid, in the same material: no reference points, so it takes the frame primvar's path.
rigid = UsdGeom.Mesh.Define(stage, "/World/Rigid")
rigid.CreatePointsAttr([(-1.25, -1.25, 0), (1.25, -1.25, 0), (1.25, 1.25, 0), (-1.25, 1.25, 0)])
rigid.CreateFaceVertexCountsAttr([4])
rigid.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
rigid.CreateDoubleSidedAttr(True)
rigid.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
move = rigid.AddTranslateOp()
move.Set(Gf.Vec3d(0, -2.0, 0), 1)
move.Set(Gf.Vec3d(0.75, -2.0, 0), 2)
UsdShade.MaterialBindingAPI.Apply(rigid.GetPrim()).Bind(shared)
held = Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0, -2.0, 0))
if mode == "reader":
    UsdGeom.PrimvarsAPI(rigid.GetPrim()).CreatePrimvar(FRAME, Sdf.ValueTypeNames.Matrix4d, UsdGeom.Tokens.constant).Set(held)
else:
    uvs = []
    for p in rigid.GetPointsAttr().Get():
        c = Gf.Vec4d(p[0], p[1], p[2], 1) * held * world_to_projector
        uvs.append(Gf.Vec2f(c[0] / c[3], c[1] / c[3]))
    UsdGeom.PrimvarsAPI(rigid.GetPrim()).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex).Set(Vt.Vec2fArray(uvs))
stage.GetRootLayer().Save()
