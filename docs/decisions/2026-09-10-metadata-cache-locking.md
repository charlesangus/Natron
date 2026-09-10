# Lock the OFX per-clip metadata cache instead of thread-copying it

2026-09-10. The OFX per-clip metadata cache in the vendored OpenFX is
protected by locks, not by giving each thread its own copy: a per-`ClipInstance`
`std::mutex` (`_metadataCacheMutex`) guards the time-keyed cache
(`_metadataCache`), a `std::atomic<int>` refcount on `MetadataSet` lets callers
share a set without copying it, a per-`Instance` mutex (`_metadataMutex`)
serialises `getOutputMetadata` and with it the plugin's get-metadata action,
and a generation counter (`_metadataGeneration`) makes sure an invalidation
racing an in-flight derivation is not lost.

## Why a lock, not Natron's usual `TLSHolder` pattern

Natron already has a standard answer for per-clip state that varies by
thread: `TLSHolder<ClipTLSData>`, used a few lines above this cache in the
same class (`Engine/OfxClipInstance.h:255-303`) for the view, mip-map level,
render data, and components-present state a clip needs during a recursive
action. Each thread gets its own `ClipTLSData`; there is no cross-thread
access to worry about, and so no lock.

The metadata cache can't use that pattern, because a per-thread copy defeats
the reason the cache exists. Deriving an output clip's metadata means calling
`kOfxImageEffectActionGetMetadata` on the plugin, and that action recurses
up the whole upstream chain — every clip's `getMetadata()` calls its own
input clips' `getMetadata()` in turn. The cache exists to make that walk a
one-time cost. If every render thread held its own copy of the cache, every
thread would independently pay for the same upstream walk the first time it
touched a given clip and time, which is exactly the cost of not caching at
all. The cache only pays for itself if it is one object all threads can read
without redoing the derivation, which means it has to be shared, which means
it has to be locked.

## The two facts that make locking necessary, not optional

A shared, lockable cache is only a hard requirement if concurrent renders can
actually reach the same `ClipInstance` at the same time. Two things in
Natron's threading model, taken together, say they can:

- **`Engine/EffectInstanceRenderRoI.cpp:1544-1547`.** Effects declaring
  `eRenderSafetyFullySafe` or `eRenderSafetyFullySafeFrame` take neither the
  `eRenderSafetyInstanceSafe` branch's `QMutexLocker` on
  `getRenderInstancesSharedMutex()` nor the `eRenderSafetyUnsafe` branch's
  recursive lock on the plugin. Neither applies to them; they fall into the
  `else` branch, which takes no lock at all:

  ```cpp
  } else {
      // no need to lock
      Q_UNUSED(locker);
  }
  ```

  So for the render-safety levels most modern OFX plugins declare, nothing
  in `Engine/` serialises calls into the plugin, or into the host state that
  backs its clips.

- **`libs/OpenFX/HostSupport/src/ofxhImageEffect.cpp:639-648`.** `Instance`'s
  copy constructor, used to build a render clone, shallow-copies `_clips`
  (a `std::map<std::string, ClipInstance*>` of raw pointers) and sets
  `_ownsData(false)`:

  ```cpp
  Instance::Instance(const Instance& other)
  : Base(other)
  , Param::SetInstance(other)
  , _plugin(other._plugin)
  , _context(other._context)
  , _descriptor(other._descriptor)
  , _clips(other._clips)
  , _interactive(other._interactive)
  , _created(false)
  , _ownsData(false)
  ...
  ```

  A render clone's `_clips` map holds the identical `ClipInstance*` values
  the original instance holds — not copies, the same addresses.

Put together: two render threads can each get a render clone of the same
effect instance, both clones point at the same `ClipInstance` objects, and
neither thread takes a lock before calling into the plugin. Any cache
attached to `ClipInstance` is therefore shared, concurrently accessed, real
state, whether or not it is designed to be. Locking it is not a defensive
choice; it is the only way for `getMetadata()` and `invalidateMetadata()` to
be correct under Natron's actual scheduling.

## Lock ordering: downstream takes the instance mutex, upstream never does

The subtle part of this design is the direction each traversal locks in, and
keeping the two consistent.

`Instance::getOutputMetadata` holds its own `_metadataMutex` across the
`getMetadata()` calls it makes on its input clips — deliberately, not
incidentally, because those calls can themselves trigger derivation on the
upstream instance, which takes *that* instance's `_metadataMutex` in turn.
Because the effect graph is a DAG and this call only ever walks from an
effect to the clips feeding it, the mutexes taken over the course of one
derivation are always acquired in the same order: downstream instance, then
the instances upstream of it. No pair of instances can ever be locked in the
opposite order by another `getOutputMetadata` call, so this direction alone
cannot deadlock.

Invalidation walks the opposite way: `ClipInstance::invalidateMetadata`
starts at whichever clip changed and recurses from an input clip to its
owning effect's *output* clip, i.e. upstream to downstream — the reverse of
derivation. If invalidation took an instance mutex anywhere in that walk, it
would be acquiring the same locks derivation acquires, but in the opposite
order: exactly the shape of an ABBA deadlock against a render thread
concurrently deriving metadata. So invalidation takes no instance mutex at
all. `ClipInstance::_metadataCacheMutex` is deliberately a strict leaf: the
lock is held only long enough to swap `_metadataCache` out and bump
`_metadataGeneration`, is released before anything else happens, and only
after it is released does `invalidateMetadata` drop the swapped-out sets'
references and recurse to the output clip. Nothing that can reach another
clip's lock, or an instance's lock, ever runs while a clip's own cache lock
is held.

A future maintainer changing either path needs to preserve this: the
instance mutex may only ever be taken downstream-to-upstream, and
invalidation may never take one.

## Divergence from the design this suite was reviewed against

None of this — the mutexes, the refcount, the generation counter — exists in
the ASWF-lineage `charlesangus/openfx` where the metadata suite was designed
and reviewed. That reference host is single-threaded: one call into the
plugin at a time, so a cache attached to a clip is never contended and needs
no lock. The locking here is a Natron-specific addition on top of an
otherwise-unchanged design, made necessary by the two facts above and by
nothing else in the suite's behavior. It changes no observable outcome for a
single-threaded host; it only makes the same cache correct under concurrent
renders. The intent is to offer it upstream alongside the rest of the suite,
so the two lineages don't drift over this.

## Known limitation: a clip querying its own effect's output metadata

A plugin that calls `clipGetMetadata` on its own output clip from inside its
`kOfxImageEffectActionGetMetadata` handler makes a request the host has no
defined answer to: deriving the output's metadata requires running the
handler, which is already running. Before this cache existed, that request
recursed without bound and overflowed the stack — the ASWF reference host
still behaves this way, since it has no cache to detect the recursion
against. With the instance mutex added here, the second call blocks on a
lock the first call already holds, on the same thread, so it deadlocks
instead of overflowing.

Neither outcome is correct, but the fix is not to reject requests for an
output clip's metadata outright: an effect legitimately asks other effects
for their already-composed output metadata from outside the get-metadata
action, and that path has to keep working. The real fix is re-entrancy
detection keyed on the thread currently deriving a given instance's
metadata, so a self-referential call can be recognized and rejected instead
of blocking or recursing. No code in this tree exercises the self-referential
path today, so this is recorded as a known gap rather than fixed here.
