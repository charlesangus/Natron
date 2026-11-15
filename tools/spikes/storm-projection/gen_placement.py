#!/usr/bin/env python3
# Writes a scene with cards at different places under one Geometry projection, each carrying its
# placement the way mode says, for comparing renders:
#   material  one material per card, geometryFrame authored on each material (the reference)
#   metadata  one material; a constant matrix primvar per card, named by the glslfx "primvars" metadata
#   reader    one material; a constant matrix primvar per card, read by a UsdPrimvarReader_matrix
# instances: 0 plain meshes; 1 native instances with the binding inside the prototype; 2 native instances
# bound through a collection on their root; 3 native instances bound directly on each instance root.
# usage: gen_placement.py <out file> <plate image> <glslfx> <mode> <instances: 0|1|2|3>
import os
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade

out_file, plate, shader_path, mode, instances = sys.argv[1:6]
plate, shader_path = os.path.abspath(plate), os.path.abspath(shader_path)
instances = int(instances)
aspect = 16.0 / 9.0
PRIMVAR = "natronProjectionFrameHold1_5"

stage = Usd.Stage.CreateNew(out_file)
stage.SetStartTimeCode(1)
stage.SetEndTimeCode(2)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)

projector = UsdGeom.Camera.Define(stage, "/World/Projector")
projector.CreateFocalLengthAttr(18.0)
projector.CreateHorizontalApertureAttr(36.0)
projector.CreateVerticalApertureAttr(36.0 / aspect)
projector.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
projector.AddTranslateOp().Set(Gf.Vec3d(0, 0, 6))

view = UsdGeom.Camera.Define(stage, "/World/View")
view.CreateFocalLengthAttr(18.0)
view.CreateHorizontalApertureAttr(36.0)
view.CreateVerticalApertureAttr(36.0 / aspect)
view.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
view.AddTranslateOp().Set(Gf.Vec3d(0, 1, 9))

to_uv = Gf.Matrix4d(0.5, 0, 0, 0,
                    0, 0.5, 0, 0,
                    0, 0, 1, 0,
                    0.5, 0.5, 0, 1)
frustum = UsdGeom.Camera(projector.GetPrim()).GetCamera(1).frustum
world_to_projector = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix() * to_uv


def material(path):
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
    if mode == "reader":
        reader = UsdShade.Shader.Define(stage, path + "/Frame")
        reader.CreateIdAttr("UsdPrimvarReader_matrix")
        reader.CreateInput("varname", Sdf.ValueTypeNames.String).Set(PRIMVAR)
        reader.CreateInput("fallback", Sdf.ValueTypeNames.Matrix4d).Set(Gf.Matrix4d(1))
        reader.CreateOutput("result", Sdf.ValueTypeNames.Matrix4d)
        shader.CreateInput("geometryFrame", Sdf.ValueTypeNames.Matrix4d).ConnectToSource(reader.ConnectableAPI(), "result")
    result.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    return result, shader


def mesh(path):
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr([(-1.5, -1.5, 0), (1.5, -1.5, 0), (1.5, 1.5, 0), (-1.5, 1.5, 0)])
    m.CreateFaceVertexCountsAttr([4])
    m.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    m.CreateDoubleSidedAttr(True)
    m.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    return m


# A parent that moves after the placement: Geometry space must carry the image along at frame 2.
group = UsdGeom.Xform.Define(stage, "/World/Group")
move = group.AddTranslateOp()
move.Set(Gf.Vec3d(0, 0, 0), 1)
move.Set(Gf.Vec3d(0, 0.75, 0), 2)
shared = None if mode == "material" else material("/World/Looks/Shared")[0]


def identity_at(x):
    return Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(x, 0, 0))


def place(prim, name, values):
    """values: {time or None: matrix}"""
    if mode == "material":
        own, shader = material("/World/Looks/" + name)
        UsdShade.MaterialBindingAPI.Apply(prim).Bind(own)
        attribute = shader.CreateInput("geometryFrame", Sdf.ValueTypeNames.Matrix4d)
        for t, value in values.items():
            attribute.Set(value) if t is None else attribute.Set(value, t)
        return
    UsdShade.MaterialBindingAPI.Apply(prim).Bind(shared)
    primvar = UsdGeom.PrimvarsAPI(prim).CreatePrimvar(PRIMVAR, Sdf.ValueTypeNames.Matrix4d, UsdGeom.Tokens.constant)
    for t, value in values.items():
        primvar.Set(value) if t is None else primvar.Set(value, t)


if not instances:
    # Held: its frame is fixed, so at frame 2 the image rides along with the parent.
    left = mesh("/World/Group/Left")
    left.AddTranslateOp().Set(Gf.Vec3d(-3.5, 0, 0))
    place(left.GetPrim(), "Left", {None: identity_at(-3.5)})
    # Held at another place.
    right = mesh("/World/Group/Right")
    right.AddTranslateOp().Set(Gf.Vec3d(3.5, 0, 0))
    place(right.GetPrim(), "Right", {None: identity_at(3.5)})
    # Animated: the frame follows its own move, so the projection stays put in the world.
    sampled = mesh("/World/Sampled")
    op = sampled.AddTranslateOp()
    op.Set(Gf.Vec3d(0, -2.5, 0), 1)
    op.Set(Gf.Vec3d(0.75, -2.5, 0), 2)
    place(sampled.GetPrim(), "Sampled", {1.0: Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0, -2.5, 0)),
                                         2.0: Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0.75, -2.5, 0))})
else:
    proto = stage.CreateClassPrim("/Proto")
    UsdGeom.Xform.Define(stage, "/Proto")
    inner = mesh("/Proto/Mesh")
    inner.AddTranslateOp().Set(Gf.Vec3d(0, 0.25, 0))
    if instances == 1:
        UsdShade.MaterialBindingAPI.Apply(inner.GetPrim()).Bind(shared)
    for name, x in (("Left", -3.5), ("Right", 3.5)):
        instance = UsdGeom.Xform.Define(stage, "/World/Group/" + name)
        instance.GetPrim().GetReferences().AddInternalReference("/Proto")
        instance.AddTranslateOp().Set(Gf.Vec3d(x, 0, 0))
        instance.GetPrim().SetInstanceable(True)
        frame = Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(x, 0.25, 0))
        UsdGeom.PrimvarsAPI(instance.GetPrim()).CreatePrimvar(PRIMVAR, Sdf.ValueTypeNames.Matrix4d, UsdGeom.Tokens.constant).Set(frame)
        if instances == 3:
            UsdShade.MaterialBindingAPI.Apply(instance.GetPrim()).Bind(shared)
    if instances == 2:
        root = stage.GetPrimAtPath("/World")
        collection = Usd.CollectionAPI.Apply(root, "assigned")
        collection.CreateExpansionRuleAttr(Usd.Tokens.expandPrims)
        collection.CreateIncludesRel().SetTargets(["/World/Group/Left", "/World/Group/Right"])
        UsdShade.MaterialBindingAPI.Apply(root).Bind(collection, shared, "assigned", UsdShade.Tokens.strongerThanDescendants)

# Bound with no placement: the identity fallback, the camera in the geometry's own coordinates.
if not instances and mode != "material":
    bare = mesh("/World/Bare")
    bare.AddTranslateOp().Set(Gf.Vec3d(0, 2.5, 0))
    UsdShade.MaterialBindingAPI.Apply(bare.GetPrim()).Bind(shared)
stage.GetRootLayer().Save()
