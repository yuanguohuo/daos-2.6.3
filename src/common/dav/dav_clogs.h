/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2015-2022, Intel Corporation */

/*
 * dav_iface.h -- Interfaces exported by DAOS internal Allocator for VOS (DAV)
 */

#ifndef __DAOS_COMMON_DAV_CLOGS_H
#define __DAOS_COMMON_CLOGS_H 1

#include <stdint.h>
#include <sys/types.h>
#include "ulog.h"

#define LANE_TOTAL_SIZE (3072) /* 3 * 1024 (sum of 3 old lane sections) */
/*
 * We have 3 kilobytes to distribute be split between transactional redo
 * and undo logs.
 * Since by far the most space consuming operations are transactional
 * snapshots, most of the space, 2304 bytes, is assigned to the undo log.
 * After that, the remainder, 640 bytes, or 40 ulog entries, is left for the
 * transactional redo logs.
 * Thanks to this distribution, all small and medium transactions should be
 * entirely performed without allocating any additional metadata.
 *
 * These values must be cacheline size aligned to be used for ulogs. Therefore
 * they are parametrized for the size of the struct ulog changes between
 * platforms.
 */
#define LANE_UNDO_SIZE (LANE_TOTAL_SIZE \
			- LANE_REDO_EXTERNAL_SIZE \
			- 2 * sizeof(struct ulog)) /* 2304 for 64B ulog */
#define LANE_REDO_EXTERNAL_SIZE ALIGN_UP(704 - sizeof(struct ulog), \
					CACHELINE_SIZE) /* 640 for 64B ulog */

struct dav_clogs {
	/*
	 * Redo log for large operations/transactions.
	 * Can be extended by the use of internal ulog.
	 */
	//Yuanguo:
	//  - 事务执行过程中，有两类内存变更：
	//      TypeA. user做的内存变更
	//      TypeB. 非user直接修改的、需要延迟到commit时才执行的内部变化 (分配器bitmap、undo.gen_num等)
	//  - 对于TypeB
	//      - 工作阶段：先记在 struct tx 的 actions 中（延迟执行描述符）
	//          例1: tx_alloc_common
	//                  -> tx_action_add
	//                  -> palloc_reserve
	//          例2: dav_tx_add_common -> dav_tx_add_snapshot 事务第一次 snapshot 一个range 时 (first_snapshot==1)
	//                  -> tx_action_add
	//                  -> palloc_set_value（TYPE_MEM: 递增 undo.gen_num）
	//
	//      - commit阶段，不会整理到本字段 (external) 中，而是会整理到 struct dav_obj 的 external->pshadow_ops 中；
	//          - tx_pre_commit：遍历 tx->ranges，把user做的内存变更直接写入 wt_redo (不经过 external)
	//          - dav_tx_commit -> palloc_publish -> palloc_exec_actions: 处理内部元数据变更
	//               - 对 struct tx 的 actions 的每条 action 生成一条 external entry，记录在 struct dav_obj 的 external->pshadow_ops 中
	//               - operation_process -> operation_process_persistent_redo
	//                   - 对本 struct dav_obj 的 external->pshadow_ops 中的每条entry:
	//                       - tx_create_wal_entry: 转换成 redo log 记录到 wt_redo，至此和user修改内存一致！
	//                       - ulog_process: apply 到内部元数据上（即修改元数据内存状态）；
	//        Yuanguo: 最初我以为是此字段，最后发现不是，而是 struct dav_obj 的 external->pshadow_ops;
	struct ULOG(LANE_REDO_EXTERNAL_SIZE) external;
	/*
	 * Undo log for snapshots done in a transaction.
	 * Can be extended/shrunk by the use of internal ulog.
	 */
	//Yuanguo: 用于 abort 回滚的 undo entries;
	struct ULOG(LANE_UNDO_SIZE) undo;
};

typedef struct dav_obj dav_obj_t;

int dav_create_clogs(dav_obj_t *hdl);
void dav_destroy_clogs(dav_obj_t *hdl);
int dav_hold_clogs(dav_obj_t *hdl);
int dav_release_clogs(dav_obj_t *hdl);

#endif /* __DAOS_COMMON_DAV_CLOGS_H */
