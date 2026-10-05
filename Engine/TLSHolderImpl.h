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

#ifndef Engine_TLSHolderImpl_h
#define Engine_TLSHolderImpl_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <type_traits>

#include "TLSHolder.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameRenderContext.h"

NATRON_NAMESPACE_ENTER

// Only effect data is inherited: a spawned thread needs the frame args of the effects it renders
// or evaluates (abort state, render time, request pass), while every other holder's data is
// per-thread state that starts fresh.
template <typename T>
struct TLSInheritsFromSpawner
    : std::false_type {
};

template <>
struct TLSInheritsFromSpawner<EffectInstance::EffectTLSData>
    : std::true_type {
};

inline EffectInstance::EffectTLSDataPtr
makeInheritedTLSData(const EffectInstance::EffectTLSData& from,
                     AppTLS::SpawnKindEnum kind)
{
    if (kind == AppTLS::eSpawnKindMultiThreadSuite) {
        return std::make_shared<EffectInstance::EffectTLSData>(from);
    }

    // A host frame threading worker scopes its own render args, and the spawner may be rendering
    // a tile itself and mutating its render args while this copy runs.
    EffectInstance::EffectTLSDataPtr ret = std::make_shared<EffectInstance::EffectTLSData>();
    ret->beginEndRenderCount = from.beginEndRenderCount;
    ret->actionRecursionLevel = from.actionRecursionLevel;
#ifdef DEBUG
    ret->canSetValue = from.canSetValue;
#endif
    ret->frameArgs = from.frameArgs;

    return ret;
}

template <typename T>
void
TLSHolder<T>::cleanupPerThreadData(const QThread* curThread) const
{
    // Destroyed once the lock is released, so that nothing T's destructor does runs under it.
    std::shared_ptr<T> removed;
    {
        QWriteLocker k(&perThreadDataMutex);
        typename ThreadDataMap::iterator found = perThreadData.find(curThread);
        if (found != perThreadData.end()) {
            removed.swap(found->second.value);
            perThreadData.erase(found);
        }
    }
}

template <typename T>
std::shared_ptr<T>
TLSHolder<T>::findDataForThread(const QThread* curThread) const
{
    QReadLocker k(&perThreadDataMutex);
    const ThreadDataMap& perThreadDataCRef = perThreadData; // take a const ref, since it's a read lock
    typename ThreadDataMap::const_iterator found = perThreadDataCRef.find(curThread);
    if (found != perThreadDataCRef.end()) {
        return found->second.value;
    }

    return std::shared_ptr<T>();
}

template <typename T>
std::shared_ptr<T>
TLSHolder<T>::insertForThread(const QThread* curThread,
                              const std::shared_ptr<T>& value) const
{
    ThreadData data;
    data.value = value;
    {
        QWriteLocker k(&perThreadDataMutex);
        std::pair<typename ThreadDataMap::iterator, bool> inserted = perThreadData.insert(std::make_pair(curThread, data));
        if (!inserted.second) {
            return inserted.first->second.value;
        }
    }
    AppTLS::recordHolderForCurrentThread(shared_from_this());

    return value;
}

template <typename T>
std::shared_ptr<T>
TLSHolder<T>::inheritFromSpawner([[maybe_unused]] const QThread* curThread) const
{
    if constexpr (!TLSInheritsFromSpawner<T>::value) {
        return std::shared_ptr<T>();
    } else {
        AppTLS::SpawnerChain chain;
        appPTR->getAppTLS()->getSpawnerChain(curThread, &chain);
        if (chain.empty()) {
            return std::shared_ptr<T>();
        }

        std::shared_ptr<T> source;
        AppTLS::SpawnKindEnum kind = AppTLS::eSpawnKindMultiThreadSuite;
        {
            QReadLocker k(&perThreadDataMutex);
            const ThreadDataMap& perThreadDataCRef = perThreadData; // take a const ref, since it's a read lock
            for (AppTLS::SpawnerChain::const_iterator it = chain.begin(); it != chain.end(); ++it) {
                if (it->kind == AppTLS::eSpawnKindHostFrameThreading) {
                    kind = AppTLS::eSpawnKindHostFrameThreading;
                }
                typename ThreadDataMap::const_iterator found = perThreadDataCRef.find(it->thread);
                if (found != perThreadDataCRef.end()) {
                    source = found->second.value;
                    break;
                }
            }
        }
        if (!source) {
            return std::shared_ptr<T>();
        }

        std::shared_ptr<T> ret = insertForThread(curThread, makeInheritedTLSData(*source, kind));
        AppTLS::notifyInheritedCopy();

        return ret;
    }
}

template <typename T>
std::shared_ptr<T>
TLSHolder<T>::createFromFrameContext([[maybe_unused]] const QThread* curThread) const
{
    if constexpr (!std::is_same<T, EffectInstance::EffectTLSData>::value) {
        return std::shared_ptr<T>();
    } else {
        const FrameRenderContext* context = AppTLS::currentFrameContext();
        if (!context) {
            return std::shared_ptr<T>();
        }
        ParallelRenderArgsPtr args = context->getArgsForHolder(this);
        if (!args) {
            return std::shared_ptr<T>();
        }

        std::shared_ptr<T> data = std::make_shared<T>();
        data->frameArgs.push_back(args);

        return insertForThread(curThread, data);
    }
}

template <typename T>
std::shared_ptr<T>
TLSHolder<T>::getTLSData() const
{
    QThread* curThread  = QThread::currentThread();
    std::shared_ptr<T> ret = findDataForThread(curThread);

    if (ret) {
        return ret;
    }

    ret = inheritFromSpawner(curThread);
    if (ret) {
        return ret;
    }

    // Even a const read installs the frame's args on this thread, so peeking at another effect's args belongs in the
    // FrameRenderContext. On a task thread, null args mean the frame does not reach this effect, not that nothing renders.
    return createFromFrameContext(curThread);
}

template <typename T>
std::shared_ptr<T>
TLSHolder<T>::getOrCreateTLSData() const
{
    QThread* curThread = QThread::currentThread();
    std::shared_ptr<T> ret = findDataForThread(curThread);

    if (ret) {
        return ret;
    }

    ret = inheritFromSpawner(curThread);
    if (ret) {
        return ret;
    }

    ret = createFromFrameContext(curThread);
    if (ret) {
        return ret;
    }

    ret = insertForThread(curThread, std::make_shared<T>());
    assert(ret);

    return ret;
}

NATRON_NAMESPACE_EXIT

#endif // Engine_TLSHolderImpl_h
