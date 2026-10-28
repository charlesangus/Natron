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

#ifndef Engine_Nodes_Transform_LensCorrect_h
#define Engine_Nodes_Transform_LensCorrect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/LensCorrectProfile.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_LENSCORRECT "LensCorrect"
#define PLUGIN_MAJOR_NATRON_LENSCORRECT 1

#define kLensCorrectParamMethod "method"
#define kLensCorrectParamMode "mode"
#define kLensCorrectParamCorrectDistortion "correctDistortion"
#define kLensCorrectParamCorrectTCA "correctTCA"
#define kLensCorrectParamCorrectVignetting "correctVignetting"
#define kLensCorrectParamCamera "camera"
#define kLensCorrectParamLens "lens"
#define kLensCorrectParamCropFactor "cropFactor"
#define kLensCorrectParamFocalLength "focalLength"
#define kLensCorrectParamAperture "aperture"
#define kLensCorrectParamDistance "distance"
#define kLensCorrectParamTargetGeometry "targetGeometry"
#define kLensCorrectParamAutoScale "autoScale"
#define kLensCorrectParamScale "scale"
#define kLensCorrectParamTCAOverride "tcaOverride"
#define kLensCorrectParamTCARed "tcaRed"
#define kLensCorrectParamTCABlue "tcaBlue"
#define kLensCorrectParamVignetteStrength "vignetteStrength"
#define kLensCorrectParamVignetteRadius "vignetteRadius"
#define kLensCorrectParamVignetteSteepness "vignetteSteepness"
#define kLensCorrectParamShowVignetteMask "showVignetteMask"
#define kLensCorrectParamInfo "info"

#define kLensCorrectMethodLensfun "lensfun"
#define kLensCorrectMethodVignette "vignette"

#define kLensCorrectModeCorrect "correct"
#define kLensCorrectModeDistort "distort"

// Option IDs of the target geometry choice; "lens" keeps the lens's own projection.
#define kLensCorrectGeometryLens "lens"
#define kLensCorrectGeometryRectilinear "rectilinear"
#define kLensCorrectGeometryFisheye "fisheye"
#define kLensCorrectGeometryPanoramic "panoramic"
#define kLensCorrectGeometryEquirectangular "equirectangular"
#define kLensCorrectGeometryOrthographic "orthographic"
#define kLensCorrectGeometryStereographic "stereographic"
#define kLensCorrectGeometryEquisolid "equisolid"
#define kLensCorrectGeometryThoby "thoby"

#define kLensCorrectInputSource 0
#define kLensCorrectInputMask 1

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kLensCorrectInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kLensCorrectNodeSpec = {
    .id = PLUGINID_NATRON_LENSCORRECT,
    .label = "LensCorrect",
    .description = "Correct, or add, a real lens's distortion, transverse chromatic aberration (TCA) and vignetting "
                   "from the lensfun database, as darktable's lens correction module does.\n"
                   "Camera, lens, focal length, aperture and focus distance come from the Source's metadata (the "
                   "EXIF a Read gives a JPEG, TIFF or raw file); a value typed in a knob replaces the metadata's. "
                   "The Info line shows what was looked up and where each value came from. A camera or lens that "
                   "is not in the database leaves the image unchanged, with a warning naming it.\n"
                   "The image may also be reprojected to another lens geometry (fisheye to rectilinear, say), "
                   "scaled so that no empty border shows, and given a manual vignette.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_LENSCORRECT,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/LensCorrect.png",
    .inputs = kLensCorrectInputs,
};

inline constexpr ImagePipelineSpec kLensCorrectPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Custom,
};

class LensCorrect
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new LensCorrect(node);
    }

    explicit LensCorrect(NodePtr node);

    virtual ~LensCorrect();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief What the knobs and the Source's metadata resolve to at time and view: the database
     * entries found, every value the model is built from and where it came from, and the warning
     * posted when the lens cannot be corrected (empty when it can, or when the method needs no
     * lens).
     **/
    struct Lookup {
        // The names searched for, and the database's names for what was found.
        std::string cameraQuery;
        std::string lensQuery;
        std::string cameraFound;
        std::string lensFound;
        std::shared_ptr<const lfLens> lens;
        float crop = 0.f;
        float focal = 0.f;
        float aperture = 0.f;
        float distance = 1000.f;
        // "knob", "metadata", "camera", "lens" or "default", one per value above.
        std::string cropFrom;
        std::string focalFrom;
        std::string apertureFrom;
        std::string distanceFrom;
        bool monochrome = false;
        std::string warning;
    };

    Lookup lookup(double time, ViewIdx view) WARN_UNUSED_RETURN;

    // The Info knob's text for a lookup.
    static std::string infoText(const Lookup& lookup) WARN_UNUSED_RETURN;

private:
    // The knob values a render reads, at one time and view.
    struct Settings {
        bool lensfun = true;
        bool reverse = false;
        bool distortion = true;
        bool tca = true;
        bool vignetting = true;
        int targetGeometry = 0;
        bool autoScale = true;
        double scale = 1.;
        bool tcaOverride = false;
        double tcaRed = 1.;
        double tcaBlue = 1.;
        double vignetteStrength = 0.;
        double vignetteRadius = 0.5;
        double vignetteSteepness = 0.5;
        bool showVignetteMask = false;
        Resampler::FilterEnum filter = Resampler::eFilterCubic;
        bool clamp = false;
        bool blackOutside = true;
    };

    // Everything a render evaluates, built on the calling thread and only read by the bands.
    struct Model {
        // The lensfun correction, empty when there is none.
        LensCorrectProfile::Profile profile;
        // The Source format in full-resolution pixels: the image lensfun's model covers.
        RectI format;
        // darktable's manual vignette, applied to the Source before the lensfun correction.
        float vignetteStrength = 0.f;
        float vignetteRadius = 0.5f;
        float vignetteSteepness = 0.5f;
        bool showVignetteMask = false;

        bool manualVignette() const { return (vignetteStrength > 0.f) || showVignetteMask; }

        bool active() const { return profile.movesPixels() || profile.hasVignetting() || manualVignette(); }
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual void onCurrentFrameMetadataChanged(const NodeMetadata& metadata) OVERRIDE FINAL;

    // Black while Source is disconnected, the Source where nothing applies.
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Accepts what the stages accept in float.
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getOpGpuInputRoI(int inputNb,
                                   double time,
                                   ViewIdx view,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    void readSettings(double time, ViewIdx view, Settings* settings) const;

    // The model at (time, view); the lookup's warning is posted, or taken back, on the way.
    Model buildModel(double time, ViewIdx view, const Settings& settings) WARN_UNUSED_RETURN;

    // Posts warning as the node's persistent message, or, when it is empty, takes back the one
    // this node posted last. Any thread.
    void postWarning(const std::string& warning);

    // Refreshes the Info knob and the warning at the current frame. Main thread.
    void refreshInfo();

    // Hides the lensfun knobs for the manual vignette method, and disables the knobs that the
    // others make irrelevant.
    void updateVisibility();

    KnobChoiceWPtr _method;
    KnobChoiceWPtr _mode;
    KnobBoolWPtr _correctDistortion;
    KnobBoolWPtr _correctTCA;
    KnobBoolWPtr _correctVignetting;
    KnobStringWPtr _camera;
    KnobStringWPtr _lens;
    KnobDoubleWPtr _cropFactor;
    KnobDoubleWPtr _focalLength;
    KnobDoubleWPtr _aperture;
    KnobDoubleWPtr _distance;
    KnobChoiceWPtr _targetGeometry;
    KnobBoolWPtr _autoScale;
    KnobDoubleWPtr _scale;
    KnobBoolWPtr _tcaOverride;
    KnobDoubleWPtr _tcaRed;
    KnobDoubleWPtr _tcaBlue;
    KnobDoubleWPtr _vignetteStrength;
    KnobDoubleWPtr _vignetteRadius;
    KnobDoubleWPtr _vignetteSteepness;
    KnobBoolWPtr _showVignetteMask;
    KnobStringWPtr _info;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobBoolWPtr _blackOutside;

    // Database queries by (maker, model, lens), which are slow and run for every tile otherwise.
    struct Found {
        std::string cameraFound;
        float cameraCrop = 0.f;
        std::string lensFound;
        std::shared_ptr<const lfLens> lens;
        std::string error;
    };
    mutable std::mutex _foundMutex;
    std::string _foundKey;
    Found _found;
    bool _hasFound = false;

    std::mutex _warningMutex;
    std::string _postedWarning;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_LensCorrect_h
