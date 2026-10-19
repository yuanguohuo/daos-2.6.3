/**
 * (C) Copyright 2016-2024 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of daos_sr
 *
 * src/placement/pl_map.h
 */

#ifndef __PL_MAP_H__
#define __PL_MAP_H__

#include <daos/placement.h>
#include <isa-l.h>

struct pl_map_ops;

/**
 * Function table for placement map.
 */
struct pl_map_ops {
	/** create a placement map */
	int (*o_create)(struct pool_map *poolmap,
			struct pl_map_init_attr *mia,
			struct pl_map **mapp);
	/** destroy a placement map */
	void (*o_destroy)(struct pl_map *map);
	/** query placement map attributes */
	int (*o_query)(struct pl_map *map, struct pl_map_attr *attr);
	/** print debug information of a placement map */
	void (*o_print)(struct pl_map *map);

	/** object methods */
	/** see \a pl_map_obj_select and \a pl_map_obj_rebalance */
	int (*o_obj_place)(struct pl_map *map,
			   uint32_t layout_gl_version,
			   struct daos_obj_md *md,
			   unsigned int	mode,
			   struct daos_obj_shard_md *shard_md,
			   struct pl_obj_layout **layout_pp);
	/** see \a pl_map_obj_rebuild */
	int (*o_obj_find_rebuild)(struct pl_map *map,
				  uint32_t layout_gl_version,
				  struct daos_obj_md *md,
				  struct daos_obj_shard_md *shard_md,
				  uint32_t rebuild_ver,
				  uint32_t *tgt_rank,
				  uint32_t *shard_id,
				  unsigned int array_size);
};

unsigned int pl_obj_shard2grp_head(struct daos_obj_shard_md *shard_md,
				   struct daos_oclass_attr *oc_attr);
unsigned int pl_obj_shard2grp_index(struct daos_obj_shard_md *shard_md,
				    struct daos_oclass_attr *oc_attr);

/** Common placement map functions */

/**
 * This struct is used to to hold information while
 * finding rebuild targets for shards located on unavailable
 * targets.
 */
//Yuanguo:
//  - 例如DAOS集群有3个node, 每个node有2个rank(共6个rank)，每个rank有12个target (共72个target);
//  - 一个 OC_RP_3GX object 的 layout 大致如下，由 struct pl_obj_layout 描述：
//      - ol_grp_nr (nr_grps) = 22   因为 72/3 = 24，预留2个group，所以是22；
//      - ol_grp_size         = 3    因为3副本；
//      - ol_nr      = 3 * 22 = 66   表示object最多占用66个target (预留的2个group有6个target)
//      - ol_shards 动态数组，元素个数是 ol_nr = 66
//
//          shard_idx   tgt_id
//           [0]          18  (Group 0, Replica 0)
//           [1]          25  (Group 0, Replica 1)
//           [2]          70  (Group 0, Replica 2)
//
//           [3]          22  (Group 1, Replica 0)
//           [4]          43  (Group 1, Replica 1)
//           [5]          33  (Group 1, Replica 2)
//
//           ...
//
//           [63]         12  (Group 21, Replica 0)
//           [64]         41  (Group 21, Replica 1)
//           [65]         38  (Group 21, Replica 2)
//
//  - 本结构标记"某个 shard 应迁往的目标 target"。注意 fs_tgt_id 存的是【新目标 target】(POST布局落点)，
//    原 target 需查 layout->ol_shards[fs_shard_idx].po_target，不在本结构中。
//    以 shard 0（原 target-18）为例，4 种 rebuild 场景：
//
//      场景          | fs_shard_idx  | fs_tgt_id     | 数据流向                               | 触发条件
//      --------------|-------------- |---------------|----------------------------------------|-----------------
//      故障          | 0             | 29 (spare)    | 同组存活副本(tgt-25,70)  --> spare-29  | 条件 new_tgt≠old_tgt
//      扩容          | 0             | 75 (新target) | 现有UPIN target (tgt-18) --> 新tgt-75  | 条件 new_tgt≠old_tgt
//      reint         | 0             | 18 (原target) | spare                    --> 原tgt-18  | 条件 new_tgt≠old_tgt
//      DOWN2UP-reint | 0             | 18 (原target) | 同组存活副本(tgt-25,70)  --> 原tgt-18  | 条件 po_rebuilding
//
//      即：
//      - failed_shard {fs_shard_idx=0, fs_tgt_id=29} (故障)   : shard 0 从 tgt-18 迁到 spare tgt-29
//      - failed_shard {fs_shard_idx=0, fs_tgt_id=75} (扩容)   : shard 0 从 tgt-18 迁到新 tgt-75
//      - failed_shard {fs_shard_idx=0, fs_tgt_id=18} (reint)  : shard 0 从 spare 搬回原 tgt-18
//      - failed_shard {fs_shard_idx=0, fs_tgt_id=18} (DOWN2UP): shard 0 回填到原 tgt-18 (落点不变)
//
//  - 关键：reint 与 DOWN2UP-reint 的 fs_tgt_id 都是 18(原target)，但触发方式不同：
//      - reint  : PRE布局 old_tgt=spare, POST布局 new_tgt=18 → new≠old，(target 变化)
//      - DOWN2UP: PRE布局 old_tgt=18,    POST布局 new_tgt=18 → new==old，(靠 po_rebuilding 标记)
//
// Yuanguo: struct failed_shard 这个名字有误导性。在 layout_find_diff（reint/rebuild diff）上下文里，它的
//   fs_tgt_id/fs_fseq/fs_rank/fs_status 全部来自新目标 target，而不是"失败的那个 target"。
struct failed_shard {
	d_list_t        fs_list;
	void		*fs_data;
	uint32_t        fs_shard_idx;
	uint32_t        fs_fseq;
	uint32_t        fs_tgt_id;
	uint16_t	fs_rank;
	uint8_t		fs_index;
	uint8_t         fs_status;
	uint32_t	fs_down2up:1;
};

#define	DF_FAILEDSHARD "shard_idx: %d, fseq: %d, tgt_id: %d, status: %d"
#define	DP_FAILEDSHARD(x) (x).fs_shard_idx, (x).fs_fseq, \
			(x).fs_tgt_id, (x).fs_status

/**
 * layout_gen_mode indicate how the layout is generated:
 *
 * PRE_REBUILD means the targets statuses need to be converted to the original
 * status before rebuild, for example UP --> NEW, DOWN --> UPIN, during layout
 * generation.
 *
 * CURRENT means the target status will not change during layout generation.
 *
 * POST_REBUILD means the targets statuses need to be converted to the status
 * after rebuild finished, for example UP --> UPIN, DOWN --> DOWNOUT, during
 * layout generation.
 */
enum layout_gen_mode {
	PRE_REBUILD	= 0,
	CURRENT		= 1,
	POST_REBUILD	= 2,
};

/**
 * This is useful for jump_map placement to pseudorandomly permute input keys
 * that are similar to each other. This dramatically improves the even-ness of
 * the distribution of output placements.
 */
static inline uint64_t
crc(uint64_t data, uint32_t init_val)
{
	return crc64_ecma_refl(init_val, (uint8_t *)&data, sizeof(data));
}

void
remap_add_one(d_list_t *remap_list, struct failed_shard *f_new);
void
reint_add_one(d_list_t *remap_list, struct failed_shard *f_new);

int
remap_alloc_one(d_list_t *remap_list, unsigned int shard_idx,
		struct pool_target *tgt, bool for_reint, void *data);

int
remap_insert_copy_one(d_list_t *remap_list, struct failed_shard *original);

void
remap_list_free_all(d_list_t *remap_list);

void
remap_dump(d_list_t *remap_list, struct daos_obj_md *md,
	   char *comment);

int
op_get_grp_size(unsigned int domain_nr, unsigned int *grp_size,
		daos_obj_id_t oid);

int
remap_list_fill(struct pl_map *map, struct daos_obj_md *md,
		struct daos_obj_shard_md *shard_md, uint32_t rebuild_ver,
		uint32_t *tgt_id, uint32_t *shard_idx,
		unsigned int array_size, int *idx,
		struct pl_obj_layout *layout, d_list_t *remap_list,
		bool fill_addition);

int
determine_valid_spares(struct pool_target *spare_tgt, struct daos_obj_md *md,
		       bool spare_avail, d_list_t *remap_list, uint32_t allow_version,
		       enum layout_gen_mode gen_mode, struct failed_shard *f_shard,
		       struct pl_obj_shard *l_shard, bool *is_extending);

int
spec_place_rank_get(unsigned int *pos, daos_obj_id_t oid,
		    struct pool_map *pl_poolmap);

int
pl_map_extend(struct pl_obj_layout *layout, d_list_t *extended_list);

bool
is_comp_avaible(struct pool_component *comp, uint32_t allow_version,
		enum layout_gen_mode gen_mode);
bool
need_remap_comp(struct pool_component *comp, uint32_t allow_status,
		enum layout_gen_mode gen_mode);

#endif /* __PL_MAP_H__ */
