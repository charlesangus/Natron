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

#include "GPUContextPool.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <map>
#include <set>
#include <stdexcept>

#include <QMutex>
#include <QThread>
#include <QWaitCondition>

#include "Engine/AppManager.h"
#include "Engine/OSGLContext.h"

NATRON_NAMESPACE_ENTER

namespace {
// Lets getContextForCurrentThread() skip the pool mutex on the threads that never asked for a context.
thread_local bool tThreadAskedForContext = false;
}

struct GPUContextPoolPrivate
{
    mutable QMutex contextPoolMutex;

    // protected by contextPoolMutex
    std::set<OSGLContextPtr> glContextPool;

#ifdef NATRON_RENDER_SHARED_CONTEXT
    OSGLContextWPtr lastUsedGLContext;
#else
    QWaitCondition glContextPoolEmpty;
    std::set<attachedGLContexts> attachedGLContexts;
#endif

    // protected by contextPoolMutex. A thread that exits leaves its context here unbound; a later thread created at
    // the same address takes it over, which is safe because an unbound context may be made current on any thread.
    std::map<QThread*, OSGLContextPtr> threadContexts;

    // The OpenGL context to use for sharing
    OSGLContextWPtr glShareContext;

    int currentOpenGLRendererMaxTexSize;

    // Rendering state set at startup. Separate from contextPoolMutex so that reading it never waits on a context
    // creation in progress.
    mutable QMutex stateMutex;
    bool stateEnabled;
    GLRendererID stateRendererID;
    std::string stateStatus;

    // -1 when there is no test override, else 0 or 1
    std::atomic<int> enabledOverride;

    // kNoMaxOverride when there is no test override
    static constexpr int kNoMaxOverride = INT_MIN;
    std::atomic<int> maxContextsOverride;

    GPUContextPoolPrivate()
        : contextPoolMutex()
        , glContextPool()
#ifdef NATRON_RENDER_SHARED_CONTEXT
        , lastUsedGLContext()
#else
        , glContextPoolEmpty()
        , attachedGLContexts()
#endif
        , threadContexts()
        , glShareContext()
        , currentOpenGLRendererMaxTexSize(0)
        , stateMutex()
        , stateEnabled(false)
        , stateRendererID()
        , stateStatus()
        , enabledOverride(-1)
        , maxContextsOverride(kNoMaxOverride)
    {
    }
};

namespace {
int
getMaxContexts(const GPUContextPoolPrivate& imp)
{
    const int override = imp.maxContextsOverride.load();
    return std::max(override == GPUContextPoolPrivate::kNoMaxOverride ? GPUContextPool::kMaxRenderGLContexts : override, 1);
}
}

GPUContextPool::GPUContextPool()
    : _imp( new GPUContextPoolPrivate() )
{
}

GPUContextPool::~GPUContextPool()
{
}

void
GPUContextPool::setRenderingState(bool enabled, const GLRendererID& rendererID, const std::string& status)
{
    QMutexLocker k(&_imp->stateMutex);

    _imp->stateEnabled = enabled;
    _imp->stateRendererID = rendererID;
    _imp->stateStatus = status;
}

bool
GPUContextPool::isRenderingEnabled() const
{
    const int override = _imp->enabledOverride.load();
    if (override >= 0) {
        return override != 0;
    }
    QMutexLocker k(&_imp->stateMutex);

    return _imp->stateEnabled;
}

GLRendererID
GPUContextPool::getRendererID() const
{
    QMutexLocker k(&_imp->stateMutex);

    return _imp->stateRendererID;
}

std::string
GPUContextPool::getRendererStatus() const
{
    QMutexLocker k(&_imp->stateMutex);

    return _imp->stateStatus;
}

void
GPUContextPool::setRenderingEnabledForTests(std::optional<bool> enabled)
{
    _imp->enabledOverride.store(enabled ? (*enabled ? 1 : 0) : -1);
}

void
GPUContextPool::setMaxContextsForTests(std::optional<int> maxContexts)
{
    _imp->maxContextsOverride.store(maxContexts ? *maxContexts : GPUContextPoolPrivate::kNoMaxOverride);
}

void
GPUContextPool::clear()
{
    QMutexLocker k(&_imp->contextPoolMutex);

    _imp->glContextPool.clear();
    _imp->threadContexts.clear();
}


int
GPUContextPool::getCurrentOpenGLRendererMaxTextureSize() const
{
    QMutexLocker k(&_imp->contextPoolMutex);
    return _imp->currentOpenGLRendererMaxTexSize;
}

OSGLContextPtr
GPUContextPool::attachGLContextToRender(bool checkIfGLLoaded)
{
    if (checkIfGLLoaded && (!appPTR->isOpenGLLoaded() || !isRenderingEnabled())) {
        return OSGLContextPtr();
    }
    QMutexLocker k(&_imp->contextPoolMutex);

    // Context-sharing disabled as it is not needed
    OSGLContextPtr shareContext;// _imp->glShareContext.lock();
    OSGLContextPtr newContext;
    const GLRendererID rendererID = getRendererID();
    const int maxContexts = getMaxContexts(*_imp);

#ifndef NATRON_RENDER_SHARED_CONTEXT
    while (_imp->glContextPool.empty() && (int)_imp->attachedGLContexts.size() >= maxContexts) {
        _imp->glContextPoolEmpty.wait(k.mutex());
    }
    if ( _imp->glContextPool.empty() ) {
        assert( (int)_imp->attachedGLContexts.size() < maxContexts );
        //  Create a new one
        newContext = std::make_shared<OSGLContext>( FramebufferConfig(), shareContext.get(), GLVersion.major, GLVersion.minor, rendererID );
    } else {
        std::set<OSGLContextPtr>::iterator it = _imp->glContextPool.begin();
        newContext = *it;
        assert(newContext);
        _imp->glContextPool.erase(it);
    }
#else

    if ( (int)_imp->glContextPool.size() < maxContexts ) {
        //  Create a new one
        newContext = std::make_shared<OSGLContext>( FramebufferConfig(), shareContext.get(), GLVersion.major, GLVersion.minor, rendererID );
        _imp->glContextPool.insert(newContext);
    } else {
        while ((int)_imp->glContextPool.size() > maxContexts) {
            _imp->glContextPool.erase(_imp->glContextPool.begin());
        }

        // Cycle through all contexts for all renders
        OSGLContextPtr lastContext = _imp->lastUsedGLContext.lock();
        if (!lastContext) {
            newContext = *_imp->glContextPool.begin();
        } else {
            std::set<OSGLContextPtr>::iterator foundLast = _imp->glContextPool.find(lastContext);
            assert( foundLast != _imp->glContextPool.end() );
            if ( foundLast == _imp->glContextPool.end() ) {
                throw std::logic_error("No context to attach");
            } else {
                std::set<OSGLContextPtr>::iterator next = foundLast;
                ++next;
                if ( next == _imp->glContextPool.end() ) {
                    next = _imp->glContextPool.begin();
                }
                newContext = *next;
            }
        }
    }

#endif //NATRON_RENDER_SHARED_CONTEXT
    assert(newContext);

    // Before the startup state is set the GL entry points are not loaded yet, so there is nothing to query.
    if (isRenderingEnabled() && !_imp->currentOpenGLRendererMaxTexSize) {
        newContext->setContextCurrentNoRender();
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &_imp->currentOpenGLRendererMaxTexSize);
        newContext->unsetCurrentContextNoRender();
    }

    // If this is the first context, set it as the sharing context
    if (!shareContext) {
        _imp->glShareContext = newContext;
    }

#ifndef NATRON_RENDER_SHARED_CONTEXT
    _imp->attachedGLContexts.insert(newContext);
#else
    _imp->lastUsedGLContext = newContext;
#endif

    return newContext;
} // GPUContextPool::attachGLContextToRender

OSGLContextPtr
GPUContextPool::getOrCreateContextForCurrentThread()
{
    if (!appPTR->isOpenGLLoaded() || !isRenderingEnabled()) {
        return OSGLContextPtr();
    }
    QThread* const thread = QThread::currentThread();
    QMutexLocker k(&_imp->contextPoolMutex);

    tThreadAskedForContext = true;
    std::map<QThread*, OSGLContextPtr>::const_iterator found = _imp->threadContexts.find(thread);
    if (found != _imp->threadContexts.end()) {
        return found->second;
    }

    const int maxContexts = getMaxContexts(*_imp);
    if ((int)_imp->threadContexts.size() >= maxContexts) {
        return OSGLContextPtr();
    }
    const GLRendererID rendererID = getRendererID();

    OSGLContextPtr newContext;
    try {
        newContext = std::make_shared<OSGLContext>(FramebufferConfig(), nullptr, GLVersion.major, GLVersion.minor, rendererID, false /*coreProfile*/, true /*threadExclusive*/);
    } catch (const std::exception&) {
        return OSGLContextPtr();
    }

    // Binding the new context to read the limit would unbind whatever context the thread is rendering with.
    if (!_imp->currentOpenGLRendererMaxTexSize && !OSGLContext::threadHasACurrentContext()) {
        newContext->setContextCurrentNoRender();
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &_imp->currentOpenGLRendererMaxTexSize);
        newContext->unsetCurrentContextNoRender();
    }

    _imp->threadContexts.insert(std::make_pair(thread, newContext));

    return newContext;
} // GPUContextPool::getOrCreateContextForCurrentThread

OSGLContextPtr
GPUContextPool::getContextForCurrentThread() const
{
    if (!tThreadAskedForContext) {
        return OSGLContextPtr();
    }
    QThread* const thread = QThread::currentThread();
    QMutexLocker k(&_imp->contextPoolMutex);

    std::map<QThread*, OSGLContextPtr>::const_iterator found = _imp->threadContexts.find(thread);

    return found == _imp->threadContexts.end() ? OSGLContextPtr() : found->second;
}

std::size_t
GPUContextPool::getNumThreadContextsUsedForRender() const
{
    QMutexLocker k(&_imp->contextPoolMutex);
    std::size_t n = 0;

    for (std::map<QThread*, OSGLContextPtr>::const_iterator it = _imp->threadContexts.begin(); it != _imp->threadContexts.end(); ++it) {
        // The PBO is created the first time a render binds the context.
        if (it->second->getPBOId() != 0) {
            ++n;
        }
    }

    return n;
}

void
GPUContextPool::releaseGLContextFromRender(const OSGLContextPtr& context)
{
#ifndef NATRON_RENDER_SHARED_CONTEXT
    QMutexLocker k(&_imp->contextPoolMutex);

    // The thread has a context on its TLS so it must be found in the attached contexts set
    std::set<OSGLContextPtr>::iterator foundAttached = _imp->attachedGLContexts.find(context);

    assert( foundAttached != _imp->attachedGLContexts.end() );
    if ( foundAttached != _imp->attachedGLContexts.end() ) {
        // Re-insert back into the contextPool so it can be re-used
        _imp->glContextPool.insert(*foundAttached);
        _imp->attachedGLContexts.erase(foundAttached);

        // Wake-up one thread waiting in attachContextToThread().
        // No need to wake all threads because each thread releasing a context will wake up one thread.
        _imp->glContextPoolEmpty.wakeOne();
    }
#else
    Q_UNUSED(context);
#endif
}

NATRON_NAMESPACE_EXIT
