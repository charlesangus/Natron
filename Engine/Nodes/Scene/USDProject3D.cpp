/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
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

#include "USDProject3D.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <QMetaObject>

#include <pxr/usd/sdf/copyUtils.h>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
// The layer of the Image input the live texture reads.
const char* const kLiveTextureLayer = "rgba";

// A name of its own per hold frame: two projections held at different frames can both reach one
// prim, through its purposes or an assign over another, and each needs the prim where it held it.
std::string
heldPrimvarName(const char* base,
                double holdFrame)
{
    std::string name(base);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.17g", holdFrame);
    name += "Hold";
    for (const char* c = buffer; *c; ++c) {
        name += (*c == '.') ? '_' : (*c == '-') ? 'm'
            : (*c == '+')                       ? 'p'
                                                : *c;
    }

    return name;
}

struct ShaderSocket {
    PXR_NS::UsdShadeShader node;
    const char* output = "value";
};

// A node input: connected to a socket, a constant, or left for the projector's values.
struct ShaderOperand {
    ShaderOperand() = default;
    ShaderOperand(float value)
        : set(true)
        , vector(value)
        , scalar(value)
    {
    }
    ShaderOperand(const PXR_NS::GfVec3f& value)
        : set(true)
        , vector(value)
    {
    }
    ShaderOperand(const ShaderSocket& source)
        : set(true)
        , connected(true)
        , socket(source)
    {
    }

    bool set = false;
    bool connected = false;
    ShaderSocket socket;
    PXR_NS::GfVec3f vector = PXR_NS::GfVec3f(0.f);
    float scalar = 0.f;
};

struct NetworkProjection {
    int space = USDProject3D::eProjectionSpaceWorld;
    // Empty unless held in Geometry space.
    std::string referencePrimvar;
    std::string framePrimvar;
    int projectOn = USDProject3D::eProjectOnBoth;
    bool crop = false;
    bool clip = false;
    int outside = USDProject3D::eOutsideBlack;
    bool hasPlate = false;
};

// Shader nodes under one material, any failure remembered so a network is checked once.
class ShaderGraph {
protected:
    ShaderGraph(const PXR_NS::UsdStageRefPtr& stage,
                const PXR_NS::SdfPath& material)
        : _stage(stage)
        , _material(material)
    {
    }

    PXR_NS::UsdShadeShader node(const std::string& name,
                                const char* id)
    {
        PXR_NS::UsdShadeShader shader = _ok ? PXR_NS::UsdShadeShader::Define(_stage, _material.AppendChild(PXR_NS::TfToken(name))) : PXR_NS::UsdShadeShader();
        _ok = shader && shader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken(id)));
        // Export strips the delegate networks by this mark, which a shader the user put under the
        // same material does not carry.
        if (_ok) {
            shader.GetPrim().SetCustomDataByKey(PXR_NS::TfToken(kSceneProjectionNetworkMark), PXR_NS::VtValue(true));
        }

        return shader;
    }

    void setValue(PXR_NS::UsdShadeShader shader,
                  const char* input,
                  const PXR_NS::SdfValueTypeName& type,
                  const PXR_NS::VtValue& value)
    {
        const PXR_NS::UsdShadeInput created = _ok ? shader.CreateInput(PXR_NS::TfToken(input), type) : PXR_NS::UsdShadeInput();
        _ok = created && created.Set(value);
    }

    void setToken(PXR_NS::UsdShadeShader shader,
                  const char* input,
                  const char* value)
    {
        setValue(shader, input, PXR_NS::SdfValueTypeNames->Token, PXR_NS::VtValue(PXR_NS::TfToken(value)));
    }

    void connect(PXR_NS::UsdShadeShader shader,
                 const char* input,
                 const PXR_NS::SdfValueTypeName& type,
                 const ShaderSocket& source)
    {
        PXR_NS::UsdShadeShader from = source.node;
        const PXR_NS::UsdShadeOutput output = (_ok && from) ? from.CreateOutput(PXR_NS::TfToken(source.output), type) : PXR_NS::UsdShadeOutput();
        const PXR_NS::UsdShadeInput created = output ? shader.CreateInput(PXR_NS::TfToken(input), type) : PXR_NS::UsdShadeInput();
        _ok = created && created.ConnectToSource(output);
    }

    void feed(PXR_NS::UsdShadeShader shader,
              const char* input,
              const PXR_NS::SdfValueTypeName& type,
              const ShaderOperand& operand)
    {
        if (!operand.set) {
            return;
        }
        if (operand.connected) {
            connect(shader, input, type, operand.socket);
        } else if (type == PXR_NS::SdfValueTypeNames->Float) {
            setValue(shader, input, type, PXR_NS::VtValue(operand.scalar));
        } else {
            setValue(shader, input, type, PXR_NS::VtValue(operand.vector));
        }
    }

    // The material's surface for renderContext, from output on shader.
    bool setSurface(const PXR_NS::UsdShadeMaterial& material,
                    const char* renderContext,
                    PXR_NS::UsdShadeShader shader,
                    const char* output)
    {
        const PXR_NS::UsdShadeOutput surface = _ok ? shader.CreateOutput(PXR_NS::TfToken(output), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeOutput();

        return surface && material.CreateSurfaceOutput(PXR_NS::TfToken(renderContext)).ConnectToSource(surface);
    }

private:
    PXR_NS::UsdStageRefPtr _stage;
    PXR_NS::SdfPath _material;
    bool _ok = true;
};

/**
 * @brief The projection as stock Cycles nodes under the material, its surface on
 * outputs:cycles:surface. Cycles takes no matrix input and texture_coordinate no perspective
 * divide, so the projected point is four dot products of the position with the projector's
 * columns and a divide. Only the topology is authored here: the inputs the projector and the
 * plate's window set come from SceneProjectionCameras::projectorValues() and plateValues().
 **/
class CyclesNetwork
    : private ShaderGraph {
public:
    CyclesNetwork(const PXR_NS::UsdStageRefPtr& stage,
                  const PXR_NS::SdfPath& material)
        : ShaderGraph(stage, material)
    {
    }

    bool author(const PXR_NS::UsdShadeMaterial& material,
                const NetworkProjection& projection)
    {
        const bool geometrySpace = projection.space == USDProject3D::eProjectionSpaceGeometry;
        PXR_NS::UsdShadeShader geometry = node("CyclesGeometry", "cycles_geometry");
        // Cycles turns the normal toward the ray and flags the back: the face's own side is the
        // true normal flipped back on a back face.
        const ShaderSocket sign = math("CyclesFaceSign", "multiply_add", ShaderSocket { geometry, "backfacing" }, -2.f, 1.f);
        const ShaderSocket ownNormal = vectorMath("CyclesFaceNormal", "scale", ShaderSocket { geometry, "true_normal" }, ShaderOperand(), ShaderOperand(), sign);

        ShaderSocket position { geometry, "position" };
        ShaderSocket normal = ownNormal;
        ShaderSocket handedness;
        if (geometrySpace) {
            PXR_NS::UsdShadeShader coordinates = node("CyclesObjectPosition", "cycles_texture_coordinate");
            const ShaderSocket object { coordinates, "object" };
            ShaderSocket columns[3];
            ShaderSocket offsets[3];
            for (int j = 0; j < 3; ++j) {
                PXR_NS::UsdShadeShader column = node(SceneProjectionCameras::cyclesFrameColumn(j), "cycles_attribute");
                setValue(column, "attribute", PXR_NS::SdfValueTypeNames->String, PXR_NS::VtValue(SceneProjectionCameras::frameColumnPrimvar(projection.framePrimvar, j)));
                columns[j] = ShaderSocket { column, "vector" };
                const ShaderSocket dot = vectorMath("CyclesFrameDot" + std::to_string(j), "dot_product", object, columns[j], ShaderOperand(), ShaderOperand(), "value");
                offsets[j] = math("CyclesFrameOffset" + std::to_string(j), "add", dot, ShaderSocket { column, "alpha" });
            }
            const ShaderSocket offset = combine("CyclesFrameOffsets", offsets[0], offsets[1], offsets[2]);
            position = vectorMath("CyclesFramePosition", "add", object, offset);
            if (!projection.referencePrimvar.empty()) {
                PXR_NS::UsdShadeShader reference = node(kSceneProjectionCyclesReferencePoints, "cycles_attribute");
                setValue(reference, "attribute", PXR_NS::SdfValueTypeNames->String, PXR_NS::VtValue(projection.referencePrimvar));
                const ShaderSocket point { reference, "vector" };
                // A missing attribute reads as zero in every output, its alpha included, so only a
                // primvar of its own can say a point at the origin is really there.
                PXR_NS::UsdShadeShader presence = node(kSceneProjectionCyclesReferencePresent, "cycles_attribute");
                setValue(presence, "attribute", PXR_NS::SdfValueTypeNames->String, PXR_NS::VtValue(SceneProjectionCameras::cyclesReferencePresentPrimvar(projection.referencePrimvar)));
                const ShaderSocket present { presence, "fac" };
                const ShaderSocket delta = vectorMath("CyclesReferenceDelta", "subtract", point, position);
                position = vectorMath("CyclesReferencePosition", "multiply_add", delta, present, position);
            }
            if (projection.projectOn != USDProject3D::eProjectOnBoth) {
                // The frame's inverse transpose, as cofactors over the determinant, carries the
                // object's normal to where the frame puts it; only the sign of the determinant
                // matters to which side faces the projector.
                PXR_NS::UsdShadeShader toObject = node("CyclesObjectNormal", "cycles_vector_transform");
                setToken(toObject, "transform_type", "normal");
                setToken(toObject, "convert_from", "world");
                setToken(toObject, "convert_to", "object");
                connect(toObject, "vector", PXR_NS::SdfValueTypeNames->Vector3f, ownNormal);
                const ShaderSocket objectNormal { toObject, "vector" };
                ShaderSocket axes[3];
                for (int j = 0; j < 3; ++j) {
                    PXR_NS::GfVec3f unit(0.f);
                    unit[j] = 1.f;
                    axes[j] = vectorMath("CyclesFrameAxis" + std::to_string(j), "add", columns[j], unit);
                }
                ShaderSocket rows[3];
                for (int k = 0; k < 3; ++k) {
                    const ShaderSocket cofactor = vectorMath("CyclesFrameCofactor" + std::to_string(k), "cross_product", axes[(k + 1) % 3], axes[(k + 2) % 3]);
                    rows[k] = vectorMath("CyclesFrameNormal" + std::to_string(k), "dot_product", cofactor, objectNormal, ShaderOperand(), ShaderOperand(), "value");
                    if (k == 0) {
                        handedness = vectorMath("CyclesFrameDeterminant", "dot_product", axes[0], cofactor, ShaderOperand(), ShaderOperand(), "value");
                    }
                }
                normal = combine("CyclesFrameNormal", rows[0], rows[1], rows[2]);
            }
        }

        ShaderSocket projected[4];
        for (int k = 0; k < 4; ++k) {
            const ShaderSocket dot = vectorMath(SceneProjectionCameras::cyclesProjectorDot(k), "dot_product", position, ShaderOperand(), ShaderOperand(), ShaderOperand(), "value");
            projected[k] = math(SceneProjectionCameras::cyclesProjectorOffset(k), "add", dot, ShaderOperand());
        }
        const ShaderSocket frame = combine("CyclesFrame", math("CyclesFrameU", "divide", projected[0], projected[3]), math("CyclesFrameV", "divide", projected[1], projected[3]), 0.f);
        const ShaderSocket uv = vectorMath(kSceneProjectionCyclesPlateTransform, "multiply_add", frame, ShaderOperand(), ShaderOperand());

        ShaderSocket inside = math("CyclesInFront", "greater_than", projected[3], 0.f);
        // Authored either way: projectorValues() sets its inputs.
        const ShaderSocket toward = vectorMath(kSceneProjectionCyclesToward, "multiply_add", position, ShaderOperand(), ShaderOperand());
        if (projection.projectOn != USDProject3D::eProjectOnBoth) {
            ShaderSocket facing = vectorMath("CyclesFacing", "dot_product", normal, toward, ShaderOperand(), ShaderOperand(), "value");
            if (handedness.node) {
                facing = math("CyclesFacingHanded", "multiply", facing, handedness);
            }
            facing = math((projection.projectOn == USDProject3D::eProjectOnFront) ? "CyclesFacesProjector" : "CyclesFacesAway",
                          (projection.projectOn == USDProject3D::eProjectOnFront) ? "greater_than" : "less_than", facing, 0.f);
            // Curves and points have no face to turn away, so they take the plate either way, as
            // in Storm. A mesh reads zero from both.
            PXR_NS::UsdShadeShader hair = node("CyclesHairInfo", "cycles_hair_info");
            PXR_NS::UsdShadeShader points = node("CyclesPointInfo", "cycles_point_info");
            const ShaderSocket isPoint = math("CyclesIsPoint", "greater_than", ShaderSocket { points, "radius" }, 0.f);
            facing = math("CyclesFacingOrNoFace", "maximum", facing, math("CyclesNoFace", "maximum", ShaderSocket { hair, "is_strand" }, isPoint));
            inside = math("CyclesInsideFacing", "multiply", inside, facing);
        }
        if (projection.clip) {
            const ShaderSocket depth = math("CyclesDepth", "divide", projected[2], projected[3]);
            inside = math("CyclesInsideClip", "multiply", inside, math("CyclesWithinClip", "compare", depth, 0.f, 1.f));
        }

        ShaderOperand colour = PXR_NS::GfVec3f(0.f);
        ShaderOperand alpha = 1.f;
        if (projection.hasPlate) {
            PXR_NS::UsdShadeShader texture = node(kSceneProjectionCyclesPlateTexture, "UsdUVTexture");
            connect(texture, "st", PXR_NS::SdfValueTypeNames->Float2, uv);
            for (const char* name : { "wrapS", "wrapT" }) {
                setToken(texture, name, "black");
            }
            // The plate is linear with straight alpha, as Storm reads it, where Cycles would
            // otherwise guess both from the file.
            setToken(texture, "sourceColorSpace", "data");
            setToken(texture, "alpha_type", "unassociated");
            colour = ShaderSocket { texture, "rgb" };
            alpha = ShaderSocket { texture, "a" };
        }
        if (projection.crop) {
            const ShaderSocket cell = vectorMath("CyclesFrameCell", "floor", frame);
            const ShaderSocket offFrame = vectorMath("CyclesFrameCellLength", "length", cell, ShaderOperand(), ShaderOperand(), ShaderOperand(), "value");
            alpha = math("CyclesCroppedAlpha", "multiply", alpha, math("CyclesInFrame", "less_than", offFrame, 0.5f));
        }
        ShaderSocket coverage;
        if (projection.outside == USDProject3D::eOutsideBlack) {
            coverage = math("CyclesCoverage", "multiply_add", alpha, inside, math("CyclesOutside", "subtract", 1.f, inside));
        } else {
            coverage = math("CyclesCoverage", "multiply", alpha, inside);
        }
        PXR_NS::UsdShadeShader emission = node("CyclesEmission", "cycles_emission");
        feed(emission, "color", PXR_NS::SdfValueTypeNames->Color3f, vectorMath("CyclesColour", "scale", colour, ShaderOperand(), ShaderOperand(), inside));
        setValue(emission, "strength", PXR_NS::SdfValueTypeNames->Float, PXR_NS::VtValue(1.f));
        PXR_NS::UsdShadeShader transparent = node("CyclesTransparent", "cycles_transparent_bsdf");
        PXR_NS::UsdShadeShader mix = node("CyclesSurface", "cycles_mix_closure");
        connect(mix, "fac", PXR_NS::SdfValueTypeNames->Float, coverage);
        connect(mix, "closure1", PXR_NS::SdfValueTypeNames->Token, ShaderSocket { transparent, "bsdf" });
        connect(mix, "closure2", PXR_NS::SdfValueTypeNames->Token, ShaderSocket { emission, "emission" });
        // hdCycles matches a terminal by the socket's label, not its name.
        return setSurface(material, "cycles", mix, "Closure");
    } // author

private:
    ShaderSocket math(const std::string& name,
                      const char* operation,
                      const ShaderOperand& a,
                      const ShaderOperand& b,
                      const ShaderOperand& c = ShaderOperand())
    {
        PXR_NS::UsdShadeShader shader = node(name, "cycles_math");
        setToken(shader, "math_type", operation);
        feed(shader, "value1", PXR_NS::SdfValueTypeNames->Float, a);
        feed(shader, "value2", PXR_NS::SdfValueTypeNames->Float, b);
        feed(shader, "value3", PXR_NS::SdfValueTypeNames->Float, c);

        return ShaderSocket { shader, "value" };
    }

    ShaderSocket vectorMath(const std::string& name,
                            const char* operation,
                            const ShaderOperand& a,
                            const ShaderOperand& b = ShaderOperand(),
                            const ShaderOperand& c = ShaderOperand(),
                            const ShaderOperand& scale = ShaderOperand(),
                            const char* output = "vector")
    {
        PXR_NS::UsdShadeShader shader = node(name, "cycles_vector_math");
        setToken(shader, "math_type", operation);
        feed(shader, "vector1", PXR_NS::SdfValueTypeNames->Vector3f, a);
        feed(shader, "vector2", PXR_NS::SdfValueTypeNames->Vector3f, b);
        feed(shader, "vector3", PXR_NS::SdfValueTypeNames->Vector3f, c);
        feed(shader, "scale", PXR_NS::SdfValueTypeNames->Float, scale);

        return ShaderSocket { shader, output };
    }

    ShaderSocket combine(const std::string& name,
                         const ShaderOperand& x,
                         const ShaderOperand& y,
                         const ShaderOperand& z)
    {
        PXR_NS::UsdShadeShader shader = node(name, "cycles_combine_xyz");
        feed(shader, "x", PXR_NS::SdfValueTypeNames->Float, x);
        feed(shader, "y", PXR_NS::SdfValueTypeNames->Float, y);
        feed(shader, "z", PXR_NS::SdfValueTypeNames->Float, z);

        return ShaderSocket { shader, "vector" };
    }
};

/**
 * @brief The projection as stock MoonRay maps under the material, its surface on
 * outputs:moonray:surface, built like the Cycles network from dot products and a divide. The
 * maps pass colours, so every scalar is carried as three equal channels, and a float input bound
 * to a map reads their average. As for Cycles, the values come from projectorValues() and
 * plateValues().
 *
 * hdMoonray hands a primvar to the maps only as user data of a type it translates
 * (lib/hydramoonray/Primvars.cc setUserDataValues): float, float2, float3, colour, int and string
 * arrays, not the float4 frame columns Cycles reads nor the matrix. Geometry space therefore reads
 * the frame from its float3 parts (SceneProjectionCameras::moonRayFramePrimvar()) through
 * AttributeMap's primitive attribute, applied to the object-space position.
 **/
class MoonRayNetwork
    : private ShaderGraph {
public:
    MoonRayNetwork(const PXR_NS::UsdStageRefPtr& stage,
                   const PXR_NS::SdfPath& material)
        : ShaderGraph(stage, material)
    {
    }

    bool author(const PXR_NS::UsdShadeMaterial& material,
                const NetworkProjection& projection)
    {
        const bool geometrySpace = projection.space == USDProject3D::eProjectionSpaceGeometry;
        PXR_NS::UsdShadeShader point = node("MoonRayPosition", "AttributeMap");
        setToken(point, "map_type", "position");
        ShaderSocket position;
        ShaderSocket columns[3];
        if (geometrySpace) {
            const ShaderSocket object = fromRender("MoonRayObjectPosition", "point", "object", ShaderSocket { point, "out" });
            ShaderSocket parts[4];
            for (int part = 0; part < 4; ++part) {
                parts[part] = attribute(SceneProjectionCameras::moonRayFrameReader(part), SceneProjectionCameras::moonRayFramePrimvar(projection.framePrimvar, part), PXR_NS::GfVec3f(0.f));
            }
            PXR_NS::UsdShadeShader offsets = node("MoonRayFrameOffsets", "FloatToRgbMap");
            for (int j = 0; j < 3; ++j) {
                columns[j] = parts[j];
                connect(offsets, kChannels[j], PXR_NS::SdfValueTypeNames->Float, op("MoonRayFrameDot" + std::to_string(j), "dot", object, columns[j]));
            }
            position = op("MoonRayFramePosition", "add", op("MoonRayFrameLinear", "add", object, ShaderSocket { offsets, "out" }), parts[3]);
        } else {
            position = fromRender("MoonRayWorldPosition", "point", "world", ShaderSocket { point, "out" });
        }
        if (!projection.referencePrimvar.empty()) {
            // The default marks a prim without reference points, which keeps its own position.
            const float missing = 1e30f;
            const ShaderSocket stored = attribute(kSceneProjectionMoonRayReferencePoints, projection.referencePrimvar, PXR_NS::GfVec3f(missing));
            const ShaderSocket present = op("MoonRayReferencePresent", "less_than", op("MoonRayReferenceLength", "length", stored, ShaderOperand()), 0.5f * missing);
            const ShaderSocket delta = op("MoonRayReferenceDelta", "subtract", stored, position);
            position = op("MoonRayReferencePosition", "add", op("MoonRayReferenceShift", "multiply", delta, present), position);
        }

        ShaderSocket projected[4];
        for (int k = 0; k < 4; ++k) {
            const ShaderSocket dot = op(SceneProjectionCameras::moonRayProjectorDot(k), "dot", position, ShaderOperand());
            projected[k] = op(SceneProjectionCameras::moonRayProjectorOffset(k), "add", dot, ShaderOperand());
        }
        PXR_NS::UsdShadeShader combined = node("MoonRayFrame", "FloatToRgbMap");
        connect(combined, kChannels[0], PXR_NS::SdfValueTypeNames->Float, op("MoonRayFrameU", "divide", projected[0], projected[3]));
        connect(combined, kChannels[1], PXR_NS::SdfValueTypeNames->Float, op("MoonRayFrameV", "divide", projected[1], projected[3]));
        setValue(combined, kChannels[2], PXR_NS::SdfValueTypeNames->Float, PXR_NS::VtValue(0.f));
        const ShaderSocket frame { combined, "out" };
        const ShaderSocket uv = op(kSceneProjectionMoonRayPlateOffset, "add", op(kSceneProjectionMoonRayPlateScale, "multiply", frame, ShaderOperand()), ShaderOperand());

        ShaderSocket inside = op("MoonRayInFront", "greater_than", projected[3], 0.f);
        // Authored either way: projectorValues() sets its inputs.
        const ShaderSocket toward = op(kSceneProjectionMoonRayToward, "add", position, ShaderOperand());
        if (projection.projectOn != USDProject3D::eProjectOnBoth) {
            // MoonRay keeps the geometric normal on the face's own side.
            PXR_NS::UsdShadeShader geometric = node("MoonRayNormal", "AttributeMap");
            setToken(geometric, "map_type", "geometric normal");
            ShaderSocket facing;
            if (geometrySpace) {
                // As for Cycles: the frame's cofactors carry the object's normal to where the frame
                // puts it, and the determinant's sign undoes a mirror's flip.
                const ShaderSocket objectNormal = fromRender("MoonRayObjectNormal", "normal", "object", ShaderSocket { geometric, "out" });
                ShaderSocket axes[3];
                for (int j = 0; j < 3; ++j) {
                    PXR_NS::GfVec3f unit(0.f);
                    unit[j] = 1.f;
                    axes[j] = op("MoonRayFrameAxis" + std::to_string(j), "add", columns[j], unit);
                }
                PXR_NS::UsdShadeShader normal = node("MoonRayFrameNormal", "FloatToRgbMap");
                ShaderSocket handedness;
                for (int k = 0; k < 3; ++k) {
                    const ShaderSocket cofactor = op("MoonRayFrameCofactor" + std::to_string(k), "cross", axes[(k + 1) % 3], axes[(k + 2) % 3]);
                    connect(normal, kChannels[k], PXR_NS::SdfValueTypeNames->Float, op("MoonRayFrameNormal" + std::to_string(k), "dot", cofactor, objectNormal));
                    if (k == 0) {
                        handedness = op("MoonRayFrameDeterminant", "dot", axes[0], cofactor);
                    }
                }
                facing = op("MoonRayFacingHanded", "multiply", op("MoonRayFacing", "dot", ShaderSocket { normal, "out" }, toward), handedness);
            } else {
                facing = op("MoonRayFacing", "dot", fromRender("MoonRayWorldNormal", "normal", "world", ShaderSocket { geometric, "out" }), toward);
            }
            const bool front = projection.projectOn == USDProject3D::eProjectOnFront;
            inside = op("MoonRayInsideFacing", "multiply", inside, op(front ? "MoonRayFacesProjector" : "MoonRayFacesAway", front ? "greater_than" : "less_than", facing, 0.f));
        }
        if (projection.clip) {
            const ShaderSocket depth = op("MoonRayDepthMagnitude", "abs", op("MoonRayDepth", "divide", projected[2], projected[3]), ShaderOperand());
            inside = op("MoonRayInsideClip", "multiply", inside, op("MoonRayWithinClip", "less_than_or_equal", depth, 1.f));
        }

        ShaderOperand colour = PXR_NS::GfVec3f(0.f);
        ShaderOperand alpha = 1.f;
        if (projection.hasPlate) {
            PXR_NS::UsdShadeShader texture = node(kSceneProjectionMoonRayPlateTexture, "UsdUVTexture");
            connect(texture, "st", PXR_NS::SdfValueTypeNames->Float2, uv);
            for (const char* name : { "wrapS", "wrapT" }) {
                setToken(texture, name, "black");
            }
            setToken(texture, "sourceColorSpace", "raw");
            colour = ShaderSocket { texture, "rgb" };
            alpha = ShaderSocket { texture, "a" };
        }
        if (projection.crop) {
            const ShaderSocket offFrame = op("MoonRayFrameCellLength", "length", op("MoonRayFrameCell", "floor", frame, ShaderOperand()), ShaderOperand());
            alpha = op("MoonRayCroppedAlpha", "multiply", alpha, op("MoonRayInFrame", "less_than", offFrame, 0.5f));
        }
        ShaderSocket coverage = op("MoonRayInsideAlpha", "multiply", alpha, inside);
        if (projection.outside == USDProject3D::eOutsideBlack) {
            coverage = op("MoonRayCoverage", "add", coverage, op("MoonRayOutside", "subtract", 1.f, inside));
        }
        PXR_NS::UsdShadeShader surface = node("MoonRaySurface", "DwaEmissiveMaterial");
        setValue(surface, "show_emission", PXR_NS::SdfValueTypeNames->Bool, PXR_NS::VtValue(true));
        connect(surface, "emission", PXR_NS::SdfValueTypeNames->Color3f, op("MoonRayColour", "multiply", colour, inside));
        connect(surface, "presence", PXR_NS::SdfValueTypeNames->Float, coverage);

        return setSurface(material, "moonray", surface, "surface");
    } // author

private:
    ShaderSocket op(const std::string& name,
                    const char* operation,
                    const ShaderOperand& a,
                    const ShaderOperand& b)
    {
        PXR_NS::UsdShadeShader shader = node(name, "OpMap");
        setToken(shader, "operation", operation);
        feed(shader, "op1", PXR_NS::SdfValueTypeNames->Color3f, a);
        feed(shader, "op2", PXR_NS::SdfValueTypeNames->Color3f, b);

        return ShaderSocket { shader, "out" };
    }

    // From render space, which AttributeMap reports in, to toSpace: world, or object for the
    // shaded prim's own.
    ShaderSocket fromRender(const std::string& name,
                            const char* inputType,
                            const char* toSpace,
                            const ShaderSocket& source)
    {
        PXR_NS::UsdShadeShader shader = node(name, "TransformSpaceMap");
        setToken(shader, "input_type", inputType);
        setToken(shader, "from_space", "render");
        setToken(shader, "to_space", toSpace);
        connect(shader, "input", PXR_NS::SdfValueTypeNames->Vector3f, source);

        return ShaderSocket { shader, "out" };
    }

    // The prim's float3 primvar, or fallback where it has none.
    ShaderSocket attribute(const std::string& name,
                           const std::string& primvar,
                           const PXR_NS::GfVec3f& fallback)
    {
        PXR_NS::UsdShadeShader shader = node(name, "AttributeMap");
        setToken(shader, "map_type", "primitive attribute");
        setValue(shader, "primitive_attribute_name", PXR_NS::SdfValueTypeNames->String, PXR_NS::VtValue(primvar));
        setToken(shader, "primitive_attribute_type", "vec3f");
        setValue(shader, "default_value", PXR_NS::SdfValueTypeNames->Color3f, PXR_NS::VtValue(fallback));
        setValue(shader, "warn_when_unavailable", PXR_NS::SdfValueTypeNames->Bool, PXR_NS::VtValue(false));

        return ShaderSocket { shader, "out" };
    }

    static constexpr const char* kChannels[3] = { "R", "G", "B" };
};
} // anonymous namespace

// The camera as the shader reads it, once, or at every frame of the project range.
struct USDProject3D::CameraSamples {
    std::vector<double> frames;
    std::vector<PXR_NS::GfMatrix4d> worldToProjector;
    std::vector<PXR_NS::GfVec4f> origin;
    // Camera Path names a camera the camera scene does not have: one zero matrix, which puts every
    // point behind the camera, so nothing projects until the stage that renders the material
    // resolves the path (SceneProjectionCameras).
    bool missing = false;
    // When missing, the project frames that stage reads the camera at: the hold, or the range when
    // the camera may move there.
    std::vector<double> deferredFrames;
    U64 digest = 0;
};

// The last material topology built, which only a change to what shapes the networks replaces.
struct ProjectionTopologyMemo {
    std::mutex lock;
    U64 key = 0;
    PXR_NS::SdfLayerRefPtr layer;
};

struct USDProject3D::Implementation {
    std::mutex lock;
    // The last samples and what they were read from, so an evaluation that changes nothing they
    // read does not read the camera scene again.
    bool samplesValid = false;
    U64 samplesKey = 0;
    std::shared_ptr<const CameraSamples> samples;
    std::shared_ptr<ProjectionTopologyMemo> topology = std::make_shared<ProjectionTopologyMemo>();
};

USDProject3D::USDProject3D(NodePtr node)
    : NativeEffectBase(node)
    , _imp(new Implementation())
    , _cameraMenuRefreshPending(false)
{
}

USDProject3D::~USDProject3D()
{
}

const NodeSpec*
USDProject3D::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDProject3D::getShaderPath()
{
    SceneCaches::registerLiveTextures(nullptr);
    const PXR_NS::PlugPluginPtr plugin = PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("natronLiveTexture");

    return plugin ? plugin->FindPluginResource(kUSDProject3DShaderFile, true) : std::string();
}

void
USDProject3D::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "Material"));
}

void
USDProject3D::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Material"),
                                        tr("The path of the material prim the node authors. Its shaders are authored under it."));

    _camera.create(this, page, kUSDProject3DParamCamera, kUSDProject3DParamCameraPath,
                   tr("The cameras of the Camera input's scene: picking one sets Camera Path to it."),
                   tr("The path of the camera to project through. Empty, the first camera of the Camera input's "
                      "scene is used. A path the Camera input does not have, or with nothing connected to it, is read "
                      "from the scene the projection renders in, such as a USDMerge with the camera below the "
                      "geometry; USDRender warns when that scene has no camera at the path."));

    KnobChoicePtr projectionSpace = createKnob<KnobChoice>(tr("Projection Space"));
    projectionSpace->setName(kUSDProject3DParamProjectionSpace);
    projectionSpace->setHintToolTip(tr("What the projection is fixed to once the material is connected to a geometry node."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("world", tr("World").toStdString(),
                                       tr("The projection stays where the camera is: geometry moving after it slides through "
                                          "the image.")
                                           .toStdString()));
        options.push_back(ChoiceOption("geometry", tr("Geometry").toStdString(),
                                       tr("The projection is fixed to the geometry as the geometry node places it, so "
                                          "transforms after that node carry the image along. With Use Frame Hold on, the "
                                          "geometry is also taken at Frame Hold, so the image sticks to it however it "
                                          "moves.")
                                           .toStdString()));
        projectionSpace->populateChoices(options);
    }
    projectionSpace->setDefaultValue((int)eProjectionSpaceWorld);
    projectionSpace->setAnimationEnabled(false);
    page->addKnob(projectionSpace);
    _projectionSpace = projectionSpace;

    KnobChoicePtr projectOn = createKnob<KnobChoice>(tr("Project On"));
    projectOn->setName(kUSDProject3DParamProjectOn);
    projectOn->setHintToolTip(tr("Which faces the image falls on, by whether they face the camera. The others show Outside."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("both", tr("Both").toStdString(), tr("Every face.").toStdString()));
        options.push_back(ChoiceOption("front", tr("Front").toStdString(), tr("Faces facing the camera.").toStdString()));
        options.push_back(ChoiceOption("back", tr("Back").toStdString(), tr("Faces facing away from the camera.").toStdString()));
        projectOn->populateChoices(options);
    }
    projectOn->setDefaultValue((int)eProjectOnBoth);
    projectOn->setAnimationEnabled(false);
    page->addKnob(projectOn);
    _projectOn = projectOn;

    KnobBoolPtr crop = createKnob<KnobBool>(tr("Crop"));
    crop->setName(kUSDProject3DParamCrop);
    crop->setHintToolTip(tr("Outside the camera's frame, which the image's format fills, the image is transparent black. Off, "
                            "the image's data beyond its format shows there. Beyond the image's data it is transparent "
                            "black either way, as the Viewer shows it."));
    crop->setDefaultValue(true);
    crop->setAnimationEnabled(false);
    page->addKnob(crop);
    _crop = crop;

    KnobBoolPtr clipNearFar = createKnob<KnobBool>(tr("Clip Near Far"));
    clipNearFar->setName(kUSDProject3DParamClipNearFar);
    clipNearFar->setHintToolTip(tr("Nearer than the camera's near clipping plane or beyond its far one, the surface shows "
                                   "Outside."));
    clipNearFar->setDefaultValue(false);
    clipNearFar->setAnimationEnabled(false);
    page->addKnob(clipNearFar);
    _clipNearFar = clipNearFar;

    KnobChoicePtr outside = createKnob<KnobChoice>(tr("Outside"));
    outside->setName(kUSDProject3DParamOutside);
    outside->setHintToolTip(tr("What the surface shows where the image does not fall: behind the camera, beyond the "
                               "clipping planes with Clip Near Far on, and on faces Project On leaves out."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("black", tr("Black").toStdString(), tr("Opaque black.").toStdString()));
        options.push_back(ChoiceOption("transparent", tr("Transparent").toStdString(),
                                       tr("Nothing: the surface is not drawn there.").toStdString()));
        outside->populateChoices(options);
    }
    outside->setDefaultValue((int)eOutsideBlack);
    outside->setAnimationEnabled(false);
    page->addKnob(outside);
    _outside = outside;

    KnobChoicePtr fitMode = createKnob<KnobChoice>(tr("Fit Mode"));
    fitMode->setName(kUSDProject3DParamFitMode);
    fitMode->setHintToolTip(tr("How the camera's aperture is conformed to the Image input's format when their shapes differ, "
                               "as in USDRender."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("fill", tr("Fill").toStdString(),
                                       tr("The aperture fills the format: the side that does not fit is cropped.").toStdString()));
        options.push_back(ChoiceOption("horizontal", tr("Horizontal").toStdString(),
                                       tr("The aperture's width fills the format's; its height is cropped or extended.").toStdString()));
        options.push_back(ChoiceOption("vertical", tr("Vertical").toStdString(),
                                       tr("The aperture's height fills the format's; its width is cropped or extended.").toStdString()));
        fitMode->populateChoices(options);
    }
    fitMode->setDefaultValue((int)HydraRender::eFitModeFill);
    fitMode->setAnimationEnabled(false);
    page->addKnob(fitMode);
    _fitMode = fitMode;

    KnobBoolPtr useFrameHold = createKnob<KnobBool>(tr("Use Frame Hold"));
    useFrameHold->setName(kUSDProject3DParamUseFrameHold);
    useFrameHold->setHintToolTip(tr("Projects through the camera as it is at Frame Hold on every frame, so a moving camera "
                                    "leaves the projection where it was then. The image still plays."));
    useFrameHold->setDefaultValue(false);
    useFrameHold->setAnimationEnabled(false);
    page->addKnob(useFrameHold);
    _useFrameHold = useFrameHold;

    KnobDoublePtr frameHold = createKnob<KnobDouble>(tr("Frame Hold"));
    frameHold->setName(kUSDProject3DParamFrameHold);
    frameHold->setHintToolTip(tr("The frame the camera is held at when Use Frame Hold is on."));
    frameHold->setDefaultValue(1.);
    frameHold->setAnimationEnabled(false);
    frameHold->setAllDimensionsEnabled(false);
    page->addKnob(frameHold);
    _frameHold = frameHold;
    SceneNodeAuthoring::createDeletedTextureSourcesKnob(this, page);
} // USDProject3D::initializeKnobs

void
USDProject3D::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    KnobBoolPtr useFrameHold = _useFrameHold.lock();
    KnobDoublePtr frameHold = _frameHold.lock();
    if (useFrameHold && frameHold) {
        frameHold->setAllDimensionsEnabled(useFrameHold->getValue());
    }
    scheduleCameraMenuRefresh();
}

void
USDProject3D::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    SceneNodeAuthoring::forgetDeletedTextureSourceIfConnected(*this, inputNo);
    scheduleCameraMenuRefresh();
}

void
USDProject3D::onInputSourceDeleted(int inputNo,
                                   const std::string& sourceName)
{
    SceneNodeAuthoring::recordDeletedTextureSource(*this, inputNo, sourceName);
}

void
USDProject3D::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleCameraMenuRefresh();
}

bool
USDProject3D::knobChanged(KnobI* k,
                          ValueChangedReasonEnum reason,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    bool menuStale = false;
    if (_camera.knobChanged(k, reason, &menuStale)) {
        if (menuStale) {
            scheduleCameraMenuRefresh();
        }

        return true;
    }
    KnobBoolPtr useFrameHold = _useFrameHold.lock();
    KnobDoublePtr frameHold = _frameHold.lock();
    if (useFrameHold && frameHold && (k == useFrameHold.get())) {
        frameHold->setAllDimensionsEnabled(useFrameHold->getValue());

        return true;
    }

    return false;
}

void
USDProject3D::scheduleCameraMenuRefresh()
{
    if (_cameraMenuRefreshPending.exchange(true)) {
        return;
    }
    // The hooks that call this fire mid-recompute of a hash, before the upstream scene settled.
    QMetaObject::invokeMethod(this, [this]() {
        _cameraMenuRefreshPending = false;
        refreshCameraMenu(); }, Qt::QueuedConnection);
}

void
USDProject3D::refreshCameraMenu()
{
    std::vector<std::string> cameras;
    EffectInstancePtr input = getInput(kUSDProject3DInputCamera);
    ScenePayload scene;
    std::string error;
    if (input && (input->evaluateScene(EvaluateSceneArgs(getCurrentTime(), ViewIdx(0)), &scene, &error) == eRenderRoIRetCodeOk) && !scene.isEmpty()) {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, &error);
        if (lease.isValid()) {
            for (const PXR_NS::SdfPath& path : HydraRender::cameraPaths(lease.getStage())) {
                cameras.push_back(path.GetString());
            }
        }
    }
    _camera.refreshMenu(cameras, true);
}

double
USDProject3D::getPlateAspect(double time) const
{
    EffectInstancePtr input = getInput(kUSDProject3DInputImage);
    if (input) {
        input = input->getNearestNonIdentity(time);
    }
    const NodeMetadataConstPtr metadata = input ? input->getMetadata(time, ViewIdx(0)) : NodeMetadataConstPtr();
    if (metadata) {
        const RectI& format = metadata->getOutputFormat();
        const double par = metadata->getPixelAspectRatio(-1);
        if ((format.width() > 0) && (format.height() > 0) && (par > 0.)) {
            return format.width() * par / format.height();
        }
    }
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);

    return (format.height() > 0) ? format.width() * format.getPixelAspectRatio() / format.height() : 1.;
}

StatusEnum
USDProject3D::renderScene(const SceneRenderActionArgs& args,
                          ScenePayload* outputScene,
                          std::string* errorMessage)
{
    const ScenePayload cameraScene = args.getInputScene(kUSDProject3DInputCamera);

    KnobChoicePtr projectOnKnob = _projectOn.lock();
    KnobBoolPtr cropKnob = _crop.lock();
    KnobBoolPtr clipKnob = _clipNearFar.lock();
    KnobChoicePtr outsideKnob = _outside.lock();
    KnobChoicePtr fitKnob = _fitMode.lock();
    KnobChoicePtr spaceKnob = _projectionSpace.lock();
    KnobBoolPtr useFrameHoldKnob = _useFrameHold.lock();
    KnobDoublePtr frameHoldKnob = _frameHold.lock();
    KnobStringPtr primPathKnob = _primPath.lock();
    if (!projectOnKnob || !cropKnob || !clipKnob || !outsideKnob || !fitKnob || !spaceKnob || !useFrameHoldKnob || !frameHoldKnob || !primPathKnob) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return eStatusFailed;
    }
    const auto fail = [this, errorMessage]() {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    };

    const std::string shaderPath = getShaderPath();
    if (shaderPath.empty()) {
        *errorMessage = tr("The projection shader %1 is missing from this installation (Resources/usd/natronLiveTexture).")
                            .arg(QString::fromUtf8(kUSDProject3DShaderFile))
                            .toStdString();

        return fail();
    }
    std::string cameraPath = _camera.getPath();
    const int projectOn = std::clamp(projectOnKnob->getValue(), (int)eProjectOnBoth, (int)eProjectOnBack);
    const bool crop = cropKnob->getValue();
    const bool clip = clipKnob->getValue();
    const int outside = std::clamp(outsideKnob->getValue(), (int)eOutsideBlack, (int)eOutsideTransparent);
    const int fitValue = fitKnob->getValue();
    const HydraRender::FitModeEnum fit = (fitValue == (int)HydraRender::eFitModeHorizontal) ? HydraRender::eFitModeHorizontal : (fitValue == (int)HydraRender::eFitModeVertical) ? HydraRender::eFitModeVertical
                                                                                                                                                                                 : HydraRender::eFitModeFill;
    const int space = std::clamp(spaceKnob->getValue(), (int)eProjectionSpaceWorld, (int)eProjectionSpaceGeometry);
    const bool useFrameHold = useFrameHoldKnob->getValue();
    const double frameHold = frameHoldKnob->getValue();
    const double aspect = getPlateAspect(args.time);

    SceneLayerMemo::Key layerKey;
    if (!getSceneLayerKey(SceneLayerReads(), &layerKey, errorMessage)) {
        return eStatusFailed;
    }
    ContentHasher keyHasher;
    keyHasher.appendU64(layerKey.nodeDigest);
    keyHasher.appendDouble(layerKey.firstFrame);
    keyHasher.appendDouble(layerKey.lastFrame);
    keyHasher.appendDouble(layerKey.framesPerSecond);
    keyHasher.appendU64(cameraScene.getStackHash());
    keyHasher.appendString(cameraPath);
    keyHasher.appendDouble(aspect);
    const U64 samplesKey = keyHasher.finalize();

    std::shared_ptr<const CameraSamples> samples;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        if (_imp->samplesValid && (_imp->samplesKey == samplesKey)) {
            samples = _imp->samples;
        }
    }
    if (!cameraPath.empty() && !(PXR_NS::SdfPath::IsValidPathString(cameraPath) && PXR_NS::SdfPath(cameraPath).IsAbsolutePath() && PXR_NS::SdfPath(cameraPath).IsPrimPath())) {
        *errorMessage = tr("Camera Path \"%1\" is not an absolute prim path such as /World/Camera.").arg(QString::fromStdString(cameraPath)).toStdString();

        return fail();
    }
    if (!samples) {
        const auto missingSamples = [&layerKey, &cameraPath, aspect, fitValue, useFrameHold, frameHold]() {
            std::shared_ptr<CameraSamples> missing = std::make_shared<CameraSamples>();
            missing->missing = true;
            missing->worldToProjector.push_back(PXR_NS::GfMatrix4d(0.));
            missing->origin.push_back(PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f));
            if (useFrameHold) {
                missing->deferredFrames.push_back(frameHold);
            } else {
                SceneLayerEdit range;
                range.firstFrame = layerKey.firstFrame;
                range.lastFrame = layerKey.lastFrame;
                missing->deferredFrames = SceneNodeAuthoring::getSampleFrames(range);
            }
            ContentHasher digest;
            digest.appendBool(true);
            digest.appendString(cameraPath);
            digest.appendDouble(aspect);
            digest.appendI64(fitValue);
            digest.appendDouble(layerKey.framesPerSecond);
            digest.appendU64(missing->deferredFrames.size());
            for (double frame : missing->deferredFrames) {
                digest.appendDouble(frame);
            }
            missing->digest = digest.finalize();

            return missing;
        };
        std::shared_ptr<CameraSamples> computed;
        if (cameraScene.isEmpty()) {
            if (cameraPath.empty()) {
                *errorMessage = tr("No camera to project through: connect one to the Camera input.").toStdString();

                return fail();
            }
            computed = missingSamples();
        } else {
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(cameraScene, errorMessage);
            const PXR_NS::UsdStageRefPtr stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the camera scene.").toStdString();
                }

                return fail();
            }
            if (cameraPath.empty()) {
                const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
                if (cameras.empty()) {
                    *errorMessage = tr("The Camera input has no camera to project through.").toStdString();

                    return fail();
                }
                cameraPath = cameras.front().GetString();
            }
            const PXR_NS::SdfPath path(cameraPath);
            const PXR_NS::UsdGeomCamera camera(stage->GetPrimAtPath(path));
            if (!camera) {
                computed = missingSamples();
            } else {
                const double fps = layerKey.framesPerSecond;
                SceneLayerEdit range;
                range.firstFrame = layerKey.firstFrame;
                range.lastFrame = layerKey.lastFrame;
                computed = std::make_shared<CameraSamples>();
                // Frames empty means one sample, authored as the default value.
                std::vector<double> readFrames;
                if (useFrameHold) {
                    readFrames.push_back(frameHold);
                } else if (SceneProjectionCameras::cameraMightVary(camera)) {
                    computed->frames = SceneNodeAuthoring::getSampleFrames(range);
                    readFrames = computed->frames;
                } else {
                    readFrames.push_back(range.firstFrame);
                }
                ContentHasher digest;
                digest.appendU64(readFrames.size());
                for (double frame : readFrames) {
                    const PXR_NS::UsdTimeCode time(SceneOps::frameToTimeCode(cameraScene, frame, fps));
                    PXR_NS::GfMatrix4d matrix;
                    PXR_NS::GfVec4f origin;
                    if (!SceneProjectionCameras::computeProjector(stage, path, time, aspect, fit, &matrix, &origin, errorMessage)) {
                        return fail();
                    }
                    computed->worldToProjector.push_back(matrix);
                    computed->origin.push_back(origin);
                    digest.appendDouble(frame);
                    digest.appendBytes(matrix.data(), 16 * sizeof(double));
                    digest.appendBytes(origin.data(), 4 * sizeof(float));
                }
                digest.appendU64(computed->frames.size());
                computed->digest = digest.finalize();
            }
            lease.release();
        }

        samples = computed;
        std::lock_guard<std::mutex> l(_imp->lock);
        _imp->samplesValid = true;
        _imp->samplesKey = samplesKey;
        _imp->samples = samples;
    }

    // The image's identity and hash, without rendering it: the layer reads nothing else of it.
    LiveTextureSource plate;
    const bool hasPlate = SceneNodeAuthoring::getLiveTextureSource(*this, kUSDProject3DInputImage, kLiveTextureLayer, &plate);
    ContentHasher facts;
    facts.appendU64(samples->digest);
    facts.appendString(shaderPath);
    facts.appendU64((U64)space);
    facts.appendBool(useFrameHold);
    if (useFrameHold) {
        facts.appendDouble(frameHold);
    }
    facts.appendBool(hasPlate);
    if (hasPlate) {
        SceneNodeAuthoring::appendLiveTextureSource(plate, &facts);
        // The texture's window falls back on the project format for an image without one.
        Format projectFormat;
        getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
        for (int side : { projectFormat.x1, projectFormat.y1, projectFormat.x2, projectFormat.y2 }) {
            facts.appendI64(side);
        }
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = !samples->frames.empty() || (hasPlate && plate.varying);

    PXR_NS::SdfPath path;
    if (!SceneNodeAuthoring::getPrimPath(primPathKnob, &path, errorMessage)) {
        return fail();
    }
    const double framesPerSecond = layerKey.framesPerSecond;
    // Everything of the material that depends on no value, built once per topology and copied into
    // each new layer: the networks are hundreds of prims, which a scrub would otherwise redefine.
    const auto authorTopology = [](const PXR_NS::UsdStageRefPtr& stage,
                                   const PXR_NS::SdfPath& path,
                                   const std::string& shaderPath,
                                   const NetworkProjection& network,
                                   std::string* error) {
        const QString pathString = QString::fromStdString(path.GetString());
        const int space = network.space;
        const int projectOn = network.projectOn;
        const bool crop = network.crop;
        const bool clip = network.clip;
        const int outside = network.outside;
        const std::string& framePrimvar = network.framePrimvar;
        const std::string& referencePrimvar = network.referencePrimvar;
        const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterial::Define(stage, path);
        PXR_NS::UsdShadeShader shader = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kUSDProject3DShaderName)));
        if (!material || !shader || !shader.SetSourceAsset(PXR_NS::SdfAssetPath(shaderPath), PXR_NS::TfToken("glslfx"))) {
            *error = tr("USD could not author the projection material at %1.").arg(pathString).toStdString();

            return false;
        }
        bool ok = true;
        if (space == (int)eProjectionSpaceGeometry) {
            PXR_NS::UsdShadeShader frameReader = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kUSDProject3DFrameReaderName)));
            const PXR_NS::UsdShadeInput frameName = frameReader ? frameReader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String) : PXR_NS::UsdShadeInput();
            const PXR_NS::UsdShadeInput fallback = frameReader ? frameReader.CreateInput(PXR_NS::TfToken("fallback"), PXR_NS::SdfValueTypeNames->Matrix4d) : PXR_NS::UsdShadeInput();
            const PXR_NS::UsdShadeOutput result = frameReader ? frameReader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Matrix4d) : PXR_NS::UsdShadeOutput();
            ok = frameReader && frameReader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_matrix")))
                && frameName && frameName.Set(framePrimvar)
                && fallback && fallback.Set(PXR_NS::GfMatrix4d(1.))
                && result;
            const PXR_NS::UsdShadeInput frame = ok ? shader.CreateInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame), PXR_NS::SdfValueTypeNames->Matrix4d) : PXR_NS::UsdShadeInput();
            ok = frame && frame.ConnectToSource(result);
        }
        if (ok && (space == (int)eProjectionSpaceGeometry) && !network.referencePrimvar.empty()) {
            PXR_NS::UsdShadeShader referenceReader = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kUSDProject3DReferenceReaderName)));
            const PXR_NS::UsdShadeInput referenceName = referenceReader ? referenceReader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String) : PXR_NS::UsdShadeInput();
            const PXR_NS::UsdShadeInput fallback = referenceReader ? referenceReader.CreateInput(PXR_NS::TfToken("fallback"), PXR_NS::SdfValueTypeNames->Float3) : PXR_NS::UsdShadeInput();
            const PXR_NS::UsdShadeOutput result = referenceReader ? referenceReader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float3) : PXR_NS::UsdShadeOutput();
            // Storm defines the primvar's accessor either way; this value is how the surface knows a prim lacks it.
            ok = referenceReader && referenceReader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float3")))
                && referenceName && referenceName.Set(referencePrimvar)
                && fallback && fallback.Set(PXR_NS::GfVec3f(1e30f))
                && result;
            const PXR_NS::UsdShadeInput reference = ok ? shader.CreateInput(PXR_NS::TfToken(kUSDProject3DInputReferencePoints), PXR_NS::SdfValueTypeNames->Float3) : PXR_NS::UsdShadeInput();
            ok = reference && reference.ConnectToSource(result);
        }
        const std::pair<const char*, int> ints[] = {
            { kUSDProject3DInputProjectionSpace, space },
            { "projectOn", projectOn },
            { "crop", crop ? 1 : 0 },
            { "clip", clip ? 1 : 0 },
            { "outside", outside },
        };
        for (const std::pair<const char*, int>& value : ints) {
            const PXR_NS::UsdShadeInput input = ok ? shader.CreateInput(PXR_NS::TfToken(value.first), PXR_NS::SdfValueTypeNames->Int) : PXR_NS::UsdShadeInput();
            ok = input && input.Set(value.second);
        }
        const PXR_NS::UsdShadeOutput surface = ok ? shader.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeOutput();
        ok = surface && material.CreateSurfaceOutput().ConnectToSource(surface);
        if (!ok) {
            *error = tr("USD could not author the projection shader of the material at %1.").arg(pathString).toStdString();

            return false;
        }

        // The plate, always a texture: the shader samples it at its own coordinates, which Storm
        // generates only for a texture input. Its st is never read but must be connected. Without
        // an image the texture has no file and shows its fallback, black.
        PXR_NS::UsdShadeShader reader = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kUSDProject3DReaderName)));
        PXR_NS::UsdShadeShader texture = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kUSDProject3DTextureName)));
        const PXR_NS::UsdShadeInput varname = reader ? reader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String) : PXR_NS::UsdShadeInput();
        const PXR_NS::UsdShadeOutput readerOutput = reader ? reader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float2) : PXR_NS::UsdShadeOutput();
        ok = reader && reader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float2"))) && varname && varname.Set(std::string("st")) && readerOutput
            && texture && texture.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdUVTexture")));
        const PXR_NS::UsdShadeInput st = ok ? texture.CreateInput(PXR_NS::TfToken("st"), PXR_NS::SdfValueTypeNames->Float2) : PXR_NS::UsdShadeInput();
        ok = st && st.ConnectToSource(readerOutput);
        // The texture covers the image's data window, beyond which the image is transparent black:
        // clamping would stretch its edge pixels across the rest of the frame.
        const PXR_NS::TfToken wrap("black");
        for (const char* name : { "wrapS", "wrapT" }) {
            const PXR_NS::UsdShadeInput input = ok ? texture.CreateInput(PXR_NS::TfToken(name), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeInput();
            ok = input && input.Set(wrap);
        }
        const PXR_NS::UsdShadeInput colorSpace = ok ? texture.CreateInput(PXR_NS::TfToken("sourceColorSpace"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeInput();
        ok = colorSpace && colorSpace.Set(PXR_NS::TfToken("auto"));
        const PXR_NS::UsdShadeOutput rgba = ok ? texture.CreateOutput(PXR_NS::TfToken("rgba"), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeOutput();
        const PXR_NS::UsdShadeInput plateInput = rgba ? shader.CreateInput(PXR_NS::TfToken("plate"), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeInput();
        if (!plateInput || !plateInput.ConnectToSource(rgba)) {
            *error = tr("USD could not author the image texture of the material at %1.").arg(pathString).toStdString();

            return false;
        }

        if (!CyclesNetwork(stage, path).author(material, network)) {
            *error = tr("USD could not author the Cycles projection network of the material at %1.").arg(pathString).toStdString();

            return false;
        }
        if (!MoonRayNetwork(stage, path).author(material, network)) {
            *error = tr("USD could not author the MoonRay projection network of the material at %1.").arg(pathString).toStdString();

            return false;
        }

        return true;
    };
    const std::shared_ptr<ProjectionTopologyMemo> topologyMemo = _imp->topology;
    SceneLayerMemo::AuthorFunc author = [samples, topologyMemo, authorTopology, path, shaderPath, space, useFrameHold, frameHold, projectOn, crop, clip, outside, hasPlate, plate, cameraPath, aspect, fitValue,
                                         framesPerSecond](const SceneLayerEdit& edit, std::string* error) {
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!stage) {
            return false;
        }
        const QString pathString = QString::fromStdString(path.GetString());
        const std::string framePrimvar = useFrameHold ? heldPrimvarName("natronProjectionFrame", frameHold) : std::string("natronProjectionFrame");
        const std::string referencePrimvar = heldPrimvarName("natronReferencePoints", frameHold);
        NetworkProjection network;
        network.space = space;
        network.framePrimvar = framePrimvar;
        if ((space == (int)eProjectionSpaceGeometry) && useFrameHold) {
            network.referencePrimvar = referencePrimvar;
        }
        network.projectOn = projectOn;
        network.crop = crop;
        network.clip = clip;
        network.outside = outside;
        network.hasPlate = hasPlate;
        ContentHasher topologyHasher;
        topologyHasher.appendString(path.GetString());
        topologyHasher.appendString(shaderPath);
        topologyHasher.appendU64((U64)space);
        topologyHasher.appendString(framePrimvar);
        topologyHasher.appendString(network.referencePrimvar);
        topologyHasher.appendU64((U64)projectOn);
        topologyHasher.appendBool(crop);
        topologyHasher.appendBool(clip);
        topologyHasher.appendU64((U64)outside);
        topologyHasher.appendBool(hasPlate);
        const U64 topologyKey = topologyHasher.finalize();
        PXR_NS::SdfLayerRefPtr topology;
        {
            std::lock_guard<std::mutex> l(topologyMemo->lock);
            if (topologyMemo->key == topologyKey) {
                topology = topologyMemo->layer;
            }
        }
        if (!topology) {
            topology = PXR_NS::SdfLayer::CreateAnonymous("natron-projection-topology");
            const PXR_NS::UsdStageRefPtr topologyStage = topology ? PXR_NS::UsdStage::Open(topology, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadNone) : PXR_NS::UsdStageRefPtr();
            if (!topologyStage) {
                *error = tr("USD could not author the projection material at %1.").arg(pathString).toStdString();

                return false;
            }
            if (!authorTopology(topologyStage, path, shaderPath, network, error)) {
                return false;
            }
            std::lock_guard<std::mutex> l(topologyMemo->lock);
            topologyMemo->key = topologyKey;
            topologyMemo->layer = topology;
        }
        {
            PXR_NS::SdfChangeBlock block;
            for (const PXR_NS::SdfPrimSpecHandle& root : topology->GetRootPrims()) {
                if (!PXR_NS::SdfCopySpec(topology, root->GetPath(), edit.layer, root->GetPath())) {
                    *error = tr("USD could not author the projection material at %1.").arg(pathString).toStdString();

                    return false;
                }
            }
        }
        PXR_NS::UsdShadeShader shader(stage->GetPrimAtPath(path.AppendChild(PXR_NS::TfToken(kUSDProject3DShaderName))));
        bool ok = true;
        if (samples->missing) {
            const PXR_NS::UsdPrim prim = shader.GetPrim();
            const PXR_NS::UsdAttribute camera = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCamera), PXR_NS::SdfValueTypeNames->String, true);
            const PXR_NS::UsdAttribute cameraAspect = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCameraAspect), PXR_NS::SdfValueTypeNames->Double, true);
            const PXR_NS::UsdAttribute cameraFit = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCameraFit), PXR_NS::SdfValueTypeNames->Int, true);
            const PXR_NS::UsdAttribute cameraFrames = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCameraFrames), PXR_NS::SdfValueTypeNames->DoubleArray, true);
            const PXR_NS::UsdAttribute cameraRate = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCameraFramesPerSecond), PXR_NS::SdfValueTypeNames->Double, true);
            const PXR_NS::VtDoubleArray frames(samples->deferredFrames.begin(), samples->deferredFrames.end());
            ok = camera && camera.Set(cameraPath) && cameraAspect && cameraAspect.Set(aspect) && cameraFit && cameraFit.Set(fitValue) && cameraFrames && cameraFrames.Set(frames)
                && cameraRate && cameraRate.Set(framesPerSecond);
            if (ok && edit.deferredProjections) {
                *edit.deferredProjections = true;
            }
        }
        // Read by the geometry node the material is grafted under, which takes itself at the same frame.
        if (ok && (space == (int)eProjectionSpaceGeometry) && useFrameHold) {
            const PXR_NS::UsdAttribute hold = shader.GetPrim().CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrFrameHold), PXR_NS::SdfValueTypeNames->Double, true);
            ok = hold && hold.Set(frameHold);
        }
        if (!ok) {
            *error = tr("USD could not author the projection shader of the material at %1.").arg(pathString).toStdString();

            return false;
        }

        const auto inputOf = [&stage, &path](const SceneProjectionCameras::ShaderValue& value) {
            PXR_NS::UsdShadeShader owner(stage->GetPrimAtPath(path.AppendChild(value.shader)));

            return owner ? owner.CreateInput(value.input, value.type).GetAttr() : PXR_NS::UsdAttribute();
        };
        // The texture holds the image's window, the format and the data beyond it; plateTransform
        // maps the format inside it, so overscan projects when Crop is off.
        if (hasPlate) {
            const LiveTextureSource live = SceneNodeAuthoring::recordLiveTexture(edit, plate);
            for (const char* name : { kUSDProject3DTextureName, kSceneProjectionCyclesPlateTexture, kSceneProjectionMoonRayPlateTexture }) {
                PXR_NS::UsdShadeShader owner(stage->GetPrimAtPath(path.AppendChild(PXR_NS::TfToken(name))));
                const PXR_NS::UsdShadeInput file = owner ? owner.CreateInput(PXR_NS::TfToken("file"), PXR_NS::SdfValueTypeNames->Asset) : PXR_NS::UsdShadeInput();
                ok = ok && file && SceneNodeAuthoring::authorValue(file.GetAttr(), edit, live.varying, [&live](double frame) { return PXR_NS::SdfAssetPath(live.getUri(frame)); });
            }
            const auto windowAt = [&live](double frame) {
                double scale[2];
                double translation[2];
                live.getWindow(frame).getUvTransform(scale, translation);

                return SceneProjectionCameras::plateValues(PXR_NS::GfVec4f((float)scale[0], (float)scale[1], (float)translation[0], (float)translation[1]));
            };
            const std::vector<SceneProjectionCameras::ShaderValue> first = windowAt(edit.firstFrame);
            for (std::size_t v = 0; ok && (v < first.size()); ++v) {
                ok = SceneNodeAuthoring::authorValue(inputOf(first[v]), edit, SceneNodeAuthoring::liveTextureWindowVaries(live), [&windowAt, v](double frame) { return windowAt(frame)[v].value; });
            }
        } else {
            for (const SceneProjectionCameras::ShaderValue& value : SceneProjectionCameras::plateValues(PXR_NS::GfVec4f(1.f, 1.f, 0.f, 0.f))) {
                const PXR_NS::UsdAttribute attribute = ok ? inputOf(value) : PXR_NS::UsdAttribute();
                ok = attribute && attribute.Set(value.value);
            }
        }
        if (samples->frames.empty()) {
            for (const SceneProjectionCameras::ShaderValue& value : SceneProjectionCameras::projectorValues(samples->worldToProjector.front(), samples->origin.front())) {
                const PXR_NS::UsdAttribute attribute = ok ? inputOf(value) : PXR_NS::UsdAttribute();
                ok = attribute && attribute.Set(value.value);
            }
        }
        for (std::size_t i = 0; ok && (i < samples->frames.size()); ++i) {
            const PXR_NS::UsdTimeCode time = SceneNodeAuthoring::frameToTimeCode(edit, samples->frames[i]);
            for (const SceneProjectionCameras::ShaderValue& value : SceneProjectionCameras::projectorValues(samples->worldToProjector[i], samples->origin[i])) {
                const PXR_NS::UsdAttribute attribute = ok ? inputOf(value) : PXR_NS::UsdAttribute();
                ok = attribute && attribute.Set(value.value, time);
            }
        }
        if (!ok) {
            *error = tr("USD could not author the projection of the material at %1.").arg(pathString).toStdString();

            return false;
        }

        return true;
    };
    if (!emitSceneLayer(ScenePayload(), reads, author, outputScene, errorMessage)) {
        return fail();
    }

    // Checked on every evaluation: the layer can stay the same while the texture fails.
    QStringList warnings;
    if (!hasPlate) {
        warnings.push_back(tr("Nothing is connected to the Image input: the projection is black."));
    } else {
        const std::string uri = plate.getUri(args.time);
        const std::string failure = SceneNodeAuthoring::getLiveTextureFailure(plate, args.time);
        if (!failure.empty()) {
            warnings.push_back(tr("The live texture %1 shows magenta, the missing-texture colour: %2.").arg(QString::fromStdString(uri)).arg(QString::fromStdString(failure)));
        }
    }
    if (warnings.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());
    }

    return eStatusOK;
} // USDProject3D::renderScene

NATRON_NAMESPACE_EXIT
