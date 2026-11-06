#!/usr/bin/env python3
# Writes the spike scene: two 4x4 cards, one tilted 50 degrees about Y facing the projector and one
# facing away (front faces only at frame 1, back faces only at frame 2), a render camera, and a material using Engine/Nodes/Scene/shaders/natronProject3D.glslfx
# whose worldToProjector is time-sampled from an animated projector camera (frames 1 and 2).
# usage: gen.py <out dir> <plate image>
import os
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade

out_dir, plate = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
here = os.path.dirname(os.path.abspath(__file__))
shader_path = os.path.normpath(os.path.join(here, "../../../Engine/Nodes/Scene/shaders/natronProject3D.glslfx"))
aspect = 16.0 / 9.0

stage = Usd.Stage.CreateNew(os.path.join(out_dir, "scene.usda"))
stage.SetStartTimeCode(1)
stage.SetEndTimeCode(2)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)

def card(path, rotate_y, translate):
    xf = UsdGeom.Xform.Define(stage, path)
    api = UsdGeom.XformCommonAPI(xf)
    api.SetTranslate(Gf.Vec3d(*translate))
    api.SetRotate(Gf.Vec3f(0, rotate_y, 0))
    mesh = UsdGeom.Mesh.Define(stage, path + "/Mesh")
    mesh.CreatePointsAttr([(-2, -2, 0), (2, -2, 0), (2, 2, 0), (-2, 2, 0)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateDoubleSidedAttr(True)
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    return mesh

front = card("/World/Front", 50, (0, 0, 0))
back = card("/World/Back", 180, (3, 0, -4))

projector = UsdGeom.Camera.Define(stage, "/World/Projector")
projector.CreateFocalLengthAttr(35.0)
projector.CreateHorizontalApertureAttr(36.0)
projector.CreateVerticalApertureAttr(36.0 / aspect)
projector.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
pxf = projector.AddTranslateOp()
pxf.Set(Gf.Vec3d(0, 0, 6), 1)
pxf.Set(Gf.Vec3d(0.8, 0.3, 6), 2)

view = UsdGeom.Camera.Define(stage, "/World/View")
view.CreateFocalLengthAttr(30.0)
view.CreateHorizontalApertureAttr(36.0)
view.CreateVerticalApertureAttr(36.0 / aspect)
view.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
look = Gf.Matrix4d().SetLookAt(Gf.Vec3d(7, 3, 9), Gf.Vec3d(2, 0, 0), Gf.Vec3d(0, 1, 0)).GetInverse()
view.AddTransformOp().Set(look)

# x and y of clip space from -1..1 to 0..1, w kept (row vectors).
to_uv = Gf.Matrix4d(0.5, 0, 0, 0,
                    0, 0.5, 0, 0,
                    0, 0, 1, 0,
                    0.5, 0.5, 0, 1)

def project(material_path, project_on):
    material = UsdShade.Material.Define(stage, material_path)
    shader = UsdShade.Shader.Define(stage, material_path + "/Project3D")
    shader.SetSourceAsset(Sdf.AssetPath(shader_path), "glslfx")
    matrix = shader.CreateInput("worldToProjector", Sdf.ValueTypeNames.Matrix4d)
    origin = shader.CreateInput("projectorOrigin", Sdf.ValueTypeNames.Float4)
    for frame in (1, 2):
        cam = UsdGeom.Camera(projector.GetPrim()).GetCamera(frame)
        frustum = cam.frustum
        matrix.Set(frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix() * to_uv, frame)
        pos = frustum.GetPosition()
        origin.Set(Gf.Vec4f(pos[0], pos[1], pos[2], 1), frame)
    on = shader.CreateInput("projectOn", Sdf.ValueTypeNames.Int)
    for frame, value in project_on:
        on.Set(value, frame)
    shader.CreateInput("crop", Sdf.ValueTypeNames.Int).Set(1)
    shader.CreateInput("clip", Sdf.ValueTypeNames.Int).Set(0)
    shader.CreateInput("outside", Sdf.ValueTypeNames.Int).Set(0)
    texture = UsdShade.Shader.Define(stage, material_path + "/Plate")
    texture.CreateIdAttr("UsdUVTexture")
    texture.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(Sdf.AssetPath(plate))
    texture.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set("auto")
    texture.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("black")
    texture.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("black")
    st = UsdShade.Shader.Define(stage, material_path + "/St")
    st.CreateIdAttr("UsdPrimvarReader_float2")
    st.CreateInput("varname", Sdf.ValueTypeNames.String).Set("st")
    texture.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(st.ConnectableAPI(), "result")
    shader.CreateInput("plate", Sdf.ValueTypeNames.Float4).ConnectToSource(
        texture.ConnectableAPI(), "rgba")
    texture.CreateOutput("rgba", Sdf.ValueTypeNames.Float4)
    material.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    return material

both = project("/World/Looks/ProjectBoth", [(1, 0)])
# Front faces only at frame 1, back faces only at frame 2.
front_only = project("/World/Looks/ProjectFront", [(1, 1), (2, 2)])
UsdShade.MaterialBindingAPI.Apply(front.GetPrim()).Bind(both)
UsdShade.MaterialBindingAPI.Apply(back.GetPrim()).Bind(front_only)
stage.GetRootLayer().Save()
print(os.path.join(out_dir, "scene.usda"))
