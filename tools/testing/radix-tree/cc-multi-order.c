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
static unsigned long other_index;
static int other_err;
static bool wrong_index;
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

static void show(const char *when, struct cc_xa_node *node)
{
	unsigned long i;

	printf("  %s:", when);
	for (i = 0; i < NR; i++) {
		void *entry = node->slots[BASE + i];

		printf(" [%lu]=%s", BASE + i,
		       !entry ? "empty" :
		       cc_xa_is_sibling(entry) ? "sibling" :
		       entry == &big ? "big" :
		       entry == &other ? "other" : "?");
	}
	printf(" count=%u occupied=%u\n", node->count, occupied(node));
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

/* "Other writers do not act on an intermediate state." */
static void insert_other(void)
{
	unsigned long i, bigs = 0;

	for (i = 0; i < NR; i++)
		bigs += peek(BASE + i) == &big;
	/* Allowed: a reader may see the entry at only part of its range. */
	nr_partial += bigs && bigs < NR;

	other_err = insert(other_index, 0, &other);
	for (i = 0; i < NR; i++) {
		if (BASE + i != other_index && peek(BASE + i) == &other)
			wrong_index = true;
	}
}

static void arm(struct cc_xa_node *node, unsigned int cas, unsigned long at)
{
	other_index = BASE + at;
	other_err = 1;
	wrong_index = false;
	watched_first = (void **)&node->slots[BASE];
	watched_last = (void **)&node->slots[BASE + NR - 1];
	countdown = cas;
	intruder = insert_other;
}

/* False once the operation finished before its cas-th CAS. */
static bool fired(void)
{
	bool ran = !intruder;

	intruder = NULL;
	nr_runs += ran;
	return ran;
}

/* What every index must hold once both writers have returned. */
static void settled(struct cc_xa_node *node, bool big_present)
{
	unsigned long i;

	EXPECT(other_err == 0 || other_err == -EEXIST);
	EXPECT(!wrong_index);
	for (i = 0; i < NR; i++) {
		void *want = big_present ? &big : NULL;

		if (!other_err && BASE + i == other_index)
			want = &other;
		EXPECT(cc_xa_load(XA, BASE + i) == want);
	}
	EXPECT(!(big_present && !other_err));
	EXPECT(node->count == occupied(node));
	EXPECT(!node->refcnt);
}

/* No second writer: the fixture itself must agree with the contract. */
static void alone(void)
{
	struct cc_xa_node *node = begin("alone");

	other_err = -EEXIST;
	wrong_index = false;
	EXPECT(!insert(BASE, ORDER, &big));
	show("after store", node);
	settled(node, true);
	EXPECT(!erase(BASE, ORDER));
	show("after delete", node);
	settled(node, false);
	end();
}

/* "A failed store or delete leaves no entry partially stored." */
static void store_meets_insert(void)
{
	unsigned int cas;
	unsigned long at;
	bool ran = true;

	for (cas = 0; ran; cas++) {
		for (at = 0; at < NR; at++) {
			char name[64];
			struct cc_xa_node *node;
			int err;

			snprintf(name, sizeof(name),
				 "store-meets-insert cas=%u insert=%lu",
				 cas, BASE + at);
			node = begin(name);
			arm(node, cas, at);
			err = insert(BASE, ORDER, &big);
			ran = fired();
			if (!ran)
				other_err = -EEXIST;
			show("after store", node);
			printf("  store error %d, insert error %d\n", err,
			       other_err);
			EXPECT(err == 0 || err == -EEXIST);
			settled(node, !err);
			end();
		}
	}
}

/* "A successful store or delete covers the whole range." */
static void delete_meets_insert(void)
{
	unsigned int cas;
	unsigned long at;
	bool ran = true;

	for (cas = 0; ran; cas++) {
		for (at = 0; at < NR; at++) {
			char name[64];
			struct cc_xa_node *node;

			snprintf(name, sizeof(name),
				 "delete-meets-insert cas=%u insert=%lu",
				 cas, BASE + at);
			node = begin(name);
			if (insert(BASE, ORDER, &big))
				abort();
			arm(node, cas, at);
			EXPECT(!erase(BASE, ORDER));
			ran = fired();
			if (!ran)
				other_err = -EEXIST;
			show("after delete", node);
			printf("  insert error %d\n", other_err);
			settled(node, false);
			end();
		}
	}
}

/*
 * A shadow is never multi-index: storing one over a range stores it in every
 * slot.  The split cases with a shadow therefore find nothing to split, and
 * check that a split attempted on stale information does no harm.
 */
#define SHADOW	cc_xa_mk_value(7)

static int split_entry(void *entry)
{
	CC_XA_STATE(xas, XA, BASE);
	int err;

	do {
		cc_xas_try_split(&xas, entry, ORDER);
	} while (cc_xas_nomem(&xas, GFP_KERNEL));
	err = cc_xas_error(&xas);
	cc_xas_clear_xa_node(&xas);
	return err;
}

static int split(void)
{
	return split_entry(SHADOW);
}

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

static void show_split(const char *when, struct cc_xa_node *node)
{
	unsigned long i;

	printf("  %s:", when);
	for (i = 0; i < NR; i++) {
		void *entry = node->slots[BASE + i];

		printf(" [%lu]=%s", BASE + i,
		       !entry ? "empty" :
		       cc_xa_is_sibling(entry) ? "sibling" :
		       entry == SHADOW ? "shadow" :
		       entry == &other ? "other" : "?");
	}
	printf(" count=%u occupied=%u values=%u value-slots=%u\n", node->count,
	       occupied(node), node->nr_values, value_slots(node));
}

static void counted(struct cc_xa_node *node)
{
	EXPECT(node->count == occupied(node));
	EXPECT(node->nr_values == value_slots(node));
	EXPECT(!node->refcnt);
}

static void split_alone(void)
{
	struct cc_xa_node *node = begin("split-alone");
	unsigned long i;

	if (insert(BASE, ORDER, SHADOW))
		abort();
	show_split("before split", node);
	counted(node);
	EXPECT(!split());
	show_split("after split", node);
	for (i = 0; i < NR; i++)
		EXPECT(node->slots[BASE + i] == SHADOW);
	counted(node);
	EXPECT(!replace_shadow(BASE + 1, &other));
	EXPECT(cc_xa_load(XA, BASE + 1) == &other);
	counted(node);
	end();
}

/* A second inserter: split the shadow, then take one index of it. */
static void store_other(void)
{
	int err = split();

	if (err && err != -EEXIST)
		abort();
	other_err = replace_shadow(other_index, &other);
}

/* "Other writers do not act on an intermediate state." */
static void split_meets_store(void)
{
	unsigned int write;
	unsigned long at, i;
	bool ran = true;

	for (write = 0; ran; write++) {
		for (at = 0; at < NR; at++) {
			char name[64];
			struct cc_xa_node *node;
			int err;

			snprintf(name, sizeof(name),
				 "split-meets-store write=%u store=%lu",
				 write, BASE + at);
			node = begin(name);
			if (insert(BASE, ORDER, SHADOW))
				abort();
			arm(node, write, at);
			intruder = store_other;
			err = split();
			ran = fired();
			show_split("after split", node);
			printf("  split error %d, store error %d\n", err,
			       other_err);
			if (ran) {
				EXPECT(err == 0 || err == -EEXIST);
				/* The second inserter's folio must survive. */
				EXPECT(!other_err);
				for (i = 0; i < NR; i++) {
					void *want = SHADOW;

					if (!other_err && i == at)
						want = &other;
					EXPECT(cc_xa_load(XA, BASE + i) ==
					       want);
				}
				counted(node);
			}
			end();
		}
	}
}

/* Reclaim or truncate removes the whole shadow. */
static void erase_shadow(void)
{
	other_err = erase(BASE, ORDER);
}

/* "A successful store or delete covers the whole range." */
static void split_meets_delete(void)
{
	unsigned int write;
	unsigned long i;
	bool ran = true;

	for (write = 0; ran; write++) {
		char name[64];
		struct cc_xa_node *node;
		int err;

		snprintf(name, sizeof(name), "split-meets-delete write=%u",
			 write);
		node = begin(name);
		if (insert(BASE, ORDER, SHADOW))
			abort();
		arm(node, write, 0);
		intruder = erase_shadow;
		err = split();
		ran = fired();
		show_split("after split", node);
		printf("  split error %d, delete error %d\n", err, other_err);
		if (ran) {
			EXPECT(err == 0 || err == -EEXIST);
			EXPECT(!other_err);
			for (i = 0; i < NR; i++) {
				void *entry = cc_xa_load(XA, BASE + i);

				EXPECT(!entry || entry == SHADOW);
			}
			counted(node);
		}
		end();
	}
}

/*
 * A multi-index shadow has no owner.  While one writer works on its sibling
 * slots, another replaces it with a folio of the same range in one CAS of the
 * canonical slot, and the sibling slots now belong to that folio.
 */
static void evict_big(void)
{
	CC_XA_STATE_ORDER(xas, XA, BASE, ORDER);

	if (insert(BASE, ORDER, &big))
		abort();
	cc_xas_store(&xas, SHADOW);
	if (cc_xas_error(&xas))
		abort();
	cc_xas_clear_xa_node(&xas);
}

static void refault_other(void)
{
	CC_XA_STATE_ORDER(xas, XA, BASE, ORDER);

	cc_xas_store_expected(&xas, SHADOW, &other);
	other_err = cc_xas_error(&xas);
	cc_xas_clear_xa_node(&xas);
}

static void shadow_meets_refault(bool splitting)
{
	unsigned int write;
	unsigned long i;
	bool ran = true;

	for (write = 0; ran; write++) {
		CC_XA_STATE_ORDER(xas, XA, BASE, ORDER);
		char name[64];
		struct cc_xa_node *node;
		int err;

		snprintf(name, sizeof(name), "shadow-%s-meets-refault write=%u",
			 splitting ? "split" : "delete", write);
		node = begin(name);
		evict_big();
		arm(node, write, 0);
		intruder = refault_other;
		if (splitting) {
			err = split();
		} else {
			cc_xas_store_expected(&xas, SHADOW, NULL);
			err = cc_xas_error(&xas);
			cc_xas_clear_xa_node(&xas);
		}
		ran = fired();
		show_split("after", node);
		printf("  error %d, refault error %d\n", err, other_err);
		if (ran) {
			EXPECT(err == 0 || err == -EEXIST);
			EXPECT(other_err == 0 || other_err == -EEXIST);
			/* A folio that was stored covers its whole range. */
			for (i = 0; i < NR && !other_err; i++)
				EXPECT(cc_xa_load(XA, BASE + i) == &other);
			counted(node);
		}
		end();
	}
}

static void shadow_delete_meets_refault(void)
{
	shadow_meets_refault(false);
}

static void shadow_split_meets_refault(void)
{
	shadow_meets_refault(true);
}

/* A second inserter takes one index, whatever it finds there. */
static void take_index(void)
{
	unsigned long i;

	other_err = replace_shadow(other_index, &other);
	if (other_err)
		other_err = insert(other_index, 0, &other);
	for (i = 0; i < NR; i++) {
		if (BASE + i != other_index && peek(BASE + i) == &other)
			wrong_index = true;
	}
}

/* The owner of a folio splits it, as mm/huge_memory.c does. */
static void folio_split_meets_insert(void)
{
	unsigned int write;
	unsigned long at, i;
	bool ran = true;

	for (write = 0; ran; write++) {
		for (at = 0; at < NR; at++) {
			char name[64];
			struct cc_xa_node *node;
			int err;

			snprintf(name, sizeof(name),
				 "folio-split-meets-insert write=%u insert=%lu",
				 write, BASE + at);
			node = begin(name);
			if (insert(BASE, ORDER, &big))
				abort();
			arm(node, write, at);
			intruder = take_index;
			err = split_entry(&big);
			ran = fired();
			if (!ran)
				other_err = -EEXIST;
			show("after split", node);
			printf("  split error %d, insert error %d\n", err,
			       other_err);
			EXPECT(!err);
			EXPECT(other_err == -EEXIST);
			EXPECT(!wrong_index);
			for (i = 0; i < NR; i++)
				EXPECT(node->slots[BASE + i] == &big);
			counted(node);
			end();
		}
	}
}

/* The owner of a folio replaces it with shadows, as eviction does. */
static void evict_meets_insert(void)
{
	unsigned int write;
	unsigned long at, i;
	bool ran = true;

	for (write = 0; ran; write++) {
		for (at = 0; at < NR; at++) {
			CC_XA_STATE_ORDER(xas, XA, BASE, ORDER);
			char name[64];
			struct cc_xa_node *node;

			snprintf(name, sizeof(name),
				 "evict-meets-insert write=%u insert=%lu",
				 write, BASE + at);
			node = begin(name);
			if (insert(BASE, ORDER, &big))
				abort();
			arm(node, write, at);
			intruder = take_index;
			cc_xas_store(&xas, SHADOW);
			EXPECT(!cc_xas_error(&xas));
			cc_xas_clear_xa_node(&xas);
			ran = fired();
			if (!ran)
				other_err = -EEXIST;
			show_split("after evict", node);
			printf("  insert error %d\n", other_err);
			EXPECT(other_err == 0 || other_err == -EEXIST);
			EXPECT(!wrong_index);
			for (i = 0; i < NR; i++) {
				void *want = SHADOW;

				if (!other_err && i == at)
					want = &other;
				EXPECT(node->slots[BASE + i] == want);
			}
			counted(node);
			end();
		}
	}
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

static void mark(unsigned long index, bool set);
static bool marked(unsigned long index);

static void far_alone(void)
{
	unsigned int order;

	for (order = CC_XA_CHUNK_SHIFT; order <= 8; order++) {
		CC_XA_STATE_ORDER(xas, XA, FAR, order);
		unsigned long nr = 1UL << order;
		char name[64];

		snprintf(name, sizeof(name), "far-alone order=%u", order);
		begin(name);
		other_err = -EEXIST;
		/* The leaves exist, deleted: an entry was here before. */
		EXPECT(!insert(FAR + 70, 0, &other));
		EXPECT(!erase(FAR + 70, 0));
		EXPECT(!insert(FAR + 5, 0, &other));
		EXPECT(!erase(FAR + 5, 0));

		EXPECT(!insert(FAR, order, &big));
		EXPECT(holds(FAR, nr, &big) == nr);
		EXPECT(!cc_xa_load(XA, FAR - 1) && !cc_xa_load(XA, FAR + nr));
		far_counted(nr);
		/* An occupied range refuses a second entry and keeps the first. */
		EXPECT(insert(FAR, order, &other) == -EEXIST);
		EXPECT(insert(FAR + nr - 1, 0, &other) == -EEXIST);
		EXPECT(holds(FAR, nr, &big) == nr);

		/* Delete with the order, as page_cache_delete() does. */
		EXPECT(!erase(FAR, order));
		EXPECT(holds(FAR, nr, NULL) == nr);

		/*
		 * Replace from one index alone, as a migration does: the
		 * new entry takes every leaf, and the marks stay.
		 */
		EXPECT(!insert(FAR, order, &big));
		mark(FAR, true);
		EXPECT(!store_order(FAR, 0, &other));
		EXPECT(holds(FAR, nr, &other) == nr);
		EXPECT(!store_order(FAR + nr - 1, 0, &big));
		EXPECT(holds(FAR, nr, &big) == nr);
		EXPECT(marked(FAR) && marked(FAR + nr - 1));
		EXPECT(!cc_xa_load(XA, FAR - 1) && !cc_xa_load(XA, FAR + nr));
		far_counted(nr);
		EXPECT(!erase(FAR, order));
		EXPECT(holds(FAR, nr, NULL) == nr);

		/* Delete from the first index alone, as a batch delete does. */
		EXPECT(!insert(FAR, order, &big));
		EXPECT(!erase(FAR, 0));
		EXPECT(holds(FAR, nr, NULL) == nr);

		/* One index in the way refuses the whole entry. */
		EXPECT(!insert(FAR + nr - 1, 0, &other));
		EXPECT(insert(FAR, order, &big) == -EEXIST);
		EXPECT(holds(FAR, nr, &big) == 0);
		EXPECT(cc_xa_load(XA, FAR + nr - 1) == &other);
		far_counted(nr);
		EXPECT(!erase(FAR + nr - 1, 0));

		/* Eviction, then a refault of the same range over the shadows. */
		EXPECT(!insert(FAR, order, &big));
		EXPECT(!store_order(FAR, order, SHADOW));
		EXPECT(holds(FAR, nr, SHADOW) == nr);
		far_counted(nr);
		cc_xas_store_expected(&xas, SHADOW, &other);
		EXPECT(!cc_xas_error(&xas));
		cc_xas_clear_xa_node(&xas);
		EXPECT(holds(FAR, nr, &other) == nr);
		far_counted(nr);
		EXPECT(!erase(FAR, order));
		EXPECT(holds(FAR, nr, NULL) == nr);
		end();
	}
}

/* The owner splits, one step at a time as mm/huge_memory.c does. */
static void far_split(void)
{
	CC_XA_STATE(xas, XA, FAR + 70);
	unsigned int order = 8, next;
	unsigned long i;

	begin("far-split");
	EXPECT(!insert(FAR, order, &big));
	while (order) {
		next = cc_xas_try_split_min_order(order);
		cc_xas_set_order(&xas, FAR + 70, next);
		cc_xas_try_split(&xas, &big, order);
		EXPECT(!cc_xas_error(&xas));
		cc_xas_clear_xa_node(&xas);
		order = next;
	}
	EXPECT(holds(FAR, 256, &big) == 256);
	EXPECT(cc_xa_get_order(XA, FAR + 70) == 0);
	far_counted(256);
	/* Each index of the split leaf can now go on its own. */
	EXPECT(!erase(FAR + 70, 0));
	EXPECT(!cc_xa_load(XA, FAR + 70));
	EXPECT(cc_xa_load(XA, FAR + 71) == &big);
	for (i = 64; i < 128; i++)
		if (i != 70)
			EXPECT(!erase(FAR + i, 0));

	/* A uniform split reaches every leaf. */
	cc_xas_set_order(&xas, FAR, 0);
	cc_xas_split(&xas, &big, 6);
	EXPECT(!cc_xas_error(&xas));
	cc_xas_clear_xa_node(&xas);
	EXPECT(cc_xa_get_order(XA, FAR + 1) == 0);
	EXPECT(holds(FAR, 64, &big) == 64);
	cc_xas_set_order(&xas, FAR + 128, 2);
	cc_xas_split(&xas, &big, 7);
	EXPECT(!cc_xas_error(&xas));
	cc_xas_clear_xa_node(&xas);
	EXPECT(cc_xa_get_order(XA, FAR + 128) == 2);
	EXPECT(cc_xa_get_order(XA, FAR + 252) == 2);

	/* A split that has nothing to do still drops the references. */
	{
		CC_XA_STATE_ORDER(far, XA, (FAR + 64), CC_XA_CHUNK_SHIFT);

		EXPECT(!store_order(FAR + 64, CC_XA_CHUNK_SHIFT, &big));
		cc_xas_load(&far, false);
		cc_xas_try_split(&far, &big, CC_XA_CHUNK_SHIFT);
		EXPECT(!cc_xas_error(&far));
		cc_xas_clear_xa_node(&far);
		cc_xas_load(&far, false);
		cc_xas_split(&far, &big, CC_XA_CHUNK_SHIFT);
		EXPECT(!cc_xas_error(&far));
		cc_xas_clear_xa_node(&far);
	}
	EXPECT(holds(FAR + 128, 128, &big) == 128);
	far_counted(256);
	end();
}

static unsigned int walk_marked(unsigned long start, void **seen)
{
	CC_XA_STATE(xas, XA, start);
	unsigned int n = 0;
	void *entry;

	cc_xas_for_each_marked(&xas, entry, ULONG_MAX, CC_XA_MARK_0) {
		if (n == 8)
			break;
		seen[n++] = entry;
	}
	cc_xas_clear_xa_node(&xas);
	return n;
}

static void mark(unsigned long index, bool set)
{
	CC_XA_STATE(xas, XA, index);

	cc_xas_load(&xas, true);
	if (set)
		cc_xas_set_mark(&xas, CC_XA_MARK_0);
	else
		cc_xas_clear_mark(&xas, CC_XA_MARK_0);
	cc_xas_clear_xa_node(&xas);
}

static bool marked(unsigned long index)
{
	CC_XA_STATE(xas, XA, index);
	bool ret;

	cc_xas_load(&xas, true);
	ret = cc_xas_get_mark(&xas, CC_XA_MARK_0);
	cc_xas_clear_xa_node(&xas);
	return ret;
}

/*
 * A store of several leaves fails for memory only before it has changed a
 * slot, as upstream xas_store() does: its leaves are created first.
 */
static void far_nomem(void)
{
	unsigned int nomem = 0;

	/* The first leaf exists, the second one does not. */
	begin("far-nomem value");
	EXPECT(!insert(FAR + 5, 0, &other));
	EXPECT(!erase(FAR + 5, 0));
	{
		CC_XA_STATE_ORDER(xas, XA, FAR, FAR_ORDER);

		for (;;) {
			cc_xas_store(&xas, SHADOW);
			if (!cc_xas_error(&xas))
				break;
			nomem++;
			EXPECT(cc_xas_error(&xas) == -ENOMEM);
			EXPECT(holds(FAR, FAR_NR, NULL) == FAR_NR);
			if (!cc_xas_nomem(&xas, GFP_KERNEL))
				break;
		}
		EXPECT(!cc_xas_error(&xas));
		cc_xas_clear_xa_node(&xas);
	}
	EXPECT(nomem);
	EXPECT(holds(FAR, FAR_NR, SHADOW) == FAR_NR);
	far_counted(FAR_NR);
	end();

	/* A refault over a shadow: the retry still finds the shadow. */
	begin("far-nomem refault");
	EXPECT(!insert(FAR, 0, SHADOW));
	nomem = 0;
	{
		CC_XA_STATE_ORDER(xas, XA, FAR, FAR_ORDER);

		for (;;) {
			cc_xas_store_expected(&xas, SHADOW, &big);
			if (!cc_xas_error(&xas))
				break;
			nomem++;
			EXPECT(cc_xa_load(XA, FAR) == SHADOW);
			if (!cc_xas_nomem(&xas, GFP_KERNEL))
				break;
		}
		EXPECT(!cc_xas_error(&xas));
		cc_xas_clear_xa_node(&xas);
	}
	EXPECT(nomem);
	EXPECT(holds(FAR, FAR_NR, &big) == FAR_NR);
	far_counted(FAR_NR);
	end();

	/* A value that cannot take a later leaf leaves the earlier one. */
	begin("far-refused value");
	EXPECT(!insert(FAR + 100, 0, &other));
	{
		CC_XA_STATE_ORDER(xas, XA, FAR, FAR_ORDER);

		do {
			cc_xas_store_expected(&xas, NULL, SHADOW);
		} while (cc_xas_nomem(&xas, GFP_KERNEL));
		EXPECT(cc_xas_error(&xas) == -EEXIST);
		cc_xas_clear_xa_node(&xas);
	}
	EXPECT(holds(FAR, FAR_NR, NULL) == FAR_NR - 1);
	EXPECT(cc_xa_load(XA, FAR + 100) == &other);
	far_counted(FAR_NR);
	end();
}

static int small[NR];

/*
 * cc_xas_store() of NULL clears the range of the state whatever it holds, as
 * xas_store() does.  cc_xas_store_expected() names one entry and removes that
 * entry alone.  The state keeps its range for the next operation.
 */
static void delete_range(void)
{
	struct cc_xa_node *node = begin("delete-range");
	void *seen[8];
	unsigned long i;

	for (i = 0; i < NR; i++)
		EXPECT(!insert(BASE + i, 0, &small[i]));
	EXPECT(!erase(BASE, ORDER));
	EXPECT(holds(BASE, NR, NULL) == NR);
	counted(node);

	/* Values are one entry per index. */
	EXPECT(!insert(BASE, ORDER, SHADOW));
	EXPECT(!erase(BASE, ORDER));
	EXPECT(holds(BASE, NR, NULL) == NR);
	counted(node);

	/* A state smaller than the entry removes all of the entry. */
	EXPECT(!insert(BASE, ORDER, &big));
	EXPECT(!erase(BASE, 1));
	EXPECT(holds(BASE, NR, NULL) == NR);

	/* An entry that begins after the range stays. */
	EXPECT(!insert(BASE, 0, &other));
	EXPECT(!insert(BASE + 2, 1, &big));
	EXPECT(!erase(BASE, 1));
	EXPECT(holds(BASE, 2, NULL) == 2 && holds(BASE + 2, 2, &big) == 2);
	EXPECT(!erase(BASE + 2, 1));
	counted(node);

	/* The delete that names its entry. */
	for (i = 0; i < NR; i++)
		EXPECT(!insert(BASE + i, 0, &small[i]));
	{
		CC_XA_STATE_ORDER(xas, XA, BASE, ORDER);

		cc_xas_store_expected(&xas, &small[0], NULL);
		EXPECT(!cc_xas_error(&xas));
		cc_xas_clear_xa_node(&xas);
	}
	EXPECT(!cc_xa_load(XA, BASE));
	for (i = 1; i < NR; i++)
		EXPECT(cc_xa_load(XA, BASE + i) == &small[i]);
	counted(node);
	end();

	begin("delete-range far");
	EXPECT(!insert(FAR + 5, 0, &small[0]));
	EXPECT(!insert(FAR + 70, 0, SHADOW));
	EXPECT(!insert(FAR + 127, 0, &small[1]));
	EXPECT(!insert(FAR + FAR_NR, 0, &other));
	EXPECT(!erase(FAR, FAR_ORDER));
	EXPECT(holds(FAR, FAR_NR, NULL) == FAR_NR);
	EXPECT(cc_xa_load(XA, FAR + FAR_NR) == &other);
	far_counted(FAR_NR);

	/*
	 * page_cache_delete() stores the shadow and clears the marks through
	 * the same state.  The state still names the range afterwards.
	 */
	EXPECT(!insert(FAR, FAR_ORDER, &big));
	mark(FAR, true);
	{
		CC_XA_STATE_ORDER(xas, XA, FAR, FAR_ORDER);

		cc_xas_store(&xas, SHADOW);
		EXPECT(!cc_xas_error(&xas));
		cc_xas_init_marks(&xas);
		EXPECT(holds(FAR, FAR_NR, SHADOW) == FAR_NR);
		EXPECT(!walk_marked(0, seen));
		cc_xas_store(&xas, NULL);
		EXPECT(!cc_xas_error(&xas));
		cc_xas_clear_xa_node(&xas);
	}
	EXPECT(holds(FAR, FAR_NR, NULL) == FAR_NR);
	far_counted(FAR_NR);

	/* The same after a store of a pointer entry. */
	{
		CC_XA_STATE_ORDER(xas, XA, FAR, FAR_ORDER);

		do {
			cc_xas_store_expected(&xas, NULL, &big);
		} while (cc_xas_nomem(&xas, GFP_KERNEL));
		EXPECT(!cc_xas_error(&xas));
		cc_xas_store(&xas, NULL);
		EXPECT(!cc_xas_error(&xas));
		cc_xas_clear_xa_node(&xas);
	}
	EXPECT(holds(FAR, FAR_NR, NULL) == FAR_NR);
	far_counted(FAR_NR);
	end();
}

static void far_other(void)
{
	unsigned long i, bigs = holds(FAR, FAR_NR, &big);

	nr_partial += bigs && bigs < FAR_NR;
	other_err = insert(other_index, 0, &other);
	for (i = 0; i < FAR_NR; i++) {
		if (FAR + i != other_index && peek(FAR + i) == &other)
			wrong_index = true;
	}
}

static void far_settled(bool big_present)
{
	unsigned long want = FAR_NR - (other_err ? 0 : 1);

	EXPECT(other_err == 0 || other_err == -EEXIST);
	EXPECT(!wrong_index);
	EXPECT(!(big_present && !other_err));
	EXPECT(holds(FAR, FAR_NR, big_present ? &big : NULL) == want);
	if (!other_err)
		EXPECT(cc_xa_load(XA, other_index) == &other);
	far_counted(FAR_NR);
}

/* Every slot write of an order-7 store or delete, against one insert. */
static void far_meets_insert(bool deleting)
{
	static const unsigned long at[] = { 0, 1, 63, 64, 100, 127 };
	unsigned int write, a;
	bool ran = true;

	for (write = 0; ran; write++) {
		for (a = 0; a < ARRAY_SIZE(at); a++) {
			int err;

			cc_xa_init(XA);
			if (insert(1, 0, &anchor))
				abort();
			if (deleting && insert(FAR, FAR_ORDER, &big))
				abort();
			other_index = FAR + at[a];
			other_err = 1;
			wrong_index = false;
			countdown = write;
			watch_all = true;
			intruder = far_other;
			err = deleting ? erase(FAR, FAR_ORDER) :
					 insert(FAR, FAR_ORDER, &big);
			watch_all = false;
			ran = fired();
			if (!ran)
				other_err = -EEXIST;
			if (deleting)
				EXPECT(!err);
			else
				EXPECT(err == 0 || err == -EEXIST);
			far_settled(!deleting && !err);
			if (failures) {
				printf("  %s write=%u insert=%lu: error %d, insert error %d\n",
				       deleting ? "delete" : "store", write,
				       other_index, err, other_err);
				end();
				return;
			}
			end();
		}
	}
	printf("  %u slot writes\n", write - 1);
}

static void far_store_meets_insert(void)
{
	far_meets_insert(false);
}

static void far_delete_meets_insert(void)
{
	far_meets_insert(true);
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

static void *shadow_writer(void *arg)
{
	struct worker *w = arg;

	rcu_register_thread();
	while (w->done + w->lost < attempts) {
		CC_XA_STATE_ORDER(xas, XA, BASE, ORDER);
		int err;

		BREATHE(w->done + w->lost);
		if (insert(BASE, ORDER, &big)) {
			w->lost++;
			continue;
		}
		/*
		 * Eviction: the folio becomes a shadow of the same range in
		 * one step.  A multi-index shadow never arrives slot by slot.
		 */
		cc_xas_store(&xas, SHADOW);
		if (cc_xas_error(&xas))
			abort();
		cc_xas_clear_xa_node(&xas);
		cc_xas_set_order(&xas, BASE, ORDER);
		cc_xas_store_expected(&xas, SHADOW, NULL);
		err = cc_xas_error(&xas);
		cc_xas_clear_xa_node(&xas);
		if (err && err != -EEXIST)
			abort();
		w->done++;
	}
	rcu_unregister_thread();
	return NULL;
}

static void *splitting_writer(void *arg)
{
	struct worker *w = arg;
	unsigned long index = BASE + w->index;

	rcu_register_thread();
	while (!stop) {
		int err;

		BREATHE(w->done + w->lost);
		err = split();
		if (err && err != -EEXIST)
			abort();
		if (replace_shadow(index, &small[w->index]) &&
		    insert(index, 0, &small[w->index])) {
			w->lost++;
			continue;
		}
		if (erase(index, 0))
			abort();
		w->done++;
	}
	rcu_unregister_thread();
	return NULL;
}

static void *big_writer(void *arg)
{
	struct worker *w = arg;

	rcu_register_thread();
	while (w->done + w->lost < attempts) {
		BREATHE(w->done + w->lost);
		if (insert(BASE, ORDER, &big)) {
			w->lost++;
			continue;
		}
		if (erase(BASE, ORDER))
			abort();
		w->done++;
	}
	rcu_unregister_thread();
	return NULL;
}

static void *small_writer(void *arg)
{
	struct worker *w = arg;

	rcu_register_thread();
	while (!stop) {
		BREATHE(w->done + w->lost);
		if (insert(BASE + w->index, 0, &small[w->index])) {
			w->lost++;
			continue;
		}
		if (erase(BASE + w->index, 0))
			abort();
		w->done++;
	}
	rcu_unregister_thread();
	return NULL;
}

static void *reader(void *arg)
{
	struct worker *w = arg;
	unsigned long i;

	rcu_register_thread();
	while (!stop) {
		BREATHE(w->loads >> 2);
		for (i = 0; i < NR; i++) {
			void *entry = peek(BASE + i);

			w->loads++;
			if (entry && entry != &big && entry != SHADOW &&
			    entry != &small[i])
				__sync_fetch_and_add(&stress_wrong, 1);
		}
	}
	rcu_unregister_thread();
	return NULL;
}

static void stress(unsigned long nr, bool shadow)
{
	struct cc_xa_node *node = begin(shadow ? "stress-split" : "stress");
	struct worker big_w = { 0 }, small_w[NR] = { { 0 } }, read_w[2] = { { 0 } };
	unsigned long i, small_done = 0, small_lost = 0, loads = 0;

	other_err = -EEXIST;
	wrong_index = false;
	attempts = nr;
	pthread_create(&big_w.thread, NULL,
		       shadow ? shadow_writer : big_writer, &big_w);
	for (i = 0; i < NR; i++) {
		small_w[i].index = i;
		pthread_create(&small_w[i].thread, NULL,
			       shadow ? splitting_writer : small_writer,
			       &small_w[i]);
	}
	for (i = 0; i < 2; i++)
		pthread_create(&read_w[i].thread, NULL, reader, &read_w[i]);
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
	show("at rest", node);
	printf("  order-2 stored %lu, refused %lu\n", big_w.done, big_w.lost);
	printf("  order-0 stored %lu, refused %lu\n", small_done, small_lost);
	printf("  lookups %lu, wrong entry %lu\n", loads, stress_wrong);
	EXPECT(!stress_wrong);
	EXPECT(big_w.done && small_done);
	if (shadow) {
		/* A shadow split off before its delete may stay behind. */
		show_split("at rest", node);
		for (i = 0; i < NR; i++) {
			void *entry = cc_xa_load(XA, BASE + i);

			EXPECT(!entry || entry == SHADOW);
		}
		counted(node);
	} else {
		settled(node, false);
	}
	end();
}

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
		{ "alone", alone },
		{ "store-meets-insert", store_meets_insert },
		{ "delete-meets-insert", delete_meets_insert },
		{ "split-alone", split_alone },
		{ "split-meets-store", split_meets_store },
		{ "split-meets-delete", split_meets_delete },
		{ "folio-split-meets-insert", folio_split_meets_insert },
		{ "evict-meets-insert", evict_meets_insert },
		{ "shadow-delete-meets-refault", shadow_delete_meets_refault },
		{ "shadow-split-meets-refault", shadow_split_meets_refault },
		{ "far-alone", far_alone },
		{ "far-split", far_split },
		{ "far-store-meets-insert", far_store_meets_insert },
		{ "far-delete-meets-insert", far_delete_meets_insert },
		{ "far-nomem", far_nomem },
		{ "delete-range", delete_range },
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
	if (argc > 1 && !strncmp(argv[1], "stress", 6)) {
		stress(argc > 2 ? strtoul(argv[2], NULL, 0) : 2000000,
		       !strcmp(argv[1], "stress-split"));
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
