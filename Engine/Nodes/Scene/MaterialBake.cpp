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

#include "MaterialBake.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

#include <QThreadPool>

#include <OpenImageIO/imageio.h>

#include "Engine/AppManager.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace MaterialBake {
namespace {
    const char* const kUdimToken = "<UDIM>";

    struct PurposeInfo {
        const char* name;
        bool colour;
        // UsdPreviewSurface's own default for the input.
        float fallback;
    };

    const PurposeInfo kPurposes[] = {
        { kMaterialBakePurposeDiffuseColor, true, 0.18f },
        { "emissiveColor", true, 0.f },
        { "specularColor", true, 0.f },
        { "metallic", false, 0.f },
        { "roughness", false, 0.5f },
        { "clearcoat", false, 0.f },
        { "clearcoatRoughness", false, 0.01f },
        { "opacity", false, 1.f },
        { "occlusion", false, 1.f },
    };

    const PurposeInfo*
    purposeInfo(const std::string& name)
    {
        for (const PurposeInfo& info : kPurposes) {
            if (name == info.name) {
                return &info;
            }
        }

        return 0;
    }

    bool
    fail(std::string* error,
         const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return false;
    }

    bool
    isCancelled(const std::function<bool()>& cancelled,
                std::string* error)
    {
        if (cancelled && cancelled()) {
            if (error) {
                error->clear();
            }

            return true;
        }

        return false;
    }

    std::string
    stripPrimvars(const std::string& name)
    {
        const std::string prefix("primvars:");

        return (name.compare(0, prefix.size(), prefix) == 0) ? name.substr(prefix.size()) : name;
    }

    std::string
    lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });

        return text;
    }

    float
    srgbToLinear(float v)
    {
        return (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }

    // Row-major and bottom-up straight RGBA, from a file or a live texture without a copy.
    struct ImageData {
        int width = 0;
        int height = 0;
        std::vector<float> rgba;
        LiveTextureBufferConstPtr live;

        const float* row(int y) const
        {
            return live ? live->getRow(y) : rgba.data() + (std::size_t)y * (std::size_t)width * 4;
        }
    };

    typedef std::shared_ptr<const ImageData> ImageDataConstPtr;

    enum WrapEnum {
        eWrapBlack = 0,
        eWrapClamp,
        eWrapRepeat,
        eWrapMirror
    };

    enum ColorSpaceEnum {
        eColorSpaceRaw = 0,
        eColorSpaceSRGB,
        eColorSpaceAuto
    };

    WrapEnum
    wrapFromName(const std::string& name,
                 bool* known)
    {
        *known = true;
        if (name == "clamp") {
            return eWrapClamp;
        }
        if ((name == "repeat") || (name == "periodic")) {
            return eWrapRepeat;
        }
        if (name == "mirror") {
            return eWrapMirror;
        }
        *known = (name == "black");

        return eWrapBlack;
    }

    // The file's own wrap modes, as OIIO reads them ("s,t" or one for both).
    void
    fileWrapModes(const OIIO::ImageSpec& spec,
                  WrapEnum* s,
                  WrapEnum* t)
    {
        const std::string modes = spec.get_string_attribute("wrapmodes");
        const std::size_t comma = modes.find(',');
        bool known = false;

        *s = wrapFromName(modes.substr(0, comma), &known);
        *t = (comma == std::string::npos) ? *s : wrapFromName(modes.substr(comma + 1), &known);
    }

    bool
    readImageFile(const std::string& path,
                  std::vector<float>* rgba,
                  int* width,
                  int* height,
                  OIIO::ImageSpec* headerSpec,
                  std::string* error)
    {
        std::error_code ec;

        if (!std::filesystem::is_regular_file(path, ec)) {
            return fail(error, "The file " + path + " is missing.");
        }
        std::string readError;
        const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &readError, FilePin::eDisk);
        if (!header || header->subimages.empty()) {
            return fail(error, "The file " + path + " could not be read: " + readError);
        }
        const OIIO::ImageSpec& spec = header->subimages.front();
        const RectI window = OiioReadSupport::dataWindowOf(spec);
        const int channels = std::min(spec.nchannels, 4);
        if (window.isNull() || (channels <= 0)) {
            return fail(error, "The file " + path + " holds no pixels.");
        }
        const std::size_t w = (std::size_t)window.width();
        const std::size_t h = (std::size_t)window.height();
        std::vector<float> raw(w * h * (std::size_t)channels);
        if (!OiioReadSupport::decode(path, 0, 0, channels, window, raw.data(), w * (std::size_t)channels, &readError, FilePin::eDisk)) {
            return fail(error, "The file " + path + " could not be read: " + readError);
        }
        rgba->assign(w * h * 4, 0.f);
        for (std::size_t i = 0; i < w * h; ++i) {
            const float* in = raw.data() + i * (std::size_t)channels;
            float* out = rgba->data() + i * 4;
            const bool grey = channels <= 2;
            out[0] = in[0];
            out[1] = grey ? in[0] : in[1];
            out[2] = grey ? in[0] : in[2];
            out[3] = (channels == 2) ? in[1] : ((channels == 4) ? in[3] : 1.f);
        }
        *width = (int)w;
        *height = (int)h;
        if (headerSpec) {
            *headerSpec = spec;
        }

        return true;
    } // readImageFile

    // Whether a file's colour is sRGB under sourceColorSpace "auto": its colour space says so, or
    // it is 8-bit RGB or RGBA with nothing said.
    bool
    autoIsSRGB(const OIIO::ImageSpec& spec)
    {
        const std::string space = lower(spec.get_string_attribute("oiio:ColorSpace"));

        if (!space.empty()) {
            return space.find("srgb") != std::string::npos;
        }

        return (spec.format.basetype == OIIO::TypeDesc::UINT8) && ((spec.nchannels == 3) || (spec.nchannels == 4));
    }

    void
    linearize(std::vector<float>* rgba)
    {
        for (std::size_t i = 0; i < rgba->size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                (*rgba)[i + c] = srgbToLinear((*rgba)[i + c]);
            }
        }
    }

    bool
    wrapIndex(int i,
              int n,
              WrapEnum wrap,
              int* out)
    {
        if ((i >= 0) && (i < n)) {
            *out = i;

            return true;
        }
        switch (wrap) {
        case eWrapClamp:
            *out = std::min(std::max(i, 0), n - 1);

            return true;
        case eWrapRepeat:
            *out = ((i % n) + n) % n;

            return true;
        case eWrapMirror: {
            const int period = 2 * n;
            const int m = ((i % period) + period) % period;
            *out = (m < n) ? m : (period - 1 - m);

            return true;
        }
        case eWrapBlack:
        default:
            return false;
        }
    }

    PXR_NS::GfVec4f
    sampleBilinear(const ImageData& image,
                   float s,
                   float t,
                   WrapEnum wrapS,
                   WrapEnum wrapT)
    {
        PXR_NS::GfVec4f sum(0.f);

        if ((image.width <= 0) || (image.height <= 0) || !std::isfinite(s) || !std::isfinite(t)) {
            return sum;
        }
        const double x = (double)s * image.width - 0.5;
        const double y = (double)t * image.height - 0.5;
        const double fx0 = std::floor(x);
        const double fy0 = std::floor(y);
        // Coordinates this far out would overflow the texel indices.
        if ((std::abs(fx0) > 1e9) || (std::abs(fy0) > 1e9)) {
            return sum;
        }
        const int x0 = (int)fx0;
        const int y0 = (int)fy0;
        const float ax = (float)(x - fx0);
        const float ay = (float)(y - fy0);
        const float wx[2] = { 1.f - ax, ax };
        const float wy[2] = { 1.f - ay, ay };
        for (int j = 0; j < 2; ++j) {
            int row = 0;
            if ((wy[j] == 0.f) || !wrapIndex(y0 + j, image.height, wrapT, &row)) {
                continue;
            }
            const float* pixels = image.row(row);
            for (int i = 0; i < 2; ++i) {
                int column = 0;
                if ((wx[i] == 0.f) || !wrapIndex(x0 + i, image.width, wrapS, &column)) {
                    continue;
                }
                const float* texel = pixels + (std::size_t)column * 4;
                const float weight = wx[i] * wy[j];
                for (int c = 0; c < 4; ++c) {
                    sum[c] += texel[c] * weight;
                }
            }
        }

        return sum;
    } // sampleBilinear

    // The tiles of a <UDIM> file texture, read on first use from any thread.
    struct UdimImages {
        std::string pattern;
        ColorSpaceEnum colorSpace = eColorSpaceAuto;
        std::mutex mutex;
        std::map<int, ImageDataConstPtr> images;
        std::set<std::string> warnings;

        ImageDataConstPtr get(int tile)
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::map<int, ImageDataConstPtr>::const_iterator found = images.find(tile);

            if (found != images.end()) {
                return found->second;
            }
            ImageDataConstPtr& slot = images[tile];
            std::string path = pattern;
            path.replace(path.find(kUdimToken), std::string(kUdimToken).size(), std::to_string(tile));
            std::error_code ec;
            // A UDIM set leaves out the tiles it has nothing for: those read as the fallback.
            if (!std::filesystem::exists(path, ec)) {
                return slot;
            }
            std::shared_ptr<ImageData> image = std::make_shared<ImageData>();
            OIIO::ImageSpec spec;
            std::string error;
            if (!readImageFile(path, &image->rgba, &image->width, &image->height, &spec, &error)) {
                warnings.insert(error + " Its texture's fallback stands in for it.");

                return slot;
            }
            if ((colorSpace == eColorSpaceSRGB) || ((colorSpace == eColorSpaceAuto) && autoIsSRGB(spec))) {
                linearize(&image->rgba);
            }
            slot = image;

            return slot;
        }
    };

    // Where a texture reads its coordinates, then the UsdTransform2d it may go through.
    struct StSource {
        enum KindEnum {
            eKindTile = 0,
            eKindReader,
            eKindConstant
        };

        KindEnum kind = eKindConstant;
        PXR_NS::GfVec2f constant = PXR_NS::GfVec2f(0.f);
        bool transformed = false;
        float matrix[4] = { 1.f, 0.f, 0.f, 1.f };
        PXR_NS::GfVec2f translation = PXR_NS::GfVec2f(0.f);

        PXR_NS::GfVec2f apply(const PXR_NS::GfVec2f& st) const
        {
            if (!transformed) {
                return st;
            }

            return PXR_NS::GfVec2f(matrix[0] * st[0] + matrix[1] * st[1] + translation[0],
                                   matrix[2] * st[0] + matrix[3] * st[1] + translation[1]);
        }
    };

    struct Texture {
        ImageDataConstPtr image;
        std::shared_ptr<UdimImages> udim;
        WrapEnum wrapS = eWrapBlack;
        WrapEnum wrapT = eWrapBlack;
        PXR_NS::GfVec4f fallback = PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f);
        PXR_NS::GfVec4f scale = PXR_NS::GfVec4f(1.f);
        PXR_NS::GfVec4f bias = PXR_NS::GfVec4f(0.f);
        StSource st;

        PXR_NS::GfVec4f sample(const PXR_NS::GfVec2f& coords) const
        {
            PXR_NS::GfVec4f value;

            if (udim) {
                const double u = std::floor((double)coords[0]);
                const double v = std::floor((double)coords[1]);
                ImageDataConstPtr tileImage;
                if ((u >= 0.) && (u < 10.) && (v >= 0.) && (v < 1000.)) {
                    tileImage = udim->get(1001 + (int)u + 10 * (int)v);
                }
                if (!tileImage) {
                    return fallback;
                }
                value = sampleBilinear(*tileImage, (float)(coords[0] - u), (float)(coords[1] - v), eWrapClamp, eWrapClamp);
            } else if (image) {
                value = sampleBilinear(*image, coords[0], coords[1], wrapS, wrapT);
            } else {
                return fallback;
            }
            for (int c = 0; c < 4; ++c) {
                value[c] = value[c] * scale[c] + bias[c];
            }

            return value;
        }
    };

    typedef std::shared_ptr<const Texture> TextureConstPtr;

    // What a texel of the tile is evaluated at.
    struct Texel {
        PXR_NS::GfVec2f st;
        PXR_NS::GfVec2f readerSt;
        PXR_NS::GfVec3d position;
        PXR_NS::GfVec3d normal;
    };

    // A texture's output: all four channels, rgb with alpha 1, or one channel spread over RGB.
    struct TextureOutput {
        TextureConstPtr texture;
        // -1 for rgba, -2 for rgb, else the channel.
        int channel = -1;

        PXR_NS::GfVec4f at(const PXR_NS::GfVec2f& coords) const
        {
            const PXR_NS::GfVec4f v = texture->sample(coords);

            if (channel == -1) {
                return v;
            }
            if (channel == -2) {
                return PXR_NS::GfVec4f(v[0], v[1], v[2], 1.f);
            }

            return PXR_NS::GfVec4f(v[channel], v[channel], v[channel], 1.f);
        }

        PXR_NS::GfVec4f at(const Texel& texel) const
        {
            const StSource& st = texture->st;
            const PXR_NS::GfVec2f coords = (st.kind == StSource::eKindTile) ? texel.st : ((st.kind == StSource::eKindReader) ? texel.readerSt : st.constant);

            return at(st.apply(coords));
        }
    };

    // The natronProject3D.glslfx surface, evaluated at a texel of one mesh.
    struct Projection {
        PXR_NS::GfMatrix4d toReference = PXR_NS::GfMatrix4d(1.);
        PXR_NS::GfMatrix4d worldToProjector = PXR_NS::GfMatrix4d(1.);
        PXR_NS::GfVec4d originWorld = PXR_NS::GfVec4d(0., 0., 0., 1.);
        PXR_NS::GfVec4f window = PXR_NS::GfVec4f(1.f, 1.f, 0.f, 0.f);
        int projectOn = 0;
        int crop = 1;
        int clip = 0;
        int outside = 0;
        TextureOutput plate;
        PXR_NS::GfVec4f constantPlate = PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f);

        PXR_NS::GfVec4f outsideValue() const
        {
            return (outside == 1) ? PXR_NS::GfVec4f(0.f) : PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f);
        }

        PXR_NS::GfVec4f at(const Texel& texel) const
        {
            const PXR_NS::GfVec4d world(texel.position[0], texel.position[1], texel.position[2], 1.);
            const PXR_NS::GfVec4d c = (world * toReference) * worldToProjector;
            const double w = (c[3] > 0.) ? c[3] : 1.;
            const PXR_NS::GfVec2d frame(c[0] / w, c[1] / w);
            const PXR_NS::GfVec2f uv((float)(frame[0] * window[0] + window[2]), (float)(frame[1] * window[1] + window[3]));

            if (projectOn != 0) {
                const PXR_NS::GfVec3d toProjector(originWorld[0] - texel.position[0] * originWorld[3],
                                                  originWorld[1] - texel.position[1] * originWorld[3],
                                                  originWorld[2] - texel.position[2] * originWorld[3]);
                const bool facing = PXR_NS::GfDot(texel.normal, toProjector) > 0.;
                if (facing != (projectOn == 1)) {
                    return outsideValue();
                }
            }
            if (c[3] <= 0.) {
                return outsideValue();
            }
            const double depth = c[2] / c[3];
            if ((clip != 0) && ((depth < -1.) || (depth > 1.))) {
                return outsideValue();
            }
            if ((crop != 0) && ((frame[0] < 0.) || (frame[1] < 0.) || (frame[0] > 1.) || (frame[1] > 1.))) {
                return PXR_NS::GfVec4f(0.f);
            }
            if ((uv[0] < 0.f) || (uv[1] < 0.f) || (uv[0] > 1.f) || (uv[1] > 1.f)) {
                return PXR_NS::GfVec4f(0.f);
            }
            const PXR_NS::GfVec4f p = plate.texture ? plate.at(uv) : constantPlate;
            if (p[3] <= 0.f) {
                return PXR_NS::GfVec4f(0.f);
            }

            return PXR_NS::GfVec4f(p[0] * p[3], p[1] * p[3], p[2] * p[3], p[3]);
        } // at
    };

    // Where one purpose's value comes from.
    struct Source {
        enum KindEnum {
            eKindConstant = 0,
            eKindTexture,
            eKindProjection
        };

        KindEnum kind = eKindConstant;
        // Spread as texture outputs are: a scalar over RGB with alpha 1.
        PXR_NS::GfVec4f constant = PXR_NS::GfVec4f(0.f);
        TextureOutput texture;
        std::shared_ptr<const Projection> projection;
        bool colour = true;

        // What the purpose's file holds at texel.
        PXR_NS::GfVec4f at(const Texel& texel) const
        {
            PXR_NS::GfVec4f v;

            switch (kind) {
            case eKindProjection:
                return projection->at(texel);
            case eKindTexture:
                v = texture.at(texel);
                break;
            case eKindConstant:
            default:
                v = constant;
                break;
            }

            return colour ? PXR_NS::GfVec4f(v[0], v[1], v[2], 1.f) : PXR_NS::GfVec4f(v[0], v[0], v[0], 1.f);
        }
    };

    bool
    toVec4(const PXR_NS::VtValue& value,
           PXR_NS::GfVec4f* out)
    {
        if (value.IsHolding<float>()) {
            const float f = value.UncheckedGet<float>();
            *out = PXR_NS::GfVec4f(f, f, f, 1.f);
        } else if (value.IsHolding<double>()) {
            const float f = (float)value.UncheckedGet<double>();
            *out = PXR_NS::GfVec4f(f, f, f, 1.f);
        } else if (value.IsHolding<int>()) {
            const float f = (float)value.UncheckedGet<int>();
            *out = PXR_NS::GfVec4f(f, f, f, 1.f);
        } else if (value.IsHolding<PXR_NS::GfVec2f>()) {
            const PXR_NS::GfVec2f v = value.UncheckedGet<PXR_NS::GfVec2f>();
            *out = PXR_NS::GfVec4f(v[0], v[1], 0.f, 1.f);
        } else if (value.IsHolding<PXR_NS::GfVec3f>()) {
            const PXR_NS::GfVec3f v = value.UncheckedGet<PXR_NS::GfVec3f>();
            *out = PXR_NS::GfVec4f(v[0], v[1], v[2], 1.f);
        } else if (value.IsHolding<PXR_NS::GfVec3d>()) {
            const PXR_NS::GfVec3d v = value.UncheckedGet<PXR_NS::GfVec3d>();
            *out = PXR_NS::GfVec4f((float)v[0], (float)v[1], (float)v[2], 1.f);
        } else if (value.IsHolding<PXR_NS::GfVec4f>()) {
            *out = value.UncheckedGet<PXR_NS::GfVec4f>();
        } else if (value.IsHolding<PXR_NS::GfVec4d>()) {
            *out = PXR_NS::GfVec4f(value.UncheckedGet<PXR_NS::GfVec4d>());
        } else {
            return false;
        }

        return true;
    }

    // What feeds input: a shader's output, or a value (its own or a material interface's).
    struct Resolved {
        PXR_NS::UsdShadeShader shader;
        PXR_NS::TfToken output;
        PXR_NS::VtValue value;
    };

    Resolved
    resolveInput(const PXR_NS::UsdShadeInput& input,
                 PXR_NS::UsdTimeCode time)
    {
        Resolved resolved;

        if (!input) {
            return resolved;
        }
        const PXR_NS::UsdShadeAttributeVector attributes = input.GetValueProducingAttributes(false);
        if (attributes.empty()) {
            input.Get(&resolved.value, time);

            return resolved;
        }
        const PXR_NS::UsdAttribute& attribute = attributes.front();
        const std::pair<PXR_NS::TfToken, PXR_NS::UsdShadeAttributeType> name = PXR_NS::UsdShadeUtils::GetBaseNameAndType(attribute.GetName());
        if (name.second == PXR_NS::UsdShadeAttributeType::Output) {
            resolved.shader = PXR_NS::UsdShadeShader(attribute.GetPrim());
            resolved.output = name.first;
        } else {
            attribute.Get(&resolved.value, time);
        }

        return resolved;
    }

    template <typename T>
    T
    inputValue(const PXR_NS::UsdShadeShader& shader,
               const char* name,
               const T& fallback,
               PXR_NS::UsdTimeCode time)
    {
        const Resolved resolved = resolveInput(shader.GetInput(PXR_NS::TfToken(name)), time);

        return resolved.value.IsHolding<T>() ? resolved.value.UncheckedGet<T>() : fallback;
    }

    // A token input that may be authored as a string too, as older files do.
    std::string
    inputText(const PXR_NS::UsdShadeShader& shader,
              const char* name,
              const std::string& fallback,
              PXR_NS::UsdTimeCode time)
    {
        const Resolved resolved = resolveInput(shader.GetInput(PXR_NS::TfToken(name)), time);

        if (resolved.value.IsHolding<PXR_NS::TfToken>()) {
            return resolved.value.UncheckedGet<PXR_NS::TfToken>().GetString();
        }
        if (resolved.value.IsHolding<std::string>()) {
            return resolved.value.UncheckedGet<std::string>();
        }

        return fallback;
    }

    PXR_NS::TfToken
    shaderId(const PXR_NS::UsdShadeShader& shader)
    {
        PXR_NS::TfToken id;

        if (shader) {
            (void)shader.GetShaderId(&id);
        }

        return id;
    }

    bool
    isLiveUri(const std::string& path)
    {
        const std::string scheme = std::string(kLiveTextureScheme) + "://";

        return path.compare(0, scheme.size(), scheme) == 0;
    }

    // The input's file path, anchored to the layer that authored it when relative: USD resolves no
    // path holding <UDIM>, and a relative one means relative to that layer.
    std::string
    filePath(const PXR_NS::UsdShadeShader& shader,
             PXR_NS::UsdTimeCode time)
    {
        const PXR_NS::UsdShadeInput input = shader.GetInput(PXR_NS::TfToken("file"));
        const PXR_NS::UsdShadeAttributeVector attributes = input ? input.GetValueProducingAttributes(false) : PXR_NS::UsdShadeAttributeVector();
        const PXR_NS::UsdAttribute attribute = attributes.empty() ? (input ? input.GetAttr() : PXR_NS::UsdAttribute()) : attributes.front();
        PXR_NS::SdfAssetPath asset;

        if (!attribute || !attribute.Get(&asset, time)) {
            return std::string();
        }
        const std::string authored = asset.GetAssetPath();
        if (authored.empty() || isLiveUri(authored)) {
            return authored;
        }
        if (!asset.GetResolvedPath().empty()) {
            return asset.GetResolvedPath();
        }
        for (const PXR_NS::SdfPropertySpecHandle& spec : attribute.GetPropertyStack(time)) {
            if (spec && spec->GetLayer()) {
                return PXR_NS::SdfComputeAssetPathRelativeToLayer(spec->GetLayer(), authored);
            }
        }

        return authored;
    }

    // Builds the sources of one mesh's material and gathers what they need from the bake.
    class MaterialReader {
    public:
        MaterialReader(const PXR_NS::UsdPrim& mesh,
                       const PXR_NS::TfToken& stName,
                       PXR_NS::UsdTimeCode time)
            : _mesh(mesh)
            , _stName(stripPrimvars(stName.GetString()))
            , _time(time)
        {
        }

        // The second texture coordinate primvar a texture reads, empty when none.
        const PXR_NS::TfToken& readerStName() const
        {
            return _readerSt;
        }

        const std::set<std::string>& warnings() const
        {
            return _warnings;
        }

        void collectUdimWarnings()
        {
            for (const std::shared_ptr<UdimImages>& set : _udims) {
                std::lock_guard<std::mutex> lock(set->mutex);
                _warnings.insert(set->warnings.begin(), set->warnings.end());
            }
        }

        // The value of input, as a purpose of info reads it, or its default.
        Source inputSource(const PXR_NS::UsdShadeShader& surface,
                           const PurposeInfo& info)
        {
            Source source;
            source.colour = info.colour;
            source.constant = PXR_NS::GfVec4f(info.fallback, info.fallback, info.fallback, 1.f);
            const Resolved resolved = resolveInput(surface.GetInput(PXR_NS::TfToken(info.name)), _time);
            if (!resolved.shader) {
                PXR_NS::GfVec4f value;
                if (!resolved.value.IsEmpty() && toVec4(resolved.value, &value)) {
                    source.constant = value;
                }

                return source;
            }
            if (!textureOutput(resolved.shader, resolved.output, &source.texture)) {
                _warnings.insert(describe(resolved.shader) + " feeds " + info.name + " of " + _mesh.GetPath().GetString() + ", which the bake does not evaluate: the input's default stands in.");

                return source;
            }
            source.kind = Source::eKindTexture;

            return source;
        }

        Source projectionSource(const PXR_NS::UsdShadeShader& shader)
        {
            std::shared_ptr<Projection> projection = std::make_shared<Projection>();
            const int space = inputValue<int>(shader, kUSDProject3DInputProjectionSpace, 0, _time);
            const PXR_NS::GfMatrix4d geometryFrame = inputValue<PXR_NS::GfMatrix4d>(shader, kUSDProject3DInputGeometryFrame, PXR_NS::GfMatrix4d(1.), _time);
            if (space == 1) {
                const PXR_NS::GfMatrix4d localToWorld = PXR_NS::UsdGeomXformCache(_time).GetLocalToWorldTransform(_mesh);
                projection->toReference = localToWorld.GetInverse() * geometryFrame;
            }
            projection->worldToProjector = inputValue<PXR_NS::GfMatrix4d>(shader, kUSDProject3DInputWorldToProjector, PXR_NS::GfMatrix4d(1.), _time);
            const PXR_NS::GfVec4f origin = inputValue<PXR_NS::GfVec4f>(shader, kUSDProject3DInputProjectorOrigin, PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f), _time);
            projection->originWorld = PXR_NS::GfVec4d(origin) * projection->toReference.GetInverse();
            projection->window = inputValue<PXR_NS::GfVec4f>(shader, kUSDProject3DInputPlateTransform, PXR_NS::GfVec4f(1.f, 1.f, 0.f, 0.f), _time);
            projection->projectOn = inputValue<int>(shader, "projectOn", 0, _time);
            projection->crop = inputValue<int>(shader, "crop", 1, _time);
            projection->clip = inputValue<int>(shader, "clip", 0, _time);
            projection->outside = inputValue<int>(shader, "outside", 0, _time);

            const Resolved plate = resolveInput(shader.GetInput(PXR_NS::TfToken("plate")), _time);
            if (plate.shader) {
                if (!textureOutput(plate.shader, plate.output, &projection->plate)) {
                    _warnings.insert(describe(plate.shader) + " feeds the plate of " + shader.GetPath().GetString() + ", which the bake does not evaluate: the plate is opaque black.");
                }
            } else if (!plate.value.IsEmpty()) {
                (void)toVec4(plate.value, &projection->constantPlate);
            }

            Source source;
            source.kind = Source::eKindProjection;
            source.projection = projection;

            return source;
        }

        void warn(const std::string& warning)
        {
            _warnings.insert(warning);
        }

    private:
        static std::string describe(const PXR_NS::UsdShadeShader& shader)
        {
            const PXR_NS::TfToken id = shaderId(shader);

            return shader.GetPath().GetString() + (id.IsEmpty() ? std::string() : " (" + id.GetString() + ")");
        }

        bool textureOutput(const PXR_NS::UsdShadeShader& shader,
                           const PXR_NS::TfToken& output,
                           TextureOutput* out)
        {
            if (shaderId(shader) != PXR_NS::TfToken("UsdUVTexture")) {
                return false;
            }
            const std::string name = output.GetString();
            if (name == "rgba") {
                out->channel = -1;
            } else if (name == "rgb") {
                out->channel = -2;
            } else if ((name.size() == 1) && (std::string("rgba").find(name[0]) != std::string::npos)) {
                out->channel = (int)std::string("rgba").find(name[0]);
            } else {
                return false;
            }
            out->texture = texture(shader);

            return true;
        }

        TextureConstPtr texture(const PXR_NS::UsdShadeShader& shader)
        {
            std::map<PXR_NS::SdfPath, TextureConstPtr>::const_iterator found = _textures.find(shader.GetPath());

            if (found != _textures.end()) {
                return found->second;
            }
            std::shared_ptr<Texture> texture = std::make_shared<Texture>();
            texture->fallback = inputValue<PXR_NS::GfVec4f>(shader, "fallback", PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f), _time);
            texture->scale = inputValue<PXR_NS::GfVec4f>(shader, "scale", PXR_NS::GfVec4f(1.f), _time);
            texture->bias = inputValue<PXR_NS::GfVec4f>(shader, "bias", PXR_NS::GfVec4f(0.f), _time);
            const std::string space = inputText(shader, "sourceColorSpace", "auto", _time);
            const ColorSpaceEnum colorSpace = (space == "raw") ? eColorSpaceRaw : ((space == "sRGB") ? eColorSpaceSRGB : eColorSpaceAuto);
            const std::string wrapS = inputText(shader, "wrapS", "useMetadata", _time);
            const std::string wrapT = inputText(shader, "wrapT", "useMetadata", _time);
            texture->st = stSource(shader);

            const std::string path = filePath(shader, _time);
            OIIO::ImageSpec spec;
            bool haveSpec = false;
            if (path.empty()) {
                _warnings.insert(shader.GetPath().GetString() + " names no file: its fallback stands in for it.");
            } else if (path.find(kUdimToken) != std::string::npos) {
                std::shared_ptr<UdimImages> set = std::make_shared<UdimImages>();
                set->pattern = path;
                set->colorSpace = colorSpace;
                texture->udim = set;
                _udims.push_back(set);
            } else if (isLiveUri(path)) {
                const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().find(path);
                if (!buffer) {
                    const std::string reason = LiveTextureStore::instance().getFailure(path);
                    _warnings.insert(path + " is not rendered" + (reason.empty() ? std::string() : ": " + reason) + ". The fallback of " + shader.GetPath().GetString() + " stands in for it.");
                } else {
                    std::shared_ptr<ImageData> image = std::make_shared<ImageData>();
                    image->width = buffer->getWidth();
                    image->height = buffer->getHeight();
                    if (colorSpace == eColorSpaceSRGB) {
                        image->rgba.reserve((std::size_t)image->width * (std::size_t)image->height * 4);
                        for (int y = 0; y < image->height; ++y) {
                            const float* row = buffer->getRow(y);
                            image->rgba.insert(image->rgba.end(), row, row + (std::size_t)image->width * 4);
                        }
                        linearize(&image->rgba);
                    } else {
                        image->live = buffer;
                    }
                    texture->image = image;
                }
            } else {
                std::shared_ptr<ImageData> image = std::make_shared<ImageData>();
                std::string error;
                if (!readImageFile(path, &image->rgba, &image->width, &image->height, &spec, &error)) {
                    _warnings.insert(error + " The fallback of " + shader.GetPath().GetString() + " stands in for it.");
                } else {
                    haveSpec = true;
                    if ((colorSpace == eColorSpaceSRGB) || ((colorSpace == eColorSpaceAuto) && autoIsSRGB(spec))) {
                        linearize(&image->rgba);
                    }
                    texture->image = image;
                }
            }

            WrapEnum metadataS = eWrapBlack;
            WrapEnum metadataT = eWrapBlack;
            if (haveSpec) {
                fileWrapModes(spec, &metadataS, &metadataT);
            }
            bool known = false;
            texture->wrapS = (wrapS == "useMetadata") ? metadataS : wrapFromName(wrapS, &known);
            texture->wrapT = (wrapT == "useMetadata") ? metadataT : wrapFromName(wrapT, &known);

            _textures[shader.GetPath()] = texture;

            return texture;
        } // texture

        StSource stSource(const PXR_NS::UsdShadeShader& texture)
        {
            StSource st;
            Resolved resolved = resolveInput(texture.GetInput(PXR_NS::TfToken("st")), _time);

            if (resolved.shader && (shaderId(resolved.shader) == PXR_NS::TfToken("UsdTransform2d"))) {
                const PXR_NS::UsdShadeShader transform = resolved.shader;
                const float degrees = inputValue<float>(transform, "rotation", 0.f, _time);
                const PXR_NS::GfVec2f scale = inputValue<PXR_NS::GfVec2f>(transform, "scale", PXR_NS::GfVec2f(1.f), _time);
                const double radians = degrees * M_PI / 180.;
                const float c = (float)std::cos(radians);
                const float s = (float)std::sin(radians);
                st.transformed = true;
                st.matrix[0] = c * scale[0];
                st.matrix[1] = -s * scale[1];
                st.matrix[2] = s * scale[0];
                st.matrix[3] = c * scale[1];
                st.translation = inputValue<PXR_NS::GfVec2f>(transform, "translation", PXR_NS::GfVec2f(0.f), _time);
                resolved = resolveInput(transform.GetInput(PXR_NS::TfToken("in")), _time);
            }
            if (!resolved.shader) {
                if (resolved.value.IsHolding<PXR_NS::GfVec2f>()) {
                    st.constant = resolved.value.UncheckedGet<PXR_NS::GfVec2f>();
                }

                return st;
            }
            if (shaderId(resolved.shader) != PXR_NS::TfToken("UsdPrimvarReader_float2")) {
                _warnings.insert(describe(resolved.shader) + " feeds the coordinates of " + texture.GetPath().GetString() + ", which the bake does not evaluate: the texture reads at (0, 0).");

                return st;
            }
            const std::string varname = stripPrimvars(inputText(resolved.shader, "varname", std::string(), _time));
            if (varname.empty() || (varname == _stName)) {
                st.kind = StSource::eKindTile;

                return st;
            }
            const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(_mesh).GetPrimvar(PXR_NS::TfToken(varname));
            if (!primvar || !primvar.HasValue()) {
                st.constant = inputValue<PXR_NS::GfVec2f>(resolved.shader, "fallback", PXR_NS::GfVec2f(0.f), _time);

                return st;
            }
            if (_readerSt.IsEmpty()) {
                _readerSt = PXR_NS::TfToken(varname);
            }
            if (_readerSt.GetString() != varname) {
                _warnings.insert(texture.GetPath().GetString() + " reads the primvar " + varname + " while another texture of " + _mesh.GetPath().GetString() + " reads " + _readerSt.GetString() + ": the bake reads one other than the layout's, so it reads " + _stName + " there.");
                st.kind = StSource::eKindTile;

                return st;
            }
            st.kind = StSource::eKindReader;

            return st;
        } // stSource

        PXR_NS::UsdPrim _mesh;
        std::string _stName;
        PXR_NS::UsdTimeCode _time;
        PXR_NS::TfToken _readerSt;
        std::map<PXR_NS::SdfPath, TextureConstPtr> _textures;
        std::vector<std::shared_ptr<UdimImages>> _udims;
        std::set<std::string> _warnings;
    };

    PXR_NS::UsdShadeShader
    boundSurface(const PXR_NS::UsdPrim& prim)
    {
        const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(PXR_NS::UsdShadeTokens->preview);

        return material ? material.ComputeSurfaceSource() : PXR_NS::UsdShadeShader();
    }

    bool
    isPreviewSurface(const PXR_NS::UsdShadeShader& shader)
    {
        return shaderId(shader) == PXR_NS::TfToken("UsdPreviewSurface");
    }

    class PreviewSurfaceBackend
        : public Backend {
    public:
        virtual void addPurposes(const PXR_NS::UsdStageRefPtr& stage,
                                 const PXR_NS::SdfPath& mesh,
                                 PXR_NS::UsdTimeCode /*time*/,
                                 std::set<std::string>* purposes) const OVERRIDE FINAL
        {
            const PXR_NS::UsdShadeShader surface = boundSurface(stage ? stage->GetPrimAtPath(mesh) : PXR_NS::UsdPrim());

            if (!isPreviewSurface(surface)) {
                return;
            }
            for (const std::string& purpose : optionalPurposes()) {
                const PXR_NS::UsdShadeInput input = surface.GetInput(PXR_NS::TfToken(purpose));
                if (input && (input.HasConnectedSource() || input.GetAttr().HasAuthoredValue())) {
                    purposes->insert(purpose);
                }
            }
        }

        virtual bool bakeMesh(const MeshArgs& args,
                              const std::vector<std::string>& purposes,
                              std::vector<std::vector<float>>* planes,
                              std::vector<std::string>* warnings,
                              std::string* error) OVERRIDE FINAL
        {
            if (isCancelled(args.cancelled, error)) {
                return false;
            }
            const PXR_NS::UsdPrim prim = args.stage ? args.stage->GetPrimAtPath(args.mesh) : PXR_NS::UsdPrim();
            if (!prim) {
                return fail(error, args.mesh.GetString() + " is not on the input scene.");
            }
            const std::size_t count = (std::size_t)std::max(args.size, 0) * (std::size_t)std::max(args.size, 0);
            if ((planes->size() != purposes.size())) {
                return fail(error, "The bake was given a plane per purpose it was not.");
            }
            for (const std::vector<float>& plane : *planes) {
                if (plane.size() != count * 4) {
                    return fail(error, "The bake was given a plane of the wrong size.");
                }
            }

            MaterialReader reader(prim, args.stName, args.time);
            const PXR_NS::UsdShadeShader surface = boundSurface(prim);
            const bool projection = SceneNodeAuthoring::isProjectionShader(surface);
            const bool preview = isPreviewSurface(surface);
            if (surface && !projection && !preview) {
                reader.warn(surface.GetPath().GetString() + ", the surface of " + args.mesh.GetString() + ", is not a UsdPreviewSurface: the bake gives it UsdPreviewSurface's defaults.");
            }
            std::vector<Source> sources;
            for (const std::string& purpose : purposes) {
                const PurposeInfo* info = purposeInfo(purpose);
                if (!info) {
                    return fail(error, "The bake has no purpose named " + purpose + ".");
                }
                Source source;
                source.colour = info->colour;
                source.constant = PXR_NS::GfVec4f(info->fallback, info->fallback, info->fallback, 1.f);
                if (preview) {
                    source = reader.inputSource(surface, *info);
                } else if (projection && (purpose == kMaterialBakePurposeDiffuseColor)) {
                    source = reader.projectionSource(surface);
                }
                sources.push_back(source);
            }

            SceneRaster::BakeArgs bake;
            bake.primPath = args.mesh;
            bake.stName = args.stName;
            bake.time = args.time;
            bake.width = args.size;
            bake.height = args.size;
            bake.udimTile = args.udimTile;
            bake.readerStName = reader.readerStName();
            SceneRaster::BakeBuffers buffers;
            if (!SceneRaster::bakeUV(args.stage, bake, &buffers, error)) {
                return false;
            }
            if ((buffers.coverage.size() != count) || (buffers.worldPosition.size() != count) || (buffers.worldNormal.size() != count)) {
                return fail(error, args.mesh.GetString() + " was baked at the wrong size.");
            }
            if (isCancelled(args.cancelled, error)) {
                return false;
            }

            const int tileIndex = args.udimTile - 1001;
            const float u0 = (float)(tileIndex % 10);
            const float v0 = (float)(tileIndex / 10);
            const int size = args.size;
            const bool reads = !buffers.readerSt.empty();
            const int threads = appPTR ? appPTR->getNCPUsAvailableForEffect() : QThreadPool::globalInstance()->maxThreadCount();
            parallelForOnGlobalPool(size, threads, [&](int y) {
                Texel texel;
                for (int x = 0; x < size; ++x) {
                    const std::size_t i = (std::size_t)x + (std::size_t)y * (std::size_t)size;
                    if (!buffers.coverage[i]) {
                        continue;
                    }
                    texel.st = PXR_NS::GfVec2f(u0 + (x + 0.5f) / size, v0 + (y + 0.5f) / size);
                    texel.readerSt = reads ? buffers.readerSt[i] : PXR_NS::GfVec2f(0.f);
                    texel.position = PXR_NS::GfVec3d(buffers.worldPosition[i]);
                    texel.normal = PXR_NS::GfVec3d(buffers.worldNormal[i]);
                    for (std::size_t p = 0; p < sources.size(); ++p) {
                        const PXR_NS::GfVec4f value = sources[p].at(texel);
                        float* out = (*planes)[p].data() + i * 4;
                        for (int c = 0; c < 4; ++c) {
                            out[c] = value[c];
                        }
                    }
                }
            });

            reader.collectUdimWarnings();
            for (const std::string& warning : reader.warnings()) {
                if (std::find(warnings->begin(), warnings->end(), warning) == warnings->end()) {
                    warnings->push_back(warning);
                }
            }

            return true;
        } // bakeMesh
    };

    bool
    writeImage(const std::string& path,
               int size,
               const std::vector<float>& rgba,
               const std::function<bool()>& cancelled,
               std::string* error)
    {
        std::error_code ec;

        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        if (ec) {
            return fail(error, "Could not create the directory of " + path + ": " + ec.message());
        }
        OIIO::ImageSpec spec(size, size, 4, OiioWriteSupport::dataTypeOiio(OiioWriteSupport::DataType::Half));
        spec.channelnames = { "R", "G", "B", "A" };
        // The values are written as they are, not converted between premultiplied and straight.
        spec.alpha_channel = -1;
        spec.z_channel = -1;
        spec.attribute("oiio:UnassociatedAlpha", 1);
        spec.attribute("compression", "zip");

        std::vector<OiioWriteSupport::WritePart> parts(1);
        parts[0].spec = spec;
        const std::size_t rowFloats = (std::size_t)size * 4;
        const OiioWriteSupport::RowSource rows = [&](int /*part*/, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride) -> bool {
            for (int r = rowBegin; r < rowEnd; ++r) {
                const float* src = rgba.data() + (std::size_t)(size - 1 - r) * rowFloats;
                float* dst = (float*)((char*)band + (std::ptrdiff_t)(r - rowBegin) * rowStride);
                std::copy(src, src + rowFloats, dst);
            }

            return true;
        };
        const OiioWriteSupport::CancelCheck stop = [&cancelled]() -> bool {
            return cancelled && cancelled();
        };
        if (!OiioWriteSupport::writeFile(path, parts, rows, stop, error)) {
            return false;
        }
        OiioReadSupport::evictHeaders(std::vector<std::string>(1, path));

        return true;
    } // writeImage
} // anon namespace

const std::vector<std::string>&
optionalPurposes()
{
    static const std::vector<std::string> purposes = []() {
        std::vector<std::string> names;

        for (const PurposeInfo& info : kPurposes) {
            if (std::string(info.name) != kMaterialBakePurposeDiffuseColor) {
                names.push_back(info.name);
            }
        }

        return names;
    }();

    return purposes;
}

std::unique_ptr<Backend>
createPreviewSurfaceBackend()
{
    return std::unique_ptr<Backend>(new PreviewSurfaceBackend());
}

bool
purposePath(const std::string& pattern,
            const std::string& purpose,
            int udimTile,
            std::string* path,
            std::string* error)
{
    const std::string token(kUdimToken);
    std::string result = pattern;
    std::size_t at = result.find(token);

    if (at == std::string::npos) {
        return fail(error, "The path " + pattern + " holds no " + token + " token for the tile number.");
    }
    if (purpose != kMaterialBakePurposeDiffuseColor) {
        if ((at > 0) && ((result[at - 1] == '.') || (result[at - 1] == '_'))) {
            result.insert(at - 1, "_" + purpose);
        } else {
            result.insert(at, "_" + purpose + ".");
        }
        at = result.find(token);
    }
    result.replace(at, token.size(), std::to_string(udimTile));
    *path = result;

    return true;
}

bool
bakeTiles(Backend& backend,
          const TilesArgs& args,
          std::vector<BakedFile>* files,
          std::vector<std::string>* warnings,
          std::string* error)
{
    files->clear();
    warnings->clear();
    if (!args.stage || args.meshes.empty()) {
        return fail(error, "There is no mesh to bake the look of.");
    }
    if (args.size <= 0) {
        return fail(error, "The tiles to bake are empty.");
    }
    std::string probe;
    if (!purposePath(args.pathPattern, kMaterialBakePurposeDiffuseColor, 1001, &probe, error)) {
        return false;
    }

    std::set<std::string> extra;
    std::map<int, std::vector<PXR_NS::SdfPath>> tiles;
    for (const PXR_NS::SdfPath& mesh : args.meshes) {
        backend.addPurposes(args.stage, mesh, args.time, &extra);
        std::set<int> covered;
        if (!SceneRaster::udimTiles(args.stage, mesh, args.stName, args.time, &covered, error)) {
            return false;
        }
        for (int tile : covered) {
            tiles[tile].push_back(mesh);
        }
    }
    if (tiles.empty()) {
        return fail(error, "The meshes' UV layouts cover no UDIM tile.");
    }
    std::vector<std::string> purposes(1, kMaterialBakePurposeDiffuseColor);
    for (const std::string& purpose : optionalPurposes()) {
        if (extra.count(purpose)) {
            purposes.push_back(purpose);
        }
    }

    const std::size_t count = (std::size_t)args.size * (std::size_t)args.size;
    for (const std::pair<const int, std::vector<PXR_NS::SdfPath>>& tile : tiles) {
        std::vector<std::vector<float>> planes(purposes.size(), std::vector<float>(count * 4, 0.f));
        for (const PXR_NS::SdfPath& mesh : tile.second) {
            MeshArgs meshArgs;
            meshArgs.stage = args.stage;
            meshArgs.mesh = mesh;
            meshArgs.stName = args.stName;
            meshArgs.time = args.time;
            meshArgs.udimTile = tile.first;
            meshArgs.size = args.size;
            meshArgs.cancelled = args.cancelled;
            if (!backend.bakeMesh(meshArgs, purposes, &planes, warnings, error)) {
                return false;
            }
        }
        for (std::size_t p = 0; p < purposes.size(); ++p) {
            if (isCancelled(args.cancelled, error)) {
                return false;
            }
            BakedFile file;
            file.purpose = purposes[p];
            file.udimTile = tile.first;
            if (!purposePath(args.pathPattern, file.purpose, file.udimTile, &file.path, error)) {
                return false;
            }
            if (!writeImage(file.path, args.size, planes[p], args.cancelled, error)) {
                return false;
            }
            files->push_back(file);
        }
    }

    return true;
} // bakeTiles

bool
readBaseTile(const std::string& path,
             int size,
             std::vector<float>* rgba,
             bool* resampled,
             std::string* error)
{
    std::vector<float> image;
    int width = 0;
    int height = 0;

    if (resampled) {
        *resampled = false;
    }
    if (size <= 0) {
        return fail(error, "The tile is empty.");
    }
    if (!readImageFile(path, &image, &width, &height, 0, error)) {
        return false;
    }
    const std::size_t count = (std::size_t)size * (std::size_t)size;
    if ((width == size) && (height == size)) {
        *rgba = std::move(image);

        return true;
    }
    if (resampled) {
        *resampled = true;
    }
    ImageData source;
    source.width = width;
    source.height = height;
    source.rgba = std::move(image);
    rgba->assign(count * 4, 0.f);
    for (int y = 0; y < size; ++y) {
        const float t = (y + 0.5f) / size;
        for (int x = 0; x < size; ++x) {
            const PXR_NS::GfVec4f v = sampleBilinear(source, (x + 0.5f) / size, t, eWrapClamp, eWrapClamp);
            float* out = rgba->data() + ((std::size_t)x + (std::size_t)y * (std::size_t)size) * 4;
            for (int c = 0; c < 4; ++c) {
                out[c] = v[c];
            }
        }
    }

    return true;
} // readBaseTile
} // namespace MaterialBake

NATRON_NAMESPACE_EXIT
