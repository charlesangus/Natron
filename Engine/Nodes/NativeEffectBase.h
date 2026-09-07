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

#ifndef Engine_Nodes_NativeEffectBase_h
#define Engine_Nodes_NativeEffectBase_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/AppManager.h" // for AppManager::createKnob
#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Statically describes one input of a NativeEffectBase subclass: its label,
 * whether it may be left unconnected, and the DataKindEnum it accepts.
 **/
struct NativeInputDescription {
    std::string label;
    bool optional;
    DataKindEnum kind;

    explicit NativeInputDescription(const std::string& label_,
                                    bool optional_ = false,
                                    DataKindEnum kind_ = eDataKindImage)
        : label(label_)
        , optional(optional_)
        , kind(kind_)
    {
    }
};

/**
 * @brief Statically describes a NativeEffectBase subclass: plugin id/label/description,
 * grouping, version, its inputs and the DataKindEnum it outputs. One instance of this,
 * returned by NativeEffectBase::getNativePluginDescription(), replaces the half-dozen
 * one-line EffectInstance accessor overrides a hand-written node otherwise repeats.
 **/
struct NativePluginDescription {
    std::string id;
    std::string label;
    std::string description;
    std::string grouping;
    int majorVersion;
    int minorVersion;
    std::vector<NativeInputDescription> inputs;
    DataKindEnum outputKind;

    NativePluginDescription()
        : id()
        , label()
        , description()
        , grouping(PLUGIN_GROUP_OTHER)
        , majorVersion(1)
        , minorVersion(0)
        , inputs()
        , outputKind(eDataKindImage)
    {
    }
};

/**
 * @brief Convenience base class for native (non-OFX) nodes living under Engine/Nodes/.
 *
 * A hand-written EffectInstance subclass otherwise repeats the same half-dozen
 * one-line overrides (getPluginID(), getPluginLabel(), getPluginGrouping(),
 * getMajorVersion(), getMinorVersion(), getNInputs(), getInputDataKind(), ...) for
 * every new node. NativeEffectBase asks for that metadata once, via a single
 * getNativePluginDescription() override, and implements those EffectInstance
 * virtuals from it.
 *
 * Everything else about EffectInstance -- knobs, rendering, undo, serialization,
 * scheduling -- is unchanged; the one virtual this class adds beyond that metadata is
 * resolveOutputDataKind(), the hook by which a native node supplies its own data-kind
 * resolution policy.
 * A subclass still overrides initializeKnobs() and render() exactly as it would
 * on top of EffectInstance directly, optionally using the createKnob() helper
 * below for the AppManager::createKnob() idiom.
 *
 * Worked example -- a minimal one-input, one-output native node:
 *
 * @code
 * #include "Engine/Nodes/NativeEffectBase.h"
 * #include "Engine/KnobTypes.h"
 *
 * NATRON_NAMESPACE_ENTER
 *
 * class ExampleNode
 *     : public NativeEffectBase
 * {
 * public:
 *     static EffectInstance* BuildEffect(NodePtr node)
 *     {
 *         return new ExampleNode(node);
 *     }
 *
 *     explicit ExampleNode(NodePtr node)
 *         : NativeEffectBase(node)
 *     {
 *     }
 *
 * private:
 *     virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
 *     {
 *         NativePluginDescription desc;
 *
 *         desc.id = "fr.natron.ExampleNode";
 *         desc.label = "Example";
 *         desc.description = "A minimal native node built on NativeEffectBase.";
 *         desc.grouping = PLUGIN_GROUP_OTHER;
 *         desc.majorVersion = 1;
 *         desc.minorVersion = 0;
 *         desc.inputs.push_back( NativeInputDescription("Source", false, eDataKindImage) );
 *         desc.outputKind = eDataKindImage;
 *
 *         return desc;
 *     }
 *
 *     virtual void initializeKnobs() OVERRIDE FINAL
 *     {
 *         KnobPagePtr page = createKnob<KnobPage>( tr("Controls") );
 *         KnobDoublePtr amount = createKnob<KnobDouble>( tr("Amount") );
 *
 *         amount->setDefaultValue(1.);
 *         page->addKnob(amount);
 *     }
 *
 *     virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
 *     {
 *         Q_UNUSED(args);
 *
 *         return eStatusOK;
 *     }
 * };
 *
 * NATRON_NAMESPACE_EXIT
 * @endcode
 **/
class NativeEffectBase
    : public EffectInstance {
public:
    explicit NativeEffectBase(NodePtr node);

    virtual ~NativeEffectBase();

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().id;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().label;
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().description;
    }

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    {
        grouping->push_back(getNativePluginDescription().grouping);
    }

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().majorVersion;
    }

    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().minorVersion;
    }

    virtual int getNInputs() const OVERRIDE WARN_UNUSED_RETURN
    {
        return (int)getNativePluginDescription().inputs.size();
    }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;
    virtual bool isInputOptional(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;

    virtual DataKindEnum getOutputDataKind() const OVERRIDE WARN_UNUSED_RETURN
    {
        return getNativePluginDescription().outputKind;
    }

    virtual DataKindEnum getInputDataKind(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief Supplies this node's data-kind resolution policy, consulted when its declared output
     * kind is eDataKindPolymorphic. The default delegates to the engine's structural resolution
     * (Node::resolveStructuralOutputDataKind()): the kinds reaching the node's polymorphic-declared
     * inputs, and the kinds its consumers require of it. A node whose kind follows something
     * narrower -- the input a switch has selected, say -- overrides this, returns that kind and
     * clears *isAmbiguous. Set *isAmbiguous instead when the policy leaves the node holding more
     * than one concrete kind at once; the returned kind is then ignored.
     * A policy that reads anything other than the node's connections -- a knob, typically -- must
     * call Node::invalidateEffectiveOutputDataKindCache() itself when that thing changes: the
     * engine only invalidates the cached answer when a connection changes.
     * Note that the engine cannot predict an overridden policy, so the connection-time check that
     * simulates what a not-yet-made edge would resolve to assumes the structural default; the
     * answer this returns is always what the node is actually taken to carry.
     **/
    virtual DataKindEnum resolveOutputDataKind(bool* isAmbiguous) const WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafeFrame;
    }

protected:
    /**
     * @brief Subclasses declare their static plugin metadata by overriding this.
     * It is queried on demand rather than cached: every call site above is off the
     * render path (registration, UI display, project load), so re-building the
     * small description each time keeps this class free of per-instance state.
     **/
    virtual NativePluginDescription getNativePluginDescription() const WARN_UNUSED_RETURN = 0;

    /**
     * @brief Wraps the AppManager::createKnob() idiom every initializeKnobs() repeats.
     **/
    template <typename KNOB_TYPE>
    std::shared_ptr<KNOB_TYPE> createKnob(const std::string& label,
                                          int dimension = 1)
    {
        return AppManager::createKnob<KNOB_TYPE>(this, label, dimension);
    }

    template <typename KNOB_TYPE>
    std::shared_ptr<KNOB_TYPE> createKnob(const QString& label,
                                          int dimension = 1)
    {
        return AppManager::createKnob<KNOB_TYPE>(this, label, dimension);
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_NativeEffectBase_h
