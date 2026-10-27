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

#ifndef Engine_Nodes_Scene_LiveTextureImage_h
#define Engine_Nodes_Scene_LiveTextureImage_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#ifdef emit
#define NATRON_LIVETEXTUREIMAGE_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hio/image.h>
#ifdef NATRON_LIVETEXTUREIMAGE_RESTORE_EMIT
#undef NATRON_LIVETEXTUREIMAGE_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief The Hio image reader for natron:// textures, which Storm loads through it as it loads any
 * texture file: it opens the asset LiveTextureResolver serves and reads its RGBA float pixels, top
 * row first unless asked for them flipped, scaled to the size asked for. One mip level, linear
 * colour, read-only.
 *
 * Registered under its type name, Natron::LiveTextureImage, by SceneCaches::registerLiveTextures,
 * with the plugInfo.json that declares its image type.
 **/
class LiveTextureImage
    : public PXR_NS::HioImage {
public:
    LiveTextureImage();
    virtual ~LiveTextureImage() OVERRIDE;

    virtual bool Read(const StorageSpec& storage) OVERRIDE;
    virtual bool ReadCropped(const int cropTop,
                             const int cropBottom,
                             const int cropLeft,
                             const int cropRight,
                             const StorageSpec& storage) OVERRIDE;
    virtual bool Write(const StorageSpec& storage,
                       const PXR_NS::VtDictionary& metadata) OVERRIDE;
    virtual const std::string& GetFilename() const OVERRIDE;
    virtual int GetWidth() const OVERRIDE;
    virtual int GetHeight() const OVERRIDE;
    virtual PXR_NS::HioFormat GetFormat() const OVERRIDE;
    virtual int GetBytesPerPixel() const OVERRIDE;
    virtual int GetNumMipLevels() const OVERRIDE;
    virtual bool IsColorSpaceSRGB() const OVERRIDE;
    virtual bool GetMetadata(const PXR_NS::TfToken& key,
                             PXR_NS::VtValue* value) const OVERRIDE;
    virtual bool GetSamplerMetadata(PXR_NS::HioAddressDimension dim,
                                    PXR_NS::HioAddressMode* param) const OVERRIDE;

protected:
    virtual bool _OpenForReading(const std::string& filename,
                                 int subimage,
                                 int mip,
                                 SourceColorSpace sourceColorSpace,
                                 bool suppressErrors) OVERRIDE;
    virtual bool _OpenForWriting(const std::string& filename) OVERRIDE;

private:
    std::string _filename;
    // Holds the bytes _pixels points into.
    std::shared_ptr<const char> _bytes;
    const float* _pixels = nullptr;
    int _width = 0;
    int _height = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_LiveTextureImage_h
