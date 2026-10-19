/**
 * (C) Copyright 2015-2024 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * DAOS Types and Functions Common to Layers/Components
 */

#ifndef DAOS_TYPES_H
#define DAOS_TYPES_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>

/** uuid_t */
#include <uuid/uuid.h>
/** for d_rank_t & d_rank_list_t */
#include <cart/types.h>

#include <daos_errno.h>

/** Maximum length (excluding the '\0') of a DAOS system name */
#define DAOS_SYS_NAME_MAX 15

/** Maximum length (excluding the '\0') of info string info via GetAttachInfo */
#define DAOS_SYS_INFO_STRING_MAX 63

/**
 * Generic data type definition
 */

typedef uint64_t	daos_size_t;
typedef uint64_t	daos_off_t;

#define crt_proc_daos_key_t	crt_proc_d_iov_t
#define crt_proc_daos_size_t	crt_proc_uint64_t
#define crt_proc_daos_epoch_t	crt_proc_uint64_t

/** size of SHA-256 */
#define DAOS_HKEY_MAX	32

typedef enum {
	DAOS_ANCHOR_TYPE_ZERO	= 0,
	DAOS_ANCHOR_TYPE_HKEY	= 1,
	DAOS_ANCHOR_TYPE_KEY	= 2,
	DAOS_ANCHOR_TYPE_EOF	= 3,
} daos_anchor_type_t;

#define DAOS_ANCHOR_BUF_MAX	104
/** Iteration Anchor */
typedef struct {
	uint16_t	da_type;	/**< daos_anchor_type_t */
	uint16_t	da_shard;	/**< internal, do not use */
	uint32_t	da_flags;	/**< see enum daos_anchor_flags */
	uint64_t	da_sub_anchors;	/**< record the offset for each shards for EC object */
	uint8_t		da_buf[DAOS_ANCHOR_BUF_MAX];	/**< internal, do not use */
} daos_anchor_t;

#define DAOS_ANCHOR_INIT	{ .da_type = DAOS_ANCHOR_TYPE_ZERO,	\
				  .da_shard = 0,			\
				  .da_flags = 0,			\
				  .da_sub_anchors = 0,			\
				  .da_buf = { 0 } }

/** Generic handle for various DAOS components like container, object, etc. */
typedef struct {
	/** generic handle value */
	uint64_t	cookie;
} daos_handle_t;

#define DAOS_HDL_INVAL	((daos_handle_t){0})
#define DAOS_TX_NONE	DAOS_HDL_INVAL

static inline bool
daos_handle_is_inval(daos_handle_t hdl)
{
	return hdl.cookie == 0;
}

static inline bool
daos_handle_is_valid(daos_handle_t hdl)
{
	return !daos_handle_is_inval(hdl);
}

/**
 * DAOS_PC_RO connects to the pool for reading only.
 *
 * DAOS_PC_RW connects to the pool for reading and writing.
 *
 * DAOS_PC_EX connects to the pool for reading and writing exclusively.
 *
 * The three flags above are mutually exclusive.
 */
#define DAOS_PC_RO	(1U << 0)
#define DAOS_PC_RW	(1U << 1)
#define DAOS_PC_EX	(1U << 2)

#define DAOS_PC_NBITS	3
#define DAOS_PC_MASK	((1U << DAOS_PC_NBITS) - 1)

/**
 * Epoch
 */
typedef uint64_t	daos_epoch_t;

/**
 * Epoch range
 */
typedef struct {
	/** Low bound of the epoch range */
	daos_epoch_t	epr_lo;
	/** High bound of the epoch range */
	daos_epoch_t	epr_hi;
} daos_epoch_range_t;

/** Highest possible epoch */
#define DAOS_EPOCH_MAX	(~0ULL)

/** Container information */
typedef struct {
	/** Container UUID */
	uuid_t			ci_uuid;
	/** Epoch of latest persistent snapshot */
	daos_epoch_t		ci_lsnapshot;
	/** Number of open handles */
	uint32_t		ci_nhandles;
	/** Number of snapshots */
	uint32_t		ci_nsnapshots;
	/** Latest open time (hybrid logical clock) */
	uint64_t		ci_md_otime;
	/** Latest close/modify time (hybrid logical clock) */
	uint64_t		ci_md_mtime;
	/* TODO: add more members, e.g., size, # objects, uid, gid... */
} daos_cont_info_t;

typedef d_iov_t daos_key_t;

static inline bool
daos_key_is_null(daos_key_t key)
{
	return key.iov_buf_len == 0 || key.iov_buf == NULL;
}

/**
 * Event and event queue
 */

typedef struct daos_event {
	/** return code of non-blocking operation */
	int			ev_error;
	/** Internal use - 152 + 8 bytes pad for pthread_mutex_t size difference on __aarch64__ */
	struct {
		uint64_t	space[20];
	}			ev_private;
	/** Used for debugging */
	uint64_t		ev_debug;
} daos_event_t;

/** Wait for completion event forever */
#define DAOS_EQ_WAIT            -1
/** Always return immediately */
#define DAOS_EQ_NOWAIT          0

typedef enum {
	/** Query outstanding completed event */
	DAOS_EQR_COMPLETED	= (1),
	/** Query # in-flight event */
	DAOS_EQR_WAITING	= (1 << 1),
	/** Query # in-flight + completed events in EQ */
	DAOS_EQR_ALL		= (DAOS_EQR_COMPLETED | DAOS_EQR_WAITING),
} daos_eq_query_t;

typedef enum {
	DAOS_EVS_READY,
	DAOS_EVS_RUNNING,
	DAOS_EVS_COMPLETED,
	DAOS_EVS_ABORTED,
} daos_ev_status_t;

/**
 * Pool target list, each target is identified by rank & target
 * index within the rank
 */
struct d_tgt_list {
	/** array of ranks */
	d_rank_t	*tl_ranks;
	/** array of targets */
	int32_t		*tl_tgts;
	/** number of ranks & tgts */
	uint32_t	tl_nr;
};

struct daos_eq;

/**
 * DAOS Hash Table Handle Types
 * The handle type, uses the least significant 4-bits in the 64-bits hhash key.
 * The bit 0 is only used for D_HYTPE_PTR (pointer type), all other types MUST
 * set bit 0 to 1.
 */
enum {
	DAOS_HTYPE_EQ		= 1, /**< event queue */
	DAOS_HTYPE_POOL		= 3, /**< pool */
	DAOS_HTYPE_CO		= 5, /**< container */
	DAOS_HTYPE_OBJ		= 7, /**< object */
	DAOS_HTYPE_ARRAY	= 9, /**< array */
	DAOS_HTYPE_TX		= 11, /**< transaction */
	DAOS_HTYPE_KV		= 13, /**< KV */
	/* Must enlarge D_HTYPE_BITS to add more types */
};

/**
 * ID of an object, 128 bits
 * The high 32-bit of daos_obj_id_t::hi are reserved for DAOS, the rest is
 * provided by the user and assumed to be unique inside a container.
 *
 * See daos_obj.h for more details
 * It is put here because it's almost used by everyone.
 */
//Yuanguo: 摘自文档
//  The upper 32 bits are used to encode the object type, and key types
//  while the lower 96 bits are a user defined identifier that must be
//  unique to the container.
//
//  oid->hi的低32bit以及oid->lo(64bit)共96bit是用户设置的；
//  DAOS使用oid->hi的高32bit来记录object的一些特性；
//
//     1B        1B           2B                         4B
//  +--------+--------+-----------------+-----------------------------------+
//  |  type  | redun  |     nr_grps     | ############ user filled #########|         daos_obj_id_t::hi
//  +--------+--------+-----------------+-----------------------------------+
//  high-addr                                                        low-addr
//
//                                     8B
//  +-----------------------------------------------------------------------+
//  |############################# user filled #############################|         daos_obj_id_t::lo
//  +-----------------------------------------------------------------------+
//  high-addr                                                        low-addr
//
//  设置，见daos_obj_set_oid()函数；
//
//  - type: 见 enum daos_otype_t 前的注释；
//
//  - redun:
//        - 它只是一个ID，不是"冗余数(redundancy)或者副本数"，例如 OR_RP_3 代表3副本，但它的值是 9 而不是 3；
//        - 取值 OR_RP_1 ~ OR_RS_16P3；
//
//  - nr_grps: 以 OC_RP_3GX 为例。GX 代表: 使用集群中 max groups (扣除预留)；也可以显示指定 group 数，例如 OC_RP_3G8)
//
//    A. when create obj (见 get_target 函数)
//        - 将集群内的所有 target，按 3 个为一组分组，得到所有分组 (groups)，每个 group 内的 3 个target 必须
//          跨 3 个failure domain.（若 5 副本，或者EC 3+2，则以 5 个为一组分组）
//
//          注意："分组"是按 object 动态哈希的，不是全局静态划分。换言之，每个 group 具体落在哪 3 个 target，是针对
//                每个 object、用 jump consistent hash 动态计算的，不同 object 的 group → target 映射不同；
//
//        - 所有分组，扣除一些预留(见reserve_grp_by_rf函数)，剩下的数量就是 nr_grps (number of groups)；被编码进 obj id
//          中，持久化，不可能发生变化。
//
//            例如，nr_grps = 36, grp_size = 3 (3副本)
//            struct dc_object { struct dc_obj_layout { struct dc_obj_shard do_shards } }
//            ┌─────────────────────────────────────────────────────────┐
//            │  struct dc_obj_shard do_shards[108]  (客户端内存)       │
//            │                                                         │
//            │  [0]  do_target_id = target_78  (Group 0, Replica 0)    │
//            │  [1]  do_target_id = target_25  (Group 0, Replica 1)    │
//            │  [2]  do_target_id = target_165 (Group 0, Replica 2)    │
//            │                                                         │
//            │  [3]  do_target_id = target_92  (Group 1, Replica 0)    │
//            │  [4]  do_target_id = target_41  (Group 1, Replica 1)    │
//            │  [5]  do_target_id = target_133 (Group 1, Replica 2)    │
//            │                                                         │
//            │  ...                                                    │
//            │                                                         │
//            │  [105] do_target_id = target_56 (Group 35, Replica 0)   │
//            │  [106] do_target_id = target_88 (Group 35, Replica 1)   │
//            │  [107] do_target_id = target_12 (Group 35, Replica 2)   │
//            └─────────────────────────────────────────────────────────┘
//
//        - obj 的 单个 dkey：通过 dkey hash 映射到 1 个 group；该 dkey 的数据在这个 group 的 3 个 target 上互为副本
//         （跨 3 failure domain）；
//        - 整个 object 的所有 dkey：通过各自的 hash 分散到全部 nr_grps 个 group 上，实现负载均衡；
//
//    B. when open obj
//        - 只要 pool_map 没变，对于当前 obj (固定的obj id)，总能算出同样的 nr_grps 个 group
//        - open obj 时（未读写）就生成，存放在 struct dc_object { struct dc_obj_layout { struct dc_obj_shard do_shards } } 中，
//          如上图；
//        - 这个布局驻留在客户端内存中；若服务端 pool_map 发生变化，obj_layout_refresh 会重建布局；
//
//    C. when cluster extended
//        - 扩容后，集群 target 数增多，pool_map 变化；
//        - 但扩容前创建的 obj 的 nr_grps 不变；
//        - 用 固定的 obj id + 固定的 nr_grps + 新 pool_map 重算 placement，仍是 nr_grps 个 group、每组仍 3 target；
//          jump consistent hash 只让"部分" shard 迁移到新增 target（最小化数据搬迁），这些发生变化的 shard 触发
//          rebuild（数据迁移到新 target）；
typedef struct {
	/** least significant (low) bits of object ID */
	uint64_t	lo;
	/** most significant (high) bits of object ID */
	uint64_t	hi;
} daos_obj_id_t;

#define DAOS_UUID_STR_SIZE 37	/* 36 + 1 for '\0' */

static inline bool
daos_is_valid_uuid_string(const char *uuid)
{
	const char	*p;
	int		 len = DAOS_UUID_STR_SIZE - 1; /* Not include the terminated '\0' */
	int		 i;

	if (strnlen(uuid, len) != len)
		return false;

	for (i = 0, p = uuid; i < len; i++, p++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (*p != '-')
				return false;
		} else if (!isxdigit(*p)) {
			return false;
		}
	}

	return true;
}
/**
 * Corresponding rank and URI for a DAOS engine
 */
struct daos_rank_uri {
	/** DAOS engine rank */
	uint32_t	 dru_rank;
	/** URI associated with rank */
	char		*dru_uri;
};

/**
 * DAOS general system information for clients
 */
struct daos_sys_info {
	/** name of DAOS system */
	char			 dsi_system_name[DAOS_SYS_INFO_STRING_MAX + 1];
	/** fabric provider in use by this system */
	char			 dsi_fabric_provider[DAOS_SYS_INFO_STRING_MAX + 1];
	/** path to agent socket */
	char                     dsi_agent_path[PATH_MAX];
	/** length of ranks array */
	uint32_t		 dsi_nr_ranks;
	/** ranks and their client-accessible URIs */
	struct daos_rank_uri	*dsi_ranks;
	/** length of MS ranks array */
	uint32_t                 dsi_nr_ms_ranks;
	/** MS rank numbers */
	uint32_t                *dsi_ms_ranks;
};

/** max pool/cont attr size */
#define DAOS_ATTR_NAME_MAX 511

#if defined(__cplusplus)
}
#endif

#endif /* DAOS_TYPES_H */
