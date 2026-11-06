// SPDX-License-Identifier: GPL-2.0+
/* Native regression of the real CC implementation and shared slab/RCU shim. */
#include "xarray-shared.h"
#include "test.h"
#include "radix-tree.h"
#include <linux/cc_xarray.h>
#include <stddef.h>
#include <string.h>

extern void kmem_cache_set_non_kernel(struct kmem_cache *, unsigned int);
extern unsigned long kmem_cache_nr_allocated(struct kmem_cache *);

static unsigned int updates;

static void updated(struct cc_xa_node *node)
{
	assert(node->nr_values <= node->count);
	updates++;
}

static void release_cursor(struct cc_xa_state *xas)
{
	cc_xas_rewind_refcnt(xas);
	cc_xas_clear_xa_node(xas);
	assert(!xas->nr_refget);
	assert(!xas->trace_root && !xas->trace_leaf);
}

/* Inspect actual allocated nodes, not a parallel implementation. */
static unsigned int check_nodes(struct cc_xa_node *node)
{
	unsigned int i, nodes = 1;

	assert(node->refcnt == 0);
	assert(node->nr_values <= node->count);
	if (node->shift)
		for (i = 0; i < CC_XA_CHUNK_SIZE; i++) {
			void *entry = node->slots[i];

			if (cc_xa_is_node(entry)) {
				struct cc_xa_node *child = cc_xa_to_node(entry);

				assert(child->parent == node);
				assert(child->offset == i);
				nodes += check_nodes(child);
			}
		}
	return nodes;
}

static void check_refs(struct cc_xarray *xa)
{
	if (cc_xa_is_node(xa->xa_head))
		assert(check_nodes(cc_xa_to_node(xa->xa_head)) ==
		       kmem_cache_nr_allocated(radix_tree_node_cachep));
}

static void finish(struct cc_xarray *xa, const char *name)
{
	check_refs(xa);
	cc_xa_destroy(xa);
	rcu_barrier();
	assert(!xa->xa_head);
	assert(kmem_cache_nr_allocated(radix_tree_node_cachep) == 0);
	assert(nr_allocated == 0);
	printf("PASS %s outstanding_nodes=%d\n", name, nr_allocated);
}

static void allocation_failure(void)
{
	struct cc_xarray xa;
	unsigned long index = 1UL << (3 * CC_XA_CHUNK_SHIFT);
	void *value = cc_xa_mk_value(27);
	CC_XA_STATE(xas, &xa, index);

	cc_xa_init(&xa);
	assert(!cc_xa_store(&xa, 1, value, GFP_KERNEL));
	kmem_cache_set_non_kernel(radix_tree_node_cachep, 0);
	rcu_read_lock();
	cc_xas_store(&xas, value);
	assert(cc_xas_error(&xas) == -ENOMEM);
	release_cursor(&xas);
	rcu_read_unlock();
	check_refs(&xa);
	assert(cc_xa_load(&xa, 1) == value);
	assert(!cc_xa_load(&xa, index));
	assert(cc_xa_err(cc_xa_store(&xa, index, value, GFP_NOWAIT)) == -ENOMEM);
	check_refs(&xa);
	assert(!cc_xa_store(&xa, index, value, GFP_KERNEL));
	assert(cc_xa_load(&xa, index) == value);
	printf("ENOMEM=%d recovery_value=%lu\n", -ENOMEM,
	       cc_xa_to_value(cc_xa_load(&xa, index)));
	finish(&xa, "allocation-failure-recovery");
}

static void reserve_release(void)
{
	struct cc_xarray xa;
	unsigned long index = CC_XA_CHUNK_SIZE + 7;
	void *value = cc_xa_mk_value(83);

	cc_xa_init(&xa);
	assert(!cc_xa_reserve(&xa, index, GFP_KERNEL));
	assert(!cc_xa_load(&xa, index));
	check_refs(&xa);
	cc_xa_release(&xa, index);
	check_refs(&xa);
	assert(!cc_xa_insert(&xa, index, value, GFP_KERNEL));
	assert(cc_xa_load(&xa, index) == value);
	assert(cc_xa_erase(&xa, index) == value);
	cc_xa_garbage_collector(&xa, NULL);
	rcu_barrier();
	assert(nr_allocated == 0);
	finish(&xa, "reserve-release-insert");
}

static void sparse_gc(void)
{
	struct cc_xarray xa;
	unsigned long indices[] = { 1, CC_XA_CHUNK_SIZE + 3,
		(1UL << (2 * CC_XA_CHUNK_SHIFT)) + 5,
		(1UL << (3 * CC_XA_CHUNK_SHIFT)) + 7 };
	unsigned int i;

	cc_xa_init(&xa);
	for (i = 0; i < ARRAY_SIZE(indices); i++)
		assert(!cc_xa_store(&xa, indices[i], cc_xa_mk_value(i + 1), GFP_KERNEL));
	check_refs(&xa);
	for (i = 0; i < ARRAY_SIZE(indices); i++) {
		assert(cc_xa_erase(&xa, indices[i]) == cc_xa_mk_value(i + 1));
		cc_xa_garbage_collector(&xa, NULL);
		rcu_barrier();
		check_refs(&xa);
	}
	assert(nr_allocated == 0);
	finish(&xa, "sparse-erase-gc");

	cc_xa_init(&xa);
	for (i = 0; i < ARRAY_SIZE(indices); i++)
		assert(!cc_xa_store(&xa, indices[i], cc_xa_mk_value(i + 1), GFP_KERNEL));
	finish(&xa, "sparse-destroy");
}

static void split(unsigned int order, unsigned int target)
{
	struct cc_xarray xa;
	unsigned long base = 1UL << order, end = base * 2;
	unsigned long i, group = 1UL << target;
	void *value = cc_xa_mk_value(19);
	CC_XA_STATE_ORDER(xas, &xa, base, order);

	cc_xa_init(&xa);
	rcu_read_lock();
	do {
		cc_xas_store(&xas, value);
		cc_xas_rewind_refcnt(&xas);
	} while (cc_xas_nomem(&xas, GFP_KERNEL));
	assert(!cc_xas_error(&xas));
	release_cursor(&xas);
	rcu_read_unlock();
	cc_xa_set_mark(&xa, base, CC_XA_MARK_0);
	cc_xa_set_mark(&xa, base, CC_XA_MARK_2);
	cc_xas_set_order(&xas, base, target);
	cc_xas_set_update(&xas, updated);
	cc_xas_split_alloc(&xas, value, order, GFP_KERNEL);
	assert(!cc_xas_error(&xas));
	updates = 0;
	rcu_read_lock();
	cc_xas_lock(&xas);
	cc_xas_split(&xas, value, order);
	assert(!cc_xas_error(&xas));
	release_cursor(&xas);
	cc_xas_unlock(&xas);
	rcu_read_unlock();
	assert(updates > 0);
	assert(!cc_xas_nomem(&xas, GFP_NOWAIT));
	check_refs(&xa);
	for (i = base; i < end; i++) {
		assert(cc_xa_load(&xa, i) == value);
		assert(cc_xa_get_order(&xa, i) == target);
		assert(cc_xa_get_mark(&xa, i, CC_XA_MARK_0));
		assert(!cc_xa_get_mark(&xa, i, CC_XA_MARK_1));
		assert(cc_xa_get_mark(&xa, i, CC_XA_MARK_2));
	}
	/* Unique values expose siblings incorrectly pointing to canonical slot 0. */
	for (i = base; i < end; i += group)
		assert(cc_xa_store(&xa, i, cc_xa_mk_value(i), GFP_KERNEL) == value);
	for (i = base; i < end; i++)
		assert(cc_xa_load(&xa, i) == cc_xa_mk_value(i & ~(group - 1)));
	assert(!cc_xa_load(&xa, base - 1));
	assert(!cc_xa_load(&xa, end));
	printf("split %u->%u canonical_groups=%lu marks=0,2 callbacks=%u\n",
	       order, target, (end - base) / group, updates);
	finish(&xa, "split-canonical-value-boundaries");
}

static void ref_boundaries(void)
{
	struct cc_xarray xa;
	struct cc_xa_node *node;
	void *value = cc_xa_mk_value(47);
	void *entries[2];
	unsigned long index = 0;
	CC_XA_STATE(xas, &xa, 1);

	cc_xa_init(&xa);
	assert(!cc_xa_store(&xa, 1, value, GFP_KERNEL));
	cc_xa_set_mark(&xa, 1, CC_XA_MARK_0);
	node = cc_xa_to_node(xa.xa_head);
	assert(node->refcnt == 0);
	/* Quiescent fault injection into an actual CC-allocated, ctor-run node. */
	rcu_read_lock();
	node->refcnt = USHRT_MAX - 2;
	assert(cc_xas_set_xa_node(&xas, node));
	assert(node->refcnt == USHRT_MAX - 1);
	cc_xas_clear_xa_node(&xas);
	assert(node->refcnt == USHRT_MAX - 2);
	node->refcnt = USHRT_MAX - 1;
	assert(cc_xa_load(&xa, 1) == value);
	assert(cc_xa_get_order(&xa, 1) == 0);
	assert(cc_xa_get_mark(&xa, 1, CC_XA_MARK_0));
	assert(cc_xa_find(&xa, &index, ULONG_MAX, CC_XA_PRESENT) == value);
	assert(index == 1);
	assert(!cc_xa_find_after(&xa, &index, ULONG_MAX, CC_XA_PRESENT));
	assert(cc_xa_extract(&xa, entries, 0, ULONG_MAX, 2, CC_XA_MARK_0) == 1);
	assert(entries[0] == value);
	assert(!cc_xas_set_xa_node(&xas, node));
	assert(cc_xas_error(&xas) == -EOVERFLOW);
	assert(node->refcnt == USHRT_MAX - 1);
	cc_xas_clear_xa_node(&xas);
	assert(cc_xa_err(cc_xa_store(&xa, 1, value, GFP_NOWAIT)) == -EOVERFLOW);
	assert(node->refcnt == USHRT_MAX - 1);
	node->refcnt = USHRT_MAX;
	assert(!cc_xas_set_xa_node(&xas, node));
	assert(cc_xas_error(&xas) == -EAGAIN);
	assert(node->refcnt == USHRT_MAX);
	node->refcnt = 0;
	release_cursor(&xas);
	rcu_read_unlock();
	assert(cc_xa_load(&xa, 1) == value);
	printf("ref_max_live=%u retired=%u overflow=%d retired_error=%d\n",
	       USHRT_MAX - 1, USHRT_MAX, -EOVERFLOW, -EAGAIN);
	finish(&xa, "ref-sentinel-error-propagation");
}

int main(int argc, char **argv)
{
	const char *test = argc > 1 ? argv[1] : "all";

	setbuf(stdout, NULL);
	BUILD_BUG_ON(sizeof(struct xa_node) != sizeof(struct cc_xa_node));
	BUILD_BUG_ON(offsetof(struct xa_node, slots) != offsetof(struct cc_xa_node, slots));
	rcu_register_thread();
	radix_tree_init();
	if (!strcmp(test, "all") || !strcmp(test, "alloc"))
		allocation_failure();
	if (!strcmp(test, "all") || !strcmp(test, "reserve"))
		reserve_release();
	if (!strcmp(test, "all") || !strcmp(test, "sparse"))
		sparse_gc();
	if (!strcmp(test, "all") || !strcmp(test, "split")) {
		split(CC_XA_CHUNK_SHIFT, 0);
		split(CC_XA_CHUNK_SHIFT + 1, 1);
	}
	if (!strcmp(test, "all") || !strcmp(test, "refs"))
		ref_boundaries();
	radix_tree_cpu_dead(1);
	rcu_barrier();
	assert(!nr_allocated);
	rcu_unregister_thread();
	return 0;
}
