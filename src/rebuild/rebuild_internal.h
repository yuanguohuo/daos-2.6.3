/**
 * (C) Copyright 2017-2024 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * rebuild: rebuild internal.h
 *
 */

#ifndef __REBUILD_INTERNAL_H__
#define __REBUILD_INTERNAL_H__

#include <stdint.h>
#include <abt.h>
#include <uuid/uuid.h>
#include <daos/rpc.h>
#include <daos/btree.h>
#include <daos/pool_map.h>
#include <daos_srv/daos_engine.h>
#include <daos_srv/rebuild.h>

/* Track the pool rebuild status on each target, which exists on
 * all server targets. Then each target will report its rebuild
 * status to the global pool tracker(see below) on the master node,
 * which is used to track the rebuild status globally.
 */
//Yuanguo:
//  per pool, per rebuild operation, 一个 daos_engine 一个 rebuild_tgt_pool_tracker 实例;
//  挂在 daos_engine 级别的全局链表 rebuild_gst.rg_tgt_tracker_list 上；
struct rebuild_tgt_pool_tracker {
	/** pin the pool during the rebuild */
	struct ds_pool		*rt_pool;
	struct sched_request	*rt_ult;

	/** the current version being rebuilt, only used by leader */
	//Yuanguo: rebuild 的目标版本；
	//  rebuild 就是让所有 object 在这个版本的 pool map 下，都放置在正确位置上。
	//          struct rebuild_tgt_pool_tracker  的 rt_rebuild_ver
	//          struct rebuild_scan_in 的 rsi_rebuild_ver
	//          struct rebuild_global_pool_tracker 的 rgt_rebuild_ver
	//          struct rebuild_task 的 dst_map_ver
	//          ds_rebuild_schedule 函数的 uint32_t map_ver 参数
	//  都是这个语义
	uint32_t		rt_rebuild_ver;

	/* rebuild generation, will increase for each re-schedule */
	uint32_t		rt_rebuild_gen;

	/** the current rebuild operation */
	daos_rebuild_opc_t	rt_rebuild_op;

	/** rebuild pool/container hdl uuid */
	uuid_t			rt_poh_uuid;
	uuid_t			rt_coh_uuid;

	/* Link it to the rebuild_global tracker_list */
	d_list_t		rt_list;
	ABT_mutex		rt_lock;
	uuid_t			rt_pool_uuid;
	/* to be rebuilt tree */
	struct btr_root		rt_tobe_rb_root;
	daos_handle_t		rt_tobe_rb_root_hdl;
	/* already rebuilt tree, only used for initiator */
	struct btr_root		rt_rebuilt_root;
	daos_handle_t		rt_rebuilt_root_hdl;
	/* number of obj records in rebuilt tree */
	unsigned int		rt_rebuilt_obj_cnt;
	d_rank_list_t		*rt_svc_list;
	d_rank_t		rt_rank;
	int			rt_errno;
	int			rt_refcount;
	uint32_t		rt_tgts_num;
	uint64_t		rt_leader_term;
	/* Wait for other to release the rpt, so the target
	 * can be go ahead to finish the rebuild.
	 */
	ABT_cond		rt_fini_cond;

	/* # to-be-rebuilt objs */
	uint64_t		rt_reported_toberb_objs;
	/* reported # rebuilt objs */
	uint64_t		rt_reported_obj_cnt;
	uint64_t		rt_reported_rec_cnt;
	uint64_t		rt_reported_size;
	/* global stable epoch to use for rebuilding the data */
	uint64_t		rt_stable_epoch;

	/* Only used by reclaim job to discard those half-rebuild data */
	uint64_t		rt_reclaim_epoch;
	/* local rebuild epoch mainly to constrain the VOS aggregation
	 * to make sure aggregation will not cross the epoch
	 */
	uint64_t		rt_rebuild_fence;

	uint32_t		rt_leader_rank;

	/* Global dtx resync version */
	uint32_t		rt_global_dtx_resync_version;

	ABT_cond		rt_global_dtx_wait_cond;
	/* new layout version for upgrade rebuild */
	uint32_t		rt_new_layout_ver;

	unsigned int		rt_lead_puller_running:1,
				rt_abort:1,
				/* re-report #rebuilt cnt per master change */
				rt_re_report:1,
				rt_finishing:1,
				rt_scan_done:1,
				rt_global_scan_done:1,
				rt_global_done:1;
};

struct rebuild_server_status {
	d_rank_t	rank;
	uint32_t	dtx_resync_version;
	uint32_t	scan_done:1,
			pull_done:1;
};

/* Track the rebuild status globally */
struct rebuild_global_pool_tracker {
	/* rebuild status */
	struct daos_rebuild_status	rgt_status;

	struct sched_request	*rgt_ult;
	/* link to rebuild_global.rg_global_tracker_list */
	d_list_t	rgt_list;

	/* the pool uuid */
	uuid_t		rgt_pool_uuid;

	/** rebuild status for each server */
	struct rebuild_server_status *rgt_servers;

	/** the current version being rebuilt */
	//Yuanguo: rebuild 的目标版本；
	//  rebuild 就是让所有 object 在这个版本的 pool map 下，都放置在正确位置上。
	//          struct rebuild_tgt_pool_tracker  的 rt_rebuild_ver
	//          struct rebuild_scan_in 的 rsi_rebuild_ver
	//          struct rebuild_global_pool_tracker 的 rgt_rebuild_ver
	//          struct rebuild_task 的 dst_map_ver
	//          ds_rebuild_schedule 函数的 uint32_t map_ver 参数
	//  都是这个语义
	uint32_t	rgt_rebuild_ver;

	/** global resync dtx version */
	uint32_t	rgt_dtx_resync_version;

	/** number of rgt_server_status */
	uint32_t	rgt_servers_number;

	uint32_t	rgt_rebuild_gen;

	/* The term of the current rebuild leader */
	uint64_t	rgt_leader_term;

	uint64_t	rgt_time_start;

	/* Stable epoch of the rebuild, the minimum epoch from
	 * all rebuilding targets
	 */
	//Yuanguo:
	//  核心作用： stable_epoch 是 rebuild 的数据一致性分界线——
	//    epoch ≤ stable_epoch 的数据：属于"旧世界"，按旧 pool map 布局存储，需要扫描并迁移到新布局
	//    epoch > stable_epoch 的数据：属于"新世界"，client 已经按新 pool map 路由写入，不需要迁移
	//
	//  - rgt_stable_epoch 如何设置？
	//    Step-1: Leader 调用 rebuild_scan_broadcast，广播 scan RPC (REBUILD_OBJECTS_SCAN) 到所有 target
	//
	//    Step-2: 各target 在 scan handler (rebuild_tgt_scan_handler) 中设置本地 HLC 时间戳，作为回复
	//            ro->rso_stable_epoch = d_hlc_get();
	//
	//    Step-3: Leader 在 CaRT aggregator 取所有 target 返回值的最大值
	//            rebuild_tgt_scan_aggregator():
	//                   if (src->rso_status == 0 &&
	//                       dst->rso_stable_epoch < src->rso_stable_epoch)
	//                     dst->rso_stable_epoch = src->rso_stable_epoch;        // 取最大值
	//
	//    Step-4: Leader 在 rebuild_scan_broadcast 中取聚合得到的最大值
	//            rgt->rgt_stable_epoch = rso->rso_stable_epoch;
	//
	//    Step-5: Leader 在 rebuild_leader_status_check 中，通过 IV 周期性广播到各 target
	//            rebuild_leader_status_notify():
	//                   iv.riv_stable_epoch = rgt->rgt_stable_epoch;
	//
	//    Step-6: 各 target 收到 IV refresh 后更新本地
	//            rebuild_iv_ent_refresh():
	//                rpt->rt_stable_epoch = dst_iv->riv_stable_epoch;
	//
	//  - 为什么取各 target HLC 的最大值？
	//    确保 stable_epoch 大于所有 target 在 scan 启动时刻的 HLC，
	//    这样所有 target 在该时间点之前已经提交的数据都会被扫描到。
	//
	//  - rgt_stable_epoch / rt_stable_epoch 如何使用？
	//    误解：各 target 只扫描 stable_epoch 之前写入的数据，然后告诉 pull data 方 (即需要重建数据的 target) 列表，
	//          这样 pull data 方就只能 pull 到 stable_epoch 之前的数据；
	//    实际：
	//        - 各 target 的 scan 阶段不依赖 stable_epoch（待确认，涉及到 分布式mvcc epoch/epoch_bound）：
	//          见 rebuild_container_scan_cb -> dtx_begin；虽然 epoch 使用了 rpt->rt_stable_epoch，但
	//              - 此时可能还未设置（并发、未执行到 Step-6）
	//              - 即使已经设置，scan 也未使用；遍历范围由 param.ip_epr = [0, DAOS_EPOCH_MAX] 控制，不受 dtx_begin 的 epoch 影响；
	//          应该这么理解：epoch.oe_value = rpt->rt_stable_epoch 是给这个扫描 DTX 打上时间戳标记，不控制数据遍历范围
	//
	//        - 各 target 发给 pull data 方 object 列表时，使用 rpt->rt_stable_epoch；见
	//              rebuild_obj_send_cb
	//              -> ds_object_migrate_send(..., max_eph = rpt->rt_stable_epoch, ...)
	//              -> 发送 RPC DAOS_OBJ_RPC_MIGRATE (migrate_in->om_max_eph = max_eph)
	//
	//        - pull data 方 (即需要重建数据的 target):  收到 RPC DAOS_OBJ_RPC_MIGRATE，处理:
	//              ds_obj_migrate_handler
	//              -> ds_migrate_object(..., max_eph = migrate_in->om_max_eph, ...)
	//              -> migrate_pool_tls_lookup_create(..., max_eph = max_eph, ...)
	//              -> migrate_pool_tls_create_one(struct migrate_pool_tls_create_arg* data{max_eph=max_eph})
	//                    存储在 TLS struct migrate_pool_tls 字段 mpt_max_eph 中；
	//          最终使用：
	//              1. migrate_obj_ult()
	//                 epr.epr_hi = tls->mpt_max_eph — fetch 数据的 epoch 上界
	//              2. migrate_punch()
	//                 过滤 dkey/akey/record punch：只迁移 ≤ mpt_max_eph 的 punch
	uint64_t	rgt_stable_epoch;

	/* reclaim epoch of the rebuild, which is used to discard
	 * the half-rebuild data if rebuild fails
	 */
	uint64_t	rgt_reclaim_epoch;

	ABT_mutex	rgt_lock;
	/* The current rebuild is done on the leader */
	ABT_cond	rgt_done_cond;

	uint32_t	rgt_refcount;

	uint32_t	rgt_opc;
	unsigned int	rgt_abort:1,
			rgt_init_scan:1;
};

/* Structure on raft replica nodes to serve completed rebuild status querying */
struct rebuild_status_completed {
	/* rebuild status */
	struct daos_rebuild_status	rsc_status;

	/* link to rebuild_global.rg_completed_list */
	d_list_t			rsc_list;

	/* the pool uuid */
	uuid_t				rsc_pool_uuid;
};

/* Structure on all targets to track all pool rebuilding */
//Yuanguo:
//  注意有两种跟踪信息：
//      - rgt (struct rebuild_global_pool_tracker 类型) ：只有 pool service leader 维护，跟踪 rebuild 全局状态，
//        挂在 rebuild_gst.rg_global_tracker_list 链表上（它是 daos_engine 级别的全局链表）
//      - rpt (struct rebuild_tgt_pool_tracker 类型)    ：每个 rank 都维护，跟踪当前 engine 上的 rebuild 状态，
//        挂在 rebuild_gst.rg_tgt_tracker_list 链表上（它是 daos_engine 级别的全局链表）
//  此外 leader 还是维护rebuild task (struct rebuild_task)
//      - 运行中task：挂在 rebuild_gst.rg_running_list 链表上
//      - 排队中task：挂在 rebuild_gst.rg_queue_list 链表上
struct rebuild_global {
	/* Link rebuild_tgt_pool_tracker on all targets.
	 * Can be inserted/deleted by system XS, be lookup by system XS or VOS target main XS.
	 * Be protected by rg_ttl_rwlock -
	 * system XS takes wrlock to insert/delete, VOS TGT XS takes rdlock to lookup,
	 * no lock needed for system XS to lookup.
	 */
	//Yuanguo: daos_engine 级别的全局链表
	//         SYS xstream (xs_id=0)：
	//             - 插入、删除：需加 write-lock;
	//             - 查找：无需加锁，因为没有其它线程(xstream)会并发修改；
	//         VOS TGT xstream：
	//             - 只查找：需加 read-lock;
	d_list_t	rg_tgt_tracker_list;

	/* Linked rebuild_global_pool_tracker on the master node,
	 * empty on other nodes.
	 * Only operated by stream 0, no need lock.
	 */
	//Yuanguo: daos_engine 级别的全局链表
	//  仅 SYS xstream (xs_id=0) 操作，无需加锁；
	d_list_t	rg_global_tracker_list;

	/**
	 * Completed rebuild status list on raft replica nodes,
	 * empty on other nodes.
	 * Only operated by stream 0, no need lock.
	 */
	d_list_t	rg_completed_list;

	/* Rebuild task running list */
	d_list_t	rg_running_list;

	/* Rebuild task queued list, on which tasks to be scheduled
	 * are linked.
	 */
	d_list_t	rg_queue_list;

	/* rwlock to protect rg_tgt_tracker_list */
	//Yuanguo: 见 rg_tgt_tracker_list 的注释；
	ABT_rwlock	rg_ttl_rwlock;

	ABT_mutex	rg_lock;
	ABT_cond	rg_stop_cond;
	/* how many pools is being rebuilt */
	unsigned int	rg_inflight;
	unsigned int	rg_rebuild_running:1,
			rg_abort:1;
};

/* Per target structure to track the rebuild status */
extern struct rebuild_global rebuild_gst;

struct rebuild_task {
	d_list_t			dst_list;
	uuid_t				dst_pool_uuid;
	struct pool_target_id_list	dst_tgts;
	daos_rebuild_opc_t		dst_rebuild_op;

	/* Epoch to use for reclaim job for discarding the data
	 * of half-rebuild/reintegrated job.
	 */
	daos_epoch_t			dst_reclaim_eph;
	uint64_t			dst_schedule_time;
	//Yuanguo: rebuild 的目标版本；
	//  rebuild 就是让所有 object 在这个版本的 pool map 下，都放置在正确位置上。
	//          struct rebuild_tgt_pool_tracker  的 rt_rebuild_ver
	//          struct rebuild_scan_in 的 rsi_rebuild_ver
	//          struct rebuild_global_pool_tracker 的 rgt_rebuild_ver
	//          struct rebuild_task 的 dst_map_ver
	//          ds_rebuild_schedule 函数的 uint32_t map_ver 参数
	//  都是这个语义
	uint32_t			dst_map_ver;
	uint32_t			dst_new_layout_version;
	/* Once the task fail, it will use reclaim version to
	 * reclaim those half-rebuild/reintegrated job.
	 */
	uint32_t			dst_reclaim_ver;
};

/* Per pool structure in TLS to check pool rebuild status
 * per xstream.
 */
struct rebuild_pool_tls {
	uuid_t		rebuild_pool_uuid;
	daos_handle_t	rebuild_tree_hdl; /*hold objects being rebuilt */
	d_list_t	rebuild_pool_list;
	uint64_t	rebuild_pool_obj_count;
	uint64_t	rebuild_pool_reclaim_obj_count;
	unsigned int	rebuild_pool_ver;
	uint32_t	rebuild_pool_gen;
	uint64_t	rebuild_pool_leader_term;
	int		rebuild_pool_status;
	unsigned int	rebuild_pool_scanning:1,
			rebuild_pool_scan_done:1;
};

/* per thread structure to track rebuild status for all pools */
struct rebuild_tls {
	/* rebuild_pool_tls will link here */
	d_list_t	rebuild_pool_list;
};

struct rebuild_root {
	struct btr_root	btr_root;
	daos_handle_t	root_hdl;
	unsigned int	count;
};

struct rebuild_tgt_query_info {
	int scanning;
	int status;
	uint64_t obj_count;
	uint64_t tobe_obj_count;
	uint64_t rec_count;
	uint64_t size;
	bool rebuilding;
	ABT_mutex lock;
};

struct rebuild_iv {
	uuid_t		riv_pool_uuid;
	uint64_t	riv_toberb_obj_count;
	uint64_t	riv_obj_count;
	uint64_t	riv_rec_count;
	uint64_t	riv_size;
	uint64_t	riv_leader_term;
	uint64_t	riv_stable_epoch;
	uint32_t	riv_seconds;
	uint32_t	riv_dtx_resyc_version;
	uint32_t	riv_global_dtx_resyc_version;
	unsigned int	riv_rank;
	unsigned int	riv_master_rank;
	unsigned int	riv_ver;
	unsigned int	riv_rebuild_gen;
	uint32_t	riv_global_done:1,
			riv_global_scan_done:1,
			riv_scan_done:1,
			riv_pull_done:1,
			riv_sync:1;
	int		riv_status;

};

#define SCAN_YIELD_FREQ		4096
#define SCAN_OBJ_YIELD_CNT	128

extern struct dss_module_key rebuild_module_key;
static inline struct rebuild_tls *
rebuild_tls_get()
{
	return dss_module_key_get(dss_tls_get(), &rebuild_module_key);
}

void rpt_get(struct rebuild_tgt_pool_tracker *rpt);
void rpt_put(struct rebuild_tgt_pool_tracker *rpt);
void rpt_delete(struct rebuild_tgt_pool_tracker *rpt);

struct rebuild_pool_tls *
rebuild_pool_tls_lookup(uuid_t pool_uuid, unsigned int ver, uint32_t gen);

struct pool_map *rebuild_pool_map_get(struct ds_pool *pool);
void rebuild_pool_map_put(struct pool_map *map);

void rebuild_tgt_scan_handler(crt_rpc_t *rpc);
int rebuild_tgt_scan_aggregator(crt_rpc_t *source, crt_rpc_t *result,
				void *priv);
int rebuild_tgt_scan_pre_forward(crt_rpc_t *rpc, void *arg);

int rebuild_iv_fetch(void *ns, struct rebuild_iv *rebuild_iv);
int rebuild_iv_update(void *ns, struct rebuild_iv *rebuild_iv,
		      unsigned int shortcut, unsigned int sync_mode,
		      bool retry);
int rebuild_iv_ns_create(struct ds_pool *pool, uint32_t map_ver,
			 d_rank_list_t *exclude_tgts,
			 unsigned int master_rank);

int rebuild_iv_init(void);
int rebuild_iv_fini(void);

void
rebuild_tgt_status_check_ult(void *arg);

int
rebuild_tgt_prepare(crt_rpc_t *rpc, struct rebuild_tgt_pool_tracker **p_rpt);

bool
rebuild_status_match(struct rebuild_tgt_pool_tracker *rpt,
		enum pool_comp_state states);

bool
is_rebuild_scanning_tgt(struct rebuild_tgt_pool_tracker *rpt);

typedef int (*rebuild_obj_insert_cb_t)(struct rebuild_root *cont_root,
				       uuid_t co_uuid, daos_unit_oid_t oid,
				       daos_epoch_t epoch, unsigned int shard,
				       unsigned int tgt_idx, unsigned int *cnt,
				       int ref);
int
rebuild_obj_insert_cb(struct rebuild_root *cont_root, uuid_t co_uuid,
		      daos_unit_oid_t oid, daos_epoch_t eph, unsigned int shard,
		      unsigned int tgt_idx, unsigned int *cnt, int ref);

int
rebuild_cont_obj_insert(daos_handle_t toh, uuid_t co_uuid, daos_unit_oid_t oid,
			daos_epoch_t epoch, unsigned int shard,
			unsigned int tgt_idx, unsigned int *cnt, int ref,
			rebuild_obj_insert_cb_t obj_cb);
int
rebuilt_btr_destroy(daos_handle_t btr_hdl);

struct rebuild_tgt_pool_tracker *
rpt_lookup(uuid_t pool_uuid, uint32_t opc, unsigned int ver, unsigned int gen);

void
rgt_get(struct rebuild_global_pool_tracker *rgt);

void
rgt_put(struct rebuild_global_pool_tracker *rgt);

struct rebuild_global_pool_tracker *
rebuild_global_pool_tracker_lookup(const uuid_t pool_uuid, unsigned int ver, unsigned int gen);

int
rebuild_global_status_update(struct rebuild_global_pool_tracker *master_rpt,
			     struct rebuild_iv *iv);

int
rebuild_notify_ras_start(uuid_t *pool, uint32_t map_ver, char *op_str);

int
rebuild_notify_ras_end(uuid_t *pool, uint32_t map_ver, char *op_str, int op_rc);

void rebuild_leader_stop(const uuid_t pool_uuid, unsigned int version,
			 uint32_t rebuild_gen, uint64_t term);
int
rebuild_obj_tree_destroy(daos_handle_t btr_hdl);
#endif /* __REBUILD_INTERNAL_H_ */
