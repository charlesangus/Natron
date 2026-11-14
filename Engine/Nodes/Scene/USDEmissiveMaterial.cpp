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

#include "USDEmissiveMaterial.h"

#include <string>

#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
// The names of the prims the node authors under the material.
const char* const kSurfaceName = "PreviewSurface";
const char* const kReaderName = "TexCoordReader";
const char* const kTextureName = "emissiveColorTexture";
const char* const kTransformName = "emissiveColorTransform";
// The layer of the input's image a live texture reads.
const char* const kLiveTextureLayer = "rgba";
} // anonymous namespace

USDEmissiveMaterial::USDEmissiveMaterial(NodePtr node)
    : NativeEffectBase(node)
{
}

USDEmissiveMaterial::~USDEmissiveMaterial()
{
}

const NodeSpec*
USDEmissiveMaterial::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDEmissiveMaterial::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "EmissiveMaterial"));
}

void
USDEmissiveMaterial::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "EmissiveMaterial"),
                                        tr("The path of the material prim the node authors. Its shaders are authored under it."));

    KnobColorPtr color = createKnob<KnobColor>(tr("Emissive Color"), 3);
    color->setName(kUSDEmissiveMaterialParamEmissiveColor);
    color->setHintToolTip(tr("The light the surface gives off, in the project working space, written unchanged. With an "
                             "image on the Emissive input or an Emissive Texture, it multiplies the image: white, the "
                             "default, shows the image as it is."));
    for (int d = 0; d < 3; ++d) {
        color->setDefaultValue(1., d);
    }
    page->addKnob(color);
    _emissiveColor = color;

    KnobFilePtr texture = createKnob<KnobFile>(tr("Emissive Texture"));
    texture->setName(kUSDEmissiveMaterialParamEmissiveTexture);
    texture->setHintToolTip(tr("An image file the surface shows, read at the prims' st texture coordinates, when nothing is "
                               "connected to the Emissive input. Authored even when it does not resolve."));
    texture->setAnimationEnabled(false);
    // The formats USD's Hio image plugins read, since Storm, not a Natron reader, loads the texture.
    texture->setFileExtensions({ "exr", "tif", "tiff", "tx", "png", "jpg", "jpeg", "tga", "bmp", "hdr", "avif" });
    page->addKnob(texture);
    _emissiveTexture = texture;

    KnobBoolPtr useAlpha = createKnob<KnobBool>(tr("Use Alpha"));
    useAlpha->setName(kUSDEmissiveMaterialParamUseAlpha);
    useAlpha->setHintToolTip(tr("When on, the alpha of the image connected to the Emissive input is the surface's opacity "
                                "(inputs:opacity, in the presence mode): where it is transparent the surface shows what is "
                                "behind it, as the image would composited over it. A transparent surface is drawn in the "
                                "renderer's translucent pass, which writes no depth or prim id. When off, the surface is "
                                "opaque."));
    useAlpha->setDefaultValue(true);
    useAlpha->setAnimationEnabled(false);
    page->addKnob(useAlpha);
    _useAlpha = useAlpha;

    SceneNodeAuthoring::createDeletedTextureSourcesKnob(this, page);
} // USDEmissiveMaterial::initializeKnobs

void
USDEmissiveMaterial::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    SceneNodeAuthoring::forgetDeletedTextureSourceIfConnected(*this, inputNo);
}

void
USDEmissiveMaterial::onInputSourceDeleted(int inputNo,
                                          const std::string& sourceName)
{
    SceneNodeAuthoring::recordDeletedTextureSource(*this, inputNo, sourceName);
}

bool
USDEmissiveMaterial::authorLayer(const SceneLayerEdit& edit,
                                 const LiveTextureSource& live,
                                 std::string* errorMessage) const
{
    PXR_NS::SdfPath path;

    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        return false;
    }
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, errorMessage);
    if (!stage) {
        return false;
    }
    KnobColorPtr color = _emissiveColor.lock();
    KnobFilePtr textureKnob = _emissiveTexture.lock();
    KnobBoolPtr useAlpha = _useAlpha.lock();
    if (!color || !textureKnob || !useAlpha) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return false;
    }

    const QString pathString = QString::fromStdString(path.GetString());
    PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterial::Define(stage, path);
    PXR_NS::UsdShadeShader surface = material ? PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kSurfaceName))) : PXR_NS::UsdShadeShader();
    if (!material || !surface || !surface.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPreviewSurface")))) {
        *errorMessage = tr("USD could not define a material at %1.").arg(pathString).toStdString();

        return false;
    }
    const PXR_NS::UsdShadeOutput surfaceOutput = surface.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token);

    using SceneNodeAuthoring::authorValue;
    const bool colorAnimated = SceneNodeAuthoring::isAnimated(color);
    auto colorAt = [&color](double frame) {
        return PXR_NS::GfVec3f((float)color->getValueAtTime(frame, 0), (float)color->getValueAtTime(frame, 1), (float)color->getValueAtTime(frame, 2));
    };

    PXR_NS::UsdShadeInput emissiveInput;
    const bool authored = surfaceOutput && material.CreateSurfaceOutput().ConnectToSource(surfaceOutput)
        && SceneNodeAuthoring::authorUnlitSurfaceInputs(surface, &emissiveInput);
    if (!authored) {
        *errorMessage = tr("USD could not author the surface of the material at %1.").arg(pathString).toStdString();

        return false;
    }

    const bool hasLive = !live.nodeName.empty();
    const std::string file = hasLive ? std::string() : SceneNodeAuthoring::getAnchoredFilePath(textureKnob);
    if (!hasLive && file.empty()) {
        if (!authorValue(emissiveInput.GetAttr(), edit, colorAnimated, colorAt)) {
            *errorMessage = tr("USD could not author the surface of the material at %1.").arg(pathString).toStdString();

            return false;
        }

        return true;
    }

    // The image, read at the "st" primvar (through the window transform of a live texture), its rgb
    // scaled by Emissive Color and fed to the emission.
    PXR_NS::UsdShadeShader reader = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kReaderName)));
    const PXR_NS::UsdShadeInput varname = reader ? reader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String) : PXR_NS::UsdShadeInput();
    const PXR_NS::UsdShadeOutput readerOutput = reader ? reader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float2) : PXR_NS::UsdShadeOutput();
    if (!reader || !reader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float2"))) || !varname || !varname.Set(std::string("st")) || !readerOutput) {
        *errorMessage = tr("USD could not author the texture coordinate reader of the material at %1.").arg(pathString).toStdString();

        return false;
    }
    PXR_NS::UsdShadeShader texture = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kTextureName)));
    bool textured = texture && texture.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdUVTexture")));
    const PXR_NS::UsdShadeInput fileInput = textured ? texture.CreateInput(PXR_NS::TfToken("file"), PXR_NS::SdfValueTypeNames->Asset) : PXR_NS::UsdShadeInput();
    const PXR_NS::UsdShadeInput st = textured ? texture.CreateInput(PXR_NS::TfToken("st"), PXR_NS::SdfValueTypeNames->Float2) : PXR_NS::UsdShadeInput();
    const PXR_NS::UsdShadeInput colorSpace = textured ? texture.CreateInput(PXR_NS::TfToken("sourceColorSpace"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeInput();
    const PXR_NS::UsdShadeInput scale = textured ? texture.CreateInput(PXR_NS::TfToken("scale"), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeInput();
    const PXR_NS::UsdShadeOutput rgb = textured ? texture.CreateOutput(PXR_NS::TfToken("rgb"), PXR_NS::SdfValueTypeNames->Float3) : PXR_NS::UsdShadeOutput();
    if (hasLive) {
        const LiveTextureSource recorded = SceneNodeAuthoring::recordLiveTexture(edit, live);
        textured = textured && fileInput
            && authorValue(fileInput.GetAttr(), edit, recorded.varying, [&recorded](double frame) { return PXR_NS::SdfAssetPath(recorded.getUri(frame)); })
            && SceneNodeAuthoring::connectLiveTextureCoordinates(stage, edit, path.AppendChild(PXR_NS::TfToken(kTransformName)), recorded, readerOutput, st);
    } else {
        textured = textured && fileInput && fileInput.Set(PXR_NS::SdfAssetPath(file))
            && st && st.ConnectToSource(readerOutput);
    }
    textured = textured
        && colorSpace && colorSpace.Set(PXR_NS::TfToken("auto"))
        && scale && authorValue(scale.GetAttr(), edit, colorAnimated, [&colorAt](double frame) {
                   const PXR_NS::GfVec3f c = colorAt(frame);

                   return PXR_NS::GfVec4f(c[0], c[1], c[2], 1.f);
               })
        && rgb && emissiveInput.ConnectToSource(rgb) && (!hasLive || !useAlpha->getValue() || SceneNodeAuthoring::connectOpacityToAlpha(surface, texture));
    if (!textured) {
        *errorMessage = tr("USD could not author the emissive texture of the material at %1.").arg(pathString).toStdString();

        return false;
    }

    return true;
} // USDEmissiveMaterial::authorLayer

StatusEnum
USDEmissiveMaterial::renderScene(const SceneRenderActionArgs& args,
                                 ScenePayload* outputScene,
                                 std::string* errorMessage)
{
    // The input's identity and hash, without rendering it: the layer reads nothing else of it, so
    // an upstream image edit re-authors this layer and no other.
    LiveTextureSource live;
    const bool connected = SceneNodeAuthoring::getLiveTextureSource(*this, kUSDEmissiveMaterialInputEmissive, kLiveTextureLayer, &live);
    ContentHasher facts;
    facts.appendBool(connected);
    if (connected) {
        SceneNodeAuthoring::appendLiveTextureSource(live, &facts);
    } else {
        live = LiveTextureSource();
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    if (!emitSceneLayer(ScenePayload(), reads, [this, &live](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, live, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    // Checked on every evaluation, not only when the layer is authored: a file can appear or
    // vanish, or a live texture fail to render, while the layer stays the same.
    QString warning;
    if (connected) {
        const std::string uri = live.getUri(args.time);
        const std::string failure = SceneNodeAuthoring::getLiveTextureFailure(live, args.time);
        if (!failure.empty()) {
            warning = tr("The live texture %1 shows magenta, the missing-texture colour: %2.").arg(QString::fromStdString(uri)).arg(QString::fromStdString(failure));
        }
    } else {
        const std::string texturePath = SceneNodeAuthoring::getAnchoredFilePath(_emissiveTexture.lock());
        if (!texturePath.empty() && PXR_NS::ArGetResolver().Resolve(texturePath).IsEmpty()) {
            warning = tr("The texture %1 does not resolve; it is authored anyway, and the surface renders without it "
                         "wherever it does not resolve.")
                          .arg(QString::fromStdString(texturePath));
        }
    }
    if (warning.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warning.toStdString());
    }

    return eStatusOK;
} // USDEmissiveMaterial::renderScene

NATRON_NAMESPACE_EXIT
