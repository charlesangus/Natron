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

#include "ActionShortcuts.h"

#include <stdexcept>

#include <QCoreApplication>
#include <QKeySequence>
#include <QWidget>

#include "Gui/GuiApplicationManager.h"

NATRON_NAMESPACE_ENTER

ActionWithShortcut::ActionWithShortcut(const std::string & group,
                                       const std::string & actionID,
                                       const std::string & actionDescription,
                                       QObject* parent,
                                       bool setShortcutOnAction)
    : QAction(parent)
    , _group( QString::fromUtf8( group.c_str() ) )
    , _shortcuts()
{
    // insert here the output of:
    // fgrep "#define kShortcutDescAction" ActionShortcuts.h | sed -e 's/#define /(void)QT_TR_NOOP(/' -e 's/ .*/);/'
    (void)QT_TR_NOOP(kShortcutDescActionNewProject);
    (void)QT_TR_NOOP(kShortcutDescActionOpenProject);
    (void)QT_TR_NOOP(kShortcutDescActionCloseProject);
    (void)QT_TR_NOOP(kShortcutDescActionReloadProject);
    (void)QT_TR_NOOP(kShortcutDescActionSaveProject);
    (void)QT_TR_NOOP(kShortcutDescActionSaveAsProject);
    (void)QT_TR_NOOP(kShortcutDescActionSaveAndIncrVersion);
    (void)QT_TR_NOOP(kShortcutDescActionExportProject);
    (void)QT_TR_NOOP(kShortcutDescActionPreferences);
    (void)QT_TR_NOOP(kShortcutDescActionQuit);
    (void)QT_TR_NOOP(kShortcutDescActionProjectSettings);
    (void)QT_TR_NOOP(kShortcutDescActionShowErrorLog);
    (void)QT_TR_NOOP(kShortcutDescActionNewViewer);
    (void)QT_TR_NOOP(kShortcutDescActionFullscreen);
    (void)QT_TR_NOOP(kShortcutDescActionShowWindowsConsole);
    (void)QT_TR_NOOP(kShortcutDescActionClearDiskCache);
    (void)QT_TR_NOOP(kShortcutDescActionClearPlaybackCache);
    (void)QT_TR_NOOP(kShortcutDescActionClearNodeCache);
    (void)QT_TR_NOOP(kShortcutDescActionClearPluginsLoadCache);
    (void)QT_TR_NOOP(kShortcutDescActionClearAllCaches);
    (void)QT_TR_NOOP(kShortcutDescActionFreeGpuMemory);
    (void)QT_TR_NOOP(kShortcutDescActionShowAbout);
    (void)QT_TR_NOOP(kShortcutDescActionRenderSelected);
    (void)QT_TR_NOOP(kShortcutDescActionEnableRenderStats);
    (void)QT_TR_NOOP(kShortcutDescActionRenderAll);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput1);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput2);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput3);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput4);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput5);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput6);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput7);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput8);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput9);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput10);
    (void)QT_TR_NOOP(kShortcutDescActionShowPaneFullScreen);
    (void)QT_TR_NOOP(kShortcutDescActionImportLayout);
    (void)QT_TR_NOOP(kShortcutDescActionExportLayout);
    (void)QT_TR_NOOP(kShortcutDescActionDefaultLayout);
    (void)QT_TR_NOOP(kShortcutDescActionNextTab);
    (void)QT_TR_NOOP(kShortcutDescActionPrevTab);
    (void)QT_TR_NOOP(kShortcutDescActionCloseTab);
    (void)QT_TR_NOOP(kShortcutDescActionLuminance);
    (void)QT_TR_NOOP(kShortcutDescActionRed);
    (void)QT_TR_NOOP(kShortcutDescActionGreen);
    (void)QT_TR_NOOP(kShortcutDescActionBlue);
    (void)QT_TR_NOOP(kShortcutDescActionAlpha);
    (void)QT_TR_NOOP(kShortcutDescActionLuminanceA);
    (void)QT_TR_NOOP(kShortcutDescActionMatteOverlay);
    (void)QT_TR_NOOP(kShortcutDescActionRedA);
    (void)QT_TR_NOOP(kShortcutDescActionGreenA);
    (void)QT_TR_NOOP(kShortcutDescActionBlueA);
    (void)QT_TR_NOOP(kShortcutDescActionAlphaA);
    (void)QT_TR_NOOP(kShortcutDescActionFitViewer);
    (void)QT_TR_NOOP(kShortcutDescActionClipEnabled);
    (void)QT_TR_NOOP(kShortcutDescActionRefresh);
    (void)QT_TR_NOOP(kShortcutDescActionRefreshWithStats);
    (void)QT_TR_NOOP(kShortcutDescActionROIEnabled);
    (void)QT_TR_NOOP(kShortcutDescActionNewROI);
    (void)QT_TR_NOOP(kShortcutDescActionPauseViewer);
    (void)QT_TR_NOOP(kShortcutDescActionPauseViewerInputA);
    (void)QT_TR_NOOP(kShortcutDescActionProxyEnabled);
    (void)QT_TR_NOOP(kShortcutDescActionProxyLevel2);
    (void)QT_TR_NOOP(kShortcutDescActionProxyLevel4);
    (void)QT_TR_NOOP(kShortcutDescActionProxyLevel8);
    (void)QT_TR_NOOP(kShortcutDescActionProxyLevel16);
    (void)QT_TR_NOOP(kShortcutDescActionProxyLevel32);
    (void)QT_TR_NOOP(kShortcutDescActionZoomLevel100);
    (void)QT_TR_NOOP(kShortcutDescActionZoomIn);
    (void)QT_TR_NOOP(kShortcutDescActionZoomOut);
    (void)QT_TR_NOOP(kShortcutDescActionHideOverlays);
    (void)QT_TR_NOOP(kShortcutDescActionHidePlayer);
    (void)QT_TR_NOOP(kShortcutDescActionHideTimeline);
    (void)QT_TR_NOOP(kShortcutDescActionHideLeft);
    (void)QT_TR_NOOP(kShortcutDescActionHideRight);
    (void)QT_TR_NOOP(kShortcutDescActionHideTop);
    (void)QT_TR_NOOP(kShortcutDescActionHideInfobar);
    (void)QT_TR_NOOP(kShortcutDescActionHideAll);
    (void)QT_TR_NOOP(kShortcutDescActionShowAll);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerPrevious);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerNext);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerBackward);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerForward);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerStop);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerPrevIncr);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerNextIncr);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerPrevKF);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerNextKF);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerFirst);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerLast);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerPlaybackIn);
    (void)QT_TR_NOOP(kShortcutDescActionPlayerPlaybackOut);
    (void)QT_TR_NOOP(kShortcutDescActionGraphRearrangeNodes);
    (void)QT_TR_NOOP(kShortcutDescActionGraphRemoveNodes);
    (void)QT_TR_NOOP(kShortcutDescActionGraphShowLinkLines);
    (void)QT_TR_NOOP(kShortcutDescActionGraphNavigateUpstream);
    (void)QT_TR_NOOP(kShortcutDescActionGraphNavigateDownstram);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSelectUp);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSelectDown);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSelectAll);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSelectAllVisible);
    (void)QT_TR_NOOP(kShortcutDescActionGraphHideInputs);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSwitchInputs);
    (void)QT_TR_NOOP(kShortcutDescActionGraphCopy);
    (void)QT_TR_NOOP(kShortcutDescActionGraphPaste);
    (void)QT_TR_NOOP(kShortcutDescActionGraphCreateLinkedNode);
    (void)QT_TR_NOOP(kShortcutDescActionGraphUnlinkNode);
    (void)QT_TR_NOOP(kShortcutDescActionGraphCut);
    (void)QT_TR_NOOP(kShortcutDescActionGraphDuplicate);
    (void)QT_TR_NOOP(kShortcutDescActionGraphDisableNodes);
    (void)QT_TR_NOOP(kShortcutDescActionGraphToggleAutoPreview);
    (void)QT_TR_NOOP(kShortcutDescActionGraphToggleAutoTurbo);
    (void)QT_TR_NOOP(kShortcutDescActionGraphTogglePreview);
    (void)QT_TR_NOOP(kShortcutDescActionGraphForcePreview);
    (void)QT_TR_NOOP(kShortcutDescActionGraphShowCacheSize);
    (void)QT_TR_NOOP(kShortcutDescActionGraphFrameNodes);
    (void)QT_TR_NOOP(kShortcutDescActionGraphFindNode);
    (void)QT_TR_NOOP(kShortcutDescActionGraphRenameNode);
    (void)QT_TR_NOOP(kShortcutDescActionGraphExtractNode);
    (void)QT_TR_NOOP(kShortcutDescActionGraphMakeGroup);
    (void)QT_TR_NOOP(kShortcutDescActionGraphExpandGroup);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorRemoveKeys);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorConstant);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorLinear);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorSmooth);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorCatmullrom);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorCubic);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorHorizontal);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorBreak);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorFree);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorSelectAll);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorCenter);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorCopy);
    (void)QT_TR_NOOP(kShortcutDescActionCurveEditorPaste);
    (void)QT_TR_NOOP(kShortcutDescActionDopeSheetEditorDeleteKeys);
    (void)QT_TR_NOOP(kShortcutDescActionDopeSheetEditorFrameSelection);
    (void)QT_TR_NOOP(kShortcutDescActionDopeSheetEditorSelectAllKeyframes);
    (void)QT_TR_NOOP(kShortcutDescActionDopeSheetEditorRenameNode);
    (void)QT_TR_NOOP(kShortcutDescActionDopeSheetEditorCopySelectedKeyframes);
    (void)QT_TR_NOOP(kShortcutDescActionDopeSheetEditorPasteKeyframes);
    (void)QT_TR_NOOP(kShortcutDescActionScriptEditorPrevScript);
    (void)QT_TR_NOOP(kShortcutDescActionScriptEditorNextScript);
    (void)QT_TR_NOOP(kShortcutDescActionScriptEditorClearHistory);
    (void)QT_TR_NOOP(kShortcutDescActionScriptExecScript);
    (void)QT_TR_NOOP(kShortcutDescActionScriptClearOutput);
    (void)QT_TR_NOOP(kShortcutDescActionScriptShowOutput);
    (void)QT_TR_NOOP(kShortcutDescActionViewer3DFrameSelection);
    (void)QT_TR_NOOP(kShortcutDescActionViewer3DFrameAll);

    QString actionIDStr = QString::fromUtf8( actionID.c_str() );
    std::list<QKeySequence> seq = getKeybind(_group, actionIDStr);

    if ( seq.empty() ) {
        seq.push_back( QKeySequence() );
    }
    _shortcuts.push_back( std::make_pair( actionIDStr, seq.front() ) );
    assert ( !_group.isEmpty() && !actionIDStr.isEmpty() );
    if (setShortcutOnAction) {
        setShortcut( seq.front() );
    }
    appPTR->addShortcutAction(_group, actionIDStr, this);
    setShortcutContext(Qt::WindowShortcut);
    setText( tr( actionDescription.c_str() ) );
}

ActionWithShortcut::ActionWithShortcut(const std::string & group,
                                       const std::list<std::string> & actionIDs,
                                       const std::string & actionDescription,
                                       QObject* parent,
                                       bool setShortcutOnAction)
    : QAction(parent)
    , _group( QString::fromUtf8( group.c_str() ) )
    , _shortcuts()
{
    QKeySequence seq0;

    for (std::list<std::string>::const_iterator it = actionIDs.begin(); it != actionIDs.end(); ++it) {
        QString actionIDStr = QString::fromUtf8( it->c_str() );
        std::list<QKeySequence> seq = getKeybind(_group, actionIDStr);
        if ( seq.empty() ) {
            seq.push_back( QKeySequence() );
        }
        _shortcuts.push_back( std::make_pair( actionIDStr, seq.front() ) );
        if ( it == actionIDs.begin() ) {
            seq0 = seq.front();
        }
        appPTR->addShortcutAction(_group, actionIDStr, this);
    }
    assert ( !_group.isEmpty() && !actionIDs.empty() );
    if (setShortcutOnAction) {
        setShortcut(seq0);
    }

    setShortcutContext(Qt::WindowShortcut);
    setText( tr( actionDescription.c_str() ) );
}

ActionWithShortcut::~ActionWithShortcut()
{
    assert ( !_group.isEmpty() && !_shortcuts.empty() );
    for (std::size_t i = 0; i < _shortcuts.size(); ++i) {
        appPTR->removeShortcutAction(_group, _shortcuts[i].first, this);
    }
}

void
ActionWithShortcut::setShortcutWrapper(const QString& actionID,
                                       const QKeySequence& shortcut)
{
    for (std::size_t i = 0; i < _shortcuts.size(); ++i) {
        if (_shortcuts[i].first == actionID) {
            _shortcuts[i].second = shortcut;
        }
    }
    setShortcut(shortcut);
}

ToolTipActionShortcut::ToolTipActionShortcut(const std::string & group,
                                             const std::string & actionID,
                                             const std::string & toolip,
                                             QWidget* parent)
    : ActionWithShortcut(group, actionID, std::string(), parent, false)
    , _widget(parent)
    , _originalToolTip( QString::fromUtf8( toolip.c_str() ) )
    , _tooltipSetInternally(false)
{
    assert(parent);
    setToolTipFromOriginalToolTip();
    _widget->installEventFilter(this);
}

ToolTipActionShortcut::ToolTipActionShortcut(const std::string & group,
                                             const std::list<std::string> & actionIDs,
                                             const std::string & toolip,
                                             QWidget* parent)
    : ActionWithShortcut(group, actionIDs, std::string(), parent, false)
    , _widget(parent)
    , _originalToolTip( QString::fromUtf8( toolip.c_str() ) )
    , _tooltipSetInternally(false)
{
    assert(parent);
    setToolTipFromOriginalToolTip();
    _widget->installEventFilter(this);
}

void
ToolTipActionShortcut::setToolTipFromOriginalToolTip()
{
    QString finalToolTip = _originalToolTip;

    for (std::size_t i = 0; i < _shortcuts.size(); ++i) {
        finalToolTip = finalToolTip.arg( _shortcuts[i].second.toString(QKeySequence::NativeText) );
    }

    _tooltipSetInternally = true;
    _widget->setToolTip(finalToolTip);
    _tooltipSetInternally = false;
}

bool
ToolTipActionShortcut::eventFilter(QObject* watched,
                                   QEvent* event)
{
    if (watched != _widget) {
        return false;
    }
    if (event->type() == QEvent::ToolTipChange) {
        if (_tooltipSetInternally) {
            return false;
        }
        _originalToolTip = _widget->toolTip();
        setToolTipFromOriginalToolTip();
    }

    return false;
}

void
ToolTipActionShortcut::setShortcutWrapper(const QString& actionID,
                                          const QKeySequence& shortcut)
{
    for (std::size_t i = 0; i < _shortcuts.size(); ++i) {
        if (_shortcuts[i].first == actionID) {
            _shortcuts[i].second = shortcut;
        }
    }
    setToolTipFromOriginalToolTip();
}

static const Qt::KeyboardModifiers kMouseBindingModifierMask = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier | kShortcutModifierSpace;

bool
MouseAction::matches(Qt::KeyboardModifiers mods,
                     Qt::MouseButton button) const
{
    mods &= kMouseBindingModifierMask;
    std::list<Qt::MouseButton>::const_iterator bit = buttons.begin();
    for (std::list<Qt::KeyboardModifiers>::const_iterator it = modifiers.begin(); it != modifiers.end(); ++it) {
        Qt::MouseButton b = (bit != buttons.end()) ? *bit : Qt::NoButton;
        if (bit != buttons.end()) {
            ++bit;
        }
        if ((*it & kMouseBindingModifierMask) != mods) {
            continue;
        }
        if ((gesture == eMouseGestureWheel) || ((b != Qt::NoButton) && (b == button))) {
            return true;
        }
    }

    return false;
}

std::vector<MouseBinding>
MouseAction::getBindings() const
{
    std::vector<MouseBinding> ret;
    std::list<Qt::MouseButton>::const_iterator bit = buttons.begin();
    for (std::list<Qt::KeyboardModifiers>::const_iterator it = modifiers.begin(); it != modifiers.end(); ++it) {
        MouseBinding b;
        b.modifiers = *it;
        b.button = (bit != buttons.end()) ? *bit : Qt::NoButton;
        if (bit != buttons.end()) {
            ++bit;
        }
        ret.push_back(b);
    }

    return ret;
}

void
MouseAction::setBindings(const std::vector<MouseBinding>& bindings)
{
    modifiers.clear();
    buttons.clear();
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        modifiers.push_back(bindings[i].modifiers);
        buttons.push_back((gesture == eMouseGestureWheel) ? Qt::NoButton : bindings[i].button);
    }
}

bool
mouseGesturesOverlap(MouseGestureEnum a,
                     MouseGestureEnum b)
{
    if ((a == eMouseGestureWheel) || (b == eMouseGestureWheel)) {
        return a == b;
    }

    return (a == eMouseGestureAny) || (b == eMouseGestureAny) || (a == b);
}

Qt::KeyboardModifiers
mouseBindingModifiers(Qt::KeyboardModifiers modifiers,
                      bool spaceHeld)
{
    Qt::KeyboardModifiers ret = modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
    if (spaceHeld) {
        ret |= kShortcutModifierSpace;
    }

    return ret;
}

QString
mouseBindingToString(const MouseBinding& binding,
                     bool wheel)
{
    QString ret;
    if (binding.modifiers & kShortcutModifierSpace) {
        ret.append(QCoreApplication::translate("ShortCutEditor", "Space"));
        ret.append(QLatin1Char('+'));
    }
    Qt::KeyboardModifiers keyMods = binding.modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
    ret.append(makeKeySequence(keyMods, (Qt::Key)0).toString(QKeySequence::NativeText));
    if (wheel) {
        ret.append(QCoreApplication::translate("ShortCutEditor", "Wheel"));

        return ret;
    }
    switch (binding.button) {
    case Qt::LeftButton:
        ret.append(QCoreApplication::translate("ShortCutEditor", "LeftButton"));
        break;
    case Qt::MiddleButton:
        ret.append(QCoreApplication::translate("ShortCutEditor", "MiddleButton"));
        break;
    case Qt::RightButton:
        ret.append(QCoreApplication::translate("ShortCutEditor", "RightButton"));
        break;
    default:
        break;
    }

    return ret;
}

static void
pushBinding(std::vector<MouseBinding>* bindings,
            Qt::KeyboardModifiers modifiers,
            Qt::MouseButton button)
{
    MouseBinding b;
    b.modifiers = modifiers;
    b.button = button;
    bindings->push_back(b);
}

bool
getViewer3DNavigationPresetBindings(int preset,
                                    const QString& actionID,
                                    std::vector<MouseBinding>* bindings)
{
    // The values of Settings::Viewer3DNavigationEnum; this file does not depend on Engine/Settings.h.
    enum { eNuke = 0,
           eMaya,
           eBlender,
           eHoudini };
    if ((preset < eNuke) || (preset > eHoudini)) {
        return false;
    }
    bindings->clear();
    const Qt::KeyboardModifiers none = Qt::NoModifier;
    const Qt::KeyboardModifiers space = kShortcutModifierSpace;
    const std::string id = actionID.toStdString();

    // Selection is the same in every preset.
    if (id == kShortcutIDMouseViewer3DSelect) {
        pushBinding(bindings, none, Qt::LeftButton);
    } else if (id == kShortcutIDMouseViewer3DAddToSelection) {
        pushBinding(bindings, Qt::ShiftModifier, Qt::LeftButton);
    } else if (id == kShortcutIDMouseViewer3DToggleSelection) {
        pushBinding(bindings, Qt::ControlModifier, Qt::LeftButton);
    } else if (id == kShortcutIDMouseViewer3DMarquee) {
        pushBinding(bindings, none, Qt::LeftButton);
    } else if (id == kShortcutIDMouseViewer3DDollyWheel) {
        pushBinding(bindings, none, Qt::NoButton);
    } else if (id == kShortcutIDMouseViewer3DTumble) {
        switch (preset) {
        case eNuke:
            pushBinding(bindings, Qt::ControlModifier, Qt::LeftButton);
            break;
        case eMaya:
            pushBinding(bindings, Qt::AltModifier, Qt::LeftButton);
            break;
        case eBlender:
            pushBinding(bindings, none, Qt::MiddleButton);
            break;
        case eHoudini:
            pushBinding(bindings, space, Qt::LeftButton);
            pushBinding(bindings, Qt::AltModifier, Qt::LeftButton);
            break;
        }
    } else if (id == kShortcutIDMouseViewer3DPan) {
        switch (preset) {
        case eNuke:
            pushBinding(bindings, Qt::AltModifier, Qt::LeftButton);
            pushBinding(bindings, none, Qt::MiddleButton);
            break;
        case eMaya:
            pushBinding(bindings, Qt::AltModifier, Qt::MiddleButton);
            break;
        case eBlender:
            pushBinding(bindings, Qt::ShiftModifier, Qt::MiddleButton);
            break;
        case eHoudini:
            pushBinding(bindings, space, Qt::MiddleButton);
            pushBinding(bindings, Qt::AltModifier, Qt::MiddleButton);
            break;
        }
    } else if (id == kShortcutIDMouseViewer3DDolly) {
        switch (preset) {
        case eNuke:
            pushBinding(bindings, Qt::AltModifier, Qt::MiddleButton);
            break;
        case eMaya:
            pushBinding(bindings, Qt::AltModifier, Qt::RightButton);
            break;
        case eBlender:
            pushBinding(bindings, Qt::ControlModifier, Qt::MiddleButton);
            break;
        case eHoudini:
            pushBinding(bindings, space, Qt::RightButton);
            pushBinding(bindings, Qt::AltModifier, Qt::RightButton);
            break;
        }
    } else {
        return false;
    }

    return true;
}

void
writeShortcutSettings(QSettings& settings,
                      const QString& actionID,
                      const BoundAction& action)
{
    const MouseAction* mAction = dynamic_cast<const MouseAction*>(&action);
    const KeyBoundAction* kAction = dynamic_cast<const KeyBoundAction*>(&action);

    settings.setValue(actionID + QString::fromUtf8("_nbShortcuts"), QVariant((int)action.modifiers.size()));

    int c = 0;
    for (std::list<Qt::KeyboardModifiers>::const_iterator it = action.modifiers.begin(); it != action.modifiers.end(); ++it, ++c) {
        settings.setValue(actionID + QString::fromUtf8("_Modifiers") + QString::number(c), (int)*it);
    }

    if (mAction) {
        c = 0;
        for (std::list<Qt::MouseButton>::const_iterator it = mAction->buttons.begin(); it != mAction->buttons.end(); ++it, ++c) {
            settings.setValue(actionID + QString::fromUtf8("_Button") + QString::number(c), (int)*it);
        }
    } else if (kAction) {
        assert(action.modifiers.size() == kAction->currentShortcut.size());
        c = 0;
        for (std::list<Qt::Key>::const_iterator it = kAction->currentShortcut.begin(); it != kAction->currentShortcut.end(); ++it, ++c) {
            settings.setValue(actionID + QString::fromUtf8("_Symbol") + QString::number(c), (int)*it);
        }
    }
}

bool
readShortcutSettings(QSettings& settings,
                     const QString& actionID,
                     BoundAction* action)
{
    MouseAction* mAction = dynamic_cast<MouseAction*>(action);
    KeyBoundAction* kAction = dynamic_cast<KeyBoundAction*>(action);
    const QString nbKey = actionID + QString::fromUtf8("_nbShortcuts");

    if (mAction) {
        if (!settings.contains(nbKey)) {
            return false;
        }
        // An empty list is a binding the user cleared.
        int nbShortcuts = settings.value(nbKey).toInt();
        std::vector<MouseBinding> bindings;
        for (int i = 0; i < nbShortcuts; ++i) {
            QString idm = actionID + QString::fromUtf8("_Modifiers") + QString::number(i);
            QString idb = actionID + QString::fromUtf8("_Button") + QString::number(i);
            if (!settings.contains(idm) || !settings.contains(idb)) {
                continue;
            }
            MouseBinding b;
            b.modifiers = Qt::KeyboardModifiers(settings.value(idm).toInt());
            b.button = (Qt::MouseButton)settings.value(idb).toInt();
            bindings.push_back(b);
        }
        mAction->setBindings(bindings);

        return false;
    }

    int nbShortcuts = 1;
    bool nbShortcutsSet = false;
    if (settings.contains(nbKey)) {
        nbShortcuts = settings.value(nbKey).toInt();
        nbShortcutsSet = true;
    }

    bool hasNonNullKeybind = false;

    if (!nbShortcutsSet) {
        if (kAction) {
            if (settings.contains(actionID + QString::fromUtf8("_Symbol"))) {
                Qt::Key key = (Qt::Key)settings.value(actionID + QString::fromUtf8("_Symbol")).toInt();
                if (key != (Qt::Key)0) {
                    kAction->currentShortcut.clear();
                    kAction->currentShortcut.push_back(key);
                    hasNonNullKeybind = true;
                }
            }
        }
        if (hasNonNullKeybind && settings.contains(actionID + QString::fromUtf8("_Modifiers"))) {
            action->modifiers.clear();
            action->modifiers.push_back(Qt::KeyboardModifiers(settings.value(actionID + QString::fromUtf8("_Modifiers")).toInt()));
        }
    } else {
        for (int i = 0; i < nbShortcuts; ++i) {
            QString idm = actionID + QString::fromUtf8("_Modifiers") + QString::number(i);

            if (kAction) {
                QString ids = actionID + QString::fromUtf8("_Symbol") + QString::number(i);
                if (settings.contains(ids)) {
                    Qt::Key key = (Qt::Key)settings.value(ids).toInt();
                    if (key != (Qt::Key)0) {
                        if (i == 0) {
                            kAction->currentShortcut.clear();
                        }
                        kAction->currentShortcut.push_back(key);
                        hasNonNullKeybind = true;
                    } else {
                        continue;
                    }
                }
            }
            if (settings.contains(idm)) {
                if (i == 0) {
                    action->modifiers.clear();
                }
                action->modifiers.push_back(Qt::KeyboardModifiers(settings.value(idm).toInt()));
            }
        }
    }

    return hasNonNullKeybind;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ActionShortcuts.cpp"
