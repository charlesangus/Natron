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

#ifndef Engine_Nodes_OCIO_OcioNodeBase_h
#define Engine_Nodes_OCIO_OcioNodeBase_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>

#include <OpenColorIO/OpenColorIO.h>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/IO/OcioSpaceKnobs.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The processor an OCIO node renders one evaluation with. Both pointers are null for an
 * identity, and for a transform that cannot be built, which error then describes.
 **/
struct OcioProcessor {
    OCIO_NAMESPACE::ConstProcessorRcPtr processor;
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu;
    std::string error;
};

/**
 * @brief The OpenColorIO knobs every native OCIO node shares: one or more spaces, all on one
 * hidden ocioConfigFile and ocioWorkingSpace pair and one Context group, and the processor
 * lookup through the project's ProjectColorManagement.
 **/
class OcioNodeKnobs {
    Q_DECLARE_TR_FUNCTIONS(OcioNodeKnobs)

public:
    OcioNodeKnobs();

    ~OcioNodeKnobs();

    OcioNodeKnobs(const OcioNodeKnobs&) = delete;
    OcioNodeKnobs& operator=(const OcioNodeKnobs&) = delete;

    /**
     * @brief Sets the node the knobs belong to, which addSpace() also does, for a node with no
     * space.
     **/
    void attach(KnobHolder* holder);

    /**
     * @brief Adds a space on \p page. The first creates the hidden config pair and the Context
     * group; a later one shares them and goes before the Context group.
     **/
    void addSpace(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const OcioSpaceKnobs::Description& description,
                  const OcioSpaceKnobs::AutomaticSpace& automaticSpace,
                  const OcioSpaceKnobs::ChoiceMade& choiceMade = OcioSpaceKnobs::ChoiceMade());

    std::size_t spaceCount() const;

    std::string space(std::size_t index) const;

    /**
     * @brief The space at \p index, or the working space while it is empty, as its menu shows.
     **/
    std::string spaceOrWorking(std::size_t index) const;

    /**
     * @brief Puts \p knob, one of the node's own, on \p page before the shared Context group, or
     * at the end when there is none.
     **/
    void addKnob(const KnobPagePtr& page,
                 const KnobIPtr& knob);

    /**
     * @brief The working space the project pushed, else the project's.
     **/
    std::string workingSpace() const;

    std::vector<std::pair<std::string, std::string>> contextVariables(double time) const;

    /**
     * @brief Hands a knob change to every space. Returns true when \p k is one of their knobs.
     **/
    bool knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason);

    /**
     * @brief Refreshes the space menus, then calls each refresher added, in order.
     **/
    void refreshMenus();

    /**
     * @brief Adds \p refresher, for menus of the node's own that list the project's config.
     **/
    void addMenuRefresher(const std::function<void()>& refresher);

    ProjectColorManagementPtr colorManagement() const;

    /**
     * @brief A "direction" menu, Forward then Inverse, not yet on a page.
     **/
    static KnobChoicePtr createDirectionKnob(KnobHolder* holder);

    /**
     * @brief The direction a "direction" menu's \p index selects.
     **/
    static OCIO_NAMESPACE::TransformDirection direction(int index);

    /**
     * @brief Fills \p menu with \p names, each its own id, keeping the entry chosen before.
     **/
    static void populateMenu(const KnobChoicePtr& menu,
                             const std::vector<std::string>& names);

    /**
     * @brief Makes the next processor of a transform naming the file \p path read it again.
     **/
    void reloadFile(const std::string& path) const;

    /**
     * @brief The processor of \p transform with the Context variables at \p time. A null
     * transform, or one whose processor is a no-op, is an identity and gives no processor.
     **/
    OcioProcessor processor(const OCIO_NAMESPACE::ConstTransformRcPtr& transform,
                            double time) const;

    /**
     * @brief The transform from \p from to \p to, or null, for an identity, when either is empty
     * or both are the same space of the project's config.
     **/
    OCIO_NAMESPACE::ConstTransformRcPtr spaceTransform(const std::string& from,
                                                       const std::string& to) const;

    /**
     * @brief Calls \p callback whenever the project's config or working space changes, until
     * unwatchConfig() or destruction. Replaces an earlier callback.
     **/
    void watchConfig(const std::function<void()>& callback);

    void unwatchConfig();

    /**
     * @brief The row of a node rendering \p processor: the source, with the processor applied to
     * its RGB. Alpha is left as it is. Allocates nothing.
     **/
    static void applyRow(const OcioProcessor& processor,
                         const RowIO& io);

private:
    ProjectPtr project() const;

    KnobHolder* _holder;
    std::vector<std::unique_ptr<OcioSpaceKnobs>> _spaces;
    std::vector<std::function<void()>> _menuRefreshers;
    std::weak_ptr<ProjectColorManagement> _watched;
    int _watchId;
};

/**
 * @brief The op part of a native OCIO node: a Point op over RGB with the host's premult, mask and
 * mix, whose Params are the processor of DERIVED's transform. DERIVED holds its knobs, creates
 * them in createKnobs() with its spaces added to ocio, and provides
 *
 *     OCIO_NAMESPACE::ConstTransformRcPtr transform(const KnobReader& reader) const;
 *
 * reading its knobs, null for an identity.
 **/
template <typename DERIVED>
struct OcioOp {
    struct Params {
        OcioProcessor ocio;
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    Params bake(const KnobReader& reader) const
    {
        return Params { ocio.processor(static_cast<const DERIVED&>(*this).transform(reader), reader.time()) };
    }

    bool isIdentity(const Params& params) const
    {
        return !params.ocio.cpu;
    }

    void row(const Params& params,
             const RowIO& io) const
    {
        OcioNodeKnobs::applyRow(params.ocio, io);
    }

    GpuOpRef gpu(const Params& /*params*/) const
    {
        return GpuOpRef::none();
    }

    OcioNodeKnobs ocio;
};

/**
 * @brief A native OCIO node: an ImageNode over an OcioOp that takes RGB and RGBA, hands knob
 * changes to the op's spaces, refreshes its menus when the project's config changes and shows a
 * persistent error while its transform cannot be built, when it passes its input through.
 **/
template <typename OP>
class OcioNode
    : public ImageNode<OP> {
public:
    explicit OcioNode(NodePtr node)
        : ImageNode<OP>(node)
        , _knobsReady(false)
        , _showsError(false)
    {
    }

    virtual ~OcioNode()
    {
        this->op().ocio.unwatchConfig();
    }

    virtual void addAcceptedComponents(int inputNb,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE
    {
        if (this->isInputMask(inputNb)) {
            ImageNode<OP>::addAcceptedComponents(inputNb, comps);

            return;
        }
        comps->push_back(ImageLayerDesc::getRGBAComponents());
        comps->push_back(ImageLayerDesc::getRGBComponents());
    }

    const OcioNodeKnobs& ocioKnobs() const
    {
        return this->op().ocio;
    }

    /**
     * @brief The processor the node renders with at (time, view).
     **/
    OcioProcessor bakeOcio(double time,
                           ViewIdx view) const WARN_UNUSED_RETURN
    {
        return bakeParams(this->op(), time, view).ocio;
    }

    /**
     * @brief Shows the error of the transform at the current time as a persistent message, or
     * clears the one shown before.
     **/
    void refreshStatus()
    {
        if (!_knobsReady) {
            return;
        }
        std::string error;
        try {
            error = bakeOcio(this->getCurrentTime(), ViewIdx(0)).error;
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!error.empty()) {
            this->setPersistentMessage(eMessageTypeError, error);
            _showsError = true;
        } else if (_showsError) {
            this->clearPersistentMessage(false);
            _showsError = false;
        }
    }

protected:
    virtual void initializeKnobs() OVERRIDE
    {
        ImageNode<OP>::initializeKnobs();
        _knobsReady = true;
        this->op().ocio.watchConfig([this]() {
            this->op().ocio.refreshMenus();
            refreshStatus();
        });
    }

    virtual void onKnobsLoaded() OVERRIDE
    {
        this->op().ocio.refreshMenus();
        refreshStatus();
    }

    // A node a project load creates drops the messages it posts until it is created, which
    // includes the one onKnobsLoaded() posts, so the status is shown again once it is.
    virtual void onEffectCreated(bool /*mayCreateFileDialog*/,
                                 const CreateNodeArgs& /*args*/) OVERRIDE
    {
        refreshStatus();
    }

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/) OVERRIDE WARN_UNUSED_RETURN
    {
        const bool handled = this->op().ocio.knobChanged(k, reason);

        if (reason != eValueChangedReasonTimeChanged) {
            refreshStatus();
        }

        return handled;
    }

private:
    // The op's transform reads knobs that do not exist until initializeKnobs() has returned.
    bool _knobsReady;
    bool _showsError;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_OCIO_OcioNodeBase_h
