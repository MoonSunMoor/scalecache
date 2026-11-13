// SPDX-License-Identifier: GPL-2.0+
/* Actual CC/ordinary XArray, workingset and list-LRU algorithms.
 * Native pthread locks, one NUMA node, no memcg or kernel IRQ context.
 */
#include "xarray-shared.h"
#include "test.h"
#include <linux/cc_xarray.h>
#include <string.h>
#include <errno.h>

#define spin_trylock(lock) (!pthread_mutex_trylock(lock))
static void replace_shadow(void);
static void **watched_slot;
static bool at_cas, at_unlock, scanning;
static unsigned int checks;
static int live_entry;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); \
		exit(1); \
	} \
} while (0)

static bool fixture_lock_held(spinlock_t *lock)
{
	int err = pthread_mutex_trylock(lock);

	CHECK(!err || err == EBUSY);
	if (!err)
		pthread_mutex_unlock(lock);
	return err == EBUSY;
}

#define lockdep_assert_held(lock) CHECK(fixture_lock_held(lock))

static void *scheduled_cas(void *slot, void *old, void *entry)
{
	if (at_cas && slot == watched_slot) {
		at_cas = false;
		replace_shadow();
	}
	return __sync_val_compare_and_swap((void **)slot, old, entry);
}

#define __sync_val_compare_and_swap(slot, old, entry) \
	scheduled_cas(slot, old, entry)
#include "../../../lib/cc_xarray.c"
#undef __sync_val_compare_and_swap

struct page { int unused; };
struct mem_cgroup;
struct inode { spinlock_t i_lock; };
struct address_space { struct xarray i_pages; struct inode *host; };
struct list_lru_one {
	struct list_head list;
	long nr_items;
	spinlock_t lock;
};
struct list_lru_node { struct list_lru_one lru; long nr_items; };
struct list_lru { struct list_lru_node *node; };
enum lru_status {
	LRU_REMOVED, LRU_REMOVED_RETRY, LRU_ROTATE, LRU_SKIP,
	LRU_RETRY, LRU_STOP
};
typedef enum lru_status (*list_lru_walk_cb)(struct list_head *,
		struct list_lru_one *, void *);
static struct list_lru_node lru_node;
static struct list_lru shadow_nodes = { .node = &lru_node };
static struct address_space mapping;
static unsigned long selected_index;
static long stats[2];
enum { WORKINGSET_NODES, WORKINGSET_NODERECLAIM };

#define virt_to_page(p) ((struct page *)(p))
#define page_to_nid(p) ((void)(p), 0)
#define mem_cgroup_from_virt(p) ((void)(p), (struct mem_cgroup *)NULL)
#define set_shrinker_bit(cg, nid, id) \
	CHECK(!(cg) && (nid) == 0 && (id) == -1)
#define atomic_long_inc(p) (++*(p))
#define atomic_long_dec(p) (--*(p))
#define __inc_node_page_state(page, state) ((void)(page), ++stats[state])
#define __dec_node_page_state(page, state) ((void)(page), --stats[state])
#define mod_lruvec_kmem_state(node, state, delta) \
	((void)(node), stats[state] += (delta))
#define cond_resched() ((void)0)

#define rcu_access_pointer(p) READ_ONCE(p)
#include "generated/cc-shadow-mapping.h"

static unsigned int inode_lru_updates;

static void inode_lru_list_add(struct inode *inode)
{
	CHECK(inode == mapping.host);
	CHECK(fixture_lock_held(&inode->i_lock));
	inode_lru_updates++;
}

#include "generated/cc-shadow-list-lru.h"

static void scheduled_unlock(spinlock_t *lock)
{
	pthread_mutex_unlock(lock);
	if (at_unlock && lock == &lru_node.lru.lock) {
		at_unlock = false;
		replace_shadow();
	}
}

static void scheduled_delete(struct xa_node *node, xa_update_node_t update)
{
	if (at_cas) {
		at_cas = false;
		replace_shadow();
	}
	xa_delete_node(node, update);
}

static unsigned int cleared_slots;

static unsigned int observed_clear(struct cc_xa_state *xas)
{
	cleared_slots = cc_xas_clear_shadow_node(xas);
	return cleared_slots;
}

#undef spin_unlock
#define spin_unlock(lock) scheduled_unlock(lock)
#define xa_delete_node(node, update) scheduled_delete(node, update)
#define cc_xas_clear_shadow_node(xas) observed_clear(xas)
#include "generated/cc-shadow-workingset.h"
#undef cc_xas_clear_shadow_node
#undef xa_delete_node
#undef spin_unlock
#define spin_unlock(lock) pthread_mutex_unlock(lock)

static void put(unsigned long index, void *entry)
{
	CC_XA_STATE(xas, CC_XARRAY(&mapping.i_pages), index);

	cc_xas_set_update(&xas, cc_workingset_update_node);
	do {
		if (!scanning)
			cc_xa_lock(xas.xa);
		cc_xas_store(&xas, entry);
		if (!cc_xas_error(&xas) && entry == &live_entry) {
			cc_xas_set_mark(&xas, CC_XA_MARK_0);
			cc_xas_set_mark(&xas, CC_XA_MARK_1);
			cc_xas_set_mark(&xas, CC_XA_MARK_2);
		}
		if (!scanning)
			cc_xa_unlock(xas.xa);
	} while (cc_xas_nomem(&xas, GFP_KERNEL));
	CHECK(!cc_xas_error(&xas));
	CHECK(!xas.nr_refget);
	cc_xas_clear_xa_node(&xas);
}

static void replace_shadow(void)
{
	put(selected_index, &live_entry);
}



static struct cc_xa_node *node_at(unsigned long index)
{
	CC_XA_STATE(xas, CC_XARRAY(&mapping.i_pages), index);
	struct cc_xa_node *node;

	cc_xas_load(&xas, false);
	node = xas.xa_node;
	CHECK(!cc_xas_not_node(node));
	cc_xas_rewind_refcnt(&xas);
	cc_xas_clear_xa_node(&xas);
	return node;
}

static void check_refs(struct cc_xa_node *node)
{
	unsigned int i;

	CHECK(!node->refcnt);
	if (!node->shift)
		return;
	for (i = 0; i < CC_XA_CHUNK_SIZE; i++) {
		void *entry = node->slots[i];

		if (cc_xa_is_node(entry))
			check_refs(cc_xa_to_node(entry));
	}
}

static void init_mapping(bool ordinary)
{
	memset(&mapping, 0, sizeof(mapping));
	if (ordinary)
		xa_init(&mapping.i_pages);
	else
		cc_xa_init(CC_XARRAY(&mapping.i_pages));
	CHECK(!lru_node.lru.nr_items && !lru_node.nr_items);
	CHECK(!stats[WORKINGSET_NODES]);
	stats[WORKINGSET_NODERECLAIM] = 0;
}

static void finish(bool ordinary)
{
	while (!list_empty(&lru_node.lru.list)) {
		CHECK(list_lru_del_obj(&shadow_nodes, lru_node.lru.list.next));
		stats[WORKINGSET_NODES]--;
	}
	if (ordinary)
		xa_destroy(&mapping.i_pages);
	else
		cc_xa_destroy(CC_XARRAY(&mapping.i_pages));
	rcu_barrier();
	CHECK(!nr_allocated);
	CHECK(!stats[WORKINGSET_NODES]);
	CHECK(!lru_node.lru.nr_items && !lru_node.nr_items);
	pthread_mutex_destroy(&mapping.i_pages.xa_lock);
}

static unsigned long scan(void)
{
	unsigned long budget = 1, freed;

	cleared_slots = 0;
	scanning = true;
	freed = __list_lru_walk_one(&shadow_nodes, 0, NULL,
			shadow_lru_isolate, NULL, &budget, true);
	scanning = false;
	CHECK(!budget);
	CHECK(lru_node.nr_items == lru_node.lru.nr_items);
	CHECK(lru_node.nr_items == stats[WORKINGSET_NODES]);
	CHECK(!pthread_mutex_trylock(&mapping.i_pages.xa_lock));
	pthread_mutex_unlock(&mapping.i_pages.xa_lock);
	CHECK(!pthread_mutex_trylock(&lru_node.lru.lock));
	pthread_mutex_unlock(&lru_node.lru.lock);
	return freed;
}

static void cc_case(const char *name)
{
	CC_XA_STATE(reader, CC_XARRAY(&mapping.i_pages), 0);
	struct cc_xa_node *node;
	void *shadow = cc_xa_mk_value(7);
	bool held = !strcmp(name, "reader");
	bool writer = !strcmp(name, "writer") || !strcmp(name, "mixed");
	bool siblings = !strcmp(name, "siblings");
	bool marked = !strcmp(name, "marked");
	unsigned long index = CC_XA_CHUNK_SIZE;
	unsigned int count = 2, i;
	unsigned long freed;
	int allocated;

	if (!strcmp(name, "root") || marked)
		index = 1;
	if (!strcmp(name, "high-index"))
		index = (1UL << 36) + 1;
	if (!strcmp(name, "batch")) {
		index = 0;
		count = CC_XA_CHUNK_SIZE;
	}
	if (marked)
		count = 1;
	init_mapping(false);
	if (siblings) {
		CHECK(!cc_xa_is_err(cc_xa_store_range(CC_XARRAY(&mapping.i_pages),
						0, 3, shadow, GFP_KERNEL)));
		index = CC_XA_CHUNK_SIZE - 1;
		count = 1;
	}
	for (i = 0; i < count; i++)
		put(index + i, shadow);
	node = node_at(index);
	CHECK(node->count == node->nr_values);
	if (marked)
		cc_xa_set_mark(CC_XARRAY(&mapping.i_pages), index, CC_XA_MARK_0);
	if (held) {
		reader.xa_index = index;
		CHECK(cc_xas_load(&reader, false) == shadow);
		CHECK(node->refcnt == 2);
	}
	selected_index = index;
	watched_slot = (void **)&node->slots[index & CC_XA_CHUNK_MASK];
	at_cas = !strcmp(name, "writer");
	at_unlock = !strcmp(name, "mixed");
	allocated = nr_allocated;
	freed = scan();
	if (held)
		rcu_barrier();
	for (i = 0; i < count; i++) {
		void *expected = writer && !i ? &live_entry : marked ? shadow : NULL;

		CHECK(cc_xa_load(CC_XARRAY(&mapping.i_pages), index + i) == expected);
	}
	CHECK(nr_allocated == allocated);
	CHECK(freed == 0);
	CHECK(cleared_slots == (marked ? 0 : writer ? count - 1 : count));
	CHECK(stats[WORKINGSET_NODERECLAIM] == 0);
	CHECK(!at_cas && !at_unlock);
	if (writer) {
		CHECK(node->count == 1 && node->nr_values == 0 && !node->del);
		CHECK(cc_xa_get_mark(CC_XARRAY(&mapping.i_pages), index, CC_XA_MARK_0));
		CHECK(cc_xa_get_mark(CC_XARRAY(&mapping.i_pages), index, CC_XA_MARK_1));
		CHECK(cc_xa_get_mark(CC_XARRAY(&mapping.i_pages), index, CC_XA_MARK_2));
	} else if (siblings) {
		for (i = 0; i < 4; i++)
			CHECK(cc_xa_load(CC_XARRAY(&mapping.i_pages), i) == shadow);
		CHECK(node->count == 4 && node->nr_values == 4 && !node->del);
	} else if (marked) {
		CHECK(node->count == 1 && node->nr_values == 1);
		cc_xa_clear_mark(CC_XARRAY(&mapping.i_pages), index, CC_XA_MARK_0);
		CHECK(scan() == 0);
		CHECK(cleared_slots == 1);
		CHECK(!cc_xa_load(CC_XARRAY(&mapping.i_pages), index));
	} else {
		CHECK(!node->count && !node->nr_values);
		if (!held)
			CHECK(!!node->del == !!cc_xa_parent(node->array, node));
	}
	if (held) {
		CHECK(node->refcnt == 2 && !node->del);
		cc_xas_rewind_refcnt(&reader);
		cc_xas_clear_xa_node(&reader);
	}
	check_refs(cc_xa_to_node(CC_XARRAY(&mapping.i_pages)->xa_head));
	CHECK(stats[WORKINGSET_NODES] == (siblings ? 1 : 0));
	finish(false);
}

static void ordinary_case(void)
{
	XA_STATE(xas, &mapping.i_pages, 1);

	init_mapping(true);
	xas_set_update(&xas, workingset_update_node);
	do {
		xas_lock(&xas);
		xas_store(&xas, xa_mk_value(7));
		xas_unlock(&xas);
	} while (xas_nomem(&xas, GFP_KERNEL));
	CHECK(!xas_error(&xas));
	CHECK(scan() == 1);
	rcu_barrier();
	CHECK(!xa_load(&mapping.i_pages, 1));
	CHECK(stats[WORKINGSET_NODERECLAIM] == 1);
	finish(true);
}



int main(int argc, char **argv)
{
	const char *cases[] = { "reader", "writer", "mixed", "root", "high-index",
		"batch", "siblings", "marked", "ordinary" };
	unsigned int i;

	rcu_register_thread();
	radix_tree_init();
	spin_lock_init(&lru_node.lru.lock);
	INIT_LIST_HEAD(&lru_node.lru.list);
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (argc > 1 && strcmp(argv[1], cases[i]))
			continue;
		if (!strcmp(cases[i], "ordinary"))
			ordinary_case();
		else
			cc_case(cases[i]);
		printf("PASS %s\n", cases[i]);
	}
	radix_tree_cpu_dead(1);
	rcu_barrier();
	CHECK(!nr_allocated);
	pthread_mutex_destroy(&lru_node.lru.lock);
	rcu_unregister_thread();
	printf("PASS %u checks\n", checks);
	return 0;
}
