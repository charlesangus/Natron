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

#ifndef Engine_RotoObjectSelect_h
#define Engine_RotoObjectSelect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QPointF>

#include "Engine/Ai/MaskTrace.h"
#include "Engine/Ai/ModelKnob.h"
#include "Engine/Ai/Segmentation.h"
#include "Engine/EngineFwd.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

class RotoCurveInteract;

// The knobs of the object select tool, by script name.
#define kRotoUIParamObjectThreshold "objectThreshold"
#define kRotoUIParamObjectThresholdLabel "Object Threshold"
#define kRotoUIParamObjectThresholdHint "The probability above which a pixel belongs to the object."

#define kRotoUIParamObjectSimplify "objectSimplify"
#define kRotoUIParamObjectSimplifyLabel "Object Simplify"
#define kRotoUIParamObjectSimplifyHint "How far, in pixels, the shapes may stray from the mask's outline. Higher values give fewer control points."

#define kRotoUIParamObjectMinArea "objectMinArea"
#define kRotoUIParamObjectMinAreaLabel "Object Min Area"
#define kRotoUIParamObjectMinAreaHint "Outlines and holes enclosing fewer square pixels than this are dropped."

#define kRotoUIParamObjectHoles "objectHoles"
#define kRotoUIParamObjectHolesLabel "Object Holes"
#define kRotoUIParamObjectHolesHint "Holes in the mask become shapes that subtract from the object's shape."

#define kRotoUIParamObjectSmooth "objectSmooth"
#define kRotoUIParamObjectSmoothLabel "Object Smooth"
#define kRotoUIParamObjectSmoothHint "Fits smooth curves through the outline. Off keeps straight segments between its corners."

#define kRotoUIParamObjectFeather "objectFeather"
#define kRotoUIParamObjectFeatherLabel "Object Feather"
#define kRotoUIParamObjectFeatherHint "The feather distance the committed shapes get."

#define kRotoUIParamObjectClearPrompts "objectClearPrompts"
#define kRotoUIParamObjectClearPromptsLabel "Object Clear Prompts"
#define kRotoUIParamObjectClearPromptsHint "Removes every click and box of the current selection (Esc)."

/**
 * @brief The object select tool of the Roto node: clicks and boxes on the viewer prompt a
 * segmentation model run on the Source input, the mask it returns is traced into outlines shown
 * as a preview, and committing turns the outlines into Beziers.
 *
 * Prompts are kept in canonical coordinates. The Source is rendered at full resolution, converted
 * to the project's default display and view as 8-bit sRGB and resampled to square pixels before it
 * is encoded, on a worker thread; prompts made meanwhile wait for the encoding. Everything else
 * runs on the main thread.
 **/
class RotoObjectSelect {
public:
    struct Prompt {
        enum Kind {
            ePositive,
            eNegative,
            eBox,
        };
        Kind kind = ePositive;
        // A point, or a box's two opposite corners.
        QPointF a;
        QPointF b;
    };

    explicit RotoObjectSelect(RotoCurveInteract* roto);
    ~RotoObjectSelect();

    RotoObjectSelect(const RotoObjectSelect&) = delete;
    RotoObjectSelect& operator=(const RotoObjectSelect&) = delete;

    /**
     * @brief Creates the model picker and the tool's settings on page and adds them to the viewer
     * UI, hidden until the tool's role is selected. Call from the host's initializeKnobs().
     **/
    void createKnobs(const KnobPagePtr& page);

    // Shows or hides the tool's viewer knobs. Leaving the tool drops its prompts.
    void setActive(bool active);
    bool isActive() const
    {
        return _active;
    }

    // Handles a change of one of the tool's knobs; returns false for any other knob.
    bool onKnobChanged(KnobI* k);

    // Selects the user's default model on a node the user creates.
    void onEffectCreated(const CreateNodeArgs& args);

    // Enables the tool's toolbar button only while the Source input is connected.
    void refreshSourceState();
    void setToolButton(const KnobButtonPtr& button);

    // The prompts are for one frame: moving to another drops them.
    void onTimeChanged(double time);

    void draw(double time);
    bool penDown(double time, const QPointF& pos, bool negative);
    bool penMotion(const QPointF& pos);
    bool penUp();
    bool removeLastPrompt();
    bool clearPrompts();
    // Turns the previewed outlines into Beziers, as one undo step. Returns false if there are none.
    bool commit();

    const std::vector<Prompt>& prompts() const
    {
        return _prompts;
    }

    // The previewed outlines, in canonical coordinates, enclosing shapes first.
    const std::vector<MaskTrace::Contour>& previewContours() const
    {
        return _contours;
    }

private:
    // Where the encoded image sits in canonical coordinates. Encoded pixels are square, with y down.
    struct Frame {
        SegmentationKey key;
        // The canonical rectangle the encoded image covers.
        RectD canonical;
        int width = 0;
        int height = 0;
    };

    struct Job;
    struct Shared;

    static void runJob(Job* job);

    MaskTrace::Options traceOptions() const;
    QPointF toEncoded(const QPointF& canonical) const;
    MaskTrace::Point toCanonical(const MaskTrace::Point& encoded) const;
    NodePtr sourceNode() const;
    // The key of the Source at time under the selected model; sets the viewer message if none.
    bool currentKey(double time, SegmentationKey* key, std::optional<ModelKnob::Resolved>* model);
    void addPrompt(double time, const Prompt& prompt);
    void requestEncoding(double time, const SegmentationKey& key, const ModelKnob::Resolved& model);
    void onJobFinished(const std::shared_ptr<Job>& job);
    // Segments the prompts again, encoding the Source first when it has to.
    void refreshMask();
    // Traces the current mask again.
    void retrace();
    void dropPrompts();
    void setMessage(const std::string& message);
    void redraw();
    void joinWorker();

    RotoCurveInteract* const _roto;
    std::unique_ptr<ModelKnob> _model;
    KnobDoubleWPtr _threshold;
    KnobDoubleWPtr _simplify;
    KnobDoubleWPtr _minArea;
    KnobBoolWPtr _holes;
    KnobBoolWPtr _smooth;
    KnobDoubleWPtr _feather;
    KnobButtonWPtr _clearPrompts;
    KnobButtonWPtr _toolButton;
    bool _active = false;

    std::vector<Prompt> _prompts;
    double _promptsTime = 0.;
    // The press of a click or box being made.
    bool _pressed = false;
    bool _dragging = false;
    bool _pressNegative = false;
    double _pressTime = 0.;
    QPointF _pressPos;
    QPointF _dragPos;

    std::shared_ptr<Segmenter> _segmenter;
    std::string _segmenterModelId;
    // The geometry of the encodings the segmenter caches, most recent first.
    std::list<Frame> _frames;
    // The frame the mask belongs to.
    Frame _frame;
    std::vector<float> _mask;
    // Canonical coordinates.
    std::vector<MaskTrace::Contour> _contours;
    std::string _message;
    // An encoding that failed is not retried until the prompts change.
    bool _hasFailed = false;
    SegmentationKey _failedKey;

    // Queued callbacks from the worker hold this weakly, so that none runs after destruction.
    std::shared_ptr<Shared> _shared;
    std::thread _worker;
    std::shared_ptr<Job> _running;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoObjectSelect_h
