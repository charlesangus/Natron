#!/usr/bin/env python3
# Writes two native instances of one card, each bound directly to one projection material on its
# instance root, with its own primvar on the root:
#   frame   a constant matrix natronProjectionFrame per instance (rigid placement)
#   sticky  a constant float3 natronReferencePoints per instance, over a prototype that deforms
#           at frame 2; instances share the prototype's points, so a vertex primvar cannot differ
#           per instance and the root can only carry one point
# usage: gen_instances.py <out file> <plate image> <glslfx> <frame|sticky>
import math
import os
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade, Vt

out_file, plate, shader_path, mode = sys.argv[1:5]
plate, shader_path = os.path.abspath(plate), os.path.abspath(shader_path)
aspect = 16.0 / 9.0
FRAME = "natronProjectionFrame"
REFERENCE = "natronReferencePoints"
NO_REFERENCE = 1e30

stage = Usd.Stage.CreateNew(out_file)
stage.SetStartTimeCode(1)
stage.SetEndTimeCode(2)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)


def camera(path, translate):
    c = UsdGeom.Camera.Define(stage, path)
    c.CreateFocalLengthAttr(18.0)
    c.CreateHorizontalApertureAttr(36.0)
    c.CreateVerticalApertureAttr(36.0 / aspect)
    c.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
    c.AddTranslateOp().Set(translate)
    return c


projector = camera("/World/Projector", Gf.Vec3d(0, 0, 6))
camera("/World/View", Gf.Vec3d(0, 0, 9))
to_uv = Gf.Matrix4d(0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 1, 0, 0.5, 0.5, 0, 1)
frustum = projector.GetCamera(1).frustum
world_to_projector = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix() * to_uv


def project_material(path):
    result = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/Project3D")
    shader.SetSourceAsset(Sdf.AssetPath(shader_path), "glslfx")
    shader.CreateInput("worldToProjector", Sdf.ValueTypeNames.Matrix4d).Set(world_to_projector)
    shader.CreateInput("projectorOrigin", Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(0, 0, 6, 1))
    shader.CreateInput("projectionSpace", Sdf.ValueTypeNames.Int).Set(1)
    shader.CreateInput("crop", Sdf.ValueTypeNames.Int).Set(1)
    texture = UsdShade.Shader.Define(stage, path + "/Plate")
    texture.CreateIdAttr("UsdUVTexture")
    texture.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(Sdf.AssetPath(plate))
    texture.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("black")
    texture.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("black")
    st = UsdShade.Shader.Define(stage, path + "/St")
    st.CreateIdAttr("UsdPrimvarReader_float2")
    st.CreateInput("varname", Sdf.ValueTypeNames.String).Set("st")
    texture.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(st.ConnectableAPI(), "result")
    texture.CreateOutput("rgba", Sdf.ValueTypeNames.Float4)
    shader.CreateInput("plate", Sdf.ValueTypeNames.Float4).ConnectToSource(texture.ConnectableAPI(), "rgba")
    for name, kind, varname, fallback in (("Frame", "matrix", FRAME, Gf.Matrix4d(1)),
                                          ("Points", "float3", REFERENCE, Gf.Vec3f(NO_REFERENCE))):
        reader = UsdShade.Shader.Define(stage, path + "/" + name)
        reader.CreateIdAttr("UsdPrimvarReader_" + kind)
        reader.CreateInput("varname", Sdf.ValueTypeNames.String).Set(varname)
        type_name = Sdf.ValueTypeNames.Matrix4d if kind == "matrix" else Sdf.ValueTypeNames.Float3
        reader.CreateInput("fallback", type_name).Set(fallback)
        reader.CreateOutput("result", type_name)
        target = "geometryFrame" if kind == "matrix" else "referencePoints"
        shader.CreateInput(target, type_name).ConnectToSource(reader.ConnectableAPI(), "result")
    result.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    return result


material = project_material("/World/Looks/Shared")

UsdGeom.Xform.Define(stage, "/Proto")
stage.GetPrimAtPath("/Proto").SetSpecifier(Sdf.SpecifierClass)
card = UsdGeom.Mesh.Define(stage, "/Proto/Mesh")
card.CreateFaceVertexCountsAttr([4])
card.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
card.CreateDoubleSidedAttr(True)
card.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
flat = [(-1.5, -1.5, 0), (1.5, -1.5, 0), (1.5, 1.5, 0), (-1.5, 1.5, 0)]
card.CreatePointsAttr().Set(flat, 1)
if mode == "sticky":
    card.GetPointsAttr().Set([(-1.5, -1.5, 0), (1.5, -1.5, 0), (2.1, 2.1, 0.6), (-1.5, 1.5, 0)], 2)
else:
    card.GetPointsAttr().Set(flat, 2)

for name, x in (("Left", -3.0), ("Right", 3.0)):
    instance = UsdGeom.Xform.Define(stage, "/World/" + name)
    instance.GetPrim().GetReferences().AddInternalReference("/Proto")
    instance.AddTranslateOp().Set(Gf.Vec3d(x, 0, 0))
    instance.GetPrim().SetInstanceable(True)
    UsdShade.MaterialBindingAPI.Apply(instance.GetPrim()).Bind(material)
    primvars = UsdGeom.PrimvarsAPI(instance.GetPrim())
    if mode == "frame":
        frame = Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(x, 0, 0))
        primvars.CreatePrimvar(FRAME, Sdf.ValueTypeNames.Matrix4d, UsdGeom.Tokens.constant).Set(frame)
    else:
        # Instances at +-3 hold their own corner of the plate: left near its red/blue edge, right in the white corner's opposite.
        point = Gf.Vec3f(-2.2 if x < 0 else 2.2, 1.0 if x < 0 else -1.0, 0)
        primvars.CreatePrimvar(REFERENCE, Sdf.ValueTypeNames.Point3fArray, UsdGeom.Tokens.constant).Set(Vt.Vec3fArray([point]))
stage.GetRootLayer().Save()
