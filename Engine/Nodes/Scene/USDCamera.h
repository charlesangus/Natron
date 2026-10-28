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

#ifndef Engine_Nodes_Scene_USDCamera_h
#define Engine_Nodes_Scene_USDCamera_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDCAMERA "USDCamera"

#define kUSDCameraParamProjection "projection"
#define kUSDCameraParamFocalLength "focalLength"
#define kUSDCameraParamHorizontalAperture "horizontalAperture"
#define kUSDCameraParamVerticalAperture "verticalAperture"
#define kUSDCameraParamHorizontalApertureOffset "horizontalApertureOffset"
#define kUSDCameraParamVerticalApertureOffset "verticalApertureOffset"
#define kUSDCameraParamNear "near"
#define kUSDCameraParamFar "far"
#define kUSDCameraParamFStop "fStop"
#define kUSDCameraParamFocusDistance "focusDistance"
#define kUSDCameraParamReadFromFile "readFromFile"
#define kUSDCameraParamFile "file"
#define kUSDCameraParamReload "reload"
#define kUSDCameraParamCameraPrim "cameraPrim"
#define kUSDCameraParamFrameOffset "frameOffset"
#define kUSDCameraParamEditTransform "editTransform"
#define kUSDCameraParamEditLens "editLens"
#define kUSDCameraParamEditClipping "editClipping"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief Authors a UsdGeomCamera prim, as a scene source.
 *
 * Lens and aperture knobs are in millimetres and are written in USD's camera units, tenths of a
 * scene unit, so they read as millimetres in a centimetre scene. Clipping and focus distance are
 * in scene units. The aperture is not tied to the project format: whatever renders through the
 * camera conforms it to its own format.
 *
 * With Read From File on, the camera comes from a UsdGeomCamera in a file, sampled at every frame
 * of the project range through the frame offset: its lens and clipping as they are, in the file's
 * units, and its world transform, its ancestors' flattened in, as one xformOp:transform. The
 * knobs then show the file's values at the current frame, disabled. An Edit toggle per group
 * enables its knobs and authors them over the file's: the transform knobs as an offset in the
 * camera's space (ops after the file's matrix), lens and clipping as replacement values.
 **/
class USDCamera
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDCAMERA,
        .label = "USDCamera",
        .description = "Create a scene holding one camera prim (UsdGeomCamera). "
                       "Focal length and aperture are in millimetres, written in USD's camera units (tenths of a scene unit); "
                       "clipping and focus distance are in scene units. Animated knobs are written as time samples.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDCamera.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDCamera(node);
    }

    explicit USDCamera(NodePtr node);

    virtual ~USDCamera();

    /**
     * @brief Drops the stamps of the file camera's files and the memoized layer, so the next
     * evaluation authors the layer again. Edits no USD layer: Reload reloads them in place.
     **/
    virtual void purgeCaches() OVERRIDE FINAL;

protected:
    virtual void refreshExtraStateAfterTimeChanged(bool isPlayback, double time) OVERRIDE FINAL;

private:
    struct FileCamera;
    struct FileSettings;
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool authorLayer(const SceneLayerEdit& edit,
                     const FileSettings& file,
                     std::string* errorMessage) const;

    std::vector<KnobIPtr> getTransformKnobs() const;
    std::vector<KnobIPtr> getLensKnobs() const;
    std::vector<KnobIPtr> getClippingKnobs() const;

    // Shows the file knobs with Read From File, and disables and stops hashing the knobs the file drives.
    void refreshKnobStates();

    // Lists the file's cameras in the Camera Prim menu; selectFirst picks the first when the
    // selected one is not among them.
    void refreshCameraChoices(bool selectFirst);

    // Writes the file camera's values at time into the chosen groups' knobs.
    void showFileValues(double time,
                        bool transform,
                        bool lens,
                        bool clipping);

    // showFileValues() of every group not being edited.
    void showFileValues(double time);

    KnobStringWPtr _primPath;
    SceneTransformKnobs _transform;
    KnobChoiceWPtr _projection;
    KnobDoubleWPtr _focalLength;
    KnobDoubleWPtr _horizontalAperture;
    KnobDoubleWPtr _verticalAperture;
    KnobDoubleWPtr _horizontalApertureOffset;
    KnobDoubleWPtr _verticalApertureOffset;
    KnobDoubleWPtr _near;
    KnobDoubleWPtr _far;
    KnobDoubleWPtr _fStop;
    KnobDoubleWPtr _focusDistance;
    KnobBoolWPtr _readFromFile;
    KnobFileWPtr _file;
    KnobButtonWPtr _reload;
    KnobChoiceWPtr _cameraPrim;
    KnobDoubleWPtr _frameOffset;
    KnobBoolWPtr _editTransform;
    KnobBoolWPtr _editLens;
    KnobBoolWPtr _editClipping;
    std::unique_ptr<FileCamera> _fileCamera;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDCamera_h
