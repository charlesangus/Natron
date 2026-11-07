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

#ifndef Engine_Nodes_Scene_Utility_UtilityRenderDelegate_h
#define Engine_Nodes_Scene_Utility_UtilityRenderDelegate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <memory>

#include <pxr/imaging/hd/renderDelegate.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

// The GetRenderStats keys, each holding a std::size_t.
#define kUtilityStatGeometryCount "utilityGeometryCount"
#define kUtilityStatInstanceCount "utilityInstanceCount"
#define kUtilityStatTriangleCount "utilityTriangleCount"
#define kUtilityStatTopologyBuilds "utilityTopologyBuilds"
#define kUtilityStatPointUpdates "utilityPointUpdates"
#define kUtilityStatColorUpdates "utilityColorUpdates"
#define kUtilityStatSceneVersion "utilitySceneVersion"

NATRON_NAMESPACE_ENTER

/**
 * @brief What the delegate hands its prims when they sync.
 **/
class UtilityRenderParam : public PXR_NS::HdRenderParam {
public:
    explicit UtilityRenderParam(UtilityScene* scene);

    UtilityScene* scene() const;

private:
    UtilityScene* _scene;
};

/**
 * @brief The Natron Utility render delegate: meshes and instancers become a UtilityScene that its
 * render passes draw on the CPU.
 **/
class UtilityRenderDelegate : public PXR_NS::HdRenderDelegate {
public:
    UtilityRenderDelegate();
    explicit UtilityRenderDelegate(const PXR_NS::HdRenderSettingsMap& settings);
    virtual ~UtilityRenderDelegate();

    // How many delegates exist right now, for the tests' leak checks. MT-safe.
    static int liveCount();

    virtual PXR_NS::HdRenderParam* GetRenderParam() const OVERRIDE;
    virtual const PXR_NS::TfTokenVector& GetSupportedRprimTypes() const OVERRIDE;
    virtual const PXR_NS::TfTokenVector& GetSupportedSprimTypes() const OVERRIDE;
    virtual const PXR_NS::TfTokenVector& GetSupportedBprimTypes() const OVERRIDE;
    virtual PXR_NS::HdResourceRegistrySharedPtr GetResourceRegistry() const OVERRIDE;
    virtual PXR_NS::HdRenderPassSharedPtr CreateRenderPass(PXR_NS::HdRenderIndex* index,
                                                           const PXR_NS::HdRprimCollection& collection) OVERRIDE;
    virtual PXR_NS::HdInstancer* CreateInstancer(PXR_NS::HdSceneDelegate* delegate,
                                                 const PXR_NS::SdfPath& id) OVERRIDE;
    virtual void DestroyInstancer(PXR_NS::HdInstancer* instancer) OVERRIDE;
    virtual PXR_NS::HdRprim* CreateRprim(const PXR_NS::TfToken& typeId,
                                         const PXR_NS::SdfPath& rprimId) OVERRIDE;
    virtual void DestroyRprim(PXR_NS::HdRprim* rprim) OVERRIDE;
    virtual PXR_NS::HdSprim* CreateSprim(const PXR_NS::TfToken& typeId,
                                         const PXR_NS::SdfPath& sprimId) OVERRIDE;
    virtual PXR_NS::HdSprim* CreateFallbackSprim(const PXR_NS::TfToken& typeId) OVERRIDE;
    virtual void DestroySprim(PXR_NS::HdSprim* sprim) OVERRIDE;
    virtual PXR_NS::HdBprim* CreateBprim(const PXR_NS::TfToken& typeId,
                                         const PXR_NS::SdfPath& bprimId) OVERRIDE;
    virtual PXR_NS::HdBprim* CreateFallbackBprim(const PXR_NS::TfToken& typeId) OVERRIDE;
    virtual void DestroyBprim(PXR_NS::HdBprim* bprim) OVERRIDE;
    virtual void CommitResources(PXR_NS::HdChangeTracker* tracker) OVERRIDE;
    virtual PXR_NS::HdAovDescriptor GetDefaultAovDescriptor(const PXR_NS::TfToken& name) const OVERRIDE;
    virtual PXR_NS::VtDictionary GetRenderStats() const OVERRIDE;

private:
    void construct();

    PXR_NS::HdResourceRegistrySharedPtr _resourceRegistry;
    std::unique_ptr<UtilityScene> _scene;
    std::unique_ptr<UtilityRenderParam> _renderParam;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityRenderDelegate_h
