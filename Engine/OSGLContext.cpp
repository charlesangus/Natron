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

#include "OSGLContext.h"

#include <limits>
#include <stdexcept>
#include <sstream> // stringstream

#include <QDebug>
#include <QMutex>
#include <QWaitCondition>

#include "Engine/AppManager.h"

#ifdef __NATRON_LINUX__
#include "Engine/OSGLContext_wayland.h"
#include "Engine/OSGLContext_x11.h"
#endif

#include "Engine/AbortableRenderInfo.h"
#include "Engine/GPUContextPool.h"
#include "Engine/GpuDeviceList.h"
#include "Global/FloatingPointExceptions.h"

NATRON_NAMESPACE_ENTER

FramebufferConfig::FramebufferConfig()
    : redBits(8)
    , greenBits(8)
    , blueBits(8)
    , alphaBits(8)
    , depthBits(24)
    , stencilBits(8)
    , accumRedBits(0)
    , accumGreenBits(0)
    , accumBlueBits(0)
    , accumAlphaBits(0)
    , auxBuffers(0)
    , stereo(GL_FALSE)
    , samples(0)
    , sRGB(GL_FALSE)
    , doublebuffer(GL_FALSE)
    , handle(0)
{
}

const FramebufferConfig&
OSGLContext::chooseFBConfig(const FramebufferConfig& desired,
                            const std::vector<FramebufferConfig>& alternatives,
                            int count)
{
    if ( alternatives.empty() || ( count > (int)alternatives.size() ) ) {
        throw std::logic_error("alternatives empty");
    }
    unsigned int missing, leastMissing = std::numeric_limits<unsigned int>::max();
    unsigned int colorDiff, leastColorDiff = std::numeric_limits<unsigned int>::max();
    unsigned int extraDiff, leastExtraDiff = std::numeric_limits<unsigned int>::max();
    int closest = -1;

    for (int i = 0; i < count; ++i) {
        const FramebufferConfig& current = alternatives[i];

        if ( (desired.stereo > 0) && (current.stereo == 0) ) {
            // Stereo is a hard constraint
            continue;
        }

        if (desired.doublebuffer != current.doublebuffer) {
            // Double buffering is a hard constraint
            continue;
        }

        // Count number of missing buffers
        {
            missing = 0;

            if ( (desired.alphaBits > 0) && (current.alphaBits == 0) ) {
                missing++;
            }

            if ( (desired.depthBits > 0) && (current.depthBits == 0) ) {
                missing++;
            }

            if ( (desired.stencilBits > 0) && (current.stencilBits == 0) ) {
                missing++;
            }

            if ( (desired.auxBuffers > 0) &&
                 ( current.auxBuffers < desired.auxBuffers) ) {
                missing += desired.auxBuffers - current.auxBuffers;
            }

            if ( (desired.samples > 0) && (current.samples == 0) ) {
                // Technically, several multisampling buffers could be
                // involved, but that's a lower level implementation detail and
                // not important to us here, so we count them as one
                missing++;
            }
        }

        // These polynomials make many small channel size differences matter
        // less than one large channel size difference

        // Calculate color channel size difference value
        {
            colorDiff = 0;

            if (desired.redBits != FramebufferConfig::ATTR_DONT_CARE) {
                colorDiff += (desired.redBits - current.redBits) *
                             (desired.redBits - current.redBits);
            }

            if (desired.greenBits != FramebufferConfig::ATTR_DONT_CARE) {
                colorDiff += (desired.greenBits - current.greenBits) *
                             (desired.greenBits - current.greenBits);
            }

            if (desired.blueBits != FramebufferConfig::ATTR_DONT_CARE) {
                colorDiff += (desired.blueBits - current.blueBits) *
                             (desired.blueBits - current.blueBits);
            }
        }

        // Calculate non-color channel size difference value
        {
            extraDiff = 0;

            if (desired.alphaBits != FramebufferConfig::ATTR_DONT_CARE) {
                extraDiff += (desired.alphaBits - current.alphaBits) *
                             (desired.alphaBits - current.alphaBits);
            }

            if (desired.depthBits != FramebufferConfig::ATTR_DONT_CARE) {
                extraDiff += (desired.depthBits - current.depthBits) *
                             (desired.depthBits - current.depthBits);
            }

            if (desired.stencilBits != FramebufferConfig::ATTR_DONT_CARE) {
                extraDiff += (desired.stencilBits - current.stencilBits) *
                             (desired.stencilBits - current.stencilBits);
            }

            if (desired.accumRedBits != FramebufferConfig::ATTR_DONT_CARE) {
                extraDiff += (desired.accumRedBits - current.accumRedBits) *
                             (desired.accumRedBits - current.accumRedBits);
            }

            if (desired.accumGreenBits != FramebufferConfig::ATTR_DONT_CARE) {
                extraDiff += (desired.accumGreenBits - current.accumGreenBits) *
                             (desired.accumGreenBits - current.accumGreenBits);
            }

            if (desired.accumBlueBits != FramebufferConfig::ATTR_DONT_CARE) {
                extraDiff += (desired.accumBlueBits - current.accumBlueBits) *
                             (desired.accumBlueBits - current.accumBlueBits);
            }

            if (desired.accumAlphaBits != FramebufferConfig::ATTR_DONT_CARE) {
                extraDiff += (desired.accumAlphaBits - current.accumAlphaBits) *
                             (desired.accumAlphaBits - current.accumAlphaBits);
            }

            if (desired.samples != FramebufferConfig::ATTR_DONT_CARE) {
                extraDiff += (desired.samples - current.samples) *
                             (desired.samples - current.samples);
            }

            if (desired.sRGB && !current.sRGB) {
                extraDiff++;
            }
        }

        // Figure out if the current one is better than the best one found so far
        // Least number of missing buffers is the most important heuristic,
        // then color buffer size match and lastly size match for other buffers

        if (missing < leastMissing) {
            closest = i;
        } else if (missing == leastMissing) {
            if ( (colorDiff < leastColorDiff) || ( (colorDiff == leastColorDiff) && (extraDiff < leastExtraDiff) ) ) {
                closest = i;
            }
        }

        if ( (int)i == closest ) {
            leastMissing = missing;
            leastColorDiff = colorDiff;
            leastExtraDiff = extraDiff;
        }
    } // for alternatives
    if (closest == -1) {
        throw std::logic_error("closest = -1");
    }

    return alternatives[closest];
} // chooseFBConfig

struct OSGLContextPrivate
{
#ifdef __NATRON_LINUX__
    std::unique_ptr<OSGLContext_xdg> _platformContext;
#endif

#ifdef NATRON_RENDER_SHARED_CONTEXT
    // When we use a single GL context, renders are all sharing the same context and lock it when trying to render
    QMutex renderOwningContextMutex;
    QWaitCondition renderOwningContextCond;
    AbortableRenderInfoPtr renderOwningContext;
    int renderOwningContextCount;
#ifdef DEBUG
    double renderOwningContextFrameTime;
#endif
#endif

    // This pbo is used to call glTexSubImage2D and glReadPixels asynchronously
    GLuint pboID;

    // The main FBO onto which we do all renders
    GLuint fboID;

    bool threadExclusive;

    OSGLContextPrivate()
        : _platformContext()
#ifdef NATRON_RENDER_SHARED_CONTEXT
        , renderOwningContextMutex()
        , renderOwningContextCond()
        , renderOwningContext()
        , renderOwningContextCount(0)
#ifdef DEBUG
        , renderOwningContextFrameTime(0)
#endif
#endif
        , pboID(0)
        , fboID(0)
        , threadExclusive(false)
    {
    }

    // Create PBO and FBO if needed
    void init();
};

OSGLContext::OSGLContext(const FramebufferConfig& pixelFormatAttrs,
                         const OSGLContext* shareContext,
                         int major,
                         int minor,
                         const GLRendererID& rendererID,
                         bool coreProfile,
                         bool threadExclusive)
    : _imp(new OSGLContextPrivate())
{
    _imp->threadExclusive = threadExclusive;
    if (coreProfile) {
        // Don't bother with core profile with OpenGL < 3
        if (major < 3) {
            coreProfile = false;
        }
    }
#ifdef DEBUG
    // Mesa's llvmpipe compiles its built-in shaders while a context is created, and that raises
    // FE_INVALID/FE_DIVBYZERO. Render threads trap those in debug builds.
    boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
#ifdef __NATRON_LINUX__
    if (appPTR->isOnWayland()) {
        _imp->_platformContext.reset( new OSGLContext_wayland(pixelFormatAttrs, major, minor, coreProfile, rendererID, shareContext ? static_cast<const OSGLContext_wayland *>(shareContext->_imp->_platformContext.get()) : nullptr) );
    } else {
        _imp->_platformContext.reset( new OSGLContext_x11(pixelFormatAttrs, major, minor, coreProfile, rendererID, shareContext ? static_cast<const OSGLContext_x11 *>(shareContext->_imp->_platformContext.get()) : nullptr) );
    }
#endif
}

OSGLContext::~OSGLContext()
{
    setContextCurrentNoRender();
    if (_imp->pboID) {
        glDeleteBuffers(1, &_imp->pboID);
    }
    if (_imp->fboID) {
        glDeleteFramebuffers(1, &_imp->fboID);
    }
    unsetCurrentContextNoRender();
}

void
OSGLContext::checkOpenGLVersion()
{
    const char *verstr = (const char *) glGetString(GL_VERSION);
    int major, minor;

    if ( (verstr == NULL) || (std::sscanf(verstr, "%d.%d", &major, &minor) != 2) ) {
        major = minor = 0;
        //fprintf(stderr, "Invalid GL_VERSION format!!!\n");
    }

    if ( (major < GLVersion.major) || ( (major == GLVersion.major) && (minor < GLVersion.minor) ) ) {
        std::stringstream ss;
        ss << NATRON_APPLICATION_NAME << " requires at least OpenGL " << GLVersion.major << "." << GLVersion.minor << "to perform OpenGL accelerated rendering." << std::endl;
        ss << "Your system only has OpenGL " << major << "." << minor << "available.";
        throw std::runtime_error( ss.str() );
    }
}

GLuint
OSGLContext::getPBOId() const
{
    return _imp->pboID;
}

GLuint
OSGLContext::getFBOId() const
{
    return _imp->fboID;
}

void
OSGLContext::setContextCurrentNoRender()
{
#ifdef __NATRON_LINUX__
    if (appPTR->isOnWayland()) {
        OSGLContext_wayland::makeContextCurrent( static_cast<const OSGLContext_wayland *>( _imp->_platformContext.get()) );
    } else {
        OSGLContext_x11::makeContextCurrent( static_cast<const OSGLContext_x11 *>( _imp->_platformContext.get()) );
    }
#endif
}

void
OSGLContext::setContextCurrent(const AbortableRenderInfoPtr& abortInfo
#ifdef DEBUG
                               ,
                               double frameTime
#endif
                               )
{
    assert(_imp && _imp->_platformContext);

#ifdef NATRON_RENDER_SHARED_CONTEXT
    QMutexLocker k(&_imp->renderOwningContextMutex);
    if (!_imp->threadExclusive) {
        while (_imp->renderOwningContext && _imp->renderOwningContext != abortInfo) {
            _imp->renderOwningContextCond.wait(k.mutex());
        }
        _imp->renderOwningContext = abortInfo;
    }
#ifdef DEBUG
    if (!_imp->renderOwningContextCount) {
        _imp->renderOwningContextFrameTime = frameTime;
        //qDebug() << "Attaching" << this << "to render frame" << frameTime;
    }
#endif
    ++_imp->renderOwningContextCount;
#endif

    setContextCurrentNoRender();

    // The first time this context is made current, allocate the PBO and FBO
    // This is thread-safe
    if (!_imp->pboID) {
        _imp->init();
    }
}

void
OSGLContext::unsetCurrentContextNoRender()
{
#ifdef __NATRON_LINUX__
    if (appPTR->isOnWayland()) {
        OSGLContext_wayland::makeContextCurrent( nullptr );
    } else {
        OSGLContext_x11::makeContextCurrent( nullptr );
    }
#endif
}

bool OSGLContext::threadHasACurrentContext() {
#ifdef __NATRON_LINUX__
    if (appPTR->isOnWayland()) {
        return OSGLContext_wayland::threadHasACurrentContext();
    }
    return OSGLContext_x11::threadHasACurrentContext();
#else
#error "Unsupported platform."
#endif
}

void
OSGLContext::unsetCurrentContext(const AbortableRenderInfoPtr& abortInfo)
{
#ifdef NATRON_RENDER_SHARED_CONTEXT
    QMutexLocker k(&_imp->renderOwningContextMutex);
    if (_imp->threadExclusive) {
        if (_imp->renderOwningContextCount <= 0) {
            return;
        }
        --_imp->renderOwningContextCount;
        if (!_imp->renderOwningContextCount) {
            unsetCurrentContextNoRender();
        }

        return;
    }
    if (abortInfo != _imp->renderOwningContext) {
        return;
    }
    --_imp->renderOwningContextCount;
    if (!_imp->renderOwningContextCount) {
#ifdef DEBUG
        //qDebug() << "Detaching" << this << "from frame" << _imp->renderOwningContextFrameTime;
#endif
        _imp->renderOwningContext.reset();
        unsetCurrentContextNoRender();
        //Wake-up only one thread waiting, since each thread that is waiting in setContextCurrent() will actually call this function.
        _imp->renderOwningContextCond.wakeOne();
    }
#else
    unsetCurrentContextNoRender();
#endif
}

bool
OSGLContext::isThreadExclusive() const
{
    return _imp->threadExclusive;
}

int
OSGLContext::getRenderBindCount() const
{
#ifdef NATRON_RENDER_SHARED_CONTEXT
    QMutexLocker k(&_imp->renderOwningContextMutex);

    return _imp->renderOwningContextCount;
#else

    return 0;
#endif
}

void
OSGLContextPrivate::init()
{
    glGenBuffers(1, &pboID);
    glGenFramebuffers(1, &fboID);
}

bool
OSGLContext::stringInExtensionString(const char* string,
                                     const char* extensions)
{
    const char* start = extensions;

    for (;; ) {
        const char* where;
        const char* terminator;

        where = strstr(start, string);
        if (!where) {
            return false;
        }

        terminator = where + strlen(string);
        if ( (where == start) || (*(where - 1) == ' ') ) {
            if ( (*terminator == ' ') || (*terminator == '\0') ) {
                break;
            }
        }

        start = terminator;
    }

    return true;
}


// GL_DEVICE_UUID_EXT from EXT_external_objects, which the bundled glad does not generate.
static const GLenum kGlDeviceUuidExt = 0x9597;

bool
OSGLContext::readCurrentDeviceUuid(void (*getUnsignedBytei_vEXT)(void),
                                   std::array<uint8_t, 16>* uuid)
{
    if (!getUnsignedBytei_vEXT || !uuid) {
        return false;
    }
    const char* extensions = reinterpret_cast<const char*>( glGetString(GL_EXTENSIONS) );
    if (!extensions) {
        return false;
    }
    if ( !stringInExtensionString("GL_EXT_memory_object", extensions) &&
         !stringInExtensionString("GL_EXT_semaphore", extensions) ) {
        return false;
    }
    typedef void (APIENTRYP GetUnsignedBytei_vEXTProc)(GLenum target, GLuint index, GLubyte* data);
    GetUnsignedBytei_vEXTProc getUuid = reinterpret_cast<GetUnsignedBytei_vEXTProc>(getUnsignedBytei_vEXT);
    while (glGetError() != GL_NO_ERROR) {
    }
    std::array<uint8_t, 16> u {};
    getUuid(kGlDeviceUuidExt, 0, u.data());
    if (glGetError() != GL_NO_ERROR) {
        return false;
    }
    *uuid = u;
    return true;
}

bool
OSGLContext::isSoftwareRendererName(const std::string& rendererName)
{
    static const char* const softwareNames[] = {
        "llvmpipe", "softpipe", "swrast", "Software Rasterizer",
    };
    for (const char* name : softwareNames) {
        if (rendererName.find(name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

GLRendererPick
OSGLContext::pickRenderer(const std::list<OpenGLRendererInfo>& renderers,
                          const GpuDeviceSummary* chosenOrNull,
                          bool allowSoftware)
{
    GLRendererPick pick;
    if ( renderers.empty() ) {
        pick.reason = "disabled: no GL renderer is available";
        return pick;
    }

    const OpenGLRendererInfo* firstAllowed = 0;
    for (const OpenGLRendererInfo& r : renderers) {
        if (r.accelerated || allowSoftware) {
            firstAllowed = &r;
            break;
        }
    }
    if (!firstAllowed) {
        pick.reason = "disabled: only a software GL renderer is available";
        return pick;
    }

    const OpenGLRendererInfo* matched = 0;
    if (chosenOrNull) {
        for (const OpenGLRendererInfo& r : renderers) {
            if ( (r.accelerated || allowSoftware) && r.hasUuid && (r.deviceUuid == chosenOrNull->uuid) ) {
                matched = &r;
                break;
            }
        }
        if (!matched && chosenOrNull->vendorID != 0) {
            for (const OpenGLRendererInfo& r : renderers) {
                if ( (r.accelerated || allowSoftware) && (r.pciVendorID != 0) &&
                     (r.pciVendorID == chosenOrNull->vendorID) && (r.pciDeviceID == chosenOrNull->deviceID) ) {
                    matched = &r;
                    break;
                }
            }
        }
    }

    const OpenGLRendererInfo* picked = matched ? matched : firstAllowed;
    pick.enabled = true;
    pick.rendererID = picked->rendererID;
    pick.reason = picked->rendererName;
    if (!picked->accelerated) {
        pick.reason += ", software";
    }
    if (matched) {
        pick.reason += " (matches the chosen Vulkan device)";
    } else if (chosenOrNull) {
        pick.reason += " (default renderer: no GL renderer matches the chosen Vulkan device)";
    } else {
        pick.reason += " (default renderer)";
    }
    return pick;
}

void
OSGLContext::getGPUInfos(std::list<OpenGLRendererInfo>& renderers)
{
#ifdef __NATRON_LINUX__
    if (appPTR->isOnWayland()) {
        OSGLContext_wayland::getGPUInfos(renderers);
    } else {
        OSGLContext_x11::getGPUInfos(renderers);
    }
#endif
}

NATRON_NAMESPACE_EXIT
