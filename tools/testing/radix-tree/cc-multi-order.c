// SPDX-License-Identifier: GPL-2.0+
/*
 * Order > 0 store and delete contract of cc_xarray, as stated in
 * Documentation/core-api/cc-xarray.rst ("Order and split interfaces").
 *
 * Deterministic: an order-0 insert runs before one slot CAS of the order-2
 * operation.  Every CAS of the operation is combined with every index of the
 * range.  The split cases do the same with every slot write of a split.
 */
#include "xarray-shared.h"
#include "test.h"
#include <linux/cc_xarray.h>
#include <string.h>
#include <errno.h>
#include <sched.h>

static void (*intruder)(void);
static void **watched_first, **watched_last;
static bool watch_all;
static unsigned int countdown;

/* Run the intruder before the countdown-th write to a watched slot. */
static void scheduled_write(void *slot)
{
	if (intruder && (watch_all || ((void **)slot >= watched_first &&
	    (void **)slot <= watched_last)) && !countdown--) {
		void (*run)(void) = intruder;

		intruder = NULL;
		run();
	}
}

static void *scheduled_cas(void *slot, void *old, void *entry)
{
	scheduled_write(slot);
	return __sync_val_compare_and_swap((void **)slot, old, entry);
}

#undef rcu_assign_pointer
#define rcu_assign_pointer(p, v) do { \
	scheduled_write(&(p)); \
	(p) = (v); \
} while (0)
#define __sync_val_compare_and_swap(slot, old, entry) \
	scheduled_cas(slot, old, entry)
#include "../../../lib/cc_xarray.c"
#undef __sync_val_compare_and_swap
#define BASE	4UL	/* order-2 entry covers BASE..BASE + 3 */
#define ORDER	2U
#define NR	(1UL << ORDER)

static struct xarray tree;
#define XA CC_XARRAY(&tree)

static int anchor, big, other;
static unsigned int failures;
static unsigned int nr_runs, nr_partial;

#define EXPECT(expr) do { \
	if (!(expr)) { \
		printf("  violated: %s\n", #expr); \
		failures++; \
	} \
} while (0)

/* Insert as the CC copy of __filemap_add_folio() does: expect an empty slot. */
static int insert(unsigned long index, unsigned int order, void *entry)
{
	CC_XA_STATE_ORDER(xas, XA, index, order);
	int err;

	do {
		cc_xas_store_expected(&xas, NULL, entry);
	} while (cc_xas_nomem(&xas, GFP_KERNEL));
	err = cc_xas_error(&xas);
	cc_xas_clear_xa_node(&xas);
	return err;
}

/* Delete as page_cache_delete() does: the caller owns the entry. */
static int erase(unsigned long index, unsigned int order)
{
	CC_XA_STATE_ORDER(xas, XA, index, order);
	int err;

	cc_xas_store(&xas, NULL);
	err = cc_xas_error(&xas);
	cc_xas_clear_xa_node(&xas);
	return err;
}

static struct cc_xa_node *leaf(void)
{
	CC_XA_STATE(xas, XA, BASE);
	struct cc_xa_node *node;

	cc_xas_load(&xas, false);
	node = xas.xa_node;
	cc_xas_rewind_refcnt(&xas);
	cc_xas_clear_xa_node(&xas);
	return node;
}

static unsigned int occupied(struct cc_xa_node *node)
{
	unsigned int i, nr = 0;

	for (i = 0; i < CC_XA_CHUNK_SIZE; i++)
		nr += !!node->slots[i];
	return nr;
}

static struct cc_xa_node *begin(const char *name)
{
	printf("%s\n", name);
	cc_xa_init(XA);
	/* A neighbour keeps the leaf allocated and linked for the whole case. */
	if (insert(1, 0, &anchor))
		abort();
	return leaf();
}

static void end(void)
{
	cc_xa_destroy(XA);
	rcu_barrier();
}

/* One lookup as a reader makes it. */
static void *peek(unsigned long index)
{
	CC_XA_STATE(xas, XA, index);
	void *entry = cc_xas_load(&xas, true);

	cc_xas_clear_xa_node(&xas);
	return entry;
}

/*
 * A shadow is never multi-index: storing one over a range stores it in every
 * slot.  The split cases with a shadow therefore find nothing to split, and
 * check that a split attempted on stale information does no harm.
 */
#define SHADOW	cc_xa_mk_value(7)

static int replace_shadow(unsigned long index, void *entry)
{
	CC_XA_STATE(xas, XA, index);
	int err;

	cc_xas_store_expected(&xas, SHADOW, entry);
	err = cc_xas_error(&xas);
	cc_xas_clear_xa_node(&xas);
	return err;
}

static unsigned int value_slots(struct cc_xa_node *node)
{
	unsigned int i, nr = 0;

	for (i = 0; i < CC_XA_CHUNK_SIZE; i++) {
		void *entry = node->slots[i];

		if (cc_xa_is_sibling(entry))
			entry = node->slots[cc_xa_to_sibling(entry)];
		nr += cc_xa_is_value(entry);
	}
	return nr;
}

static void counted(struct cc_xa_node *node)
{
	EXPECT(node->count == occupied(node));
	EXPECT(node->nr_values == value_slots(node));
	EXPECT(!node->refcnt);
}

/*
 * Entries of a leaf or more.  A cc node stays linked to its parent after it
 * is deleted, so such an entry fills the leaves of its range instead of a
 * slot of an inner node.
 */
#define FAR		(4UL * CC_XA_CHUNK_SIZE)
#define FAR_ORDER	7U
#define FAR_NR		(1UL << FAR_ORDER)

static unsigned long holds(unsigned long base, unsigned long nr, void *entry)
{
	unsigned long i, n = 0;

	for (i = 0; i < nr; i++)
		n += cc_xa_load(XA, base + i) == entry;
	return n;
}

static struct cc_xa_node *leaf_of(unsigned long index)
{
	CC_XA_STATE(xas, XA, index);
	struct cc_xa_node *node;

	cc_xas_load(&xas, false);
	node = xas.xa_node;
	cc_xas_rewind_refcnt(&xas);
	cc_xas_clear_xa_node(&xas);
	return node;
}

/* Every leaf of the range agrees with its slots. */
static void far_counted(unsigned long nr)
{
	unsigned long i;

	for (i = 0; i < nr; i += CC_XA_CHUNK_SIZE) {
		struct cc_xa_node *node = leaf_of(FAR + i);

		/* A leaf without entries is deleted and no longer reached. */
		if (!node || node->shift)
			continue;
		counted(node);
	}
}

static int store_order(unsigned long index, unsigned int order, void *entry)
{
	CC_XA_STATE_ORDER(xas, XA, index, order);
	int err;

	do {
		cc_xas_store(&xas, entry);
	} while (cc_xas_nomem(&xas, GFP_KERNEL));
	err = cc_xas_error(&xas);
	cc_xas_clear_xa_node(&xas);
	return err;
}

/*
 * Threads instead of a schedule.  One writer inserts and deletes the order-2
 * entry, one writer per index does the same with an order-0 entry of its own,
 * and readers check that an index never returns another index's entry.
 * Run with "stress [attempts]"; not part of the default run.  It ends after
 * that many order-2 attempts and yields regularly, so it needs no clock.
 *
 * "stress-split [attempts]" adds a shadow: the order-2 writer turns its entry
 * into a shadow before it removes it, as eviction and reclaim do, and the
 * order-0 writers split that shadow and replace their own index, as
 * concurrent inserters do.
 */
static int small[NR];
static volatile bool stop;
static unsigned long attempts;

#define BREATHE(n) do { if (!((n) & 1023)) sched_yield(); } while (0)
static unsigned long stress_wrong;
static unsigned long walk_seen, walk_moved;

struct worker {
	pthread_t thread;
	unsigned long index;
	unsigned long done, lost, loads;
};

/*
 * "stress-far [attempts]": the same with an order-7 entry, which fills two
 * leaves.  Its writer stores it, turns it into shadows, stores a second
 * entry over the shadows and deletes that.
 *
 * "stress-nodes [attempts]": the order-0 writers alone.  Nothing else keeps
 * their leaves in use, so the leaves are deleted and reused all the time.
 */
static const unsigned long far_at[NR] = { 0, 63, 64, 127 };
static unsigned long erase_missed, store_hidden;
static bool far_idle;

static void drop_shadows(void)
{
	unsigned long i;

	for (i = 0; i < FAR_NR; i++) {
		CC_XA_STATE(xas, XA, FAR + i);

		cc_xas_store_expected(&xas, SHADOW, NULL);
		cc_xas_clear_xa_node(&xas);
	}
}

static void *far_writer(void *arg)
{
	struct worker *w = arg;

	rcu_register_thread();
	while (w->done + w->lost < attempts) {
		CC_XA_STATE_ORDER(xas, XA, FAR, FAR_ORDER);

		BREATHE(w->done + w->lost);
		if (far_idle) {
			sched_yield();
			w->done++;
			continue;
		}
		if (insert(FAR, FAR_ORDER, &big)) {
			w->lost++;
			continue;
		}
		if (holds(FAR, FAR_NR, &big) != FAR_NR)
			__sync_fetch_and_add(&stress_wrong, 1);
		if (w->done & 1) {
			if (erase(FAR, FAR_ORDER))
				abort();
			w->done++;
			continue;
		}
		if (store_order(FAR, FAR_ORDER, SHADOW))
			abort();
		do {
			cc_xas_store_expected(&xas, SHADOW, &other);
		} while (cc_xas_nomem(&xas, GFP_KERNEL));
		if (!cc_xas_error(&xas)) {
			cc_xas_clear_xa_node(&xas);
			if (holds(FAR, FAR_NR, &other) != FAR_NR)
				__sync_fetch_and_add(&stress_wrong, 1);
			if (erase(FAR, FAR_ORDER))
				abort();
		} else {
			cc_xas_clear_xa_node(&xas);
			drop_shadows();
		}
		w->done++;
	}
	rcu_unregister_thread();
	return NULL;
}

static void *far_small_writer(void *arg)
{
	struct worker *w = arg;
	unsigned long index = FAR + far_at[w->index];

	rcu_register_thread();
	while (!stop) {
		BREATHE(w->done + w->lost);
		if (replace_shadow(index, &small[w->index]) &&
		    insert(index, 0, &small[w->index])) {
			w->lost++;
			continue;
		}
		/* Nobody else writes this index while it holds our entry. */
		if (peek(index) != &small[w->index])
			__sync_fetch_and_add(&store_hidden, 1);
		if (erase(index, 0))
			abort();
		if (peek(index) == &small[w->index])
			__sync_fetch_and_add(&erase_missed, 1);
		w->done++;
	}
	rcu_unregister_thread();
	return NULL;
}

static void *far_reader(void *arg)
{
	struct worker *w = arg;
	unsigned long i, j;

	rcu_register_thread();
	while (!stop) {
		BREATHE(w->loads >> 7);
		for (i = 0; i < FAR_NR; i++) {
			void *entry = peek(FAR + i);

			w->loads++;
			if (!entry || entry == &big || entry == &other ||
			    entry == SHADOW)
				continue;
			for (j = 0; j < NR; j++)
				if (far_at[j] == i && entry == &small[j])
					break;
			if (j == NR)
				__sync_fetch_and_add(&stress_wrong, 1);
		}
		/* A walk meets only entries of the range, at their index. */
		{
			CC_XA_STATE(xas, XA, FAR + (w->loads & 127));
			void *entry;

			rcu_read_lock();
			cc_xas_for_each(&xas, entry, FAR + FAR_NR - 1) {
				if (cc_xas_retry(&xas, entry))
					continue;
				if (xas.xa_index < FAR ||
				    xas.xa_index >= FAR + FAR_NR ||
				    peek(xas.xa_index) != entry)
					__sync_fetch_and_add(&walk_moved, 1);
				cc_xas_get_order(&xas);
				__sync_fetch_and_add(&walk_seen, 1);
			}
			rcu_read_unlock();
			cc_xas_clear_xa_node(&xas);
		}
	}
	rcu_unregister_thread();
	return NULL;
}

static void stress_far(unsigned long nr, bool idle)
{
	struct worker big_w = { 0 }, small_w[NR] = { { 0 } }, read_w[2] = { { 0 } };
	unsigned long i, small_done = 0, small_lost = 0, loads = 0;

	begin(idle ? "stress-nodes" : "stress-far");
	far_idle = idle;
	attempts = nr;
	pthread_create(&big_w.thread, NULL, far_writer, &big_w);
	for (i = 0; i < NR; i++) {
		small_w[i].index = i;
		pthread_create(&small_w[i].thread, NULL, far_small_writer,
			       &small_w[i]);
	}
	for (i = 0; i < 2; i++)
		pthread_create(&read_w[i].thread, NULL, far_reader, &read_w[i]);
	pthread_join(big_w.thread, NULL);
	stop = true;
	for (i = 0; i < NR; i++) {
		pthread_join(small_w[i].thread, NULL);
		small_done += small_w[i].done;
		small_lost += small_w[i].lost;
	}
	for (i = 0; i < 2; i++) {
		pthread_join(read_w[i].thread, NULL);
		loads += read_w[i].loads;
	}
	drop_shadows();
	if (!idle)
		printf("  order-7 stored %lu, refused %lu\n", big_w.done,
		       big_w.lost);
	printf("  order-0 stored %lu, refused %lu\n", small_done, small_lost);
	printf("  lookups %lu, wrong entry %lu\n", loads, stress_wrong);
	printf("  stores that a lookup did not find %lu, deletes that left the entry %lu\n",
	       store_hidden, erase_missed);
	EXPECT(!stress_wrong);
	printf("  entries walked %lu, no longer there at a second look %lu\n",
	       walk_seen, walk_moved);
	EXPECT(!store_hidden && !erase_missed);
	EXPECT(big_w.done && small_done);
	EXPECT(holds(FAR, FAR_NR, NULL) == FAR_NR);
	far_counted(FAR_NR);
	end();
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*run)(void);
	} cases[] = {
	};
	unsigned int i, failed = 0;

	rcu_register_thread();
	radix_tree_init();
	if (argc > 1 && (!strcmp(argv[1], "stress-far") ||
			 !strcmp(argv[1], "stress-nodes"))) {
		stress_far(argc > 2 ? strtoul(argv[2], NULL, 0) : 200000,
			   !strcmp(argv[1], "stress-nodes"));
		printf("%s %s\n", failures ? "FAIL" : "PASS", argv[1]);
		rcu_unregister_thread();
		return !!failures;
	}
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		if (argc > 1 && strcmp(argv[1], cases[i].name))
			continue;
		failures = 0;
		cases[i].run();
		printf("%s %s\n", failures ? "FAIL" : "PASS", cases[i].name);
		failed += !!failures;
	}
	printf("interleavings %u, reader saw part of the entry %u\n",
	       nr_runs, nr_partial);
	radix_tree_cpu_dead(1);
	rcu_barrier();
	rcu_unregister_thread();
	return !!failed;
}
