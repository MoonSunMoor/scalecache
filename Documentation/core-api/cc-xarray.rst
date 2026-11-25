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
Do not use DAX on CC mappings either: DAX stores multi-order entries with
ordinary XArray operations.  Both stay disabled until the multi-index
requirements in "Order and split interfaces" are met.
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

``cc_xas_store()``, ``cc_xas_store_expected()``, ``cc_xas_try_split()``,
``cc_xas_split()`` and both shadow-clear helpers unwind traversal references
on every return.  They do not provide a general cursor-release guarantee:
callers must still release the cursor.  ``cc_xas_try_split()`` also
releases it on error, leaving the error encoded in the state until reset.

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

``cc_xas_store_expected(&xas, old, entry)`` is ``cc_xas_store()`` with the
slot CAS expecting ``old``.  If the canonical slot no longer holds ``old``,
it changes nothing and records ``-EEXIST``.  A path that checks the current
entry and then stores, without a lock that excludes other writers, must use
it and pass the entry it observed (``NULL`` if none).  Otherwise an
insertion made between the check and the store is silently replaced, and
both callers succeed.  The CC copy of ``__filemap_add_folio()`` (#5) must
use it, passing the entry collected by its ``cc_xas_for_each_conflict()``
scan.  This is the one intended difference from the original insertion
code.  Paths that store over an entry they own, such as a locked folio
whose references are frozen during deletion, replacement or migration,
keep ``cc_xas_store()``.  Only the canonical slot is compared.

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
* ``-EEXIST``: a mutation lost a race for a slot, which changed between its
  observation and the CAS.  It does not mean that the index is occupied.
  The slot may now be empty, for example when a store into an empty head
  loses to another thread publishing the root, or when reclaim removed the
  observed shadow.  Return to the caller's existing conflict path, which
  looks the index up again.  Do not add an unconditional retry loop or
  translate it into ``-EAGAIN`` or a new ``-EBUSY`` convention.

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
``cc_xas_set_order()``.  Their ``entry`` argument is the value of the
replacement entries.  A split within one node also requires the canonical
slot to hold ``entry`` and records ``-EEXIST`` otherwise: every caller
replaces an entry with smaller entries of the same value, and without a lock
this is the only way to name the entry that is meant.  If no leaf holds the
index, there is nothing to split and no error is recorded.

``cc_xas_try_split_min_order(order)`` is a pure calculation.  At a chunk
boundary it returns ``order - 1`` (or zero for order zero); otherwise it
returns the lower chunk boundary.  It gives the next target of a split
that goes one step at a time, not a general arbitrary-order split.

``cc_xas_try_split()`` returns void; inspect ``cc_xas_error()``.  It splits
only the piece that holds the index of the state.  A target order of a leaf
or more changes nothing, because such an entry is already one entry per
leaf.  ``-EINVAL`` is recorded for an entry larger than a leaf with a
smaller target; step down with ``cc_xas_try_split_min_order()``.
Errors free an unpublished allocation chain.

``cc_xas_split()`` splits every leaf of the entry.  ``cc_xas_split_alloc()``
is kept for callers copied from the ordinary XArray and does nothing.

A split changes each slot with a CAS from the value that made it part of the
entry and skips a slot that has left the entry, so it never overwrites
another writer.  The caller must own the entry, as the holder of a folio lock
does.  A cc split never creates a node: entries live in leaves only, so
``cc_xas_split_alloc()`` allocates nothing.

Callers may rely on the following for order-greater-than-zero stores and
deletes, and must not add compensation code:

* A failed store or delete leaves no entry partially stored.  Nodes created
  on the way may remain, as with the ordinary XArray.  This holds for a value
  stored over a range too: the slots that still hold it are emptied again.
* A store records ``-ENOMEM`` only before it has changed a slot, as upstream
  does.  The leaves of an entry of several leaves are created before the
  first slot is written, so the retry after ``cc_xas_nomem()`` finds the
  range as the caller saw it.
* ``cc_xas_find_conflict()`` and ``cc_xas_for_each_conflict()`` look at every
  leaf of a range of several leaves, whether the first leaf exists, is
  logically deleted or holds entries.
* A successful store or delete covers the whole range.  ``cc_xas_store()``
  of ``NULL`` clears every slot of the range of the state, whatever it holds,
  and the rest of an entry that begins in the range, as ``xas_store()`` does.
  ``cc_xas_store_expected()`` of ``NULL`` names one entry and removes that
  entry alone.
* Other writers do not act on an intermediate state, for example by storing
  into the wrong slot.

Upstream ``xas_store()`` fails only before it changes any slot, so the
7.2.8 originals have no compensation code.  ``cc_xas_store()`` changes the
slots one CAS at a time and provides the guarantees as follows.

* A store writes the canonical slot first and the sibling slots after it.
  If a slot cannot be taken, it releases the slots in reverse order.  A
  delete clears the sibling slots last to first and the canonical slot at
  the end.  A sibling therefore never refers to a slot that does not hold
  its entry.
* A lookup that follows a sibling checks that the sibling is still there
  after it has read the canonical slot.
* ``cc_xas_store_expected()`` records ``-EEXIST`` when its index lies inside
  another entry or when the canonical slot holds a larger entry.
* A multi-index pointer entry is deleted, replaced or split only by its
  owner, as the holder of a folio lock is.  The owner may replace it with
  ``cc_xas_store()`` through a state of any order at any index of the entry,
  as ``folio_migrate_mapping()`` does with an order-0 state: the new entry
  takes every slot and every leaf of the old one and keeps its marks.
  A value has no owner, so it is
  never stored as a multi-index entry.  Storing a value over a range stores
  it in every slot, and ``cc_xas_get_order()`` of a value is zero.

Callers should also know the limits:

* While a store or delete is in progress, a lookup may find the entry at
  only part of its range, and ``cc_xas_get_order()`` may return a smaller
  order.
* Shadow values that an order-greater-than-zero store displaced before it
  failed with ``-EEXIST`` are dropped, not restored.
* ``-EEXIST`` from ``cc_xas_split()``, or an error from the stores that give
  the pieces to their folios, means that the ownership rule was broken.
  There is no undo: leaves before the failing one are already split.
* A lookup can still return an entry for the wrong index if, between its two
  reads, the entry is deleted and an entry of the same geometry is stored
  again.  Page cache lookups must keep checking that the folio they locked
  contains the index.
* An entry of a leaf or more (order 6 and up with 64-slot nodes) is not
  stored in a slot of an inner node, because a cc node stays linked to its
  parent after it is deleted.  It fills every leaf of its range instead, with
  a canonical slot of its own in each leaf.  A store fills the leaves first
  to last and empties them again if one cannot be taken; a delete follows the
  same pointer from leaf to leaf.  After a successful store or delete the
  state keeps its index and order, as with ``xas_store()``, and its cursor is
  at the first index of the range.
* Callers see such an entry as one entry all the same.  ``cc_xas_find()``,
  ``cc_xas_find_marked()``, the iterators built on them, ``cc_xa_find_after()``
  and ``cc_xas_pause()`` return it once and continue after its last leaf.
  ``cc_xas_get_order()`` counts the leaves that hold the pointer and returns
  the order of the whole entry.  ``cc_xas_advance()`` may be given an index in
  another leaf.  These look at the neighbouring leaves with walks of their
  own, so they cost more for such an entry than for a smaller one.
* The marks of such an entry are kept in every leaf, so that a marked search
  that starts inside the entry finds it and the pieces of a split inherit
  them.  ``cc_xas_set_mark()`` and ``cc_xas_clear_mark()`` change all the
  leaves when the state is at the canonical slot of one of them.  The caller
  must serialise mark updates of one entry, as the page cache does.
* A reader that does not own the entry may see part of it while it is stored
  or deleted, and then gets a smaller order or the entry more than once.

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
separate from tree references.  In addition:

* Match the CC copy one-to-one with the 7.2.8 original function, including
  its large-folio code.  Do not drop order-greater-than-zero handling
  because large folios are disabled.  First-stage tests do not exercise
  that code; it is validated when large folios are enabled.
* Some paths have used ``i_pages`` without a CC branch since 6.6.8, for
  example ``page_cache_delete_batch()`` (#4) and ``__folio_end_writeback()``
  and ``__folio_mark_dirty()`` (#13).  Check such paths when porting the
  topic.  The list is not exhaustive.

  * If an ordinary XArray operation deletes or frees CC nodes there, as
    ``page_cache_delete_batch()`` does with ``xas_store(&xas, NULL)``, fix it
    as a required fix: a separate commit with a reproducer, as for the
    shadow cleanup.
  * If it only changes marks, check it and record the result.  Fix it only
    if a problem is reproduced.

* Keep large folios and DAX disabled for CC mappings until the
  multi-index requirements in "Order and split interfaces" are met.
