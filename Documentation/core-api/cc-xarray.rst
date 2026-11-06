.. SPDX-License-Identifier: GPL-2.0+

Concurrent XArray migration contracts
====================================

ScaleCache's Concurrent XArray (``include/linux/cc_xarray.h``) is not a
lockless drop-in replacement for every XArray operation.  The contracts
below apply to the cc-core port and to consumers being ported on top of it.
See :doc:`xarray` for the underlying data structure and ordinary XArray API.

Cursor lifetime and conflict retries
-----------------------------------

Acquire advanced cursor pins inside an RCU read-side section.  A successful
pin remains valid until ``cc_xas_clear_xa_node()``; unwind retained traversal
references with ``cc_xas_rewind_refcnt()`` as well.  Node retirement requires
zero pins and physical freeing waits for an RCU grace period.  Public RCU
read APIs do not consume these advanced cursor pins.

``cc_xas_try_split()`` preserves the observed entry and can reject stale
entry/order observations or competing pins with ``-EAGAIN``.  Advanced pin
admission into an active split reports ``-EBUSY``.  On either conflict,
release cursor/traversal references, drop the operation's locks, reset the
state and restart the whole conflict scan.  Do not retain an old conflict
entry or continue from its old cursor.  An ENOMEM-only retry loop is not
sufficient; allocation recovery remains a separate ``cc_xas_nomem()`` path.
Other errors, such as invalid split orders, are not conflict retries.

Preallocated ``cc_xas_split()`` requires external exclusion of writers and
GC over the affected range.  ``xa_lock`` alone does not exclude lockless
CC writers.  Destruction and subtree replacement likewise require exclusion
of all users; the bounded split API is not permission to omit that exclusion
from other range operations.

Committed erase
---------------

After ``cc_xas_store(xas, NULL)`` has committed its slot deletion, empty-node
logical deletion is optional cleanup.  Failure to re-pin a node or its
parent during that cleanup leaves ``CC_XAS_RESTART``, with no error, and
preserves the original old-entry return value.  This applies to every re-pin
failure, not just ``-EBUSY``.  Cleanup must not replay the completed erase:
a later writer's entry belongs to that later operation.

This rule does not suppress failures before the slot change.  Consumers
must still inspect the advanced error state and handle genuine admission,
allocation and conflict failures.  Multi-index stores retain their existing
old-entry return limitations documented in ``lib/cc_xarray.c``.

Garbage collection and shadow accounting
---------------------------------------

``cc_xa_garbage_collector(xa, update)`` takes an explicit node-update callback.
Pass ``cc_workingset_update_node`` for a tracked page-cache mapping, and
``NULL`` only for an untracked array.  The callback is used synchronously
and is not retained.  It runs without the node lock, may run without
``xa_lock`` and with interrupts disabled, and must serialize its own node
bookkeeping without sleeping, enabling interrupts or mutating the array.
Do not hold a lock that the callback needs, in particular the shadow-LRU
lock, across full-array GC.  Advanced mapping operations should install the
same accounting callback through ``cc_mapping_set_update()``.

Full-array GC permits concurrent single-slot writers and cursor pins:
pinned nodes are skipped, retirement prevents new pins, and freeing uses
RCU.  Destruction and range replacement/splitting must be excluded.
Retirement first atomically claims the same ``gc_flag`` ownership used by
logical deletion and reuse, then retires zero references.  A plain flag
check followed by reference retirement is insufficient: a logical deleter
may temporarily drop its cursor pin while it still owns that flag.

The shadow shrinker instead uses the private
``cc_xa_reclaim_empty_children(node)`` interface.  It keeps RCU and the
candidate's node lock, mapping ``xa_lock``, shadow-LRU lock and optional inode
lock.  It only traverses logically deleted, empty, off-LRU child subtrees.
There are at most ``CC_XA_CHUNK_SIZE`` descendant-node visits per call, each
with at most ``CC_XA_CHUNK_SIZE`` slot inspections, in addition to the
candidate's own slots.  Descendant locks are trylocks.  Postorder retirement
handles nested tombstones without a whole-mapping scan; budget exhaustion or
contention leaves the remaining links for a later walk.

These children are already excluded from their parents' logical counts.
Unlinking them neither decrements those counts again nor invokes an update
callback or changes LRU membership.  The helper returns true only when no
physical child nodes remain.  Otherwise the shrinker returns ``LRU_SKIP``
with its original LRU lock still held, preserving the list walker's saved
iterator.  It must not drop/reacquire that lock and then return ``LRU_SKIP``.
When cleanup finishes, the existing candidate-isolation path may reclaim the
shadow node and use ``LRU_REMOVED_RETRY`` with the LRU lock dropped.

``address_space.nrexceptional`` is not part of this port's interface.  Do not
restore it, refer to it from downstream topics, or replace its old tests
with ``nrpages`` tests: resident-page accounting is not a count of shadow or
DAX value entries.  Use the target subsystem's current entry/accounting
rules.  In particular, a zero resident-page count is not a reason to skip
DAX value-entry writeback.  CC ``nrpages`` updates continue to use
``mapping->nr_lock`` where required by the page-cache port.
