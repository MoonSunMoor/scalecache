.. SPDX-License-Identifier: GPL-2.0

============================
ccXArray calling conventions
============================

This document defines the calling conventions of the ScaleCache cc-core
port from Linux 6.6.8 to 7.2.8.  It complements
``Documentation/core-api/xarray.rst``; a matching name does not make a
``cc_`` operation interchangeable with an ordinary XArray operation.
The implementation is in ``include/linux/cc_xarray.h`` and
``lib/cc_xarray.c``.

This first-stage port targets order-0 page-cache operation.  Keep large
folios disabled for CC mappings through the page-cache allocation policy,
not by disabling ``CONFIG_TRANSPARENT_HUGEPAGE`` or ``CONFIG_XARRAY_MULTI``.
The presence of split APIs is not a guarantee of concurrent large-folio
support.  Direct flush (dflush) is separate from these tree interfaces.

Design constraints from the paper
=================================

The reference is `ScaleCache: A Scalable Page Cache for Multiple Solid-State
Drives <https://doi.org/10.1145/3627703.3629588>`_, EuroSys '24,
Sections 3.1 and 3.2, Algorithms 1--5.  Three properties govern this API:

* Node publication and single-slot entry updates use a CAS winner.  A node
  that has been linked into the tree belongs to the tree, not to the losing
  expansion attempt.  Do not free it because a later head CAS lost.
* Walks track references from root to leaf and release them in reverse
  order.  Logical deletion hides an empty node; insertion may reuse it.
  The paper's deletion-in-progress flag (``ldflag``) is represented by
  ``gc_flag`` here; ``del`` identifies a logically deleted node.
* Logical deletion is not physical reclamation.  The paper's safe point
  excludes further insertion/search and reuse before physical removal.
  A node-reference count alone does not establish that safe point.

Do not replace these rules with ordinary XArray node freeing, a new node
lock, or an assumption that RCU defers CC node freeing.  A successful
shadow removal does not mean that a node was physically reclaimed.

Selecting an interface and callback
===================================

For an address_space, test
``cc_xa_is_ccxarray(CC_XARRAY(&mapping->i_pages))`` before dispatching to
CC operations.  ``CC_XARRAY()`` is a cast, not initialization or conversion
of an existing tree.  Preserve the mapping's established initialization
and the ordinary XArray branch for non-CC mappings.

``cc_xa_*`` convenience functions own their temporary operation state.
``cc_xa_load()`` takes an RCU read lock internally; ``cc_xa_store()`` takes
``xa_lock`` internally.  An ``__cc_xa_*`` variant can instead require the
caller to hold that lock; check the individual function's contract.
These locks do not exclude lockless advanced CC writers.

``cc_xas_*`` advanced functions use a caller-owned ``struct cc_xa_state``.
They do not uniformly acquire locks, release references, or encode errors
in their return value.  Use the rules below rather than mechanically
renaming ordinary ``xas_*`` calls.

Page-cache mutators must retain node-update notification.  For the usual
mapping-dependent setup, call ``cc_mapping_set_update(&xas, mapping)``;
it installs ``cc_workingset_update_node`` for non-DAX, non-shmem mappings.
Paths already restricted to those mappings may set that callback directly
with ``cc_xas_set_update()``.  Generic convenience wrappers do not install
the page-cache callback on behalf of the caller.

The callback updates the shadow-node LRU, not the entry's lifetime.
Its existing ``xa_lock`` assertion and list checks conflict with original
CC filemap insertion, which does not take that lock.  This inherited
mismatch is not resolved by this port.  Installing the callback is
necessary, but is not proof that all callback/list-LRU races are excluded.
Do not silently add a new global locking requirement to the CC copies.

Operation state and reference ownership
=======================================

Declare one stack state per operation with ``CC_XA_STATE()`` or
``CC_XA_STATE_ORDER()``.  Do not share or copy an active state: it owns
references and may own unpublished allocation.  Do not assign ``xa_node``
or manipulate ``refcnt``, ``trace_root`` or ``trace_leaf`` directly.

Two independent reference classes contribute to node reference counts:

* A walk acquires traversal references with the internal
  ``cc_xa_get_node()`` mechanism.  ``cc_xas_rewind_refcnt()`` releases the
  retained traversal chain; it does not release the cursor or clear errors.
* ``cc_xas_set_xa_node()`` transfers the cursor reference.  Release the
  cursor with ``cc_xas_clear_xa_node()``.  This is a reset to the root-restart
  state, so it also clears any error.  It does not unwind traversal refs.

On an exit that still owns both, capture the error, unwind traversal refs,
then release the cursor.  Read ``cc_xas_error()`` before resetting the
cursor.  Neither cleanup operation substitutes for the other.

``cc_xas_store()``, ``cc_xas_try_split()``, ``cc_xas_split()`` and both
shadow-clear helpers unwind traversal references on every return.
They do not provide a general cursor-release guarantee: callers must still
release the cursor.  ``cc_xas_try_split()`` also releases it on error,
leaving the error encoded in the state until reset.

``cc_xas_reset()``, ``cc_xas_set()`` and ``cc_xas_set_order()`` change the
cursor, not the retained traversal chain.  Unwind any retained chain
before repurposing a state.  Restart from the full page index; a node's
local slot offset is not an index.

Entry loads and object lifetime
===============================

``cc_xas_load(&xas, false)`` retains traversal references for the caller's
next operation.  ``cc_xas_load(&xas, true)`` unwinds them before returning.
Both can leave a cursor reference.  A valid positioned state may only
reload its current slot: calling load again is not necessarily a new
root-to-leaf walk.  Use ``cc_xas_set()`` after unwinding when a fresh walk
is required, as for the shadow-clear helpers.

Node references protect the CC traversal; they do not acquire a page or
folio reference.  Keep the enclosing mapping alive and apply the caller's
existing page/folio reference acquisition and revalidation protocol.
A returned pointer is an observation, not ownership of that object.

``cc_xa_load()`` handles its own state cleanup.  Advanced walkers must also
handle internal retry/zero entries through ``cc_xas_retry()`` where their
lookup protocol requires it.  That internal-entry handling is distinct
from retrying a mutation after ``-EEXIST``.

Preserve each original caller's RCU/IRQ/preemption context.  This port does
not introduce a blanket caller-RCU requirement for the original CC copies.
Conversely, an RCU read lock is not permission to race physical CC garbage
collection or destruction: CC nodes are freed synchronously.

Stores, errors and allocation
=============================

``cc_xas_store()`` returns the old or competing entry, while failures are
recorded in ``cc_xas_error(&xas)``.  Check the state even when the returned
entry is non-NULL.  It is not an expected-old-value API: a prior load and
comparison do not bind the store's own load/CAS to that observation.

``cc_xa_store()`` and its locked wrapper instead return an encoded error
on failure; decode it with ``cc_xa_err()``.  Do not decode an advanced
store's returned entry as a substitute for checking its state.

For store/try-split callers, distinguish:

* ``-ENOMEM``: allocation failed.  ``cc_xas_nomem()`` can allocate a node
  with the supplied GFP flags and restart the operation.  It returns true
  only when that allocation succeeded.  Drop locks that cannot sleep
  before calling it with blocking GFP flags, then restore the caller's
  required synchronization before the next attempt.
* ``-EINVAL``: an unsupported argument or split geometry.  It is not a
  contention signal and must not be retried as one.
* ``-EEXIST``: a mutation detected a conflicting slot value.  Return to the
  caller's existing conflict path; do not add an unconditional retry loop
  or translate it into ``-EAGAIN`` or a new ``-EBUSY`` convention.

This is not a closed errno list for every CC API.  For example,
``__cc_xa_insert()`` retains ``-EBUSY`` for an already occupied entry.
Preserve such operation-specific semantics.

The ownership/error portion of an order-0 advanced store is::

    do {
        previous = cc_xas_store(&xas, entry);
    } while (cc_xas_nomem(&xas, gfp));

    error = cc_xas_error(&xas);
    cc_xas_clear_xa_node(&xas);

This fragment assumes the caller supplies its operation's synchronization,
including dropping/reacquiring non-sleepable locks around ``cc_xas_nomem()``
when needed.  ``previous`` is usable as a successful result only if
``error`` is zero.  No extra traversal unwind is needed after store.
Do not reset the cursor before ``cc_xas_nomem()``: it would erase the
``-ENOMEM`` state that the allocator tests.

A state can also own unused preallocation in ``xa_alloc``.  When abandoning
an operation with such allocation, use ``cc_xas_nomem(&xas, GFP_NOWAIT)``
once the state is no longer ``-ENOMEM``; it frees the unused chain and
returns false.  Preserve any error before resetting it for that cleanup.
Do not discard a state that still owns unpublished nodes.

Order and split interfaces
==========================

``cc_xas_get_order()`` requires a successfully loaded, valid cursor at the
canonical slot, not a sibling slot.  It reads the current cursor without
reloading or changing references.  ``cc_xa_get_order()`` owns its temporary
state; a return of zero does not distinguish absence from an order-0 entry.

For ``cc_xas_try_split()`` and ``cc_xas_split()``, the ``order`` argument is
the current order.  The desired target order is set in the state with
``cc_xas_set_order()``.  Their ``entry`` argument is the replacement value,
not an expected old entry.

``cc_xas_try_split_min_order(order)`` is a pure calculation.  At a chunk
boundary it returns ``order - 1`` (or zero for order zero); otherwise it
returns the lower chunk boundary.  It chooses a target requiring at most
one new node, not a general arbitrary-order split.

``cc_xas_try_split()`` returns void; inspect ``cc_xas_error()``.  It can use
one preallocated node or allocate with ``GFP_NOWAIT``.  Its ``-EINVAL`` check
covers the supported child-creation geometry, not all malformed inputs.
Its ``-EEXIST`` check revalidates the canonical slot immediately before the
first shared mutation, against the entry loaded by that invocation.
It does not detect later changes, sibling-only changes, or ABA.
Success preserves unused preallocation; errors free the unpublished chain.

``cc_xas_split_alloc()`` prepares nodes for ``cc_xas_split()`` and may sleep
as permitted by GFP flags.  Check its state before proceeding; release
unused allocation on every abandoned path.  Do not call ``cc_xas_split()``
without the allocation its requested geometry needs.

Both split operations update multiple slots.  The caller must exclude
concurrent writers and physical collection for the affected range in
addition to meeting the documented ``xa_lock`` requirement.  ``xa_lock``
alone cannot exclude lockless ``cc_xas_store()``.  Neither API supplies a
concurrent split transaction or a large-folio design.

Do not assume that failing order-greater-than-zero stores or deletes roll
back earlier slot updates.  Multi-slot rollback/atomicity is not provided
by this first-stage port.

Shadow cleanup
==============

These helpers are for page-cache shadow values, not arbitrary value-entry
containers.  Retain the enclosing mapping/inode and existing reclaim or
truncate locks; install the CC update callback.  Start a fresh root-to-leaf
walk with traversal references retained.  Merely setting the cursor to an
LRU-selected node does not satisfy that requirement.

``cc_xas_clear_shadow_entry(&xas, observed)`` conditionally clears one
order-0 shadow, including a direct head value at index zero.  Pass the
exact result of ``cc_xas_load(&xas, false)`` as ``observed``.  It uses that
value as the CAS expectation rather than reloading a new expectation.
A different shadow or a page installed after that observation is left
intact.  Equality is value equality; it does not detect replacement away
from and back to the same encoded value (ABA).

The helper returns zero or one removed slot, not a failure errno or a
number of reclaimed nodes.  It skips marked and multi-index entries.
A zero return does not distinguish those cases from absence or a lost CAS.
It unwinds traversal references on every return; release the cursor even
when nothing was removed.  A minimal sequence, within the caller's existing
truncate synchronization, is::

    CC_XA_STATE(xas, CC_XARRAY(&mapping->i_pages), index);
    void *observed;
    unsigned int removed;

    cc_mapping_set_update(&xas, mapping);
    observed = cc_xas_load(&xas, false);
    removed = cc_xas_clear_shadow_entry(&xas, observed);
    cc_xas_clear_xa_node(&xas);

For bounded cleanup, visit only indices in the requested range.  Do not use
the node-wide helper to clear a single index or a partial-node range.

``cc_xas_clear_shadow_node()`` is for a selected, non-NULL leaf node.
After obtaining a fresh traversal from its full index, verify that the
cursor still identifies the selected node.  It observes each slot during
the scan and conditionally clears eligible shadows using that observation.
It does not use an earlier node-wide snapshot or clear concurrently
changed values unconditionally.  It returns the number of slots removed
and unwinds traversal references; the caller releases the cursor.

Both helpers leave marks intact, adjust count/value only for successful
slot removals, and use existing update/logical-deletion paths.  Mark checks
and the slot CAS are not one transaction.  These helpers neither promise
to clear every shadow under contention nor make concurrent mark updates
atomic.  An empty root is not promised to be logically deleted or freed.

Physical reclamation and follow-on integration
==============================================

``cc_xa_garbage_collector(struct cc_xarray *xa)`` and ``cc_xa_destroy()``
can physically free nodes synchronously.  Their callers must establish
quiescent users, including no retained cursors/traversal references, and
the paper's safe-point exclusion of new insertion/search/reuse.  Neither
RCU nor taking ``xa_lock`` alone establishes this for lockless CC callers.

This cc-core port does not connect garbage collection to file release.
Logical shadow cleanup therefore does not promise eventual physical
reclamation.  A later integration must establish the safe point before
adding such a call; do not invoke the collector from an ordinary shrinker
or truncate walk merely because a node became empty.

When porting a follow-on CC copy, preserve the non-CC path and check all
return/early-exit paths for state errors, traversal refs, cursors and
unused allocation.  Keep mapping callbacks and page/folio ownership
separate from tree references.  Keep large-folio policy disabled for CC
mappings until the separate multi-slot concurrency design is integrated.
