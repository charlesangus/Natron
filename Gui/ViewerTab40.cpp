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

#include "ViewerTab.h"
#include "ViewerTabPrivate.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

#include <QDebug>

#include <QApplication>
#include <QCheckBox>
#include <QToolBar>

#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h" // NodePtr
#include "Engine/OutputSchedulerThread.h" // RenderEngine
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Gui/Button.h"
#include "Gui/ChannelsComboBox.h"
#include "Gui/CurveEditor.h"
#include "Gui/CurveWidget.h"
#include "Gui/DopeSheetEditor.h"
#include "Gui/Gui.h"
#include "Gui/InfoViewerWidget.h"
#include "Gui/LineEdit.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/RenderStatsDialog.h"
#include "Gui/ScaleSliderQWidget.h"
#include "Gui/SpinBox.h"
#include "Gui/TabWidget.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewerGL.h"


NATRON_NAMESPACE_ENTER

void
ViewerTab::onInputChanged(int inputNb)
{
    ///rebuild the name maps
    NodePtr inp;
    const std::vector<NodeWPtr> &inputs  = _imp->viewerNode->getNode()->getGuiInputs();

    if ( (inputNb >= 0) && ( inputNb < (int)inputs.size() ) ) {
        inp = inputs[inputNb].lock();
    }


    if (inp) {
        ViewerTabPrivate::InputNamesMap::iterator found = _imp->inputNamesMap.find(inputNb);
        if ( found != _imp->inputNamesMap.end() ) {
            NodePtr input = found->second.input.lock();
            if (!input) {
                return;
            }
            const std::string & curInputName = input->getLabel();
            found->second.input = inp;
            int indexInA = _imp->firstInputImage->itemIndex( QString::fromUtf8( curInputName.c_str() ) );
            int indexInB = _imp->secondInputImage->itemIndex( QString::fromUtf8( curInputName.c_str() ) );
            assert(indexInA != -1 && indexInB != -1);
            found->second.name = QString::fromUtf8( inp->getLabel().c_str() );
            _imp->firstInputImage->setItemText(indexInA, found->second.name);
            _imp->secondInputImage->setItemText(indexInB, found->second.name);
        } else {
            ViewerTabPrivate::InputName inpName;
            inpName.input = inp;
            inpName.name = QString::fromUtf8( inp->getLabel().c_str() );
            _imp->inputNamesMap.insert( std::make_pair(inputNb, inpName) );
            _imp->firstInputImage->addItem(inpName.name);
            _imp->secondInputImage->addItem(inpName.name);
        }
    } else {
        ViewerTabPrivate::InputNamesMap::iterator found = _imp->inputNamesMap.find(inputNb);

        ///The input has been disconnected
        if ( found != _imp->inputNamesMap.end() ) {
            NodePtr input = found->second.input.lock();
            if (!input) {
                return;
            }
            const std::string & curInputName = input->getLabel();
            _imp->firstInputImage->blockSignals(true);
            _imp->secondInputImage->blockSignals(true);
            _imp->firstInputImage->removeItem( QString::fromUtf8( curInputName.c_str() ) );
            _imp->secondInputImage->removeItem( QString::fromUtf8( curInputName.c_str() ) );
            _imp->firstInputImage->blockSignals(false);
            _imp->secondInputImage->blockSignals(false);
            _imp->inputNamesMap.erase(found);
        }
    }

    //refreshLayerAndAlphaChannelComboBox();
} // ViewerTab::onInputChanged

void
ViewerTab::onInputNameChanged(int inputNb,
                              const QString & name)
{
    ViewerTabPrivate::InputNamesMap::iterator found = _imp->inputNamesMap.find(inputNb);

    assert( found != _imp->inputNamesMap.end() );
    int indexInA = _imp->firstInputImage->itemIndex(found->second.name);
    int indexInB = _imp->secondInputImage->itemIndex(found->second.name);
    assert(indexInA != -1 && indexInB != -1);
    _imp->firstInputImage->setItemText(indexInA, name);
    _imp->secondInputImage->setItemText(indexInB, name);
    found->second.name = name;
}

void
ViewerTab::manageSlotsForInfoWidget(int textureIndex,
                                    bool connect)
{
    RenderEnginePtr engine = _imp->viewerNode->getRenderEngine();

    assert(engine);
    if (connect) {
        QObject::connect( engine.get(), SIGNAL(fpsChanged(double,double)), _imp->infoWidget[textureIndex], SLOT(setFps(double,double)) );
        QObject::connect( engine.get(), SIGNAL(renderFinished(int)), _imp->infoWidget[textureIndex], SLOT(hideFps()) );
    } else {
        QObject::disconnect( engine.get(), SIGNAL(fpsChanged(double,double)), _imp->infoWidget[textureIndex],
                             SLOT(setFps(double,double)) );
        QObject::disconnect( engine.get(), SIGNAL(renderFinished(int)), _imp->infoWidget[textureIndex], SLOT(hideFps()) );
    }
}

void
ViewerTab::setImageFormat(int textureIndex,
                          const ImageLayerDesc& components,
                          ImageBitDepthEnum depth,
                          const QString& kindLabel)
{
    _imp->infoWidget[textureIndex]->setImageFormat(components, depth, kindLabel);
}

void
ViewerTab::setViewerPaused(bool paused,
                           bool allInputs)
{
    _imp->pauseButton->setChecked(paused);
    _imp->pauseButton->setDown(paused);
    _imp->viewerNode->setViewerPaused(paused, allInputs);
    abortRendering();
    if ( !_imp->viewerNode->getApp()->getProject()->isLoadingProject() ) {
        if (!paused) {
            // Refresh the viewer
            _imp->viewerNode->renderCurrentFrame(true);
        }
    }
}

bool
ViewerTab::isViewerPaused(int texIndex) const
{
    return _imp->viewerNode->isViewerPaused(texIndex);
}

void
ViewerTab::toggleViewerPauseMode(bool allInputs)
{
    bool isPaused = _imp->pauseButton->isDown();

    setViewerPaused(!isPaused, allInputs);
}

void
ViewerTab::onPauseViewerButtonClicked(bool clicked)
{
    bool allInputs = qApp->keyboardModifiers().testFlag(Qt::ShiftModifier);

    setViewerPaused(clicked, allInputs);
}

void
ViewerTab::editFrameRange(const QString& text,
                          int first,
                          int last,
                          quint64 gestureKey)
{
    if (first > last) {
        return;
    }
    editViewerKnobs(text, [this, first, last]() {
        _imp->setViewerValue<int>(kViewerParamInPoint, first);
        _imp->setViewerValue<int>(kViewerParamOutPoint, last); }, gestureKey);
    refreshFrameRangeFromKnobs();
}

void
ViewerTab::onPlaybackInButtonClicked()
{
    SequenceTime curIn, curOut;

    _imp->timeLineGui->getBounds(&curIn, &curOut);
    curIn = (SequenceTime)_imp->timeLineGui->getTimeline()->currentFrame();
    curIn = std::min(curIn, curOut);
    editFrameRange(tr("In Point"), curIn, curOut, 0);
    _imp->timeLineGui->recenterOnBounds();
}

void
ViewerTab::onPlaybackOutButtonClicked()
{
    SequenceTime curIn, curOut;

    _imp->timeLineGui->getBounds(&curIn, &curOut);
    curOut = (SequenceTime)_imp->timeLineGui->getTimeline()->currentFrame();
    curOut = std::max(curIn, curOut);
    editFrameRange(tr("Out Point"), curIn, curOut, 0);
    _imp->timeLineGui->recenterOnBounds();
}

void
ViewerTab::onPlaybackInSpinboxValueChanged(double value)
{
    SequenceTime curIn, curOut;

    _imp->timeLineGui->getBounds(&curIn, &curOut);
    curIn = (SequenceTime)value;
    curIn = std::min(curIn, curOut);
    editFrameRange(tr("In Point"), curIn, curOut, _imp->playBackInputSpinbox->getGestureKey());
    _imp->timeLineGui->recenterOnBounds();
}

void
ViewerTab::onPlaybackOutSpinboxValueChanged(double value)
{
    SequenceTime curIn, curOut;

    _imp->timeLineGui->getBounds(&curIn, &curOut);
    curOut = (SequenceTime)value;
    curOut = std::max(curIn, curOut);
    editFrameRange(tr("Out Point"), curIn, curOut, _imp->playBackOutputSpinbox->getGestureKey());
    _imp->timeLineGui->recenterOnBounds();
}

void
ViewerTab::setFrameRange(int left,
                         int right)
{
    setTimelineBounds(left, right);
    _imp->timeLineGui->recenterOnBounds();
}

void
ViewerTab::onTimelineBoundariesChanged(SequenceTime first,
                                       SequenceTime second)
{
    // A drag on the timeline is one undo entry; the timeline following the project's frame range
    // is not an edit of its own.
    if (_imp->timelineBoundsGesture.isOpen()) {
        editFrameRange(tr("Frame Range"), first, second, _imp->timelineBoundsGesture.key());
    } else {
        setTimelineBounds(first, second);
    }
}

void
ViewerTab::onCanSetFPSLabelClicked(bool toggled)
{
    onCanSetFPSClicked(toggled);
}

void
ViewerTab::onCanSetFPSClicked(bool toggled)
{
    editViewerKnobs(tr("Enable Fps"), [this, toggled]() {
        _imp->setViewerValue<bool>(kViewerParamEnableFps, toggled);
    });
    refreshFpsFromKnobs();
}

void
ViewerTab::setFPSLocked(bool fpsLocked)
{
    _imp->setViewerValue<bool>(kViewerParamEnableFps, !fpsLocked);
}

bool
ViewerTab::isFPSLocked() const
{
    return !_imp->viewerValue<bool>(kViewerParamEnableFps, false);
}

void
ViewerTab::connectToViewerCache()
{
    _imp->timeLineGui->connectSlotsToViewerCache();
}

void
ViewerTab::disconnectFromViewerCache()
{
    _imp->timeLineGui->disconnectSlotsFromViewerCache();
}

void
ViewerTab::clearTimelineCacheLine()
{
    if (_imp->timeLineGui) {
        _imp->timeLineGui->clearCachedFrames();
    }
}

void
ViewerTab::refreshTimelineCacheLineIfStale()
{
    if (_imp->timeLineGui) {
        _imp->timeLineGui->refreshCachedFramesIfStale();
    }
}

void
ViewerTab::toggleInfobarVisbility()
{
    bool visible;
    {
        QMutexLocker l(&_imp->visibleToolbarsMutex);
        visible = !_imp->infobarVisible;
    }

    setInfobarVisible(visible);
}

void
ViewerTab::togglePlayerVisibility()
{
    bool visible;
    {
        QMutexLocker l(&_imp->visibleToolbarsMutex);
        visible = !_imp->playerVisible;
    }

    setPlayerVisible(visible);
}

void
ViewerTab::toggleTimelineVisibility()
{
    bool visible;
    {
        QMutexLocker l(&_imp->visibleToolbarsMutex);
        visible = !_imp->timelineVisible;
    }

    setTimelineVisible(visible);
}

void
ViewerTab::toggleLeftToolbarVisiblity()
{
    bool visible;
    {
        QMutexLocker l(&_imp->visibleToolbarsMutex);
        visible = !_imp->leftToolbarVisible;
    }

    setLeftToolbarVisible(visible);
}

void
ViewerTab::toggleRightToolbarVisibility()
{
    bool visible;
    {
        QMutexLocker l(&_imp->visibleToolbarsMutex);
        visible =  !_imp->rightToolbarVisible;
    }

    setRightToolbarVisible(visible);
}

void
ViewerTab::toggleTopToolbarVisibility()
{
    bool visible;
    {
        QMutexLocker l(&_imp->visibleToolbarsMutex);
        visible = !_imp->topToolbarVisible;
    }

    setTopToolbarVisible(visible);
}

void
ViewerTab::setLeftToolbarVisible(bool visible)
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    _imp->leftToolbarVisible = visible;
    for (std::list<ViewerTabPrivate::PluginViewerContext>::iterator it = _imp->currentNodeContext.begin(); it != _imp->currentNodeContext.end(); ++it) {
        QToolBar* bar = it->currentContext->getToolBar();
        if (bar) {
            bar->setVisible(_imp->leftToolbarVisible);
        }
    }
}

void
ViewerTab::setRightToolbarVisible(bool visible)
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    _imp->rightToolbarVisible = visible;
}

void
ViewerTab::setTopToolbarVisible(bool visible)
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    _imp->topToolbarVisible = visible;
    _imp->firstSettingsRow->setVisible(_imp->topToolbarVisible);
    _imp->secondSettingsRow->setVisible(_imp->topToolbarVisible);
    for (std::list<ViewerTabPrivate::PluginViewerContext>::iterator it = _imp->currentNodeContext.begin(); it != _imp->currentNodeContext.end(); ++it) {
        it->currentContext->getContainerWidget()->setVisible(_imp->topToolbarVisible);
    }
}

void
ViewerTab::setPlayerVisible(bool visible)
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    _imp->playerVisible = visible;
    _imp->playerButtonsContainer->setVisible(_imp->playerVisible);
}

void
ViewerTab::setTimelineVisible(bool visible)
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    _imp->timelineVisible = visible;
    _imp->timeLineGui->setVisible(_imp->timelineVisible);
}

void
ViewerTab::setInfobarVisible(bool visible)
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    _imp->infobarVisible = visible;
    for (int i = 0; i < 2; ++i) {
        if (i == 1) {
            int inputIndex = -1;

            for (ViewerTabPrivate::InputNamesMap::iterator it = _imp->inputNamesMap.begin(); it != _imp->inputNamesMap.end(); ++it) {
                if ( it->second.name == _imp->secondInputImage->getCurrentIndexText() ) {
                    inputIndex = it->first;
                    break;
                }
            }
            if ( (getCompositingOperator() == eViewerCompositingOperatorNone) || (inputIndex == -1) ) {
                continue;
            }
        }

        _imp->infoWidget[i]->setVisible(_imp->infobarVisible);
    }
}

void
ViewerTab::showAllToolbars()
{
    if ( !isTopToolbarVisible() ) {
        toggleTopToolbarVisibility();
    }
    if ( !isRightToolbarVisible() ) {
        toggleRightToolbarVisibility();
    }
    if ( !isLeftToolbarVisible() ) {
        toggleLeftToolbarVisiblity();
    }
    if ( !isInfobarVisible() ) {
        toggleInfobarVisbility();
    }
    if ( !isPlayerVisible() ) {
        togglePlayerVisibility();
    }
    if ( !isTimelineVisible() ) {
        toggleTimelineVisibility();
    }
}

void
ViewerTab::hideAllToolbars()
{
    if ( isTopToolbarVisible() ) {
        toggleTopToolbarVisibility();
    }
    if ( isRightToolbarVisible() ) {
        toggleRightToolbarVisibility();
    }
    if ( isLeftToolbarVisible() ) {
        toggleLeftToolbarVisiblity();
    }
    if ( isInfobarVisible() ) {
        toggleInfobarVisbility();
    }
    if ( isPlayerVisible() ) {
        togglePlayerVisibility();
    }
    if ( isTimelineVisible() ) {
        toggleTimelineVisibility();
    }
}

bool
ViewerTab::isInfobarVisible() const
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    return _imp->infobarVisible;
}

bool
ViewerTab::isTopToolbarVisible() const
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    return _imp->topToolbarVisible;
}

bool
ViewerTab::isPlayerVisible() const
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    return _imp->playerVisible;
}

bool
ViewerTab::isTimelineVisible() const
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    return _imp->timelineVisible;
}

bool
ViewerTab::isLeftToolbarVisible() const
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    return _imp->leftToolbarVisible;
}

bool
ViewerTab::isRightToolbarVisible() const
{
    QMutexLocker l(&_imp->visibleToolbarsMutex);

    return _imp->rightToolbarVisible;
}

void
ViewerTab::setAsFileDialogViewer()
{
    _imp->isFileDialogViewer = true;
}

bool
ViewerTab::isFileDialogViewer() const
{
    return _imp->isFileDialogViewer;
}

void
ViewerTab::setCustomTimeline(const TimeLinePtr& timeline)
{
    _imp->timeLineGui->setTimeline(timeline);
    manageTimelineSlot(true, timeline);
}

void
ViewerTab::manageTimelineSlot(bool disconnectPrevious,
                              const TimeLinePtr& timeline)
{
    if (disconnectPrevious) {
        TimeLinePtr previous = _imp->timeLineGui->getTimeline();
        QObject::disconnect( previous.get(), SIGNAL(frameChanged(SequenceTime,int)),
                             this, SLOT(onTimeLineTimeChanged(SequenceTime,int)) );
    }

    QObject::connect( timeline.get(), SIGNAL(frameChanged(SequenceTime,int)),
                      this, SLOT(onTimeLineTimeChanged(SequenceTime,int)) );
}

TimeLinePtr
ViewerTab::getTimeLine() const
{
    return _imp->timeLineGui->getTimeline();
}

bool
ViewerTab::isPickerEnabled() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->pickerButton->isChecked();
}

void
ViewerTab::setPickerEnabled(bool enabled)
{
    _imp->pickerButton->setChecked(enabled);
    _imp->pickerButton->setDown(enabled);
    setInfobarVisible(true);
}

void
ViewerTab::onMousePressCalledInViewer()
{
    takeClickFocus();
    if ( getGui() ) {
        getGui()->setActiveViewer(this);
    }
}

void
ViewerTab::onPickerButtonClickedInternal(ViewerTab* caller,
                                         bool clicked)
{
    if (this == caller) {
        const std::list<ViewerTab*> &viewers = getGui()->getViewersList();
        for (std::list<ViewerTab*>::const_iterator it = viewers.begin(); it != viewers.end(); ++it) {
            if ( (*it) != caller ) {
                (*it)->onPickerButtonClickedInternal(caller, clicked);
            }
        }
    }

    _imp->pickerButton->setDown(clicked);

    setInfobarVisible(clicked);
    getGui()->clearColorPickers();
}

void
ViewerTab::onPickerButtonClicked(bool clicked)
{
    onPickerButtonClickedInternal(this, clicked);
}

void
ViewerTab::onCheckerboardButtonClicked()
{
    const bool enabled = !isCheckerboardEnabled();

    editViewerKnobs(tr("Enable Checkerboard"), [this, enabled]() {
        _imp->setViewerValue<bool>(kViewerParamCheckerboard, enabled);
    });
    refreshFromViewerKnob(kViewerParamCheckerboard);
}

bool
ViewerTab::isCheckerboardEnabled() const
{
    return _imp->viewerValue<bool>(kViewerParamCheckerboard, false);
}

void
ViewerTab::setCheckerboardEnabled(bool enabled)
{
    _imp->setViewerValue<bool>(kViewerParamCheckerboard, enabled);
}

void
ViewerTab::onSpinboxFpsChanged(double fps)
{
    if ( !getGui() ) {
        // A focus out event can reach the box while the gui is closing.
        return;
    }
    editViewerKnobs(tr("Desired Fps"), [this, fps]() { _imp->setViewerValue<double>(kViewerParamFps, fps); }, _imp->fpsBox->getGestureKey());
    refreshFpsFromKnobs();
}

double
ViewerTab::getDesiredFps() const
{
    QMutexLocker l(&_imp->fpsMutex);

    return _imp->fps;
}

void
ViewerTab::setDesiredFps(double fps)
{
    _imp->setViewerValue<double>(kViewerParamFps, fps);
}

void
ViewerTab::setProjection(double zoomLeft,
                         double zoomBottom,
                         double zoomFactor,
                         double zoomAspectRatio)
{
    _imp->viewer->setProjection(zoomLeft, zoomBottom, zoomFactor, zoomAspectRatio);
    QString str = QString::number( std::floor(zoomFactor * 100 + 0.5) );
    str.append( QLatin1Char('%') );
    str.prepend( QString::fromUtf8("  ") );
    str.append( QString::fromUtf8("  ") );
    _imp->zoomCombobox->setCurrentText_no_emit(str);
}

void
ViewerTab::refreshViewerRenderingState()
{
    int value = _imp->viewerNode->getNode()->getIsNodeRenderingCounter();

    if (value >= 1) {
        _imp->refreshButton->setIcon(_imp->iconRefreshOn);
    } else {
        _imp->refreshButton->setIcon(_imp->iconRefreshOff);
    }
}

void
ViewerTab::setTurboButtonDown(bool down)
{
    _imp->turboButton->setDown(down);
    _imp->turboButton->setChecked(down);
}

void
ViewerTab::redrawGLWidgets()
{
    _imp->viewer->update();
    _imp->timeLineGui->update();
}

void
ViewerTab::getTimelineBounds(int* left,
                             int* right) const
{
    return _imp->timeLineGui->getBounds(left, right);
}

void
ViewerTab::setTimelineBounds(int left,
                             int right)
{
    if (left > right) {
        return;
    }
    _imp->setViewerValue<int>(kViewerParamInPoint, left);
    _imp->setViewerValue<int>(kViewerParamOutPoint, right);
}

void
ViewerTab::centerOn(SequenceTime left,
                    SequenceTime right)
{
    _imp->timeLineGui->centerOn(left, right);
}

void
ViewerTab::centerOn_tripleSync(SequenceTime left,
                               SequenceTime right)
{
    _imp->timeLineGui->centerOn_tripleSync(left, right);
}

void
ViewerTab::setFrameRangeEdited(bool edited)
{
    _imp->timeLineGui->setFrameRangeEdited(edited);
}

void
ViewerTab::onInternalNodeLabelChanged(const QString& name)
{
    TabWidget* parent = dynamic_cast<TabWidget*>( parentWidget() );

    if (parent) {
        setLabel( name.toStdString() );
        parent->setTabLabel(this, name);
    }
}

void
ViewerTab::onInternalNodeScriptNameChanged(const QString& /*name*/)
{
    // always running in the main thread
    std::string newName = _imp->viewerNode->getNode()->getFullyQualifiedName();
    if ( NATRON_PYTHON_NAMESPACE::isKeyword(newName) ) {
        Dialogs::errorDialog(_imp->viewerNode->getNode()->getLabel(), newName + " is a Python keyword");

        return;
    }
    std::string oldName = getScriptName();

    for (std::size_t i = 0; i < newName.size(); ++i) {
        if (newName[i] == '.') {
            newName[i] = '_';
        }
    }


    assert( qApp && qApp->thread() == QThread::currentThread() );
    getGui()->unregisterTab(this);
    setScriptName(newName);
    getGui()->registerTab(this, this);
    TabWidget* parent = dynamic_cast<TabWidget*>( parentWidget() );
    if (parent) {
        parent->onTabScriptNameChanged(this, oldName, newName);
    }
}

static ChoiceOption
viewerLayerChoiceOption(const ImageLayerDesc& layer)
{
    // A colour view's label is its own ID ("rgba", not "rgba." + a channels label): getLayerOption()
    // appends getChannelsLabel(), which views leave empty.
    if (ImageLayerDesc::isColorViewID(layer.getLayerID())) {
        return ChoiceOption(layer.getLayerID(), layer.getLayerLabel(), "");
    }

    return layer.getLayerOption();
}

// The alpha menu entry, counting the leading "-", of layer's last channel. A colour view's channel
// is listed under the one view the alpha menu keeps for the stream's layout, and is absent when
// that layout lacks it.
static int
viewerAlphaMenuIndexOfLastChannel(const ImageLayerDesc& layer,
                                  const std::list<ImageLayerDesc>& alphaComponents)
{
    const int lastChannel = (int)layer.getChannels().size() - 1;
    const bool isColorView = ImageLayerDesc::isColorViewID(layer.getLayerID());
    const int bit = isColorView ? ImageLayerDesc::colorViewChannelBit(layer.getLayerID(), lastChannel) : -1;
    int index = 1;

    for (std::list<ImageLayerDesc>::const_iterator it = alphaComponents.begin(); it != alphaComponents.end(); ++it) {
        if (isColorView && it->isColorLayer()) {
            const int channel = ImageLayerDesc::colorViewChannelIndex(it->getLayerID(), bit);

            return (channel == -1) ? -1 : (index + channel);
        }
        if (!isColorView && (*it == layer)) {
            return index + lastChannel;
        }
        index += (int)it->getChannels().size();
    }

    return -1;
}

// The alpha menu entry, counting the leading "-", whose channel ID is alphaID. A colour channel saved
// under another view (rgba.R after the stream became RGB) is the entry for the same logical channel.
// -1 when the menu lists no such channel.
static int
viewerAlphaMenuIndexOfID(const std::string& alphaID,
                         const std::list<ImageLayerDesc>& alphaComponents)
{
    int index = 1;

    for (std::list<ImageLayerDesc>::const_iterator it = alphaComponents.begin(); it != alphaComponents.end(); ++it) {
        for (int i = 0; i < (int)it->getChannels().size(); ++i, ++index) {
            if (it->getChannelOption(i).id == alphaID) {
                return index;
            }
        }
    }

    const std::string::size_type dot = alphaID.rfind('.');
    if (dot == std::string::npos) {
        return -1;
    }
    const std::string viewID = alphaID.substr(0, dot);
    if (!ImageLayerDesc::isColorViewID(viewID)) {
        return -1;
    }
    const std::vector<std::string>& viewChannels = ImageLayerDesc::getColorView(viewID).getChannels();
    const std::vector<std::string>::const_iterator found = std::find(viewChannels.begin(), viewChannels.end(), alphaID.substr(dot + 1));
    if (found == viewChannels.end()) {
        return -1;
    }
    const int bit = ImageLayerDesc::colorViewChannelBit(viewID, (int)(found - viewChannels.begin()));

    index = 1;
    for (std::list<ImageLayerDesc>::const_iterator it = alphaComponents.begin(); it != alphaComponents.end(); ++it) {
        if (it->isColorLayer()) {
            const int channel = ImageLayerDesc::colorViewChannelIndex(it->getLayerID(), bit);

            return (channel == -1) ? -1 : (index + channel);
        }
        index += (int)it->getChannels().size();
    }

    return -1;
}

// The channel ID of an alpha menu entry, counting the leading "-"; empty for "-" or past the end.
static std::string
viewerAlphaIDAtMenuIndex(int menuIndex,
                         const std::list<ImageLayerDesc>& alphaComponents)
{
    int index = 1;

    for (std::list<ImageLayerDesc>::const_iterator it = alphaComponents.begin(); it != alphaComponents.end(); ++it) {
        const int count = (int)it->getChannels().size();
        if (menuIndex < index + count) {
            return (menuIndex >= index) ? it->getChannelOption(menuIndex - index).id : std::string();
        }
        index += count;
    }

    return std::string();
}

void
ViewerTab::autoSwitchDisplayChannelsForLayer(const ImageLayerDesc& chosen)
{
    if (chosen.getNumComponents() == 1) {
        writeDisplayChannels(5, true);
        _imp->viewerChannelsAutoswitchedToAlpha = true;
    } else if (_imp->viewerChannelsAutoswitchedToAlpha && (chosen.getNumComponents() > 1) && (_imp->viewerValue<int>(kViewerParamDisplayChannels, 1) == 5)) {
        writeDisplayChannels(1, true);
        _imp->viewerChannelsAutoswitchedToAlpha = false;
    }
}

void
ViewerTab::refreshLayerAndAlphaChannelComboBox()
{
    TimeLinePtr timeline = getTimeLine();

    refreshLayerAndAlphaChannelComboBoxAtTime(timeline ? timeline->currentFrame() : 0.);
}

void
ViewerTab::refreshLayerAndAlphaChannelComboBoxAtTime(double time)
{
    KnobLayerSelectPtr layerKnob = _imp->viewerKnob<KnobLayerSelect>(kViewerParamLayer);
    KnobChannelSelectPtr alphaKnob = _imp->viewerKnob<KnobChannelSelect>(kViewerParamAlphaChannel);
    if (!layerKnob || !alphaKnob) {
        return;
    }

    _imp->layerMenuTime = time;
    _imp->layerMenuView = _imp->viewerNode->getViewerCurrentView();
    std::list<ImageLayerDesc> components;
    std::list<ImageLayerDesc> alphaComponents;
    _imp->getLayerViewsAvailabelAtTime(_imp->layerMenuTime, _imp->layerMenuView, &components, &alphaComponents, NULL);

    const QString noChoice = QString::fromUtf8("-");
    _imp->layerChoice->clear();
    _imp->alphaChannelChoice->clear();
    _imp->layerChoice->addItem(noChoice);
    _imp->alphaChannelChoice->addItem(noChoice);

    // The menus show what the knobs hold and never write them: a saved layer or channel the input
    // lacks stays in the knob, shown as an unlisted entry, while the viewer renders without it.
    const std::string layerID = layerKnob->getLayer();
    int layerIndex = layerID.empty() ? 0 : -1;
    int firstColorIndex = -1;
    int index = 1;
    for (std::list<ImageLayerDesc>::iterator it = components.begin(); it != components.end(); ++it, ++index) {
        _imp->layerChoice->addItem(QString::fromUtf8(viewerLayerChoiceOption(*it).label.c_str()));
        if ((layerIndex == -1) && (it->getLayerID() == layerID)) {
            layerIndex = index;
        }
        if (it->isColorLayer() && (firstColorIndex == -1)) {
            firstColorIndex = index;
        }
    }
    // A colour view always resolves to the stream's colour plane, whatever views the stream lists.
    if ((layerIndex == -1) && ImageLayerDesc::isColorViewID(layerID)) {
        layerIndex = firstColorIndex;
    }
    if (layerIndex != -1) {
        _imp->layerChoice->setCurrentIndex_no_emit(layerIndex);
    } else {
        _imp->layerChoice->setCurrentText_no_emit(QString::fromUtf8(layerID.c_str()));
    }

    for (std::list<ImageLayerDesc>::iterator it = alphaComponents.begin(); it != alphaComponents.end(); ++it) {
        for (int i = 0; i < (int)it->getChannels().size(); ++i) {
            _imp->alphaChannelChoice->addItem(QString::fromUtf8(it->getChannelOption(i).label.c_str()));
        }
    }
    const std::string alphaID = alphaKnob->get();
    const int alphaIndex = alphaID.empty() ? 0 : viewerAlphaMenuIndexOfID(alphaID, alphaComponents);
    if (alphaIndex != -1) {
        _imp->alphaChannelChoice->setCurrentIndex_no_emit(alphaIndex);
    } else {
        _imp->alphaChannelChoice->setCurrentText_no_emit(QString::fromUtf8(alphaID.c_str()));
    }
} // ViewerTab::refreshLayerAndAlphaChannelComboBoxAtTime

void
ViewerTab::onAlphaChannelComboChanged(int index)
{
    std::list<ImageLayerDesc> components;

    _imp->getLayerViewsAvailabelAtTime(_imp->layerMenuTime, _imp->layerMenuView, NULL, &components, NULL);
    const std::string alphaID = viewerAlphaIDAtMenuIndex(index, components);

    editViewerKnobs(tr("Alpha Channel"), [this, alphaID]() {
        KnobChannelSelectPtr knob = _imp->viewerKnob<KnobChannelSelect>(kViewerParamAlphaChannel);
        if (knob && (knob->get() != alphaID)) {
            knob->set(alphaID);
        }
    });
    refreshLayerAndAlphaChannelComboBox();
}

void
ViewerTab::onLayerComboChanged(int index)
{
    std::list<ImageLayerDesc> components;
    std::list<ImageLayerDesc> alphaComponents;

    _imp->getLayerViewsAvailabelAtTime(_imp->layerMenuTime, _imp->layerMenuView, &components, &alphaComponents, NULL);
    if ( ( index >= (int)(components.size() + 1) ) || (index < 0) ) {
        qDebug() << "ViewerTab::onLayerComboChanged: invalid index";

        return;
    }
    std::list<ImageLayerDesc>::iterator chosen = components.begin();
    std::advance(chosen, std::max(index - 1, 0));
    const bool none = (index == 0);

    editViewerKnobs(tr("Layer"), [&]() {
        KnobLayerSelectPtr layerKnob = _imp->viewerKnob<KnobLayerSelect>(kViewerParamLayer);
        KnobChannelSelectPtr alphaKnob = _imp->viewerKnob<KnobChannelSelect>(kViewerParamAlphaChannel);
        if (!layerKnob || !alphaKnob) {
            return;
        }
        if (none) {
            if (!layerKnob->getLayer().empty()) {
                layerKnob->setLayer(std::string());
            }
            if (!alphaKnob->get().empty()) {
                alphaKnob->setNone();
            }

            return;
        }
        const std::vector<std::string>& channels = chosen->getChannels();
        // A single-channel layer is shown through the A display, which reads the alpha channel
        // rather than the layer: point the alpha channel at that one channel too, or picking the
        // alpha view would keep showing the colour image. When the stream lacks a colour view's
        // last channel, the alpha channel names it anyway and the viewer reads zero.
        std::string alphaID;
        if ((channels.size() == 4) || (channels.size() == 1)) {
            const int alphaIndex = viewerAlphaMenuIndexOfLastChannel(*chosen, alphaComponents);
            if (alphaIndex != -1) {
                alphaID = viewerAlphaIDAtMenuIndex(alphaIndex, alphaComponents);
            }
        }
        if (alphaID.empty() && ImageLayerDesc::isColorViewID(chosen->getLayerID()) && !channels.empty()) {
            alphaID = chosen->getChannelOption((int)channels.size() - 1).id;
        }
        if (!alphaID.empty() && (alphaKnob->get() != alphaID)) {
            alphaKnob->set(alphaID);
        }
        autoSwitchDisplayChannelsForLayer(*chosen);
        if (layerKnob->getLayer() != chosen->getLayerID()) {
            layerKnob->setLayer(chosen->getLayerID());
        }
    });
    refreshLayerAndAlphaChannelComboBox();
}

void
ViewerTab::onGainToggled(bool clicked)
{
    editViewerKnobs(tr("Enable Gain"), [this, clicked]() {
        _imp->setViewerValue<bool>(kViewerParamEnableGain, clicked);
    });
    // The click toggled the button; the knob may have refused the change.
    refreshColorControlsFromKnobs();
}

void
ViewerTab::setGainFromToolbar(double fstop,
                              quint64 gestureKey)
{
    // 0 f-stops switches gain off and keeps the knob's value for re-enabling.
    editViewerKnobs(tr("Gain"), [this, fstop]() {
        _imp->setViewerValue<bool>(kViewerParamEnableGain, fstop != 0.);
        if (fstop != 0.) {
            _imp->setViewerValue<double>(kViewerParamGain, fstop);
        } }, gestureKey);
    refreshColorControlsFromKnobs();
}

void
ViewerTab::onGainSliderChanged(double v)
{
    setGainFromToolbar(v, _imp->gainSlider->getGestureKey());
}

void
ViewerTab::onGainSpinBoxValueChanged(double v)
{
    setGainFromToolbar(v, _imp->gainBox->getGestureKey());
}

void
ViewerTab::onGammaToggled(bool clicked)
{
    editViewerKnobs(tr("Enable Gamma"), [this, clicked]() {
        _imp->setViewerValue<bool>(kViewerParamEnableGamma, clicked);
    });
    refreshColorControlsFromKnobs();
}

void
ViewerTab::setGammaFromToolbar(double gamma,
                               quint64 gestureKey)
{
    editViewerKnobs(tr("Gamma"), [this, gamma]() {
        _imp->setViewerValue<bool>(kViewerParamEnableGamma, gamma != 1.);
        if (gamma != 1.) {
            _imp->setViewerValue<double>(kViewerParamGamma, gamma);
        } }, gestureKey);
    refreshColorControlsFromKnobs();
}

void
ViewerTab::onGammaSliderValueChanged(double value)
{
    setGammaFromToolbar(value, _imp->gammaSlider->getGestureKey());
}

void
ViewerTab::onGammaSpinBoxValueChanged(double value)
{
    setGammaFromToolbar(value, _imp->gammaBox->getGestureKey());
}

void
ViewerTab::onGammaSliderEditingFinished(bool hasMovedOnce)
{
    bool autoProxyEnabled = appPTR->getCurrentSettings()->isAutoProxyEnabled();

    if (autoProxyEnabled && hasMovedOnce) {
        getGui()->renderAllViewers(true);
    }
}

void
ViewerTab::onGainSliderEditingFinished(bool hasMovedOnce)
{
    bool autoProxyEnabled = appPTR->getCurrentSettings()->isAutoProxyEnabled();

    if (autoProxyEnabled && hasMovedOnce) {
        getGui()->renderAllViewers(true);
    }
}

bool
ViewerTab::isViewersSynchroEnabled() const
{
    return _imp->syncViewerButton->isDown();
}

void
ViewerTab::synchronizeOtherViewersProjection()
{
    assert( getGui() );
    getGui()->setMasterSyncViewer(this);
    double left, bottom, factor, par;
    _imp->viewer->getProjection(&left, &bottom, &factor, &par);
    const std::list<ViewerTab*>& viewers = getGui()->getViewersList();
    for (std::list<ViewerTab*>::const_iterator it = viewers.begin(); it != viewers.end(); ++it) {
        if ( (*it) != this ) {
            (*it)->getViewer()->setProjection(left, bottom, factor, par);
            (*it)->getInternalNode()->renderCurrentFrame(true);
        }
    }
}

void
ViewerTab::onSyncViewersButtonPressed(bool clicked)
{
    const std::list<ViewerTab*>& viewers = getGui()->getViewersList();

    for (std::list<ViewerTab*>::const_iterator it = viewers.begin(); it != viewers.end(); ++it) {
        (*it)->_imp->syncViewerButton->setDown(clicked);
        (*it)->_imp->syncViewerButton->setChecked(clicked);
    }
    if (clicked) {
        synchronizeOtherViewersProjection();
    }
}

void
ViewerTab::zoomIn()
{
    double factor = _imp->viewer->getZoomFactor();

    factor *= 1.1;
    factor *= 100;
    _imp->viewer->zoomSlot(factor);
    QString text = QString::number( std::floor(factor + 0.5) ) + QLatin1Char('%');
    _imp->zoomCombobox->setCurrentText_no_emit(text);
}

void
ViewerTab::zoomOut()
{
    double factor = _imp->viewer->getZoomFactor();

    factor *= 0.9;
    factor *= 100;
    _imp->viewer->zoomSlot(factor);
    QString text = QString::number( std::floor(factor + 0.5) ) + QLatin1Char('%');
    _imp->zoomCombobox->setCurrentText_no_emit(text);
}

void
ViewerTab::onZoomComboboxCurrentIndexChanged(int /*index*/)
{
    QString text = _imp->zoomCombobox->getCurrentIndexText();

    if ( text == QString::fromUtf8("+") ) {
        zoomIn();
    } else if ( text == QString::fromUtf8("-") ) {
        zoomOut();
    } else if ( text == QString::fromUtf8("Fit") ) {
        centerViewer();
    } else {
        text.remove( QLatin1Char('%') );
        int v = text.toInt();
        assert(v > 0);
        _imp->viewer->zoomSlot(v);
    }
}

void
ViewerTab::onRenderStatsAvailable(int time,
                                  ViewIdx view,
                                  double wallTime,
                                  const RenderStatsMap& stats)
{
    assert( QThread::currentThread() == qApp->thread() );
    RenderStatsDialog* dialog = getGui()->getRenderStatsDialog();
    if (dialog) {
        dialog->addStats(time, view, wallTime, stats);
    }
}

void
ViewerTab::toggleTripleSync(bool toggled)
{
    _imp->tripleSyncButton->setDown(toggled);
    getGui()->setTripleSyncEnabled(toggled);
    if (toggled) {
        DopeSheetEditor* deditor = getGui()->getDopeSheetEditor();
        CurveEditor* cEditor = getGui()->getCurveEditor();
        //Sync curve editor and dopesheet tree width
        cEditor->setTreeWidgetWidth( deditor->getTreeWidgetWidth() );

        SequenceTime left, right;
        _imp->timeLineGui->getVisibleRange(&left, &right);
        getGui()->centerOpenedViewersOn(left, right);
        deditor->centerOn(left, right);
        cEditor->getCurveWidget()->centerOn(left, right);
    }
}

void
ViewerTab::onPanelMadeCurrent()
{
    // Refresh the image since so far the viewer was probably not in sync with internal data
    if ( _imp->viewerNode && getGui() && !getGui()->getApp()->getProject()->isLoadingProject() && getGui()->getApp()->getNodesBeingCreated().empty() ) {
        _imp->viewerNode->renderCurrentFrame(true);
    }
}

NATRON_NAMESPACE_EXIT
