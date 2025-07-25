/**
 * (C) Copyright 2016-2024 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of daos
 *
 * src/include/daos/btree.h
 */

#ifndef __DAOS_BTREE_H__
#define __DAOS_BTREE_H__

#include <daos/common.h>
#include <daos_types.h>
#include <daos/mem.h>

/**
 * KV record of the btree.
 *
 * NB: could be PM data structure.
 */
//Yuanguo:  实际上是B+Tree
//
//                          struct btr_node
//
//        tn_child    tn_recs[...]数组
//        +-------+---------+---------+---------+---------+---------+
//        |       |   500   |         |         |         |         | key (union, e.g. rec_ukey )
//        |       +---------+---------+---------+---------+---------+
//        |       |         |         |         |         |         | val (rec_off)
//        +-------+---------+---------+---------+---------+---------+
//            |         |
//            |         |
// +----------+         +----------------------------------------+
// |                                                             |
// V                                                             | rec指向right node的first key
// off_left (原root)                                             V
//  +---------+---------+---------+---------+---------+          +---------+---------+---------+---------+---------+
//  |   200   |   240   |   300   |   400   |         | key      |   500   |   600   |         |         |         | key (union)
//  +---------+---------+---------+---------+---------+          +---------+---------+---------+---------+---------+
//  |         |         |         |         |         | val      |         |         |         |         |         | val (rec_off)
//  +---------+---------+---------+---------+---------+          +---------+---------+---------+---------+---------+
//      |          |       ...
//      V          V
//     value      value    ...
//Yuanguo:
//   对于一颗btree，sizeof(struct btr_record)是固定的：
struct btr_record {
	/**
	 * It could either be memory ID for the child node, or body of this
	 * record. The record body could be any of various things:
	 *
	 * - the value address of KV record.
	 * - a structure includes both the variable-length key and value.
	 * - a complex data structure under this record, e.g. a sub-tree.
	 */
    //Yuanguo: 两种情况
    //  - 指向child的指针：
    //  - 指向value的指针: 对应key的3种情况，value不同
    //      - 若是variable-length/large key: 指向key-val-pair；这种情况下，类似于hashmap,
    //        btree中排序的key是hash值(hashed key or hkey)，指向的是key-val-pair；
    //        hash conflict如何处理？
    //      - 若是uint64_t key: 指向value本身；
    //      - 若是direct key (fixed-size-small-bytearray): 指向value本身；
	umem_off_t		rec_off;
	/**
	 * Fix-size key can be stored in if it is small enough (DAOS_HKEY_MAX),
	 * or hashed key for variable-length/large key. In the later case,
	 * the hashed key can be used for efficient comparison.
	 *
	 * When BTR_FEAT_UINT_KEY is set, no key callbacks are used for
	 * comparisons.
	 *
	 * When BTR_FEAT_DIRECT_KEY is used, we store the umem offset of the
	 * relevant leaf node for direct key comparison
	 */
    //Yuanguo: record的key，在btree中有序(即使hash key，也是有序的)，有3种情况：
    //  - 用户key是variable-length/large key: 这里存它的hash值(算法不同hash值的长度也不相同，但对于一颗btree是固定的)，key-val-pair由rec_off指向；
    //  - 用户key是uint64_t: 这里就是uint64_t key
    //  - 用户key是fixed-size-small-bytearray (direct key)，bytearray直接用于比较，决定顺序；
    //      - leaf node：存储bytearray本身  (待确认)
    //      - non-leaf-node: 存储指向leaf node的指针，避免冗余  (待确认)
	union {
		char			rec_hkey[0]; /* hashed key */
		uint64_t		rec_ukey[0]; /* uint key */
		umem_off_t		rec_node[0]; /* direct key */
	};
};

/**
 * Tree node.
 *
 * NB: could be PM data structure.
 */
//Yuanguo: 内存布局
//  +----------------------+
//  |                      |
//  |  struct btr_node     |
//  |                      |
//  +----------------------+
//  |  struct btr_record   | tn_recs[0]
//  +----------------------+
//  |  struct btr_record   | tn_recs[1]
//  +----------------------+
//  |  struct btr_record   | tn_recs[2]
//  +----------------------+
//  |  ......              |
//  +----------------------+
//  |  struct btr_record   | tn_recs[btr_root::tr_node_size-1]
//  +----------------------+
//
//  数组tn_recs预留的空间个数是btr_root::tr_node_size, 见:
//      btr_node_split_and_insert() -->
//      btr_node_alloc() -->
//      btr_node_size()
//
//  里面真实存的元素个数是tn_keyn
struct btr_node {
	/** leaf, root etc */
	uint16_t			tn_flags;
	/** number of keys stored in this node */
    //Yuanguo: 不算tn_child指向的那个(第一个)
	uint16_t			tn_keyn;
	/** padding bytes */
	uint32_t			tn_pad_32;
	/** generation, reserved for COW */
	uint64_t			tn_gen;
	/** the first child, it is unused on leaf node */
	umem_off_t			tn_child;
	/** records in this node */
	struct btr_record		tn_recs[0];
};

enum {
	BTR_ORDER_MIN			= 3,
	BTR_ORDER_MAX			= 63
};

/**
 * Tree root descriptor, it consists of tree attributes and reference to the
 * actual root node.
 *
 * NB: Can be stored in pmem
 */
struct btr_root {
	/** For dynamic tree ordering, the root node temporarily has less
	 * entries than the order
	 */
	uint8_t				tr_node_size;
	/** configured btree order */
	uint8_t				tr_order;
	/** depth of the tree */
    //Yuanguo: 只有一个root node时，tr_depth=1，它的level是0
    //    假如tr_depth=20, leaf node的level=19
	uint16_t			tr_depth;
	/**
	 * ID to find a registered tree class, which provides customized
	 * functions etc.
	 */
	uint32_t			tr_class;
	/** the actual features of the tree, e.g. hash type, integer key */
	uint64_t			tr_feats;
	/** generation, reserved for COW */
	uint64_t			tr_gen;
	/** pointer to root node (struct btr_node), UMOFF_NULL for empty tree */
	umem_off_t			tr_node;
};

/** btree attributes returned by query function. */
struct btr_attr {
	/** Estimate of entries in tree.  Exact for tree depth <= 1 */
	int				ba_count;
	/** tree order */
	unsigned int			ba_order;
	/** tree depth */
	unsigned int			ba_depth;
	unsigned int			ba_class;
	uint64_t			ba_feats;
	/** memory class, pmem pool etc */
	struct umem_attr		ba_uma;
};

/** btree statistics returned by query function. */
struct btr_stat {
	/** total number of tree nodes */
	uint64_t			bs_node_nr;
	/** total number of records in the tree */
	uint64_t			bs_rec_nr;
	/** total number of bytes of all keys */
	uint64_t			bs_key_sum;
	/** max key size */
	uint64_t			bs_key_max;
	/** total number of bytes of all values */
	uint64_t			bs_val_sum;
	/** max value size */
	uint64_t			bs_val_max;
};

struct btr_rec_stat {
	/** record key size */
	uint64_t			rs_ksize;
	/** record value size */
	uint64_t			rs_vsize;
};

struct btr_instance;

typedef enum {
	/** probe a specific key */
	BTR_PROBE_SPEC		= (1 << 8),
	/**
	 * unconditionally trust the probe result from the previous call,
	 * bypass probe process for dbtree_upsert (or delete) in the future.
	 *
	 * This can reduce search overhead for use cases like this:
	 *    rc = dbtree_fetch(...key...);
	 *    if (rc == -DER_NONEXIST) {
	 *	    do_something_else(...);
	 *	    rc = dbtree_upsert(..., BTR_PROBE_BYPASS, key...);
	 *    }
	 *
	 * Please be careful while using this flag, because it could break
	 * the correctness of dbtree if inserting a new key to a mismatched
	 * probe path.
	 */
	BTR_PROBE_BYPASS	= 0,
	/** the first record in the tree */
	BTR_PROBE_FIRST		= 1,
	/** the last record in the tree */
	BTR_PROBE_LAST		= 2,
	/** probe the record whose key equals to the provide key */
	BTR_PROBE_EQ		= BTR_PROBE_SPEC,
	/** probe the record whose key is great to the provided key */
	BTR_PROBE_GT		= BTR_PROBE_SPEC | 1,
	/** probe the record whose key is less to the provided key */
	BTR_PROBE_LT		= BTR_PROBE_SPEC | 2,
	/** probe the record whose key is great/equal to the provided key */
	BTR_PROBE_GE		= BTR_PROBE_SPEC | 3,
	/** probe the record whose key is less/equal to the provided key */
	BTR_PROBE_LE		= BTR_PROBE_SPEC | 4,
} dbtree_probe_opc_t;

/** the return value of to_hkey_cmp/to_key_cmp callback */
enum btr_key_cmp_rc {
	BTR_CMP_EQ	= (0),		/* equal */
	BTR_CMP_LT	= (1 << 0),	/* less than */
	BTR_CMP_GT	= (1 << 1),	/* greater than */
	/**
	 * User can return it combined with BTR_CMP_LT/GT. If it is set,
	 * dbtree can fetch/update value even the provided key is less/greater
	 * than the compared key.
	 */
	BTR_CMP_UNKNOWN	= (1 << 2),	/* unset */
	BTR_CMP_ERR	= (1 << 3),	/* error */
};

/**
 * Customized tree function table.
 */
typedef struct {
	/**
	 * Generate a fix-size hashed key from the real key.
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param key	[IN]	key buffer
	 * \param hkey	[OUT]	hashed key
	 */
	void		(*to_hkey_gen)(struct btr_instance *tins,
				       d_iov_t *key, void *hkey);
	/** Static callback to get size of the hashed key. */
	int		(*to_hkey_size)(void);

	/** Static callback to metadata size of the record
	 *
	 * \param alloc_overhead[IN]	Expected per-allocation overhead
	 */
	int		(*to_rec_msize)(int alloc_ovheread);
	/**
	 * Optional:
	 * Comparison of hashed key.
	 *
	 * Absent:
	 * Calls memcmp.
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param rec	[IN]	Record to be compared with \a key.
	 * \param key	[IN]	Key to be compared with key of \a rec.
	 *
	 * \a return	BTR_CMP_LT	hkey of \a rec is smaller than \a hkey
	 *		BTR_CMP_GT	hkey of \a rec is larger than \a hkey
	 *		BTR_CMP_EQ	hkey of \a rec is equal to \a hkey
	 *		BTR_CMP_ERR	error in the hkey comparison
	 *		return any other value will cause assertion, segfault or
	 *		other undefined result.
	 */
	int		(*to_hkey_cmp)(struct btr_instance *tins,
				       struct btr_record *rec, void *hkey);
	/**
	 * Optional:
	 * Comparison of real key. It can be ignored if there is no hash
	 * for the key and key size is fixed. See \a btr_record for the details.
	 *
	 * Absent:
	 * Skip the function and only check rec::rec_hkey for the search.
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param rec	[IN]	Record to be compared with \a key.
	 * \param key	[IN]	Key to be compared with key of \a rec.
	 *
	 * \a return	BTR_CMP_LT	key of \a rec is smaller than \a key
	 *		BTR_CMP_GT	key of \a rec is larger than \a key
	 *		BTR_CMP_EQ	key of \a rec is equal to \a key
	 *		BTR_CMP_ERR	error in the key comparison
	 *		return any other value will cause assertion, segfault or
	 *		other undefined result.
	 */
	int		(*to_key_cmp)(struct btr_instance *tins,
				      struct btr_record *rec, d_iov_t *key);

	/**
	 * Required if using direct keys. (Should only be called for direct key)
	 * The encoding/decoding of direct keys is required so that the key can
	 * be serialized.
	 *
	 * @param tins		[IN]	Tree instance which contains the
	 *				root umem offset and memory class etc.
	 * @param key		[IN]	The current key of iteration.
	 * @param anchor	[OUT]	Anchor for the iteration
	 */
	void		(*to_key_encode)(struct btr_instance *tins,
					 d_iov_t *key,
					 daos_anchor_t *anchor);
	/**
	 * Required if using direct keys. (Should only be called for direct key)
	 *
	 * @param tins		[IN]	Tree instance which contains the root
	 *				umem offset and memory class etc.
	 * @param key		[OUT]	The key of iteration. Anchor will
	 *				be decoded to key.
	 * @param anchor	[IN]	Anchor of where iteration process is.
	 */
	void		(*to_key_decode)(struct btr_instance *tins,
					 d_iov_t *key,
					 daos_anchor_t *anchor);

	/**
	 * Allocate record body for \a key and \a val.
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param key	[IN]	Key buffer
	 * \param val	[IN]	Value buffer, it could be either data blob,
	 *			or complex data structure that can be parsed
	 *			by the tree class.
	 * \param rec	[OUT]	Returned record body pointer,
	 *			See \a btr_record for the details.
	 * \param val_out [OUT]	Returned value address.
	 */
	int		(*to_rec_alloc)(struct btr_instance *tins,
					d_iov_t *key, d_iov_t *val,
					struct btr_record *rec, d_iov_t *val_out);
	/**
	 * Free the record body stored in \a rec::rec_off
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param rec	[IN]	The record to be destroyed.
	 * \param args	[OUT]
	 *			Optional: opaque buffer for providing arguments
	 *			to handle special cases for free. for example,
	 *			allocator/GC address for externally allocated
	 *			resources.
	 */
	int		(*to_rec_free)(struct btr_instance *tins,
				       struct btr_record *rec, void *args);
	/**
	 * Fetch value or both key & value of a record.
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param rec	[IN]	Record to be read from.
	 * \param key	[OUT]	Optional, sink buffer for the returned key,
	 *			or key address.
	 * \param val	[OUT]	Sink buffer for the returned value or the
	 *			value address.
	 */
	int		(*to_rec_fetch)(struct btr_instance *tins,
					struct btr_record *rec,
					d_iov_t *key, d_iov_t *val);
	/**
	 * Update value of a record, the new value should be stored in the
	 * current rec::rec_off.
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param rec	[IN]	Record to be updated.
	 * \param val	[IN]	New value to be stored for the record.
	 * \param val_out [OUT]	Returned value address.
	 * \a return	0	success.
	 *		-DER_NO_PERM
	 *			cannot make inplace change, should call
	 *			rec_free() to release the original record
	 *			and rec_alloc() to create a new record.
	 *		1
	 *			cannot make inplace change, but rec_free()
	 *			and rec_alloc() for replacing are done.
	 *		-ve	error code
	 */
	int		(*to_rec_update)(struct btr_instance *tins,
					 struct btr_record *rec,
					 d_iov_t *key, d_iov_t *val, d_iov_t *val_out);
	/**
	 * Optional:
	 * Return key and value size of the record.
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param rec	[IN]	Record to get size from.
	 * \param rstat	[OUT]	Returned key & value size.
	 */
	int		(*to_rec_stat)(struct btr_instance *tins,
				       struct btr_record *rec,
				       struct btr_rec_stat *rstat);
	/**
	 * Convert record into readable string and store it in \a buf.
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param rec	[IN]	Record to be converted.
	 * \param leaf	[IN]	Both key and value should be converted to
	 *			string if it is true, otherwise only the
	 *			hashed key will be converted to string.
	 *			(record of intermediate node),
	 * \param buf	[OUT]	Buffer to store the returned string.
	 * \param buf_len [IN]	Buffer length.
	 */
	char	       *(*to_rec_string)(struct btr_instance *tins,
					 struct btr_record *rec, bool leaf,
					 char *buf, int buf_len);
	/**
	 * Optional:
	 * Check whether the given record is available to outside or not.
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param rec	[IN]	Record to be checked.
	 * \param intent [IN]	The intent for why check the record.
	 *
	 * \a return	ALB_AVAILABLE_DIRTY	The target is available but with
	 *					some uncommitted modification
	 *					or garbage, need cleanup.
	 *		ALB_AVAILABLE_CLEAN	The target is available,
	 *					no pending modification.
	 *		ALB_UNAVAILABLE		The target is unavailable.
	 *		-DER_INPROGRESS		If the target record is in
	 *					some uncommitted DTX, the caller
	 *					needs to retry related operation
	 *					some time later.
	 *		Other negative values on error.
	 */
	int		(*to_check_availability)(struct btr_instance *tins,
						 struct btr_record *rec,
						 uint32_t intent);
	/**
	 * Allocate a tree node
	 *
	 * \param tins	[IN]	Tree instance which contains the root umem
	 *			offset and memory class etc.
	 * \param size	[IN]	Node size
	 * \a return		Allocated node address (offset within the pool)
	 */
	umem_off_t	(*to_node_alloc)(struct btr_instance *tins, int size);

} btr_ops_t;

/**
 * Tree instance, it is instantiated while creating or opening a tree.
 */
struct btr_instance {
	/** instance of memory class for the tree */
	struct umem_instance		 ti_umm;
	/** Private data for opener */
	void				*ti_priv;
	/**
	 * The container open handle.
	 */
	daos_handle_t			 ti_coh;
	/** root umem offset */
	umem_off_t			 ti_root_off;
	/** root pointer */
	struct btr_root			*ti_root;
	/** Customized operations for the tree */
	btr_ops_t			*ti_ops;
};

/**
 * Inline data structure for embedding the key bundle and key into an anchor
 * for serialization.
 */
#define	EMBEDDED_KEY_MAX	100
struct btr_embedded_key {
	/** Inlined iov key references */
	uint32_t	ek_size;
	/** Inlined buffer the key references*/
	unsigned char	ek_key[EMBEDDED_KEY_MAX];
};

D_CASSERT(sizeof(struct btr_embedded_key) == DAOS_ANCHOR_BUF_MAX);

static inline void
embedded_key_encode(d_iov_t *key, daos_anchor_t *anchor)
{
	struct btr_embedded_key *embedded =
		(struct btr_embedded_key *)anchor->da_buf;

	D_ASSERT(key->iov_len <= sizeof(embedded->ek_key));

	memcpy(embedded->ek_key, key->iov_buf, key->iov_len);
	/** Pointers will have to be set on decode. */
	embedded->ek_size = key->iov_len;
}

static inline void
embedded_key_decode(d_iov_t *key, daos_anchor_t *anchor)
{
	struct btr_embedded_key *embedded =
		(struct btr_embedded_key *)anchor->da_buf;

	/* Fix the pointer first */
	key->iov_buf = &embedded->ek_key[0];
	key->iov_len = embedded->ek_size;
	key->iov_buf_len = embedded->ek_size;
}

/* Features are passed as 64-bit unsigned integer.   Only the bits below are
 * reserved.   A specific class can define its own bits to customize behavior.
 * For example, VOS can use bits to indicate the type of key comparison used
 * for user supplied key.   In general, using the upper bits is safer to avoid
 * conflicts in the future.
 */
enum btr_feats {
	/** Key is an unsigned integer.  Implies no hash or key callbacks */
	BTR_FEAT_UINT_KEY = (1 << 0),
	/** Key is not hashed or stored by library.  User must provide
	 * to_key_cmp callback
	 */
	BTR_FEAT_DIRECT_KEY = (1 << 1),
	/** Root is dynamically sized up to tree order.  This bit is set for a
	 *  tree class
	 */
	BTR_FEAT_DYNAMIC_ROOT = (1 << 2),
	/** Skip rebalance leaf when delete some record from the leaf. */
	BTR_FEAT_SKIP_LEAF_REBAL = (1 << 3),

    //Yuanguo: 区别
    //  BTR_FEAT_EMBED_FIRST : 是否支持embedded root;
    //  BTR_FEAT_EMBEDDED    : 是否真的使用了embedded root特性(已知支持embedded root的情况下);

	/** Tree supports embedded root. */
	BTR_FEAT_EMBED_FIRST = (1 << 4),
	/** Marks that the current root is an embedded value */
	BTR_FEAT_EMBEDDED = (1 << 5),
	/** Put new entries above this line */
	/** Convenience entry for calculating mask for all feats */
	BTR_FEAT_HELPER,
	/** Mask for all feats */
	BTR_FEAT_MASK = ((BTR_FEAT_HELPER - 1) << 1) - 1,
};

D_CASSERT(((BTR_FEAT_HELPER - 1) & BTR_FEAT_MASK) == (BTR_FEAT_HELPER - 1));

/**
 * Get the return code of to_hkey_cmp/to_key_cmp in case of success, for failure
 * case need to directly set it as BTR_CMP_ERR.
 */
static inline int
dbtree_key_cmp_rc(int rc)
{
	if (rc == 0)
		return BTR_CMP_EQ;
	else if (rc < 0)
		return BTR_CMP_LT;
	else
		return BTR_CMP_GT;
}

static inline int
dbtree_is_empty_inplace(const struct btr_root *root)
{
	D_ASSERT(root != NULL);
	return root->tr_depth == 0;
}

int  dbtree_class_register(unsigned int tree_class, uint64_t tree_feats,
			   btr_ops_t *ops);
int  dbtree_create(unsigned int tree_class, uint64_t tree_feats,
		   unsigned int tree_order, struct umem_attr *uma,
		   umem_off_t *root_offp, daos_handle_t *toh);
int  dbtree_create_inplace(unsigned int tree_class, uint64_t tree_feats,
			   unsigned int tree_order, struct umem_attr *uma,
			   struct btr_root *root, daos_handle_t *toh);
int  dbtree_create_inplace_ex(unsigned int tree_class, uint64_t tree_feats,
			      unsigned int tree_order, struct umem_attr *uma,
			      struct btr_root *root, daos_handle_t coh,
			      void *priv, daos_handle_t *toh);
int  dbtree_open(umem_off_t root_off, struct umem_attr *uma,
		 daos_handle_t *toh);
int  dbtree_open_inplace(struct btr_root *root, struct umem_attr *uma,
			 daos_handle_t *toh);
int  dbtree_open_inplace_ex(struct btr_root *root, struct umem_attr *uma,
			    daos_handle_t coh, void *priv, daos_handle_t *toh);
int  dbtree_close(daos_handle_t toh);
int  dbtree_destroy(daos_handle_t toh, void *args);
int  dbtree_drain(daos_handle_t toh, int *credits, void *args, bool *destroyed);
int  dbtree_lookup(daos_handle_t toh, d_iov_t *key, d_iov_t *val_out);
int  dbtree_update(daos_handle_t toh, d_iov_t *key, d_iov_t *val);
int  dbtree_fetch(daos_handle_t toh, dbtree_probe_opc_t opc, uint32_t intent,
		  d_iov_t *key, d_iov_t *key_out, d_iov_t *val_out);
int  dbtree_fetch_cur(daos_handle_t toh, d_iov_t *key_out, d_iov_t *val_out);
int  dbtree_fetch_prev(daos_handle_t toh, d_iov_t *key_out, d_iov_t *val_out, bool move);
int  dbtree_fetch_next(daos_handle_t toh, d_iov_t *key_out, d_iov_t *val_out, bool move);
int  dbtree_upsert(daos_handle_t toh, dbtree_probe_opc_t opc, uint32_t intent,
		   d_iov_t *key, d_iov_t *val, d_iov_t *val_out);
int  dbtree_delete(daos_handle_t toh, dbtree_probe_opc_t opc,
		   d_iov_t *key, void *args);
int  dbtree_query(daos_handle_t toh, struct btr_attr *attr,
		  struct btr_stat *stat);
int  dbtree_is_empty(daos_handle_t toh);
int  dbtree_feats_set(struct btr_root *root, struct umem_instance *umm, uint64_t feats);

static inline uint64_t
dbtree_feats_get(struct btr_root *root)
{
	return root->tr_feats;
}

struct umem_instance *btr_hdl2umm(daos_handle_t toh);

/**
 * hashed key for the key-btree, it is stored in btr_record::rec_hkey
 */

/** Inline key is max of 15 bytes.  The extra byte in the struct is used
 *  to encode the type (hash or inline) and the length of the inline key.
 */
#define KH_INLINE_MAX 15


struct ktr_hkey {
	/** murmur64 hash */
	union {
		/** NB: This assumes little endian.  We already have little
		 *  endian assumptions with integer keys so this isn't the
		 *  first violation.  The hkey_gen code will trigger an
		 *  assertion if this is violated.
		 */
		struct {
			/** Length of key shifted left by 2 bits. */
			uint32_t	kh_len;
			/** string32 hash of key */
			uint32_t	kh_str32;
			/** Murmur hash of key */
			uint64_t	kh_murmur64;
		};
		struct {
			/** length shifted left by 2 bits. Low bit means inline
			 *  key.  An extra bit is reserved for future use.
			 */
			char		kh_inline_len;
			/** Inline key */
			char		kh_inline[KH_INLINE_MAX];
		};
		/** For comparison convenience */
		uint64_t		kh_hash[2];
	};
};

/** hash seed for murmur hash */
#define BTR_MUR_SEED	0xC0FFEE

D_CASSERT(sizeof(struct ktr_hkey) == 16);
void hkey_common_gen(d_iov_t *key_iov, void *hkey);
int hkey_common_cmp(struct ktr_hkey *k1, struct ktr_hkey *k2);
void hkey_int_gen(d_iov_t *key,  void *hkey);

/******* iterator API ******************************************************/

enum {
	/**
	 * Use the embedded iterator of the open handle.
	 * It can reduce memory consumption, but state of iterator can be
	 * overwritten by other tree operation.
	 */
	BTR_ITER_EMBEDDED	= (1 << 0),
};

int dbtree_key2anchor(daos_handle_t toh, d_iov_t *key, daos_anchor_t *anchor);
int dbtree_iter_prepare(daos_handle_t toh, unsigned int options,
			daos_handle_t *ih);
int dbtree_iter_finish(daos_handle_t ih);
int dbtree_iter_probe(daos_handle_t ih, dbtree_probe_opc_t opc,
		      uint32_t intent, d_iov_t *key, daos_anchor_t *anchor);
int dbtree_iter_next(daos_handle_t ih);
int dbtree_iter_prev(daos_handle_t ih);
int dbtree_iter_fetch(daos_handle_t ih, d_iov_t *key,
		      d_iov_t *val, daos_anchor_t *anchor);
int dbtree_iter_delete(daos_handle_t ih, void *args);
int dbtree_iter_empty(daos_handle_t ih);

/**
 * Prototype of dbtree_iterate() callbacks. When a callback returns an rc,
 *
 *   - if rc == 0, dbtree_iterate() continues;
 *   - if rc == 1, dbtree_iterate() stops and returns 0;
 *   - otherwise, dbtree_iterate() stops and returns rc.
 */
typedef int (*dbtree_iterate_cb_t)(daos_handle_t ih, d_iov_t *key,
				   d_iov_t *val, void *arg);
int dbtree_iterate(daos_handle_t toh, uint32_t intent, bool backward,
		   dbtree_iterate_cb_t cb, void *arg);

enum {
	DBTREE_VOS_BEGIN	= 10,
	DBTREE_VOS_END		= DBTREE_VOS_BEGIN + 9,
	DBTREE_DSM_BEGIN	= 20,
	DBTREE_DSM_END		= DBTREE_DSM_BEGIN + 19,
	DBTREE_SMD_BEGIN	= 40,
	DBTREE_SMD_END		= DBTREE_SMD_BEGIN + 9,
};

/** Get overhead constants for a given tree class
 *
 * \param alloc_overhead[IN]	Expected per-allocation overhead in bytes
 * \param tclass[IN]		The registered tree class
 * \param feats[IN]		The features used to initialize the tree class
 * \param tree_order[IN]	The expected tree order used in creation
 * \param ovhd[OUT]		Struct to fill with overheads
 *
 * \return 0 on success, error otherwise
 */
int dbtree_overhead_get(int alloc_overhead, unsigned int tclass, uint64_t feats,
			int tree_order, struct daos_tree_overhead *ovhd);

#endif /* __DAOS_BTREE_H__ */
