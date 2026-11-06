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

#ifndef Engine_Ai_ModelPackage_h
#define Engine_Ai_ModelPackage_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <stdexcept>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

class ModelPackageError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class AiColourSpace {
    eSrgb,
    eLinearRec709,
    eLinearRec2020,
    // The camera's native RGB or its raw mosaic, before any colour matrix.
    eCamera,
};

/**
 * @brief What a darktable-ai package's config.json says about one model of the package. A
 * package holding several models (model_x2.onnx and model_x4.onnx, say) keys their attributes by
 * file stem; the package-wide attributes apply to each unless the model's own override them.
 * Tensor names and shapes are not in config.json: InferenceSession reads them from the model.
 **/
struct ModelConfig {
    std::string id;
    std::string name;
    std::string description;
    // darktable's task name ("mask", "denoise", "rawdenoise", "upscale"), not AiModelTask's.
    std::string task;
    std::string arch;
    std::string version;
    // Whether the model runs over an image in tiles; mask models see the whole image at once.
    bool tiling = false;
    int numInputs = 1;
    // The file stem the attributes were looked up under.
    std::string variant;
    // The static input sizes the model was exported at, square, largest first as listed.
    // Empty when config.json gives none.
    std::vector<int> inputSizes;
    AiColourSpace colourSpace = AiColourSpace::eSrgb;
    // darktable's input_kind ("bayer_v1", "linear_v1"), empty when not given.
    std::string inputKind;
    // The model's attributes as a compact JSON object, package-wide ones merged under its own.
    std::string attributesJson;
    std::string weightsLicence;
    std::string trainingDataLicence;

    // The first input size, or 0 when the model takes any size.
    int tileSize() const
    {
        return inputSizes.empty() ? 0 : inputSizes.front();
    }
};

/**
 * @brief Parses config.json for the model whose first file is variantFile. Throws
 * ModelPackageError when the document is malformed, its backend is not "onnx", or it names a
 * colour space this build does not know.
 **/
ModelConfig parseModelConfig(const std::string& json, const std::string& variantFile);

// Reads <packageDirectory>/config.json.
ModelConfig readModelConfig(const std::string& packageDirectory, const std::string& variantFile);

/**
 * @brief Extracts a .dtmodel (a zip of config.json and the ONNX files, under a folder named after
 * the package) into destination, which is created and must not exist yet. Members outside the
 * package folder are refused, as are unsafe names. Throws ModelPackageError, leaving whatever
 * was extracted for the caller to delete.
 **/
void unpackModelPackage(const std::string& archive, const std::string& package, const std::string& destination);

/**
 * @brief The lowercase hex SHA-256 of a file's contents. Throws ModelPackageError when the file
 * cannot be read.
 **/
std::string sha256OfFile(const std::string& path);

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_ModelPackage_h
