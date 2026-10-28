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

#ifndef Tests_SceneTestEffect_h
#define Tests_SceneTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/AuthoredKnobSet.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/ScenePayload.h"

#define kTestPluginIDSceneSource "test.natron.built-in.SceneSource"
#define kTestPluginIDSceneOp "test.natron.built-in.SceneOp"
#define kTestPluginIDSceneGeometry "test.natron.built-in.SceneGeometry"
#define kTestPluginIDSceneAuthored "test.natron.built-in.SceneAuthored"

// What a failing SceneSource reports, so a test can find it in the action's message.
#define kSceneTestSourceFailureMessage "SceneSource was set to fail"

// The prim and attributes the stubs author, read back by the tests through the composed stage.
#define kSceneTestPrimPath "/Root"
#define kSceneTestSourceAttribute "natron:sourceValue"
#define kSceneTestOpAttribute "natron:opValue"
// The source value a SceneOp in the facts mode read upstream.
#define kSceneTestFactAttribute "natron:sourceValueRead"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;
class SceneTestGeometry;

// How many times SceneSource's renderScene() ran, so a test can tell a pass-through from a second
// evaluation.
inline std::atomic<int>&
sceneTestSourceRenderCount()
{
    static std::atomic<int> count(0);

    return count;
}

// Run inside SceneSource's renderScene(), so a test can abort the render from within the action.
inline std::function<void()>&
sceneTestSourceHook()
{
    static std::function<void()> hook;

    return hook;
}

// Defined in Tests/Scene/SceneTestAuthoring.cpp, the only test file outside the scene tests that
// sees USD: this header is included by wmain.cpp, which must stay free of pxr.
bool sceneTestAuthorSource(const SceneLayerEdit& edit, int value, std::string* errorMessage);
bool sceneTestAuthorOp(const SceneLayerEdit& edit, int value, std::string* errorMessage);
bool sceneTestAuthorFactOp(const SceneLayerEdit& edit, int value, int sourceValue, std::string* errorMessage);
// The source value the stack composes, read through SceneQueryPool; 0 if none.
bool sceneTestReadSourceValue(const ScenePayload& payload, int* value, std::string* errorMessage);
// SceneTestGeometry's scene: material grafted under a cube at primPath bound to it.
bool sceneTestRenderGeometry(SceneTestGeometry* effect, const ScenePayload& material, const std::string& primPath, SceneGraftMemo* memo, ScenePayload* output, std::string* errorMessage);

/**
 * @brief A scene source: defines kSceneTestPrimPath carrying its "value" knob, or fails with
 * kSceneTestSourceFailureMessage while its "fail" knob is set.
 **/
class SceneTestSource
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new SceneTestSource(n);
    }

    explicit SceneTestSource(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    U64 getLayerAuthorCount() const
    {
        return getSceneLayerAuthorCount();
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDSceneSource,
        .label = "Test Scene Source",
        .outputKind = eDataKindScene,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobIntPtr value = createKnob<KnobInt>(std::string("Value"));

        value->setName("value");
        value->setAnimationEnabled(false);
        value->setDefaultValue(0);
        page->addKnob(value);
        _value = value;

        KnobBoolPtr fail = createKnob<KnobBool>(std::string("Fail"));
        fail->setName("fail");
        fail->setAnimationEnabled(false);
        fail->setDefaultValue(false);
        page->addKnob(fail);
        _fail = fail;
    }

    virtual StatusEnum renderScene(const SceneRenderActionArgs& /*args*/,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ++sceneTestSourceRenderCount();
        if (sceneTestSourceHook()) {
            sceneTestSourceHook()();
        }

        KnobBoolPtr failKnob = _fail.lock();
        if (failKnob && failKnob->getValue()) {
            *errorMessage = kSceneTestSourceFailureMessage;

            return eStatusFailed;
        }

        KnobIntPtr valueKnob = _value.lock();
        const int value = valueKnob ? valueKnob->getValue() : 0;

        return emitSceneLayer(ScenePayload(), [value](const SceneLayerEdit& edit, std::string* error) { return sceneTestAuthorSource(edit, value, error); }, outputScene, errorMessage) ? eStatusOK : eStatusFailed;
    }

    KnobIntWPtr _value;
    KnobBoolWPtr _fail;
};

/**
 * @brief A scene op: one scene input, over which it authors its "value" knob on
 * kSceneTestPrimPath.
 **/
class SceneTestOp
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new SceneTestOp(n);
    }

    explicit SceneTestOp(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    // The Disable knob is one of the default knobs only a settings panel creates, and the
    // disabled pass-through test needs it: without it setNodeDisabled() does nothing.
    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return true;
    }

    U64 getLayerAuthorCount() const
    {
        return getSceneLayerAuthorCount();
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindScene, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDSceneOp,
        .label = "Test Scene Op",
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobIntPtr value = createKnob<KnobInt>(std::string("Value"));

        value->setName("value");
        value->setAnimationEnabled(false);
        value->setDefaultValue(0);
        page->addKnob(value);
        _value = value;
        // 0: keys on the whole upstream stack; 1: reads nothing upstream; 2: reads only the
        // source value, through SceneQueryPool, and keys on it.
        KnobIntPtr upstreamRead = createKnob<KnobInt>(std::string("Upstream Read"));
        upstreamRead->setName("upstreamRead");
        upstreamRead->setAnimationEnabled(false);
        upstreamRead->setDefaultValue(0);
        page->addKnob(upstreamRead);
        _upstreamRead = upstreamRead;
    }

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        KnobIntPtr valueKnob = _value.lock();
        const int value = valueKnob ? valueKnob->getValue() : 0;

        KnobIntPtr readKnob = _upstreamRead.lock();
        const int mode = readKnob ? readKnob->getValue() : 0;
        const ScenePayload upstream = args.getInputScene(0);
        if (mode == 1) {
            SceneLayerReads reads;
            reads.upstream = SceneLayerMemo::Key::eUpstreamReadNothing;
            return emitSceneLayer(upstream, reads, [value](const SceneLayerEdit& edit, std::string* error) { return sceneTestAuthorOp(edit, value, error); }, outputScene, errorMessage) ? eStatusOK : eStatusFailed;
        }
        if (mode == 2) {
            int sourceValue = 0;
            if (!sceneTestReadSourceValue(upstream, &sourceValue, errorMessage)) {
                return eStatusFailed;
            }
            SceneLayerReads reads;
            reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
            reads.factsDigest = (U64)(std::int64_t)sourceValue;
            return emitSceneLayer(upstream, reads, [value, sourceValue](const SceneLayerEdit& edit, std::string* error) { return sceneTestAuthorFactOp(edit, value, sourceValue, error); }, outputScene, errorMessage) ? eStatusOK : eStatusFailed;
        }
        return emitSceneLayer(upstream, [value](const SceneLayerEdit& edit, std::string* error) { return sceneTestAuthorOp(edit, value, error); }, outputScene, errorMessage) ? eStatusOK : eStatusFailed;
    }

    KnobIntWPtr _value;
    KnobIntWPtr _upstreamRead;
};

/**
 * @brief A pass-through scene op whose three value knobs form an AuthoredKnobSet, for
 * Scene/AuthoredKnobSet_Test.cpp.
 **/
class SceneTestAuthored
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new SceneTestAuthored(n);
    }

    explicit SceneTestAuthored(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    const AuthoredKnobSet& getAuthoredKnobs() const
    {
        return _authored;
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindScene, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDSceneAuthored,
        .label = "Test Scene Authored",
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));

        KnobDoublePtr size = createKnob<KnobDouble>(std::string("Size"));
        size->setName("size");
        size->setDefaultValue(1.);
        _authored.add(this, page, size);

        KnobIntPtr count = createKnob<KnobInt>(std::string("Count"));
        count->setName("count");
        count->setAnimationEnabled(false);
        count->setDefaultValue(3);
        _authored.add(this, page, count);

        KnobStringPtr tag = createKnob<KnobString>(std::string("Tag"));
        tag->setName("tag");
        tag->setAnimationEnabled(false);
        _authored.add(this, page, tag);
    }

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/) OVERRIDE FINAL
    {
        return _authored.onKnobChanged(k, reason);
    }

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* /*errorMessage*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *outputScene = args.getInputScene(0);

        return eStatusOK;
    }

    AuthoredKnobSet _authored;
};

/**
 * @brief A geometry stand-in until shipped primitives exist: defines a Cube at its Prim Path
 * (/<script name> by default) and takes a material on its Material input, through
 * SceneNodeAuthoring::graftMaterialInput.
 **/
class SceneTestGeometry
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new SceneTestGeometry(n);
    }

    explicit SceneTestGeometry(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    // emitSceneLayer() keyed on factsDigest, for sceneTestRenderGeometry().
    bool emitLayer(const ScenePayload& upstream,
                   U64 factsDigest,
                   const SceneLayerMemo::AuthorFunc& author,
                   ScenePayload* output,
                   std::string* errorMessage)
    {
        SceneLayerReads reads;
        reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
        reads.factsDigest = factsDigest;
        reads.frameRange = false;

        return emitSceneLayer(upstream, reads, author, output, errorMessage);
    }

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Material", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDSceneGeometry,
        .label = "Test Scene Geometry",
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));

        _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Geometry"), QString());
    }

    virtual void onScriptNameChanged(const std::string& /*fullyQualifiedName*/) OVERRIDE FINAL
    {
        followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "Geometry"));
    }

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        KnobStringPtr primPath = _primPath.lock();
        if (!primPath) {
            return eStatusFailed;
        }

        return sceneTestRenderGeometry(this, args.getInputScene(0), primPath->getValue(), &_graftMemo, outputScene, errorMessage) ? eStatusOK : eStatusFailed;
    }

    KnobStringWPtr _primPath;
    SceneGraftMemo _graftMemo;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_SceneTestEffect_h
