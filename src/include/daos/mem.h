/**
 * (C) Copyright 2016-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of daos
 *
 * src/include/daos/daos_mem.h
 */

#ifndef __DAOS_MEM_H__
#define __DAOS_MEM_H__

#include <daos_types.h>
#include <daos/common.h>

/**
 * Terminologies:
 *
 * pmem		Persistent Memory
 * vmem		Volatile Memory
 *
 * umem		Unified memory abstraction
 * umoff	Unified Memory offset
 */

int umempobj_settings_init(bool md_on_ssd);

/* convert backend type to umem class id */
int umempobj_backend_type2class_id(int backend);

/* umem persistent object property flags */
#define	UMEMPOBJ_ENABLE_STATS	0x1

#ifdef DAOS_PMEM_BUILD
enum {
	DAOS_MD_PMEM	= 0,
	DAOS_MD_BMEM	= 1,
	DAOS_MD_ADMEM	= 2,
};

/* return umem backend type */
int umempobj_get_backend_type(void);

#endif

struct umem_wal_tx;

struct umem_wal_tx_ops {
	/**
	 * Get number of umem_actions in TX redo log.
	 *
	 * \param tx[in]	umem_wal_tx pointer
	 */
	uint32_t	(*wtx_act_nr)(struct umem_wal_tx *tx);

	/**
	 * Get payload size of umem_actions in TX redo log.
	 *
	 * \param tx[in]	umem_wal_tx pointer
	 */
	uint32_t	(*wtx_payload_sz)(struct umem_wal_tx *tx);

	/**
	 * Get the first umem_action in TX redo log.
	 *
	 * \param tx[in]	umem_wal_tx pointer
	 */
	struct umem_action *	(*wtx_act_first)(struct umem_wal_tx *tx);

	/**
	 * Get the next umem_action in TX redo log.
	 *
	 * \param tx[in]	umem_wal_tx pointer
	 */
	struct umem_action *	(*wtx_act_next)(struct umem_wal_tx *tx);
};

#define UTX_PRIV_SIZE	(256)
struct umem_wal_tx {
	struct umem_wal_tx_ops	*utx_ops;
	int			 utx_stage;	/* enum umem_pobj_tx_stage */
	uint64_t		 utx_id;
	/* umem class specific TX data */
	struct {
		char		 utx_space[UTX_PRIV_SIZE];
	}			 utx_private;
};

/** Describing a storage region for I/O */
struct umem_store_region {
	/** start offset of the region */
	daos_off_t	sr_addr;
	/** size of the region */
	daos_size_t	sr_size;
};

/** I/O descriptor, it can include arbitrary number of storage regions */
struct umem_store_iod {
	/* number of regions */
	int				 io_nr;
	/** embedded one for single region case */
	struct umem_store_region	 io_region;
	struct umem_store_region	*io_regions;
};

struct umem_store;

struct umem_store_ops {
	int	(*so_load)(struct umem_store *store, char *start);
	int	(*so_read)(struct umem_store *store, struct umem_store_iod *iod,
			   d_sg_list_t *sgl);
	int	(*so_write)(struct umem_store *store, struct umem_store_iod *iod,
			    d_sg_list_t *sgl);
	int	(*so_flush_prep)(struct umem_store *store, struct umem_store_iod *iod,
				 daos_handle_t *fh);
	int	(*so_flush_copy)(daos_handle_t fh, d_sg_list_t *sgl);
	int (*so_flush_post)(daos_handle_t fh, int err);
	int	(*so_wal_reserv)(struct umem_store *store, uint64_t *id);
	int	(*so_wal_submit)(struct umem_store *store, struct umem_wal_tx *wal_tx,
				 void *data_iod);
	int	(*so_wal_replay)(struct umem_store *store,
				 int (*replay_cb)(uint64_t tx_id, struct umem_action *act,
						  void *data),
				 void *arg);
	/* See bio_wal_id_cmp() */
	int	(*so_wal_id_cmp)(struct umem_store *store, uint64_t id1, uint64_t id2);
};

/** The offset of an object from the base address of the pool */
typedef uint64_t umem_off_t;

//Yuanguo: 只对 BMEM/ADMEM 有意义
struct umem_store {
    //Yuanguo: MD-on-SSD场景下
    //    path = "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-0"  (on tmpfs)
    //    stor_size = path文件的大小 - blob-header-size(1*4k) (注意不是struct dav_phdr)
	/**
	 * Size of the umem storage, excluding blob header which isn't visible to allocator.
	 */
	daos_size_t		 stor_size;
	uint32_t		 stor_blk_size;
	uint32_t		 stor_hdr_blks;
	/** private data passing between layers */
    //Yuanguo: MD-on-SSD场景下，stor_priv 指向 struct bio_meta_context 对象，里面是用于访问
    //  data/meta/wal blob的context (spdk blob id等)；
	void			*stor_priv;
	void			*stor_stats;
	void                    *vos_priv;
	/** Cache for this store */
	struct umem_cache       *cache;
	/**
	 * Callbacks provided by upper level stack, umem allocator uses them to operate
	 * the storage device.
	 */
	struct umem_store_ops	*stor_ops;
	/* backend type */
	int			 store_type;
	/* standalone store */
	bool			 store_standalone;
	/* backend SSD is in faulty state */
	bool			 store_faulty;
};

struct umem_slab_desc {
	size_t		unit_size;
	unsigned	class_id;
};

struct umem_pool {
    //Yuanguo:
    //  - MD-on-PMEM: up_priv指向pmemobj_create返回的memory pool，类型是PMEMobjpool，即指向一个transactional object store；
    //  - MD-on-SSD:  up_priv指向一个struct dav_obj实例，其实是模拟PMEMobjpool;
	void			*up_priv;
	struct umem_store	 up_store;
	/** Slabs of the umem pool */
	struct umem_slab_desc	 up_slabs[0];
};

/* umem persistent object functions */
struct umem_pool *umempobj_create(const char *path, const char *layout_name,
				  int prop_flags, size_t poolsize,
				  mode_t mode, struct umem_store *store);
struct umem_pool *umempobj_open(const char *path, const char *layout_name,
				int prop_flags, struct umem_store *store);
void  umempobj_close(struct umem_pool *pool);
void *umempobj_get_rootptr(struct umem_pool *pool, size_t size);
int   umempobj_get_heapusage(struct umem_pool *pool,
			     daos_size_t *cur_allocated);
void  umempobj_log_fraginfo(struct umem_pool *pool);

/** Number of flag bits to reserve for encoding extra information in
 *  a umem_off_t entry.
 */
#define UMOFF_NUM_FLAG_BITS	(8)
/** The absolute value of a flag mask must be <= this value */
#define UMOFF_MAX_FLAG		(1ULL << UMOFF_NUM_FLAG_BITS)
/** Number of bits to shift the flag bits */
#define UMOFF_FLAG_SHIFT	(63 - UMOFF_NUM_FLAG_BITS)
/** Mask for flag bits */
#define UMOFF_FLAG_MASK		((UMOFF_MAX_FLAG - 1) << UMOFF_FLAG_SHIFT)
/** In theory and offset can be NULL but in practice, pmemobj_root
 *  is not at offset 0 as pmdk reserves some space for its internal
 *  use.   So, use 0 for NULL.   Invalid bits are also considered
 *  NULL.
 */
#define UMOFF_NULL		(0ULL)
/** Check for a NULL value including possible invalid flag bits */
#define UMOFF_IS_NULL(umoff)	(umem_off2offset(umoff) == 0)

/** Retrieves any flags that are set.
 *
 *  \param	offset[IN]	The value from which to get flags
 */
static inline uint64_t
umem_off2flags(umem_off_t umoff)
{
	return (umoff & UMOFF_FLAG_MASK) >> UMOFF_FLAG_SHIFT;
}

/** Retrieves the offset portion of a umem_off_t
 *
 *  \param	offset[IN]	The value from which to get offset
 */
static inline uint64_t
umem_off2offset(umem_off_t umoff)
{
	return umoff & ~UMOFF_FLAG_MASK;
}

/** Set flags on a umem_off_t address
 *  The flags parameter must be < UMOFF_MAX_FLAG.
 *
 *  \param	offset[IN,OUT]	The value is marked NULL with additional flags
 *  \param	flags[IN]	Auxilliarly information about the null entry
 */
static inline void
umem_off_set_flags(umem_off_t *offset, uint64_t flags)
{
	D_ASSERTF(flags < UMOFF_MAX_FLAG,
		  "Attempt to set invalid flag bits on umem_off_t\n");
	*offset = umem_off2offset(*offset) | (flags << UMOFF_FLAG_SHIFT);
}

/** Set a umem_off_t address to NULL and set flags
 *  The flags parameter must be < UMOFF_MAX_FLAG.
 *
 *  \param	offset[IN,OUT]	The value is marked NULL with additional flags
 *  \param	flags[IN]	Auxilliarly information about the null entry
 */
static inline void
umem_off_set_null_flags(umem_off_t *offset, uint64_t flags)
{
	D_ASSERTF(flags < UMOFF_MAX_FLAG,
		  "Attempt to set invalid flag bits on umem_off_t\n");
	*offset = flags << UMOFF_FLAG_SHIFT;
}

/* print format of umoff */
#define UMOFF_PF		DF_X64
#define UMOFF_P(umoff)		umem_off2offset(umoff)

enum umem_pobj_tx_stage {
	UMEM_STAGE_NONE,	/* no transaction in this thread */
	UMEM_STAGE_WORK,	/* transaction in progress */
	UMEM_STAGE_ONCOMMIT,	/* successfully committed */
	UMEM_STAGE_ONABORT,	/* tx_begin failed or transaction aborted */
	UMEM_STAGE_FINALLY,	/* always called */

	MAX_UMEM_TX_STAGE
};

typedef enum {
	/** volatile memory */
  //Yuanguo: 用于测试，非生产可用。
	UMEM_CLASS_VMEM,
	/** persistent memory */
	UMEM_CLASS_PMEM,
	/** persistent memory but ignore PMDK snapshot */
	UMEM_CLASS_PMEM_NO_SNAP,
	/** blob backed memory */
	UMEM_CLASS_BMEM,
	/** ad-hoc memory */
  //Yuanguo: ADMEM 和 BMEM 类似，也是使用 WAL 持久化。实验性，非生产可用。
	UMEM_CLASS_ADMEM,
	/** unknown */
	UMEM_CLASS_UNKNOWN,
} umem_class_id_t;

typedef void (*umem_tx_cb_t)(void *data, bool noop);

#define UMEM_TX_DATA_MAGIC	(0xc01df00d)
#define UMEM_TX_CB_SHIFT_MAX	20	/* 1m callbacks */
#define UMEM_TX_CB_SHIFT_INIT	5	/* 32 callbacks */

struct umem_tx_stage_item;

/**
 * data structure to store all pmem transaction stage callbacks.
 * See pmemobj_tx_begin and pmemobj_tx_end of PMDK for the details.
 */
struct umem_tx_stage_data {
	int				 txd_magic;
	unsigned int			 txd_commit_cnt;
	unsigned int			 txd_commit_max;
	unsigned int			 txd_abort_cnt;
	unsigned int			 txd_abort_max;
	unsigned int			 txd_end_cnt;
	unsigned int			 txd_end_max;
	struct umem_tx_stage_item	*txd_commit_vec;
	struct umem_tx_stage_item	*txd_abort_vec;
	struct umem_tx_stage_item	*txd_end_vec;
};

/** Initialize \a txd which is for attaching pmem transaction stage callbacks */
int  umem_init_txd(struct umem_tx_stage_data *txd);
/** Finalize the txd initialized by \a umem_init_txd */
void umem_fini_txd(struct umem_tx_stage_data *txd);

struct umem_instance;

/* Flags associated with various umem ops */
#define UMEM_FLAG_ZERO		(((uint64_t)1) << 0)
#define UMEM_FLAG_NO_FLUSH	(((uint64_t)1) << 1)
#define UMEM_XADD_NO_SNAPSHOT	(((uint64_t)1) << 2)

/* type num used by umem ops */
enum {
	UMEM_TYPE_ANY,
};

/* Hints for umem atomic copy operation primarily for bmem implementation */
enum acopy_hint {
	UMEM_COMMIT_IMMEDIATE = 0, /* commit immediate, do not call within a tx */
	UMEM_RESERVED_MEM	/* memory from dav_reserve(), commit on publish */
};

typedef struct {
	/** free umoff */
	int		 (*mo_tx_free)(struct umem_instance *umm,
				       umem_off_t umoff);
	/**
	 * allocate umoff with the specified size & flags
	 *
	 * \param umm	   [IN]	umem class instance.
	 * \param size	   [IN]	size to allocate.
	 * \param flags	   [IN]	flags like zeroing, noflush (for PMDK)
	 * \param type_num [IN]	struct type (for PMDK)
	 */
	//Yuanguo:
	// (BMEM) mo_tx_alloc 和 mo_reserve: 两者其实都没有真正分配内存（没有反转bitmap），只是预留了内存，并返回offset（用户可以修改那块内存）
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    |                  | mo_tx_alloc                            | mo_reserve                                     |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | 调用场景         | 必须在 tx_begin ... tx_commit 之间     | 不要求在事务内                                 |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | action 维护      | tx->actions                            | 调用者提供 action                              |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | ranges 维护      | tx->ranges，但不生成undo log，abort不必| 无，纳入事务时处理？                           |
	//    |                  | 回滚到原始状态(新预留内存)             |                                                |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | 何时真正分配     | commit 时事务框架统一调 palloc_publish | 调用者在后续的某个事务中，显式调用             |
	//    |                  | 翻转 bitmap                            | mo_tx_publish 来"领养"这些 action              |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | 如何cancel预留   | abort 时由事务框架统一 cancel 这些     | 显示调用 mo_cancel                             |
	//    |                  | action                                 |                                                |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | 设计意图         | 事务内使用内存                         | 先在事务外预留空间并填数据，然后在一个事务中一 |
	//    |                  |                                        | 次性把所有预留"纳入"事务保护。                 |
	//    |------------------|----------------------------------------|------------------------------------------------|
	// 关于内存的预留/分配，见 struct tx 和 struct dav_clogs 中的注释。
	umem_off_t	 (*mo_tx_alloc)(struct umem_instance *umm, size_t size,
					uint64_t flags, unsigned int type_num);
	/**
	 * Add the specified range of umoff to current memory transaction.
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param umoff	[IN]	memory offset to be added to transaction.
	 * \param offset [IN]	start offset of \a umoff tracked by the
	 *			transaction.
	 * \param size	[IN]	size of \a umoff tracked by the transaction.
	 */
	//Yuanguo:
	//  (BMEM) 和新分配内存不同，当事务要修改一块旧内存时，需要额外工作：
	//    - 添加到 tx->ranges（同新分配内存）
	//    - 记录undo log；若tx abort，必须使用 undo log 把内存恢复到原来的状态；见 dav_tx_add_common 的注释！
	// “修改”一块内存前，就需要调用本函数来完成上述两件事。
	int		 (*mo_tx_add)(struct umem_instance *umm,
				      umem_off_t umoff, uint64_t offset,
				      size_t size);

	/**
	 * Add the specified range of umoff to current memory transaction but
	 * with flags.
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param umoff	[IN]	memory offset to be added to transaction.
	 * \param offset [IN]	start offset of \a umoff tracked by the
	 *			transaction.
	 * \param size	[IN]	size of \a umoff tracked by the transaction.
	 * \param flags [IN]	PMDK flags
	 */
	//Yuanguo:
	// (BMEM) 同 mo_tx_add，但若 flags 包含 DAV_XADD_NO_SNAPSHOT 位，则不生成 undo log (不需要回滚)
	int		 (*mo_tx_xadd)(struct umem_instance *umm,
				       umem_off_t umoff, uint64_t offset,
				       size_t size, uint64_t flags);
	/**
	 * Add the directly accessible pointer to current memory transaction.
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param ptr [IN]	Directly accessible memory pointer.
	 * \param size	[IN]	size to be tracked by the transaction.
	 */
	//Yuanguo:
	//  for pmem: pmem_tx_add_ptr
	//  for bmem: bmem_tx_add_ptr
	//Yuanguo:
	// (BMEM) 同 mo_tx_add，只是参数形式不同；
	int		 (*mo_tx_add_ptr)(struct umem_instance *umm,
					  void *ptr, size_t size);
	/** abort memory transaction */
	int		 (*mo_tx_abort)(struct umem_instance *umm, int error);
	/** start memory transaction */
	//Yuanguo: 初始化事务
	//   - 预留 cache page
	//   - 分配 WAL ID
	//   - 分配 umem_wal_tx(管理redo logs)
	//   - 初始化 clogs (管理undo logs；管理 external log；见 struct dav_clogs 中的注释)
	//   - 初始化 tx->actions
	//   - 初始化 tx->ranges
	//   - tx->stage = DAV_TX_STAGE_WORK  进入工作阶段
	int		 (*mo_tx_begin)(struct umem_instance *umm,
					struct umem_tx_stage_data *txd);

	//Yuanguo: (BMEM) 内存操作
	//
	//  |-------------------|-------------------------|------------------------------|------------------------------|------------------------------------|
	//  | 修改对象          | 触发 API                | 何时可见                     | 持久化方式                   | 涉及结构                           |
	//  |-------------------|-------------------------|------------------------------|------------------------------|------------------------------------|
	//  | 用户数据修改      | dav_tx_add_range + 直接 | 立即可见（普通 C 写入 RAM）  | 先把原值写入 UNDO log；      | pop->undo → operation_add_buffer → |
	//  |                   | *ptr = ...              |                              | commit 丢弃；abort 时回滚；  | ULOG_OPERATION_BUF_CPY(undo log)   |
	//  |-------------------|-------------------------|------------------------------|------------------------------|------------------------------------|
	//  | 用户分配/释放内存 | dav_tx_alloc /          | commit 时生效（在用户视角：  | REDO 路径：tx actions 暂存 → | tx->actions[] → palloc_publish →   |
	//  |                   | dav_tx_free             | 返回的 offset 立即可用，但分 | commit 时生成 redo entry →   | external (pshadow_ops) →           |
	//  |                   |                         | 配器元数据 commit 后才落定） | 写 WAL → apply               | 操作 chunk_header / bitmap         |
	//  |-------------------|-------------------------|------------------------------|------------------------------|------------------------------------|
	//  | 分配器内部元数据  | 由分配器内部触发        | commit 时生效                | REDO 写 WAL → apply 到 RAM   | 同上：与"用户分配"是同一条路径     |
	//  |（chunk_header /   |（无独立 API）           |（apply pshadow_ops）         |                              |       本质就是它的内部步骤         |
	//  |  bitmap）         |                         |                              |                              |                                    |
	//  |-------------------|-------------------------|------------------------------|------------------------------|------------------------------------|
	//  | root 指针 / size  | dav_root / 内部         | 同上                         | 同上                         | 同上                               |
	//  | 等小标量          | operation_add_entry     |                              |                              |                                    |
	//  |-------------------|-------------------------|------------------------------|------------------------------|------------------------------------|
	//
	//Yuanguo:
	//  A. 用户数据修改
	//     修改时：
	//         - 把被修改前的状态保存到 undo log (struct dav_obj::undo->ulog 指向的 struct dav_obj::clogs.undo)；
	//         - 把修改区间记录到 tx->ranges；
	//         - 不产生用户修改类 action，但首次 snapshot 会产生一个内部 gen_num++ action，用于 UNDO 失效机制；
	//           这个action的处理，同下面 "用户分配/释放内存"
	//     commit时：
	//         - 根据 tx->ranges 以及内存状态（被修改后的），生成 redo/wal log 累积到 struct dav_tx wt_redo
	//         - 丢弃undo log；
	//         - redo/wal log (struct dav_tx wt_redo) 持久化到 nvme wal blob
	//     rollback时：
	//         - 使用undo log 恢复到修改前的状态；
	//  B. 用户分配/释放内存（以分配为例）
	//     分配时：
	//         - 把分配的内存区间记录到 tx->ranges；
	//         - 把分配动作(action) 记录到 tx->actions 中，不立即 apply（不立即可见）；
	//         - 返回地址，用户可以立即开始写入数据；
	//         - 不产生undo log；
	//     commit时：
	//         - 根据 tx->ranges 以及内存状态（用户新写的数据），生成 redo/wal log 累积到 struct dav_tx wt_redo
	//         - tx->actions 中的 action(huge模式修改chunk_header；run模式修改bitmap中的bits)，转换成redo log记录到
	//           struct dav_obj::external->pshadow_ops/transient_ops 中；
	//         - pshadow_ops 中的 redo log，进一步转成 redo/wal log 累积到 struct dav_tx wt_redo；
	//           transient_ops：不用持久化；它对内存的修改，可以通过重构heap完成
	//         - apply pshadow_ops 和 transient_ops, 实际修改 chunk_header (huge模式) 或 bitmap (run模式)，使分配可见；
	//         - redo/wal log (struct dav_tx wt_redo) 持久化到 nvme wal blob
	//     rollback时:
	//         - 丢弃tx->actions，不apply：用户得到的内存以及写入的数据，自动无效，因为内存分配动作(修改 chunk_header/bitmap)没有执行；
	//  C. 分配器内部元数据(chunk_header/bitmap):
	//     修改时：如上
	//     commit时：如上
	//     rollback时：如上
	//  D. root 指针/size等小标量:
	//     修改时：如上
	//     commit时：如上
	//     rollback时：如上

	/** commit memory transaction */
	//Yuanguo: (BMEM)
	// dav_tx_commit()
	//   ├── 1. obj_tx_callback(tx)            // WORK 阶段回调
	//   │
	//   ├── 2. tx_pre_commit(tx)              // 刷 WAL redo（数据修改）
	//   │       └── ravl_delete_cb(tx->ranges, tx_flush_range)
	//   │           ├── 遍历 ranges 树，对每个 range 调用:
	//   │           │        mo_wal_flush(pop, ptr, size)  // 将用户修改的数据写入 WAL redo，并链到 struct dav_tx wt_redo 链表尾；
	//   │           └── free tx->ranges
	//   │
	//   ├── 3. mo_wal_drain(&pop->p_ops)      // 等待之前的 WAL 写入完成
	//   │
	//   ├── 4. operation_start(pop->external) // 开启新的 operation 上下文；下面 palloc_publish 将把 tx->actions 中的"actions" 转换成 
	//   │                                     // redo log，记录到 pop->external->pshadow_ops 中；这些 "actions" 代表内部元数据(bitmap等)
	//   │                                     // 的修改；
	//   │
	//   ├── 5. palloc_publish(heap, tx->actions, cnt, ctx=pop->external)
	//   │       └─ palloc_exec_actions(heap, ctx, actv, actvcnt)
	//   │           ├── a. qsort(actv) — 按 lock 地址排序，确保加锁顺序一致
	//   │           ├── b. 逐个 action 加锁 + exec:
	//   │           │      action_funcs[act->type].exec(heap, act, ctx)
	//   │           │       └── palloc_heap_action_exec():  <-- 对于分配内存的action
	//   │           │           act->m.m_ops->prep_hdr(&act->m, act->new_state, ctx)
	//   │           │           生成 bitmap 修改的 redo entry（SET_BITS/CLR_BITS），写到pop->external->pshadow_ops.ulog
	//   │           │           此时还没有真正修改 bitmap
	//   │           │
	//   │           │       └── palloc_mem_action_exec(): <-- 对于修改内存的 action (非用户修改，分配器内部状态维护)
	//   │           │                                         act->type 为 DAV_ACTION_TYPE_MEM, 生成的 ulog (注意是redo log) 为 ULOG_OPERATION_SET 类型（设置一个持久内存单元64B）
	//   │           │                                         例如： dav_tx_add_snapshot 过程中（维护 undo log），当 tx->first_snapshot 成立时，会产生 DAV_ACTION_TYPE_MEM 类action;
	//   │           │                                                即维护 undo log 时，生成了一个 redo log;
	//   │           │
	//   │           ├── c. mo_wal_drain() — 等待 object header 持久化
	//   │           │
	//   │           ├── d. operation_process(ctx=pop->external) — 执行所有持久化修改
	//   │           │      ├── 单条优化路径：如果只有1条 redo entry
	//   │           │      │   └── tx_create_wal_entry() + ulog_entry_apply()
	//   │           │      │       // 直接写 WAL + 原地修改 bitmap
	//   │           │      └── 多条路径：
	//   │           │          └── operation_process_persistent_redo(ctx=pop->external)
	//   │           │              ├── ulog_foreach_entry → tx_create_wal_entry()  遍历 ctx->pshadow_ops.ulog（只包含 LOG_PERSISTENT entry），对每个 entry:
	//   │           │              │                                               调用 tx_create_wal_entry 转换成 WAL 动作，追加到 struct dav_tx 的 wt_redo 链表尾!
	//   │           │              │                                               此时仅在 DRAM 中累积，尚未持久化! 持久化见下面 dav_tx_end!
	//   │           │              ├── ulog_process() → ulog_foreach_entry → ulog_process_entry：原地 apply 所有修改！
	//   │           │              │                                               真正翻转 bitmap（alloc→used, free→free）
	//   │           │              │                                               分配内存的bitmap修改，内部状态设置，等，现在“可见”！
	//   │           │              └── ulog_clobber() — 清理临时 redo log
	//   │           │
	//   │           ├── e. 逐个 action: on_process() — 更新运行时状态（持锁）
	//   │           ├── f. 逐个 action: 解锁
	//   │           ├── g. 逐个 action: on_unlock() — 释放运行时资源（无锁）
	//   │           └── h. operation_finish(ctx=pop->external) — 清理 redo log
	//   │
	//   ├── 6. tx_post_commit(tx=pop->undo)
	//   │       └── operation_finish(pop->undo) — 清理 undo log
	//   │               // 与 operation_finish 对应的 operation_start(pop->undo) 在 dav_tx_begin 中就被调用了
	//   │               // 因为 pop->undo 是用于存储 undo log；而 undo log 在 user 修改内存(dav_tx_add_common → dav_tx_add_snapshot) 时产生；
	//   │               // 另外，operation_process 不会对 pop->undo 调用(1. 我检查了代码； 2. operation_process 内有断言)；
	//   │
	//   ├── 7. dav_release_clogs(pop)         // 释放 commit log 资源
	//   │
	//   └── 8. tx->stage = ONCOMMIT → obj_tx_callback(tx) // ONCOMMIT 回调
	//
	// 然后调用方调用 dav_tx_end():
	//   dav_tx_end(data)
	//   ├── 清理 tx_entries 链表
	//   ├── VEC_DELETE(&tx->actions)
	//   └── lw_tx_end(pop, data)
	//       ├── stats_persist()               // 持久化统计数据
	//       └── dav_wal_tx_commit(pop, utx, data)
	//           └── dav_wal_tx_submit(hdl, utx, data)
	//               └── store->stor_ops->so_wal_submit(store, utx, data)
	//                   // 将所有 WAL redo actions 提交给底层存储
	int		 (*mo_tx_commit)(struct umem_instance *umm, void *data);

#ifdef DAOS_PMEM_BUILD
	/** get TX stage */
	int		 (*mo_tx_stage)(void);

	/**
	 * Reserve space with specified size.
	 *
	 * \param umm	[IN]		umem class instance.
	 * \param act	[IN|OUT]	action used for later cancel/publish.
	 * \param size	[IN]		size to be reserved.
	 * \param type_num [IN]		struct type (for PMDK)
	 */
	//Yuanguo:
	// (BMEM) mo_tx_alloc 和 mo_reserve: 两者其实都没有真正分配内存（没有反转bitmap），只是预留了内存，并返回offset（用户可以修改那块内存）
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    |                  | mo_tx_alloc                            | mo_reserve                                     |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | 调用场景         | 必须在 tx_begin ... tx_commit 之间     | 不要求在事务内                                 |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | action 维护      | tx->actions                            | 调用者提供 action                              |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | ranges 维护      | tx->ranges，但不生成undo log，abort不必| 无，纳入事务时处理？                           |
	//    |                  | 回滚到原始状态(新预留内存)             |                                                |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | 何时真正分配     | commit 时事务框架统一调 palloc_publish | 调用者在后续的某个事务中，显式调用             |
	//    |                  | 翻转 bitmap                            | mo_tx_publish 来"领养"这些 action              |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | 如何cancel预留   | abort 时由事务框架统一 cancel 这些     | 显示调用 mo_cancel                             |
	//    |                  | action                                 |                                                |
	//    |------------------|----------------------------------------|------------------------------------------------|
	//    | 设计意图         | 事务内使用内存                         | 先在事务外预留空间并填数据，然后在一个事务中一 |
	//    |                  |                                        | 次性把所有预留"纳入"事务保护。                 |
	//    |------------------|----------------------------------------|------------------------------------------------|
	// 关于内存的预留/分配，见 struct tx 和 struct dav_clogs 中的注释。
	umem_off_t	 (*mo_reserve)(struct umem_instance *umm, void *act, size_t size,
				       unsigned int type_num);

	/**
	 * Defer free til commit.  For use with reserved extents that are not
	 * yet published.  For VMEM, it just calls free.
	 *
	 * \param umm	[IN]		umem class instance.
	 * \param off	[IN]		offset of allocation
	 * \param act	[IN|OUT]	action used for later cancel/publish.
	 */
	void		 (*mo_defer_free)(struct umem_instance *umm, umem_off_t off, void *act);

	/**
	 * Cancel the reservation.
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param actv	[IN]	action array to be canceled.
	 * \param actv_cnt [IN]	size of action array.
	 */
	void		 (*mo_cancel)(struct umem_instance *umm, void *act, int actv_cnt);

	/**
	 * Publish the reservation (make it persistent).
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param actv	[IN]	action array to be published.
	 * \param actv_cnt [IN]	size of action array.
	 */
	// Yuanguo: 将预留操作从"私有/暂态"状态变为"事务可见/即将持久化"状态；见 mo_tx_alloc / mo_reserve
	int		 (*mo_tx_publish)(struct umem_instance *umm, void *act, int actv_cnt);

	/**
	 * Atomically copy the contents from src to the destination address.
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param dest	[IN]	destination address
	 * \param src	[IN]	source address
	 * \param len	[IN]	length of data to be copied.
	 * \param hint	[IN]	hint on when to persist.
	 */
	 void *		(*mo_atomic_copy)(struct umem_instance *umem,
					  void *dest, const void *src,
					  size_t len, enum acopy_hint hint);

	/** free umoff atomically */
	int		 (*mo_atomic_free)(struct umem_instance *umm,
					   umem_off_t umoff);

	/**
	 * allocate umoff with the specified size & flags atomically
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param size	[IN]	size to allocate.
	 * \param flags	[IN]	flags like zeroing, noflush (for PMDK)
	 * \param type_num [IN]	struct type (for PMDK)
	 */
	umem_off_t	 (*mo_atomic_alloc)(struct umem_instance *umm, size_t size,
					    unsigned int type_num);

	/**
	 * flush data at specific offset to persistent store.
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param addr	[IN]	starting address
	 * \param size	[IN]	total bytes to be flushed.
	 */
	void		(*mo_atomic_flush)(struct umem_instance *umm, void *addr,
					   size_t size);

#endif
	/**
	 * Add one commit or abort callback to current transaction.
	 *
	 * PMDK doesn't provide public API to get stage_callback_arg, so
	 * we have to make @txd as an input parameter.
	 *
	 * \param umm	[IN]	umem class instance.
	 * \param txd	[IN]	transaction stage data.
	 * \param cb	[IN]	commit or abort callback.
	 * \param data	[IN]	callback data.
	 */
	int		 (*mo_tx_add_callback)(struct umem_instance *umm,
					       struct umem_tx_stage_data *txd,
					       int stage, umem_tx_cb_t cb,
					       void *data);
} umem_ops_t;

/** attributes to initialize an unified memory class */
struct umem_attr {
	umem_class_id_t			 uma_id;
	struct umem_pool		*uma_pool;
};

/** instance of an unified memory class */
struct umem_instance {
	umem_class_id_t		 umm_id;
	int			 umm_nospc_rc;
	const char		*umm_name;
	struct umem_pool	*umm_pool;
	/** Cache the pool id field for umem addresses */
	uint64_t		 umm_pool_uuid_lo;
	/** Cache the base address of the pool */
	uint64_t		 umm_base;
	/** class member functions */
	umem_ops_t		*umm_ops;
};

#ifdef DAOS_PMEM_BUILD
void umem_stage_callback(int stage, void *data);
#endif

int  umem_class_init(struct umem_attr *uma, struct umem_instance *umm);
void umem_attr_get(struct umem_instance *umm, struct umem_attr *uma);

/** Convert an offset to pointer.
 *
 *  \param	umm[IN]		The umem pool instance
 *  \param	umoff[in]	The offset to convert
 *
 *  \return	The address in memory
 */
static inline void *
umem_off2ptr(const struct umem_instance *umm, umem_off_t umoff)
{
	if (UMOFF_IS_NULL(umoff))
		return NULL;

	return (void *)(umm->umm_base + umem_off2offset(umoff));
}

/** Convert pointer to an offset.
 *
 *  \param	umm[IN]		The umem pool instance
 *  \param	ptr[in]		The direct pointer to convert
 *
 *  Returns the umem offset
 */
static inline umem_off_t
umem_ptr2off(const struct umem_instance *umm, void *ptr)
{
	if (ptr == NULL)
		return UMOFF_NULL;

	return (umem_off_t)ptr - umm->umm_base;
}

/**
 * Get pmemobj pool uuid
 *
 * \param	umm[IN]	The umem pool instance
 */
static inline uint64_t
umem_get_uuid(const struct umem_instance *umm)
{
	return umm->umm_pool_uuid_lo;
}

static inline bool
umem_has_tx(struct umem_instance *umm)
{
	return umm->umm_ops->mo_tx_add != NULL;
}

#define umem_alloc_verb(umm, flags, size)			                                   \
	({                                                                                         \
		umem_off_t __umoff;                                                                \
                                                                                                   \
		__umoff = (umm)->umm_ops->mo_tx_alloc(umm, size, flags, UMEM_TYPE_ANY);   \
		D_ASSERTF(umem_off2flags(__umoff) == 0,                                            \
			  "Invalid assumption about allocnot using flag bits");                    \
		D_DEBUG(DB_MEM,                                                                    \
			"allocate %s umoff=" UMOFF_PF " size=%zu base=" DF_X64                     \
			" pool_uuid_lo=" DF_X64 "\n",                                              \
			(umm)->umm_name, UMOFF_P(__umoff), (size_t)(size), (umm)->umm_base,        \
			(umm)->umm_pool_uuid_lo);                                                  \
		__umoff;                                                                           \
	})

#define umem_alloc(umm, size)						\
	umem_alloc_verb(umm, 0, size)

#define umem_zalloc(umm, size)						\
	umem_alloc_verb(umm, UMEM_FLAG_ZERO, size)

#define umem_alloc_noflush(umm, size)					\
	umem_alloc_verb(umm, UMEM_FLAG_NO_FLUSH, size)

#define umem_free(umm, umoff)                                                                      \
	({                                                                                         \
		D_DEBUG(DB_MEM,                                                                    \
			"Free %s umoff=" UMOFF_PF " base=" DF_X64 " pool_uuid_lo=" DF_X64 "\n",    \
			(umm)->umm_name, UMOFF_P(umoff), (umm)->umm_base,                          \
			(umm)->umm_pool_uuid_lo);                                                  \
                                                                                                   \
		(umm)->umm_ops->mo_tx_free(umm, umoff);                                            \
	})

static inline int
umem_tx_add_range(struct umem_instance *umm, umem_off_t umoff, uint64_t offset,
		  size_t size)
{
	if (umm->umm_ops->mo_tx_add)
		return umm->umm_ops->mo_tx_add(umm, umoff, offset, size);
	else
		return 0;
}

static inline int
umem_tx_xadd_range(struct umem_instance *umm, umem_off_t umoff, uint64_t offset,
		   size_t size, uint64_t flags)
{
	//Yuanguo:
	//  pmem: pmem_tx_xadd
	//  bmem: bmem_tx_xadd
	if (umm->umm_ops->mo_tx_xadd)
		return umm->umm_ops->mo_tx_xadd(umm, umoff, offset, size,
						flags);
	else
		return 0;
}

static inline int
umem_tx_add_ptr(struct umem_instance *umm, void *ptr, size_t size)
{
	if (umm->umm_ops->mo_tx_add_ptr)
		return umm->umm_ops->mo_tx_add_ptr(umm, ptr, size);
	else
		return 0;
}

static inline int
umem_tx_xadd_ptr(struct umem_instance *umm, void *ptr, size_t size,
		 uint64_t flags)
{
	umem_off_t	offset = umem_ptr2off(umm, ptr);

	return umem_tx_xadd_range(umm, offset, 0, size, flags);
}

#define umem_tx_add(umm, umoff, size)					\
	umem_tx_add_range(umm, umoff, 0, size)

#define umem_tx_xadd(umm, umoff, size, flags)				\
	umem_tx_xadd_range(umm, umoff, 0, size, flags)

static inline int
umem_tx_begin(struct umem_instance *umm, struct umem_tx_stage_data *txd)
{
    //Yuanguo:
    //  pmem: pmem_tx_begin
    //  bmem: bmem_tx_begin
	if (umm->umm_ops->mo_tx_begin)
		return umm->umm_ops->mo_tx_begin(umm, txd);
	else
		return 0;
}

static inline int
umem_tx_commit_ex(struct umem_instance *umm, void *data)
{
    //Yuanguo:
    //  - pmem: pmem_tx_commit()
    //  - bmem: bmem_tx_commit()
	if (umm->umm_ops->mo_tx_commit)
		return umm->umm_ops->mo_tx_commit(umm, data);
	else
		return 0;
}

static inline int
umem_tx_commit(struct umem_instance *umm)
{
	return umem_tx_commit_ex(umm, NULL);
}

static inline int
umem_tx_abort(struct umem_instance *umm, int err)
{
    //Yuanguo:
    //  - pmem: pmem_tx_abort()
    //  - bmem: bmem_tx_abort()
	if (umm->umm_ops->mo_tx_abort)
		return umm->umm_ops->mo_tx_abort(umm, err);
	else
		return err;
}

static inline int
umem_tx_end_ex(struct umem_instance *umm, int err, void *data)
{
	if (err)
		return umem_tx_abort(umm, err);
	else
		return umem_tx_commit_ex(umm, data);
}

static inline int
umem_tx_end(struct umem_instance *umm, int err)
{
	return umem_tx_end_ex(umm, err, NULL);
}

#ifdef DAOS_PMEM_BUILD
bool umem_tx_inprogress(struct umem_instance *umm);
bool umem_tx_none(struct umem_instance *umm);

int umem_tx_errno(int err);

static inline int
umem_tx_stage(struct umem_instance *umm)
{
	return umm->umm_ops->mo_tx_stage();
}

/* Get number of umem_actions in TX redo log */
static inline uint32_t
umem_tx_act_nr(struct umem_wal_tx *tx)
{
    //Yuanguo: 对于MD-on-SSD (dav)，wtx_act_nr = wal_tx_act_nr
	return tx->utx_ops->wtx_act_nr(tx);
}

/* Get payload size of umem_actions in TX redo list */
static inline uint32_t
umem_tx_act_payload_sz(struct umem_wal_tx *tx)
{
	return tx->utx_ops->wtx_payload_sz(tx);
}

/* Get the first umem_action in TX redo list */
static inline struct umem_action *
umem_tx_act_first(struct umem_wal_tx *tx)
{
	return tx->utx_ops->wtx_act_first(tx);
}

/* Get the next umem_action in TX redo list */
static inline struct umem_action *
umem_tx_act_next(struct umem_wal_tx *tx)
{
	return tx->utx_ops->wtx_act_next(tx);
}

struct umem_rsrvd_act;

/* Get the active reserved actions cnt pending for publications */
int umem_rsrvd_act_cnt(struct umem_rsrvd_act *act);
/* Allocate array of structures for reserved actions */
int umem_rsrvd_act_alloc(struct umem_instance *umm, struct umem_rsrvd_act **act, int cnt);
/* Extend the array of structures for reserved actions to max_cnt */
int umem_rsrvd_act_realloc(struct umem_instance *umm, struct umem_rsrvd_act **act, int max_cnt);
/* Free up the array of reserved actions */
int umem_rsrvd_act_free(struct umem_rsrvd_act **act);

umem_off_t umem_reserve(struct umem_instance *umm,
			struct umem_rsrvd_act *rsrvd_act, size_t size);
void umem_defer_free(struct umem_instance *umm, umem_off_t off,
		     struct umem_rsrvd_act *rsrvd_act);
void umem_cancel(struct umem_instance *umm, struct umem_rsrvd_act *rsrvd_act);
int umem_tx_publish(struct umem_instance *umm,
		    struct umem_rsrvd_act *rsrvd_act);

static inline void *
umem_atomic_copy(struct umem_instance *umm, void *dest, void *src, size_t len,
		 enum acopy_hint hint)
{
	D_ASSERT(umm->umm_ops->mo_atomic_copy != NULL);
	return umm->umm_ops->mo_atomic_copy(umm, dest, src, len, hint);
}

static inline umem_off_t
umem_atomic_alloc(struct umem_instance *umm, size_t len, unsigned int type_num)
{
	D_ASSERT(umm->umm_ops->mo_atomic_alloc != NULL);
	return umm->umm_ops->mo_atomic_alloc(umm, len, type_num);
}

static inline int
umem_atomic_free(struct umem_instance *umm, umem_off_t umoff)
{
	D_ASSERT(umm->umm_ops->mo_atomic_free != NULL);
	return umm->umm_ops->mo_atomic_free(umm, umoff);
}

static inline void
umem_atomic_flush(struct umem_instance *umm, void *addr, size_t len)
{
	if (umm->umm_ops->mo_atomic_flush)
		umm->umm_ops->mo_atomic_flush(umm, addr, len);
	return;
}

int
umem_tx_add_cb(struct umem_instance *umm, struct umem_tx_stage_data *txd, int stage,
	       umem_tx_cb_t cb, void *data);

static inline int
umem_tx_add_callback(struct umem_instance *umm, struct umem_tx_stage_data *txd,
		     int stage, umem_tx_cb_t cb, void *data)
{
	D_ASSERT(umm->umm_ops->mo_tx_add_callback != NULL);
	return umm->umm_ops->mo_tx_add_callback(umm, txd, stage, cb, data);
}

/*********************************************************************************/

/* Type of memory actions */
enum {
	UMEM_ACT_NOOP			= 0,
	/** copy appended payload to specified storage address */
	UMEM_ACT_COPY,
	/** copy payload addressed by @ptr to specified storage address */
	UMEM_ACT_COPY_PTR,
	/** assign 8/16/32 bits integer to specified storage address */
	UMEM_ACT_ASSIGN,
	/** move specified bytes from source address to destination address */
	UMEM_ACT_MOVE,
	/** memset a region with specified value */
	UMEM_ACT_SET,
	/** set the specified bit in bitmap */
	UMEM_ACT_SET_BITS,
	/** unset the specified bit in bitmap */
	UMEM_ACT_CLR_BITS,
	/** it's checksum of the specified address */
	UMEM_ACT_CSUM,
};

/**
 * Memory operations for redo/undo.
 * 16 bytes for bit operation (set/clr) and integer assignment, 32+ bytes for other operations.
 */
#define UMEM_ACT_PAYLOAD_MAX_LEN	(1ULL << 20)
struct umem_action {
	uint16_t			ac_opc;
	union {
		struct {
			uint64_t		addr;
			uint64_t		size;
			uint8_t			payload[0];
		} ac_copy;	/**< copy payload from @payload to @addr */
		struct {
			uint64_t		addr;
			uint64_t		size;
			uint64_t		ptr;
		} ac_copy_ptr;	/**< copy payload from @ptr to @addr */
		struct {
			uint16_t		size;
			uint32_t		val;
			uint64_t		addr;
		} ac_assign;	/**< assign integer to @addr, int64 should use ac_copy */
		struct {
			uint16_t		num;
			uint32_t		pos;
			uint64_t		addr;
		} ac_op_bits;	/**< set or clear the @pos bit in bitmap @addr */
		struct {
			uint8_t			val;
			uint32_t		size;
			uint64_t		addr;
		} ac_set;	/**< memset(addr, val, size) */
		struct {
			uint32_t		size;
			uint64_t		src;
			uint64_t		dst;
		} ac_move;	/**< memmove(dst, size src) */
		struct {
			uint32_t		csum;
			uint32_t		size;
			uint64_t		addr;
		} ac_csum;	/**< it is checksum of data stored in @addr */
	};
};

#define UMEM_CACHE_PAGE_SZ_SHIFT  24 /* 16MB */
#define UMEM_CACHE_PAGE_SZ        (1 << UMEM_CACHE_PAGE_SZ_SHIFT)
#define UMEM_CACHE_PAGE_SZ_MASK   (UMEM_CACHE_PAGE_SZ - 1)

#define UMEM_CACHE_CHUNK_SZ_SHIFT 12 /* 4KB */
#define UMEM_CACHE_CHUNK_SZ       (1 << UMEM_CACHE_CHUNK_SZ_SHIFT)
#define UMEM_CACHE_CHUNK_SZ_MASK  (UMEM_CACHE_CHUNK_SZ - 1)

//Yuanguo: bitmap包含多少个uint64_t?
//   1 << (24-12-6) = 64
// 64个uint64_t，共4096个bit；每个bit代表一个cache-chunk(4k)，构成一个page(16M)
#define UMEM_CACHE_BMAP_SZ        (1 << (UMEM_CACHE_PAGE_SZ_SHIFT - UMEM_CACHE_CHUNK_SZ_SHIFT - 6))

struct umem_page_info;
/** 16 MB page */
struct umem_page {
	/** page ID */
	unsigned int		 pg_id;
	/** refcount */
	int			 pg_ref;
	/** page info */
	struct umem_page_info   *pg_info;
};

/** Global cache status for each umem_store */
struct umem_cache {
	struct umem_store	*ca_store;
	/** Total pages store */
	uint64_t                 ca_num_pages;
	/** Total pages in cache */
	uint64_t                 ca_mapped;
	/** Maximum number of cached pages */
	uint64_t                 ca_max_mapped;
	/** Free list for mapped page info */
	d_list_t                 ca_pi_free;
	/** all the dirty pages */
	d_list_t                 ca_pgs_dirty;
	/** Pages waiting for copy to DMA buffer */
	d_list_t                 ca_pgs_copying;
	/** LRU list all pages not in one of the other states for future eviction support */
	d_list_t                 ca_pgs_lru;
	/** TODO: some other global status */
	/** All pages, sorted by umem_page::pg_id */
	struct umem_page         ca_pages[0];
};

struct umem_cache_chkpt_stats {
	/** Last committed checkpoint id */
	uint64_t	*uccs_chkpt_id;
	/** Number of pages processed */
	int		 uccs_nr_pages;
	/** Number of dirty chunks copied */
	int		 uccs_nr_dchunks;
	/** Number of sgl iovs used to copy dirty chunks */
	int		 uccs_nr_iovs;
};

static inline uint64_t
umem_cache_size2pages(uint64_t len)
{
	D_ASSERT((len & UMEM_CACHE_PAGE_SZ_MASK) == 0);

	return len >> UMEM_CACHE_PAGE_SZ_SHIFT;
}

static inline uint64_t
umem_cache_size_round(uint64_t len)
{
	return (len + UMEM_CACHE_PAGE_SZ_MASK) & ~UMEM_CACHE_PAGE_SZ_MASK;
}

//Yuanguo:
//  1. MD-on-SSD场景：tmpfs file path = /mnt/daos0/c090c2fc-8d83-45de-babe-104bad165593/vos-0被mmap到内存空间；
//
//     /mnt/daos0/c090c2fc-8d83-45de-babe-104bad165593/vos-0
//                           (memory)
//
//           base +---------------------------------+
//                |                                 |
//                |         struct dav_phdr         |
//                |              (4k)               |
//                |                                 |
//      heap_base +---------------------------------+
//                | +-----------------------------+ |
//                | | struct heap_header (1k)     | |
//                | +-----------------------------+ |
//                | | struct zone_header (64B)    | |
//                | | struct chunk_header(8B)     | |
//                | | struct chunk_header(8B)     | |
//                | | ...... 65528个,不一定都用   | |
//                | | struct chunk_header(8B)     | |
//                | | chunk (256k)                | |
//                | | chunk (256k)                | |
//                | | ...... 最多65528个          | |
//                | | chunk (256k)                | |
//                | +-----------------------------+ |
//                | | struct zone_header (64B)    | |
//                | | struct chunk_header(8B)     | |
//                | | struct chunk_header(8B)     | |
//                | | ...... 65528个,不一定都用   | |
//                | | struct chunk_header(8B)     | |
//                | | chunk (256k)                | |
//                | | chunk (256k)                | |
//                | | ...... 最多65528个          | |
//                | | chunk (256k)                | |
//                | +-----------------------------+ |
//                | |                             | |
//                | |     ... more zones ...      | |
//                | |                             | |
//                | |  除了最后一个，前面的zone   | |
//                | |  都是65528个chunk,接近16G   | |
//                | |                             | |
//                | +-----------------------------+ |
//                +---------------------------------+
//
//     offset: 相对于base的偏移；
//     输出: 这个偏移对应的cache page;
static inline struct umem_page *
umem_cache_off2page(struct umem_cache *cache, umem_off_t offset)
{
	uint64_t idx = offset >> UMEM_CACHE_PAGE_SZ_SHIFT;

	D_ASSERTF(idx < cache->ca_num_pages,
		  "offset=" DF_U64 ", num_pages=" DF_U64 ", idx=" DF_U64 "\n", offset,
		  cache->ca_num_pages, idx);

	return &cache->ca_pages[idx];
}

/** From a mapped page address, return the umem_cache it belongs to */
static inline struct umem_cache *
umem_page2cache(struct umem_page *page)
{
	return (struct umem_cache *)container_of(&page[-page->pg_id], struct umem_cache, ca_pages);
}

/** From a mapped page address, return the umem_store it belongs to */
static inline struct umem_store *
umem_page2store(struct umem_page *page)
{
	return umem_page2cache(page)->ca_store;
}

/** Allocate global cache for umem store.  All 16MB pages are initially unmapped
 *
 * \param[in]	store		The umem store
 * \param[in]	max_mapped	0 or Maximum number of mapped 16MB pages (must be 0 for now)
 *
 * \return 0 on success
 */
int
umem_cache_alloc(struct umem_store *store, uint64_t max_mapped);

/** Free global cache for umem store.  Pages must be unmapped first
 *
 * \param[in]	store	Store for which to free cache
 *
 * \return 0 on success
 */
int
umem_cache_free(struct umem_store *store);

/** Query if the page cache has enough space to map a range
 *
 * \param[in]	store		The store
 * \param[in]	num_pages	Number of pages to bring into cache
 *
 * \return number of pages that need eviction to support mapping the range
 */
int
umem_cache_check(struct umem_store *store, uint64_t num_pages);

/** Evict the pages.   This invokes the unmap callback. (XXX: not yet implemented)
 *
 * \param[in]	store		The store
 * \param[in]	num_pages	Number of pages to evict
 *
 * \return 0 on success, -DER_BUSY means a checkpoint is needed to evict the pages
 */
int
umem_cache_evict(struct umem_store *store, uint64_t num_pages);

/** Adds a mapped range of pages to the page cache.
 *
 * \param[in]	store		The store
 * \param[in]	offset		The offset in the umem cache
 * \param[in]	start_addr	Start address of mapping
 * \param[in]	num_pages	Number of consecutive 16MB pages to being cached
 *
 * \return 0 on success
 */
int
umem_cache_map_range(struct umem_store *store, umem_off_t offset, void *start_addr,
		     uint64_t num_pages);

/** Take a reference on the pages in the range.   Only needed for cases where we need the page to
 *  stay loaded across a yield, such as the VOS object cache.  Pages in the range must be mapped.
 *
 *  \param[in]	store	The umem store
 *  \param[in]	addr	The address of the hold
 *  \param[in]	size	The size of the hold
 *
 *  \return 0 on success
 */
int
umem_cache_pin(struct umem_store *store, umem_off_t addr, daos_size_t size);

/** Release a reference on pages in the range.  Pages in the range must be mapped and held.
 *
 *  \param[in]	store	The umem store
 *  \param[in]	addr	The address of the hold
 *  \param[in]	size	The size of the hold
 *
 *  \return 0 on success
 */
int
umem_cache_unpin(struct umem_store *store, umem_off_t addr, daos_size_t size);

/**
 * Touched the region identified by @addr and @size, it will mark pages in this region as
 * dirty (also set bitmap within each page), and put it on dirty list
 *
 * This function is called by allocator(probably VOS as well) each time it creates memory
 * snapshot (calls tx_snap) or just to mark a region to be flushed.
 *
 * \param[in]	store	The umem store
 * \param[in]	wr_tx	The writing transaction
 * \param[in]	addr	The start address
 * \param[in]	size	size of dirty region
 *
 * \return 0 on success, -DER_CHKPT_BUSY if a checkpoint is in progress on the page. The calling
 *         transaction must either abort or find another location to modify.
 */
int
umem_cache_touch(struct umem_store *store, uint64_t wr_tx, umem_off_t addr, daos_size_t size);

/** Callback for checkpoint to wait for the commit of chkpt_tx.
 *
 * \param[in]	arg		Argument passed to umem_cache_checkpoint
 * \param[in]	chkpt_tx	The WAL transaction ID we are waiting to commit to WAL
 * \param[out]	committed_tx	The WAL tx ID of the last transaction committed to WAL
 */
typedef void
umem_cache_wait_cb_t(void *arg, uint64_t chkpt_tx, uint64_t *committed_tx);

/**
 * Write all dirty pages before @wal_tx to MD blob. (XXX: not yet implemented)
 *
 * This function can yield internally, it is called by checkpoint service of upper level stack.
 *
 * \param[in]		store		The umem store
 * \param[in]		wait_cb		Callback for to wait for wal commit completion
 * \param[in]		arg		argument for wait_cb
 * \param[in,out]	chkpt_id	Input is last committed id, output is checkpointed id
 * \param[out]		chkpt_stats	check point stats
 *
 * \return 0 on success
 */
int
umem_cache_checkpoint(struct umem_store *store, umem_cache_wait_cb_t wait_cb, void *arg,
		      uint64_t *chkpt_id, struct umem_cache_chkpt_stats *chkpt_stats);

#endif /** DAOS_PMEM_BUILD */

#endif /* __DAOS_MEM_H__ */
