// SPDX-License-Identifier: GPL-2.0+
/*
 * Concurrent XArray implementation
 * Based on XArray implementation
 * Copyright (c) 2017-2018 Microsoft Corporation
 * Copyright (c) 2018-2020 Oracle
 * Original Author: Matthew Wilcox <willy@infradead.org>
 */

#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <linux/xarray.h>
#include <linux/cc_xarray.h>

#include "radix-tree.h"

/*
 * Coding conventions in this file:
 *
 * @xa is used to refer to the entire cc_xarray.
 * @xas is the 'cc_xarray operation state'.  It may be either a pointer to
 * an cc_xa_state, or an cc_xa_state stored on the stack.  This is an unfortunate
 * ambiguity.
 * @index is the index of the entry being operated on
 * @mark is an cc_xa_mark_t; a small number indicating one of the mark bits.
 * @node refers to an cc_xa_node; usually the primary one being operated on by
 * this function.
 * @offset is the index into the slots array inside an cc_xa_node.
 * @parent refers to the @cc_xa_node closer to the head than @node.
 * @entry refers to something stored in a slot in the cc_xarray
 */

static inline unsigned int cc_xa_lock_type(const struct cc_xarray *xa)
{
	return (__force unsigned int)xa->xa_flags & 3;
}

static inline void cc_xas_lock_type(struct cc_xa_state *xas, unsigned int lock_type)
{
	if (lock_type == CC_XA_LOCK_IRQ)
		cc_xas_lock_irq(xas);
	else if (lock_type == CC_XA_LOCK_BH)
		cc_xas_lock_bh(xas);
	else
		cc_xas_lock(xas);
}

static inline void cc_xas_unlock_type(struct cc_xa_state *xas, unsigned int lock_type)
{
	if (lock_type == CC_XA_LOCK_IRQ)
		cc_xas_unlock_irq(xas);
	else if (lock_type == CC_XA_LOCK_BH)
		cc_xas_unlock_bh(xas);
	else
		cc_xas_unlock(xas);
}

static inline bool cc_xa_track_free(const struct cc_xarray *xa)
{
	return xa->xa_flags & CC_XA_FLAGS_TRACK_FREE;
}

static inline bool cc_xa_zero_busy(const struct cc_xarray *xa)
{
	return xa->xa_flags & CC_XA_FLAGS_ZERO_BUSY;
}

static inline void cc_xa_mark_set(struct cc_xarray *xa, cc_xa_mark_t mark)
{
	if (!(xa->xa_flags & CC_XA_FLAGS_MARK(mark)))
		xa->xa_flags |= CC_XA_FLAGS_MARK(mark);
}

static inline void cc_xa_mark_clear(struct cc_xarray *xa, cc_xa_mark_t mark)
{
	if (xa->xa_flags & CC_XA_FLAGS_MARK(mark))
		xa->xa_flags &= ~(CC_XA_FLAGS_MARK(mark));
}

static inline unsigned long *node_marks(struct cc_xa_node *node, cc_xa_mark_t mark)
{
	return node->marks[(__force unsigned)mark];
}

static inline bool node_get_mark(struct cc_xa_node *node,
		unsigned int offset, cc_xa_mark_t mark)
{
	return test_bit(offset, node_marks(node, mark));
}

/* returns true if the bit was set */
static inline bool node_set_mark(struct cc_xa_node *node, unsigned int offset,
				cc_xa_mark_t mark)
{
	return __test_and_set_bit(offset, node_marks(node, mark));
}

/* returns true if the bit was set */
static inline bool node_clear_mark(struct cc_xa_node *node, unsigned int offset,
				cc_xa_mark_t mark)
{
	return __test_and_clear_bit(offset, node_marks(node, mark));
}

static inline bool node_any_mark(struct cc_xa_node *node, cc_xa_mark_t mark)
{
	return !bitmap_empty(node_marks(node, mark), CC_XA_CHUNK_SIZE);
}

static inline void node_mark_all(struct cc_xa_node *node, cc_xa_mark_t mark)
{
	bitmap_fill(node_marks(node, mark), CC_XA_CHUNK_SIZE);
}

#define mark_inc(mark) do { \
	mark = (__force cc_xa_mark_t)((__force unsigned)(mark) + 1); \
} while (0)

/*
 * cc_xas_squash_marks() - Merge all marks to the first entry
 * @xas: Array operation state.
 *
 * Set a mark on the first entry if any entry has it set.  Clear marks on
 * all sibling entries.
 */
static void cc_xas_squash_marks(const struct cc_xa_state *xas)
{
	cc_xa_mark_t mark = 0;
	unsigned int limit = xas->xa_offset + xas->xa_sibs + 1;

	for (;;) {
		unsigned long *marks = node_marks(xas->xa_node, mark);

		if (find_next_bit(marks, limit, xas->xa_offset + 1) != limit) {
			__set_bit(xas->xa_offset, marks);
			bitmap_clear(marks, xas->xa_offset + 1, xas->xa_sibs);
		}
		if (mark == CC_XA_MARK_MAX)
			break;
		mark_inc(mark);
	}
}

/* extracts the offset within this node from the index */
static unsigned int get_offset(unsigned long index, struct cc_xa_node *node)
{
	return (index >> node->shift) & CC_XA_CHUNK_MASK;
}

static void cc_xas_set_offset(struct cc_xa_state *xas)
{
	xas->xa_offset = get_offset(xas->xa_index, xas->xa_node);
}

/* move the index either forwards (find) or backwards (sibling slot) */
static void cc_xas_move_index(struct cc_xa_state *xas, unsigned long offset)
{
	unsigned int shift = xas->xa_node->shift;
	xas->xa_index &= ~CC_XA_CHUNK_MASK << shift;
	xas->xa_index += offset << shift;
}

static void __cc_xas_advance(struct cc_xa_state *xas)
{
	xas->xa_offset++;
	cc_xas_move_index(xas, xas->xa_offset);
}

static void *set_bounds(struct cc_xa_state *xas)
{
	//xas->xa_node = CC_XAS_BOUNDS;
	cc_xas_set_xa_node(xas, CC_XAS_BOUNDS);
	return NULL;
}

/*
 * Starts a walk.  If the @xas is already valid, we assume that it's on
 * the right path and just return where we've got to.  If we're in an
 * error state, return NULL.  If the index is outside the current scope
 * of the cc_xarray, return NULL without changing @xas->xa_node.  Otherwise
 * set @xas->xa_node to NULL and return the current head of the array.
 */
static void *cc_xas_start(struct cc_xa_state *xas)
{
	void *entry;

	if (cc_xas_valid(xas))
		return cc_xas_reload(xas);
	if (cc_xas_error(xas))
		return NULL;

	entry = cc_xa_head(xas->xa);
	if (!cc_xa_is_node(entry)) {
		if (xas->xa_index)
			return set_bounds(xas);
	} else {
		if ((xas->xa_index >> cc_xa_to_node(entry)->shift) > CC_XA_CHUNK_MASK)
			return set_bounds(xas);
	}

	//xas->xa_node = NULL;
	cc_xas_set_xa_node(xas, NULL);
	return entry;
}

static __always_inline void *cc_xas_descend(struct cc_xa_state *xas,
					 struct cc_xa_node *node)
{
	unsigned int offset = get_offset(xas->xa_index, node);
	void *entry = cc_xa_entry(xas->xa, node, offset);

	//xas->xa_node = node;
	cc_xas_set_xa_node(xas, node);
	/*
	 * A sibling and the slot it refers to are read separately.  Writers
	 * clear a sibling before they release its canonical slot, so the entry
	 * belongs to this index only if the sibling is still there afterwards.
	 */
	while (cc_xa_is_sibling(entry)) {
		void *sibling = entry;

		entry = cc_xa_entry(xas->xa, node, cc_xa_to_sibling(sibling));
		if (cc_xa_entry(xas->xa, node, offset) == sibling) {
			offset = cc_xa_to_sibling(sibling);
			break;
		}
		entry = cc_xa_entry(xas->xa, node, offset);
	}

	xas->xa_offset = offset;
	return entry;
}

/**
 * cc_xas_load() - Load an entry from the XArray (advanced).
 * @xas: XArray operation state.
 *
 * Usually walks the @xas to the appropriate state to load the entry
 * stored at xa_index.  However, it will do nothing and return %NULL if
 * @xas is in an error state.  cc_xas_load() will never expand the tree.
 *
 * If the cc_xa_state is set up to operate on a multi-index entry, cc_xas_load()
 * may return %NULL or an internal entry, even if there are entries
 * present within the range specified by @xas.
 *
 * Context: Any context.  The caller should hold the xa_lock or the RCU lock.
 * Return: Usually an entry in the XArray, but see description for exceptions.
 */
void *cc_xas_load(struct cc_xa_state *xas, bool rewind)
{
	void *entry = cc_xas_start(xas);
	struct cc_xa_node *node;

	while (cc_xa_is_node(entry)) {
		node = cc_xa_to_node(entry);
		node = cc_xa_get_node(xas, node);

		/* wait for other thread */
		while (__atomic_load_n(&node->gc_flag, __ATOMIC_SEQ_CST))
			cpu_relax();

		/* node logically deleted! returning NULL */
		if (__atomic_load_n(&node->del, __ATOMIC_SEQ_CST)) {
			entry = NULL;
			//cc_xas_reset(xas);
			break;
		}

		if (xas->xa_shift > node->shift)
			break;
		entry = cc_xas_descend(xas, node);
		if (node->shift == 0)		// reached leaf node
			break;
	}
	if (rewind)
		cc_xas_rewind_refcnt(xas);

	return entry;
}
EXPORT_SYMBOL_GPL(cc_xas_load);

/*
 * An entry of a leaf or more fills its leaves: it is in slot 0 of each of
 * them and the last slot is a sibling of that.  Return the entry if @node is
 * such a leaf.  A value never fills a leaf this way.
 */
static __always_inline void *cc_xa_leaf_entry(struct cc_xarray *xa,
					      const struct cc_xa_node *node)
{
	void *entry;

	if (cc_xas_not_node(node) || node->shift)
		return NULL;
	entry = cc_xa_entry(xa, node, 0);
	if (!entry || cc_xa_is_internal(entry) || cc_xa_is_value(entry))
		return NULL;
	if (cc_xa_entry(xa, node, CC_XA_CHUNK_MASK) != cc_xa_mk_sibling(0))
		return NULL;
	return entry;
}

/* The same for the leaf that holds @index. */
static void *cc_xa_leaf_entry_at(struct cc_xarray *xa, unsigned long index)
{
	CC_XA_STATE(xas, xa, index);
	void *entry;

	cc_xas_load(&xas, true);
	entry = cc_xa_leaf_entry(xa, xas.xa_node);
	cc_xas_clear_xa_node(&xas);
	return entry;
}

/*
 * The leaves that @entry fills around the leaf of @index, as the first index
 * of the first one and of the last one.  The same pointer in the next leaf
 * is the same entry.
 */
static void cc_xa_leaf_run(struct cc_xarray *xa, unsigned long index,
			   void *entry, unsigned long *first,
			   unsigned long *last)
{
	index &= ~(unsigned long)CC_XA_CHUNK_MASK;
	*first = index;
	*last = index;
	while (*first && cc_xa_leaf_entry_at(xa, *first - 1) == entry)
		*first -= CC_XA_CHUNK_SIZE;
	while (*last + CC_XA_CHUNK_SIZE > *last &&
	       cc_xa_leaf_entry_at(xa, *last + CC_XA_CHUNK_SIZE) == entry)
		*last += CC_XA_CHUNK_SIZE;
}

/* cc_xas_advance() to an index outside the node of the cursor. */
void cc_xas_advance_far(struct cc_xa_state *xas, unsigned long index)
{
	cc_xas_set(xas, index);
	cc_xas_load(xas, true);
	/* cc_xas_advance() leaves the slot of the index, not its canonical. */
	if (cc_xas_is_node(xas))
		cc_xas_set_offset(xas);
}
EXPORT_SYMBOL_GPL(cc_xas_advance_far);

#define CC_XA_RCU_FREE	((struct cc_xarray *)1)

static void cc_xa_node_free(struct cc_xa_node *node)
{
	CC_XA_NODE_BUG_ON(node, !list_empty(&node->private_list));
	node->array = CC_XA_RCU_FREE;
	//call_rcu(&node->rcu_head, radix_tree_node_rcu_free);
	radix_tree_node_rcu_free(&node->rcu_head);
}

/*
 * cc_xas_destroy() - Free any resources allocated during the XArray operation.
 * @xas: XArray operation state.
 *
 * This function is now internal-only.
 */
static void cc_xas_destroy(struct cc_xa_state *xas)
{
	struct cc_xa_node *next, *node = xas->xa_alloc;

	while (node) {
		CC_XA_NODE_BUG_ON(node, !list_empty(&node->private_list));
		next = rcu_dereference_raw(node->parent);
		radix_tree_node_rcu_free(&node->rcu_head);
		xas->xa_alloc = node = next;
	}
}

/**
 * cc_xas_nomem() - Allocate memory if needed.
 * @xas: XArray operation state.
 * @gfp: Memory allocation flags.
 *
 * If we need to add new nodes to the XArray, we try to allocate memory
 * with GFP_NOWAIT while holding the lock, which will usually succeed.
 * If it fails, @xas is flagged as needing memory to continue.  The caller
 * should drop the lock and call cc_xas_nomem().  If cc_xas_nomem() succeeds,
 * the caller should retry the operation.
 *
 * Forward progress is guaranteed as one node is allocated here and
 * stored in the cc_xa_state where it will be found by cc_xas_alloc().  More
 * nodes will likely be found in the slab allocator, but we do not tie
 * them up here.
 *
 * Return: true if memory was needed, and was successfully allocated.
 */
bool cc_xas_nomem(struct cc_xa_state *xas, gfp_t gfp)
{
	if (xas->xa_node != CC_XA_ERROR(-ENOMEM)) {
		cc_xas_destroy(xas);
		return false;
	}
	if (xas->xa->xa_flags & CC_XA_FLAGS_ACCOUNT)
		gfp |= __GFP_ACCOUNT;
	xas->xa_alloc = kmem_cache_alloc(radix_tree_node_cachep, gfp);
	if (!xas->xa_alloc)
		return false;
	xas->xa_alloc->parent = NULL;
	CC_XA_NODE_BUG_ON(xas->xa_alloc, !list_empty(&xas->xa_alloc->private_list));
	//xas->xa_node = CC_XAS_RESTART;
	cc_xas_set_xa_node(xas, CC_XAS_RESTART);
	return true;
}
EXPORT_SYMBOL_GPL(cc_xas_nomem);

/*
 * __cc_xas_nomem() - Drop locks and allocate memory if needed.
 * @xas: XArray operation state.
 * @gfp: Memory allocation flags.
 *
 * Internal variant of cc_xas_nomem().
 *
 * Return: true if memory was needed, and was successfully allocated.
 */
static bool __cc_xas_nomem(struct cc_xa_state *xas, gfp_t gfp)
	__must_hold(xas->xa->xa_lock)
{
	unsigned int lock_type = cc_xa_lock_type(xas->xa);

	if (xas->xa_node != CC_XA_ERROR(-ENOMEM)) {
		cc_xas_destroy(xas);
		return false;
	}
	if (xas->xa->xa_flags & CC_XA_FLAGS_ACCOUNT)
		gfp |= __GFP_ACCOUNT;
	if (gfpflags_allow_blocking(gfp)) {
		cc_xas_unlock_type(xas, lock_type);
		xas->xa_alloc = kmem_cache_alloc(radix_tree_node_cachep, gfp);
		cc_xas_lock_type(xas, lock_type);
	} else {
		xas->xa_alloc = kmem_cache_alloc(radix_tree_node_cachep, gfp);
	}
	if (!xas->xa_alloc)
		return false;
	xas->xa_alloc->parent = NULL;
	CC_XA_NODE_BUG_ON(xas->xa_alloc, !list_empty(&xas->xa_alloc->private_list));
	//xas->xa_node = CC_XAS_RESTART;
	cc_xas_set_xa_node(xas, CC_XAS_RESTART);
	return true;
}

static void cc_xas_update(struct cc_xa_state *xas, struct cc_xa_node *node)
{
	if (xas->xa_update)
		xas->xa_update(node);
	else
		CC_XA_NODE_BUG_ON(node, !list_empty(&node->private_list));
}

static void *cc_xas_alloc(struct cc_xa_state *xas, unsigned int shift)
{
	struct cc_xa_node *parent = xas->xa_node;
	struct cc_xa_node *node = xas->xa_alloc;

	if (cc_xas_invalid(xas))
		return NULL;

	if (node) {
		xas->xa_alloc = NULL;
	} else {
		gfp_t gfp = GFP_NOWAIT;

		if (xas->xa->xa_flags & CC_XA_FLAGS_ACCOUNT)
			gfp |= __GFP_ACCOUNT;

		node = kmem_cache_alloc(radix_tree_node_cachep, gfp);
		if (!node) {
			cc_xas_set_err(xas, -ENOMEM);
			return NULL;
		}
	}

	if (parent) {
		node->offset = xas->xa_offset;
		//parent->count++;
		//CC_XA_NODE_BUG_ON(node, parent->count > CC_XA_CHUNK_SIZE);
		//cc_xas_update(xas, parent);
	}
	CC_XA_NODE_BUG_ON(node, shift > BITS_PER_LONG);
	CC_XA_NODE_BUG_ON(node, !list_empty(&node->private_list));
	node->shift = shift;
	node->count = 0;
	node->nr_values = 0;
	node->refcnt = 0;
	node->gc_flag = 0;
	node->del = 0;
	RCU_INIT_POINTER(node->parent, xas->xa_node);
	node->array = xas->xa;

	return node;
}

#ifdef CONFIG_XARRAY_MULTI
/* Returns the number of indices covered by a given cc_xa_state */
static unsigned long cc_xas_size(const struct cc_xa_state *xas)
{
	return (xas->xa_sibs + 1UL) << xas->xa_shift;
}
#endif

/*
 * Use this to calculate the maximum index that will need to be created
 * in order to add the entry described by @xas.  Because we cannot store a
 * multi-index entry at index 0, the calculation is a little more complex
 * than you might expect.
 */
static unsigned long cc_xas_max(struct cc_xa_state *xas)
{
	unsigned long max = xas->xa_index;

#ifdef CONFIG_XARRAY_MULTI
	if (xas->xa_shift || xas->xa_sibs) {
		unsigned long mask = cc_xas_size(xas) - 1;
		max |= mask;
		if (mask == max)
			max++;
	}
#endif

	return max;
}

/* The maximum index that can be contained in the array without expanding it */
static unsigned long max_index(void *entry)
{
	if (!cc_xa_is_node(entry))
		return 0;
	return (CC_XA_CHUNK_SIZE << cc_xa_to_node(entry)->shift) - 1;
}

static void cc_xas_shrink(struct cc_xa_state *xas)
{
	struct cc_xarray *xa = xas->xa;
	struct cc_xa_node *node = xas->xa_node;

	for (;;) {
		void *entry;

		CC_XA_NODE_BUG_ON(node, node->count > CC_XA_CHUNK_SIZE);
		if (node->count != 1)
			break;
		entry = cc_xa_entry_locked(xa, node, 0);
		if (!entry)
			break;
		if (!cc_xa_is_node(entry) && node->shift)
			break;
		if (cc_xa_is_zero(entry) && cc_xa_zero_busy(xa))
			entry = NULL;
		//xas->xa_node = CC_XAS_BOUNDS;
		cc_xas_set_xa_node(xas, CC_XAS_BOUNDS);

		RCU_INIT_POINTER(xa->xa_head, entry);
		if (cc_xa_track_free(xa) && !node_get_mark(node, 0, CC_XA_FREE_MARK))
			cc_xa_mark_clear(xa, CC_XA_FREE_MARK);

		node->count = 0;
		node->nr_values = 0;
		if (!cc_xa_is_node(entry))
			RCU_INIT_POINTER(node->slots[0], CC_XA_RETRY_ENTRY);
		cc_xas_update(xas, node);
		//cc_xa_node_free(node);
		if (!cc_xa_is_node(entry))
			break;
		node = cc_xa_to_node(entry);
		node->parent = NULL;
	}
}

/*
 * cc_xas_delete_node() - Attempt to delete an cc_xa_node
 * @xas: Array operation state.
 *
 * Attempts to delete the @xas->xa_node.  This will fail if xa->node has
 * a non-zero reference count.
 */
static void cc_xas_delete_node(struct cc_xa_state *xas)
{
	struct cc_xa_node *node = xas->xa_node;

	for (;;) {
		struct cc_xa_node *parent;
		unsigned char parent_cnt, count;

		/* Escape if @node is root node */
		if (!cc_xa_parent(xas->xa, node))
			break;

		/*
		 * Other writers may have changed the count since the caller
		 * saw it reach zero, and as in update_node() their changes
		 * need not be matched yet: a store that fills the leaf adds
		 * CC_XA_CHUNK_SIZE on top of a change that is still on its way.
		 */
		count = __atomic_load_n(&node->count, __ATOMIC_SEQ_CST);
		CC_XA_NODE_BUG_ON(node, count > 2 * CC_XA_CHUNK_SIZE &&
					count < U8_MAX - CC_XA_CHUNK_SIZE);
		/* Check if entry count is not zero. */
		if (count)
			break;

		/* Return if gc_flag is already set. 
		 * --> Someone else is already doing gc... */
		if (!__sync_bool_compare_and_swap(&node->gc_flag, 0, 1))
			break;

		if (__atomic_load_n(&node->del, __ATOMIC_SEQ_CST)) {
			CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&node->gc_flag, 1, 0));
			break;
		}

		/*
		 * At this point,
		 * 1) @node is not root node.
		 * 2) @node has no entries
		 * 3) This thread is performing logical deletion...
		 */
		cc_xas_set_xa_node(xas, NULL);	// to put refcnt of xas->xa_node

		/* Check if there are other threads in the parent node */
		if (__atomic_load_n(&node->parent->refcnt, __ATOMIC_SEQ_CST) > 1) {
			CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&node->gc_flag, 1, 0));
			cc_xas_set_xa_node(xas, node);
			break;
		}

		/* Check if there are other threads in the @node */
		if (__atomic_load_n(&node->refcnt, __ATOMIC_SEQ_CST) > 1) {
			CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&node->gc_flag, 1, 0));
			cc_xas_set_xa_node(xas, node);
			break;
		}

		/*
		 * The count was read before gc_flag was taken.  A store that
		 * had already passed the gc_flag wait may have completed and
		 * dropped its references since, and its entry must not end up
		 * in a deleted node.  Stores that come later wait on gc_flag.
		 */
		if (__atomic_load_n(&node->count, __ATOMIC_SEQ_CST)) {
			CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&node->gc_flag, 1, 0));
			cc_xas_set_xa_node(xas, node);
			break;
		}

		parent = cc_xa_parent_locked(xas->xa, node);
		//xas->xa_node = parent;
		cc_xas_set_xa_node(xas, parent);
		xas->xa_offset = node->offset;
		//pr_info("deleting node[@%px]... count %u parent @%px\n", node, node->count, parent);
		//cc_xa_node_free(node);
		CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&node->del, 0, 1));

		if (!parent) {
			xas->xa->xa_head = NULL;
			//xas->xa_node = CC_XAS_BOUNDS;
			cc_xas_set_xa_node(xas, CC_XAS_BOUNDS);
			return;
		}

		//parent->slots[xas->xa_offset] = NULL;
		//parent->count--;
		parent_cnt = __sync_sub_and_fetch(&parent->count, 1);
		CC_XA_NODE_BUG_ON(parent, parent_cnt > CC_XA_CHUNK_SIZE);
		/* BUG if gc_flag is already released by other threads */
		CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&node->gc_flag, 1, 0));
		node = parent;
		cc_xas_update(xas, node);
	}

	/* The Concurrent Xarray should not be shrinked if @node is root node */
	//if (!node->parent)
	//	cc_xas_shrink(xas);
}

/**
 * cc_xas_free_nodes() - Free this node and all nodes that it references
 * @xas: Array operation state.
 * @top: Node to free
 *
 * This node has been removed from the tree.  We must now free it and all
 * of its subnodes.  There may be RCU walkers with references into the tree,
 * so we must replace all entries with retry markers.
 */
static void cc_xas_free_nodes(struct cc_xa_state *xas, struct cc_xa_node *top)
{
	unsigned int offset = 0;
	struct cc_xa_node *node = top;

	for (;;) {
		void *entry = cc_xa_entry_locked(xas->xa, node, offset);

		if (node->shift && cc_xa_is_node(entry)) {
			node = cc_xa_to_node(entry);
			offset = 0;
			continue;
		}
		if (entry)
			RCU_INIT_POINTER(node->slots[offset], CC_XA_RETRY_ENTRY);
		offset++;
		while (offset == CC_XA_CHUNK_SIZE) {
			struct cc_xa_node *parent;

			parent = cc_xa_parent_locked(xas->xa, node);
			offset = node->offset + 1;
			node->count = 0;
			node->nr_values = 0;
			cc_xas_update(xas, node);
			cc_xa_node_free(node);
			if (node == top)
				return;
			node = parent;
		}
	}
}

/*
 * Add parents until the selected head can contain cc_xas_max(xas).  Return
 * that raw head snapshot and its height together; an empty head returns the
 * planned allocation height.  Only the cursor reference follows expansion,
 * never the downward traversal trace.
 *
 * Parent election makes a candidate shared before head publication.  Helpers
 * may publish it, but its election winner owns the update callback.  Shared
 * nodes retain the existing quiescent physical-reclamation requirement.
 */
static int cc_xas_expand(struct cc_xa_state *xas, void **head)
{
	struct cc_xarray *xa = xas->xa;
	unsigned long max = cc_xas_max(xas);

	for (;;) {
		void *old_head = cc_xa_head_locked(xa), *entry = old_head;
		struct cc_xa_node *node, *oldroot = NULL;
		unsigned int shift = 0;
		cc_xa_mark_t mark = 0;

		if (cc_xa_is_node(old_head)) {
			oldroot = cc_xa_to_node(old_head);
			cc_xas_set_xa_node(xas, oldroot);
			shift = oldroot->shift + CC_XA_CHUNK_SHIFT;
		} else {
			cc_xas_set_xa_node(xas, NULL);
			if (!old_head && !cc_xa_zero_busy(xa)) {
				*head = NULL;
				if (max == 0)
					return 0;
				while ((max >> shift) >= CC_XA_CHUNK_SIZE)
					shift += CC_XA_CHUNK_SHIFT;
				return shift + CC_XA_CHUNK_SHIFT;
			}
			if (!old_head)
				entry = CC_XA_ZERO_ENTRY;
		}

		if (max <= max_index(old_head)) {
			*head = old_head;
			return shift;
		}

		CC_XA_NODE_BUG_ON(oldroot, shift > BITS_PER_LONG);
		node = cc_xas_alloc(xas, shift);
		if (!node)
			return -ENOMEM;

		/* Allocation inherits the cursor parent; this is a new root. */
		RCU_INIT_POINTER(node->parent, NULL);
		node->offset = 0;
		node->count = 1;
		if (cc_xa_is_value(entry))
			node->nr_values = 1;
		RCU_INIT_POINTER(node->slots[0], entry);

		/* Propagate the aggregated mark info to the new child. */
		for (;;) {
			if (cc_xa_track_free(xa) && mark == CC_XA_FREE_MARK) {
				node_mark_all(node, CC_XA_FREE_MARK);
				if (!cc_xa_marked(xa, CC_XA_FREE_MARK)) {
					node_clear_mark(node, 0, CC_XA_FREE_MARK);
					cc_xa_mark_set(xa, CC_XA_FREE_MARK);
				}
			} else if (cc_xa_marked(xa, mark)) {
				node_set_mark(node, 0, mark);
			}
			if (mark == CC_XA_MARK_MAX)
				break;
			mark_inc(mark);
		}

		if (oldroot) {
			struct cc_xa_node *root_parent;

			oldroot->offset = 0;
			root_parent = __sync_val_compare_and_swap(
					&oldroot->parent, NULL, node);
			if (root_parent) {
				/* Only our losing, unpublished candidate is private. */
				cc_xa_node_free(node);
				(void)__sync_val_compare_and_swap(&xa->xa_head,
						old_head, cc_xa_mk_node(root_parent));
				continue;
			}
			/* A linked parent remains shared even if this CAS loses. */
			(void)__sync_val_compare_and_swap(&xa->xa_head, old_head,
						cc_xa_mk_node(node));
		} else if (__sync_val_compare_and_swap(&xa->xa_head, old_head,
						cc_xa_mk_node(node)) != old_head) {
			cc_xa_node_free(node);
			continue;
		}
		cc_xas_update(xas, node);
		/* Reobserve the head instead of ascending an unpublished parent. */
	}
}

/*
 * cc_xas_create() - Create a slot to store an entry in.
 * @xas: XArray operation state.
 * @allow_root: %true if we can store the entry in the root directly
 *
 * Most users will not need to call this function directly, as it is called
 * by cc_xas_store().  It is useful for doing conditional store operations
 * (see the cc_xa_cmpxchg() implementation for an example).
 *
 * Return: If the slot already existed, returns the contents of this slot.
 * If the slot was newly created, returns %NULL.  If it failed to create the
 * slot, returns %NULL and indicates the error in @xas.
 *
 * Notice that the node containing the slot returned from this function 
 * got refcnt increased. User of this function must decrease refcnt of the node.
 *
 * Root selection keeps its head snapshot and height paired.  If initial
 * root publication loses, discard only the private candidate and reselect.
 *
 */
static void *cc_xas_create(struct cc_xa_state *xas, bool allow_root)
{
	struct cc_xarray *xa = xas->xa;
	void *entry, *curr;
	void __rcu **slot;
	struct cc_xa_node *node = xas->xa_node, *parent = NULL;
	int shift;
	unsigned int order = xas->xa_shift;
	unsigned char parent_cnt;

	if (cc_xas_top(node)) {
select_root:
		shift = cc_xas_expand(xas, &entry);
		if (shift < 0)
			return NULL;
		if (!shift && !allow_root)
			shift = CC_XA_CHUNK_SHIFT;
		node = xas->xa_node;
		slot = &xa->xa_head;
	} else if (cc_xas_error(xas)) {
		return NULL;
	} else if (node) {
		unsigned int offset = xas->xa_offset;

		shift = node->shift;
		entry = cc_xa_entry_locked(xa, node, offset);
		slot = &node->slots[offset];
	} else {
		shift = 0;
		entry = cc_xa_head_locked(xa);
		slot = &xa->xa_head;
	}

	while (shift > order) {
		shift -= CC_XA_CHUNK_SHIFT;
		if (!entry) {
			parent = xas->xa_node;
			node = cc_xas_alloc(xas, shift);
			if (!node)
				return NULL;
			node = cc_xa_get_node(xas, node);
			if (cc_xa_track_free(xa))
				node_mark_all(node, CC_XA_FREE_MARK);
			//rcu_assign_pointer(*slot, cc_xa_mk_node(node));
			/* Perform CAS */
			if ((curr = __sync_val_compare_and_swap(
					slot, NULL, cc_xa_mk_node(node))) != NULL) {
				cc_xa_put_node(xas, node);
				cc_xa_node_free(node);
				if (slot == &xa->xa_head)
					goto select_root;
				node = cc_xa_get_node(xas, cc_xa_to_node(curr));
				goto descend;
			}
			if (parent) {
				parent_cnt = __sync_add_and_fetch(&parent->count, 1);
				CC_XA_NODE_BUG_ON(node, parent_cnt > CC_XA_CHUNK_SIZE);
				cc_xas_update(xas, parent);
			}
		} else if (cc_xa_is_node(entry)) {
			node = cc_xa_get_node(xas, cc_xa_to_node(entry));
			/* Wait for the other thread */
			while (__atomic_load_n(&node->gc_flag, __ATOMIC_SEQ_CST))
				cpu_relax();

			if (__sync_bool_compare_and_swap(&node->del, 1, 0)) {
				/* If the @node is marked as deleted, reuse it. */
				parent = cc_xa_parent(xas->xa, node);
				if (parent)
					__sync_add_and_fetch(&parent->count, 1);
				//printk("reusing node[@%px]... parent @%px (%s:%d)\n", node, parent, __func__, __LINE__);
			}
		} else {
			/* @node is leaf node and entry is pointer or value. */
			break;
		}
descend:
		entry = cc_xas_descend(xas, node);
		slot = &node->slots[xas->xa_offset];
		parent = node;
	}

	return entry;
}

/**
 * cc_xas_create_range() - Ensure that stores to this range will succeed
 * @xas: XArray operation state.
 *
 * Creates all of the slots in the range covered by @xas.  Sets @xas to
 * create single-index entries and positions it at the beginning of the
 * range.  This is for the benefit of users which have not yet been
 * converted to use multi-index entries.
 */
void cc_xas_create_range(struct cc_xa_state *xas)
{
	unsigned long index = xas->xa_index;
	unsigned char shift = xas->xa_shift;
	unsigned char sibs = xas->xa_sibs;

	xas->xa_index |= ((sibs + 1) << shift) - 1;
	if (cc_xas_is_node(xas) && xas->xa_node->shift == xas->xa_shift)
		xas->xa_offset |= sibs;
	xas->xa_shift = 0;
	xas->xa_sibs = 0;

	for (;;) {
		cc_xas_create(xas, true);
		cc_xas_rewind_refcnt(xas);
		if (cc_xas_error(xas))
			goto restore;
		if (xas->xa_index <= (index | CC_XA_CHUNK_MASK))
			goto success;
		xas->xa_index -= CC_XA_CHUNK_SIZE;

		for (;;) {
			struct cc_xa_node *node = xas->xa_node;
			//xas->xa_node = cc_xa_parent_locked(xas->xa, node);
			cc_xas_set_xa_node(xas, cc_xa_parent_locked(xas->xa, node));
			xas->xa_offset = node->offset - 1;
			if (node->offset != 0)
				break;
		}
	}

restore:
	xas->xa_shift = shift;
	xas->xa_sibs = sibs;
	xas->xa_index = index;
	return;
success:
	xas->xa_index = index;
	if (xas->xa_node)
		cc_xas_set_offset(xas);
}
//EXPORT_SYMBOL_GPL(cc_xas_create_range);

static void update_node(struct cc_xa_state *xas, struct cc_xa_node *node,
		int count, int values)
{
	int node_count, nr_values;
	struct cc_xa_node *parent;

	if (!node || (!count && !values))
		return;

	CC_XA_NODE_BUG_ON(node, !__atomic_load_n(&node->refcnt, __ATOMIC_SEQ_CST));

	/* Wait for other thread */
	while (__atomic_load_n(&node->gc_flag, __ATOMIC_SEQ_CST))
		cpu_relax();

	// node->count += count;
	node_count = __sync_add_and_fetch(&node->count, count);
	// node->nr_values += values;
	nr_values = __sync_add_and_fetch(&node->nr_values, values);


	if (node_count > 0) {
		if (__sync_bool_compare_and_swap(&node->del, 1, 0)) {
			// if the node is marked as deleted, reuse it.
			parent = cc_xa_parent(xas->xa, node);
			if (parent)
				__sync_add_and_fetch(&parent->count, 1);
			//printk("reusing node[@%px]... parent @%px (%s:%d)\n", node, parent, __func__, __LINE__);
		}
	}

	/*
	 * Slot CASes and these counter updates are separate steps, so a
	 * concurrent update of the same node may be applied first.  Each slot
	 * contributes at most one unapplied change, so a counter can
	 * transiently reach 2 * CC_XA_CHUNK_SIZE.  Values near U8_MAX are
	 * transient underflow, as before.
	 */
	CC_XA_NODE_BUG_ON(node, (node_count > 2 * CC_XA_CHUNK_SIZE) &&
				(node_count < (U8_MAX - CC_XA_CHUNK_SIZE)));
	CC_XA_NODE_BUG_ON(node, (nr_values > 2 * CC_XA_CHUNK_SIZE) &&
				(nr_values < (U8_MAX - CC_XA_CHUNK_SIZE)));
	cc_xas_update(xas, node);
	if (node_count <= 0 && count < 0)
		cc_xas_delete_node(xas);
}

static bool cc_xas_larger_entry(struct cc_xa_state *xas,
				struct cc_xa_node *node, unsigned int max)
{
	return max < CC_XA_CHUNK_MASK &&
	       cc_xa_entry_locked(xas->xa, node, max + 1) ==
	       cc_xa_mk_sibling(xas->xa_offset);
}

/*
 * Store the value @entry in every slot that @xas covers in @node.
 *
 * A value has no owner: any writer that expects it may replace or remove it.
 * A writer working on the sibling slots of a multi-index value could not tell
 * that the canonical slot changed hands meanwhile, so a value is never stored
 * as a multi-index entry.  Each slot of the range holds @entry itself.
 *
 * With @whole, the canonical slot holds an entry of this range that the
 * caller owns, as eviction of a folio does.  Its siblings are replaced last to
 * first and the canonical slot at the end, so the siblings that remain still
 * refer to that entry.
 *
 * Otherwise the canonical slot is written first.  If a slot cannot be taken,
 * the slots that still hold @entry are emptied again; a slot that another
 * writer took over meanwhile is left to it.
 */
static bool cc_xas_store_values(struct cc_xa_state *xas,
				struct cc_xa_node *node, void **firstp,
				void *entry, bool expect, bool whole,
				int *count, int *values)
{
	unsigned int offset = xas->xa_offset;
	unsigned int max = offset + xas->xa_sibs;
	void *sibling = cc_xa_mk_sibling(offset);
	void *first = *firstp, *curr;
	unsigned long taken = 0, empty = 0, value = 0;
	unsigned int i;

	if (whole) {
		for (i = max; i > offset; i--) {
			if (__sync_val_compare_and_swap(&node->slots[i], sibling,
							entry) == sibling)
				*values += !cc_xa_is_value(first);
		}
		curr = __sync_val_compare_and_swap(&node->slots[offset], first,
						   entry);
		if (curr != first) {
			*firstp = curr;
			return false;
		}
		*values += !cc_xa_is_value(first);
		return true;
	}

	curr = __sync_val_compare_and_swap(&node->slots[offset], first, entry);
	if (curr != first) {
		*firstp = curr;
		return false;
	}
	if (cc_xa_is_node(first) && node->shift)
		cc_xas_free_nodes(xas, cc_xa_to_node(first));
	*count += !first;
	*values += !cc_xa_is_value(first);

	for (i = offset + 1; i <= max; i++) {
		curr = cc_xa_entry_locked(xas->xa, node, i);
		if (curr == entry)
			continue;
		if (expect && curr && !cc_xa_is_value(curr))
			goto undo;
		if (__sync_val_compare_and_swap(&node->slots[i], curr,
						entry) != curr)
			goto undo;
		taken |= 1UL << i;
		if (!curr)
			empty |= 1UL << i;
		if (cc_xa_is_value(curr))
			value |= 1UL << i;
		if (cc_xa_is_node(curr) && node->shift)
			cc_xas_free_nodes(xas, cc_xa_to_node(curr));
		*count += !curr;
		*values += !cc_xa_is_value(curr);
	}
	return true;

undo:
	*firstp = curr;
	*count = 0;
	*values = 0;
	while (--i > offset) {
		if (!(taken & (1UL << i)))
			continue;
		if (__sync_val_compare_and_swap(&node->slots[i], entry,
						NULL) == entry) {
			*count -= !(empty & (1UL << i));
			*values -= !!(value & (1UL << i));
		} else {
			*count += !!(empty & (1UL << i));
			*values += !(value & (1UL << i));
		}
	}
	if (__sync_val_compare_and_swap(&node->slots[offset], entry,
					first) != entry) {
		*count += !first;
		*values += !cc_xa_is_value(first);
	}
	return false;
}

/*
 * Store @entry over the slots that @xas covers in @node.
 *
 * The canonical slot is written first and every sibling slot refers to it, so
 * a sibling never refers to a slot that does not hold @entry.  If a slot
 * cannot be taken, the slots already written are released in reverse order
 * and nothing of @entry remains.  Shadow values displaced before the failure
 * are dropped rather than restored.  A value @entry is stored by
 * cc_xas_store_values() instead.
 *
 * With @expect, a sibling slot is only taken when it is empty or holds a
 * value.  On failure *@firstp is the entry that was in the way.
 */
static __always_inline bool
cc_xas_store_slots(struct cc_xa_state *xas, struct cc_xa_node *node,
		   void **firstp, void *entry, bool expect, int *count,
		   int *values)
{
	unsigned int offset = xas->xa_offset;
	unsigned int max = offset + xas->xa_sibs;
	void *sibling = cc_xa_mk_sibling(offset);
	void *first = *firstp, *prev = first, *curr;
	bool value = cc_xa_is_value(entry);
	unsigned long taken = 0;
	int dropped = 0, dropped_values = 0;
	bool whole = first && max > offset;
	unsigned int i;

	BUILD_BUG_ON(CC_XA_CHUNK_SIZE > BITS_PER_LONG);

	/* Does the entry in the canonical slot already cover this range? */
	for (i = offset + 1; whole && i <= max; i++)
		whole = cc_xa_entry_locked(xas->xa, node, i) == sibling;

	if (value && max > offset)
		return cc_xas_store_values(xas, node, firstp, entry, expect,
					   whole, count, values);

	/*
	 * A sibling past the range means the entry in the canonical slot is
	 * larger than this store.  A caller that expects a certain entry splits
	 * it first; taking the slot would hand it the rest of that range.
	 */
	if (expect && cc_xas_larger_entry(xas, node, max)) {
		*firstp = sibling;
		return false;
	}

	curr = __sync_val_compare_and_swap(&node->slots[offset], first, entry);
	if (curr != first) {
		*firstp = curr;
		return false;
	}
	/* The same entry may have come back larger since the check above. */
	if (expect && cc_xas_larger_entry(xas, node, max)) {
		i = offset + 1;
		curr = sibling;
		goto undo;
	}
	if (cc_xa_is_node(first) && node->shift)
		cc_xas_free_nodes(xas, cc_xa_to_node(first));
	*count += !first;
	*values += !cc_xa_is_value(first) - !value;

	/*
	 * The owner replaced its entry with one of the same range.  The
	 * canonical slot was the whole store.
	 */
	if (whole && !expect) {
		*values += (!cc_xa_is_value(first) - !value) * xas->xa_sibs;
		return true;
	}

	for (i = offset + 1; i <= max; i++) {
		curr = cc_xa_entry_locked(xas->xa, node, i);
		if (!cc_xa_is_sibling(curr))
			prev = curr;
		if (curr != sibling) {
			if (expect && curr && !cc_xa_is_value(curr))
				goto undo;
			if (__sync_val_compare_and_swap(&node->slots[i], curr,
							sibling) != curr)
				goto undo;
			taken |= 1UL << i;
			if (cc_xa_is_node(curr) && node->shift)
				cc_xas_free_nodes(xas, cc_xa_to_node(curr));
			if (curr) {
				dropped++;
				dropped_values += cc_xa_is_value(prev);
			}
		}
		*count += !curr;
		*values += !cc_xa_is_value(prev) - !value;
	}
	return true;

undo:
	*firstp = curr;
	/* Nothing else may write a slot that refers to our canonical slot. */
	while (--i > offset) {
		if (!(taken & (1UL << i)))
			continue;
		curr = __sync_val_compare_and_swap(&node->slots[i], sibling,
						   NULL);
		CC_XA_NODE_BUG_ON(node, curr != sibling);
	}
	curr = __sync_val_compare_and_swap(&node->slots[offset], entry, first);
	CC_XA_NODE_BUG_ON(node, curr != entry);
	*count = -dropped;
	*values = -dropped_values;
	return false;
}

/*
 * Clear the slots that @xas covers in @node, and the sibling slots that
 * follow them.
 *
 * Sibling slots are cleared last-to-first and the canonical slot last, so a
 * sibling never refers to a slot that another writer could have refilled.
 * Only the canonical slot is compared with *@firstp.  The caller owns the
 * entry, so that comparison fails only if this rule is broken; *@firstp then
 * returns the entry found.
 *
 * With @range, every slot of the range of @xas is cleared whatever it holds,
 * as xas_store() does for a NULL entry.  Otherwise the caller named one entry
 * and only that entry goes.
 */
static bool cc_xas_erase_slots(struct cc_xa_state *xas,
			       struct cc_xa_node *node, void **firstp,
			       int *count, int *values, bool range, bool *full)
{
	unsigned int offset = xas->xa_offset;
	unsigned int max = offset + xas->xa_sibs;
	unsigned int last = max;
	void *sibling = cc_xa_mk_sibling(offset);
	void *first = *firstp, *curr;
	bool filled;
	unsigned int i;

	/*
	 * A value is never multi-index.  Siblings that refer to this slot
	 * belong to an entry that replaced the value since it was loaded.
	 */
	if (cc_xa_is_value(first) && !range)
		last = offset;

	while (!cc_xa_is_value(first) && last < CC_XA_CHUNK_MASK &&
	       cc_xa_entry_locked(xas->xa, node, last + 1) == sibling)
		last++;
	filled = first && !offset && last == CC_XA_CHUNK_MASK &&
		 cc_xa_entry_locked(xas->xa, node, last) == sibling;

	for (i = last; i > offset; i--) {
		curr = __sync_val_compare_and_swap(&node->slots[i], sibling,
						   NULL);
		if (curr == sibling) {
			(*count)--;
			*values -= cc_xa_is_value(first);
			continue;
		}
		/*
		 * The slot does not belong to the entry in the canonical slot.
		 * Past the range it is not ours to remove.
		 */
		if (!range || i > max)
			continue;
		while (curr) {
			void *seen = curr;

			curr = __sync_val_compare_and_swap(&node->slots[i],
							   seen, NULL);
			if (curr != seen)
				continue;
			(*count)--;
			*values -= cc_xa_is_value(seen);
			break;
		}
	}

	curr = __sync_val_compare_and_swap(&node->slots[offset], first, NULL);
	if (curr != first) {
		*firstp = curr;
		return false;
	}
	if (cc_xa_is_node(first) && node->shift)
		cc_xas_free_nodes(xas, cc_xa_to_node(first));
	*count -= !!first;
	*values -= cc_xa_is_value(first);
	*full = filled;
	return true;
}

/* Common body of cc_xas_store() and cc_xas_store_expected(). */
enum cc_xa_store_mode {
	CC_XA_STORE_PLAIN,	/* the caller owns what it replaces */
	CC_XA_STORE_EXACT,	/* the canonical slot must hold @expected */
	CC_XA_STORE_VACANT,	/* the canonical slot must be empty or a value */
};

/*
 * Store @entry in the node that the walk ends in.  *@full is set when a
 * delete removed an entry that filled its leaf.
 */
static void cc_xas_init_slot_marks(const struct cc_xa_state *xas);

static __always_inline void *
__cc_xas_store_node(struct cc_xa_state *xas, void *entry,
		    enum cc_xa_store_mode mode, void *expected, bool *full)
{
	bool expect = mode != CC_XA_STORE_PLAIN;
	struct cc_xa_node *node;
	void __rcu **slot = &xas->xa->xa_head;	// slot pointer
	int count = 0;
	int values = 0;
	void *first, *curr;
	bool stored;

	if (entry) {
		// entry is not node and not error -> then the new node could be root
		bool allow_root = !cc_xa_is_node(entry) && !cc_xa_is_zero(entry);
		first = cc_xas_create(xas, allow_root);	// create slot to move pointer in
	} else {
		first = cc_xas_load(xas, false);		// if entry is NULL -> no store?
	}

	if (cc_xas_invalid(xas))
		goto out;
	if (mode == CC_XA_STORE_EXACT ? first != expected :
	    mode == CC_XA_STORE_VACANT && first && !cc_xa_is_value(first)) {
		cc_xas_set_err(xas, -EEXIST);
		goto out;
	}
	node = xas->xa_node;
	/*
	 * The walk followed a sibling, so this index lies inside another
	 * entry.  A caller that does not own that entry must not write to its
	 * canonical slot; it splits the entry first, as upstream does.
	 */
	if (expect && node &&
	    xas->xa_offset != get_offset(xas->xa_index, node)) {
		cc_xas_set_err(xas, -EEXIST);
		goto out;
	}
	if (node && (xas->xa_shift < node->shift))
		xas->xa_sibs = 0;
	if ((first == entry) && !xas->xa_sibs)	// No need to update slot! It's already same!
		goto out;

	if (node) {
		slot = &node->slots[xas->xa_offset];
		if (xas->xa_sibs) {
			cc_xas_squash_marks(xas);
			/* Values are one entry per index and carry no mark. */
			if (cc_xa_is_value(entry))
				cc_xas_init_slot_marks(xas);
		}
	}
	/*
	 * Must clear the marks before setting the entry to NULL, otherwise
	 * cc_xas_for_each_marked may find a NULL entry and stop early.
	 */
	if (!entry)
		cc_xas_init_slot_marks(xas);

	if (!node) {
		curr = __sync_val_compare_and_swap(slot, first, entry);
		if (curr != first) {
			cc_xas_set_err(xas, -EEXIST);
			first = curr;
		} else if (cc_xa_is_node(first)) {	// coming from scan_shadow_nodes()
			cc_xas_free_nodes(xas, cc_xa_to_node(first));
		}
		goto out;
	}

	curr = first;
	if (entry)
		stored = cc_xas_store_slots(xas, node, &curr, entry, expect,
					    &count, &values);
	else
		stored = cc_xas_erase_slots(xas, node, &curr, &count, &values,
					    mode == CC_XA_STORE_PLAIN, full);

	/* The error state replaces the cursor, so account for the slots first. */
	update_node(xas, node, count, values);
	if (!stored) {
		cc_xas_set_err(xas, -EEXIST);
		first = curr;
	}
out:
	cc_xas_rewind_refcnt(xas);
	return first;
}

/* The same out of line, for the stores that go from leaf to leaf. */
static noinline void *cc_xas_store_node(struct cc_xa_state *xas, void *entry,
					enum cc_xa_store_mode mode,
					void *expected, bool *full)
{
	return __cc_xas_store_node(xas, entry, mode, expected, full);
}

/*
 * Undo of a value stored over the leaf at the cursor: empty the slots that
 * still hold it.  A slot that another writer took over is left to it.
 */
static void cc_xas_erase_values(struct cc_xa_state *xas, void *entry)
{
	struct cc_xa_node *node;
	int count = 0, values = 0;
	unsigned int i;

	cc_xas_load(xas, false);
	node = xas->xa_node;
	if (!cc_xas_invalid(xas) && node && !node->shift) {
		for (i = CC_XA_CHUNK_SIZE; i--;) {
			if (__sync_val_compare_and_swap(&node->slots[i], entry,
							NULL) != entry)
				continue;
			count--;
			values--;
		}
		update_node(xas, node, count, values);
	}
	cc_xas_rewind_refcnt(xas);
}

/* Point @xas at the whole leaf that holds @index. */
static void cc_xas_set_leaf(struct cc_xa_state *xas, unsigned long index)
{
	xas->xa_index = index & ~(unsigned long)CC_XA_CHUNK_MASK;
	xas->xa_shift = 0;
	xas->xa_sibs = CC_XA_CHUNK_MASK;
	cc_xas_set_xa_node(xas, CC_XAS_RESTART);
}

/*
 * The entry @first filled the leaf at the cursor and has been removed from
 * it.  Remove it from the leaves that follow.  The same pointer in the next
 * leaf is the same entry, so this needs no order from the caller.
 */
static void cc_xas_erase_leaves(struct cc_xa_state *xas, void *first)
{
	unsigned long index = xas->xa_index;
	unsigned long next = index | CC_XA_CHUNK_MASK;
	unsigned char shift = xas->xa_shift, sibs = xas->xa_sibs;
	bool full = true;

	while (full && ++next) {
		full = false;
		cc_xas_set_leaf(xas, next);
		cc_xas_store_node(xas, NULL, CC_XA_STORE_EXACT, first, &full);
		next |= CC_XA_CHUNK_MASK;
	}
	/* Back to the slot the caller emptied; the last probe found the end. */
	xas->xa_shift = shift;
	xas->xa_sibs = sibs;
	cc_xas_set(xas, index);
	cc_xas_load(xas, true);
}

/*
 * The owner replaced @first with @entry through a state of less than a leaf,
 * and @first filled the leaf at the cursor.  Replace it in the other leaves
 * it fills, as a store to the canonical slot of a smaller entry replaces all
 * of that entry.  The same pointer in the next leaf is the same entry.
 */
static void cc_xas_replace_leaves(struct cc_xa_state *xas, void *first,
				  void *entry)
{
	unsigned long index = xas->xa_index;
	unsigned long leaf = index & ~(unsigned long)CC_XA_CHUNK_MASK;
	unsigned char shift = xas->xa_shift, sibs = xas->xa_sibs;
	unsigned long at;
	bool full;

	for (at = leaf + CC_XA_CHUNK_SIZE;
	     at && cc_xa_leaf_entry_at(xas->xa, at) == first;
	     at += CC_XA_CHUNK_SIZE) {
		cc_xas_set_leaf(xas, at);
		cc_xas_store_node(xas, entry, CC_XA_STORE_PLAIN, NULL, &full);
	}
	for (at = leaf; at && cc_xa_leaf_entry_at(xas->xa, at - 1) == first;) {
		at -= CC_XA_CHUNK_SIZE;
		cc_xas_set_leaf(xas, at);
		cc_xas_store_node(xas, entry, CC_XA_STORE_PLAIN, NULL, &full);
	}
	xas->xa_shift = shift;
	xas->xa_sibs = sibs;
	cc_xas_set(xas, index);
	cc_xas_load(xas, true);
}

/*
 * cc nodes are deleted logically and stay linked to their parent, so a slot
 * of an inner node cannot be given to an entry.  An entry of a leaf or more
 * is stored in the leaves instead: it fills each leaf of its range, with a
 * canonical slot of its own in every leaf.
 *
 * The leaves are filled first to last and emptied again last to first if one
 * of them cannot be taken, so a failed store leaves nothing of @entry.  The
 * first leaf obeys @expect; the others must be empty or hold values.  On
 * success the cursor is at the first leaf and describes that leaf.
 */
static void *__cc_xas_store(struct cc_xa_state *xas, void *entry,
			    bool expect, void *expected)
{
	enum cc_xa_store_mode mode = expect ? CC_XA_STORE_EXACT :
					      CC_XA_STORE_PLAIN;
	unsigned char shift = xas->xa_shift, sibs = xas->xa_sibs;
	unsigned long base, leaves, i;
	void *first = NULL, *curr;
	bool full = false;
	long err;

	if (shift < CC_XA_CHUNK_SHIFT || cc_xas_error(xas)) {
		first = __cc_xas_store_node(xas, entry, mode, expected, &full);
		if (cc_xas_error(xas))
			return first;
		if (full)
			cc_xas_erase_leaves(xas, first);
		else if (entry && !expect && first != entry && !xas->xa_offset &&
			 first && !cc_xa_is_internal(first) &&
			 !cc_xa_is_value(first) &&
			 cc_xa_leaf_entry(xas->xa, xas->xa_node) == entry)
			cc_xas_replace_leaves(xas, first, entry);
		return first;
	}

	base = xas->xa_index & ~(((sibs + 1UL) << shift) - 1);
	leaves = (sibs + 1UL) << (shift - CC_XA_CHUNK_SHIFT);

	if (!entry) {
		/*
		 * A delete that names its entry follows that entry from its
		 * first leaf.  Otherwise every leaf of the range is cleared,
		 * and the rest of an entry that goes on after the last one.
		 */
		for (i = 0; i < leaves; i++) {
			cc_xas_set_leaf(xas, base + i * CC_XA_CHUNK_SIZE);
			curr = cc_xas_store_node(xas, NULL, mode, expected,
						 &full);
			if (!i)
				first = curr;
			if (expect || cc_xas_error(xas))
				break;
		}
		if (cc_xas_error(xas))
			return first;
		if (full)
			cc_xas_erase_leaves(xas, curr);
		goto done;
	}

	/*
	 * Create the leaves first.  Nodes stay once they are linked, so the
	 * stores below find every node: like upstream xas_store(), this can
	 * only fail for memory before it has changed a slot.
	 */
	for (i = 1; i < leaves; i++) {
		cc_xas_set_leaf(xas, base + i * CC_XA_CHUNK_SIZE);
		cc_xas_create(xas, true);
		cc_xas_rewind_refcnt(xas);
		if (cc_xas_error(xas)) {
			err = cc_xas_error(xas);
			goto fail;
		}
	}

	for (i = 0; i < leaves; i++) {
		cc_xas_set_leaf(xas, base + i * CC_XA_CHUNK_SIZE);
		curr = cc_xas_store_node(xas, entry, mode, expected, &full);
		if (!i)
			first = curr;
		if (cc_xas_error(xas))
			break;
		if (expect)
			mode = CC_XA_STORE_VACANT;
	}
	if (i == leaves) {
done:
		/* The state keeps its range; the cursor is at its start. */
		cc_xas_set_leaf(xas, base);
		cc_xas_load(xas, true);
		xas->xa_shift = shift;
		xas->xa_sibs = sibs;
		return first;
	}

	err = cc_xas_error(xas);
	while (i--) {
		cc_xas_set_leaf(xas, base + i * CC_XA_CHUNK_SIZE);
		if (cc_xa_is_value(entry))
			cc_xas_erase_values(xas, entry);
		else
			cc_xas_store_node(xas, NULL, CC_XA_STORE_EXACT, entry,
					  &full);
	}
fail:
	xas->xa_index = base;
	xas->xa_shift = shift;
	xas->xa_sibs = sibs;
	cc_xas_set_err(xas, err);
	return first;
}

/**
 * cc_xas_store() - Store this entry in the XArray.
 * @xas: XArray operation state.
 * @entry: New entry.
 *
 * If @xas is operating on a multi-index entry, the entry returned by this
 * function is essentially meaningless (it may be an internal entry or it
 * may be %NULL, even if there are non-NULL entries at some of the indices
 * covered by the range).  This is not a problem for any current users,
 * and can be changed if needed.
 *
 * Releases retained traversal references on every return.  The cursor and
 * error state are preserved; callers must release the cursor separately.
 *
 * Return: The old entry at this index.
 */
void *cc_xas_store(struct cc_xa_state *xas, void *entry)
{
	return __cc_xas_store(xas, entry, false, NULL);
}
EXPORT_SYMBOL_GPL(cc_xas_store);

/**
 * cc_xas_store_expected() - Store this entry if the slot still holds @old.
 * @xas: XArray operation state.
 * @old: Entry the caller observed at this index, or %NULL if it saw none.
 * @entry: New entry.
 *
 * Like cc_xas_store(), but the store is bound to the caller's observation:
 * the slot CAS expects @old rather than a value reloaded by the store.  If
 * the canonical slot no longer holds @old, nothing is changed and -EEXIST
 * is recorded in @xas.  Use this where the caller checks the current
 * entry (for example with cc_xas_for_each_conflict()) and then stores
 * without a lock excluding other writers.  A concurrent insertion between
 * the check and the store is then reported instead of silently replaced.
 *
 * Only the canonical slot is compared.  For a multi-index store this does
 * not compare sibling slots or make the multi-slot update atomic.
 *
 * Releases retained traversal references on every return.  The cursor and
 * error state are preserved; callers must release the cursor separately.
 *
 * Return: The entry found at this index (equal to @old on success).
 */
void *cc_xas_store_expected(struct cc_xa_state *xas, void *old, void *entry)
{
	return __cc_xas_store(xas, entry, true, old);
}
EXPORT_SYMBOL_GPL(cc_xas_store_expected);

static bool cc_xas_clear_shadow_slot(struct cc_xa_state *xas,
				     unsigned int offset, void *entry)
{
	struct cc_xa_node *node = xas->xa_node;
	void __rcu **slot;

	if (!cc_xa_is_value(entry))
		return false;
	if (node) {
		if (node_get_mark(node, offset, CC_XA_MARK_0) ||
		    node_get_mark(node, offset, CC_XA_MARK_1) ||
		    node_get_mark(node, offset, CC_XA_MARK_2))
			return false;
		if (offset + 1 < CC_XA_CHUNK_SIZE &&
		    cc_xa_is_sibling(cc_xa_entry(xas->xa, node, offset + 1)))
			return false;
		slot = &node->slots[offset];
	} else {
		if (cc_xa_marked(xas->xa, CC_XA_MARK_0) ||
		    cc_xa_marked(xas->xa, CC_XA_MARK_1) ||
		    cc_xa_marked(xas->xa, CC_XA_MARK_2))
			return false;
		slot = &xas->xa->xa_head;
	}
	return __sync_val_compare_and_swap(slot, entry, NULL) == entry;
}

/**
 * cc_xas_clear_shadow_entry() - Conditionally clear one order-0 shadow.
 * @xas: State loaded from the root with retained traversal references.
 * @entry: Shadow observed by cc_xas_load() before this call.
 *
 * Removes the slot only if it still contains @entry.  Marked and multi-index
 * entries are left intact.  Shadow marks are not modified.  Releases traversal
 * references on every return; the caller must release the cursor separately.
 *
 * Return: Number of shadow slots removed (zero or one).
 */
unsigned int cc_xas_clear_shadow_entry(struct cc_xa_state *xas, void *entry)
{
	struct cc_xa_node *node;
	unsigned int removed = 0;

	if (cc_xas_invalid(xas))
		goto out;
	node = xas->xa_node;
	if (node && node->shift)
		goto out;
	removed = cc_xas_clear_shadow_slot(xas, xas->xa_offset, entry);
	if (removed)
		update_node(xas, node, -1, -1);
out:
	cc_xas_rewind_refcnt(xas);
	return removed;
}

/**
 * cc_xas_clear_shadow_node() - Conditionally clear unmarked single-slot shadows.
 * @xas: State retaining the selected node and its traversal references.
 *
 * Changed, marked and sibling-backed entries are left intact.  Only successful
 * removals contribute to the node counters.  Shadow marks are not modified.
 * Releases traversal references; the caller must release the cursor.
 *
 * Return: Number of shadow slots removed, not physically reclaimed nodes.
 */
unsigned int cc_xas_clear_shadow_node(struct cc_xa_state *xas)
{
	struct cc_xa_node *node = xas->xa_node;
	unsigned int offset;
	int removed = 0;

	for (offset = 0; offset < CC_XA_CHUNK_SIZE; offset++) {
		void *entry = cc_xa_entry(xas->xa, node, offset);

		removed += cc_xas_clear_shadow_slot(xas, offset, entry);
	}

	if (removed)
		update_node(xas, node, -removed, -removed);
	else
		cc_xas_update(xas, node);
	cc_xas_rewind_refcnt(xas);
	return removed;
}

/**
 * cc_xas_get_mark() - Returns the state of this mark.
 * @xas: XArray operation state.
 * @mark: Mark number.
 *
 * Return: true if the mark is set, false if the mark is clear or @xas
 * is in an error state.
 */
bool cc_xas_get_mark(const struct cc_xa_state *xas, cc_xa_mark_t mark)
{
	if (cc_xas_invalid(xas))
		return false;
	if (!xas->xa_node)
		return cc_xa_marked(xas->xa, mark);
	return node_get_mark(xas->xa_node, xas->xa_offset, mark);
}
//EXPORT_SYMBOL_GPL(cc_xas_get_mark);

/**
 * cc_xas_set_mark() - Sets the mark on this entry and its parents.
 * @xas: XArray operation state.
 * @mark: Mark number.
 *
 * Sets the specified mark on this entry, and walks up the tree setting it
 * on all the ancestor entries.  Does nothing if @xas has not been walked to
 * an entry, or is in an error state.
 */
static __always_inline void cc_node_set_mark(struct cc_xarray *xa,
		struct cc_xa_node *node, unsigned int offset, cc_xa_mark_t mark)
{
	while (node) {
		if (node_set_mark(node, offset, mark))
			return;
		offset = node->offset;
		node = cc_xa_parent_locked(xa, node);
	}

	if (!cc_xa_marked(xa, mark))
		cc_xa_mark_set(xa, mark);
}

static __always_inline void cc_node_clear_mark(struct cc_xarray *xa,
		struct cc_xa_node *node, unsigned int offset, cc_xa_mark_t mark)
{
	while (node) {
		if (!node_clear_mark(node, offset, mark))
			return;
		if (node_any_mark(node, mark))
			return;

		offset = node->offset;
		node = cc_xa_parent_locked(xa, node);
	}

	if (cc_xa_marked(xa, mark))
		cc_xa_mark_clear(xa, mark);
}

/*
 * An entry of more than one leaf has a canonical slot in each leaf, and a
 * search that starts inside the entry only looks at the leaves from there
 * on.  So every leaf carries the marks of the entry.
 */
static noinline void cc_xas_mark_leaves(const struct cc_xa_state *xas,
					void *entry, cc_xa_mark_t mark,
					bool set)
{
	struct cc_xarray *xa = xas->xa;
	unsigned long first, last, index;

	cc_xa_leaf_run(xa, xas->xa_index, entry, &first, &last);
	for (index = first; ; index += CC_XA_CHUNK_SIZE) {
		if (index != (xas->xa_index & ~(unsigned long)CC_XA_CHUNK_MASK)) {
			CC_XA_STATE(leaf, xa, index);

			cc_xas_load(&leaf, true);
			if (cc_xa_leaf_entry(xa, leaf.xa_node) != entry)
				;
			else if (set)
				cc_node_set_mark(xa, leaf.xa_node, 0, mark);
			else
				cc_node_clear_mark(xa, leaf.xa_node, 0, mark);
			cc_xas_clear_xa_node(&leaf);
		}
		if (index == last)
			break;
	}
}

/* Kept in the callers: an entry of less than a leaf ends here. */
static __always_inline void cc_xas_mark(const struct cc_xa_state *xas,
					cc_xa_mark_t mark, bool set)
{
	struct cc_xarray *xa = xas->xa;
	void *entry;

	if (cc_xas_invalid(xas))
		return;

	if (set)
		cc_node_set_mark(xa, xas->xa_node, xas->xa_offset, mark);
	else
		cc_node_clear_mark(xa, xas->xa_node, xas->xa_offset, mark);

	if (xas->xa_offset)
		return;
	entry = cc_xa_leaf_entry(xa, xas->xa_node);
	if (entry)
		cc_xas_mark_leaves(xas, entry, mark, set);
}

void cc_xas_set_mark(const struct cc_xa_state *xas, cc_xa_mark_t mark)
{
	cc_xas_mark(xas, mark, true);
}
EXPORT_SYMBOL_GPL(cc_xas_set_mark);

/**
 * cc_xas_clear_mark() - Clears the mark on this entry and its parents.
 * @xas: XArray operation state.
 * @mark: Mark number.
 *
 * Clears the specified mark on this entry, and walks back to the head
 * attempting to clear it on all the ancestor entries.  Does nothing if
 * @xas has not been walked to an entry, or is in an error state.
 */
void cc_xas_clear_mark(const struct cc_xa_state *xas, cc_xa_mark_t mark)
{
	cc_xas_mark(xas, mark, false);
}
//EXPORT_SYMBOL_GPL(cc_xas_clear_mark);

/**
 * cc_xas_init_marks() - Initialise all marks for the entry
 * @xas: Array operations state.
 *
 * Initialise all marks for the entry specified by @xas.  If we're tracking
 * free entries with a mark, we need to set it on all entries.  All other
 * marks are cleared.
 *
 * This implementation is not as efficient as it could be; we may walk
 * up the tree multiple times.
 */
void cc_xas_init_marks(const struct cc_xa_state *xas)
{
	cc_xa_mark_t mark = 0;

	for (;;) {
		if (cc_xa_track_free(xas->xa) && mark == CC_XA_FREE_MARK)
			cc_xas_set_mark(xas, mark);
		else
			cc_xas_clear_mark(xas, mark);
		if (mark == CC_XA_MARK_MAX)
			break;
		mark_inc(mark);
	}
}
EXPORT_SYMBOL_GPL(cc_xas_init_marks);

/* cc_xas_init_marks() for the slot of the cursor alone. */
static void cc_xas_init_slot_marks(const struct cc_xa_state *xas)
{
	cc_xa_mark_t mark = 0;

	if (cc_xas_invalid(xas))
		return;

	for (;;) {
		if (cc_xa_track_free(xas->xa) && mark == CC_XA_FREE_MARK)
			cc_node_set_mark(xas->xa, xas->xa_node,
					 xas->xa_offset, mark);
		else
			cc_node_clear_mark(xas->xa, xas->xa_node,
					   xas->xa_offset, mark);
		if (mark == CC_XA_MARK_MAX)
			break;
		mark_inc(mark);
	}
}

#ifdef CONFIG_XARRAY_MULTI
static unsigned int node_get_marks(struct cc_xa_node *node, unsigned int offset)
{
	unsigned int marks = 0;
	cc_xa_mark_t mark = CC_XA_MARK_0;

	for (;;) {
		if (node_get_mark(node, offset, mark))
			marks |= 1 << (__force unsigned int)mark;
		if (mark == CC_XA_MARK_MAX)
			break;
		mark_inc(mark);
	}

	return marks;
}

static inline void node_mark_slots(struct cc_xa_node *node, unsigned int sibs,
		cc_xa_mark_t mark)
{
	unsigned int i;

	if (sibs == 0)
		node_mark_all(node, mark);
	else {
		for (i = 0; i < CC_XA_CHUNK_SIZE; i += sibs + 1)
			node_set_mark(node, i, mark);
	}
}

static void node_set_marks(struct cc_xa_node *node, unsigned int offset,
			struct cc_xa_node *child, unsigned int sibs,
			unsigned int marks)
{
	cc_xa_mark_t mark = CC_XA_MARK_0;

	for (;;) {
		if (marks & (1 << (__force unsigned int)mark)) {
			node_set_mark(node, offset, mark);
			if (child)
				node_mark_slots(child, sibs, mark);
		}
		if (mark == CC_XA_MARK_MAX)
			break;
		mark_inc(mark);
	}
}

/*
 * Split the entry @curr at the cursor, which covers @sibs further slots of
 * @node, into entries of the order in @xas that hold @entry.
 *
 * Pieces are taken from the last to the first, the canonical slot of a piece
 * before its siblings, so a sibling always refers to a slot that holds its
 * entry.  Every slot is taken from the exact value that made it part of
 * @curr.  A slot that holds anything else has left the entry (another split,
 * a delete or a store got there first) and is skipped.
 *
 * Return: false if the canonical slot no longer held @curr.  Pieces split off
 * before that stay; they hold @entry.
 */
static bool cc_xas_split_slots(struct cc_xa_state *xas,
			       struct cc_xa_node *node, void *curr,
			       void *entry, unsigned int sibs,
			       unsigned int marks, int *values)
{
	unsigned int first = xas->xa_offset;
	unsigned int canon = first + sibs - xas->xa_sibs;
	void *old = cc_xa_mk_sibling(first);
	int delta = cc_xa_is_value(entry) - cc_xa_is_value(curr);
	unsigned int i;

	/* A value is never multi-index, so there is nothing to split. */
	if (cc_xa_is_value(curr))
		return true;

	for (;; canon -= xas->xa_sibs + 1) {
		void *expect = canon == first ? curr : old;

		if (__sync_val_compare_and_swap(&node->slots[canon], expect,
						entry) != expect) {
			if (canon == first)
				return false;
			continue;
		}
		node_set_marks(node, canon, NULL, 0, marks);
		*values += delta;
		for (i = canon + xas->xa_sibs; i > canon; i--) {
			if (__sync_val_compare_and_swap(&node->slots[i], old,
					cc_xa_mk_sibling(canon)) == old)
				*values += delta;
		}
		if (canon == first)
			return true;
	}
}

/**
 * cc_xas_try_split_min_order() - Calculate the minimum target split order.
 * @order: Current entry order.
 *
 * Calculate the minimum target order for a single try_split operation
 * requiring at most one new node.  At a chunk boundary, only one order
 * can be removed; otherwise, splitting down to the chunk boundary needs
 * no new nodes.  This is a pure calculation: it neither allocates memory
 * nor splits an entry or changes the XArray.
 *
 * Context: Any context.
 * Return: The minimum target order, or 0 if @order is 0.
 */
unsigned int cc_xas_try_split_min_order(unsigned int order)
{
	if (order % CC_XA_CHUNK_SHIFT == 0)
		return order == 0 ? 0 : order - 1;

	return order - (order % CC_XA_CHUNK_SHIFT);
}
EXPORT_SYMBOL_GPL(cc_xas_try_split_min_order);

/**
 * cc_xas_split_alloc() - Allocate memory for splitting an entry.
 * @xas: XArray operation state.
 * @entry: New entry which will be stored in the array.
 * @order: Current entry order.
 * @gfp: Memory allocation flags.
 *
 * A cc entry lives in leaf nodes only, so a split never needs a new node and
 * this function has nothing to allocate.  It exists so that a caller copied
 * from the ordinary XArray keeps its shape.
 *
 * Context: Any context.
 */
void cc_xas_split_alloc(struct cc_xa_state *xas, void *entry, unsigned int order,
		gfp_t gfp)
{
}
EXPORT_SYMBOL_GPL(cc_xas_split_alloc);

/*
 * Split the entry at the cursor index within its leaf.  @sibs is the number
 * of further slots that the entry covers in that leaf.
 */
static void cc_xas_split_leaf(struct cc_xa_state *xas, void *entry,
			      unsigned int sibs)
{
	void *curr = cc_xas_load(xas, false);
	struct cc_xa_node *node = xas->xa_node;
	unsigned int marks;
	int values = 0;
	bool done;

	if (cc_xas_top(node)) {
		cc_xas_rewind_refcnt(xas);
		return;
	}

	/*
	 * Without a lock the entry loaded here need not be the one the
	 * caller looked at.  Split only what the caller names.
	 */
	done = curr == entry;
	if (done) {
		marks = node_get_marks(node, xas->xa_offset);
		done = cc_xas_split_slots(xas, node, curr, entry, sibs, marks,
					  &values);
		__sync_fetch_and_add(&node->nr_values, values);
		cc_xas_update(xas, node);
	}
	cc_xas_rewind_refcnt(xas);
	if (!done) {
		cc_xas_destroy(xas);
		cc_xas_set_err(xas, -EEXIST);
	}
}

/**
 * cc_xas_split() - Split a multi-index entry into smaller entries.
 * @xas: XArray operation state.
 * @entry: The entry to split, which is also stored in the smaller entries.
 * @order: Current entry order.
 *
 * The size of the new entries is set in @xas.  Every slot is changed with a
 * CAS from the value that made it part of the entry, so the split does not
 * overwrite another writer.  -EEXIST is recorded if a canonical slot does
 * not hold @entry.  The caller must own the entry, as the holder of a folio
 * lock does.
 *
 * An entry of a leaf or more is one entry per leaf.  Splitting it into
 * pieces of a leaf or more changes nothing; otherwise each of its leaves is
 * split.
 *
 * Context: Any context.
 */
void cc_xas_split(struct cc_xa_state *xas, void *entry, unsigned int order)
{
	unsigned long index = xas->xa_index;
	unsigned long base, leaves, i;

	if (xas->xa_shift >= CC_XA_CHUNK_SHIFT) {
		cc_xas_rewind_refcnt(xas);
		return;
	}
	if (order < CC_XA_CHUNK_SHIFT) {
		cc_xas_split_leaf(xas, entry, (1U << order) - 1);
		return;
	}

	base = index & ~((1UL << order) - 1);
	leaves = 1UL << (order - CC_XA_CHUNK_SHIFT);
	for (i = 0; i < leaves; i++) {
		cc_xas_set(xas, base + i * CC_XA_CHUNK_SIZE);
		cc_xas_split_leaf(xas, entry, CC_XA_CHUNK_MASK);
		if (cc_xas_error(xas))
			return;
	}
	cc_xas_set(xas, index);
}
EXPORT_SYMBOL_GPL(cc_xas_split);

/**
 * cc_xas_try_split() - Try to split a multi-index entry.
 * @xas: XArray operation state.
 * @entry: The entry to split, which is also stored in the smaller entries.
 * @order: Current entry order.
 *
 * The size of the new entries is set in @xas.  Only the piece that holds the
 * index of @xas is split, as cc_xas_try_split_min_order() steps expect.
 *
 * Every slot is changed with a CAS from the value that made it part of the
 * entry, so the split does not overwrite another writer.  -EEXIST is
 * recorded if the canonical slot does not hold @entry: the split replaces an
 * entry with smaller entries of the same value, and without a lock that is
 * the only way to name the entry meant.  The caller must own the entry, as
 * the holder of a folio lock does.  Values are never multi-index and need no
 * split.
 *
 * An entry of a leaf or more is one entry per leaf, so a target order of a
 * leaf or more changes nothing.  -EINVAL is recorded for an entry larger
 * than a leaf with a smaller target; step down with
 * cc_xas_try_split_min_order() or use cc_xas_split().  A cc split never
 * allocates.  Errors free an unpublished allocation chain; all exits release
 * traversal references.
 *
 * Context: Any context.
 */
void cc_xas_try_split(struct cc_xa_state *xas, void *entry, unsigned int order)
{
	if (xas->xa_shift >= CC_XA_CHUNK_SHIFT) {
		cc_xas_rewind_refcnt(xas);
		return;
	}
	if (order > CC_XA_CHUNK_SHIFT) {
		cc_xas_rewind_refcnt(xas);
		cc_xas_destroy(xas);
		cc_xas_set_err(xas, -EINVAL);
		return;
	}
	cc_xas_split_leaf(xas, entry, order == CC_XA_CHUNK_SHIFT ?
			  CC_XA_CHUNK_MASK : (1U << order) - 1);
}
EXPORT_SYMBOL_GPL(cc_xas_try_split);
#endif

/**
 * cc_xas_pause() - Pause a walk to drop a lock.
 * @xas: XArray operation state.
 *
 * Some users need to pause a walk and drop the lock they're holding in
 * order to yield to a higher priority thread or carry out an operation
 * on an entry.  Those users should call this function before they drop
 * the lock.  It resets the @xas to be suitable for the next iteration
 * of the loop after the user has reacquired the lock.  If most entries
 * found during a walk require you to call cc_xas_pause(), the cc_xa_for_each()
 * iterator may be more appropriate.
 *
 * Note that cc_xas_pause() only works for forward iteration.  If a user needs
 * to pause a reverse iteration, we will need a cc_xas_pause_rev().
 */
void cc_xas_pause(struct cc_xa_state *xas)
{
	struct cc_xa_node *node = xas->xa_node;
	void *leaf;

	if (cc_xas_invalid(xas))
		return;

	leaf = cc_xa_leaf_entry(xas->xa, node);
	//xas->xa_node = CC_XAS_RESTART;
	cc_xas_set_xa_node(xas, CC_XAS_RESTART);
	if (node) {
		unsigned long offset = xas->xa_offset;
		while (++offset < CC_XA_CHUNK_SIZE) {
			if (!cc_xa_is_sibling(cc_xa_entry(xas->xa, node, offset)))
				break;
		}
		/* The walk may have started at an index inside the entry. */
		xas->xa_index &= ~0UL << node->shift;
		xas->xa_index &= ~((unsigned long)CC_XA_CHUNK_MASK << node->shift);
		xas->xa_index += offset << node->shift;
		/* Resume after the last leaf of an entry of several leaves. */
		while (leaf && xas->xa_index &&
		       cc_xa_leaf_entry_at(xas->xa, xas->xa_index) == leaf)
			xas->xa_index += CC_XA_CHUNK_SIZE;
		if (xas->xa_index == 0)
			//xas->xa_node = CC_XAS_BOUNDS;
			cc_xas_set_xa_node(xas, CC_XAS_BOUNDS);
	} else {
		xas->xa_index++;
	}
}
EXPORT_SYMBOL_GPL(cc_xas_pause);

/*
 * __cc_xas_prev() - Find the previous entry in the XArray.
 * @xas: XArray operation state.
 *
 * Helper function for cc_xas_prev() which handles all the complex cases
 * out of line.
 */
void *__cc_xas_prev(struct cc_xa_state *xas)
{
	void *entry;

	if (!cc_xas_frozen(xas->xa_node))
		xas->xa_index--;
	if (!xas->xa_node)
		return set_bounds(xas);
	if (cc_xas_not_node(xas->xa_node))
		return cc_xas_load(xas, true);

	if (xas->xa_offset != get_offset(xas->xa_index, xas->xa_node))
		xas->xa_offset--;

	while (xas->xa_offset == 255) {
		xas->xa_offset = xas->xa_node->offset - 1;
		//xas->xa_node = cc_xa_parent(xas->xa, xas->xa_node);
		cc_xas_set_xa_node(xas, cc_xa_parent(xas->xa, xas->xa_node));
		if (!xas->xa_node)
			return set_bounds(xas);
	}

	for (;;) {
		entry = cc_xa_entry(xas->xa, xas->xa_node, xas->xa_offset);
		if (!cc_xa_is_node(entry))
			return entry;

		//xas->xa_node = cc_xa_to_node(entry);
		cc_xas_set_xa_node(xas, cc_xa_to_node(entry));
		cc_xas_set_offset(xas);
	}
}
EXPORT_SYMBOL_GPL(__cc_xas_prev);

/*
 * __cc_xas_next() - Find the next entry in the XArray.
 * @xas: XArray operation state.
 *
 * Helper function for cc_xas_next() which handles all the complex cases
 * out of line.
 */
void *__cc_xas_next(struct cc_xa_state *xas)
{
	void *entry;

	if (!cc_xas_frozen(xas->xa_node))
		xas->xa_index++;
	if (!xas->xa_node)
		return set_bounds(xas);
	if (cc_xas_not_node(xas->xa_node))
		return cc_xas_load(xas, true);

	if (xas->xa_offset != get_offset(xas->xa_index, xas->xa_node))
		xas->xa_offset++;

	while (xas->xa_offset == CC_XA_CHUNK_SIZE) {
		xas->xa_offset = xas->xa_node->offset + 1;
		//xas->xa_node = cc_xa_parent(xas->xa, xas->xa_node);
		cc_xas_set_xa_node(xas, cc_xa_parent(xas->xa, xas->xa_node));
		if (!xas->xa_node)
			return set_bounds(xas);
	}

	for (;;) {
		entry = cc_xa_entry(xas->xa, xas->xa_node, xas->xa_offset);
		if (!cc_xa_is_node(entry))
			return entry;

		//xas->xa_node = cc_xa_to_node(entry);
		cc_xas_set_xa_node(xas, cc_xa_to_node(entry));
		cc_xas_set_offset(xas);
	}
}
EXPORT_SYMBOL_GPL(__cc_xas_next);

/**
 * cc_xas_find() - Find the next present entry in the XArray.
 * @xas: XArray operation state.
 * @max: Highest index to return.
 *
 * If the @xas has not yet been walked to an entry, return the entry
 * which has an index >= xas.xa_index.  If it has been walked, the entry
 * currently being pointed at has been processed, and so we move to the
 * next entry.
 *
 * If no entry is found and the array is smaller than @max, the iterator
 * is set to the smallest index not yet in the array.  This allows @xas
 * to be immediately passed to cc_xas_store().
 *
 * Return: The entry, if found, otherwise %NULL.
 */
void *cc_xas_find(struct cc_xa_state *xas, unsigned long max)
{
	void *entry, *skip = NULL;
	unsigned long next = 0;

	if (cc_xas_error(xas) || xas->xa_node == CC_XAS_BOUNDS)
		return NULL;
	if (xas->xa_index > max)
		return set_bounds(xas);

	if (!xas->xa_node) {
		xas->xa_index = 1;
		return set_bounds(xas);
	} else if (xas->xa_node == CC_XAS_RESTART) {
		entry = cc_xas_load(xas, false);
		if (entry || cc_xas_not_node(xas->xa_node)) {
			cc_xas_rewind_refcnt(xas);
			return entry;
		}
	} else if (!xas->xa_node->shift) {
		/*
		 * An entry of several leaves has a canonical slot in each of
		 * them; the walk returns it once.
		 */
		skip = cc_xa_leaf_entry(xas->xa, xas->xa_node);
		next = (xas->xa_index | CC_XA_CHUNK_MASK) + 1;
		if (xas->xa_offset != (xas->xa_index & CC_XA_CHUNK_MASK))
			xas->xa_offset = ((xas->xa_index - 1) &
					  CC_XA_CHUNK_MASK) + 1;
	}

	__cc_xas_advance(xas);

	while (xas->xa_node && (xas->xa_index <= max)) {
		if (unlikely(xas->xa_offset == CC_XA_CHUNK_SIZE)) {
			xas->xa_offset = xas->xa_node->offset + 1;
			//xas->xa_node = cc_xa_parent(xas->xa, xas->xa_node);
			cc_xas_set_xa_node(xas, cc_xa_parent(xas->xa, xas->xa_node));
			continue;
		}

		entry = cc_xa_entry(xas->xa, xas->xa_node, xas->xa_offset);
		if (cc_xa_is_node(entry)) {
			//xas->xa_node = cc_xa_to_node(entry);
			cc_xas_set_xa_node(xas, cc_xa_to_node(entry));
			xas->xa_offset = 0;
			continue;
		}
		if (entry && !cc_xa_is_sibling(entry)) {
			if (entry != skip || xas->xa_index != next ||
			    cc_xa_leaf_entry(xas->xa, xas->xa_node) != entry) {
				cc_xas_rewind_refcnt(xas);
				return entry;
			}
			/* The next leaf of the entry just returned. */
			next += CC_XA_CHUNK_SIZE;
			xas->xa_offset = CC_XA_CHUNK_MASK;
		}

		__cc_xas_advance(xas);
	}

	if (!xas->xa_node)
		//xas->xa_node = CC_XAS_BOUNDS;
		cc_xas_set_xa_node(xas, CC_XAS_BOUNDS);
	cc_xas_rewind_refcnt(xas);
	return NULL;
}
EXPORT_SYMBOL_GPL(cc_xas_find);

/**
 * cc_xas_find_marked() - Find the next marked entry in the XArray.
 * @xas: XArray operation state.
 * @max: Highest index to return.
 * @mark: Mark number to search for.
 *
 * If the @xas has not yet been walked to an entry, return the marked entry
 * which has an index >= xas.xa_index.  If it has been walked, the entry
 * currently being pointed at has been processed, and so we return the
 * first marked entry with an index > xas.xa_index.
 *
 * If no marked entry is found and the array is smaller than @max, @xas is
 * set to the bounds state and xas->xa_index is set to the smallest index
 * not yet in the array.  This allows @xas to be immediately passed to
 * cc_xas_store().
 *
 * If no entry is found before @max is reached, @xas is set to the restart
 * state.
 *
 * Return: The entry, if found, otherwise %NULL.
 */
void *cc_xas_find_marked(struct cc_xa_state *xas, unsigned long max, cc_xa_mark_t mark)
{
	bool advance = true;
	unsigned int offset;
	void *entry, *skip = NULL;
	unsigned long next = 0;
	struct cc_xa_node *node;

	if (cc_xas_error(xas))
		return NULL;
	if (xas->xa_index > max)
		goto max;

	if (!xas->xa_node) {
		xas->xa_index = 1;
		goto out;
	} else if (!cc_xas_top(xas->xa_node)) {
		/* As in cc_xas_find(): an entry of several leaves, once. */
		skip = cc_xa_leaf_entry(xas->xa, xas->xa_node);
		next = xas->xa_offset == CC_XA_CHUNK_SIZE ? xas->xa_index :
				(xas->xa_index | CC_XA_CHUNK_MASK) + 1;
	} else {
		advance = false;
		entry = cc_xa_head(xas->xa);
		//xas->xa_node = NULL;
		cc_xas_set_xa_node(xas, NULL);
		if (xas->xa_index > max_index(entry))
			goto out;
		if (!cc_xa_is_node(entry)) {
			if (cc_xa_marked(xas->xa, mark))
				return entry;
			xas->xa_index = 1;
			goto out;
		}

		node = cc_xa_to_node(entry);
		//xas->xa_node = node;
		cc_xas_set_xa_node(xas, node);
		xas->xa_offset = xas->xa_index >> xas->xa_node->shift;
	}

	while (xas->xa_index <= max) {
		if (unlikely(xas->xa_offset == CC_XA_CHUNK_SIZE)) {
			xas->xa_offset = xas->xa_node->offset + 1;
			//xas->xa_node = cc_xa_parent(xas->xa, xas->xa_node);
			cc_xas_set_xa_node(xas, cc_xa_parent(xas->xa, xas->xa_node));
			if (!xas->xa_node)
				break;
			advance = false;
			continue;
		}

		if (!advance) {
			entry = cc_xa_entry(xas->xa, xas->xa_node, xas->xa_offset);
			if (cc_xa_is_sibling(entry)) {
				xas->xa_offset = cc_xa_to_sibling(entry);
				cc_xas_move_index(xas, xas->xa_offset);
			}
		}

		offset = cc_xas_find_chunk(xas, advance, mark);
		if (offset > xas->xa_offset) {
			advance = false;
			cc_xas_move_index(xas, offset);
			/* Mind the wrap */
			if ((xas->xa_index - 1) >= max)
				goto max;
			xas->xa_offset = offset;
			if (offset == CC_XA_CHUNK_SIZE)
				continue;
		}

		entry = cc_xa_entry(xas->xa, xas->xa_node, xas->xa_offset);
		if (!entry && !(cc_xa_track_free(xas->xa) && mark == CC_XA_FREE_MARK))
			continue;
		if (cc_xa_is_sibling(entry))
			continue;
		if (!cc_xa_is_node(entry)) {
			if (entry != skip || xas->xa_index != next ||
			    cc_xa_leaf_entry(xas->xa, xas->xa_node) != entry)
				return entry;
			next += CC_XA_CHUNK_SIZE;
			advance = true;
			continue;
		}
		//xas->xa_node = cc_xa_to_node(entry);
		cc_xas_set_xa_node(xas, cc_xa_to_node(entry));
		cc_xas_set_offset(xas);
	}

out:
	if (xas->xa_index > max)
		goto max;
	return set_bounds(xas);
max:
	//xas->xa_node = CC_XAS_RESTART;
	cc_xas_set_xa_node(xas, CC_XAS_RESTART);
	return NULL;
}
EXPORT_SYMBOL_GPL(cc_xas_find_marked);

/**
 * cc_xas_find_conflict() - Find the next present entry in a range.
 * @xas: XArray operation state.
 *
 * The @xas describes both a range and a position within that range.
 *
 * Context: Any context.  Expects xa_lock to be held.
 * Return: The next entry in the range covered by @xas or %NULL.
 */
void *cc_xas_find_conflict(struct cc_xa_state *xas)
{
	void *curr;

	if (cc_xas_error(xas))
		return NULL;

	if (!xas->xa_node)
		return NULL;

	if (cc_xas_top(xas->xa_node)) {
		curr = cc_xas_start(xas);
		if (!curr)
			return NULL;
		while (cc_xa_is_node(curr)) {
			struct cc_xa_node *node = cc_xa_to_node(curr);
			node = cc_xa_get_node(xas, node);
			/* Wait for other thread */
			while (__atomic_load_n(&node->gc_flag, __ATOMIC_SEQ_CST))
				cpu_relax();
			if (__atomic_load_n(&node->del, __ATOMIC_SEQ_CST)) {
				if (node->shift >= xas->xa_shift) {
					/* @node logically deleted! returning NULL */
					cc_xas_rewind_refcnt(xas);
					return NULL;
				}
				/*
				 * A deleted leaf of a range of several leaves
				 * holds nothing; the leaves after it may.
				 */
				cc_xas_set_xa_node(xas, node);
				xas->xa_offset = CC_XA_CHUNK_MASK;
				curr = NULL;
				break;
			}
			curr = cc_xas_descend(xas, node);
			//cc_xa_put_node(node);
		}
		if (curr) {
			cc_xas_rewind_refcnt(xas);
			return curr;
		}
	}

	if (xas->xa_node->shift > xas->xa_shift) {
		cc_xas_rewind_refcnt(xas);
		return NULL;
	}

	for (;;) {
		if (xas->xa_node->shift == xas->xa_shift) {
			if ((xas->xa_offset & xas->xa_sibs) == xas->xa_sibs)
				break;
		} else if (xas->xa_offset == CC_XA_CHUNK_MASK) {
			xas->xa_offset = xas->xa_node->offset;
			//xas->xa_node = cc_xa_parent_locked(xas->xa, xas->xa_node);
			cc_xas_set_xa_node(xas, cc_xa_parent_locked(xas->xa, xas->xa_node));
			if (!xas->xa_node)
				break;
			continue;
		}
		curr = cc_xa_entry_locked(xas->xa, xas->xa_node, ++xas->xa_offset);
		if (cc_xa_is_sibling(curr))
			continue;
		while (cc_xa_is_node(curr)) {
			//xas->xa_node = cc_xa_to_node(curr);
			cc_xas_set_xa_node(xas, cc_xa_to_node(curr));
			xas->xa_offset = 0;
			curr = cc_xa_entry_locked(xas->xa, xas->xa_node, 0);
		}
		if (curr) {
			cc_xas_rewind_refcnt(xas);
			return curr;
		}
	}
	xas->xa_offset -= xas->xa_sibs;
	cc_xas_rewind_refcnt(xas);
	return NULL;
}
EXPORT_SYMBOL_GPL(cc_xas_find_conflict);

static void cc_xa_garbage_collect_entry(struct cc_xarray *xa, void *entry, unsigned long index, unsigned long shift, int *done)
{
	struct cc_xa_node *node, *parent;
	unsigned long i;
	void *temp;

	if (!cc_xa_is_node(entry))
		return;

	if (shift == 0) {
		//pr_cont("%px\n", entry);
		return;
	}

	node = cc_xa_to_node(entry);	
	if (__atomic_load_n(&node->count, __ATOMIC_SEQ_CST))	// Check if entry count is not zero.
		goto iter_node;

	// Give up if gc_flag is already set. --> Someone else is already doing gc...
	if (!__sync_bool_compare_and_swap(&node->gc_flag, 0, 1))
		goto iter_node;

	if (__atomic_load_n(&node->refcnt, __ATOMIC_SEQ_CST))	// Check if node has users.
		goto clear_gc;

	if (__atomic_load_n(&node->del, __ATOMIC_SEQ_CST)) {
		parent = cc_xa_parent_locked(xa, node);

		if (!parent) {
			xa->xa_head = NULL;
			return;
		}

		if (!__sync_bool_compare_and_swap(&parent->gc_flag, 0, 1))	// wait for other thread
			goto clear_gc;

		if (__atomic_load_n(&parent->refcnt, __ATOMIC_SEQ_CST))	{	// Check if parent node has users.
			CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&parent->gc_flag, 1, 0));
			goto clear_gc;
		}

		temp = parent->slots[node->offset];
		while (!__sync_bool_compare_and_swap(&parent->slots[node->offset], temp, NULL))
			temp = parent->slots[node->offset];

		*done = 0;
		cc_xa_node_free(node);
		CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&parent->gc_flag, 1, 0));
		entry = NULL;
		return;
	}
	//cc_xa_dump_node(node);
clear_gc:
	CC_XA_NODE_BUG_ON(node, !__sync_bool_compare_and_swap(&node->gc_flag, 1, 0));
	
iter_node:
	for (i = 0; i < CC_XA_CHUNK_SIZE; i++){
		cc_xa_garbage_collect_entry(xa, node->slots[i],
		      index + (i << node->shift), node->shift, done);
		if (!cc_xa_head(xa))
			return;
	}
}

/**
 * cc_xa_garbage_collector() - Store this entry in the XArray.
 * @xa: XArray.
 * @index: Index into array.
 * @entry: New entry.
 * @gfp: Meh -u origin masterory allocation flags.
 *
 * Set NUll to entries that have CC_XA_MARK_DEL marks
 */
void cc_xa_garbage_collector(struct cc_xarray *xa)
{
	struct cc_xa_node *node;
	unsigned int shift;
	void *entry;
	int done = 0;
	unsigned long flags;

	BUG_ON(!xa);

	for (;;) {
		entry = cc_xa_head(xa);
		if (done)
			break;
		node = NULL;
		shift = 0;

		if (cc_xa_is_node(entry)) {
			node = cc_xa_to_node(entry);
			shift = node->shift + CC_XA_CHUNK_SHIFT;
		}
		//cc_xa_dump(xa);
		done = 1;
		cc_xa_garbage_collect_entry(xa, entry, 0, shift, &done);
	}

	if (!entry)
		cc_xa_destroy(xa);
}
EXPORT_SYMBOL(cc_xa_garbage_collector);

static inline void *cc_xa_zero_to_null(void *entry)
{
	return cc_xa_is_zero(entry) ? NULL : entry;
}

/**
 * cc_xa_load() - Load an entry from an XArray.
 * @xa: XArray.
 * @index: index into array.
 *
 * Context: Any context.  Takes and releases the RCU lock.
 * Return: The entry at @index in @xa.
 */
void *cc_xa_load(struct cc_xarray *xa, unsigned long index)
{
	CC_XA_STATE(xas, xa, index);
	void *entry;

	rcu_read_lock();
	do {
		entry = cc_xa_zero_to_null(cc_xas_load(&xas, true));
	} while (cc_xas_retry(&xas, entry));
	rcu_read_unlock();
	cc_xas_clear_xa_node(&xas);
	return entry;
}
EXPORT_SYMBOL(cc_xa_load);

static void *cc_xas_result(struct cc_xa_state *xas, void *curr)
{
	if (cc_xas_error(xas))
		curr = xas->xa_node;
	return curr;
}

/**
 * __cc_xa_erase() - Erase this entry from the XArray while locked.
 * @xa: XArray.
 * @index: Index into array.
 *
 * After this function returns, loading from @index will return %NULL.
 * If the index is part of a multi-index entry, all indices will be erased
 * and none of the entries will be part of a multi-index entry.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.
 * Return: The entry which used to be at this index.
 */
void *__cc_xa_erase(struct cc_xarray *xa, unsigned long index)
{
	CC_XA_STATE(xas, xa, index);
	void *result;

	result = cc_xas_result(&xas,
			cc_xa_zero_to_null(cc_xas_store(&xas, NULL)));
	cc_xas_clear_xa_node(&xas);
	return result;
}
//EXPORT_SYMBOL(__cc_xa_erase);

/**
 * cc_xa_erase() - Erase this entry from the XArray.
 * @xa: XArray.
 * @index: Index of entry.
 *
 * After this function returns, loading from @index will return %NULL.
 * If the index is part of a multi-index entry, all indices will be erased
 * and none of the entries will be part of a multi-index entry.
 *
 * Context: Any context.  Takes and releases the xa_lock.
 * Return: The entry which used to be at this index.
 */
void *cc_xa_erase(struct cc_xarray *xa, unsigned long index)
{
	void *entry;

	cc_xa_lock(xa);
	entry = __cc_xa_erase(xa, index);
	cc_xa_unlock(xa);

	return entry;
}
//EXPORT_SYMBOL(cc_xa_erase);

/**
 * __cc_xa_store() - Store this entry in the XArray.
 * @xa: XArray.
 * @index: Index into array.
 * @entry: New entry.
 * @gfp: Memory allocation flags.
 *
 * You must already be holding the xa_lock when calling this function.
 * It will drop the lock if needed to allocate memory, and then reacquire
 * it afterwards.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.  May
 * release and reacquire xa_lock if @gfp flags permit.
 * Return: The old entry at this index or cc_xa_err() if an error happened.
 */
void *__cc_xa_store(struct cc_xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	CC_XA_STATE(xas, xa, index);
	void *curr, *result;

	if (WARN_ON_ONCE(cc_xa_is_advanced(entry)))
		return CC_XA_ERROR(-EINVAL);
	if (cc_xa_track_free(xa) && !entry)
		entry = CC_XA_ZERO_ENTRY;

	do {
		curr = cc_xas_store(&xas, entry);
		if (cc_xa_track_free(xa))
			cc_xas_clear_mark(&xas, CC_XA_FREE_MARK);
	} while (__cc_xas_nomem(&xas, gfp));

	result = cc_xas_result(&xas, cc_xa_zero_to_null(curr));
	cc_xas_clear_xa_node(&xas);
	return result;
}
//EXPORT_SYMBOL(__cc_xa_store);

/**
 * cc_xa_store() - Store this entry in the XArray.
 * @xa: XArray.
 * @index: Index into array.
 * @entry: New entry.
 * @gfp: Memory allocation flags.
 *
 * After this function returns, loads from this index will return @entry.
 * Storing into an existing multi-index entry updates the entry of every index.
 * The marks associated with @index are unaffected unless @entry is %NULL.
 *
 * Context: Any context.  Takes and releases the xa_lock.
 * May sleep if the @gfp flags permit.
 * Return: The old entry at this index on success, cc_xa_err(-EINVAL) if @entry
 * cannot be stored in an XArray, or cc_xa_err(-ENOMEM) if memory allocation
 * failed.
 */
void *cc_xa_store(struct cc_xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	void *curr;

	cc_xa_lock(xa);
	curr = __cc_xa_store(xa, index, entry, gfp);
	cc_xa_unlock(xa);

	return curr;
}
//EXPORT_SYMBOL(cc_xa_store);

static inline void *__cc_xa_cmpxchg_raw(struct cc_xarray *xa,
		unsigned long index, void *old, void *entry, gfp_t gfp);

/**
 * __cc_xa_cmpxchg() - Conditionally replace an entry in the XArray.
 * @xa: XArray.
 * @index: Index into array.
 * @old: Old value to test against.
 * @entry: New value to place in array.
 * @gfp: Memory allocation flags.
 *
 * You must already be holding the xa_lock when calling this function.
 * It will drop the lock if needed to allocate memory, and then reacquire
 * it afterwards.
 *
 * If the entry at @index is the same as @old, replace it with @entry.
 * If the return value is equal to @old, then the exchange was successful.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.  May
 * release and reacquire xa_lock if @gfp flags permit.
 * Return: The old entry at this index or cc_xa_err() if an error happened.
 */
void *__cc_xa_cmpxchg(struct cc_xarray *xa, unsigned long index,
			void *old, void *entry, gfp_t gfp)
{
	return cc_xa_zero_to_null(__cc_xa_cmpxchg_raw(xa, index, old, entry, gfp));
}
//EXPORT_SYMBOL(__cc_xa_cmpxchg);

static inline void *__cc_xa_cmpxchg_raw(struct cc_xarray *xa,
		unsigned long index, void *old, void *entry, gfp_t gfp)
{
	CC_XA_STATE(xas, xa, index);
	void *curr, *result;

	if (WARN_ON_ONCE(cc_xa_is_advanced(entry)))
		return CC_XA_ERROR(-EINVAL);

	do {
		curr = cc_xas_load(&xas, false);
		if (curr == old) {
			cc_xas_store(&xas, entry);
			if (cc_xa_track_free(xa) && entry && !curr)
				cc_xas_clear_mark(&xas, CC_XA_FREE_MARK);
		}
		cc_xas_rewind_refcnt(&xas);
	} while (__cc_xas_nomem(&xas, gfp));

	result = cc_xas_result(&xas, curr);
	cc_xas_clear_xa_node(&xas);
	return result;
}

/**
 * __cc_xa_insert() - Store this entry in the XArray if no entry is present.
 * @xa: XArray.
 * @index: Index into array.
 * @entry: New entry.
 * @gfp: Memory allocation flags.
 *
 * Inserting a NULL entry will store a reserved entry (like cc_xa_reserve())
 * if no entry is present.  Inserting will fail if a reserved entry is
 * present, even though loading from this index will return NULL.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.  May
 * release and reacquire xa_lock if @gfp flags permit.
 * Return: 0 if the store succeeded.  -EBUSY if another entry was present.
 * -ENOMEM if memory could not be allocated.
 */
int __cc_xa_insert(struct cc_xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	void *curr;
	int error;

	if (!entry)
		entry = CC_XA_ZERO_ENTRY;
	curr = __cc_xa_cmpxchg_raw(xa, index, NULL, entry, gfp);
	error = cc_xa_err(curr);
	if (error)
		return error;
	return curr ? -EBUSY : 0;
}
//EXPORT_SYMBOL(__cc_xa_insert);

#ifdef CONFIG_XARRAY_MULTI
static void cc_xas_set_range(struct cc_xa_state *xas, unsigned long first,
		unsigned long last)
{
	unsigned int shift = 0;
	unsigned long sibs = last - first;
	unsigned int offset = CC_XA_CHUNK_MASK;

	cc_xas_set(xas, first);

	while ((first & CC_XA_CHUNK_MASK) == 0) {
		if (sibs < CC_XA_CHUNK_MASK)
			break;
		if ((sibs == CC_XA_CHUNK_MASK) && (offset < CC_XA_CHUNK_MASK))
			break;
		shift += CC_XA_CHUNK_SHIFT;
		if (offset == CC_XA_CHUNK_MASK)
			offset = sibs & CC_XA_CHUNK_MASK;
		sibs >>= CC_XA_CHUNK_SHIFT;
		first >>= CC_XA_CHUNK_SHIFT;
	}

	offset = first & CC_XA_CHUNK_MASK;
	if (offset + sibs > CC_XA_CHUNK_MASK)
		sibs = CC_XA_CHUNK_MASK - offset;
	if ((((first + sibs + 1) << shift) - 1) > last)
		sibs -= 1;

	xas->xa_shift = shift;
	xas->xa_sibs = sibs;
}

/**
 * cc_xa_store_range() - Store this entry at a range of indices in the XArray.
 * @xa: XArray.
 * @first: First index to affect.
 * @last: Last index to affect.
 * @entry: New entry.
 * @gfp: Memory allocation flags.
 *
 * After this function returns, loads from any index between @first and @last,
 * inclusive will return @entry.
 * Storing into an existing multi-index entry updates the entry of every index.
 * The marks associated with @index are unaffected unless @entry is %NULL.
 *
 * Context: Process context.  Takes and releases the xa_lock.  May sleep
 * if the @gfp flags permit.
 * Return: %NULL on success, cc_xa_err(-EINVAL) if @entry cannot be stored in
 * an XArray, or cc_xa_err(-ENOMEM) if memory allocation failed.
 */
void *cc_xa_store_range(struct cc_xarray *xa, unsigned long first,
		unsigned long last, void *entry, gfp_t gfp)
{
	CC_XA_STATE(xas, xa, 0);
	void *result;

	if (WARN_ON_ONCE(cc_xa_is_internal(entry)))
		return CC_XA_ERROR(-EINVAL);
	if (last < first)
		return CC_XA_ERROR(-EINVAL);

	do {
		cc_xas_lock(&xas);
		if (entry) {
			unsigned int order = BITS_PER_LONG;
			if (last + 1)
				order = __ffs(last + 1);
			cc_xas_set_order(&xas, last, order);
			cc_xas_create(&xas, true);
			cc_xas_rewind_refcnt(&xas);
			if (cc_xas_error(&xas))
				goto unlock;
		}
		do {
			cc_xas_set_range(&xas, first, last);
			cc_xas_store(&xas, entry);
			if (cc_xas_error(&xas))
				goto unlock;
			first += cc_xas_size(&xas);
		} while (first <= last);
unlock:
		cc_xas_unlock(&xas);
	} while (cc_xas_nomem(&xas, gfp));

	result = cc_xas_result(&xas, NULL);
	cc_xas_clear_xa_node(&xas);
	return result;
}
//EXPORT_SYMBOL(cc_xa_store_range);

/**
 * cc_xas_get_order() - Get the order of a loaded entry.
 * @xas: XArray operation state.
 *
 * Call after cc_xas_load() has successfully loaded an entry.  The caller
 * must keep @xas valid and free of errors, with xa_offset at the canonical
 * entry rather than a sibling slot.
 *
 * This function does not reload the entry or change the cursor.  For an
 * entry of a leaf or more it also looks at the neighbouring leaves, with
 * walks of its own.
 *
 * Return: The order of the entry, or 0 for an entry stored in the root.
 */
static int cc_node_order(struct cc_xarray *xa, struct cc_xa_node *node,
			 unsigned int offset, unsigned long index)
{
	unsigned long first, last, leaves;
	int order = 0;
	void *entry;

	/*
	 * A reader without the entry's owner lock may find that a larger
	 * entry has taken the slot since it was loaded.
	 */
	entry = cc_xa_entry(xa, node, offset);
	if (cc_xa_is_sibling(entry))
		offset = cc_xa_to_sibling(entry);
	for (;;) {
		unsigned int slot = offset + (1 << order);

		if (slot >= CC_XA_CHUNK_SIZE)
			break;
		if (!cc_xa_is_sibling(cc_xa_entry(xa, node, slot)))
			break;
		order++;
	}

	order += node->shift;
	if (order != CC_XA_CHUNK_SHIFT)
		return order;

	/* An entry of several leaves: count the leaves it fills. */
	entry = cc_xa_leaf_entry(xa, node);
	if (!entry)
		return order;
	cc_xa_leaf_run(xa, index, entry, &first, &last);
	for (leaves = ((last - first) >> CC_XA_CHUNK_SHIFT) + 1; leaves > 1;
	     leaves >>= 1)
		order++;
	return order;
}

int cc_xas_get_order(struct cc_xa_state *xas)
{
	if (!xas->xa_node)
		return 0;

	return cc_node_order(xas->xa, xas->xa_node, xas->xa_offset,
			     xas->xa_index);
}
EXPORT_SYMBOL_GPL(cc_xas_get_order);

/**
 * cc_xa_get_order() - Get the order of an entry.
 * @xa: XArray.
 * @index: Index of the entry.
 *
 * Return: A number between 0 and 63 indicating the order of the entry.
 */
int cc_xa_get_order(struct cc_xarray *xa, unsigned long index)
{
	CC_XA_STATE(xas, xa, index);
	void *entry;
	int order = 0;

	rcu_read_lock();
	entry = cc_xas_load(&xas, true);

	if (!entry)
		goto unlock;

	if (!xas.xa_node)
		goto unlock;

	order = cc_node_order(xa, xas.xa_node, xas.xa_offset, index);
unlock:
	rcu_read_unlock();

	cc_xas_clear_xa_node(&xas);
	return order;
}
EXPORT_SYMBOL(cc_xa_get_order);
#endif /* CONFIG_XARRAY_MULTI */

/**
 * __cc_xa_alloc() - Find somewhere to store this entry in the XArray.
 * @xa: XArray.
 * @id: Pointer to ID.
 * @limit: Range for allocated ID.
 * @entry: New entry.
 * @gfp: Memory allocation flags.
 *
 * Finds an empty entry in @xa between @limit.min and @limit.max,
 * stores the index into the @id pointer, then stores the entry at
 * that index.  A concurrent lookup will not see an uninitialised @id.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.  May
 * release and reacquire xa_lock if @gfp flags permit.
 * Return: 0 on success, -ENOMEM if memory could not be allocated or
 * -EBUSY if there are no free entries in @limit.
 */
int __cc_xa_alloc(struct cc_xarray *xa, u32 *id, void *entry,
		struct xa_limit limit, gfp_t gfp)
{
	CC_XA_STATE(xas, xa, 0);
	int error;

	if (WARN_ON_ONCE(cc_xa_is_advanced(entry)))
		return -EINVAL;
	if (WARN_ON_ONCE(!cc_xa_track_free(xa)))
		return -EINVAL;

	if (!entry)
		entry = CC_XA_ZERO_ENTRY;

	do {
		xas.xa_index = limit.min;
		cc_xas_find_marked(&xas, limit.max, CC_XA_FREE_MARK);
		if (xas.xa_node == CC_XAS_RESTART)
			cc_xas_set_err(&xas, -EBUSY);
		else
			*id = xas.xa_index;
		cc_xas_store(&xas, entry);
		cc_xas_clear_mark(&xas, CC_XA_FREE_MARK);
	} while (__cc_xas_nomem(&xas, gfp));

	error = cc_xas_error(&xas);
	cc_xas_clear_xa_node(&xas);
	return error;
}
//EXPORT_SYMBOL(__xa_alloc);

/**
 * __cc_xa_alloc_cyclic() - Find somewhere to store this entry in the XArray.
 * @xa: XArray.
 * @id: Pointer to ID.
 * @entry: New entry.
 * @limit: Range of allocated ID.
 * @next: Pointer to next ID to allocate.
 * @gfp: Memory allocation flags.
 *
 * Finds an empty entry in @xa between @limit.min and @limit.max,
 * stores the index into the @id pointer, then stores the entry at
 * that index.  A concurrent lookup will not see an uninitialised @id.
 * The search for an empty entry will start at @next and will wrap
 * around if necessary.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.  May
 * release and reacquire xa_lock if @gfp flags permit.
 * Return: 0 if the allocation succeeded without wrapping.  1 if the
 * allocation succeeded after wrapping, -ENOMEM if memory could not be
 * allocated or -EBUSY if there are no free entries in @limit.
 */
int __cc_xa_alloc_cyclic(struct cc_xarray *xa, u32 *id, void *entry,
		struct xa_limit limit, u32 *next, gfp_t gfp)
{
	u32 min = limit.min;
	int ret;

	limit.min = max(min, *next);
	ret = __cc_xa_alloc(xa, id, entry, limit, gfp);
	if ((xa->xa_flags & CC_XA_FLAGS_ALLOC_WRAPPED) && ret == 0) {
		xa->xa_flags &= ~CC_XA_FLAGS_ALLOC_WRAPPED;
		ret = 1;
	}

	if (ret < 0 && limit.min > min) {
		limit.min = min;
		ret = __cc_xa_alloc(xa, id, entry, limit, gfp);
		if (ret == 0)
			ret = 1;
	}

	if (ret >= 0) {
		*next = *id + 1;
		if (*next == 0)
			xa->xa_flags |= CC_XA_FLAGS_ALLOC_WRAPPED;
	}
	return ret;
}
//EXPORT_SYMBOL(__cc_xa_alloc_cyclic);

/**
 * __cc_xa_set_mark() - Set this mark on this entry while locked.
 * @xa: XArray.
 * @index: Index of entry.
 * @mark: Mark number.
 *
 * Attempting to set a mark on a %NULL entry does not succeed.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.
 */
void __cc_xa_set_mark(struct cc_xarray *xa, unsigned long index, cc_xa_mark_t mark)
{
	CC_XA_STATE(xas, xa, index);
	void *entry = cc_xas_load(&xas, true);

	if (entry)
		cc_xas_set_mark(&xas, mark);
	cc_xas_clear_xa_node(&xas);
}
//EXPORT_SYMBOL(__cc_xa_set_mark);

/**
 * __cc_xa_clear_mark() - Clear this mark on this entry while locked.
 * @xa: XArray.
 * @index: Index of entry.
 * @mark: Mark number.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.
 */
void __cc_xa_clear_mark(struct cc_xarray *xa, unsigned long index, cc_xa_mark_t mark)
{
	CC_XA_STATE(xas, xa, index);
	void *entry = cc_xas_load(&xas, true);

	if (entry)
		cc_xas_clear_mark(&xas, mark);
	cc_xas_clear_xa_node(&xas);
}
EXPORT_SYMBOL(__cc_xa_clear_mark);

/**
 * cc_xa_get_mark() - Inquire whether this mark is set on this entry.
 * @xa: XArray.
 * @index: Index of entry.
 * @mark: Mark number.
 *
 * This function uses the RCU read lock, so the result may be out of date
 * by the time it returns.  If you need the result to be stable, use a lock.
 *
 * Context: Any context.  Takes and releases the RCU lock.
 * Return: True if the entry at @index has this mark set, false if it doesn't.
 */
bool cc_xa_get_mark(struct cc_xarray *xa, unsigned long index, cc_xa_mark_t mark)
{
	CC_XA_STATE(xas, xa, index);
	void *entry;

	rcu_read_lock();
	entry = cc_xas_start(&xas);
	while (cc_xas_get_mark(&xas, mark)) {
		if (!cc_xa_is_node(entry))
			goto found;
		entry = cc_xas_descend(&xas, cc_xa_to_node(entry));
	}
	rcu_read_unlock();
	cc_xas_clear_xa_node(&xas);
	return false;
 found:
	rcu_read_unlock();
	cc_xas_clear_xa_node(&xas);
	return true;
}
//EXPORT_SYMBOL(cc_xa_get_mark);

/**
 * cc_xa_set_mark() - Set this mark on this entry.
 * @xa: XArray.
 * @index: Index of entry.
 * @mark: Mark number.
 *
 * Attempting to set a mark on a %NULL entry does not succeed.
 *
 * Context: Process context.  Takes and releases the xa_lock.
 */
void cc_xa_set_mark(struct cc_xarray *xa, unsigned long index, cc_xa_mark_t mark)
{
	cc_xa_lock(xa);
	__cc_xa_set_mark(xa, index, mark);
	cc_xa_unlock(xa);
}
//EXPORT_SYMBOL(cc_xa_set_mark);

/**
 * cc_xa_clear_mark() - Clear this mark on this entry.
 * @xa: XArray.
 * @index: Index of entry.
 * @mark: Mark number.
 *
 * Clearing a mark always succeeds.
 *
 * Context: Process context.  Takes and releases the xa_lock.
 */
void cc_xa_clear_mark(struct cc_xarray *xa, unsigned long index, cc_xa_mark_t mark)
{
	cc_xa_lock(xa);
	__cc_xa_clear_mark(xa, index, mark);
	cc_xa_unlock(xa);
}
//EXPORT_SYMBOL(cc_xa_clear_mark);

/**
 * cc_xa_find() - Search the XArray for an entry.
 * @xa: XArray.
 * @indexp: Pointer to an index.
 * @max: Maximum index to search to.
 * @filter: Selection criterion.
 *
 * Finds the entry in @xa which matches the @filter, and has the lowest
 * index that is at least @indexp and no more than @max.
 * If an entry is found, @indexp is updated to be the index of the entry.
 * This function is protected by the RCU read lock, so it may not find
 * entries which are being simultaneously added.  It will not return an
 * %CC_XA_RETRY_ENTRY; if you need to see retry entries, use cc_xas_find().
 *
 * Context: Any context.  Takes and releases the RCU lock.
 * Return: The entry, if found, otherwise %NULL.
 */
void *cc_xa_find(struct cc_xarray *xa, unsigned long *indexp,
			unsigned long max, cc_xa_mark_t filter)
{
	CC_XA_STATE(xas, xa, *indexp);
	void *entry;

	rcu_read_lock();
	do {
		if ((__force unsigned int)filter < CC_XA_MAX_MARKS)
			entry = cc_xas_find_marked(&xas, max, filter);
		else
			entry = cc_xas_find(&xas, max);
	} while (cc_xas_retry(&xas, entry));
	rcu_read_unlock();

	if (entry)
		*indexp = xas.xa_index;
	cc_xas_clear_xa_node(&xas);
	return entry;
}
//EXPORT_SYMBOL(cc_xa_find);

static bool cc_xas_sibling(struct cc_xa_state *xas)
{
	struct cc_xa_node *node = xas->xa_node;
	unsigned long mask;

	if (!node)
		return false;
	mask = (CC_XA_CHUNK_SIZE << node->shift) - 1;
	return (xas->xa_index & mask) >
		((unsigned long)xas->xa_offset << node->shift);
}

/**
 * cc_xa_find_after() - Search the XArray for a present entry.
 * @xa: XArray.
 * @indexp: Pointer to an index.
 * @max: Maximum index to search to.
 * @filter: Selection criterion.
 *
 * Finds the entry in @xa which matches the @filter and has the lowest
 * index that is above @indexp and no more than @max.
 * If an entry is found, @indexp is updated to be the index of the entry.
 * This function is protected by the RCU read lock, so it may miss entries
 * which are being simultaneously added.  It will not return an
 * %CC_XA_RETRY_ENTRY; if you need to see retry entries, use cc_xas_find().
 *
 * Context: Any context.  Takes and releases the RCU lock.
 * Return: The pointer, if found, otherwise %NULL.
 */
void *cc_xa_find_after(struct cc_xarray *xa, unsigned long *indexp,
			unsigned long max, cc_xa_mark_t filter)
{
	CC_XA_STATE(xas, xa, *indexp + 1);
	void *entry, *inside = NULL;

	if (xas.xa_index == 0)
		return NULL;

	rcu_read_lock();
	/* An entry of several leaves that holds @indexp goes on in the next. */
	if (!(xas.xa_index & CC_XA_CHUNK_MASK))
		inside = cc_xa_leaf_entry_at(xa, *indexp);
	for (;;) {
		if ((__force unsigned int)filter < CC_XA_MAX_MARKS)
			entry = cc_xas_find_marked(&xas, max, filter);
		else
			entry = cc_xas_find(&xas, max);

		if (cc_xas_invalid(&xas))
			break;
		if (cc_xas_sibling(&xas))
			continue;
		/* The entry that holds @indexp, found again at its start. */
		if (entry && xas.xa_index <= *indexp)
			continue;
		if (entry && entry == inside && xas.xa_index == *indexp + 1)
			continue;
		if (!cc_xas_retry(&xas, entry))
			break;
	}
	rcu_read_unlock();

	if (entry)
		*indexp = xas.xa_index;
	cc_xas_clear_xa_node(&xas);
	return entry;
}
//EXPORT_SYMBOL(cc_xa_find_after);

static unsigned int cc_xas_extract_present(struct cc_xa_state *xas, void **dst,
			unsigned long max, unsigned int n)
{
	void *entry;
	unsigned int i = 0;

	rcu_read_lock();
	cc_xas_for_each(xas, entry, max) {
		if (cc_xas_retry(xas, entry))
			continue;
		dst[i++] = entry;
		if (i == n)
			break;
	}
	rcu_read_unlock();

	return i;
}

static unsigned int cc_xas_extract_marked(struct cc_xa_state *xas, void **dst,
			unsigned long max, unsigned int n, cc_xa_mark_t mark)
{
	void *entry;
	unsigned int i = 0;

	rcu_read_lock();
	cc_xas_for_each_marked(xas, entry, max, mark) {
		if (cc_xas_retry(xas, entry))
			continue;
		dst[i++] = entry;
		if (i == n)
			break;
	}
	rcu_read_unlock();

	return i;
}

/**
 * cc_xa_extract() - Copy selected entries from the XArray into a normal array.
 * @xa: The source XArray to copy from.
 * @dst: The buffer to copy entries into.
 * @start: The first index in the XArray eligible to be selected.
 * @max: The last index in the XArray eligible to be selected.
 * @n: The maximum number of entries to copy.
 * @filter: Selection criterion.
 *
 * Copies up to @n entries that match @filter from the XArray.  The
 * copied entries will have indices between @start and @max, inclusive.
 *
 * The @filter may be an XArray mark value, in which case entries which are
 * marked with that mark will be copied.  It may also be %cc_xa_PRESENT, in
 * which case all entries which are not %NULL will be copied.
 *
 * The entries returned may not represent a snapshot of the XArray at a
 * moment in time.  For example, if another thread stores to index 5, then
 * index 10, calling cc_xa_extract() may return the old contents of index 5
 * and the new contents of index 10.  Indices not modified while this
 * function is running will not be skipped.
 *
 * If you need stronger guarantees, holding the xa_lock across calls to this
 * function will prevent concurrent modification.
 *
 * Context: Any context.  Takes and releases the RCU lock.
 * Return: The number of entries copied.
 */
unsigned int cc_xa_extract(struct cc_xarray *xa, void **dst, unsigned long start,
			unsigned long max, unsigned int n, cc_xa_mark_t filter)
{
	CC_XA_STATE(xas, xa, start);

	if (!n)
		return 0;

	if ((__force unsigned int)filter < CC_XA_MAX_MARKS)
		return cc_xas_extract_marked(&xas, dst, max, n, filter);
	return cc_xas_extract_present(&xas, dst, max, n);
}
//EXPORT_SYMBOL(cc_xa_extract);

/**
 * cc_xa_destroy() - Free all internal data structures.
 * @xa: XArray.
 *
 * After calling this function, the XArray is empty and has freed all memory
 * allocated for its internal data structures.  You are responsible for
 * freeing the objects referenced by the XArray.
 *
 * Context: Any context.  Takes and releases the xa_lock, interrupt-safe.
 */
void cc_xa_destroy(struct cc_xarray *xa)
{
	CC_XA_STATE(xas, xa, 0);
	unsigned long flags;
	void *entry;

	//xas.xa_node = NULL;
	cc_xas_set_xa_node(&xas, NULL);
	cc_xas_lock_irqsave(&xas, flags);
	entry = cc_xa_head_locked(xa);
	RCU_INIT_POINTER(xa->xa_head, NULL);
	cc_xas_init_marks(&xas);
	if (cc_xa_zero_busy(xa))
		cc_xa_mark_clear(xa, CC_XA_FREE_MARK);
	/* lockdep checks we're still holding the lock in cc_xas_free_nodes() */
	if (cc_xa_is_node(entry))
		cc_xas_free_nodes(&xas, cc_xa_to_node(entry));
	cc_xas_unlock_irqrestore(&xas, flags);
	cc_xas_clear_xa_node(&xas);
}
//EXPORT_SYMBOL(cc_xa_destroy);

//#ifdef CC_XA_DEBUG
void cc_xa_dump_node(const struct cc_xa_node *node)
{
	unsigned i, j;

	if (!node)
		return;
	if ((unsigned long)node & 3) {
		pr_cont("node %px\n", node);
		return;
	}

	pr_cont("node @%px %s %d parent @%px shift %d count %d values %d "
		"gc_flag %d del %d refcnt %hu "
		"array @%px list @%px @%px marks",
		node, node->parent ? "offset" : "max", node->offset,
		node->parent, node->shift, node->count, node->nr_values,
		node->gc_flag, node->del, node->refcnt,
		node->array, node->private_list.prev, node->private_list.next);
	for (i = 0; i < CC_XA_MAX_MARKS; i++)
		for (j = 0; j < CC_XA_MARK_LONGS; j++)
			pr_cont(" %lx", node->marks[i][j]);
	pr_cont("\n");
}
EXPORT_SYMBOL(cc_xa_dump_node);

static void cc_xa_dump_index(unsigned long index, unsigned int shift)
{
	if (!shift)
		pr_info("%lu: ", index);
	else if (shift >= BITS_PER_LONG)
		pr_info("0-%lu: ", ~0UL);
	else
		pr_info("%lu-%lu: ", index, index | ((1UL << shift) - 1));
}

static void cc_xa_dump_entry(const void *entry, unsigned long index, unsigned long shift)
{
	if (!entry)
		return;

	cc_xa_dump_index(index, shift);

	if (cc_xa_is_node(entry)) {
		if (shift == 0) {
			pr_cont("@%px\n", entry);
		} else {
			unsigned long i;
			struct cc_xa_node *node = cc_xa_to_node(entry);
			cc_xa_dump_node(node);
			for (i = 0; i < CC_XA_CHUNK_SIZE; i++)
				cc_xa_dump_entry(node->slots[i],
				      index + (i << node->shift), node->shift);
		}
	} else if (cc_xa_is_value(entry))
		pr_cont("value %ld (0x%lx) [@%px]\n", cc_xa_to_value(entry),
						cc_xa_to_value(entry), entry);
	else if (!cc_xa_is_internal(entry))
		pr_cont("@%px\n", entry);
	else if (cc_xa_is_retry(entry))
		pr_cont("retry (%ld)\n", cc_xa_to_internal(entry));
	else if (cc_xa_is_sibling(entry))
		pr_cont("sibling (slot %ld)\n", cc_xa_to_sibling(entry));
	else if (cc_xa_is_zero(entry))
		pr_cont("zero (%ld)\n", cc_xa_to_internal(entry));
	else
		pr_cont("UNKNOWN ENTRY (@%px)\n", entry);
}

void cc_xa_dump(const struct cc_xarray *xa)
{
	void *entry = cc_xa_head(xa);
	unsigned int shift = 0;

	pr_info("cc_xarray: @%px head @%px flags %x marks %d %d %d\n", xa, entry,
			xa->xa_flags, cc_xa_marked(xa, CC_XA_MARK_0),
			cc_xa_marked(xa, CC_XA_MARK_1), cc_xa_marked(xa, CC_XA_MARK_2));
	if (cc_xa_is_node(entry))
		shift = cc_xa_to_node(entry)->shift + CC_XA_CHUNK_SHIFT;
	cc_xa_dump_entry(entry, 0, shift);
}
//#endif
