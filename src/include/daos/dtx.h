/**
 * (C) Copyright 2019-2023 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef __DAOS_DTX_H__
#define __DAOS_DTX_H__

#include <time.h>
#include <uuid/uuid.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>

#include <daos_types.h>
#include <gurt/debug.h>
#include <gurt/common.h>

/* If the count of committable DTXs on leader exceeds this threshold,
 * it will trigger batched DTX commit globally. We will optimize the
 * threshould with considering RPC limitation, PMDK transaction, and
 * CPU schedule efficiency, and so on.
 */
#define DTX_THRESHOLD_COUNT		(1 << 9)

/* The time (in second) threshold for batched DTX commit. */
#define DTX_COMMIT_THRESHOLD_AGE	10

/*
 * VOS aggregation should try to avoid aggregating in the epoch range where
 * lots of data records are pending to commit, so the aggregation epoch upper
 * bound is: current HLC - (DTX batched commit threshold + buffer period)
 *
 * To avoid conflicting of aggregation vs. transactions, any transactional
 * update/fetch with epoch lower than the aggregation upper bound should be
 * rejected and restarted.
 */
#define DAOS_AGG_THRESHOLD	(DTX_COMMIT_THRESHOLD_AGE + 10) /* seconds */

enum dtx_target_flags {
	/* The target only contains read-only operations for the DTX. */
	DTF_RDONLY			= (1 << 0),
};

enum dtx_grp_flags {
	/* The group only contains read-only operations for the DTX. */
	DGF_RDONLY			= (1 << 0),
};

enum dtx_mbs_flags {
	/* The targets being modified via the DTX belong to a replicated
	 * object within single redundancy group.
	 */
	DMF_SRDG_REP = (1 << 0),
	/* The MBS contains the DTX leader information, usually used for
	 * distributed transaction. In old release (before 2.4), for some
	 * stand-alone modification, leader information may be not stored
	 * inside MBS as optimization.
	 */
	DMF_CONTAIN_LEADER = (1 << 1),
	/* The dtx_memberships::dm_tgts is sorted against target ID. Obsolete. */
	DMF_SORTED_TGT_ID = (1 << 2),
	/* The dtx_memberships::dm_tgts is sorted against shard index.
	 * For most of cases, shard index matches the shard ID. But during
	 * shard migration, there may be some temporary shards in related
	 * object layout. Under such case, related shard ID is not unique
	 * in the object layout, but the shard index is unique. So we use
	 * shard index to sort the dtx_memberships::dm_tgts. Obsolete.
	 */
	DMF_SORTED_SAD_IDX = (1 << 3),
	/* The dtx target information are organized as dtx_coll_target. */
	DMF_COLL_TARGET = (1 << 4),
	/*
	 * The range for the ranks [min, max] on which some object shards reside.
	 * It is usually used for collective DTX and appended after the bitmap in
	 * the MBS data.
	 */
	DMF_RANK_RANGE = (1 << 5),
};

/**
 * The daos target that participates in the DTX.
 */
struct dtx_daos_target {
	/* Globally target ID, corresponding to pool_component::co_id. */
	uint32_t			ddt_id;
	union {
		/* For distributed transaction, see dtx_target_flags. */
		uint32_t		ddt_flags;
		uint32_t		ddt_padding;
	};
};

/**
 * The items (replica or EC shard) belong to the same redundancy group make
 * up a modification group that is subset of related DAOS redundancy group.
 *
 * These information will be used for DTX recovery as following:
 *
 * During DTX recovery, for a non-committed DTX, its new leader queries with
 * other alive participants for such DTX status. If all alive ones reply the
 * new leader with 'prepared', then before making the decision to commit the
 * DTX, we need to handle some corner cases:
 *
 * Some corrupted DTX participant may have ever refused (because of conflict
 * with other DTX that may be committed or may be not) related modification.
 * But it did not reply to the old leader before its corruption, or did but
 * the old leader crashed before abort such DTX. If the case happened on all
 * members in some modification group, then when DTX recovery, nobody knows
 * there have ever been DTX conflict. Under such case, the new leader should
 * NOT commit such DTX, otherwise, it will break DAOS transaction semantics
 * for other conflict DTXs. On the other hand, abort such DTX is also NOT a
 * safe solution, because it is possible that the corrupted DTX participant
 * may have committed such DTX before its (and the old leader) corruption.
 *
 * So once we detect some group corruption or lost during the DTX recovery,
 * we can neither commit nor abort related DTX to avoid further damage the
 * system. Instead, we can mark it with some flags and introduce more human
 * knowledge to recover it sometime later.
 */
struct dtx_redundancy_group {
	/* How many touched shards in this group. */
	uint32_t			drg_tgt_cnt;

	/* The degree of redundancy. For EC based group, it is equal to the
	 * count of parity nodes + 1. For replicated one, it is the same as
	 * the drg_tgt_cnt.
	 *
	 * If all the shards 'drg_ids[0 - drg_redundancy - 1]' are lost,
	 * then the group is regarded as unavailable.
	 */
	uint16_t			drg_redundancy;

	/* See dtx_grp_flags. */
	uint16_t			drg_flags;

	/* The shards' IDs, corresponding to pool_component::co_id. For the
	 * leader group that is the first in dtx_memberships, 'drg_index[0]'
	 * is for the leader, the other 'drg_index[1 - drg_redundancy - 1]'
	 * are the leader candidates for DTX recovery.
	 */
	uint32_t			drg_ids[0];
};

/*
 * How many targets are recorded in dtx_memberships::dm_tgts for collective DTX. The first one is
 * current leader, the others are for new leader candicates in order when leader switched.
 *
 * For most of cases, when DTX leader switch happens, DTX resync will commit or abort related DTX.
 * After that, related DTX dtx_memberships will become useless any longer and discarded. So unless
 * the new leader is dead and excluded during current DTX resync, one new leader candidate will be
 * enough. We record three new leader candidates, that can resolve the leader election trouble for
 * twice when leader switch during DTX resync.
 */
#define DTX_COLL_INLINE_TARGETS		4

/**
 * A collective transaction may contains a lot of participants. If we store all of them one by one
 * in the dtx_memberships (MBS) structure, then the MBS body will be very large. Transferring such
 * large MBS on network is inconvenient and may have to via RDAM instead of directly packed inside
 * related RPC body.
 *
 * To avoid such bad situation, collective DTX will use dtx_coll_target. Instead of recording all
 * the DTX participants information in MBS, the dtx_coll_target will record the targets reside on
 * current engine, that can be used for local DTX operation (commit, abort, check).
 *
 * Please note that collective DTX only can be used for single object based stand alone operation.
 * If current user is the collective DTX leader, and wants to operate the collective DTX on other
 * DAOS engines, then it needs to re-calculate related participants based on related object layout.
 * For most of commit/abort cases, the collective DTX leader has already prepared the paraticipants
 * information in DRAM before starting the DTX, it is unnecessary to re-calculate the paraticipants.
 * The re-calculation DTX paraticipants will happen when resync or cleanup the collective DTX. Such
 * two cases are relative rare, so even if the overhead for such re-calculation would be quite high,
 * it will not affect the whole system too much.
 *
 * On the other hand, DTX refresh is frequently used DTX logic. Efficiently find out the DTX leader
 * is crucial for that. Consider DTX leader switch, we will record several new leader candidates in
 * the MBS in front of the collective targets information. Then for most of cases, DTX refresh does
 * not need to re-calculation DTX paraticipants.
 */
struct dtx_coll_target {
	/* Fault domain level - used for generating related object layout. */
	uint32_t			dct_fdom_lvl;
	/* Performance domain affinity - used for generating related object layout. */
	uint32_t			dct_pda;
	/* Performance domain level - used for generating related object layout. */
	uint32_t			dct_pdom_lvl;
	/* The object layout version - used for generating related object layout. */
	uint16_t			dct_layout_ver;
	/* How many shards on current engine that participant in the collective DTX. */
	uint8_t				dct_tgt_nr;
	/* The size of dct_bitmap. */
	uint8_t				dct_bitmap_sz;
	/*
	 * The ID (pool_component::co_id) array for targets on current engine, used for DTX check.
	 * The bitmap for local object shards on current engine is appended after the ID array. The
	 * bitmap is used for DTX commit and abort. In fact, we can re-calculate such bitmap based
	 * on the taregets ID, but directly store the bitmap is more efficient since it is not big.
	 */
	uint32_t			dct_tgts[0];
};

struct dtx_memberships {
	/* How many touched shards in the DTX. */
	uint32_t			dm_tgt_cnt;

	/* How many modification groups in the DTX. For standalone modification,
	 * be as optimization, we will not store modification group information
	 * inside 'dm_data'. Similarly for the distributed transaction that all
	 * the touched targets are in the same redundancy group.
	 */
	uint32_t			dm_grp_cnt;

	/* sizeof(dm_data). */
	uint32_t			dm_data_size;

	/* see dtx_mbs_flags. */
	uint16_t			dm_flags;

	union {
		/* DTX entry flags during DTX recovery. */
		uint16_t		dm_dte_flags;
		/* For alignment. */
		uint16_t		dm_padding;
	};

	/* The first 'sizeof(struct dtx_daos_target) * dm_tgt_cnt' is the
	 * dtx_daos_target array. The subsequent can be redundancy groups
	 * or dtx_coll_target, depends on dm_flags.
	 */
	union {
		char			dm_data[0];
		struct dtx_daos_target	dm_tgts[0];
	};
};

/**
 * DAOS two-phase commit transaction identifier,
 * generated by client, globally unique.
 */
struct dtx_id {
	/** The uuid of the transaction */
	uuid_t			dti_uuid;
	/** The HLC timestamp (not epoch) of the transaction */
	uint64_t		dti_hlc;
};

void daos_dti_gen_unique(struct dtx_id *dti);
void daos_dti_gen(struct dtx_id *dti, bool zero);
void daos_dti_reset(void);

static inline void
daos_dti_copy(struct dtx_id *des, const struct dtx_id *src)
{
	if (src != NULL)
		*des = *src;
	else
		memset(des, 0, sizeof(*des));
}

static inline bool
daos_is_zero_dti(const struct dtx_id *dti)
{
	return dti->dti_hlc == 0;
}

static inline bool
daos_dti_equal(struct dtx_id *dti0, struct dtx_id *dti1)
{
	return memcmp(dti0, dti1, sizeof(*dti0)) == 0;
}

static inline uint32_t *
dtx_coll_mbs_rankrange(struct dtx_memberships *mbs)
{
	struct dtx_daos_target *ddt;
	struct dtx_coll_target *dct;
	size_t                  size;

	D_ASSERT(mbs->dm_flags & DMF_COLL_TARGET);
	D_ASSERT(mbs->dm_flags & DMF_RANK_RANGE);

	ddt = &mbs->dm_tgts[0];
	dct = (struct dtx_coll_target *)(ddt + mbs->dm_tgt_cnt);

	size = sizeof(*ddt) * mbs->dm_tgt_cnt + sizeof(*dct) +
	       sizeof(dct->dct_tgts[0]) * dct->dct_tgt_nr + dct->dct_bitmap_sz;
	size = (size + 3) & ~3;

	return (uint32_t *)((void *)ddt + size);
}

#define DF_DTI		DF_UUID"."DF_X64
#define DF_DTIF		DF_UUIDF"."DF_X64
#define DP_DTI(dti)	DP_UUID((dti)->dti_uuid), (dti)->dti_hlc

enum daos_ops_intent {
	DAOS_INTENT_DEFAULT		= 0, /* fetch/enumerate/query */
	DAOS_INTENT_PURGE		= 1, /* purge/aggregation */
	DAOS_INTENT_UPDATE		= 2, /* write/insert */
	DAOS_INTENT_PUNCH		= 3, /* punch/delete */
	DAOS_INTENT_MIGRATION		= 4, /* for migration related scan */
	DAOS_INTENT_CHECK		= 5, /* check aborted or not */
	DAOS_INTENT_KILL		= 6, /* delete object/key */
	DAOS_INTENT_IGNORE_NONCOMMITTED	= 7, /* ignore non-committed DTX. */
	DAOS_INTENT_DISCARD		= 8, /* discard data */
};

/**
 * DAOS two-phase commit transaction status.
 */
enum dtx_status {
	/* DTX is pre-allocated, not prepared yet. */
	DTX_ST_INITED		= 0,
	/** Local participant has done the modification. */
	DTX_ST_PREPARED		= 1,
	/** The DTX has been committed. */
	DTX_ST_COMMITTED	= 2,
	/** The DTX is corrupted, some participant RDG(s) may be lost. */
	DTX_ST_CORRUPTED	= 3,
	/** The DTX is committable, but not committed, non-persistent status. */
	DTX_ST_COMMITTABLE	= 4,
	/** The DTX is aborted. */
	DTX_ST_ABORTED		= 5,
	/** The DTX is in aborting, non-persistent status. */
	DTX_ST_ABORTING		= 6,
	/** The DTX is in committing, non-persistent status. */
	DTX_ST_COMMITTING	= 7,
	/** The DTX is in preparing, non-persistent status. */
	DTX_ST_PREPARING	= 8,
};

enum daos_dtx_alb {
	/* unavailable case */
	ALB_UNAVAILABLE		= 0,
	/* available, no (or not care) pending modification */
	ALB_AVAILABLE_CLEAN	= 1,
	/* available but with dirty modification */
	ALB_AVAILABLE_DIRTY	= 2,
	/* available, aborted or garbage */
	ALB_AVAILABLE_ABORTED	= 3,
};

static inline unsigned int
dtx_alb2state(int alb)
{
	switch (alb) {
	case ALB_UNAVAILABLE:
	case ALB_AVAILABLE_DIRTY:
		return DTX_ST_PREPARED;
	case ALB_AVAILABLE_CLEAN:
		return DTX_ST_COMMITTED;
	case ALB_AVAILABLE_ABORTED:
		return DTX_ST_ABORTED;
	default:
		D_ASSERTF(alb < 0, "Invalid alb:%d\n", alb);
		return alb;
	}
}

enum daos_tx_flags {
	DTF_RETRY_COMMIT	= 1, /* TX commit will be retry. */
};

/** Epoch context of a DTX */
struct dtx_epoch {
	/** epoch */
	daos_epoch_t		oe_value;
	/** first epoch chosen */
	daos_epoch_t		oe_first;
	/** such as DTX_EPOCH_UNCERTAIN, etc. */
	uint32_t		oe_flags;
	union {
		uint32_t	oe_padding;
		/** see 'obj_rpc_flags' when it is transferred on wire. */
		uint32_t	oe_rpc_flags;
	};
};

/* dtx_epoch.oe_flags */
#define DTX_EPOCH_UNCERTAIN	(1U << 0)	/**< oe_value is uncertain */

/** Does \a epoch contain a chosen TX epoch? */
static inline bool
dtx_epoch_chosen(struct dtx_epoch *epoch)
{
	return (epoch->oe_value != 0 && epoch->oe_value != DAOS_EPOCH_MAX);
}

/** Are \a and \b equal? */
static inline bool
dtx_epoch_equal(struct dtx_epoch *a, struct dtx_epoch *b)
{
	return (a->oe_value == b->oe_value && a->oe_first == b->oe_first &&
		a->oe_flags == b->oe_flags);
}

#endif /* __DAOS_DTX_H__ */
