/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2015-2022, Intel Corporation */

/*
 * tx.c -- transactions implementation
 */

#include <inttypes.h>
#include <wchar.h>
#include <errno.h>

#include "queue.h"
#include "ravl.h"
#include "obj.h"
#include "out.h"
#include "tx.h"
#include "valgrind_internal.h"
#include "memops.h"
#include "dav_internal.h"

struct tx_data {
    //Yuanguo:
    // struct {
    //	 struct tx_data *sle_next;
    // } tx_entry;
	DAV_SLIST_ENTRY(tx_data) tx_entry;
	jmp_buf env;
	enum dav_tx_failure_behavior failure_behavior;
};

//Yuanguo:
//  一个线程(xstream)一个struct tx对象，存在thread local 变量里，见get_tx()函数；
//
//            struct tx
//  +---------------------------+
//  |                           |
//  |        tx_entries         |
//  | +-----------------------+ |              struct tx_data                      struct tx_data
//  | |      *slh_first       | | ----->  +------------------------+   +----> +------------------------+   +----> ...
//  | +-----------------------+ |         |                        |   |      |                        |   |
//  |                           |         |      tx_entry          |   |      |      tx_entry          |   |
//  |        ......             |         | +--------------------+ |   |      | +--------------------+ |   |
//  +---------------------------+         | |    *sle_next       | |---+      | |    *sle_next       | |---+
//                                        | +--------------------+ |          | +--------------------+ |
//                                        |                        |          |                        |
//                                        |      ......            |          |      ......            |
//                                        +------------------------+          +------------------------+
//
//  transactions可以嵌套，嵌套的transactions共用一个transaction id （其实就是一个transaction?)
//  每层嵌套有一个struct tx_data对象，构成链表；
//  最内层的transaction在最前；
//  所以，outermost transaction的tx_entry是NULL;
struct tx {
	dav_obj_t *pop;
	enum dav_tx_stage stage;
	int last_errnum;

	//Yuanguo:
	// struct txd {
	// 	  struct tx_data *slh_first;
	// } tx_entries;
	DAV_SLIST_HEAD(txd, tx_data) tx_entries;

	//Yuanguo:
	//  使用一个avl-tree保存current transaction修改的heap ranges:
	//      - 辅助生成 redo logs (持久化到 NVME wal blob)，见
	//          tx_pre_commit ->
	//          ravl_delete_cb ->
	//          tx_flush_range ->
	//          mo_wal_flush ->
	//          dav_wal_tx_snap
	//      - 辅助生成 undo logs (DRAM 中，用于abort 回滚，不用持久化)
	//          - undo logs 就是这些 ranges 的snapshot，用于在transaction abort时回滚；
	//          - 详细逻辑见 dav_tx_add_common() 函数;
	struct ravl *ranges;

	//Yuanguo:
	// struct {
	// 	struct dav_action *buffer;
	// 	size_t size;
	// 	size_t capacity;
	// } actions;
	//
	//Yuanguo: 非常重要，见 struct dav_clogs 中的注释 ！！！
	VEC(, struct dav_action) actions;

	dav_tx_callback stage_callback;
	void *stage_callback_arg;

	int first_snapshot;
};

/*
 * get_tx -- returns current transaction
 *
 * This function should be used only in high-level functions.
 */
static struct tx *
get_tx()
{
	static __thread struct tx tx;

	return &tx;
}

struct tx_alloc_args {
	uint64_t flags;
	const void *copy_ptr;
	size_t copy_size;
};

#define ALLOC_ARGS(flags)\
(struct tx_alloc_args){flags, NULL, 0}

struct tx_range_def {
	uint64_t offset;
	uint64_t size;
	uint64_t flags;
};

/*
 * tx_range_def_cmp -- compares two snapshot ranges
 */
static int
tx_range_def_cmp(const void *lhs, const void *rhs)
{
	const struct tx_range_def *l = lhs;
	const struct tx_range_def *r = rhs;

	if (l->offset > r->offset)
		return 1;
	else if (l->offset < r->offset)
		return -1;

	return 0;
}

static void
obj_tx_abort(int errnum, int user);

/*
 * obj_tx_fail_err -- (internal) dav_tx_abort variant that returns
 * error code
 */
static inline int
obj_tx_fail_err(int errnum, uint64_t flags)
{
	if ((flags & DAV_FLAG_TX_NO_ABORT) == 0)
		obj_tx_abort(errnum, 0);
	errno = errnum;
	return errnum;
}

/*
 * obj_tx_fail_null -- (internal) dav_tx_abort variant that returns
 * null PMEMoid
 */
static inline uint64_t
obj_tx_fail_null(int errnum, uint64_t flags)
{
	if ((flags & DAV_FLAG_TX_NO_ABORT) == 0)
		obj_tx_abort(errnum, 0);
	errno = errnum;
	return 0;
}

/* ASSERT_IN_TX -- checks whether there's open transaction */
#define ASSERT_IN_TX(tx) do {\
	if ((tx)->stage == DAV_TX_STAGE_NONE)\
		FATAL("%s called outside of transaction", __func__);\
} while (0)

/* ASSERT_TX_STAGE_WORK -- checks whether current transaction stage is WORK */
#define ASSERT_TX_STAGE_WORK(tx) do {\
	if ((tx)->stage != DAV_TX_STAGE_WORK)\
		FATAL("%s called in invalid stage %d", __func__, (tx)->stage);\
} while (0)

/*
 * tx_action_reserve -- (internal) reserve space for the given number of actions
 */
static int
tx_action_reserve(struct tx *tx, size_t n)
{
	size_t entries_size = (VEC_SIZE(&tx->actions) + n) *
		sizeof(struct ulog_entry_val);

	if (operation_reserve(tx->pop->external, entries_size) != 0)
		return -1;

	return 0;
}

/*
 * tx_action_add -- (internal) reserve space and add a new tx action
 */
static struct dav_action *
tx_action_add(struct tx *tx)
{
	if (tx_action_reserve(tx, 1) != 0)
		return NULL;

	VEC_INC_BACK(&tx->actions);

	return &VEC_BACK(&tx->actions);
}

/*
 * tx_action_remove -- (internal) remove last tx action
 */
static void
tx_action_remove(struct tx *tx)
{
	VEC_POP_BACK(&tx->actions);
}

/*
 * constructor_tx_alloc -- (internal) constructor for normal alloc
 */
static int
constructor_tx_alloc(void *ctx, void *ptr, size_t usable_size, void *arg)
{
	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct tx_alloc_args *args = arg;

	/* do not report changes to the new object */
	VALGRIND_ADD_TO_TX(ptr, usable_size);

	if (args->flags & DAV_FLAG_ZERO)
		memset(ptr, 0, usable_size);

	if (args->copy_ptr && args->copy_size != 0) {
		FATAL("dav xalloc does not support copy_ptr\n");
		memcpy(ptr, args->copy_ptr, args->copy_size);
	}

	return 0;
}

/*
 * tx_restore_range -- (internal) restore a single range from undo log
 */
static void
tx_restore_range(dav_obj_t *pop, struct ulog_entry_buf *range)
{
	void *begin, *end;
	size_t size = range->size;
	uint64_t range_offset = ulog_entry_offset(&range->base);

	begin = OBJ_OFF_TO_PTR(pop, range_offset);
	end = (char *)begin + size;
	ASSERT((char *)end >= (char *)begin);

	memcpy(begin, range->data, size);
}

/*
 * tx_undo_entry_apply -- applies modifications of a single ulog entry
 */
static int
tx_undo_entry_apply(struct ulog_entry_base *e, void *arg,
		    const struct mo_ops *p_ops)
{
	/* suppress unused-parameter errors */
	SUPPRESS_UNUSED(arg);

	struct ulog_entry_buf *eb;

	switch (ulog_entry_type(e)) {
	case ULOG_OPERATION_BUF_CPY:
		eb = (struct ulog_entry_buf *)e;

		tx_restore_range(p_ops->base, eb);
		break;
#ifdef	WAL_SUPPORTS_AND_OR_OPS
	case ULOG_OPERATION_AND:
	case ULOG_OPERATION_OR:
#else
	case ULOG_OPERATION_CLR_BITS:
	case ULOG_OPERATION_SET_BITS:
#endif
	case ULOG_OPERATION_SET:
	case ULOG_OPERATION_BUF_SET:
	default:
		ASSERT(0);
	}

	return 0;
}

/*
 * tx_abort_set -- (internal) abort all set operations
 */
static void
tx_abort_set(dav_obj_t *pop)
{
	ulog_foreach_entry((struct ulog *)&pop->clogs.undo,
		tx_undo_entry_apply, NULL, &pop->p_ops);
	operation_finish(pop->undo, ULOG_INC_FIRST_GEN_NUM);
}

/*
 * tx_flush_range -- (internal) flush one range
 */
static void
tx_flush_range(void *data, void *ctx)
{
	dav_obj_t *pop = ctx;
	struct tx_range_def *range = data;

	if (!(range->flags & DAV_FLAG_NO_FLUSH)) {
		mo_wal_flush(&pop->p_ops, OBJ_OFF_TO_PTR(pop, range->offset),
			     range->size, range->flags & DAV_XADD_WAL_CPTR);
	}
	VALGRIND_REMOVE_FROM_TX(OBJ_OFF_TO_PTR(pop, range->offset),
				range->size);
}

/*
 * tx_clean_range -- (internal) clean one range
 */
static void
tx_clean_range(void *data, void *ctx)
{
	dav_obj_t *pop = ctx;
	struct tx_range_def *range = data;

	VALGRIND_REMOVE_FROM_TX(OBJ_OFF_TO_PTR(pop, range->offset),
		range->size);
	VALGRIND_SET_CLEAN(OBJ_OFF_TO_PTR(pop, range->offset), range->size);
}

/*
 * tx_pre_commit -- (internal) do pre-commit operations
 */
//Yuanguo:
//  - 对每个 tx 的修改的 range (tx->ranges)，生成一个 redo log entry，并链到 struct dav_tx wt_redo 链表尾
//  - free tx->ranges
static void
tx_pre_commit(struct tx *tx)
{
	/* Flush all regions and destroy the whole tree. */
	ravl_delete_cb(tx->ranges, tx_flush_range, tx->pop);
	tx->ranges = NULL;
}

/*
 * tx_abort -- (internal) abort all allocated objects
 */
static void
tx_abort(dav_obj_t *pop)
{
	struct tx *tx = get_tx();

	tx_abort_set(pop);

	ravl_delete_cb(tx->ranges, tx_clean_range, pop);
	palloc_cancel(pop->do_heap,
		VEC_ARR(&tx->actions), VEC_SIZE(&tx->actions));
	tx->ranges = NULL;
}

/*
 * tx_ranges_insert_def -- (internal) allocates and inserts a new range
 *	definition into the ranges tree
 */
static int
tx_ranges_insert_def(dav_obj_t *pop, struct tx *tx,
	const struct tx_range_def *rdef)
{
	/* suppress unused-parameter errors */
	SUPPRESS_UNUSED(pop);

	DAV_DBG("(%lu,%lu) size=%zu",
		rdef->offset / 4096, rdef->offset % 4096, rdef->size);

	int ret = ravl_emplace_copy(tx->ranges, rdef);

	if (ret && errno == EEXIST)
		FATAL("invalid state of ranges tree");
	return ret;
}

/*
 * tx_alloc_common -- (internal) common function for alloc and zalloc
 */
static uint64_t
tx_alloc_common(struct tx *tx, size_t size, type_num_t type_num,
		palloc_constr constructor, struct tx_alloc_args args)
{
	const struct tx_range_def *r;
	uint64_t off;

	if (size > DAV_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		return obj_tx_fail_null(ENOMEM, args.flags);
	}

	dav_obj_t *pop = tx->pop;

	struct dav_action *action = tx_action_add(tx);

	if (action == NULL)
		return obj_tx_fail_null(ENOMEM, args.flags);

	if (palloc_reserve(pop->do_heap, size, constructor, &args, type_num, 0,
		CLASS_ID_FROM_FLAG(args.flags),
		ARENA_ID_FROM_FLAG(args.flags), action) != 0)
		goto err_oom;

	palloc_get_prange(action, &off, &size, 1);
	r = &(struct tx_range_def){off, size, args.flags};
	if (tx_ranges_insert_def(pop, tx, r) != 0)
		goto err_oom;

	return action->heap.offset;

err_oom:
	tx_action_remove(tx);
	D_CRIT("out of memory\n");
	return obj_tx_fail_null(ENOMEM, args.flags);
}

/*
 * tx_create_wal_entry -- convert to WAL a single ulog UNDO entry
 */
int
tx_create_wal_entry(struct ulog_entry_base *e, void *arg,
		    const struct mo_ops *p_ops)
{
	/* suppress unused-parameter errors */
	SUPPRESS_UNUSED(arg);

	int			 rc = 0;
	uint64_t		 offset = ulog_entry_offset(e);
	daos_size_t		 dst_size = sizeof(uint64_t);
	struct ulog_entry_val	*ev;
	struct ulog_entry_buf	*eb;
	uint64_t		 v;
	uint64_t		*dst;

	D_ASSERT(p_ops->base != NULL);
	dst = (uint64_t *)((uintptr_t)((dav_obj_t *)p_ops->base)->do_base + offset);

	switch (ulog_entry_type(e)) {
#ifdef	WAL_SUPPORTS_AND_OR_OPS
	case ULOG_OPERATION_AND:
		ev = (struct ulog_entry_val *)e;
		v = ev->value;

		rc = dav_wal_tx_and(p_ops->base, dst, v);
		break;
	case ULOG_OPERATION_OR:
		ev = (struct ulog_entry_val *)e;
		v = ev->value;

		rc = dav_wal_tx_or(p_ops->base, dst, v);
		break;
#else
	case ULOG_OPERATION_CLR_BITS:
		ev = (struct ulog_entry_val *)e;
		v = ev->value;

		rc = dav_wal_tx_clr_bits(p_ops->base, dst, ULOG_ENTRY_VAL_TO_POS(v),
					 ULOG_ENTRY_VAL_TO_BITS(v));
		break;
	case ULOG_OPERATION_SET_BITS:
		ev = (struct ulog_entry_val *)e;
		v = ev->value;

		rc = dav_wal_tx_set_bits(p_ops->base, dst, ULOG_ENTRY_VAL_TO_POS(v),
					 ULOG_ENTRY_VAL_TO_BITS(v));
		break;
#endif
	case ULOG_OPERATION_SET:
		ev = (struct ulog_entry_val *)e;

		rc = dav_wal_tx_snap(p_ops->base, dst, dst_size, (void *)&ev->value, 0);
		break;
	case ULOG_OPERATION_BUF_SET:
		eb = (struct ulog_entry_buf *)e;

		dst_size = eb->size;
		rc = dav_wal_tx_set(p_ops->base, dst, 0, dst_size);
		break;
	case ULOG_OPERATION_BUF_CPY:
		eb = (struct ulog_entry_buf *)e;

		dst_size = eb->size;
		/* The only undo entry from dav that needs to be
		 * transformed into redo
		 */
		rc = dav_wal_tx_snap(p_ops->base, dst, dst_size, dst, 0);
		break;
	default:
		ASSERT(0);
	}

	return rc;
}

int
lw_tx_begin(dav_obj_t *pop)
{
	struct umem_wal_tx	*utx = NULL;
	int			 rc;
	uint64_t		 wal_id;

    //Yuanguo: 预留一个transaction id
	rc = dav_wal_tx_reserve(pop, &wal_id);
	if (rc) {
		D_ERROR("so_wal_reserv failed, "DF_RC"\n", DP_RC(rc));
		return rc;
	}
	if (pop->do_utx == NULL) {
        //Yuanguo: 初始化transaction
		utx = dav_umem_wtx_new(pop);
		if (utx == NULL) {
			D_ERROR("dav_umem_wtx_new failed\n");
			return ENOMEM;
		}
	}
	pop->do_utx->utx_id = wal_id;
	return rc;
}

int
lw_tx_end(dav_obj_t *pop, void *data)
{
	struct umem_wal_tx	*utx;
	int			 rc;

	/* Persist the frequently updated persistent globals */
	stats_persist(pop, pop->do_stats);

	utx = pop->do_utx;
	D_ASSERT(utx != NULL);
	pop->do_utx = NULL;

	rc = dav_wal_tx_commit(pop, utx, data);
	D_FREE(utx);
	return rc;
}

/*
 * dav_tx_begin -- initializes new transaction
 */
int
dav_tx_begin(dav_obj_t *pop, jmp_buf env, ...)
{
	int		 err = 0;
	struct tx	*tx = get_tx();
	uint64_t	 wal_id;

	enum dav_tx_failure_behavior failure_behavior = DAV_TX_FAILURE_ABORT;

	if (tx->stage == DAV_TX_STAGE_WORK) {
		//Yuanguo: nested transaction；已在事务中, 再开一层
		//   - 不重新预留 WAL ID
		//   - 不重新分配 clogs / ranges / actions
		//   - 只继承 parent 的 failure_behavior
		//   - 只压入一个 tx_data 到 tx_entries 链表 (引用计数)
		if (tx->pop != pop) {
			ERR("nested transaction for different pool");
			return obj_tx_fail_err(EINVAL, 0);
		}

		/* inherits this value from the parent transaction */
		struct tx_data *txd = DAV_SLIST_FIRST(&tx->tx_entries);

		failure_behavior = txd->failure_behavior;

		VALGRIND_START_TX;
	} else if (tx->stage == DAV_TX_STAGE_NONE) {
		//Yuanguo: 非 nested transaction，即最外层 transaction.
		//   - 预留 cache page, WAL ID
		//   - 分配 umem_wal_tx, clogs, ranges, actions
		//   - 完整初始化
		struct umem_wal_tx *utx = NULL;

		DAV_DBG("");
		//Yuanguo: 预留transaction id；实际上是在wal (wal blob on NVMe)上预留位置，
		//  因为transaction id对应着wal上的一个位置；
		err = dav_wal_tx_reserve(pop, &wal_id);
		if (err) {
			D_ERROR("so_wal_reserv failed, "DF_RC"\n", DP_RC(err));
			goto err_abort;
		}

		//Yuanguo: dav_tx_end() --> lw_tx_end() 会把 pop->do_utx 释放并置 NULL.
		//  所以正常情况下，最外层 transaction begin 时 do_utx 应该总是 NULL;
		//  这个判断，可能是只是一个防御性代码，或者历史遗留。证据：
		//      - dav_umem_wtx_new() 中有 D_ASSERT(dav_hdl->do_utx == NULL);
		//      - 那个 D_ASSERT 和这个 if 判断 是重复的；
		//  忽略这个这判断，认为 pop->do_utx 总是 NULL !
		if (pop->do_utx == NULL) {
			utx = dav_umem_wtx_new(pop);
			if (utx == NULL) {
				err = ENOMEM;
				goto err_abort;
			}
		}
		//Yuanguo: 初始化transaction的id；
		pop->do_utx->utx_id = wal_id;

		//Yuanguo: 一个vos，即一个xstream/线程，有一个thread local的struct tx对象；
		//
		//  struct tx 和 struct umem_wal_tx (dav_umem_wtx_new分配的) 有什么关系？
		//
		//   |----------|------------------------------------|---------------------------------------------------------------------|
		//   |          | struct tx                          | struct umem_wal_tx                                                  |
		//   |----------|------------------------------------|---------------------------------------------------------------------|
		//   | 生命周期 | thread-local 永驻,跨事务复用       | 每个最外层transaction分配/释放                                      |
		//   |----------|------------------------------------|---------------------------------------------------------------------|
		//   | 管什么   | 事务逻辑状态: stage, ranges(修改区 | WAL 数据：utx_id、wt_redo 链表(redo actions)、迭代redo logs的函数指 |
		//   |          | 域), actions(deferred alloc/free), | 针(BIO 的 bio_wal_commit 不知道 redo 怎么组织，它只通过这四个函数指 |
		//   |          | callbacks                          | 针迭代所有 redo action，序列化后写入 NVMe WAL blob)                 |
		//   |----------|------------------------------------|---------------------------------------------------------------------|
		//   | 谁用它   | DAV 内部 (commit/abort 决策)       | BIO WAL 层 (通过 utx_ops 迭代 redo 落盘)                            |
		//   |----------|------------------------------------|---------------------------------------------------------------------|
		//   | 存在哪   | 栈上 static __thread               | 堆上 D_ALLOC，挂在 pop->do_utx                                      |
		//   |----------|------------------------------------|---------------------------------------------------------------------|
		//   | 一句话   | 事务做了什么改动；事务状态         | 这些改动怎么变成 WAL redo 交给 BIO                                  |
		//   |----------|------------------------------------|---------------------------------------------------------------------|

		tx = get_tx();

		VALGRIND_START_TX;

		dav_hold_clogs(pop);
		operation_start(pop->undo);

		VEC_INIT(&tx->actions);
		DAV_SLIST_INIT(&tx->tx_entries);

		//Yuanguo:
		//  使用一个avl-tree保存current transaction修改的heap ranges:
		//      - 辅助生成 redo logs (持久化到 NVME wal blob)，见
		//          tx_pre_commit ->
		//          ravl_delete_cb ->
		//          tx_flush_range ->
		//          mo_wal_flush ->
		//          dav_wal_tx_snap
		//      - 辅助生成 undo logs (DRAM 中，用于abort 回滚，不用持久化)
		//          - undo logs 就是这些 ranges 的snapshot，用于在transaction abort时回滚；
		//          - 详细逻辑见 dav_tx_add_common() 函数;
		tx->ranges = ravl_new_sized(tx_range_def_cmp,
			sizeof(struct tx_range_def));
		tx->first_snapshot = 1;
		tx->pop = pop;
	} else {
		FATAL("Invalid stage %d to begin new transaction", tx->stage);
	}

	struct tx_data *txd;

	D_ALLOC_PTR_NZ(txd);
	if (txd == NULL) {
		err = errno;
		D_CRIT("Malloc!\n");
		goto err_abort;
	}

	tx->last_errnum = 0;
	ASSERT(env == NULL);
	if (env != NULL)
		memcpy(txd->env, env, sizeof(jmp_buf));
	else
		memset(txd->env, 0, sizeof(jmp_buf));

	txd->failure_behavior = failure_behavior;

	DAV_SLIST_INSERT_HEAD(&tx->tx_entries, txd, tx_entry);

	tx->stage = DAV_TX_STAGE_WORK;

	/* handle locks */
	va_list argp;

	va_start(argp, env);

	enum dav_tx_param param_type;

	while ((param_type = va_arg(argp, enum dav_tx_param)) !=
			DAV_TX_PARAM_NONE) {
		if (param_type == DAV_TX_PARAM_CB) {
			dav_tx_callback cb =
					va_arg(argp, dav_tx_callback);
			void *arg = va_arg(argp, void *);

			if (tx->stage_callback &&
					(tx->stage_callback != cb ||
					tx->stage_callback_arg != arg)) {
				FATAL(
			 "transaction callback is already set, old %p new %p old_arg %p new_arg %p",
					tx->stage_callback, cb,
					tx->stage_callback_arg, arg);
			}

			tx->stage_callback = cb;
			tx->stage_callback_arg = arg;
		} else {
			ASSERT(param_type == DAV_TX_PARAM_CB);
		}
	}
	va_end(argp);

	ASSERT(err == 0);
	return 0;

err_abort:
	if (tx->stage == DAV_TX_STAGE_WORK)
		obj_tx_abort(err, 0);
	else
		tx->stage = DAV_TX_STAGE_ONABORT;
	return err;
}

/*
 * tx_abort_on_failure_flag -- (internal) return 0 or DAV_FLAG_TX_NO_ABORT
 * based on transaction setting
 */
static uint64_t
tx_abort_on_failure_flag(struct tx *tx)
{
	struct tx_data *txd = DAV_SLIST_FIRST(&tx->tx_entries);

	if (txd->failure_behavior == DAV_TX_FAILURE_RETURN)
		return DAV_FLAG_TX_NO_ABORT;
	return 0;
}

/*
 * obj_tx_callback -- (internal) executes callback associated with current stage
 */
static void
obj_tx_callback(struct tx *tx)
{
	if (!tx->stage_callback)
		return;

	struct tx_data *txd = DAV_SLIST_FIRST(&tx->tx_entries);

	/* is this the outermost transaction? */
	if (DAV_SLIST_NEXT(txd, tx_entry) == NULL)
		tx->stage_callback(tx->pop, tx->stage, tx->stage_callback_arg);
}

/*
 * dav_tx_stage -- returns current transaction stage
 */
enum dav_tx_stage
dav_tx_stage(void)
{
	return get_tx()->stage;
}

/*
 * obj_tx_abort -- aborts current transaction
 */
static void
obj_tx_abort(int errnum, int user)
{
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);
	ASSERT(tx->pop != NULL);

	if (errnum == 0)
		errnum = ECANCELED;

	tx->stage = DAV_TX_STAGE_ONABORT;
	struct tx_data *txd = DAV_SLIST_FIRST(&tx->tx_entries);

	if (DAV_SLIST_NEXT(txd, tx_entry) == NULL) {
		/* this is the outermost transaction */

		/* process the undo log */
		tx_abort(tx->pop);

		dav_release_clogs(tx->pop);
	}

	tx->last_errnum = errnum;
	errno = errnum;
	if (user) {
		DAV_DBG("!explicit transaction abort");
	}

	/* ONABORT */
	obj_tx_callback(tx);

	if (!util_is_zeroed(txd->env, sizeof(jmp_buf)))
		longjmp(txd->env, errnum);
}

/*
 * dav_tx_abort -- aborts current transaction
 *
 * Note: this function should not be called from inside of dav.
 */
void
dav_tx_abort(int errnum)
{
	DAV_API_START();
	DAV_DBG("");
	obj_tx_abort(errnum, 1);
	DAV_API_END();
}

/*
 * dav_tx_errno -- returns last transaction error code
 */
int
dav_tx_errno(void)
{
	DAV_DBG("err:%d", get_tx()->last_errnum);

	return get_tx()->last_errnum;
}

static void
tx_post_commit(struct tx *tx)
{
	operation_finish(tx->pop->undo, 0);
}

/*
 * dav_tx_commit -- commits current transaction
 */
void
dav_tx_commit(void)
{
	DAV_API_START();
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);
	ASSERT(tx->pop);
	DAV_DBG("");

	/* WORK */
	obj_tx_callback(tx);
	dav_obj_t *pop = tx->pop;

	struct tx_data *txd = DAV_SLIST_FIRST(&tx->tx_entries);

    //Yuanguo: 见`struct tx`前的注释，tx_entries链表中，越内层的transaction越靠前，所以最外层transaction
    //    的tx_entry的next为NULL；
	if (DAV_SLIST_NEXT(txd, tx_entry) == NULL) {
		/* this is the outermost transaction */

		/* pre-commit phase */
		//Yuanguo:
		//  - 对每个 tx 的修改的 range (tx->ranges)，生成一个 redo log entry，并链到 struct dav_tx wt_redo 链表尾
		//  - free tx->ranges
		tx_pre_commit(tx);

		mo_wal_drain(&pop->p_ops);

		//Yuanguo:
		//  - 见 struct dav_clogs 的 external 的注释，非常重要！！！
		//  - pop->external->ulog 指向 struct dav_clogs 的 external；见 dav_create_clogs()
		//这里是初始化，为下面 palloc_publish 把 struct tx actions 转换成 external log 做准备
		operation_start(pop->external);

		palloc_publish(pop->do_heap, VEC_ARR(&tx->actions),
			       VEC_SIZE(&tx->actions), pop->external);

		tx_post_commit(tx);

		dav_release_clogs(pop);
	}

	tx->stage = DAV_TX_STAGE_ONCOMMIT;

	/* ONCOMMIT */
	obj_tx_callback(tx);
	DAV_API_END();
}

/*
 * dav_tx_end -- ends current transaction
 */
int
dav_tx_end(void *data)
{
	struct tx *tx = get_tx();

	if (tx->stage == DAV_TX_STAGE_WORK)
		FATAL("dav_tx_end called without dav_tx_commit");

	if (tx->pop == NULL)
		FATAL("dav_tx_end called without dav_tx_begin");

	if (tx->stage_callback &&
			(tx->stage == DAV_TX_STAGE_ONCOMMIT ||
			 tx->stage == DAV_TX_STAGE_ONABORT)) {
		tx->stage = DAV_TX_STAGE_FINALLY;
		obj_tx_callback(tx);
	}

	struct tx_data *txd = DAV_SLIST_FIRST(&tx->tx_entries);

	DAV_SLIST_REMOVE_HEAD(&tx->tx_entries, tx_entry);

	D_FREE(txd);

	VALGRIND_END_TX;
	int ret = tx->last_errnum;

	if (DAV_SLIST_EMPTY(&tx->tx_entries)) {
		dav_obj_t *pop = tx->pop;
		dav_tx_callback cb = tx->stage_callback;
		void *arg = tx->stage_callback_arg;
		int rc;

		DAV_DBG("");
		ASSERT(pop);
		tx->pop = NULL;
		tx->stage = DAV_TX_STAGE_NONE;
		tx->stage_callback = NULL;
		tx->stage_callback_arg = NULL;

		VEC_DELETE(&tx->actions);
		/* tx should not be accessed after this */

		/* commit to WAL */
		rc = lw_tx_end(pop, data);
		/* TODO: Handle WAL commit errors */
		D_ASSERT(rc == 0);

		if (cb) {
			cb(pop, DAV_TX_STAGE_NONE, arg);
		}
	} else {
		/* resume the next transaction */
		tx->stage = DAV_TX_STAGE_WORK;

		/* abort called within inner transaction, waterfall the error */
		if (tx->last_errnum)
			obj_tx_abort(tx->last_errnum, 0);
	}

	return ret;
}

/*
 * vg_verify_initialized -- when executed under Valgrind verifies that
 *   the buffer has been initialized; explicit check at snapshotting time,
 *   because Valgrind may find it much later when it's impossible to tell
 *   for which snapshot it triggered
 */
static void
vg_verify_initialized(dav_obj_t *pop, const struct tx_range_def *def)
{
	/* suppress unused-parameter errors */
	SUPPRESS_UNUSED(pop, def);
#if VG_MEMCHECK_ENABLED
	if (!On_memcheck)
		return;

	VALGRIND_DO_DISABLE_ERROR_REPORTING;
	char *start = OBJ_OFF_TO_PTR(pop, def->offset);
	char *uninit = (char *)VALGRIND_CHECK_MEM_IS_DEFINED(start, def->size);

	if (uninit) {
		VALGRIND_PRINTF(
			"Snapshotting uninitialized data in range <%p,%p> (<offset:0x%lx,size:0x%lx>)\n",
			start, start + def->size, def->offset, def->size);

		if (uninit != start)
			VALGRIND_PRINTF("Uninitialized data starts at: %p\n",
					uninit);

		VALGRIND_DO_ENABLE_ERROR_REPORTING;
		VALGRIND_CHECK_MEM_IS_DEFINED(start, def->size);
	} else {
		VALGRIND_DO_ENABLE_ERROR_REPORTING;
	}
#endif
}

/*
 * dav_tx_add_snapshot -- (internal) creates a variably sized snapshot
 */
//Yuanguo:
//  参数snapshot: 要被存入 undo log 的区间
static int
dav_tx_add_snapshot(struct tx *tx, struct tx_range_def *snapshot)
{
	/*
	 * Depending on the size of the block, either allocate an
	 * entire new object or use cache.
	 */
	void *ptr = OBJ_OFF_TO_PTR(tx->pop, snapshot->offset);

	VALGRIND_ADD_TO_TX(ptr, snapshot->size);

	/* do nothing */
	if (snapshot->flags & DAV_XADD_NO_SNAPSHOT)
		return 0;

	if (!(snapshot->flags & DAV_XADD_ASSUME_INITIALIZED))
		vg_verify_initialized(tx->pop, snapshot);

	/*
	 * If we are creating the first snapshot, setup a redo log action to
	 * increment counter in the undo log, so that the log becomes
	 * invalid once the redo log is processed.
	 */
	if (tx->first_snapshot) {
		struct dav_action *action = tx_action_add(tx);

		if (action == NULL)
			return -1;

		uint64_t *n = &tx->pop->clogs.undo.gen_num;

		palloc_set_value(tx->pop->do_heap, action,
			n, *n + 1);

		tx->first_snapshot = 0;
	}

	return operation_add_buffer(tx->pop->undo, ptr, ptr, snapshot->size,
		ULOG_OPERATION_BUF_CPY);
}

/*
 * dav_tx_merge_flags -- (internal) common code for merging flags between
 * two ranges to ensure resultant behavior is correct
 */
static void
dav_tx_merge_flags(struct tx_range_def *dest, struct tx_range_def *merged)
{
	/*
	 * DAV_XADD_NO_FLUSH should only be set in merged range if set in
	 * both ranges
	 */
	if ((dest->flags & DAV_XADD_NO_FLUSH) &&
				!(merged->flags & DAV_XADD_NO_FLUSH)) {
		dest->flags = dest->flags & (~DAV_XADD_NO_FLUSH);
	}

	/*
	 * Extend DAV_XADD_WAL_CPTR when merged.
	 * REVISIT: Ideally merge should happen only if address ranges
	 * overlap. Current code merges adjacent ranges even if only one
	 * of them has this flag set. Fix this before closing DAOS-11049.
	 */
	if (merged->flags & DAV_XADD_WAL_CPTR)
		dest->flags = dest->flags | DAV_XADD_WAL_CPTR;
}

/*
 * dav_tx_add_common -- (internal) common code for adding persistent memory
 * into the transaction
 */
//Yuanguo 注释：
//  使用一个 avl-tree(tx->ranges) 保存 current transaction 修改的 ranges；其作用是**使undo log能被正确地创建**（以及 reodo log能被正确地创建），具体地：
//      - 事务 (tx) 修改内存时，要考虑失败abort场景，到时需要把内存恢复到原始状态，也就是需要 undo log;
//      - undo log 何时创建呢？一个 tx 之中，可能修改同一块内存多次：例如原始状态是"AAAA"，第一次修改成"BBBB"，第二次修改成"CCCC" ......
//        显然，只能在第一次修改的时候，生成undo log，即 AAAA -> BBBB 时，把 AAAA 记入undo log;
//      - 所以，tx 需要记录它已经修改了哪些 ranges，下次修改一个range时，要和已经修改的 ranges 对比，看是不是第一次修改；
//      - 这就是 tx->ranges 的作用：记录tx 已经修改了哪些 ranges；
//
//  注意：
//      1. tx->ranges 本身并不是 undo log，它只是帮助我们正确地创建 undo log.
//             undo log entire 在 tx->pop->undo / tx->pop->clogs.undo 中；
//      2. snapshot 和 undo log 基本上可以认为是同一个东西的两个视角:
//             做一个range的snapshot  =  往 pop->clogs.undo 写一条 BUF_CPY entry（内容 = 该 range 修改前的原始字节），见 operation_add_buffer(..., ULOG_OPERATION_BUF_CPY)
//      3. 本函数只负责 tx 的 undo log
//
//Yuanguo 引用 AI 注释：
//  对新增的内存范围与已有快照范围进行合并/去重，避免重复快照，并在必要时创建 undo 日志条目，用于 abort 恢复
static int
dav_tx_add_common(struct tx *tx, struct tx_range_def *args)
{
	if (args->size > DAV_MAX_ALLOC_SIZE) {
		ERR("snapshot size too large");
		return obj_tx_fail_err(EINVAL, args->flags);
	}

	if (!OBJ_OFFRANGE_FROM_HEAP(tx->pop, args->offset, (args->offset + args->size))) {
		ERR("object outside of heap");
		return obj_tx_fail_err(EINVAL, args->flags);
	}

	int ret = 0;

	/*
	 * Search existing ranges backwards starting from the end of the
	 * snapshot.
	 */
	struct tx_range_def r = *args;

	DAV_DBG("(%lu,%lu) size=%zu", r.offset / 4096, r.offset % 4096, r.size);
	struct tx_range_def search = {0, 0, 0};
	/*
	 * If the range is directly adjacent to an existing one,
	 * they can be merged, so search for less or equal elements.
	 */
	enum ravl_predicate p = RAVL_PREDICATE_LESS_EQUAL;
	struct ravl_node *nprev = NULL;

	while (r.size != 0) {
		//Yuanguo:
		//  将要修改range r: [r.offset, r.offset + r.size)，
		//
		//                r: |======================|
		//                                          ^
		//                                          |
		//                                       search-point
		//
		//  在 search-point 左侧(RAVL_PREDICATE_LESS_EQUAL) 搜索最接近 search-point 的 range；可能的结果:
		//
		//      - Case1: 无重叠
		//
		//                        r: |======================|
		//              f: |======|
		//              f: null
		//
		//      - Case2:
		//
		//                        r: |======================|
		//              f: |======================|
		//              f:               |========|
		//
		//      - Case3:
		//
		//                        r: |----------------------|
		//              f:    |===================================|
		//              f:                   |====================|
		//
		//  循环可以看作递归，Case1 是递归出口。
		search.offset = r.offset + r.size;
		struct ravl_node *n = ravl_find(tx->ranges, &search, p);
		/*
		 * We have to skip searching for LESS_EQUAL because
		 * the snapshot we would find is the one that was just
		 * created.
		 */
		p = RAVL_PREDICATE_LESS;

		struct tx_range_def *f = n ? ravl_data(n) : NULL;

		size_t fend = f == NULL ? 0 : f->offset + f->size;
		size_t rend = r.offset + r.size;

		if (fend == 0 || fend < r.offset) {
			//Yuanguo: Case1 无重叠
			//
			//                        r: |======================|
			//              f: |======|
			//              f: null

			/*
			 * If found no range or the found range is not
			 * overlapping or adjacent on the left side, we can just
			 * create the entire r.offset + r.size snapshot.
			 *
			 * Snapshot:
			 *	--+-
			 * Existing ranges:
			 *	---- (no ranges)
			 * or	+--- (no overlap)
			 * or	---+ (adjacent on on right side)
			 */

			//Yuanguo:
			//   假如第一次循环，就是Case1，nprev==NULL；
			//   假如经过Case2之后，下一轮循环立即进入Case1，则 nprev != NULL；见Case2的注释；
			//   假如经过Case3之后，下一轮循环立即进入Case1，则 nprev != NULL；见Case3的注释；
			if (nprev != NULL) {
				/*
				 * But, if we have an existing adjacent snapshot
				 * on the right side, we can just extend it to
				 * include the desired range.
				 */
				struct tx_range_def *fprev = ravl_data(nprev);

				ASSERTeq(rend, fprev->offset);
				fprev->offset -= r.size;
				fprev->size += r.size;
			} else {
				/*
				 * If we don't have anything adjacent, create
				 * a new range in the tree.
				 */
				ret = tx_ranges_insert_def(tx->pop,
					tx, &r);
				if (ret != 0)
					break;
			}
			ret = dav_tx_add_snapshot(tx, &r);
			//Yuanguo: 递归出口
			break;
		} else if (fend <= rend) {
			//Yuanguo: Case2 又分两种情况
			//
			//   Case2-A:
			//
			//               r: |==============================|
			//           f: |=======================|
			//                  |    intersection   | snapshot |
			//
			//                  r = empty 因为 r.size -= (intersection + snapshot.size) 之后为0，即 r 除去 intersection部分 和 snapshot部分 之后，就没有了；
			//                               intersection部分：已经snap；
			//                               snapshot部分：本次要snap；
			//                  f = f 并 r
			//                  snapshot添加进去，while结束(r.size为0)
			//
			//   Case2-B:
			//
			//               r: |==============================|
			//                   f:  |==============|
			//                       | intersection | snapshot |
			//                  | s  |
			//
			//                  r = s;    因为 r.size -= (intersection + snapshot.size) 之后剩s，即 r 除去 intersection部分 和 snapshot部分 之后，还剩下s需要继续处理；
			//                               intersection部分：已经snap；
			//                               snapshot部分：本次要snap；
			//                  f = f 并 snapshot
			//
			//                  snapshot添加进去，while下一次循环，处理r=s部分：进入新一轮循环（递归）
			//                      - 还可能出现同样的 3 种情况；
			//                      - 但 Case1 略不同：nprev 是 f (即 avl-tree node `n`) 而不是 NULL，所以，s 在 Case1 中，将合并到 f

			/*
			 * If found range has its end inside of the desired
			 * snapshot range, we can extend the found range by the
			 * size leftover on the left side.
			 *
			 * Snapshot:
			 *	--+++--
			 * Existing ranges:
			 *	+++---- (overlap on left)
			 * or	---+--- (found snapshot is inside)
			 * or	---+-++ (inside, and adjacent on the right)
			 * or	+++++-- (desired snapshot is inside)
			 *
			 */
			struct tx_range_def snapshot = *args;

			snapshot.offset = fend;
			/* the side not yet covered by an existing snapshot */
			snapshot.size = rend - fend;

			/* the number of bytes intersecting in both ranges */
			size_t intersection = fend - MAX(f->offset, r.offset);

			r.size -= intersection + snapshot.size;
			//Yuanguo: 在 avl-tree 中，原地修改 f 的尺寸，使其覆盖住 snapshot部分
			f->size += snapshot.size;
			dav_tx_merge_flags(f, args);

			if (snapshot.size != 0) {
				ret = dav_tx_add_snapshot(tx, &snapshot);
				if (ret != 0)
					break;
			}

			/*
			 * If there's a snapshot adjacent on right side, merge
			 * the two ranges together.
			 */
			//Yuanguo: Case2/Case3 之后，下一轮递归又到此，则 nprev != NULL
			if (nprev != NULL) {
				struct tx_range_def *fprev = ravl_data(nprev);

				ASSERTeq(rend, fprev->offset);
				f->size += fprev->size;
				dav_tx_merge_flags(f, fprev);
				ravl_remove(tx->ranges, nprev);
			}
		} else if (fend >= r.offset) {
			//Yuanguo: Case3 也分两种情况
			//
			//   Case3-A:
			//
			//                      r: |====================|
			//             f:  |=======================================|
			//                         |   overlap          |
			//
			//             r = empty  (r.size -= overlap 之后为0)
			//             while结束(r.size为0)
			//             这种情况下，没有添加任何range，因为r完全被f覆盖；
			//
			//   Case3-B:
			//
			//                      r: |====================|
			//                               f: |======================|
			//                                  | overlap   |
			//                         |   s    |
			//
			//             r = s
			//             这种情况下，也没有添加range；
			//             while下一次循环，处理r=s部分：进入新一轮循环（递归）
			//                      - 还可能出现同样的 3 种情况；
			//                      - 但 Case1 略不同：nprev 是 f (即 avl-tree node `n`) 而不是 NULL，所以，s 在 Case1 中，将合并到 f

			/*
			 * If found range has its end extending beyond the
			 * desired snapshot.
			 *
			 * Snapshot:
			 *	--+++--
			 * Existing ranges:
			 *	-----++ (adjacent on the right)
			 * or	----++- (overlapping on the right)
			 * or	----+++ (overlapping and adjacent on the right)
			 * or	--+++++ (desired snapshot is inside)
			 *
			 * Notice that we cannot create a snapshot based solely
			 * on this information without risking overwriting an
			 * existing one. We have to continue iterating, but we
			 * keep the information about adjacent snapshots in the
			 * nprev variable.
			 */
			size_t overlap = rend - MAX(f->offset, r.offset);

			r.size -= overlap;
			dav_tx_merge_flags(f, args);
		} else {
			ASSERT(0);
		}

		nprev = n;
	}

	if (ret != 0) {
		DAV_DBG("out of memory\n");
		return obj_tx_fail_err(ENOMEM, args->flags);
	}

	return 0;
}

/*
 * dav_tx_add_range_direct -- adds persistent memory range into the
 *					transaction
 */
//Yuanguo: 等价于PMem的函数(见pmdk-2.1.0/include/libpmemobj/tx_base.h)
//     int pmemobj_tx_add_range_direct(const void *ptr, size_t size);
// 它的注释是：
//     Takes a "snapshot" of the given memory region and saves it in the undo log.
//     (Yuanguo: region就是参数ptr指向的大小为size的heap区域，在函数中使用struct tx_range_def类型表示)
//     The application is then free to directly modify the object in that memory
//     range.
//     In case of failure or abort, all the changes within this range will
//     be rolled-back automatically.
int
dav_tx_add_range_direct(const void *ptr, size_t size)
{
	DAV_API_START();
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);
	ASSERT(tx->pop != NULL);

	int ret;

	uint64_t flags = tx_abort_on_failure_flag(tx);

	if (!OBJ_PTR_FROM_POOL(tx->pop, ptr)) {
		ERR("object outside of pool");
		ret = obj_tx_fail_err(EINVAL, flags);
		DAV_API_END();
		return ret;
	}

	struct tx_range_def args = {
		.offset = OBJ_PTR_TO_OFF(tx->pop, ptr),
		.size = size,
		.flags = flags,
	};

	ret = dav_tx_add_common(tx, &args);

	DAV_API_END();
	return ret;
}

/*
 * dav_tx_xadd_range_direct -- adds persistent memory range into the
 *					transaction
 */
int
dav_tx_xadd_range_direct(const void *ptr, size_t size, uint64_t flags)
{

	DAV_API_START();
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);

	int ret;
	uint64_t off;

	flags |= tx_abort_on_failure_flag(tx);

	if (flags & ~DAV_XADD_VALID_FLAGS) {
		ERR("unknown flags 0x%" PRIx64, flags
			& ~DAV_XADD_VALID_FLAGS);
		ret = obj_tx_fail_err(EINVAL, flags);
		DAV_API_END();
		return ret;
	}

	if (!OBJ_PTR_FROM_POOL(tx->pop, ptr)) {
		ERR("object outside of pool");
		ret = obj_tx_fail_err(EINVAL, flags);
		DAV_API_END();
		return ret;
	}

	off = OBJ_PTR_TO_OFF(tx->pop, ptr);
	struct tx_range_def args = {
		.offset = off,
		.size = size,
		.flags = flags,
	};

	ret = dav_tx_add_common(tx, &args);

	DAV_API_END();
	return ret;
}

/*
 * dav_tx_add_range -- adds persistent memory range into the transaction
 */
int
dav_tx_add_range(uint64_t hoff, size_t size)
{
	DAV_API_START();
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);

	int ret;

	uint64_t flags = tx_abort_on_failure_flag(tx);

	ASSERT(OBJ_OFF_IS_VALID(tx->pop, hoff));

	struct tx_range_def args = {
		.offset = hoff,
		.size = size,
		.flags = flags,
	};

	ret = dav_tx_add_common(tx, &args);

	DAV_API_END();
	return ret;
}

/*
 * dav_tx_xadd_range -- adds persistent memory range into the transaction
 */
int
dav_tx_xadd_range(uint64_t hoff, size_t size, uint64_t flags)
{
	DAV_API_START();
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);

	int ret;

	flags |= tx_abort_on_failure_flag(tx);

	if (flags & ~DAV_XADD_VALID_FLAGS) {
		ERR("unknown flags 0x%" PRIx64, flags
			& ~DAV_XADD_VALID_FLAGS);
		ret = obj_tx_fail_err(EINVAL, flags);
		DAV_API_END();
		return ret;
	}

	ASSERT(OBJ_OFF_IS_VALID(tx->pop, hoff));

	struct tx_range_def args = {
		.offset = hoff,
		.size = size,
		.flags = flags,
	};

	ret = dav_tx_add_common(tx, &args);

	DAV_API_END();
	return ret;
}

/*
 * dav_tx_alloc -- allocates a new object
 */
uint64_t
dav_tx_alloc(size_t size, uint64_t type_num)
{
	uint64_t off;

	DAV_API_START();
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);

	uint64_t flags = tx_abort_on_failure_flag(tx);

	if (size == 0) {
		ERR("allocation with size 0");
		off = obj_tx_fail_null(EINVAL, flags);
		DAV_API_END();
		return off;
	}

	off = tx_alloc_common(tx, size, (type_num_t)type_num,
			constructor_tx_alloc, ALLOC_ARGS(flags));

	DAV_API_END();
	return off;
}

/*
 * dav_tx_zalloc -- allocates a new zeroed object
 */
uint64_t
dav_tx_zalloc(size_t size, uint64_t type_num)
{
	uint64_t off;
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);

	uint64_t flags = DAV_FLAG_ZERO;

	flags |= tx_abort_on_failure_flag(tx);

	DAV_API_START();
	if (size == 0) {
		ERR("allocation with size 0");
		off = obj_tx_fail_null(EINVAL, flags);
		DAV_API_END();
		return off;
	}

	off = tx_alloc_common(tx, size, (type_num_t)type_num,
			constructor_tx_alloc, ALLOC_ARGS(flags));

	DAV_API_END();
	return off;
}

/*
 * dav_tx_xalloc -- allocates a new object
 */
uint64_t
dav_tx_xalloc(size_t size, uint64_t type_num, uint64_t flags)
{
	uint64_t off;
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);

	flags |= tx_abort_on_failure_flag(tx);

	DAV_API_START();

	if (size == 0) {
		ERR("allocation with size 0");
		off = obj_tx_fail_null(EINVAL, flags);
		DAV_API_END();
		return off;
	}

	if (flags & ~DAV_TX_XALLOC_VALID_FLAGS) {
		ERR("unknown flags 0x%" PRIx64, flags
			& ~(DAV_TX_XALLOC_VALID_FLAGS));
		off = obj_tx_fail_null(EINVAL, flags);
		DAV_API_END();
		return off;
	}

	off = tx_alloc_common(tx, size, (type_num_t)type_num,
			constructor_tx_alloc, ALLOC_ARGS(flags));

	DAV_API_END();
	return off;
}

/*
 * dav_tx_xfree -- frees an existing object, with no_abort option
 */
static int
dav_tx_xfree(uint64_t off, uint64_t flags)
{
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);

	flags |= tx_abort_on_failure_flag(tx);

	if (flags & ~DAV_XFREE_VALID_FLAGS) {
		ERR("unknown flags 0x%" PRIx64,
				flags & ~DAV_XFREE_VALID_FLAGS);
		return obj_tx_fail_err(EINVAL, flags);
	}

	if (off == 0)
		return 0;

	dav_obj_t *pop = tx->pop;

	ASSERT(pop != NULL);
	ASSERT(OBJ_OFF_IS_VALID(pop, off));

	DAV_API_START();

	struct dav_action *action;
	uint64_t roff = palloc_get_realoffset(pop->do_heap, off);

	struct tx_range_def range = {roff, 0, 0};
	struct ravl_node *n = ravl_find(tx->ranges, &range,
			RAVL_PREDICATE_LESS_EQUAL);

	/*
	 * If attempting to free an object allocated within the same
	 * transaction, simply cancel the alloc and remove it from the actions.
	 */
	if (n != NULL) {
		struct tx_range_def *r = ravl_data(n);

		if ((r->offset + r->size) < roff)
			goto out;

		VEC_FOREACH_BY_PTR(action, &tx->actions) {
			if (action->type == DAV_ACTION_TYPE_HEAP &&
			    action->heap.offset == off) {
				void *ptr = OBJ_OFF_TO_PTR(pop, roff);
				uint64_t toff, usize;

				palloc_get_prange(action, &toff, &usize, 1);
				D_ASSERT(usize <= r->size);
				if ((r->offset == roff) && (r->size == usize)) {
					/* Exact match. */
					ravl_remove(tx->ranges, n);
				} else if (r->offset == roff) {
					/* Retain the right portion. */
					r->offset += usize;
					r->size   -= usize;
				} else {
					/* Retain the left portion. */
					uint64_t osize = r->size;

					r->size = roff - r->offset;

					/* Still data after range remove. */
					osize -= (r->size + usize);
					if (osize) {
						struct tx_range_def *r1 =
							&(struct tx_range_def)
							 {roff + usize, osize, r->flags};

						tx_ranges_insert_def(pop, tx, r1);
					}
				}

				VALGRIND_SET_CLEAN(ptr, usize);
				VALGRIND_REMOVE_FROM_TX(ptr, usize);
				palloc_cancel(pop->do_heap, action, 1);
				VEC_ERASE_BY_PTR(&tx->actions, action);
				DAV_API_END();
				return 0;
			}
		}
	}

out:
	action = tx_action_add(tx);
	if (action == NULL) {
		int ret = obj_tx_fail_err(errno, flags);

		DAV_API_END();
		return ret;
	}

	palloc_defer_free(pop->do_heap, off, action);

	DAV_API_END();
	return 0;
}

/*
 * dav_tx_free -- frees an existing object
 */
int
dav_tx_free(uint64_t off)
{
	return dav_tx_xfree(off, 0);
}

void*
dav_tx_off2ptr(uint64_t off)
{
	struct tx *tx = get_tx();

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);
	ASSERT(tx->pop != NULL);

	ASSERT(OBJ_OFF_IS_VALID(tx->pop, off));
	return (void *)OBJ_OFF_TO_PTR(tx->pop, off);
}

/*
 * dav_reserve -- reserves a single object
 */
uint64_t
dav_reserve(dav_obj_t *pop, struct dav_action *act, size_t size, uint64_t type_num)
{
	int tx_inprogress = 0;
	int rc;

	DAV_DBG("pop %p act %p size %zu type_num %llx",
		pop, act, size,
		(unsigned long long)type_num);

	if (get_tx()->stage != DAV_TX_STAGE_NONE)
		tx_inprogress = 1;

	DAV_API_START();
	if (!tx_inprogress) {
		rc = lw_tx_begin(pop);
		if (rc)
			return 0;
	}

	if (palloc_reserve(pop->do_heap, size, NULL, NULL, type_num,
		0, 0, 0, act) != 0) {
		DAV_API_END();
		return 0;
	}

	if (!tx_inprogress)
		lw_tx_end(pop, NULL);
	DAV_API_END();
	return act->heap.offset;
}

/*
 * dav_defer_free -- creates a deferred free action
 */
void
dav_defer_free(dav_obj_t *pop, uint64_t off, struct dav_action *act)
{
	ASSERT(off != 0);
	ASSERT(OBJ_OFF_IS_VALID(pop, off));
	palloc_defer_free(pop->do_heap, off, act);
}

#if	0
/*
 * dav_publish -- publishes a collection of actions
 */
int
dav_publish(dav_obj_t *pop, struct dav_action *actv, size_t actvcnt)
{
	DAV_API_START();
	struct operation_context *ctx = pmalloc_operation_hold(pop);

	size_t entries_size = actvcnt * sizeof(struct ulog_entry_val);

	if (operation_reserve(ctx, entries_size) != 0) {
		DAV_API_END();
		return -1;
	}

	palloc_publish(&pop->do_heap, actv, actvcnt, ctx);

	pmalloc_operation_release(pop);

	DAV_API_END();
	return 0;
}
#endif

/*
 * dav_cancel -- cancels collection of actions
 */
void
dav_cancel(dav_obj_t *pop, struct dav_action *actv, size_t actvcnt)
{
	DAV_DBG("actvcnt=%zu", actvcnt);
	DAV_API_START();
	palloc_cancel(pop->do_heap, actv, actvcnt);
	DAV_API_END();
}


/*
 * dav_tx_publish -- publishes actions inside of a transaction,
 * with no_abort option
 */
int
dav_tx_publish(struct dav_action *actv, size_t actvcnt)
{
	struct tx *tx = get_tx();
	uint64_t flags = 0;
	uint64_t off, size;
	int ret;

	ASSERT_IN_TX(tx);
	ASSERT_TX_STAGE_WORK(tx);

	flags |= tx_abort_on_failure_flag(tx);

	DAV_API_START();

	if (tx_action_reserve(tx, actvcnt) != 0) {
		ret = obj_tx_fail_err(ENOMEM, flags);

		DAV_API_END();
		return ret;
	}

	for (size_t i = 0; i < actvcnt; ++i) {
		VEC_PUSH_BACK(&tx->actions, actv[i]);
		if (palloc_action_isalloc(&actv[i])) {
			palloc_get_prange(&actv[i], &off, &size, 1);
			struct tx_range_def r = {off, size, DAV_XADD_NO_SNAPSHOT|DAV_XADD_WAL_CPTR};

			ret = dav_tx_add_common(tx, &r);
			D_ASSERT(ret == 0);
		}
	}

	DAV_API_END();
	return 0;
}

/* arguments for constructor_alloc */
struct constr_args {
	int zero_init;
	dav_constr constructor;
	void *arg;
};


/* arguments for constructor_alloc_root */
struct carg_root {
	size_t size;
	dav_constr constructor;
	void *arg;
};

/* arguments for constructor_realloc and constructor_zrealloc */
struct carg_realloc {
	void *ptr;
	size_t old_size;
	size_t new_size;
	int zero_init;
	type_num_t user_type;
	dav_constr constructor;
	void *arg;
};

/*
 * constructor_zrealloc_root -- (internal) constructor for dav_root
 */
static int
constructor_zrealloc_root(void *ctx, void *ptr, size_t usable_size, void *arg)
{
	dav_obj_t *pop = ctx;

	DAV_DBG("pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	VALGRIND_ADD_TO_TX(ptr, usable_size);

	struct carg_realloc *carg = arg;

	if (usable_size > carg->old_size) {
		size_t grow_len = usable_size - carg->old_size;
		void *new_data_ptr = (void *)((uintptr_t)ptr + carg->old_size);

		mo_wal_memset(&pop->p_ops, new_data_ptr, 0, grow_len, 0);
	}
	int ret = 0;

	if (carg->constructor)
		ret = carg->constructor(pop, ptr, carg->arg);

	VALGRIND_REMOVE_FROM_TX(ptr, usable_size);

	return ret;
}

/*
 * obj_realloc_root -- (internal) reallocate root object
 */
static int
obj_alloc_root(dav_obj_t *pop, size_t size)
{
	struct operation_context *ctx;
	struct carg_realloc       carg;
	int                       ret;

	DAV_DBG("pop %p size %zu", pop, size);

	carg.ptr = OBJ_OFF_TO_PTR(pop, pop->do_phdr->dp_root_offset);
	carg.old_size = pop->do_phdr->dp_root_size;
	carg.new_size = size;
	carg.user_type = 0;
	carg.constructor = NULL;
	carg.zero_init = 1;
	carg.arg = NULL;

	ret = lw_tx_begin(pop);
	if (ret)
		return ret;
	ctx = pop->external;
	operation_start(ctx);

	operation_add_entry(ctx, &pop->do_phdr->dp_root_size, size, ULOG_OPERATION_SET);

	ret = palloc_operation(pop->do_heap, pop->do_phdr->dp_root_offset,
			&pop->do_phdr->dp_root_offset, size,
			constructor_zrealloc_root, &carg,
			0, 0, 0, 0, ctx); /* REVISIT: object_flags and type num ignored*/

	lw_tx_end(pop, NULL);
	return ret;
}

/*
 * dav_root_construct -- returns root object
 */
uint64_t
dav_root(dav_obj_t *pop, size_t size)
{
	DAV_DBG("pop %p size %zu", pop, size);

	DAV_API_START();
	if (size > DAV_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		DAV_API_END();
		return 0;
	}

	if (size == 0 && pop->do_phdr->dp_root_offset == 0) {
		ERR("requested size cannot equals zero");
		errno = EINVAL;
		DAV_API_END();
		return 0;
	}

	/* REVISIT START
	 * For thread safety the below block has to be protected by lock
	 */
	if (size > pop->do_phdr->dp_root_size &&
			obj_alloc_root(pop, size)) {
		ERR("dav_root failed");
		DAV_API_END();
		return 0;
	}

	/* REVISIT END */

	DAV_API_END();
	return pop->do_phdr->dp_root_offset;
}

/*
 * constructor_alloc -- (internal) constructor for obj_alloc_construct
 */
static int
constructor_alloc(void *ctx, void *ptr, size_t usable_size, void *arg)
{
	dav_obj_t *pop = ctx;

	struct mo_ops *p_ops = &pop->p_ops;

	DAV_DBG("pop %p ptr %p arg %p", pop, ptr, arg);

	ASSERTne(ptr, NULL);
	ASSERTne(arg, NULL);

	struct constr_args *carg = arg;

	if (carg->zero_init)
		mo_wal_memset(p_ops, ptr, 0, usable_size, 0);

	int ret = 0;

	if (carg->constructor)
		ret = carg->constructor(pop, ptr, carg->arg);

	return ret;
}

/*
 * obj_alloc_construct -- (internal) allocates a new object with constructor
 */
static int
obj_alloc_construct(dav_obj_t *pop, uint64_t *offp, size_t size,
	type_num_t type_num, uint64_t flags,
	dav_constr constructor, void *arg)
{
	struct operation_context *ctx;
	struct constr_args        carg;
	int                       ret;

	if (size > DAV_MAX_ALLOC_SIZE) {
		ERR("requested size too large");
		errno = ENOMEM;
		return -1;
	}

	carg.zero_init = flags & DAV_FLAG_ZERO;
	carg.constructor = constructor;
	carg.arg = arg;

	ret = lw_tx_begin(pop);
	if (ret)
		return ret;
	ctx = pop->external;
	operation_start(ctx);

	ret = palloc_operation(pop->do_heap, 0, offp, size, constructor_alloc,
			&carg, type_num, 0, CLASS_ID_FROM_FLAG(flags),
			ARENA_ID_FROM_FLAG(flags), ctx);

	lw_tx_end(pop, NULL);
	return ret;
}

/*
 * dav_alloc -- allocates a new object
 */
int
dav_alloc(dav_obj_t *pop, uint64_t *offp, size_t size,
	  uint64_t type_num, dav_constr constructor, void *arg)
{
	DAV_DBG("pop %p offp %p size %zu type_num %llx constructor %p arg %p",
		pop, offp, size, (unsigned long long)type_num, constructor, arg);

	if (size == 0) {
		ERR("allocation with size 0");
		errno = EINVAL;
		return -1;
	}

	if (offp == NULL) {
		ERR("allocation offp is NULL");
		errno = EINVAL;
		return -1;
	}

	DAV_API_START();
	int ret = obj_alloc_construct(pop, offp, size, type_num,
			0, constructor, arg);

	DAV_API_END();
	return ret;
}

/*
 * dav_free -- frees an existing object
 */
void
dav_free(dav_obj_t *pop, uint64_t off)
{
	struct operation_context *ctx;
	int                       rc;

	DAV_DBG("oid.off 0x%016" PRIx64, off);

	if (off == 0)
		return;

	DAV_API_START();

	ASSERTne(pop, NULL);
	ASSERT(OBJ_OFF_IS_VALID(pop, off));
	rc = lw_tx_begin(pop);
	D_ASSERT(rc == 0);
	ctx = pop->external;
	operation_start(ctx);

	palloc_operation(pop->do_heap, off, NULL, 0, NULL, NULL,
			0, 0, 0, 0, ctx);

	lw_tx_end(pop, NULL);
	DAV_API_END();
}

/*
 * dav_memcpy_persist -- dav version of memcpy
 */
void *
dav_memcpy_persist(dav_obj_t *pop, void *dest, const void *src,
	size_t len)
{
	int rc;

	DAV_DBG("pop %p dest %p src %p len %zu", pop, dest, src, len);
	D_ASSERT((dav_tx_stage() == DAV_TX_STAGE_NONE));

	DAV_API_START();
	rc = lw_tx_begin(pop);
	D_ASSERT(rc == 0);

	void *ptr = mo_wal_memcpy(&pop->p_ops, dest, src, len, 0);

	lw_tx_end(pop, NULL);
	DAV_API_END();
	return ptr;
}

/*
 * dav_memcpy_persist -- dav version of memcpy with deferrred commit to blob.
 */
void *
dav_memcpy_persist_relaxed(dav_obj_t *pop, void *dest, const void *src,
			   size_t len)
{
	DAV_DBG("pop %p dest %p src %p len %zu", pop, dest, src, len);
	DAV_API_START();
	if (pop->do_utx == NULL && dav_umem_wtx_new(pop) == NULL)
		return 0;

	void *ptr = mo_wal_memcpy(&pop->p_ops, dest, src, len, 0);

	DAV_API_END();
	return ptr;
}
