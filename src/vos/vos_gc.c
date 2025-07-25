/**
 * (C) Copyright 2019-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of daos
 *
 * vos/vos_gc.c
 */
#define D_LOGFAC	DD_FAC(vos)

#include <daos/btree.h>
#include <daos/mem.h>
#include <daos_srv/vos.h>
#include "vos_internal.h"

enum {
	GC_CREDS_MIN	= 1,	/**< minimum credits for vos_gc_run/pool() */
	GC_CREDS_SLACK	= 8,	/**< credits for slack mode */
	GC_CREDS_TIGHT	= 32,	/**< credits for tight mode */
	GC_CREDS_MAX	= 4096,	/**< maximum credits for vos_gc_run/pool() */
};

/**
 * Default garbage bag size consumes <= 4K space
 * - header of vos_gc_bag_df is 64 bytes
 * - PMDK allocation overhead is 16 bytes,
 * - each item consumes 16 bytes, 250 * 16 = 4000 bytes
 * - together is 4080 bytes, reserve 16 bytes for future use
 */
static int gc_bag_size	= 250 + 3 * 256;

/** VOS garbage collector */
struct vos_gc {
	const char		 *gc_name;
	/** type of the GC, see @vos_gc_type */
	enum vos_gc_type	  gc_type;
	/**
	 * default execution credits for drain \a gc_drain.
	 * GC consumes user credits if this member is set to zero.
	 */
	const int		  gc_drain_creds;
	/**
	 * drain an item (release its children) collected by the current GC
	 * Release sub-items for @item, sub-item could be:
	 * - values of an akey
	 * - akeys of a dkey
	 * - dkeys of an object
	 * - objects of a container
	 *
	 * This function should return if @item has no more sub-items, or
	 * consumed all @credits (releasing a sub-item consumes a credit).
	 * @empty is set to true if all sub-items have been drained,
	 * otherwise it's set to false.
	 */
	int			(*gc_drain)(struct vos_gc *gc,
					    struct vos_pool *pool,
					    daos_handle_t coh,
					    struct vos_gc_item *item,
					    int *credits, bool *empty);
	/**
	 * free an item collected by the current GC
	 * It is unused for now, but we might need it if we want to support
	 * GC_BIO, see comments in vos_gc_type.
	 */
	int (*gc_free)(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh,
		       struct vos_gc_item *item);
};

/**
 * drain items stored in btree, this function returns when the btree is empty,
 * or all credits are consumed (releasing a leaf record consumes one credit)
 */
static int
gc_drain_btr(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh,
	     struct btr_root *root, int *credits, bool *empty)
{
	daos_handle_t	toh;
	int		rc;

	rc = dbtree_open_inplace_ex(root, &pool->vp_uma, coh, pool, &toh);
	if (rc == -DER_NONEXIST) { /* empty tree */
		*empty = true;
		return 0;
	}
	if (rc)
		goto failed;

	D_DEBUG(DB_TRACE, "drain btree for %s, creds=%d\n",
		gc->gc_name, *credits);
	rc = dbtree_drain(toh, credits, vos_hdl2cont(coh), empty);
	dbtree_close(toh);
	if (rc)
		goto failed;

	D_ASSERT(*credits >= 0);
	D_ASSERT(*empty || *credits == 0);
	D_DEBUG(DB_TRACE, "empty=%d, remainded creds=%d\n", *empty, *credits);
	return 0;
 failed:
	D_ERROR("Failed to drain %s btree: " DF_RC "\n", gc->gc_name, DP_RC(rc));
	return rc;
}

/**
 * drain items stored in evtree, this function returns when the evtree is empty,
 * or all credits are consumed (releasing a leaf record consumes one credit)
 */
static int
gc_drain_evt(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh,
	     struct evt_root *root, int *credits, bool *empty)
{
	struct evt_desc_cbs cbs;
	daos_handle_t	    toh;
	int		    rc;

	vos_evt_desc_cbs_init(&cbs, pool, coh);
	rc = evt_open(root, &pool->vp_uma, &cbs, &toh);
	if (rc == -DER_NONEXIST) {
		*empty = true;
		return 0;
	}
	if (rc)
		goto failed;

	D_DEBUG(DB_TRACE, "drain %s evtree, creds=%d\n", gc->gc_name, *credits);
	rc = evt_drain(toh, credits, empty);
	D_ASSERT(evt_close(toh) == 0);
	if (rc)
		goto failed;

	D_ASSERT(*credits >= 0);
	D_ASSERT(*empty || *credits == 0);
	D_DEBUG(DB_TRACE, "empty=%d, remainded creds=%d\n", *empty, *credits);
	return 0;
 failed:
	D_ERROR("Failed to drain evtree %s: " DF_RC "\n", gc->gc_name, DP_RC(rc));
	return rc;
}

/**
 * drain versioned values of a key, it returns when the value tree is empty,
 * or all credits are consumed (releasing a value consumes one credit)
 */
static int
gc_drain_key(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh,
	     struct vos_gc_item *item, int *credits, bool *empty)
{
	struct vos_krec_df *key = umem_off2ptr(&pool->vp_umm, item->it_addr);
	int                 creds = *credits;
	int		    rc;

	if (key->kr_bmap & KREC_BF_NO_AKEY && gc->gc_type == GC_DKEY) {
		/** Special case, this will defer to the free callback
		 *  and the tree will be inserted as akey.
		 */
		*empty = true;
		return 0;
	}

	if (key->kr_bmap & KREC_BF_BTR) {
		rc = gc_drain_btr(gc, pool, coh, &key->kr_btr, credits, empty);

	} else if (key->kr_bmap & KREC_BF_EVT) {
		D_ASSERT(gc->gc_type == GC_AKEY);
		rc = gc_drain_evt(gc, pool, coh, &key->kr_evt, credits, empty);

	} else { /* empty key generated by punch */
		*empty = true;
		return 0;
	}

	if (rc) {
		D_ERROR("%s drain failed: "DF_RC"\n", gc->gc_name, DP_RC(rc));
		return rc;
	}

	if (gc->gc_type == GC_DKEY)
		return 0;

	/* gather value stats for akey */
	creds -= *credits;
	if (key->kr_bmap & KREC_BF_BTR)
		pool->vp_gc_stat.gs_singvs += creds;
	else
		pool->vp_gc_stat.gs_recxs += creds;
	return 0;
}

static int
gc_free_dkey(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh, struct vos_gc_item *item)
{
	struct vos_krec_df *krec = umem_off2ptr(&pool->vp_umm, item->it_addr);

	D_ASSERT(krec->kr_bmap & KREC_BF_DKEY);
	if (krec->kr_bmap & KREC_BF_NO_AKEY)
		gc_add_item(pool, coh, GC_AKEY, item->it_addr, item->it_args);
	else
		umem_free(&pool->vp_umm, item->it_addr);
	return 0;
}

/**
 * drain all keys stored in an object, it returns when the key tree is empty,
 * or all credits are consumed (releasing a key consumes one credit)
 */
static int
gc_drain_obj(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh,
	     struct vos_gc_item *item, int *credits, bool *empty)
{
	struct vos_obj_df *obj = umem_off2ptr(&pool->vp_umm, item->it_addr);

	return gc_drain_btr(gc, pool, coh, &obj->vo_tree, credits, empty);
}

static int
gc_bags_move(struct vos_pool *pool, struct vos_gc_bin_df *dest_bin,
	     struct vos_gc_bin_df *src_bin)
{
	struct umem_instance	*umm = &pool->vp_umm;
	struct vos_gc_bag_df	*bag;
	int			 rc;

	rc = umem_tx_add_ptr(umm, dest_bin, sizeof(*dest_bin));
	if (rc != 0)
		return rc;

	bag = umem_off2ptr(umm, dest_bin->bin_bag_last);
	if (bag == NULL || bag->bag_item_nr == 0) {
		if (bag) {
			/* Old bag is empty */
			rc = umem_free(umm, dest_bin->bin_bag_last);
			if (rc != 0)
				return rc;
		}
		dest_bin->bin_bag_first = src_bin->bin_bag_first;
		dest_bin->bin_bag_last = src_bin->bin_bag_last;
		dest_bin->bin_bag_nr = src_bin->bin_bag_nr;
		if (!gc_have_pool(pool))
			gc_add_pool(pool);

		goto reset_src;
	}

	/** Last entry in pool list */
	bag = umem_off2ptr(umm, dest_bin->bin_bag_last);

	rc = umem_tx_add_ptr(umm, &bag->bag_next, sizeof(bag->bag_next));
	if (rc != 0)
		return rc;

	bag->bag_next = src_bin->bin_bag_first;
	dest_bin->bin_bag_last = src_bin->bin_bag_last;
	if (!gc_have_pool(pool))
		gc_add_pool(pool);

reset_src:
	rc = umem_tx_add_ptr(umm, src_bin, sizeof(*src_bin));
	if (rc != 0)
		return rc;

	src_bin->bin_bag_first = 0;
	src_bin->bin_bag_last = 0;
	src_bin->bin_bag_nr = 0;

	return 0;
}

/**
 * drain all objects stored in a container, it returns when the key tree is
 * empty, or all credits are consumed (releasing an object consumes one credit)
 */
static int
gc_drain_cont(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh,
	      struct vos_gc_item *item, int *credits, bool *empty)
{
	struct vos_gc_bin_df	*src_bin;
	struct vos_cont_df	*cont = umem_off2ptr(&pool->vp_umm,
						     item->it_addr);
	int			 i;
	int			 rc;

	/*
	 * When we prepaer to drain the container, we do not need DTX entry any long.
	 * Then destroy DTX table firstly to avoid dangling DXT records during drain
	 * the container (that may yield).
	 */
	rc = vos_dtx_table_destroy(&pool->vp_umm, cont);
	if (rc != 0)
		return rc;

	/** Move any leftover bags to the pool gc */
	for (i = GC_AKEY; i < GC_CONT; i++) {
		src_bin = &cont->cd_gc_bins[i];

		if (src_bin->bin_bag_first == UMOFF_NULL)
			continue;

		rc = gc_bags_move(pool, &pool->vp_pool_df->pd_gc_bins[i],
				  src_bin);
		if (rc != 0)
			return rc;

		/** Indicate to caller that we've taken over container bags */
		return 1;
	}

	D_ASSERT(daos_handle_is_inval(coh));
	return gc_drain_btr(gc, pool, coh, &cont->cd_obj_root,
			    credits, empty);
}

static int
gc_free_cont(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh, struct vos_gc_item *item)
{
	return umem_free(&pool->vp_umm, item->it_addr);
}

static struct vos_gc gc_table[] = {
    {
	.gc_name        = "akey",
	.gc_type        = GC_AKEY,
	.gc_drain_creds = 0, /* consume user credits */
	.gc_drain       = gc_drain_key,
	.gc_free        = NULL,
    },

    {
	.gc_name        = "dkey",
	.gc_type        = GC_DKEY,
	.gc_drain_creds = 32,
	.gc_drain       = gc_drain_key,
	.gc_free        = gc_free_dkey,
    },
    {
	.gc_name        = "object",
	.gc_type        = GC_OBJ,
	.gc_drain_creds = 8,
	.gc_drain       = gc_drain_obj,
	.gc_free        = NULL,
    },
    {
	.gc_name        = "container",
	.gc_type        = GC_CONT,
	.gc_drain_creds = 1,
	.gc_drain       = gc_drain_cont,
	.gc_free        = gc_free_cont,
    },
    {
	.gc_name = "unknown",
	.gc_type = GC_MAX,
    },
};

static const char *
gc_type2name(enum vos_gc_type type)
{
	D_ASSERT(type < GC_MAX);
	return gc_table[type].gc_name;
}

struct vos_gc_bin_df *
gc_type2bin(struct vos_pool *pool, struct vos_container *cont,
	    enum vos_gc_type type)
{
	D_ASSERT(type < GC_MAX);
	if (cont == NULL)
		return &pool->vp_pool_df->pd_gc_bins[type];

	D_ASSERT(type < GC_CONT);
	return &cont->vc_cont_df->cd_gc_bins[type];
}

/**
 * Free the first (oldest) garbage bag of a garbage bin unless it is also the
 * last (newest) bag.
 */
static int
gc_bin_free_bag(struct umem_instance *umm, struct vos_container *cont,
		struct vos_gc_bin_df *bin, umem_off_t bag_id)
{
	struct vos_gc_bag_df *bag = umem_off2ptr(umm, bag_id);
	int		      rc;

	D_ASSERT(bag_id == bin->bin_bag_first);
	if (cont == NULL && bag_id == bin->bin_bag_last) {
		/* don't free the last bag, only reset it */
		D_ASSERT(bin->bin_bag_nr == 1);
		rc = umem_tx_add_ptr(umm, bag, sizeof(*bag));
		if (rc == 0) {
			bag->bag_item_first = bag->bag_item_last = 0;
			bag->bag_item_nr = 0;
		}

		return rc;
	}

	if (cont != NULL) {
		D_ASSERT(bin->bin_bag_nr > 0);
	} else {
		D_ASSERT(bin->bin_bag_nr > 1);
		D_ASSERT(bag->bag_next != UMOFF_NULL);
	}

	rc = umem_tx_add_ptr(umm, bin, sizeof(*bin));
	if (rc == 0) {
		bin->bin_bag_first = bag->bag_next;
		bin->bin_bag_nr--;

		if (bag->bag_next == UMOFF_NULL)
			bin->bin_bag_last = UMOFF_NULL;

		rc = umem_free(umm, bag_id);
	}

	return rc;
}

/**
 * Returns the last (newest) garbage bag, it allocates a new bag if there
 * is no bag in the bin, or the last bag is full.
 */
struct vos_gc_bag_df *
gc_bin_find_bag(struct umem_instance *umm, struct vos_gc_bin_df *bin)
{
	struct vos_gc_bag_df *bag = NULL;
	umem_off_t	      bag_id;
	int		      size;
	int		      rc;

	if (!UMOFF_IS_NULL(bin->bin_bag_last)) {
		bag_id = bin->bin_bag_last;
		bag = umem_off2ptr(umm, bag_id);
		if (bag->bag_item_nr < bin->bin_bag_size)
			return bag;
	}

	/* allocate a new bag */
	size = offsetof(struct vos_gc_bag_df, bag_items[bin->bin_bag_size]);
	bag_id = umem_zalloc(umm, size);
	if (UMOFF_IS_NULL(bag_id))
		return NULL;

	rc = umem_tx_add_ptr(umm, bin, sizeof(*bin));
	if (rc != 0)
		return NULL;

	if (bag) { /* the original last bag */
		rc = umem_tx_add_ptr(umm, bag, sizeof(*bag));
		if (rc != 0)
			return NULL;

		bag->bag_next = bag_id;
	} else {
		/* this is a new bin */
		bin->bin_bag_first = bag_id;
	}

	bin->bin_bag_last = bag_id;
	bin->bin_bag_nr++;

	return umem_off2ptr(umm, bag_id);
}

static int
gc_bin_add_item(struct umem_instance *umm, struct vos_gc_bin_df *bin,
		struct vos_gc_item *item)
{
	struct vos_gc_bag_df *bag;
	struct vos_gc_item   *it;
	int		      last;
	int		      rc;

	bag = gc_bin_find_bag(umm, bin);
	if (!bag)
		return -DER_NOSPACE;

	D_ASSERT(bag->bag_item_nr < bin->bin_bag_size);
	/* NB: umem_tx_add with UMEM_XADD_NO_SNAPSHOT, this is totally
	 * safe because we never overwrite valid items
	 */
	it = &bag->bag_items[bag->bag_item_last];
	umem_tx_xadd_ptr(umm, it, sizeof(*it), UMEM_XADD_NO_SNAPSHOT);
	memcpy(it, item, sizeof(*it));

	last = bag->bag_item_last + 1;
	if (last == bin->bin_bag_size)
		last = 0;

	rc = umem_tx_add_ptr(umm, bag, sizeof(*bag));
	if (rc == 0) {
		bag->bag_item_last = last;
		bag->bag_item_nr += 1;
	}

	return rc;
}

static struct vos_gc_item *
gc_get_item(struct vos_gc *gc, struct vos_pool *pool,
	    struct vos_container *cont)
{
	struct vos_gc_bin_df	*bin = gc_type2bin(pool, cont, gc->gc_type);
	struct vos_gc_bag_df	*bag;

	bag = umem_off2ptr(&pool->vp_umm, bin->bin_bag_first);
	if (bag == NULL) /* empty bin */
		return NULL;

	if (bag->bag_item_nr == 0) { /* empty bag */
		D_ASSERT(bag->bag_next == UMOFF_NULL);
		return NULL;
	}

	return &bag->bag_items[bag->bag_item_first];
}

static int
gc_drain_item(struct vos_gc *gc, struct vos_pool *pool, daos_handle_t coh,
	      struct vos_gc_item *item, int *credits, bool *empty)
{
	int	creds;
	int	rc;

	if (!gc->gc_drain) {
		/* NB: all the current GC types have drain function, but the
		 * future BIO GC may not have drain function.
		 */
		*empty = true;
		return 0;
	}

	if (gc->gc_type == GC_AKEY) {
		creds = *credits;
	} else {
		/* do not consume user credits, because this only flatten its
		 * subtree and wouldn't free any user key/data.
		 */
		creds = gc->gc_drain_creds;
	}

	D_ASSERT(item->it_addr != 0);
	rc = gc->gc_drain(gc, pool, coh, item, &creds, empty);
	if (rc < 0)
		return rc;

	if (gc->gc_type == GC_AKEY) {
		/* single value/recx tree wouldn't be flatterned (might be
		 * changed in the future), instead they are freed within
		 * dbtree/evtree_drain(), so user credits should be consumed.
		 */
		D_ASSERT(*credits >= creds);
		*credits = creds;
	}
	return rc;
}

static int
gc_free_item(struct vos_gc *gc, struct vos_pool *pool,
	     struct vos_container *cont, struct vos_gc_item *item)
{
	struct vos_gc_bin_df *bin = gc_type2bin(pool, cont, gc->gc_type);
	struct vos_gc_bag_df *bag;
	int		      first;
	struct vos_gc_item    it;
	int		      rc = 0;

	bag = umem_off2ptr(&pool->vp_umm, bin->bin_bag_first);
	D_ASSERT(bag && bag->bag_item_nr > 0);
	D_ASSERT(item == &bag->bag_items[bag->bag_item_first]);
	it = *item;

	first = bag->bag_item_first + 1;
	if (first == bin->bin_bag_size)
		first = 0;

	if (first == bag->bag_item_last) {
		/* it's going to be a empty bag */
		D_ASSERT(bag->bag_item_nr == 1);
		rc = gc_bin_free_bag(&pool->vp_umm, cont, bin,
				     bin->bin_bag_first);
		if (rc)
			goto failed;
	} else {
		rc = umem_tx_add_ptr(&pool->vp_umm, bag, sizeof(*bag));
		if (rc)
			goto failed;

		bag->bag_item_first = first;
		bag->bag_item_nr--;
	}

	D_DEBUG(DB_TRACE, "GC released a %s\n", gc->gc_name);
	/* this is the real container|object|dkey|akey free */
	if (gc->gc_free)
		rc = gc->gc_free(gc, pool, vos_cont2hdl(cont), &it);
	else
		rc = umem_free(&pool->vp_umm, it.it_addr);

	if (rc != 0)
		goto failed;

	switch (gc->gc_type) {
	default:
		D_ASSERT(0);
		break;
	case GC_AKEY:
		pool->vp_gc_stat.gs_akeys++;
		break;
	case GC_DKEY:
		pool->vp_gc_stat.gs_dkeys++;
		break;
	case GC_OBJ:
		pool->vp_gc_stat.gs_objs++;
		break;
	case GC_CONT:
		pool->vp_gc_stat.gs_conts++;
		break;
	}
failed:
	return rc;
}

/**
 * Add an item for garbage collection, this item and all its sub-items will
 * be freed by vos_gc_run/pool().
 *
 * NB: this function must be called within pmdk transaction.
 */
int
gc_add_item(struct vos_pool *pool, daos_handle_t coh,
	    enum vos_gc_type type, umem_off_t item_off, uint64_t args)
{
	struct vos_container	*cont = vos_hdl2cont(coh);
	struct vos_gc_bin_df	*bin = gc_type2bin(pool, cont, type);
	struct vos_gc_item	 item;
	int			 rc;

	D_DEBUG(DB_TRACE, "Add %s addr="DF_X64"\n",
		gc_type2name(type), item_off);

	if (pool->vp_dying)
		return 0; /* OK to ignore because the pool is being deleted */

	item.it_addr = item_off;
	item.it_args = args;
	rc = gc_bin_add_item(&pool->vp_umm, bin, &item);
	if (rc) {
		D_ERROR("Failed to add item, pool=" DF_UUID ", rc=" DF_RC "\n",
			DP_UUID(pool->vp_id), DP_RC(rc));
		return rc;
	}

	if (!gc_have_pool(pool))
		gc_add_pool(pool);

	/** New item to remove from the container */
	if (cont != NULL && d_list_empty(&cont->vc_gc_link))
		d_list_add_tail(&cont->vc_gc_link, &pool->vp_gc_cont);

	return rc;
}

struct vos_container *
gc_get_container(struct vos_pool *pool)
{
	struct vos_container	*cont;
	/** In order to be fair to other containers, we remove this from the
	 * list.  If we run out of credits, we will put it at the back of
	 * the list and give another container a turn next time.
	 */
	cont = d_list_pop_entry(&pool->vp_gc_cont, struct vos_container,
				vc_gc_link);
	if (DAOS_FAIL_CHECK(DAOS_VOS_GC_CONT_NULL))
		D_ASSERT(cont == NULL);

	return cont;
}

static void
gc_update_stats(struct vos_pool *pool)
{
	struct vos_gc_stat    *stat  = &pool->vp_gc_stat;
	struct vos_gc_stat    *gstat = &pool->vp_gc_stat_global;
	struct vos_gc_metrics *vgm;

	if (pool->vp_metrics != NULL) {
		vgm = &pool->vp_metrics->vp_gc_metrics;
		d_tm_inc_counter(vgm->vgm_cont_del, stat->gs_conts);
		d_tm_inc_counter(vgm->vgm_obj_del, stat->gs_objs);
		d_tm_inc_counter(vgm->vgm_dkey_del, stat->gs_dkeys);
		d_tm_inc_counter(vgm->vgm_akey_del, stat->gs_akeys);
		d_tm_inc_counter(vgm->vgm_ev_del, stat->gs_recxs);
		d_tm_inc_counter(vgm->vgm_sv_del, stat->gs_singvs);
	}

	gstat->gs_conts += stat->gs_conts;
	gstat->gs_objs += stat->gs_objs;
	gstat->gs_dkeys += stat->gs_dkeys;
	gstat->gs_akeys += stat->gs_akeys;
	gstat->gs_recxs += stat->gs_recxs;
	gstat->gs_singvs += stat->gs_singvs;

	memset(stat, 0, sizeof(*stat));
}

/**
 * Run garbage collector for a pool, it returns if all @credits are consumed
 * or there is nothing to be reclaimed.
 */
static int
gc_reclaim_pool(struct vos_pool *pool, int *credits, bool *empty_ret)
{
	struct vos_container	*cont = gc_get_container(pool);
	struct vos_gc		*gc    = &gc_table[0]; /* start from akey */
	int			 creds = *credits;
	int			 rc;

	if (pool->vp_dying) {
		*empty_ret = true;
		D_GOTO(done, rc = 0);
	}

	/* take an extra ref to avoid concurrent container destroy/free */
	if (cont != NULL)
		vos_cont_addref(cont);

	rc = umem_tx_begin(&pool->vp_umm, NULL);
	if (rc) {
		D_ERROR("Failed to start transacton for " DF_UUID ": " DF_RC "\n",
			DP_UUID(pool->vp_id), DP_RC(rc));
		if (cont != NULL)
			vos_cont_decref(cont);
		*empty_ret = false;
		goto done;
	}

	*empty_ret = false;
	while (creds > 0) {
		struct vos_gc_item *item;
		bool		    empty = false;

		D_DEBUG(DB_TRACE, "GC=%s cont=%p credits=%d/%d\n", gc->gc_name,
			cont, creds, *credits);

		item = gc_get_item(gc, pool, cont);

		if (item == NULL) {
			if (cont != NULL) {
				if (gc->gc_type == GC_OBJ) { /* top level GC */
					D_DEBUG(DB_TRACE, "container %p objects"
						" reclaimed\n", cont);
					vos_cont_decref(cont);
					cont = gc_get_container(pool);
					/* take a ref on new cont */
					if (cont != NULL)
						vos_cont_addref(cont);
					gc = &gc_table[0]; /* reset to akey */
					continue;
				}
			} else if (gc->gc_type == GC_CONT) { /* top level GC */
				D_DEBUG(DB_TRACE, "Nothing to reclaim\n");
				*empty_ret = true;
				break;
			}
			D_DEBUG(DB_TRACE, "GC=%s is empty\n", gc->gc_name);
			gc++; /* try upper level tree */
			continue;
		}

		if (DAOS_FAIL_CHECK(DAOS_VOS_GC_CONT))
			D_ASSERT(cont != NULL);

		rc = gc_drain_item(gc, pool, vos_cont2hdl(cont), item, &creds,
				   &empty);
		if (rc < 0) {
			D_ERROR("GC=%s error: " DF_RC "\n", gc->gc_name, DP_RC(rc));
			break;
		}

		if (empty && creds) {
			/* item can be released and removed from bin */
			rc = gc_free_item(gc, pool, cont, item);
			if (rc) {
				D_ERROR("GC=%s free item error: "DF_RC"\n", gc->gc_name, DP_RC(rc));
				break;
			}
			creds--;
		}

		D_DEBUG(DB_TRACE, "GC=%s credits=%d empty=%d\n",
			gc->gc_name, creds, empty);

		if (rc == 1) {
			/** We moved some container entries to the pool,
			 *  so reset to akey level and start over.
			 */
			gc = &gc_table[0];
			continue;
		}

		/* always try to free akeys and values because they are the
		 * items consuming most storage space.
		 */
		if (gc->gc_type == GC_AKEY)
			continue;

		/* should have flattened some items to the child GC, switch
		 * to the child GC.
		 */
		gc--;
	}
	D_DEBUG(DB_TRACE,
		"pool="DF_UUID", creds origin=%d, current=%d, rc=%s\n",
		DP_UUID(pool->vp_id), *credits, creds, d_errstr(rc));

	rc = umem_tx_end(&pool->vp_umm, rc);
	if (rc == 0)
		*credits = creds;

	if (cont != NULL && d_list_empty(&cont->vc_gc_link)) {
		/** The container may not be empty so add it back to end of
		 *  the list.
		 */
		d_list_add_tail(&cont->vc_gc_link, &pool->vp_gc_cont);
	}

	/* hopefully if last ref cont_free() will dequeue it */
	if (cont != NULL)
		vos_cont_decref(cont);

done:
	gc_update_stats(pool);

	return rc;
}

/**
 * Initialize garbage bins for a pool.
 *
 * NB: there is no need to free garbage bins, because destroy pool will free
 * them for free.
 */
int
gc_init_pool(struct umem_instance *umm, struct vos_pool_df *pd)
{
	int		i;
	umem_off_t	bag_id;
	int		size;
	int		rc;

	D_DEBUG(DB_IO, "Init garbage bins for pool="DF_UUID"\n",
		DP_UUID(pd->pd_id));

	for (i = 0; i < GC_MAX; i++) {
		struct vos_gc_bin_df *bin = &pd->pd_gc_bins[i];

		size = offsetof(struct vos_gc_bag_df, bag_items[gc_bag_size]);
		bag_id = umem_zalloc(umm, size);
		if (UMOFF_IS_NULL(bag_id))
			return -DER_NOMEM;

		rc = umem_tx_add_ptr(umm, bin, sizeof(*bin));
		if (rc != 0)
			return rc;

		bin->bin_bag_size  = gc_bag_size;
		bin->bin_bag_first = bag_id;
		bin->bin_bag_last = bag_id;
		bin->bin_bag_nr = 1;
	}
	return 0;
}

/**
 * Initialize garbage bins for a pool
 *
 * NB: there is no need to free garbage bins, because destroy container will
 * free them for free.
 */
int
gc_init_cont(struct umem_instance *umm, struct vos_cont_df *cd)
{
	int	i;

	D_DEBUG(DB_IO, "Init garbage bins for cont="DF_UUID"\n",
		DP_UUID(cd->cd_id));

	for (i = 0; i < GC_CONT; i++) {
		struct vos_gc_bin_df *bin = &cd->cd_gc_bins[i];

		bin->bin_bag_first = UMOFF_NULL;
		bin->bin_bag_last  = UMOFF_NULL;
		bin->bin_bag_size  = gc_bag_size;
		bin->bin_bag_nr	   = 0;
	}
	return 0;
}

/**
 * Check if newly opened container needs to be added to garbage collection list
 */
void
gc_check_cont(struct vos_container *cont)
{
	int	i;
	struct vos_gc_bin_df	*bin;

	D_INIT_LIST_HEAD(&cont->vc_gc_link);

	for (i = 0; i < GC_CONT; i++) {
		bin = gc_type2bin(cont->vc_pool, cont, i);
		if (bin->bin_bag_first != UMOFF_NULL) {
			d_list_add_tail(&cont->vc_gc_link,
					&cont->vc_pool->vp_gc_cont);
			return;
		}
	}
}

/**
 * Attach a pool for GC, this function also pins the pool in open hash table.
 * GC will remove this pool from open hash if it has nothing left for GC and
 * user has already closed it.
 */
int
gc_add_pool(struct vos_pool *pool)
{
	struct vos_tls	   *tls = vos_tls_get(pool->vp_sysdb);

	D_DEBUG(DB_TRACE, "Register pool="DF_UUID" for GC\n",
		DP_UUID(pool->vp_id));

	D_ASSERT(d_list_empty(&pool->vp_gc_link));

	pool->vp_opened++; /* pin the vos_pool in open-hash */
	vos_pool_addref(pool); /* +1 for the link */
	d_list_add_tail(&pool->vp_gc_link, &tls->vtl_gc_pools);
	return 0;
}

/**
 * Detach a pool for GC
 * NB: this function should NOT be called while closing a pool, it's called
 * when a pool is being destroyed
 */
void
gc_del_pool(struct vos_pool *pool)
{
	D_ASSERT(pool->vp_opened > 0);
	D_ASSERT(!d_list_empty(&pool->vp_gc_link));

	pool->vp_opened--;
	if (pool->vp_opened == 0)
		vos_pool_hash_del(pool); /* un-pin from open-hash */

	d_list_del_init(&pool->vp_gc_link);
	vos_pool_decref(pool); /* -1 for the link */
}

bool
gc_have_pool(struct vos_pool *pool)
{
	return !d_list_empty(&pool->vp_gc_link);
}

static void
gc_log_pool(struct vos_pool *pool)
{
	struct vos_gc_stat *stat = &pool->vp_gc_stat_global;

	D_DEBUG(DB_TRACE,
		"Pool="DF_UUID", GC reclaimed:\n"
		"  containers = "DF_U64"\n"
		"  objects    = "DF_U64"\n"
		"  dkeys      = "DF_U64"\n"
		"  akeys      = "DF_U64"\n"
		"  singvs     = "DF_U64"\n"
		"  recxs      = "DF_U64"\n",
		DP_UUID(pool->vp_id),
		stat->gs_conts, stat->gs_objs,
		stat->gs_dkeys, stat->gs_akeys,
		stat->gs_singvs, stat->gs_recxs);
}

/**
 * Resource reclaim for all opened VOS pool.
 * This function returns when there is nothing to reclaim or consumed all
 * credits. It returns the remainded credits.
 */
#if VOS_STANDALONE
static int
vos_gc_run(int *credits)
{
	struct vos_tls	*tls	 = vos_tls_get(true);
	d_list_t	*pools	 = &tls->vtl_gc_pools;
	int		 rc	 = 0;
	int		 checked = 0;
	int		 creds;

	creds = *credits;
	if (creds < GC_CREDS_MIN || creds > GC_CREDS_MAX) {
		D_ERROR("Invalid credits=%d\n", creds);
		return -DER_INVAL;
	}

	if (d_list_empty(pools)) {
		/* Garbage collection has nothing to do.  Just return without
		 * logging.  Otherwise, tests produce huge logs with little
		 * useful information when trace debug bit is set.
		 */
		return 0;
	}

	while (!d_list_empty(pools)) {
		struct vos_pool *pool;
		bool		 empty = false;

		pool = d_list_entry(pools->next, struct vos_pool, vp_gc_link);
		D_DEBUG(DB_TRACE, "GC pool="DF_UUID", creds=%d\n",
			DP_UUID(pool->vp_id), creds);

		rc = gc_reclaim_pool(pool, &creds, &empty);
		if (rc) {
			D_ERROR("GC pool="DF_UUID" error=%s\n",
				DP_UUID(pool->vp_id), d_errstr(rc));
			break;
		}
		checked++;
		if (empty) {
			D_DEBUG(DB_TRACE,
				"Deregister pool="DF_UUID", empty=%d\n",
				DP_UUID(pool->vp_id), empty);
			gc_log_pool(pool);
			gc_del_pool(pool);

		} else {
			D_DEBUG(DB_TRACE, "Re-add pool="DF_UUID", opened=%d\n",
				DP_UUID(pool->vp_id), pool->vp_opened);

			d_list_move_tail(&pool->vp_gc_link, pools);
		}

		if (creds == 0)
			break; /* consumed all credits */
	}
	D_DEBUG(DB_TRACE, "checked %d pools, consumed %d/%d credits\n",
		checked, *credits - creds, *credits);

	*credits = creds;
	return rc;
}
#endif

/**
 * Function for VOS standalone mode, it reclaims all the deleted items.
 */
void
gc_wait(void)
{
#if VOS_STANDALONE
	int total = 0;

	while (1) {
		int creds = GC_CREDS_TIGHT;
		int rc;

		total += creds;
		rc = vos_gc_run(&creds);
		if (rc) {
			D_CRIT("GC failed %s\n", d_errstr(rc));
			return;
		}

		if (creds != 0) {
			D_DEBUG(DB_TRACE, "Consumed %d credits\n",
				total - creds);
			return;
		}
	}
#endif
}

int
vos_gc_pool_tight(daos_handle_t poh, int *credits)
{
	struct vos_pool *pool = vos_hdl2pool(poh);
	bool		 empty;
	int		 total;
	int		 rc;

	if (!credits || *credits <= 0)
		return -DER_INVAL;

	if (!pool)
		return -DER_NO_HDL;

	if (!gc_have_pool(pool))
		return 0; /* nothing to reclaim for this pool */

	total = *credits;
	rc = gc_reclaim_pool(pool, credits, &empty);
	if (rc) {
		D_CRIT("gc_reclaim_pool failed " DF_RC "\n", DP_RC(rc));
		return 0; /* caller can't do anything for it */
	}
	total -= *credits; /* subtract the remained credits */

	if (empty) {
		if (total != 0) /* did something */
			gc_log_pool(pool);
		/*
		 * Recheck since vea_free() called when drain sv/ev record may
		 * result in yield on transaction end callback.
		 */
		if (gc_have_pool(pool))
			gc_del_pool(pool);
	}

	return 0;
}

struct vos_gc_param {
	struct umem_instance	*vgc_umm;
	int			(*vgc_yield_func)(void *arg);
	void			*vgc_yield_arg;
	uint32_t		 vgc_credits;
};

static inline bool
vos_gc_yield(void *arg)
{
	struct vos_gc_param	*param = arg;
	int			 rc;

	/* Current DTX handle must be NULL, since GC runs under non-DTX mode. */
	D_ASSERT(vos_dth_get(false) == NULL);

	if (param->vgc_yield_func == NULL) {
		param->vgc_credits = GC_CREDS_TIGHT;
		bio_yield(param->vgc_umm);
		return false;
	}

	rc = param->vgc_yield_func(param->vgc_yield_arg);
	if (rc < 0)	/* Abort */
		return true;

	/* rc == 0: tight mode; rc == 1: slack mode */
	param->vgc_credits = (rc == 0) ? GC_CREDS_TIGHT : GC_CREDS_SLACK;

	return false;
}

/** public API to reclaim space for a opened pool */
int
vos_gc_pool(daos_handle_t poh, int credits, int (*yield_func)(void *arg),
	    void *yield_arg)
{
	struct d_tm_node_t      *duration = NULL;
	struct d_tm_node_t      *tight    = NULL;
	struct d_tm_node_t      *slack    = NULL;
	struct vos_pool		*pool = vos_hdl2pool(poh);
	struct vos_tls		*tls  = vos_tls_get(pool->vp_sysdb);
	struct vos_gc_param	 param;
	uint32_t		 nr_flushed = 0;
	int			 rc = 0, total = 0;

	D_ASSERT(daos_handle_is_valid(poh));
	D_ASSERT(pool->vp_sysdb == false);

	vos_space_update_metrics(pool);

	param.vgc_umm		= &pool->vp_umm;
	param.vgc_yield_func	= yield_func;
	param.vgc_yield_arg	= yield_arg;
	param.vgc_credits	= GC_CREDS_TIGHT;

	/* To accelerate flush on container destroy done */
	if (!gc_have_pool(pool)) {
		if (pool->vp_vea_info != NULL)
			rc = vea_flush(pool->vp_vea_info, UINT32_MAX, &nr_flushed);
		return rc < 0 ? rc : nr_flushed;
	}

	tls->vtl_gc_running++;

	if (pool->vp_metrics != NULL) {
		duration = pool->vp_metrics->vp_gc_metrics.vgm_duration;
		slack    = pool->vp_metrics->vp_gc_metrics.vgm_slack_cnt;
		tight    = pool->vp_metrics->vp_gc_metrics.vgm_tight_cnt;
	}

	while (1) {
		int	creds = param.vgc_credits;

		d_tm_mark_duration_start(duration, D_TM_CLOCK_THREAD_CPUTIME);
		if (creds == GC_CREDS_TIGHT)
			d_tm_inc_counter(tight, 1);
		else
			d_tm_inc_counter(slack, 1);

		if (credits > 0 && (credits - total) < creds)
			creds = credits - total;

		total += creds;
		rc = vos_gc_pool_tight(poh, &creds);

		if (rc) {
			D_ERROR("GC pool failed: " DF_RC "\n", DP_RC(rc));
			d_tm_mark_duration_end(duration);
			break;
		}
		total -= creds; /* subtract the remainded credits */
		if (creds != 0) {
			d_tm_mark_duration_end(duration);
			break; /* reclaimed everything */
		}

		if (credits > 0 && total >= credits) {
			d_tm_mark_duration_end(duration);
			break; /* consumed all credits */
		}

		d_tm_mark_duration_end(duration);

		if (vos_gc_yield(&param)) {
			D_DEBUG(DB_TRACE, "GC pool run aborted\n");
			break;
		}
	}

	if (total != 0) /* did something */
		D_DEBUG(DB_TRACE, "GC consumed %d credits\n", total);

	D_ASSERT(tls->vtl_gc_running > 0);
	tls->vtl_gc_running--;
	return rc < 0 ? rc : nr_flushed;
}

inline bool
vos_gc_pool_idle(daos_handle_t poh)
{
	D_ASSERT(daos_handle_is_valid(poh));
	return !gc_have_pool(vos_hdl2pool(poh));
}

inline void
gc_reserve_space(daos_size_t *rsrvd)
{
	rsrvd[DAOS_MEDIA_SCM]	+= gc_bag_size * (daos_size_t)GC_CREDS_MAX;
	rsrvd[DAOS_MEDIA_NVME]	+= 0;
}

/** Exported VOS API for explicit VEA flush */
int
vos_flush_pool(daos_handle_t poh, uint32_t nr_flush, uint32_t *nr_flushed)
{
	struct vos_pool	*pool = vos_hdl2pool(poh);
	int		 rc;

	D_ASSERT(daos_handle_is_valid(poh));

	if (pool->vp_vea_info == NULL) {
		if (nr_flushed != NULL)
			*nr_flushed = 0;
		return 1;
	}

	rc = vea_flush(pool->vp_vea_info, nr_flush, nr_flushed);
	if (rc)
		D_ERROR("VEA flush failed. "DF_RC"\n", DP_RC(rc));

	return rc;
}

#define VOS_GC_DIR "vos_gc"
void
vos_gc_metrics_init(struct vos_gc_metrics *vgm, const char *path, int tgt_id)
{
	int rc;

	/* GC slice duration */
	rc = d_tm_add_metric(&vgm->vgm_duration, D_TM_DURATION | D_TM_CLOCK_THREAD_CPUTIME,
			     "GC slice duration", NULL, "%s/%s/duration/tgt_%u", path, VOS_GC_DIR,
			     tgt_id);
	if (rc)
		D_WARN("Failed to create 'duration' telemetry: " DF_RC "\n", DP_RC(rc));

	/* GC container deletion */
	rc = d_tm_add_metric(&vgm->vgm_cont_del, D_TM_COUNTER, "GC containers deleted", NULL,
			     "%s/%s/cont_del/tgt_%u", path, VOS_GC_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'cont_del' telemetry: " DF_RC "\n", DP_RC(rc));

	/* GC object deletion */
	rc = d_tm_add_metric(&vgm->vgm_obj_del, D_TM_COUNTER, "GC objects deleted", NULL,
			     "%s/%s/obj_del/tgt_%u", path, VOS_GC_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'obj_del' telemetry: " DF_RC "\n", DP_RC(rc));

	/* GC dkey deletion */
	rc = d_tm_add_metric(&vgm->vgm_dkey_del, D_TM_COUNTER, "GC dkeys deleted", NULL,
			     "%s/%s/dkey_del/tgt_%u", path, VOS_GC_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'dkey_del' telemetry: " DF_RC "\n", DP_RC(rc));

	/* GC akey deletion */
	rc = d_tm_add_metric(&vgm->vgm_akey_del, D_TM_COUNTER, "GC akeys deleted", NULL,
			     "%s/%s/akey_del/tgt_%u", path, VOS_GC_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'akey_del' telemetry: " DF_RC "\n", DP_RC(rc));

	/* GC ev deletion */
	rc = d_tm_add_metric(&vgm->vgm_ev_del, D_TM_COUNTER, "GC ev deleted", NULL,
			     "%s/%s/ev_del/tgt_%u", path, VOS_GC_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'ev_del' telemetry: " DF_RC "\n", DP_RC(rc));

	/* GC sv deletion */
	rc = d_tm_add_metric(&vgm->vgm_sv_del, D_TM_COUNTER, "GC sv deleted", NULL,
			     "%s/%s/sv_del/tgt_%u", path, VOS_GC_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'sv_del' telemetry: " DF_RC "\n", DP_RC(rc));

	/* GC slack mode runs */
	rc = d_tm_add_metric(&vgm->vgm_slack_cnt, D_TM_COUNTER, "GC slack mode count", NULL,
			     "%s/%s/slack_cnt/tgt_%u", path, VOS_GC_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'slack_cnt' telemetry: " DF_RC "\n", DP_RC(rc));

	/* GC tight mode runs */
	rc = d_tm_add_metric(&vgm->vgm_tight_cnt, D_TM_COUNTER, "GC tight mode count", NULL,
			     "%s/%s/tight_cnt/tgt_%u", path, VOS_GC_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'tight_cnt' telemetry: " DF_RC "\n", DP_RC(rc));
}
