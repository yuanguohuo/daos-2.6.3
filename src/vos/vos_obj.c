/**
 * (C) Copyright 2016-2024 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of daos
 *
 * vos/vos_obj.c
 */
#define D_LOGFAC	DD_FAC(vos)

#include <daos/common.h>
#include <daos/checksum.h>
#include <daos/btree.h>
#include <daos_types.h>
#include <daos_srv/vos.h>
#include "vos_internal.h"

/** Ensure the values of recx flags map to those exported by evtree */
D_CASSERT((uint32_t)VOS_VIS_FLAG_UNKNOWN == (uint32_t)EVT_UNKNOWN);
D_CASSERT((uint32_t)VOS_VIS_FLAG_COVERED == (uint32_t)EVT_COVERED);
D_CASSERT((uint32_t)VOS_VIS_FLAG_VISIBLE == (uint32_t)EVT_VISIBLE);
D_CASSERT((uint32_t)VOS_VIS_FLAG_PARTIAL == (uint32_t)EVT_PARTIAL);
D_CASSERT((uint32_t)VOS_VIS_FLAG_LAST == (uint32_t)EVT_LAST);

static inline bool
is_fake_iter(struct vos_obj_iter *oiter)
{
	return (oiter->it_flags & (VOS_IT_DKEY_EV | VOS_IT_DKEY_SV)) != 0;
}

static inline bool
fake_iter_child_is_array(struct vos_obj_iter *oiter)
{
	return (oiter->it_flags & VOS_IT_DKEY_EV) != 0;
}

bool vos_dkey_punch_propagate;

struct vos_key_info {
	umem_off_t		*ki_known_key;
	struct vos_object	*ki_obj;
	bool			 ki_non_empty;
	bool			 ki_has_uncommitted;
	const void		*ki_first;
};

static inline int
key_iter_fetch_helper(struct vos_obj_iter *oiter, struct vos_rec_bundle *rbund, d_iov_t *keybuf,
		      daos_anchor_t *anchor)
{
	d_iov_t			 kiov;
	d_iov_t			 riov;
	struct dcs_csum_info	 csum;

	tree_rec_bundle2iov(rbund, &riov);

	rbund->rb_iov	= keybuf;
	rbund->rb_csum	= &csum;

	d_iov_set(rbund->rb_iov, NULL, 0); /* no copy */
	ci_set_null(rbund->rb_csum);

	return dbtree_iter_fetch(oiter->it_hdl, &kiov, &riov, anchor);
}

/** This callback is invoked only if the tree is not empty */
static int
empty_tree_check(daos_handle_t ih, vos_iter_entry_t *entry,
		 vos_iter_type_t type, vos_iter_param_t *param, void *cb_arg,
		 unsigned int *acts)
{
	struct vos_iterator	*iter;
	struct vos_obj_iter	*oiter;
	struct vos_rec_bundle	 rbund = {0};
	d_iov_t			 key_iov;
	struct umem_instance	*umm;
	struct vos_key_info	*kinfo = cb_arg;
	int			 rc;

	if (kinfo->ki_first == entry->ie_key.iov_buf)
		return 1; /** We've seen this one before */

	/** Save the first thing we see so we can stop iteration early
	 *  if we see it again on 2nd pass.
	 */
	if (kinfo->ki_first == NULL)
		kinfo->ki_first = entry->ie_key.iov_buf;

	if (entry->ie_vis_flags == VOS_IT_UNCOMMITTED) {
		kinfo->ki_has_uncommitted = true;
		return 0;
	}

	iter = vos_hdl2iter(ih);
	oiter = vos_iter2oiter(iter);
	rc = key_iter_fetch_helper(oiter, &rbund, &key_iov, NULL);
	if (rc != 0)
		return rc;

	D_ASSERT(key_iov.iov_len == entry->ie_key.iov_len);
	D_ASSERT(((char *)key_iov.iov_buf)[0] == ((char *)entry->ie_key.iov_buf)[0]);
	D_ASSERT(((char *)key_iov.iov_buf)[key_iov.iov_len - 1] ==
		 ((char *)entry->ie_key.iov_buf)[key_iov.iov_len - 1]);
	umm = vos_obj2umm(kinfo->ki_obj);
	rc = umem_tx_add_ptr(umm, kinfo->ki_known_key, sizeof(*(kinfo->ki_known_key)));
	if (rc != 0)
		return rc;

	*(kinfo->ki_known_key) = umem_ptr2off(umm, rbund.rb_krec);

	kinfo->ki_non_empty = true;

	return 1; /* Return positive number to break iteration */
}

static int
tree_is_empty(struct vos_object *obj, umem_off_t *known_key, daos_handle_t toh,
	      const daos_epoch_range_t *epr, vos_iter_type_t type)
{
	daos_anchor_t		 anchor = {0};
	struct dtx_handle	*dth = vos_dth_get(obj->obj_cont->vc_pool->vp_sysdb);
	struct umem_instance	*umm;
	d_iov_t			 key;
	struct vos_key_info	 kinfo = {0};
	struct vos_krec_df	*krec;
	int			 rc;

	/** The address of the known_key, which actually points at the krec is guaranteed by PMDK
	 *  to be allocated at an 8 byte alignment so the low order bit is available to mark it as
	 *  punched.
	 */
	if (*known_key != UMOFF_NULL && (*known_key & 0x1) == 0)
		return 0;

	kinfo.ki_obj = obj;
	kinfo.ki_known_key = known_key;

	if (*known_key == UMOFF_NULL)
		goto tail;

	krec = umem_off2ptr(vos_obj2umm(obj), (*known_key & ~(1ULL)));
	d_iov_set(&key, vos_krec2key(krec), krec->kr_size);
	dbtree_key2anchor(toh, &key, &anchor);

	rc = vos_iterate_key(obj, toh, type, epr, true, empty_tree_check,
			     &kinfo, dth, &anchor);

	if (rc < 0)
		return rc;

	if (kinfo.ki_non_empty)
		return 0;

	/** Start from beginning one more time.  It will iterate until it
	 *  sees the first thing it saw
	 */
tail:
	rc = vos_iterate_key(obj, toh, type, epr, true, empty_tree_check,
			     &kinfo, dth, NULL);

	if (rc < 0)
		return rc;

	if (kinfo.ki_non_empty)
		return 0;

	/** We didn't find any committed entries, so reset to an unknown key */
	umm = vos_obj2umm(obj);
	rc = umem_tx_add_ptr(umm, known_key, sizeof(*known_key));
	if (rc != 0)
		return rc;

	*known_key = UMOFF_NULL;

	if (kinfo.ki_has_uncommitted)
		return -DER_INPROGRESS;

	/** The tree is empty */
	return 1;
}

static int
vos_propagate_check(struct vos_object *obj, umem_off_t *known_key, daos_handle_t toh,
		    struct vos_ts_set *ts_set, const daos_epoch_range_t *epr,
		    vos_iter_type_t type)
{
	const char	*tree_name = NULL;
	uint64_t	 punch_flag = VOS_OF_PUNCH_PROPAGATE;
	int		 rc = 0;
	uint32_t	 read_flag = 0;
	uint32_t	 write_flag = 0;

	if (vos_ts_set_check_conflict(ts_set, epr->epr_hi)) {
		D_DEBUG(DB_IO, "Failed to punch key: "DF_RC"\n",
			DP_RC(-DER_TX_RESTART));
		return -DER_TX_RESTART;
	}

	switch (type) {
	case VOS_ITER_DKEY:
		read_flag = VOS_TS_READ_OBJ;
		write_flag = VOS_TS_WRITE_OBJ;
		tree_name = "DKEY";
		if (!vos_dkey_punch_propagate)
			return 0; /** Unless we explicitly enable it, disable punch propagation */
	case VOS_ITER_AKEY:
		read_flag = VOS_TS_READ_DKEY;
		write_flag = VOS_TS_WRITE_DKEY;
		tree_name = "AKEY";
		break;
	default:
		D_ASSERT(0);
	}

	/** The check for propagation needs to update the read
	 *  time on the object as it's iterating the dkey tree
	 */
	vos_ts_set_append_cflags(ts_set, read_flag);

	rc = tree_is_empty(obj, known_key, toh, epr, type);
	if (rc > 0) {
		/** tree is now empty, set the flags so we can punch
		 *  the parent
		 */
		D_DEBUG(DB_TRACE, "%s tree empty, punching parent\n",
			tree_name);
		vos_ts_set_append_vflags(ts_set, punch_flag);
		vos_ts_set_append_cflags(ts_set, write_flag);

		return 1;
	}

	VOS_TX_LOG_FAIL(rc, "Could not check emptiness on punch: "DF_RC"\n",
			DP_RC(rc));

	return rc;
}

struct key_ilog_info {
	struct vos_ilog_info	ki_obj;
	struct vos_ilog_info	ki_dkey;
	struct vos_ilog_info	ki_akey;
};

/**
 * @} vos_tree_helper
 */
static int
key_punch(struct vos_object *obj, daos_epoch_t epoch, daos_epoch_t bound,
	  uint32_t pm_ver, daos_key_t *dkey, unsigned int akey_nr,
	  daos_key_t *akeys, uint64_t flags, struct vos_ts_set *ts_set)
{
	struct vos_krec_df	*krec;
	struct vos_rec_bundle	 rbund;
	struct dcs_csum_info	 csum;
	struct key_ilog_info	*info;
	daos_epoch_range_t	 epr = {0, epoch};
	d_iov_t			 riov;
	daos_handle_t		 toh = DAOS_HDL_INVAL;
	int			 i;
	int			 rc;

	D_ALLOC_PTR(info);
	if (info == NULL)
		return -DER_NOMEM;

	vos_ilog_fetch_init(&info->ki_obj);
	vos_ilog_fetch_init(&info->ki_dkey);
	vos_ilog_fetch_init(&info->ki_akey);
	rc = obj_tree_init(obj);
	if (rc)
		D_GOTO(out, rc);

	rc = vos_ilog_punch(obj->obj_cont, &obj->obj_df->vo_ilog, &epr, bound,
			    NULL, &info->ki_obj, ts_set, false, false);
	if (rc)
		D_GOTO(out, rc);

	tree_rec_bundle2iov(&rbund, &riov);
	rbund.rb_off	= UMOFF_NULL;
	rbund.rb_ver	= pm_ver;
	rbund.rb_csum	= &csum;
	ci_set_null(&csum);

	if (!akeys)
		goto punch_dkey;

	rc = key_tree_prepare(obj, obj->obj_toh, VOS_BTR_DKEY,
			      dkey, SUBTR_CREATE, DAOS_INTENT_PUNCH,
			      &krec, &toh, ts_set);
	if (rc) {
		D_ERROR("Error preparing dkey: rc="DF_RC"\n",
			DP_RC(rc));
		D_GOTO(out, rc);
	}

	rc = vos_ilog_punch(obj->obj_cont, &krec->kr_ilog, &epr, bound,
			    &info->ki_obj, &info->ki_dkey, ts_set, false,
			    false);
	if (rc)
		D_GOTO(out, rc);

	/* We do not need to add an incarnation log entry in parent tree
	 * on punch.   If the subtree has nothing but punches, no need
	 * to track that.  If it has updates, the parent tree will have
	 * updates
	 */
	rbund.rb_tclass	= VOS_BTR_AKEY;
	for (i = 0; i < akey_nr; i++) {
		rbund.rb_iov = &akeys[i];
		rc = key_tree_punch(obj, toh, epoch, bound, &akeys[i], &riov,
				    flags, ts_set, &krec->kr_known_akey, &info->ki_dkey,
				    &info->ki_akey);
		if (rc != 0) {
			VOS_TX_LOG_FAIL(rc, "Failed to punch akey: rc="
					DF_RC"\n", DP_RC(rc));
			break;
		}
	}

	if (rc == 0 && (flags & VOS_OF_REPLAY_PC) == 0) {
		/** Check if we need to propagate the punch */
		rc = vos_propagate_check(obj, &krec->kr_known_akey, toh, ts_set, &epr,
					 VOS_ITER_AKEY);
	}

	if (rc != 1) {
		/** key_tree_punch will handle dkey flags if punch is propagated */
		if (rc == 0)
			rc = vos_key_mark_agg(obj->obj_cont, krec, epoch);
		goto out;
	}
	/** else propagate the punch */

punch_dkey:
	rbund.rb_iov = dkey;
	rbund.rb_tclass	= VOS_BTR_DKEY;

	rc = key_tree_punch(obj, obj->obj_toh, epoch, bound, dkey, &riov,
			    flags, ts_set, &obj->obj_df->vo_known_dkey, &info->ki_obj,
			    &info->ki_dkey);
	if (rc != 0)
		D_GOTO(out, rc);

	if (rc == 0 && (flags & VOS_OF_REPLAY_PC) == 0) {
		/** Check if we need to propagate to object */
		rc = vos_propagate_check(obj, &obj->obj_df->vo_known_dkey, obj->obj_toh, ts_set,
					 &epr, VOS_ITER_DKEY);
	}
 out:
	vos_ilog_fetch_finish(&info->ki_obj);
	vos_ilog_fetch_finish(&info->ki_dkey);
	vos_ilog_fetch_finish(&info->ki_akey);

	if (daos_handle_is_valid(toh)) {
		D_ASSERT(krec != NULL);
		key_tree_release(toh, (krec->kr_bmap & KREC_BF_EVT) != 0);
	}

	D_FREE(info);

	return rc;
}

static int
obj_punch(daos_handle_t coh, struct vos_object *obj, daos_epoch_t epoch,
	  daos_epoch_t bound, uint64_t flags, struct vos_ts_set *ts_set)
{
	struct daos_lru_cache	*occ;
	struct vos_container	*cont;
	struct vos_ilog_info	*info;
	int			 rc;

	cont = vos_hdl2cont(coh);
	occ  = vos_obj_cache_current(cont->vc_pool->vp_sysdb);
	D_ALLOC_PTR(info);
	if (info == NULL)
		return -DER_NOMEM;
	vos_ilog_fetch_init(info);
	rc = vos_oi_punch(cont, obj->obj_id, epoch, bound, flags, obj->obj_df,
			  info, ts_set);
	if (rc)
		D_GOTO(failed, rc);

	/* evict it from cache, because future fetch should only see empty
	 * object (without obj_df)
	 */
	vos_obj_evict(occ, obj);
failed:
	vos_ilog_fetch_finish(info);
	D_FREE(info);
	return rc;
}

/** If the object/key doesn't exist, we should augment the set with any missing
 *  entries
 */
static void
vos_punch_add_missing(struct vos_ts_set *ts_set, daos_key_t *dkey, int akey_nr,
		      daos_key_t *akeys)
{
	struct vos_akey_data	ad;

	ad.ad_is_iod = false;
	ad.ad_keys = akeys;

	vos_ts_add_missing(ts_set, dkey, akey_nr, &ad);
}

/**
 * Punch an object, or punch a dkey, or punch an array of akeys.
 */
int
vos_obj_punch(daos_handle_t coh, daos_unit_oid_t oid, daos_epoch_t epoch,
	      uint32_t pm_ver, uint64_t flags, daos_key_t *dkey,
	      unsigned int akey_nr, daos_key_t *akeys, struct dtx_handle *dth)
{
	struct vos_dtx_act_ent	**daes = NULL;
	struct vos_dtx_cmt_ent	**dces = NULL;
	struct vos_ts_set	*ts_set;
	struct vos_container	*cont;
	struct vos_object	*obj = NULL;
	bool			 punch_obj = false;
	uint64_t		 hold_flags;
	daos_epoch_range_t	 epr = { 0 };
	daos_epoch_t		 bound;
	int			 rc = 0;
	uint64_t		 cflags = 0;

	if (oid.id_shard % 3 == 1 && DAOS_FAIL_CHECK(DAOS_DTX_FAIL_IO))
		return -DER_IO;

	cont = vos_hdl2cont(coh);

	if (vos_obj_skip_akey_supported(cont, oid) && dkey != NULL && akeys != NULL) {
		D_ERROR("Akey punch is not supported when no akey exists: " DF_UOID "\n",
			DP_UOID(oid));

		return -DER_INVAL;
	}

	if (dth && dth->dth_local)
		++dth->dth_op_seq;

	if (dtx_is_real_handle(dth)) {
		epr.epr_hi = dth->dth_epoch;
		bound = MAX(dth->dth_epoch_bound, dth->dth_epoch);
	} else {
		epr.epr_hi = epoch;
		bound = epoch;
	}

	D_DEBUG(DB_IO, "Punch "DF_UOID", epoch "DF_X64"\n",
		DP_UOID(oid), epr.epr_hi);

	rc = vos_tgt_health_check(cont, true);
	if (rc) {
		DL_ERROR(rc, DF_UOID": Reject punch due to faulty NVMe.", DP_UOID(oid));
		return rc;
	}

	if (dtx_is_valid_handle(dth)) {
		if (akey_nr) {
			cflags = VOS_TS_WRITE_AKEY;
			if (flags & VOS_OF_COND_PUNCH)
				cflags |= VOS_TS_READ_AKEY;
		} else if (dkey != NULL) {
			cflags = VOS_TS_WRITE_DKEY;
			if (flags & VOS_OF_COND_PUNCH)
				cflags |= VOS_TS_READ_DKEY;
		} else {
			cflags = VOS_TS_WRITE_OBJ;
			if (flags & VOS_OF_COND_PUNCH)
				cflags |= VOS_TS_READ_OBJ;
		}

	}

	rc = vos_ts_set_allocate(&ts_set, flags, cflags, akey_nr,
				 dth, cont->vc_pool->vp_sysdb);
	if (rc != 0)
		goto reset;

	rc = vos_ts_set_add(ts_set, cont->vc_ts_idx, NULL, 0);
	if (rc != 0)
		goto reset;

	rc = vos_tx_begin(dth, vos_cont2umm(cont), cont->vc_pool->vp_sysdb);
	if (rc != 0)
		goto reset;

	/* Commit the CoS DTXs via the PUNCH PMDK transaction. */
	if (dtx_is_valid_handle(dth) && dth->dth_dti_cos_count > 0 &&
	    !dth->dth_cos_done) {
		D_ALLOC_ARRAY(daes, dth->dth_dti_cos_count);
		if (daes == NULL)
			D_GOTO(reset, rc = -DER_NOMEM);

		D_ALLOC_ARRAY(dces, dth->dth_dti_cos_count);
		if (dces == NULL)
			D_GOTO(reset, rc = -DER_NOMEM);

		rc = vos_dtx_commit_internal(cont, dth->dth_dti_cos,
					     dth->dth_dti_cos_count, 0, false, NULL, daes, dces);
		if (rc < 0)
			goto reset;
		if (rc == 0)
			D_FREE(daes);
	}

	hold_flags = (flags & VOS_OF_COND_PUNCH) ? 0 : VOS_OBJ_CREATE;
	hold_flags |= VOS_OBJ_VISIBLE;
	/* NB: punch always generate a new incarnation of the object */
	rc = vos_obj_hold(vos_obj_cache_current(cont->vc_pool->vp_sysdb), vos_hdl2cont(coh),
			  oid, &epr, bound, hold_flags, DAOS_INTENT_PUNCH, &obj, ts_set);
	if (rc == 0) {
		if (dkey) { /* key punch */
			rc = key_punch(obj, epr.epr_hi, bound, pm_ver, dkey,
				       akey_nr, akeys, flags, ts_set);
			if (rc > 0)
				punch_obj = true;
		} else {
			punch_obj = true;
		}

		if (punch_obj)
			rc = obj_punch(coh, obj, epr.epr_hi, bound, flags,
				       ts_set);
		if (obj != NULL) {
			if (rc == 0 && epr.epr_hi > obj->obj_df->vo_max_write) {
				rc = umem_tx_xadd_ptr(
				    vos_cont2umm(cont), &obj->obj_df->vo_max_write,
				    sizeof(obj->obj_df->vo_max_write), UMEM_XADD_NO_SNAPSHOT);
				if (rc == 0)
					obj->obj_df->vo_max_write = epr.epr_hi;
			}

			if (rc == 0)
				rc = vos_mark_agg(cont, &obj->obj_df->vo_tree,
						  &cont->vc_cont_df->cd_obj_root, epoch);

			vos_obj_release(vos_obj_cache_current(cont->vc_pool->vp_sysdb), obj, 0,
					rc != 0);
		}
	}

reset:
	if (rc != 0)
		D_DEBUG(DB_IO, "Failed to punch object "DF_UOID": rc = %d\n",
			DP_UOID(oid), rc);

	if (rc == 0 || rc == -DER_NONEXIST) {
		/** We must prevent underpunch with regular I/O */
		if (rc == 0 && (flags & VOS_OF_REPLAY_PC) == 0)
			bound = DAOS_EPOCH_MAX;
		if (vos_ts_wcheck(ts_set, epr.epr_hi, bound))
			rc = -DER_TX_RESTART;
	}

	if (rc == 0)
		vos_ts_set_upgrade(ts_set);

	if (rc == -DER_NONEXIST || rc == 0) {
		vos_punch_add_missing(ts_set, dkey, akey_nr, akeys);
		vos_ts_set_update(ts_set, epr.epr_hi);
	}

	if (rc == 0) {
		vos_ts_set_wupdate(ts_set, epr.epr_hi);

		if (dtx_is_valid_handle(dth) && dth->dth_local) {
			rc = vos_insert_oid(dth, cont, &oid);
		}
	}

	rc = vos_tx_end(cont, dth, NULL, NULL, true, NULL, rc);
	if (dtx_is_valid_handle(dth)) {
		if (rc == 0)
			dth->dth_cos_done = 1;
		else
			dth->dth_cos_done = 0;

		if (daes != NULL)
			vos_dtx_post_handle(cont, daes, dces, dth->dth_dti_cos_count,
					    false, rc != 0, false);
	}

	D_FREE(daes);
	D_FREE(dces);
	vos_ts_set_free(ts_set);

	if (rc == 0) {
		rc = vos_tgt_health_check(cont, true);
		if (rc)
			DL_ERROR(rc, "Fail punch due to faulty NVMe.");
	}

	return rc;
}

int
vos_obj_key2anchor(daos_handle_t coh, daos_unit_oid_t oid, daos_key_t *dkey, daos_key_t *akey,
		   daos_anchor_t *anchor)
{
	struct vos_container  *cont;
	struct vos_krec_df    *krec = NULL;
	struct daos_lru_cache *occ;
	int                    rc;
	int                    flags = 0;
	struct vos_object     *obj;
	daos_epoch_range_t     epr = {0, DAOS_EPOCH_MAX};
	daos_handle_t          toh;

	cont = vos_hdl2cont(coh);
	if (cont == NULL) {
		D_ERROR("Container is not open\n");
		return -DER_INVAL;
	}
	occ = vos_obj_cache_current(cont->vc_pool->vp_sysdb);

	rc = vos_obj_hold(occ, cont, oid, &epr, DAOS_EPOCH_MAX, 0, DAOS_INTENT_DEFAULT, &obj, NULL);
	if (rc != 0) {
		if (rc == -DER_NONEXIST) {
			daos_anchor_set_eof(anchor);
			return 0;
		}

		D_ERROR("Could not hold object oid=" DF_UOID " rc=" DF_RC "\n", DP_UOID(oid),
			DP_RC(rc));
		return rc;
	}

	rc = obj_tree_init(obj);
	if (rc)
		goto out;

	if (akey == NULL) {
		rc = dbtree_key2anchor(obj->obj_toh, dkey, anchor);
		D_DEBUG(DB_TRACE, "oid=" DF_UOID " dkey=" DF_KEY " to anchor: rc=" DF_RC "\n",
			DP_UOID(oid), DP_KEY(dkey), DP_RC(rc));
		goto out;
	}

	/** If the dkey has no akey, this will enable the operation to succeed */
	if (vos_obj_skip_akey_supported(obj->obj_cont, obj->obj_id)) {
		flags |= SUBTR_FLAT;
		if (daos_is_array(obj->obj_id.id_pub))
			flags |= SUBTR_EVT;
	}

	/** Otherwise, we need to find the dkey to convert the akey to the anchor */
	rc = key_tree_prepare(obj, obj->obj_toh, VOS_BTR_DKEY, dkey, flags, DAOS_INTENT_DEFAULT,
			      &krec, &toh, NULL);
	if (rc) {
		if (rc == -DER_NONEXIST) {
			daos_anchor_set_eof(anchor);
			goto out;
		}
		D_ERROR("Error preparing dkey: oid=" DF_UOID " dkey=" DF_KEY " rc=" DF_RC "\n",
			DP_UOID(oid), DP_KEY(dkey), DP_RC(rc));
		D_GOTO(out, rc);
	}

	if (krec->kr_bmap & KREC_BF_NO_AKEY) {
		/** There is no akey tree to query.  In accordance with the design to fake it for
		 *  iterators, let's create a fake anchor
		 */
		vos_fake_anchor_create(anchor);
	} else {
		rc = dbtree_key2anchor(toh, akey, anchor);
	}
	D_DEBUG(DB_TRACE,
		"oid=" DF_UOID " dkey=" DF_KEY " akey=" DF_KEY " to anchor: rc=" DF_RC "\n",
		DP_UOID(oid), DP_KEY(dkey), DP_KEY(akey), DP_RC(rc));

	key_tree_release(toh, (krec->kr_bmap & KREC_BF_EVT) != 0);
out:
	vos_obj_release(occ, obj, 0, false);

	return rc;
}

static int
vos_obj_delete_internal(daos_handle_t coh, daos_unit_oid_t oid, bool only_delete_entry)
{
	struct vos_container	*cont = vos_hdl2cont(coh);
	struct daos_lru_cache	*occ  = vos_obj_cache_current(cont->vc_pool->vp_sysdb);
	struct umem_instance	*umm = vos_cont2umm(cont);
	struct vos_object	*obj;
	daos_epoch_range_t	 epr = {0, DAOS_EPOCH_MAX};
	int			 rc;

	rc = vos_obj_hold(occ, cont, oid, &epr, 0, VOS_OBJ_VISIBLE,
			  DAOS_INTENT_KILL, &obj, NULL);
	if (rc == -DER_NONEXIST)
		return 0;

	if (rc) {
		D_ERROR("Failed to hold object: " DF_RC "\n", DP_RC(rc));
		return rc;
	}

	rc = umem_tx_begin(umm, NULL);
	if (rc)
		goto out;

	rc = vos_oi_delete(cont, obj->obj_id, only_delete_entry);
	if (rc)
		D_ERROR("Failed to delete object: " DF_RC "\n", DP_RC(rc));

	rc = umem_tx_end(umm, rc);

out:
	vos_obj_release(occ, obj, 0, true);
	return rc;
}

int
vos_obj_delete(daos_handle_t coh, daos_unit_oid_t oid)
{
	return vos_obj_delete_internal(coh, oid, false);
}

int
vos_obj_delete_ent(daos_handle_t coh, daos_unit_oid_t oid)
{
	return vos_obj_delete_internal(coh, oid, true);
}

/* Delete a key in its parent tree.
 * NB: there is no "delete" in DAOS data model, this is really for the
 * system DB, or space reclaim after data movement.
 */
int
vos_obj_del_key(daos_handle_t coh, daos_unit_oid_t oid, daos_key_t *dkey,
		daos_key_t *akey)
{
	struct vos_container	*cont = vos_hdl2cont(coh);
	struct daos_lru_cache	*occ  = vos_obj_cache_current(cont->vc_pool->vp_sysdb);
	struct umem_instance	*umm  = vos_cont2umm(cont);
	struct vos_object	*obj;
	daos_key_t		*key;
	daos_epoch_range_t	 epr = {0, DAOS_EPOCH_MAX};
	daos_handle_t		 toh;
	int			 rc;

	rc = vos_obj_hold(occ, cont, oid, &epr, 0, VOS_OBJ_VISIBLE | VOS_OBJ_KILL_DKEY,
			  DAOS_INTENT_KILL, &obj, NULL);
	if (rc == -DER_NONEXIST)
		return 0;

	if (rc) {
		D_ERROR("object hold error: "DF_RC"\n", DP_RC(rc));
		return rc;
	}

	rc = umem_tx_begin(umm, NULL);
	if (rc) {
		D_ERROR("memory TX start error: "DF_RC"\n", DP_RC(rc));
		goto out;
	}

	rc = obj_tree_init(obj);
	if (rc) {
		D_ERROR("init dkey tree error: "DF_RC"\n", DP_RC(rc));
		goto out_tx;
	}

	if (akey) { /* delete akey */
		key = akey;
		rc = key_tree_prepare(obj, obj->obj_toh, VOS_BTR_DKEY,
				      dkey, 0, DAOS_INTENT_PUNCH, NULL,
				      &toh, NULL);
		if (rc) {
			D_ERROR("open akey tree error: "DF_RC"\n", DP_RC(rc));
			goto out_tx;
		}
	} else { /* delete dkey */
		key = dkey;
		toh = obj->obj_toh;
	}

	rc = key_tree_delete(obj, toh, key);
	if (rc) {
		D_ERROR("delete key error: "DF_RC"\n", DP_RC(rc));
		goto out_tree;
	}
	/* fall through */
out_tree:
	if (akey)
		key_tree_release(toh, false);
out_tx:
	rc = umem_tx_end(umm, rc);
out:
	vos_obj_release(occ, obj, 0, true);
	return rc;
}

static int
key_iter_ilog_check(struct vos_krec_df *krec, struct vos_obj_iter *oiter,
		    daos_epoch_range_t *epr, bool check_existence, struct vos_ts_set *ts_set)
{
	struct umem_instance	*umm;
	int			 rc;

	umm = vos_obj2umm(oiter->it_obj);
	rc = vos_ilog_fetch(umm, vos_cont2hdl(oiter->it_obj->obj_cont),
			    vos_iter_intent(&oiter->it_iter), &krec->kr_ilog,
			    oiter->it_epr.epr_hi, oiter->it_iter.it_bound, false,
			    &oiter->it_punched, NULL, &oiter->it_ilog_info);

	if (rc != 0)
		goto out;

	if (vos_has_uncertainty(ts_set, &oiter->it_ilog_info,
				oiter->it_epr.epr_hi, oiter->it_iter.it_bound))
		D_GOTO(out, rc = -DER_TX_RESTART);

	rc = vos_ilog_check(&oiter->it_ilog_info, &oiter->it_epr, epr,
			    (oiter->it_flags & VOS_IT_PUNCHED) == 0);
out:
	D_ASSERTF(check_existence || rc != -DER_NONEXIST,
		  "Probe is required before fetch\n");
	return rc;
}

static int
key_ilog_prepare(struct vos_obj_iter *oiter, daos_handle_t toh,
		 enum vos_tree_class tclass, daos_key_t *key,
		 int flags, daos_handle_t *sub_toh, struct vos_krec_df **krecp,
		 daos_epoch_range_t *epr, struct vos_punch_record *punched,
		 struct vos_ilog_info *info, struct vos_ts_set *ts_set)
{
	struct vos_krec_df	*krec = NULL;
	struct vos_object	*obj = oiter->it_obj;
	int			 rc;

	if (krecp != NULL)
		*krecp = NULL;

	rc = key_tree_prepare(obj, toh, tclass, key, flags,
			      vos_iter_intent(&oiter->it_iter), &krec,
			      sub_toh, ts_set);
	if (rc == -DER_NONEXIST)
		return rc;

	if (rc != 0) {
		D_ERROR("Cannot load the prepare key tree: "DF_RC"\n",
			DP_RC(rc));
		return rc;
	}

	/* Update the lower bound for nested iterator */
	rc = key_iter_ilog_check(krec, oiter, epr, true, ts_set);
	if (rc != 0)
		goto fail;

	if (punched && vos_epc_punched(punched->pr_epc, punched->pr_minor_epc,
				       &info->ii_prior_punch))
		*punched = info->ii_prior_punch;

	if (krecp != NULL)
		*krecp = krec;

	return 0;
fail:
	if (sub_toh) {
		D_ASSERT(krec != NULL);
		key_tree_release(*sub_toh, key_tree_is_evt(flags, tclass, krec));
	}
	return rc;
}

static inline int
key_ilog_prepare_dkey(struct vos_obj_iter *oiter, daos_key_t *key, daos_handle_t *sub_toh,
		      struct vos_krec_df **krecp, struct vos_ts_set *ts_set)
{
	struct vos_object *obj   = oiter->it_obj;
	int                flags = 0;

	if (vos_obj_skip_akey_supported(obj->obj_cont, obj->obj_id)) {
		flags |= SUBTR_FLAT;
		if (daos_is_array(obj->obj_id.id_pub))
			flags |= SUBTR_EVT;
	}

	return key_ilog_prepare(oiter, obj->obj_toh, VOS_BTR_DKEY, key, flags, sub_toh, krecp,
				&oiter->it_epr, &oiter->it_punched, &oiter->it_ilog_info, ts_set);
}

/**
 * @defgroup vos_obj_iters VOS object iterators
 * @{
 *
 * - iterate d-key
 * - iterate a-key (array)
 * - iterate recx
 */

static int
key_iter_fill(struct vos_krec_df *krec, struct vos_obj_iter *oiter, bool check_existence,
	      vos_iter_entry_t *ent)
{
	daos_epoch_range_t	epr = {0, DAOS_EPOCH_MAX};
	uint32_t		ts_type;
	int			rc;

	if (oiter->it_iter.it_type == VOS_ITER_AKEY) {
		if (krec->kr_bmap & KREC_BF_EVT) {
			ent->ie_child_type = VOS_ITER_RECX;
		} else if (krec->kr_bmap & KREC_BF_BTR) {
			ent->ie_child_type = VOS_ITER_SINGLE;
		} else {
			ent->ie_child_type = VOS_ITER_NONE;
		}
		ts_type = VOS_TS_TYPE_AKEY;
	} else {
		ent->ie_child_type = VOS_ITER_AKEY;
		ts_type = VOS_TS_TYPE_DKEY;
	}

	rc = key_iter_ilog_check(krec, oiter, &epr, check_existence, NULL);
	if (rc == -DER_NONEXIST)
		return VOS_ITER_CB_SKIP;
	if (rc != 0) {
		if (!oiter->it_iter.it_show_uncommitted || rc != -DER_INPROGRESS)
			return rc;
		/** Mark the entry as uncommitted but return it to the iterator */
		ent->ie_vis_flags = VOS_IT_UNCOMMITTED;
	} else {
		ent->ie_vis_flags = VOS_VIS_FLAG_VISIBLE;
		if (oiter->it_ilog_info.ii_create == 0) {
			/* The key has no visible subtrees so mark it covered */
			ent->ie_vis_flags = VOS_VIS_FLAG_COVERED;
		}
	}

	ent->ie_epoch = epr.epr_hi;
	ent->ie_punch = oiter->it_ilog_info.ii_next_punch;
	ent->ie_obj_punch = oiter->it_obj->obj_ilog_info.ii_next_punch;
	vos_ilog_last_update(&krec->kr_ilog, ts_type, &ent->ie_last_update,
			     !!oiter->it_iter.it_for_sysdb);

	return 0;
}

static int
key_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *ent,
	       daos_anchor_t *anchor, bool check_existence, uint32_t flags)
{

	uint64_t		 start_seq;
	vos_iter_desc_t		 desc;
	struct vos_rec_bundle	 rbund;
	struct vos_krec_df	*krec;
	struct dtx_handle	*dth;
	uint64_t		 feats;
	uint32_t		 ts_type;
	unsigned int		 acts;
	int			 rc;
	struct vos_object	*obj = oiter->it_obj;
	bool			 is_sysdb = obj->obj_cont->vc_pool->vp_sysdb;

	rc = key_iter_fetch_helper(oiter, &rbund, &ent->ie_key, anchor);
	D_ASSERTF(check_existence || rc != -DER_NONEXIST,
		  "Iterator should probe before fetch\n");
	if (rc != 0)
		return rc;

	D_ASSERT(rbund.rb_krec);
	krec = rbund.rb_krec;

	if (check_existence && oiter->it_iter.it_filter_cb != NULL &&
	    (flags & VOS_ITER_PROBE_AGAIN) == 0) {
		desc.id_type = oiter->it_iter.it_type;
		desc.id_key = ent->ie_key;
		desc.id_parent_punch = oiter->it_punched.pr_epc;
		if (krec->kr_bmap & KREC_BF_BTR)
			feats = dbtree_feats_get(&krec->kr_btr);
		else
			feats = evt_feats_get(&krec->kr_evt);
		if (!vos_feats_agg_time_get(feats, &desc.id_agg_write)) {
			if (desc.id_type == VOS_ITER_DKEY)
				ts_type = VOS_TS_TYPE_DKEY;
			else
				ts_type = VOS_TS_TYPE_AKEY;
			vos_ilog_last_update(&krec->kr_ilog, ts_type, &desc.id_agg_write,
					     !!oiter->it_iter.it_for_sysdb);
		}

		acts = 0;
		start_seq = vos_sched_seq(is_sysdb);
		dth = vos_dth_get(is_sysdb);
		vos_dth_set(NULL, is_sysdb);
		rc = oiter->it_iter.it_filter_cb(vos_iter2hdl(&oiter->it_iter), &desc,
						 oiter->it_iter.it_filter_arg,
						 &acts);
		vos_dth_set(dth, is_sysdb);
		if (rc != 0)
			return rc;
		if (start_seq != vos_sched_seq(is_sysdb))
			acts |= VOS_ITER_CB_YIELD;
		if (acts & (VOS_ITER_CB_EXIT | VOS_ITER_CB_ABORT | VOS_ITER_CB_RESTART |
			    VOS_ITER_CB_DELETE | VOS_ITER_CB_YIELD))
			return acts;
		if (acts & VOS_ITER_CB_SKIP)
			return VOS_ITER_CB_SKIP;
	}

	return key_iter_fill(krec, oiter, check_existence, ent);
}

static int
key_iter_fetch_root(struct vos_obj_iter *oiter, vos_iter_type_t type,
		    struct vos_iter_info *info)
{
	struct vos_object	*obj = oiter->it_obj;
	struct evt_desc_cbs      cbs;
	struct vos_krec_df	*krec;
	struct vos_rec_bundle	 rbund;
	d_iov_t			 keybuf;
	int			 rc;

	rc = key_iter_fetch_helper(oiter, &rbund, &keybuf, NULL);

	if (rc != 0) {
		D_ERROR("Could not fetch key: "DF_RC"\n", DP_RC(rc));
		return rc;
	}

	krec = rbund.rb_krec;
	info->ii_vea_info = obj->obj_cont->vc_pool->vp_vea_info;
	info->ii_uma = vos_obj2uma(obj);

	info->ii_epr = oiter->it_epr;
	info->ii_punched = oiter->it_punched;
	info->ii_filter_cb = oiter->it_iter.it_filter_cb;
	info->ii_filter_arg = oiter->it_iter.it_filter_arg;
	/* Update the lower bound for nested iterator */
	rc = key_iter_ilog_check(krec, oiter, &info->ii_epr, false, NULL);
	if (rc != 0)
		return rc;

	if (vos_epc_punched(info->ii_punched.pr_epc,
			    info->ii_punched.pr_minor_epc,
			    &oiter->it_ilog_info.ii_prior_punch))
		info->ii_punched = oiter->it_ilog_info.ii_prior_punch;

	if (type == VOS_ITER_RECX) {
		if ((krec->kr_bmap & KREC_BF_EVT) == 0)
			return -DER_NONEXIST;
		info->ii_evt = &krec->kr_evt;
	} else if (type == VOS_ITER_SINGLE || (krec->kr_bmap & KREC_BF_NO_AKEY) == 0) {
		if ((krec->kr_bmap & KREC_BF_BTR) == 0)
			return -DER_NONEXIST;
		info->ii_btr = &krec->kr_btr;
	} else {
		D_ASSERTF(type == VOS_ITER_AKEY, "type = %d\n", type);
		D_ASSERTF(krec->kr_bmap & KREC_BF_NO_AKEY, "krec->kr_bmap = %x\n", krec->kr_bmap);
		/** For fake akey, we open the subtree and store it in the
		 * iterator handle.  For nested case, go ahead and open the
		 * subtree
		 */
		if (krec->kr_bmap & KREC_BF_EVT) {
			vos_evt_desc_cbs_init(&cbs, vos_obj2pool(obj), vos_cont2hdl(obj->obj_cont));
			rc = evt_open(&krec->kr_evt, info->ii_uma, &cbs, &info->ii_tree_hdl);
			if (rc) {
				D_DEBUG(DB_TRACE,
					"Failed to open tree for nested iterator:"
					" rc = " DF_RC "\n",
					DP_RC(rc));
				return rc;
			}
			info->ii_fake_akey_flag = VOS_IT_DKEY_EV;
		} else {
			rc = dbtree_open_inplace_ex(&krec->kr_btr, info->ii_uma,
						    vos_cont2hdl(obj->obj_cont), vos_obj2pool(obj),
						    &info->ii_tree_hdl);
			if (rc) {
				D_DEBUG(DB_TRACE,
					"Failed to open tree for nested iterator:"
					" rc = " DF_RC "\n",
					DP_RC(rc));
				return rc;
			}
			info->ii_fake_akey_flag = VOS_IT_DKEY_SV;
		}
		info->ii_ilog_info = &oiter->it_ilog_info;
		info->ii_dkey_krec = krec;
	}

	return 0;
}

static int
key_iter_copy(struct vos_obj_iter *oiter, vos_iter_entry_t *ent,
	      d_iov_t *iov_out)
{
	if (ent->ie_key.iov_len > iov_out->iov_buf_len)
		return -DER_OVERFLOW;

	D_ASSERT(ent->ie_key.iov_buf != NULL);
	D_ASSERT(iov_out->iov_buf != NULL);

	memcpy(iov_out->iov_buf, ent->ie_key.iov_buf, ent->ie_key.iov_len);
	iov_out->iov_len = ent->ie_key.iov_len;
	return 0;
}

/** Check the current key */
static int
key_iter_match_probe(struct vos_obj_iter *oiter, daos_anchor_t *anchor, uint32_t flags)
{
	static __thread vos_iter_entry_t	entry;
	int					rc;

retry:
	rc = key_iter_fetch(oiter, &entry, anchor, true, flags);
	if (rc == VOS_ITER_CB_SKIP) {
		rc = dbtree_iter_next(oiter->it_hdl);
		if (rc == 0)
			goto retry;
	}
	D_ASSERT(rc <= 0 || (rc & (VOS_ITER_CB_EXIT | VOS_ITER_CB_DELETE | VOS_ITER_CB_YIELD |
				   VOS_ITER_CB_ABORT)) != 0);
	VOS_TX_TRACE_FAIL(rc, "match failed, rc="DF_RC"\n",
			  DP_RC(rc));
	return rc;
}

static int
key_iter_probe(struct vos_obj_iter *oiter, daos_anchor_t *anchor, uint32_t flags)
{
	int	next_opc;
	int	rc;

	next_opc = (flags & VOS_ITER_PROBE_NEXT) ? BTR_PROBE_GT : BTR_PROBE_GE;

	rc = dbtree_iter_probe(oiter->it_hdl,
			       vos_anchor_is_zero(anchor) ? BTR_PROBE_FIRST : next_opc,
			       vos_iter_intent(&oiter->it_iter),
			       NULL, anchor);
	if (rc)
		D_GOTO(out, rc);

	rc = key_iter_match_probe(oiter, anchor, flags);
 out:
	return rc;
}

static int
key_iter_next(struct vos_obj_iter *oiter, daos_anchor_t *anchor)
{
	int	rc;

	rc = dbtree_iter_next(oiter->it_hdl);
	if (rc)
		D_GOTO(out, rc);

	rc = key_iter_match_probe(oiter, anchor, 0);
out:
	return rc;
}

/**
 * Iterator for the d-key tree.
 */
static int
dkey_iter_prepare(struct vos_obj_iter *oiter)
{
	int	rc;

	rc = dbtree_iter_prepare(oiter->it_obj->obj_toh, 0, &oiter->it_hdl);

	return rc;
}

/**
 * Iterator for the akey tree.
 */
static int
akey_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *dkey,
		  struct vos_ts_set *ts_set)
{
	daos_handle_t		 toh;
	struct vos_krec_df      *krec = NULL;
	int                      rc;

	rc = key_ilog_prepare_dkey(oiter, dkey, &toh, &krec, ts_set);
	if (rc != 0)
		goto failed;

	if (krec->kr_bmap & KREC_BF_NO_AKEY) {
		/** In such case, toh will refer to a child tree so we an
		 * initialize its iterator in such case as it is needed.
		 * We also set the type of the tree here so we know what
		 * type of nested iterator we need to use.
		 */
		oiter->it_hdl = toh;
		if (krec->kr_bmap & KREC_BF_EVT)
			oiter->it_flags |= VOS_IT_DKEY_EV;
		else
			oiter->it_flags |= VOS_IT_DKEY_SV;
		oiter->it_fake_akey = 0;
		oiter->it_dkey_krec = krec;
	} else {
		/* see BTR_ITER_EMBEDDED for the details */
		rc = dbtree_iter_prepare(toh, BTR_ITER_EMBEDDED, &oiter->it_hdl);
		key_tree_release(toh, false);
	}

	if (rc == 0)
		return 0;

failed:
	VOS_TX_LOG_FAIL(rc, "Could not prepare akey iterator "DF_RC"\n",
			DP_RC(rc));
	return rc;
}

static int
prepare_key_from_toh(struct vos_obj_iter *oiter, daos_handle_t toh)
{
	return dbtree_iter_prepare(toh, 0, &oiter->it_hdl);
}

/**
 * Record extent (recx) iterator
 */

/**
 * Record extent (recx) iterator
 */
static int singv_iter_fetch(struct vos_obj_iter *oiter,
			   vos_iter_entry_t *it_entry,
			   daos_anchor_t *anchor);
/**
 * Prepare the iterator for the recx tree.
 */
static int
singv_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *dkey,
		   daos_key_t *akey)
{
	struct vos_krec_df      *krec = NULL;
	daos_handle_t		 ak_toh;
	daos_handle_t		 sv_toh;
	int			 rc;

	rc = key_ilog_prepare_dkey(oiter, dkey, &ak_toh, &krec, NULL);
	if (rc != 0)
		return rc;

	if (krec->kr_bmap & KREC_BF_NO_AKEY) {
		sv_toh = ak_toh;
		ak_toh = DAOS_HDL_INVAL;
	} else {
		rc = key_ilog_prepare(oiter, ak_toh, VOS_BTR_AKEY, akey, 0, &sv_toh, NULL,
				      &oiter->it_epr, &oiter->it_punched, &oiter->it_ilog_info,
				      NULL);
		if (rc != 0)
			D_GOTO(failed_1, rc);
	}

	/* see BTR_ITER_EMBEDDED for the details */
	rc = dbtree_iter_prepare(sv_toh, BTR_ITER_EMBEDDED, &oiter->it_hdl);
	if (rc != 0)
		D_DEBUG(DB_IO, "Cannot prepare singv iterator: "DF_RC"\n",
			DP_RC(rc));
	key_tree_release(sv_toh, false);
 failed_1:
	if (daos_handle_is_valid(ak_toh))
		key_tree_release(ak_toh, false);
	return rc;
}

/**
 * Probe the single value based on @opc and conditions in @entry (epoch),
 * return the matched one to @entry.
 */
static int
singv_iter_probe_fetch(struct vos_obj_iter *oiter, dbtree_probe_opc_t opc,
		       vos_iter_entry_t *entry)
{
	struct vos_svt_key	key;
	d_iov_t			kiov;
	int			rc;

	d_iov_set(&kiov, &key, sizeof(key));
	key.sk_epoch = entry->ie_epoch;
	key.sk_minor_epc = entry->ie_minor_epc;

	rc = dbtree_iter_probe(oiter->it_hdl, opc,
			       vos_iter_intent(&oiter->it_iter), &kiov, NULL);
	if (rc != 0)
		return rc;

	memset(entry, 0, sizeof(*entry));
	rc = singv_iter_fetch(oiter, entry, NULL);
	return rc;
}

/**
 * Find the data that was written before/in the specified epoch of @oiter
 * for the recx in @entry. If this recx has no data for this epoch, then
 * this function will move on to the next recx and repeat this process.
 */
static int
singv_iter_probe_epr(struct vos_obj_iter *oiter, vos_iter_entry_t *entry)
{
	daos_epoch_range_t *epr = &oiter->it_epr;

	while (1) {
		int	opc;
		int	rc;

		switch (oiter->it_epc_expr) {
		default:
			return -DER_INVAL;

		case VOS_IT_EPC_EQ:
			if (entry->ie_epoch > epr->epr_hi)
				return -DER_NONEXIST;

			if (entry->ie_epoch < epr->epr_lo) {
				entry->ie_epoch = epr->epr_lo;
				opc = BTR_PROBE_EQ;
				break;
			}
			return 0;

		case VOS_IT_EPC_RE:
			if (entry->ie_epoch > epr->epr_hi)
				return -DER_NONEXIST;

			if (entry->ie_epoch < epr->epr_lo) {
				entry->ie_epoch = epr->epr_lo;
				opc = BTR_PROBE_GE;
				break;
			}
			return 0;

		case VOS_IT_EPC_RR:
			if (entry->ie_epoch < epr->epr_lo) {
				return -DER_NONEXIST; /* end of story */
			}

			if (entry->ie_epoch > epr->epr_hi) {
				entry->ie_epoch = epr->epr_hi;
				opc = BTR_PROBE_LE;
				break;
			}
			return 0;

		case VOS_IT_EPC_GE:
			if (entry->ie_epoch < epr->epr_lo) {
				entry->ie_epoch = epr->epr_lo;
				opc = BTR_PROBE_GE;
				break;
			}
			return 0;

		case VOS_IT_EPC_LE:
			if (entry->ie_epoch > epr->epr_lo) {
				entry->ie_epoch = epr->epr_lo;
				opc = BTR_PROBE_LE;
				break;
			}
			return 0;
		}
		rc = singv_iter_probe_fetch(oiter, opc, entry);
		if (rc != 0)
			return rc;
	}
}

static int
singv_iter_probe(struct vos_obj_iter *oiter, daos_anchor_t *anchor, uint32_t flags)
{
	vos_iter_entry_t	entry;
	daos_anchor_t		tmp = {0};
	int			next_opc;
	int			opc;
	int			rc;

	if (oiter->it_epc_expr == VOS_IT_EPC_RR) {
		next_opc = (flags & VOS_ITER_PROBE_NEXT) ? BTR_PROBE_LT : BTR_PROBE_LE;
		opc = vos_anchor_is_zero(anchor) ? BTR_PROBE_LAST : next_opc;
	} else {
		next_opc = (flags & VOS_ITER_PROBE_NEXT) ? BTR_PROBE_GT : BTR_PROBE_GE;
		opc = vos_anchor_is_zero(anchor) ? BTR_PROBE_FIRST : next_opc;
	}

	rc = dbtree_iter_probe(oiter->it_hdl, opc,
			       vos_iter_intent(&oiter->it_iter), NULL, anchor);
	if (rc != 0)
		return rc;

	memset(&entry, 0, sizeof(entry));
	rc = singv_iter_fetch(oiter, &entry, &tmp);
	if (rc != 0)
		return rc;

	if (anchor != NULL) {
		if (memcmp(anchor, &tmp, sizeof(tmp)) == 0)
			return 0;

		D_DEBUG(DB_IO, "Can't find the provided anchor\n");
		/**
		 * the original recx has been merged/discarded, so we need to
		 * call singv_iter_probe_epr() and check if the current record
		 * can match the condition.
		 */
	}
	rc = singv_iter_probe_epr(oiter, &entry);
	return rc;
}

static int
singv_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
		 daos_anchor_t *anchor)
{
	struct vos_svt_key	key;
	struct vos_rec_bundle	rbund;
	d_iov_t			kiov;
	d_iov_t			riov;
	int			rc;

	d_iov_set(&kiov, &key, sizeof(key));

	tree_rec_bundle2iov(&rbund, &riov);
	rbund.rb_biov	= &it_entry->ie_biov;
	rbund.rb_csum	= &it_entry->ie_csum;

	memset(&it_entry->ie_biov, 0, sizeof(it_entry->ie_biov));
	ci_set_null(rbund.rb_csum);

	rc = dbtree_iter_fetch(oiter->it_hdl, &kiov, &riov, anchor);
	if (rc)
		D_GOTO(out, rc);

	it_entry->ie_vis_flags = VOS_VIS_FLAG_VISIBLE;
	it_entry->ie_epoch	 = key.sk_epoch;
	it_entry->ie_minor_epc	 = key.sk_minor_epc;
	if (vos_epc_punched(it_entry->ie_epoch, it_entry->ie_minor_epc,
			    &oiter->it_punched)) {
		/* entry is covered by a key/object punch */
		it_entry->ie_vis_flags = VOS_VIS_FLAG_COVERED;
	}
	it_entry->ie_rsize	 = rbund.rb_rsize;
	it_entry->ie_gsize	 = rbund.rb_gsize;
	it_entry->ie_ver	 = rbund.rb_ver;
	it_entry->ie_recx.rx_idx = 0;
	it_entry->ie_recx.rx_nr  = 1;
	it_entry->ie_dtx_state	 = rbund.rb_dtx_state;
 out:
	return rc;
}

static int
singv_iter_next(struct vos_obj_iter *oiter)
{
	vos_iter_entry_t entry;
	int		 rc;
	int		 opc;
	int		 vis_flag;

	/* Only one SV rec is visible for the given @epoch,
	 * so return -DER_NONEXIST directly for the next().
	 */
	vis_flag = oiter->it_flags & VOS_IT_RECX_COVERED;
	if (vis_flag == VOS_IT_RECX_VISIBLE) {
		D_ASSERT(oiter->it_epc_expr == VOS_IT_EPC_RR ||
			 oiter->it_epc_expr == VOS_IT_EPC_RE);
		return -DER_NONEXIST;
	}

	memset(&entry, 0, sizeof(entry));
	rc = singv_iter_fetch(oiter, &entry, NULL);
	if (rc != 0)
		return rc;

	if (oiter->it_epc_expr == VOS_IT_EPC_RE)
		entry.ie_epoch += 1;
	else if (oiter->it_epc_expr == VOS_IT_EPC_RR)
		entry.ie_epoch -= 1;
	else
		entry.ie_epoch = DAOS_EPOCH_MAX;

	opc = (oiter->it_epc_expr == VOS_IT_EPC_RR) ?
		BTR_PROBE_LE : BTR_PROBE_GE;

	rc = singv_iter_probe_fetch(oiter, opc, &entry);
	if (rc != 0)
		return rc;

	rc = singv_iter_probe_epr(oiter, &entry);
	return rc;
}

#define recx_flags_set(flags, setting)	\
	(((flags) & (setting)) == (setting))

D_CASSERT((int)VOS_IT_RECX_COVERED == (int)EVT_ITER_COVERED);
D_CASSERT((int)VOS_IT_RECX_VISIBLE == (int)EVT_ITER_VISIBLE);
D_CASSERT((int)VOS_IT_RECX_SKIP_HOLES == (int)EVT_ITER_SKIP_HOLES);

static inline uint32_t
recx_get_flags(struct vos_obj_iter *oiter, bool embed)
{
	uint32_t options   = 0;
	uint32_t vis_flags = oiter->it_flags & (VOS_IT_RECX_COVERED |
						VOS_IT_RECX_SKIP_HOLES);

	if (embed)
		options |= EVT_ITER_EMBEDDED;

	options |= vis_flags;
	if (oiter->it_flags & VOS_IT_RECX_REVERSE)
		options |= EVT_ITER_REVERSE;
	if (oiter->it_flags & VOS_IT_FOR_PURGE)
		options |= EVT_ITER_FOR_PURGE;
	if (oiter->it_flags & VOS_IT_FOR_DISCARD)
		options |= EVT_ITER_FOR_DISCARD;
	if (oiter->it_flags & VOS_IT_FOR_MIGRATION)
		options |= EVT_ITER_FOR_MIGRATION;
	return options;
}

/**
 * Sets the range filter.
 */
static inline void
recx2filter(struct evt_filter *filter, daos_recx_t *recx)
{
	if (recx->rx_nr == 0) {
		filter->fr_ex.ex_lo = 0ULL;
		filter->fr_ex.ex_hi = ~(0ULL);
	} else {
		filter->fr_ex.ex_lo = recx->rx_idx;
		filter->fr_ex.ex_hi = recx->rx_idx + recx->rx_nr - 1;
	}
}

/**
 * Prepare the iterator for the recx tree.
 */
static int
recx_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *dkey,
		  daos_key_t *akey, struct vos_ts_set *ts_set)
{
	struct vos_krec_df      *krec   = NULL;
	struct evt_filter	 filter = {0};
	daos_handle_t		 ak_toh;
	daos_handle_t		 rx_toh;
	int			 rc;
	uint32_t		 options;

	rc = key_ilog_prepare_dkey(oiter, dkey, &ak_toh, &krec, ts_set);
	if (rc != 0)
		return rc;

	if (krec->kr_bmap & KREC_BF_NO_AKEY) {
		rx_toh = ak_toh;
		ak_toh = DAOS_HDL_INVAL;
	} else {
		rc = key_ilog_prepare(oiter, ak_toh, VOS_BTR_AKEY, akey, SUBTR_EVT, &rx_toh, NULL,
				      &oiter->it_epr, &oiter->it_punched, &oiter->it_ilog_info,
				      ts_set);
		if (rc != 0)
			D_GOTO(failed, rc);
	}

	recx2filter(&filter, &oiter->it_recx);
	filter.fr_epr.epr_lo = oiter->it_epr.epr_lo;
	filter.fr_epr.epr_hi = oiter->it_iter.it_bound;
	filter.fr_epoch = oiter->it_epr.epr_hi;
	filter.fr_punch_epc = oiter->it_punched.pr_epc;
	filter.fr_punch_minor_epc = oiter->it_punched.pr_minor_epc;
	options                   = recx_get_flags(oiter, true);
	rc = evt_iter_prepare(rx_toh, options, &filter,
			      &oiter->it_hdl);
	if (rc != 0) {
		D_DEBUG(DB_IO, "Cannot prepare recx iterator : "DF_RC"\n",
			DP_RC(rc));
	}
	key_tree_release(rx_toh, true);
 failed:
	if (daos_handle_is_valid(ak_toh))
		key_tree_release(ak_toh, false);
	return rc;
}
static int
recx_iter_probe(struct vos_obj_iter *oiter, daos_anchor_t *anchor)
{
	int	opc;
	int	rc;

	opc = vos_anchor_is_zero(anchor) ? EVT_ITER_FIRST : EVT_ITER_FIND;
	rc = evt_iter_probe(oiter->it_hdl, opc, NULL, vos_anchor_is_zero(anchor) ? NULL : anchor);
	return rc;
}

static int
recx_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
		daos_anchor_t *anchor)
{
	struct evt_extent	*ext;
	struct evt_entry	 entry;
	int			 rc;
	unsigned int		 inob;

	rc = evt_iter_fetch(oiter->it_hdl, &inob, &entry, anchor);
	if (rc != 0)
		D_GOTO(out, rc);

	memset(it_entry, 0, sizeof(*it_entry));

	ext = &entry.en_sel_ext;
	it_entry->ie_epoch	 = entry.en_epoch;
	it_entry->ie_minor_epc	 = entry.en_minor_epc;
	it_entry->ie_recx.rx_idx = ext->ex_lo;
	it_entry->ie_recx.rx_nr	 = evt_extent_width(ext);
	ext = &entry.en_ext;
	/* Also export the original extent and the visibility flags */
	it_entry->ie_orig_recx.rx_idx = ext->ex_lo;
	it_entry->ie_orig_recx.rx_nr	 = evt_extent_width(ext);
	it_entry->ie_vis_flags = entry.en_visibility;
	it_entry->ie_rsize	= inob;
	it_entry->ie_ver	= entry.en_ver;
	it_entry->ie_csum	= entry.en_csum;
	it_entry->ie_dtx_state	= dtx_alb2state(entry.en_avail_rc);
	bio_iov_set(&it_entry->ie_biov, entry.en_addr,
		    it_entry->ie_recx.rx_nr * it_entry->ie_rsize);
 out:
	return rc;
}

static int
recx_iter_copy(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
	       d_iov_t *iov_out)
{
	struct bio_io_context	*bioc;
	struct umem_instance	*umem;
	struct bio_iov		*biov = &it_entry->ie_biov;

	D_ASSERT(bio_iov2buf(biov) == NULL);
	D_ASSERT(iov_out->iov_buf != NULL);

	/* Skip copy and return success for a punched record */
	if (bio_addr_is_hole(&biov->bi_addr))
		return 0;
	else if (iov_out->iov_buf_len < bio_iov2len(biov))
		return -DER_OVERFLOW;

	/*
	 * Set 'iov_len' beforehand, cause it will be used as copy
	 * size in vos_media_read().
	 */
	iov_out->iov_len = bio_iov2len(biov);
	bioc = vos_data_ioctxt(oiter->it_obj->obj_cont->vc_pool);
	umem = &oiter->it_obj->obj_cont->vc_pool->vp_umm;

	return vos_media_read(bioc, umem, biov->bi_addr, iov_out);
}

static int
recx_iter_next(struct vos_obj_iter *oiter)
{
	return evt_iter_next(oiter->it_hdl);
}

static int
recx_iter_fini(struct vos_obj_iter *oiter)
{
	return evt_iter_finish(oiter->it_hdl);
}

/**
 * common functions for iterator.
 */
static int vos_obj_iter_fini(struct vos_iterator *vitr);

/** prepare an object content iterator */
int
vos_obj_iter_prep(vos_iter_type_t type, vos_iter_param_t *param,
		  struct vos_iterator **iter_pp,
		  struct vos_ts_set *ts_set)
{
	struct vos_obj_iter	*oiter;
	struct vos_container	*cont = NULL;
	bool			 is_sysdb = false;
	struct dtx_handle	*dth = NULL;
	daos_epoch_t		 bound;
	int			 rc;

	D_ALLOC_PTR(oiter);
	if (oiter == NULL)
		return -DER_NOMEM;

	/* ip_hdl is dkey or akey tree open handle for vos_iterate_key() */
	if (param->ip_flags != VOS_IT_KEY_TREE) {
		D_ASSERT(!(param->ip_flags & VOS_IT_KEY_TREE));
		cont = vos_hdl2cont(param->ip_hdl);
		is_sysdb = cont->vc_pool->vp_sysdb;
		dth = vos_dth_get(is_sysdb);
	}

	bound = dtx_is_valid_handle(dth) ? dth->dth_epoch_bound :
		param->ip_epr.epr_hi;
	oiter->it_iter.it_bound = MAX(bound, param->ip_epr.epr_hi);
	oiter->it_iter.it_filter_cb = param->ip_filter_cb;
	oiter->it_iter.it_filter_arg = param->ip_filter_arg;
	vos_ilog_fetch_init(&oiter->it_ilog_info);
	oiter->it_iter.it_type = type;
	oiter->it_epr = param->ip_epr;
	oiter->it_epc_expr = param->ip_epc_expr;

	oiter->it_flags = param->ip_flags;
	oiter->it_recx = param->ip_recx;
	if (param->ip_flags & VOS_IT_FOR_PURGE)
		oiter->it_iter.it_for_purge = 1;
	if (param->ip_flags & VOS_IT_FOR_DISCARD)
		oiter->it_iter.it_for_discard = 1;
	if (param->ip_flags & VOS_IT_FOR_MIGRATION)
		oiter->it_iter.it_for_migration = 1;
	if (param->ip_flags & VOS_IT_FOR_AGG)
		oiter->it_iter.it_for_agg = 1;
	if (is_sysdb)
		oiter->it_iter.it_for_sysdb = 1;
	if (param->ip_flags == VOS_IT_KEY_TREE) {
		/** Prepare the iterator from an already open tree handle.   See
		 *  vos_iterate_key
		 */
		D_ASSERT(type == VOS_ITER_DKEY || type == VOS_ITER_AKEY);
		oiter->it_obj = param->ip_dkey.iov_buf;
		rc = prepare_key_from_toh(oiter, param->ip_hdl);
		goto done;
	}

	rc = vos_ts_set_add(ts_set, cont->vc_ts_idx, NULL, 0);
	D_ASSERT(rc == 0);

	/* XXX the condition epoch ranges could cover multiple versions of
	 * the object/key if it's punched more than once. However, rebuild
	 * system should guarantee this will never happen.
	 */
	rc = vos_obj_hold(vos_obj_cache_current(is_sysdb), cont,
			  param->ip_oid, &oiter->it_epr,
			  oiter->it_iter.it_bound,
			  (oiter->it_flags & VOS_IT_PUNCHED) ? 0 :
			  VOS_OBJ_VISIBLE, vos_iter_intent(&oiter->it_iter),
			  &oiter->it_obj, ts_set);
	if (rc != 0) {
		VOS_TX_LOG_FAIL(rc, "Could not hold object to iterate: "DF_RC
				"\n", DP_RC(rc));
		D_GOTO(failed, rc);
	}

	oiter->it_punched = oiter->it_obj->obj_ilog_info.ii_prior_punch;

	rc = obj_tree_init(oiter->it_obj);
	if (rc != 0)
		goto failed;

	switch (type) {
	default:
		D_ERROR("unknown iterator type %d.\n", type);
		rc = -DER_INVAL;
		break;

	case VOS_ITER_DKEY:
		rc = dkey_iter_prepare(oiter);
		break;

	case VOS_ITER_AKEY:
		rc = akey_iter_prepare(oiter, &param->ip_dkey, ts_set);
		break;

	case VOS_ITER_SINGLE:
		rc = singv_iter_prepare(oiter, &param->ip_dkey,
					&param->ip_akey);
		break;

	case VOS_ITER_RECX:
		rc = recx_iter_prepare(oiter, &param->ip_dkey, &param->ip_akey,
				       ts_set);
		break;
	}

done:
	if (rc != 0)
		D_GOTO(failed, rc);

	*iter_pp = &oiter->it_iter;
	return 0;
 failed:
	vos_obj_iter_fini(&oiter->it_iter);
	return rc;
}

int
vos_obj_dkey_iter_nested_tree_fetch(struct vos_iterator *iter, vos_iter_type_t type,
				    struct vos_iter_info *info)
{
	struct vos_obj_iter	*oiter = vos_iter2oiter(iter);
	int			 rc = 0;

	if (unlikely(type != VOS_ITER_AKEY)) {
		D_ERROR("Invalid nested iterator type for "
			"VOS_ITER_DKEY: %d\n",
			type);
		return -DER_INVAL;
	}

	rc = key_iter_fetch_root(oiter, type, info);

	if (rc != 0) {
		D_DEBUG(DB_TRACE,
			"Failed to fetch and initialize cursor "
			"subtree: rc=" DF_RC "\n",
			DP_RC(rc));
		return rc;
	}

	info->ii_obj = oiter->it_obj;

	return 0;
}

int
vos_obj_akey_iter_nested_tree_fetch(struct vos_iterator *iter, vos_iter_type_t type,
				    struct vos_iter_info *info)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);
	int                  rc    = 0;

	if (unlikely(type != VOS_ITER_RECX && type != VOS_ITER_SINGLE)) {
		D_ERROR("Invalid nested iterator type for "
			"VOS_ITER_AKEY: %d\n",
			type);
		return -DER_INVAL;
	}

	if (is_fake_iter(oiter)) {
		info->ii_vea_info = oiter->it_obj->obj_cont->vc_pool->vp_vea_info;
		info->ii_uma      = vos_obj2uma(oiter->it_obj);

		info->ii_epr        = oiter->it_epr;
		info->ii_punched    = oiter->it_punched;
		info->ii_filter_cb  = oiter->it_iter.it_filter_cb;
		info->ii_filter_arg = oiter->it_iter.it_filter_arg;

		if (vos_epc_punched(info->ii_punched.pr_epc, info->ii_punched.pr_minor_epc,
				    &oiter->it_ilog_info.ii_prior_punch))
			info->ii_punched = oiter->it_ilog_info.ii_prior_punch;

		info->ii_tree_hdl = oiter->it_hdl;
		/** Tells the prepare that it should use the handle to prepare
		 * the nested tree handle to prepare nested iterator
		 */
		info->ii_fake_akey_flag = oiter->it_flags & (VOS_IT_DKEY_SV | VOS_IT_DKEY_EV);
		goto out;
	}

	rc = key_iter_fetch_root(oiter, type, info);

	if (rc != 0) {
		D_DEBUG(DB_TRACE, "Failed to fetch and initialize cursor "
			"subtree: rc="DF_RC"\n", DP_RC(rc));
		return rc;
	}

out:
	info->ii_obj = oiter->it_obj;

	return 0;
}

int
vos_obj_invalid_iter_nested_tree_fetch(struct vos_iterator *iter, vos_iter_type_t type,
				       struct vos_iter_info *info)
{
	D_ERROR("Iterator type has no subtree\n");
	return -DER_INVAL;
}

static int
dkey_nested_iter_init(struct vos_obj_iter *oiter, struct vos_iter_info *info)
{
	int			 rc;
	struct vos_container	*cont = vos_hdl2cont(info->ii_hdl);
	uint64_t                 flags = 0;

	if ((oiter->it_flags & VOS_IT_PUNCHED) == 0)
		flags |= VOS_OBJ_VISIBLE;
	if (oiter->it_iter.it_for_agg)
		flags |= VOS_OBJ_AGGREGATE;
	if (oiter->it_iter.it_for_discard)
		flags |= VOS_OBJ_DISCARD;

	/* XXX the condition epoch ranges could cover multiple versions of
	 * the object/key if it's punched more than once. However, rebuild
	 * system should guarantee this will never happen.
	 */
	rc = vos_obj_hold(vos_obj_cache_current(cont->vc_pool->vp_sysdb), cont, info->ii_oid,
			  &info->ii_epr, oiter->it_iter.it_bound, flags,
			  vos_iter_intent(&oiter->it_iter), &oiter->it_obj, NULL);

	D_ASSERTF(rc != -DER_NONEXIST,
		  "Nested iterator called without setting probe");
	if (rc != 0) {
		/** -DER_NONEXIST and -DER_INPROGRESS should be caught earlier.
		 *  This function should only be called after a successful
		 *  probe.
		 */
		D_ERROR("Could not hold object: "DF_RC"\n", DP_RC(rc));
		return rc;
	}

	rc = obj_tree_init(oiter->it_obj);

	if (rc != 0)
		goto failed;

	info->ii_punched = oiter->it_obj->obj_ilog_info.ii_prior_punch;

	rc = dkey_iter_prepare(oiter);

	if (rc != 0)
		goto failed;

	return 0;
failed:
	vos_obj_release(vos_obj_cache_current(cont->vc_pool->vp_sysdb), oiter->it_obj, flags,
			false);

	return rc;
}

static inline int
nested_prep_common_init(struct vos_container *cont, struct vos_obj_iter **oiterp,
			struct vos_iter_info *info)
{
	struct vos_obj_iter     *oiter;
	struct dtx_handle	*dth;
	daos_epoch_t             bound;

	*oiterp = NULL;

	D_ALLOC_PTR(oiter);
	if (oiter == NULL)
		return -DER_NOMEM;

	*oiterp = oiter;
	vos_ilog_fetch_init(&oiter->it_ilog_info);
	dth                          = vos_dth_get(cont->vc_pool->vp_sysdb);
	bound = dtx_is_valid_handle(dth) ? dth->dth_epoch_bound :
		info->ii_epr.epr_hi;
	oiter->it_iter.it_bound = MAX(bound, info->ii_epr.epr_hi);
	oiter->it_epr = info->ii_epr;
	oiter->it_iter.it_filter_cb = info->ii_filter_cb;
	oiter->it_iter.it_filter_arg = info->ii_filter_arg;
	oiter->it_punched = info->ii_punched;
	oiter->it_epc_expr = info->ii_epc_expr;
	oiter->it_flags              = info->ii_flags;
	if (info->ii_flags & VOS_IT_FOR_PURGE)
		oiter->it_iter.it_for_purge = 1;
	if (info->ii_flags & VOS_IT_FOR_DISCARD)
		oiter->it_iter.it_for_discard = 1;
	if (info->ii_flags & VOS_IT_FOR_MIGRATION)
		oiter->it_iter.it_for_migration = 1;
	if (cont->vc_pool->vp_sysdb)
		oiter->it_iter.it_for_sysdb = 1;

	return 0;
}

static inline void
nested_prep_common_abort(struct vos_obj_iter *oiter)
{
	vos_ilog_fetch_finish(&oiter->it_ilog_info);
	D_FREE(oiter);
}

static int
vos_obj_dkey_iter_nested_prep(vos_iter_type_t type, struct vos_iter_info *info,
			      struct vos_iterator **iter_pp)
{
	struct vos_obj_iter *oiter;
	int                  rc = 0;

	if (type != VOS_ITER_DKEY) {
		D_ERROR("Unexpected type: %d\n", type);
		return -DER_INVAL;
	}

	rc = nested_prep_common_init(vos_hdl2cont(info->ii_hdl), &oiter, info);
	if (rc != 0)
		return rc;

	rc = dkey_nested_iter_init(oiter, info);
	if (rc == 0) {
		*iter_pp = &oiter->it_iter;
		return 0;
	}

	nested_prep_common_abort(oiter);
	return rc;
}

static int
vos_obj_akey_iter_nested_prep(vos_iter_type_t type, struct vos_iter_info *info,
			      struct vos_iterator **iter_pp)
{
	struct vos_object   *obj = info->ii_obj;
	struct vos_obj_iter *oiter;
	daos_handle_t        toh;
	int                  rc = 0;

	if (type != VOS_ITER_AKEY) {
		D_ERROR("Unexpected type: %d\n", type);
		return -DER_INVAL;
	}

	rc = nested_prep_common_init(obj->obj_cont, &oiter, info);

	oiter->it_obj = obj;

	if (info->ii_fake_akey_flag) {
		/** In this case, we already opened the subtree so just store it
		 * in the iterator handle for future use.
		 */
		vos_ilog_copy_info(&oiter->it_ilog_info, info->ii_ilog_info);
		oiter->it_hdl = info->ii_tree_hdl;
		oiter->it_flags |= info->ii_fake_akey_flag;
		oiter->it_fake_akey = 0;
		oiter->it_dkey_krec = info->ii_dkey_krec;
		goto success;
	}

	rc = dbtree_open_inplace_ex(info->ii_btr, info->ii_uma, vos_cont2hdl(obj->obj_cont),
				    vos_obj2pool(obj), &toh);
	if (rc) {
		D_DEBUG(DB_TRACE,
			"Failed to open tree for iterator:"
			" rc = " DF_RC "\n",
			DP_RC(rc));
		goto failed;
	}
	rc = dbtree_iter_prepare(toh, BTR_ITER_EMBEDDED, &oiter->it_hdl);

	key_tree_release(toh, false);

	if (rc != 0) {
		D_DEBUG(DB_TRACE, "Failed to prepare iterator: rc = "DF_RC"\n",
			DP_RC(rc));
		goto failed;
	}

success:
	*iter_pp = &oiter->it_iter;
	return 0;

failed:
	nested_prep_common_abort(oiter);
	return rc;
}

static int
vos_obj_iter_sv_nested_prep(vos_iter_type_t type, struct vos_iter_info *info,
			    struct vos_iterator **iter_pp)
{
	struct vos_object   *obj = info->ii_obj;
	struct vos_obj_iter *oiter;
	daos_handle_t        toh;
	int                  flags = BTR_ITER_EMBEDDED;
	int                  rc    = 0;

	if (type != VOS_ITER_SINGLE) {
		D_ERROR("Unexpected type: %d\n", type);
		return -DER_INVAL;
	}

	rc = nested_prep_common_init(obj->obj_cont, &oiter, info);

	oiter->it_obj = obj;
	if (info->ii_fake_akey_flag) {
		D_ASSERTF(info->ii_fake_akey_flag == VOS_IT_DKEY_SV, "Invalid value for flag: %x\n",
			  info->ii_fake_akey_flag);
		toh = info->ii_tree_hdl;
		/* Don't use embedded iterator here because we may not be the
		 * only nested iterator
		 */
		flags = 0;
		goto prepare;
	}

	rc = dbtree_open_inplace_ex(info->ii_btr, info->ii_uma, vos_cont2hdl(obj->obj_cont),
				    vos_obj2pool(obj), &toh);
	if (rc) {
		D_DEBUG(DB_TRACE,
			"Failed to open tree for iterator:"
			" rc = " DF_RC "\n",
			DP_RC(rc));
		goto failed;
	}
prepare:
	rc = dbtree_iter_prepare(toh, flags, &oiter->it_hdl);

	if (info->ii_fake_akey_flag == 0)
		key_tree_release(toh, false);

	if (rc != 0) {
		D_DEBUG(DB_TRACE, "Failed to prepare iterator: rc = " DF_RC "\n", DP_RC(rc));
		goto failed;
	}

	*iter_pp = &oiter->it_iter;
	return 0;

failed:
	nested_prep_common_abort(oiter);
	return rc;
}

static int
vos_obj_ev_iter_nested_prep(vos_iter_type_t type, struct vos_iter_info *info,
			    struct vos_iterator **iter_pp)
{
	struct vos_object   *obj = info->ii_obj;
	struct vos_obj_iter *oiter;
	struct evt_desc_cbs  cbs;
	struct evt_filter    filter = {0};
	daos_handle_t        toh;
	bool                 embed = true;
	int                  rc    = 0;
	uint32_t             options;

	if (type != VOS_ITER_RECX) {
		D_ERROR("Unexpected type: %d\n", type);
		return -DER_INVAL;
	}

	rc = nested_prep_common_init(obj->obj_cont, &oiter, info);

	oiter->it_obj = obj;

	if (info->ii_fake_akey_flag) {
		D_ASSERTF(info->ii_fake_akey_flag == VOS_IT_DKEY_EV, "Invalid value for flag: %x\n",
			  info->ii_fake_akey_flag);
		toh = info->ii_tree_hdl;
		/* We may not be the only nested iterator so don't use embedded
		 * iterator here
		 */
		embed = false;
		goto prepare;
	}

	vos_evt_desc_cbs_init(&cbs, vos_obj2pool(obj), vos_cont2hdl(obj->obj_cont));
	rc = evt_open(info->ii_evt, info->ii_uma, &cbs, &toh);
	if (rc) {
		D_DEBUG(DB_TRACE,
			"Failed to open tree for iterator:"
			" rc = " DF_RC "\n",
			DP_RC(rc));
		goto failed;
	}
prepare:
	recx2filter(&filter, &info->ii_recx);
	filter.fr_epr.epr_lo      = oiter->it_epr.epr_lo;
	filter.fr_epr.epr_hi      = oiter->it_iter.it_bound;
	filter.fr_epoch           = oiter->it_epr.epr_hi;
	filter.fr_punch_epc       = oiter->it_punched.pr_epc;
	filter.fr_punch_minor_epc = oiter->it_punched.pr_minor_epc;
	options                   = recx_get_flags(oiter, embed);
	rc                        = evt_iter_prepare(toh, options, &filter, &oiter->it_hdl);
	if (info->ii_fake_akey_flag == 0)
		key_tree_release(toh, type == VOS_ITER_RECX);

	if (rc != 0) {
		D_DEBUG(DB_TRACE, "Failed to prepare iterator: rc = " DF_RC "\n", DP_RC(rc));
		goto failed;
	}

	*iter_pp = &oiter->it_iter;
	return 0;
failed:
	nested_prep_common_abort(oiter);
	return rc;
}

/** release the object iterator */
static int
vos_obj_iter_fini(struct vos_iterator *iter)
{
	struct vos_obj_iter	*oiter = vos_iter2oiter(iter);
	int			 rc;
	struct vos_object	*object;
	uint64_t                 flags = 0;

	if (daos_handle_is_inval(oiter->it_hdl))
		D_GOTO(out, rc = -DER_NO_HDL);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		break;

	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
		if (is_fake_iter(oiter)) {
			/** In fake akey iterator, we use the subtree handle, so
			 * release it here.
			 */
			key_tree_release(oiter->it_hdl, fake_iter_child_is_array(oiter));
			break;
		}
	case VOS_ITER_SINGLE:
		rc = dbtree_iter_finish(oiter->it_hdl);
		break;
	case VOS_ITER_RECX:
		rc = recx_iter_fini(oiter);
		break;
	}
 out:
	/* Release the object only if we didn't borrow it from the parent
	 * iterator.   The generic code reference counts the iterators
	 * to ensure that a parent never gets removed before all nested
	 * iterators are finalized
	 */
	object = oiter->it_obj;
	if (oiter->it_flags != VOS_IT_KEY_TREE && object != NULL &&
	    (iter->it_type == VOS_ITER_DKEY || !iter->it_from_parent)) {
		if (iter->it_type == VOS_ITER_DKEY) {
			if (iter->it_for_discard)
				flags = VOS_OBJ_DISCARD;
			else if (iter->it_for_agg)
				flags = VOS_OBJ_AGGREGATE;
		}
		vos_obj_release(vos_obj_cache_current(object->obj_cont->vc_pool->vp_sysdb), object,
				flags, false);
	}

	vos_ilog_fetch_finish(&oiter->it_ilog_info);
	D_FREE(oiter);
	return 0;
}

int
vos_obj_dkey_iter_probe(struct vos_iterator *iter, daos_anchor_t *anchor, uint32_t flags)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_DKEY, "type is %d\n", iter->it_type);

	return key_iter_probe(oiter, anchor, flags);
}

int
vos_obj_akey_iter_probe(struct vos_iterator *iter, daos_anchor_t *anchor, uint32_t flags)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_AKEY, "type is %d\n", iter->it_type);

	if (is_fake_iter(oiter)) {
		if (vos_anchor_is_zero(anchor) || (flags & VOS_ITER_PROBE_NEXT) == 0) {
			oiter->it_fake_akey = '0';
			return 0;
		}
		/** Indicate we are done iterating */
		oiter->it_fake_akey = 0;
		return -DER_NONEXIST;
	}

	return key_iter_probe(oiter, anchor, flags);
}

int
vos_obj_sv_iter_probe(struct vos_iterator *iter, daos_anchor_t *anchor, uint32_t flags)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_SINGLE, "type is %d\n", iter->it_type);

	return singv_iter_probe(oiter, anchor, flags);
}

int
vos_obj_ev_iter_probe(struct vos_iterator *iter, daos_anchor_t *anchor, uint32_t flags)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_RECX, "type is %d\n", iter->it_type);

	return recx_iter_probe(oiter, anchor);
}

static int
vos_obj_dkey_iter_next(struct vos_iterator *iter, daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_DKEY, "type is %d\n", iter->it_type);

	return key_iter_next(oiter, anchor);
}

static int
vos_obj_akey_iter_next(struct vos_iterator *iter, daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_AKEY, "type is %d\n", iter->it_type);
	if (is_fake_iter(oiter)) {
		/** Indicate we are done iterating */
		oiter->it_fake_akey = 0;
		return -DER_NONEXIST;
	}

	return key_iter_next(oiter, anchor);
}

static int
vos_obj_sv_iter_next(struct vos_iterator *iter, daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_SINGLE, "type is %d\n", iter->it_type);
	return singv_iter_next(oiter);
}

static int
vos_obj_ev_iter_next(struct vos_iterator *iter, daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_RECX, "type is %d\n", iter->it_type);
	return recx_iter_next(oiter);
}

static int
vos_obj_dkey_iter_fetch(struct vos_iterator *iter, vos_iter_entry_t *it_entry,
			daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_DKEY, "type is %d\n", iter->it_type);

	return key_iter_fetch(oiter, it_entry, anchor, false, 0);
}

static int
vos_obj_akey_iter_fetch(struct vos_iterator *iter, vos_iter_entry_t *it_entry,
			daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_AKEY, "type is %d\n", iter->it_type);
	if (is_fake_iter(oiter)) {
		D_ASSERTF(oiter->it_fake_akey == '0', "Must probe before fetch");
		if (anchor != NULL)
			vos_fake_anchor_create(anchor);
		if (fake_iter_child_is_array(oiter))
			it_entry->ie_child_type = VOS_ITER_RECX;
		else
			it_entry->ie_child_type = VOS_ITER_SINGLE;
		it_entry->ie_vis_flags = VOS_VIS_FLAG_VISIBLE;
		if (oiter->it_ilog_info.ii_create == 0) {
			/* The key has no visible subtrees so mark it covered */
			it_entry->ie_vis_flags = VOS_VIS_FLAG_COVERED;
		}

		it_entry->ie_epoch     = oiter->it_epr.epr_hi;
		it_entry->ie_punch     = oiter->it_ilog_info.ii_next_punch;
		it_entry->ie_obj_punch = oiter->it_obj->obj_ilog_info.ii_next_punch;
		/** Use the dkey for this */
		vos_ilog_last_update(&oiter->it_dkey_krec->kr_ilog, VOS_TS_TYPE_DKEY,
				     &it_entry->ie_last_update, !!oiter->it_iter.it_for_sysdb);
		d_iov_set(&it_entry->ie_key, &oiter->it_fake_akey, sizeof(oiter->it_fake_akey));

		return 0;
	}

	return key_iter_fetch(oiter, it_entry, anchor, false, 0);
}

static int
vos_obj_sv_iter_fetch(struct vos_iterator *iter, vos_iter_entry_t *it_entry, daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_SINGLE, "type is %d\n", iter->it_type);
	return singv_iter_fetch(oiter, it_entry, anchor);
}

static int
vos_obj_ev_iter_fetch(struct vos_iterator *iter, vos_iter_entry_t *it_entry, daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	D_ASSERTF(iter->it_type == VOS_ITER_RECX, "type is %d\n", iter->it_type);
	return recx_iter_fetch(oiter, it_entry, anchor);
}

static int
vos_obj_iter_copy(struct vos_iterator *iter, vos_iter_entry_t *it_entry,
		  d_iov_t *iov_out)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
		return key_iter_copy(oiter, it_entry, iov_out);
	case VOS_ITER_SINGLE:
	case VOS_ITER_RECX:
		return recx_iter_copy(oiter, it_entry, iov_out);
	default:
		D_ASSERT(0);
		return -DER_INVAL;
	}
}

static int
obj_iter_delete(struct vos_obj_iter *oiter, void *args)
{
	struct umem_instance	*umm;
	int			 rc = 0;

	umm = vos_obj2umm(oiter->it_obj);

	rc = umem_tx_begin(umm, NULL);
	if (rc != 0)
		goto exit;

	rc = dbtree_iter_delete(oiter->it_hdl, args);

	rc = umem_tx_end(umm, rc);
exit:
	if (rc != 0)
		DL_CDEBUG(rc == -DER_TX_BUSY, DB_TRACE, DLOG_ERR, rc,
			  "Failed to delete iter entry");
	return rc;
}

/*
 * For a single value btree, grab the value of the current iter cursor, and use
 * the offset to get the durable format (pmem) structure with the bio_addr. Then
 * set it to corrupt.
 */
static int
sv_iter_corrupt(struct vos_obj_iter *oiter)
{
	struct umem_instance	*umm;
	struct vos_svt_key	 skey = {0};
	struct vos_rec_bundle	 rbund = {0};
	struct bio_iov		 biov = {0};
	daos_anchor_t		 anchor = {0};
	d_iov_t			 key, val;
	size_t			 addr_offset;
	struct vos_irec_df	*irec;
	int			 rc = 0;

	umm = vos_obj2umm(oiter->it_obj);

	rc = umem_tx_begin(umm, NULL);
	if (rc != 0)
		return rc;

	/* Bundle the key and value structures into appropriate iovs */
	tree_rec_bundle2iov(&rbund, &val);
	rbund.rb_biov = &biov;
	d_iov_set(&key, &skey, sizeof(skey));

	/* Fetch the key/value for the current iter cursor */
	rc = dbtree_iter_fetch(oiter->it_hdl, &key, &val, &anchor);
	if (rc != 0) {
		D_ERROR("dbtree_iter_fetch failed: "DF_RC"\n", DP_RC(rc));
		rc = umem_tx_end(umm, rc);
		return rc;
	}

	addr_offset = offsetof(struct vos_irec_df, ir_ex_addr);
	rc = umem_tx_add(umm, rbund.rb_off + addr_offset,
			 sizeof(*irec) - addr_offset);
	if (rc != 0) {
		D_ERROR("umem_tx_add failed: "DF_RC"\n", DP_RC(rc));
		rc = umem_tx_end(umm, rc);
		return rc;
	}

	D_DEBUG(DB_IO, "Setting record bio_addr flag to corrupted\n");
	irec = umem_off2ptr(umm, rbund.rb_off);
	BIO_ADDR_SET_CORRUPTED(&irec->ir_ex_addr);

	rc = umem_tx_end(umm, rc);
	return rc;
}

int
vos_obj_iter_check_punch(daos_handle_t ih)
{
	struct vos_iterator	*iter = vos_hdl2iter(ih);
	struct vos_obj_iter	*oiter = vos_iter2oiter(iter);
	struct umem_instance	*umm;
	struct vos_krec_df	*krec;
	struct vos_object	*obj;
	daos_key_t		 key;
	struct vos_rec_bundle	 rbund;
	int			 rc;

	D_ASSERTF(iter->it_type == VOS_ITER_AKEY ||
		  iter->it_type == VOS_ITER_DKEY,
		  "Punch check support only for keys, not values\n");

	rc = key_iter_fetch_helper(oiter, &rbund, &key, NULL);
	D_ASSERTF(rc != -DER_NONEXIST,
		  "Iterator should probe before aggregation\n");
	if (rc != 0)
		return rc;

	obj = oiter->it_obj;
	krec = rbund.rb_krec;
	umm = vos_obj2umm(oiter->it_obj);

	if (!vos_ilog_is_punched(vos_cont2hdl(obj->obj_cont), &krec->kr_ilog, &oiter->it_epr,
				 &oiter->it_punched, &oiter->it_ilog_info))
		return 0;

	/** Ok, ilog is fully punched, so we can move it to gc heap */
	rc = umem_tx_begin(umm, NULL);
	if (rc != 0)
		goto exit;

	/* Incarnation log is empty, delete the object */
	D_DEBUG(DB_IO, "Moving %s to gc heap\n",
		iter->it_type == VOS_ITER_DKEY ? "dkey" : "akey");

	rc = dbtree_iter_delete(oiter->it_hdl, obj->obj_cont);
	D_ASSERT(rc != -DER_NONEXIST);

	rc = umem_tx_end(umm, rc);
exit:
	if (rc == 0)
		return 1;

	return rc;
}
int
vos_obj_iter_aggregate(daos_handle_t ih, bool range_discard)
{
	struct vos_iterator	*iter = vos_hdl2iter(ih);
	struct vos_obj_iter	*oiter = vos_iter2oiter(iter);
	struct umem_instance	*umm;
	struct vos_krec_df	*krec;
	struct vos_object	*obj;
	daos_key_t		 key;
	struct vos_rec_bundle	 rbund;
	bool			 delete = false, invisible = false;
	int			 rc;

	D_ASSERTF(iter->it_type == VOS_ITER_AKEY ||
		  iter->it_type == VOS_ITER_DKEY,
		  "Aggregation only supported on keys\n");

	if (is_fake_iter(oiter))
		return 0; /** Defer this decision to the dkey iterator */

	rc = key_iter_fetch_helper(oiter, &rbund, &key, NULL);
	D_ASSERTF(rc != -DER_NONEXIST,
		  "Iterator should probe before aggregation\n");
	if (rc != 0)
		return rc;

	obj = oiter->it_obj;
	krec = rbund.rb_krec;
	umm = vos_obj2umm(oiter->it_obj);

	rc = umem_tx_begin(umm, NULL);
	if (rc != 0)
		goto exit;

	rc = vos_ilog_aggregate(vos_cont2hdl(obj->obj_cont), &krec->kr_ilog,
				&oiter->it_epr, iter->it_for_discard, false,
				&oiter->it_punched, &oiter->it_ilog_info);

	if (rc == 1) {
		/* Incarnation log is empty so delete the key */
		delete = true;
		D_DEBUG(DB_IO, "Removing %s from tree\n",
			iter->it_type == VOS_ITER_DKEY ? "dkey" : "akey");

		/* XXX: The value tree may be not empty because related prepared transaction can
		 *	be aborted. Then it will be added and handled via GC when ktr_rec_free().
		 */

		rc = dbtree_iter_delete(oiter->it_hdl, obj->obj_cont);
		D_ASSERT(rc != -DER_NONEXIST);
	} else if (rc == -DER_NONEXIST) {
		/* Key no longer exists at epoch but isn't empty */
		invisible = true;
		rc = 0;
	}

	rc = umem_tx_end(umm, rc);

exit:
	if (rc == 0 && (delete || invisible))
		return delete ? 1 : 2;

	return rc;
}

static int
vos_obj_iter_process(struct vos_iterator *iter, vos_iter_proc_op_t op,
		     void *args)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (op) {
	case VOS_ITER_PROC_OP_DELETE:
		switch (iter->it_type) {
		default:
			D_ASSERT(0);
			return -DER_INVAL;
		case VOS_ITER_DKEY:
		case VOS_ITER_AKEY:
			if (is_fake_iter(oiter))
				return 0;
		case VOS_ITER_SINGLE:
			return obj_iter_delete(oiter, args);
		case VOS_ITER_RECX:
			return evt_iter_delete(oiter->it_hdl, NULL);
		}
	case VOS_ITER_PROC_OP_MARK_CORRUPT:
		if (iter->it_type == VOS_ITER_SINGLE)
			return sv_iter_corrupt(oiter);
		if (iter->it_type == VOS_ITER_RECX)
			return evt_iter_corrupt(oiter->it_hdl);
		break;
	default:
		D_ASSERT(0);
	}
	return 0;
}

static int
vos_obj_iter_empty(struct vos_iterator *iter)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);
	bool                 evt   = false;

	if (daos_handle_is_inval(oiter->it_hdl))
		return -DER_NO_HDL;

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;
	case VOS_ITER_DKEY:
		/* fall through */
	case VOS_ITER_AKEY:
		if (oiter->it_flags & VOS_IT_DKEY_EV)
			evt = true;
		/* fall through */
	case VOS_ITER_SINGLE:
		if (!evt)
			return dbtree_iter_empty(oiter->it_hdl);
		/* fall through */
	case VOS_ITER_RECX:
		return evt_iter_empty(oiter->it_hdl);
	}
}

struct vos_iter_ops vos_obj_dkey_iter_ops = {
    .iop_prepare           = vos_obj_iter_prep,
    .iop_nested_tree_fetch = vos_obj_dkey_iter_nested_tree_fetch,
    .iop_nested_prepare    = vos_obj_dkey_iter_nested_prep,
    .iop_finish            = vos_obj_iter_fini,
    .iop_probe             = vos_obj_dkey_iter_probe,
    .iop_next              = vos_obj_dkey_iter_next,
    .iop_fetch             = vos_obj_dkey_iter_fetch,
    .iop_copy              = vos_obj_iter_copy,
    .iop_process           = vos_obj_iter_process,
    .iop_empty             = vos_obj_iter_empty,
};

struct vos_iter_ops vos_obj_akey_iter_ops = {
    .iop_prepare           = vos_obj_iter_prep,
    .iop_nested_tree_fetch = vos_obj_akey_iter_nested_tree_fetch,
    .iop_nested_prepare    = vos_obj_akey_iter_nested_prep,
    .iop_finish            = vos_obj_iter_fini,
    .iop_probe             = vos_obj_akey_iter_probe,
    .iop_next              = vos_obj_akey_iter_next,
    .iop_fetch             = vos_obj_akey_iter_fetch,
    .iop_copy              = vos_obj_iter_copy,
    .iop_process           = vos_obj_iter_process,
    .iop_empty             = vos_obj_iter_empty,
};

struct vos_iter_ops vos_obj_sv_iter_ops = {
    .iop_prepare           = vos_obj_iter_prep,
    .iop_nested_tree_fetch = vos_obj_invalid_iter_nested_tree_fetch,
    .iop_nested_prepare    = vos_obj_iter_sv_nested_prep,
    .iop_finish            = vos_obj_iter_fini,
    .iop_probe             = vos_obj_sv_iter_probe,
    .iop_next              = vos_obj_sv_iter_next,
    .iop_fetch             = vos_obj_sv_iter_fetch,
    .iop_copy              = vos_obj_iter_copy,
    .iop_process           = vos_obj_iter_process,
    .iop_empty             = vos_obj_iter_empty,
};

struct vos_iter_ops vos_obj_ev_iter_ops = {
    .iop_prepare           = vos_obj_iter_prep,
    .iop_nested_tree_fetch = vos_obj_invalid_iter_nested_tree_fetch,
    .iop_nested_prepare    = vos_obj_ev_iter_nested_prep,
    .iop_finish            = vos_obj_iter_fini,
    .iop_probe             = vos_obj_ev_iter_probe,
    .iop_next              = vos_obj_ev_iter_next,
    .iop_fetch             = vos_obj_ev_iter_fetch,
    .iop_copy              = vos_obj_iter_copy,
    .iop_process           = vos_obj_iter_process,
    .iop_empty             = vos_obj_iter_empty,
};
/**
 * @} vos_obj_iters
 */
