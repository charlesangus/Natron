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

#include "Plugin.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <locale>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>

#include <QDateTime>
#include <QFileInfo>
#include <QMutex>

// clang-format off
CLANG_DIAG_OFF(unknown-pragmas)
CLANG_DIAG_OFF(tautological-undefined-compare) // appeared in clang 3.5
#include <ofxhPluginCache.h>
#include <ofxhPluginAPICache.h>
#include <ofxhImageEffect.h>
#include <ofxhImageEffectAPI.h>
CLANG_DIAG_ON(tautological-undefined-compare)
CLANG_DIAG_ON(unknown-pragmas)
// clang-format on

#include "Global/GitVersion.h"

#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

namespace {
// Native plug-ins are compiled into the running executable, and GIT_COMMIT is a committed
// constant that a rebuild of changed code keeps, so the executable itself is stamped. Not its
// path: an AppImage is mounted at a new one every launch. An executable that cannot be stamped
// gets the session salt, since nothing then tells its code apart from another build's.
U64
runningExecutableStamp()
{
    static const U64 stamp = []() {
        ContentHasher h;
        struct statx st;
        if (statx(AT_FDCWD, "/proc/self/exe", 0, STATX_MTIME | STATX_SIZE, &st) != 0) {
            h.appendBool(false);
            h.appendU64(ContentHasher::sessionSalt());
        } else {
            h.appendBool(true);
            h.appendI64((std::int64_t)st.stx_mtime.tv_sec);
            h.appendI64((std::int64_t)st.stx_mtime.tv_nsec);
            h.appendU64(st.stx_size);
        }

        return h.finalize();
    }();

    return stamp;
}

// Plug-ins whose output depends on state that no hash covers. Their results are reused within a
// session but never served from the disk cache in a later one, where that state may differ.
struct VolatilePlugin {
    const char* id;
    const char* reason;
};

const VolatilePlugin kVolatilePlugins[] = {
    { "net.fxarena.openfx.Text", "renders with the system fonts found through fontconfig" },
    { "net.fxarena.openfx.RichText", "renders with the system fonts found through fontconfig" },
    { "net.fxarena.openfx.MagickText", "renders with the fonts ImageMagick finds on the system" },
    { "net.fxarena.openfx.Polaroid", "draws its caption with a system font" },
    { "fr.inria.openfx.OIIOText", "renders with the fonts OpenImageIO finds on the system" },
    { "net.fxarena.openfx.HaldCLUT", "reads, and can download, preset LUTs under ~/.clut" },
};

bool
isVolatilePlugin(const std::string& id)
{
    for (const VolatilePlugin& plugin : kVolatilePlugins) {
        if (id == plugin.id) {
            return true;
        }
    }

    return false;
}
} // anonymous namespace

Plugin::~Plugin()
{
    delete _lock;
    delete _binary;
}

void
Plugin::setPluginID(const QString & id)
{
    _id = id;
}

const QString &
Plugin::getPluginID() const
{
    return _id;
}

bool
Plugin::isReader() const
{
    return _isReader;
}

bool
Plugin::isWriter() const
{
    return _isWriter;
}

QString
Plugin::getPluginShortcutGroup() const
{
    return getPluginLabel() + QLatin1String(" Viewer Interface");
}

void
Plugin::setPluginLabel(const QString & label)
{
    _label = label;
}

const QString &
Plugin::getPluginLabel() const
{
    return _label;
}

const QString
Plugin::getLabelVersionMajorMinorEncoded() const
{
    return getLabelWithoutSuffix() + QLatin1Char(' ') + QString::number(_majorVersion) + QLatin1Char('.') + QString::number(_minorVersion);
}

QString
Plugin::makeLabelWithoutSuffix(const QString& label)
{
    if ( label.startsWith( QString::fromUtf8("Read") ) || label.startsWith( QString::fromUtf8("Write") ) ) {
        return label;
    } else if ( label.endsWith( QString::fromUtf8("OFX") ) ) {
        return label.mid(0, label.size() - 3);
    } else if ( label.endsWith( QString::fromUtf8("CImg") ) ) {
        return label.mid(0, label.size() - 4);
    } else if ( label.endsWith( QString::fromUtf8("OIIO") ) ) {
        return label.mid(0, label.size() - 4);
    }

    return label;
}

const QString&
Plugin::getLabelWithoutSuffix() const
{
    return _labelWithoutSuffix;
}

void
Plugin::setLabelWithoutSuffix(const QString& label)
{
    _labelWithoutSuffix = label;
}

const QString
Plugin::getLabelVersionMajorEncoded() const
{
    return getLabelWithoutSuffix() + QLatin1Char(' ') + QString::number(_majorVersion);
}

QString
Plugin::generateUserFriendlyPluginID() const
{
    QString grouping = _grouping.size() > 0 ? _grouping[0] : QString();

    return getLabelWithoutSuffix() + QString::fromUtf8("  [") + grouping + QLatin1Char(']');
}

QString
Plugin::generateUserFriendlyPluginIDMajorEncoded() const
{
    QString grouping = _grouping.size() > 0 ? _grouping[0] : QString();

    return getLabelVersionMajorEncoded() + QString::fromUtf8("  [") + grouping + QLatin1Char(']');
}

void
Plugin::setToolsetScript(bool isToolset)
{
    _toolSetScript = isToolset;
}

bool
Plugin::getToolsetScript() const
{
    return _toolSetScript;
}

const QString&
Plugin::getIconFilePath() const
{
    return _iconFilePath;
}

void
Plugin::setIconFilePath(const QString& filePath)
{
    _iconFilePath = filePath;
}

const QStringList&
Plugin::getGroupIconFilePath() const
{
    return _groupIconFilePath;
}

const QStringList&
Plugin::getGrouping() const
{
    return _grouping;
}

QRecursiveMutex*
Plugin::getPluginLock() const
{
    return _lock;
}

LibraryBinary*
Plugin::getLibraryBinary() const
{
    return _binary;
}

int
Plugin::getMajorVersion() const
{
    return _majorVersion;
}

int
Plugin::getMinorVersion() const
{
    return _minorVersion;
}

void
Plugin::setHasShortcut(bool has) const
{
    _hasShortcutSet = has;
}

bool
Plugin::getHasShortcut() const
{
    return _hasShortcutSet;
}

void
Plugin::setPythonModule(const QString& module)
{
    _pythonModule = module;
}

const QString&
Plugin::getPythonModule() const
{
    return _pythonModule; // < does not end with .py
}

void
Plugin::getPythonModuleNameAndPath(QString* moduleName,
                                   QString* modulePath) const
{
    int foundLastSlash = _pythonModule.lastIndexOf( QLatin1Char('/') );

    if (foundLastSlash != -1) {
        *modulePath = _pythonModule.mid(0, foundLastSlash + 1);
        *moduleName = _pythonModule.mid(foundLastSlash + 1);
    } else {
        *moduleName = _pythonModule;
    }
}

void
Plugin::setOfxPlugin(OFX::Host::ImageEffect::ImageEffectPlugin* p)
{
    _ofxPlugin = p;
}

OFX::Host::ImageEffect::ImageEffectPlugin*
Plugin::getOfxPlugin() const
{
    return _ofxPlugin;
}

U64
Plugin::ofxBinaryStamp(const std::string& binaryPath)
{
    ContentHasher h;
    h.appendString("ofx");
    // The file name and not the path: plug-ins bundled in an AppImage sit under its mount point,
    // which differs every launch, and a path in the stamp would miss the disk cache every session.
    // The plug-in ID and version, hashed beside this stamp, tell bundles of one name apart.
    QFileInfo info(QString::fromUtf8(binaryPath.c_str()));
    h.appendString(info.fileName().toStdString());
    h.appendBool(info.exists());
    h.appendI64(info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0);
    h.appendI64(info.exists() ? info.size() : 0);

    return h.finalize();
}

U64
Plugin::getBinaryContentStamp() const
{
    U64 stamp = _binaryContentStamp.load(std::memory_order_acquire);
    if (stamp != 0) {
        return stamp;
    }

    ContentHasher h;
    OFX::Host::PluginBinary* binary = _ofxPlugin ? _ofxPlugin->getBinary() : 0;
    if (binary) {
        h.appendU64(ofxBinaryStamp(binary->getFilePath()));
    } else {
        h.appendString("native");
        h.appendString(NATRON_VERSION_STRING);
        h.appendString(GIT_COMMIT);
        h.appendU64(runningExecutableStamp());
    }
    const bool isVolatile = isVolatilePlugin(_id.toStdString());
    h.appendBool(isVolatile);
    if (isVolatile) {
        h.appendU64(ContentHasher::sessionSalt());
    }
    stamp = h.finalize();
    _binaryContentStamp.store(stamp, std::memory_order_release);

    return stamp;
}

OFX::Host::ImageEffect::Descriptor*
Plugin::getOfxDesc(ContextEnum* ctx) const
{
    *ctx = _ofxContext;

    return _ofxDescriptor;
}

bool
Plugin::getIsUserCreatable() const
{
    return !_isInternalOnly && _activated && !_isDeprecated;
}

void
Plugin::setOfxDesc(OFX::Host::ImageEffect::Descriptor* desc,
                   ContextEnum ctx)
{
    assert(ctx != eContextNone);
    _ofxDescriptor = desc;
    _ofxContext = ctx;
}

bool
Plugin::isRenderScaleEnabled() const
{
    return _renderScaleEnabled;
}

void
Plugin::setRenderScaleEnabled(bool b)
{
    _renderScaleEnabled = b;
}

bool
Plugin::isMultiThreadingEnabled() const
{
    return _multiThreadingEnabled;
}

void
Plugin::setMultiThreadingEnabled(bool b)
{
    _multiThreadingEnabled = b;
}

bool
Plugin::isOpenGLEnabled() const
{
    return _openglActivated;
}

void
Plugin::setOpenGLEnabled(bool b)
{
    _openglActivated = b;
}

void
Plugin::setOpenGLRenderSupport(PluginOpenGLRenderSupport support)
{
    _openglRenderSupport = support;
}

PluginOpenGLRenderSupport
Plugin::getPluginOpenGLRenderSupport() const
{
    return _openglRenderSupport;
}

bool
Plugin::isActivated() const
{
    return _activated;
}

void
Plugin::setActivated(bool b)
{
    _activated = b;
}

void
PluginGroupNode::tryAddChild(const PluginGroupNodePtr& plugin)
{
    for (std::list<PluginGroupNodePtr>::iterator it = _children.begin(); it != _children.end(); ++it) {
        if (*it == plugin) {
            return;
        }
    }
    _children.push_back(plugin);
}

void
PluginGroupNode::tryRemoveChild(PluginGroupNode* plugin)
{
    for (std::list<PluginGroupNodePtr>::iterator it = _children.begin(); it != _children.end(); ++it) {
        if (it->get() == plugin) {
            _children.erase(it);

            return;
        }
    }
}

NATRON_NAMESPACE_EXIT
