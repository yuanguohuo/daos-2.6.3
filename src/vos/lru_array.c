/**
 * (C) Copyright 2020-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * LRU array implementation
 * vos/lru_array.c
 *
 * Author: Jeff Olivier <jeffrey.v.olivier@intel.com>
 */
#define D_LOGFAC DD_FAC(vos)
#include "lru_array.h"
#include "vos_internal.h"

/** Internal converter for real index to entity index in sub array */
//Yuanguo: 从 subarray index，构造 array index；
#define ent2idx(array, sub, ent_idx)	\
	(((sub)->ls_array_idx << (array)->la_array_shift) + (ent_idx))

static void
evict_cb(struct lru_array *array, struct lru_sub *sub, struct lru_entry *entry,
	 uint32_t idx)
{
	uint32_t	real_idx;

	if (array->la_cbs.lru_on_evict == NULL) {
		/** By default, reset the entry */
		memset(entry->le_payload, 0, array->la_payload_size);
		return;
	}

	real_idx = ent2idx(array, sub, idx);

	array->la_evicting++;
	array->la_cbs.lru_on_evict(entry->le_payload, real_idx, array->la_arg);
	array->la_evicting--;
}

static void
init_cb(struct lru_array *array, struct lru_sub *sub, struct lru_entry *entry,
	uint32_t idx)
{
	uint32_t	real_idx;

	if (array->la_cbs.lru_on_init == NULL)
		return;

	real_idx = ent2idx(array, sub, idx);

	array->la_cbs.lru_on_init(entry->le_payload, real_idx, array->la_arg);
}

static void
fini_cb(struct lru_array *array, struct lru_sub *sub, struct lru_entry *entry,
	uint32_t idx)
{
	uint32_t	real_idx;

	if (array->la_cbs.lru_on_fini == NULL)
		return;

	real_idx = ent2idx(array, sub, idx);

	array->la_cbs.lru_on_fini(entry->le_payload, real_idx, array->la_arg);
}

static void
alloc_cb(struct lru_array *array, daos_size_t size)
{
	if (array->la_cbs.lru_on_alloc == NULL)
		return;

	array->la_cbs.lru_on_alloc(array->la_arg, size);
}

static void
free_cb(struct lru_array *array, daos_size_t size)
{
	if (array->la_cbs.lru_on_free == NULL)
		return;

	array->la_cbs.lru_on_free(array->la_arg, size);
}

//Yuanguo: 以container的dtx array为例 （ vos_cont_open() --> lrua_array_alloc() ）
//  nr_ent     =  DTX_ARRAY_LEN =  1 << 20
//  nr_arrays  = DTX_ARRAY_NR   =  1 << 11
//  payload_size = sizeof(struct vos_dtx_act_ent)
//  flags        = LRU_FLAG_REUSE_UNIQUE  由于 nr_arrays > 1, flags 最终是 LRU_FLAG_REUSE_UNIQUE ｜ LRU_FLAG_EVICT_MANUAL
//  cbs = lru_dtx_cache_cbs;  这些callback没有实际作用，只是记录一些alloc和free (for trace)
//
//  array->nr_arrays   = 1<<11 = 2048                     index 的高 11-bit 是 subarray 号；共2048个subarray；
//  array->la_idx_mask = 511 = 111111111 (二进制9个1)     index 的低 9-bit  是 subarray 内的 index；每个subarray大小是512；
//  array->la_array_shift = 9                             la_idx_mask 的位数；index 右移 9-bit 即得到 subarray 号；
//  即array总共 1<<20 个entry，分为 1<<11 = 2048 个 subarray，每个 subarray 512 个entry;
//
//  宏：
//     lrua_idx2sub(array, idx)       从 array 内 idx 得到 subarray 指针；
//     lrua_idx2ent(array, idx)       从 array 内 idx 得到 subarray 内 idx；
//     ent2idx(array, sub, ent_idx)   从 subarray 内 idx 构造 array 内idx;
//
//  本函数是分配一个 subarray 的 ls_table (即entries空间):
//      rec_size = sizeof(struct lru_entry) + {sizeof(struct vos_dtx_act_ent)对齐到8的整数倍}
//      nr_ents  = array->la_idx_mask + 1 = 512；
//
//     +========================+
//     |     struct lru_sub     |
//     |        ls_lru          |                            ls_lru  指向lru 环的起始index (coldest)；
//     |        ls_free         |                            ls_free 指向free环的起始index；
//     |        ......          |
//     |                        |
//     |        ls_table        | ------+
//     +========================+       |
//     +----struct lru_entry----+ <-----+                     这些struct lru_entry构成2个环
//     |                        |                                   1. free 环
//     |      le_payload        | ---+                              2. lru  环
//     +------------------------+    |                                    ls_lru
//     +----struct lru_entry----+    |              ... ---> hotest --->  coldest ---> ... --->
//     |                        |    |
//     |      le_payload        | -------+
//     +------------------------+    |   |
//     .                        .    |   |
//     .      ......            .    |   |
//     .                        .    |   |
//     +----struct lru_entry----+    |   |
//     |                        |    |   |
//     |      le_payload        | -----------+
//     +------------------------+    |   |   |
//     +------------------------+ <--+   |   |
//     |////////////////////////|        |   |
//     |////////////////////////|        |   |
//     |////////////////////////| <------+   |
//     |////////////////////////|            |
//     | ...................... |            |
//     | ...................... |            |
//     | ...................... |            |
//     | ...................... |            |
//     | ...................... |            |
//     |////////////////////////| <----------+
//     |////////////////////////|
//     |////////////////////////|
//     +------------------------+
int
lrua_array_alloc_one(struct lru_array *array, struct lru_sub *sub)
{
	struct lru_entry	*entry;
	char			*payload;
	size_t			 rec_size;
	uint32_t		 nr_ents = array->la_idx_mask + 1;
	uint32_t		 prev_idx = nr_ents - 1;
	uint32_t		 idx;

	rec_size = sizeof(*entry) + array->la_payload_size;
	//Yuanguo: 分配 512 个 entry 的空间；
	D_ALLOC(sub->ls_table, rec_size * nr_ents);
	if (sub->ls_table == NULL)
		return -DER_NOMEM;

	//Yuanguo: 对于dtx array而言，没有实际作用，只是调用lru_dtx_cache_cbs中的callback函数，记录alloc (for trace)
	alloc_cb(array, rec_size * nr_ents);

	/** Add newly allocated ones to head of list */
	//Yuanguo: 把当前 subarray 从 unused (unused其实是unallocated的意思) 列表移除，并添加到 free 列表；
	//  free列表实际上是指 包含free entry的subarray 的列表；
	d_list_del(&sub->ls_link);
	d_list_add(&sub->ls_link, &array->la_free_sub);

	payload = sub->ls_payload = &sub->ls_table[nr_ents];
	sub->ls_lru = LRU_NO_IDX;
	sub->ls_free = 0;
	//Yuanguo: 初始时，所有entry都放进 free 环；
	//  通过 struct lru_entry 的 le_prev_idx 和 le_next_idx 构成一个"循环double list"
	//  第一个的prev是最后一个；最后一个的next是第一个；
	for (idx = 0; idx < nr_ents; idx++) {
		entry = &sub->ls_table[idx];
		entry->le_payload = payload;
		entry->le_prev_idx = prev_idx;
		entry->le_next_idx = (idx + 1) & array->la_idx_mask;
		init_cb(array, sub, entry, idx);
		payload += array->la_payload_size;
		prev_idx = idx;
	}

	return 0;
}

static inline bool
sub_find_free(struct lru_array *array, struct lru_sub *sub,
	      struct lru_entry **entryp, uint32_t *idx, uint64_t key)
{
	struct lru_entry	*entry;
	uint32_t		 tree_idx;

	//Yuanguo: subarray `sub` 没有free entry；
	if (sub->ls_free == LRU_NO_IDX)
		return false;

	//Yuanguo: ls_free 指向当前 subarray 的 free 环；
	tree_idx = sub->ls_free;

	entry = &sub->ls_table[tree_idx];

	/** Remove from free list */
	//Yuanguo: 从 free 环摘除；
	lrua_remove_entry(array, sub, &sub->ls_free, entry, tree_idx);

	/** Insert at tail (mru) */
	//Yuanguo: 插入 lru 环；
	lrua_insert(sub, &sub->ls_lru, entry, tree_idx, true);

	entry->le_key = key;

	*entryp = entry;

	//Yuanguo: 从 subarray index，构造 array index；
	*idx = ent2idx(array, sub, tree_idx);

	return true;
}

static inline int
manual_find_free(struct lru_array *array, struct lru_entry **entryp,
		 uint32_t *idx, uint64_t key)
{
	struct lru_sub	*sub = NULL;
	bool		 found;
	int		 rc;

	/** First search already allocated lists */
	//Yuanguo:
	//  多subarray的情况下(la_flags & LRU_FLAG_EVICT_MANUAL 不为0)，la_free_sub 列表中的 subarray 一定都有 free entry！
	//  原因见lrua_remove_entry()函数的结尾处：
	//       从 subarray 的 free 环摘除，且摘除完之后就没有free entry了，则把 subarray 从它所在的列表(la_free_sub)移除！
	//  所以，只要还在 la_free_sub 中的subarray，一定有 free entry!
	//  这个遍历是否有必要？
	d_list_for_each_entry(sub, &array->la_free_sub, ls_link) {
		if (sub_find_free(array, sub, entryp, idx, key))
			return 0;
	}

	/** No free entries */
	//Yuanguo: 为什么不淘汰呢？lru 体现在何处？
	//  见 LRU_FLAG_EVICT_MANUAL 前的注释：No automatic eviction of the LRU ...
	if (d_list_empty(&array->la_unused_sub))
		return -DER_BUSY;; /* No free sub arrays either */

	sub = d_list_entry(array->la_unused_sub.next, struct lru_sub, ls_link);
	rc = lrua_array_alloc_one(array, sub);
	if (rc != 0)
		return rc;

	found = sub_find_free(array, sub, entryp, idx, key);
	D_ASSERT(found);

	return 0;
}

int
lrua_find_free(struct lru_array *array, struct lru_entry **entryp,
	       uint32_t *idx, uint64_t key)
{
	struct lru_sub		*sub;
	struct lru_entry	*entry;

	*entryp = NULL;

	if (array->la_flags & LRU_FLAG_EVICT_MANUAL) {
		return manual_find_free(array, entryp, idx, key);
	}

	//Yuanguo: array->la_flags & LRU_FLAG_EVICT_MANUAL == 0，说明只有1个subarray，即 array->la_sub[0]，
	//  所以只能从它中找；
	sub = &array->la_sub[0];
	if (sub_find_free(array, sub, entryp, idx, key))
		return 0;

	//Yuanguo: 上面sub_find_free()失败，淘汰已用的entry；
	//  sub->ls_lru 指向 subarray 的 lru 环；
	//  淘汰entry，把它分配给现在的caller；
	//  问：持有index的原来的caller怎么办呢？
	//  答：lrua_lookup_idx(), lrua_evictx() 等函数会比较key；
	entry = &sub->ls_table[sub->ls_lru];
	/** Key should not be 0, otherwise, it should be in free list */
	D_ASSERT(entry->le_key != 0);

	evict_cb(array, sub, entry, sub->ls_lru);

	*idx = ent2idx(array, sub, sub->ls_lru);
	entry->le_key = key;
	//Yuanguo: 反正ls_lru指向的是一个环，让它指向下一个，是为了下次淘汰发生时，不淘汰当前 entry，而是淘汰next (当前entry刚刚放进来)
	//             `entry`       ls_lru
	//     ... ---> hotest --->  coldest ---> ... --->
	sub->ls_lru = entry->le_next_idx;

	*entryp = entry;

	return 0;
}

//Yuanguo: 其实本函数不应该叫做 lrua_evictx， 叫 lrua_remove 更好!
void
lrua_evictx(struct lru_array *array, uint32_t idx, uint64_t key)
{
	struct lru_entry	*entry;
	struct lru_sub		*sub;
	uint32_t		 ent_idx;

	D_ASSERT(array != NULL);
	D_ASSERT(key != 0);

	if (idx >= array->la_count)
		return;

	sub = lrua_idx2sub(array, idx);
	ent_idx = lrua_idx2ent(array, idx);

	if (sub->ls_table == NULL)
		return;

	entry = &sub->ls_table[ent_idx];

	//Yuanguo: lrua_find_free() 函数可能 silently 淘汰一个entry；
	//  user-1: lrua_find_free() 返回 idx = A, 存入keyA
	//  user-2: lrua_find_free() 返回 idx = A, 存入keyB；此时user-1持有的idx = A 以无效；
	//  现在user-1 调用 lrua_evictx(array, idx=A, key=keyA)，就会在这里返回，do nothing!
	if (key != entry->le_key)
		return;

	evict_cb(array, sub, entry, ent_idx);

	entry->le_key = 0;

	/** Remove from active list */
	lrua_remove_entry(array, sub, &sub->ls_lru, entry, ent_idx);

	//Yuanguo: 移除`entry`之前，subarray `sub` 没有任何free entry (sub->ls_free == LRU_NO_IDX)，
	//  移除之后，就有了free entry，所以应该加入array的la_free_sub列表(包含 free entry 的 subarray 的列表)；
	//  问1：为什么要限制 array->la_flags & LRU_FLAG_EVICT_MANUAL 呢？
	//  答1：猜测，假如 array->la_flags & LRU_FLAG_EVICT_MANUAL 不成立，array只有一个subarray，没有维护 la_free_sub/la_unused_sub 列表的必要；
	//       例如lrua_find_free()函数，对于 array->la_flags & LRU_FLAG_EVICT_MANUAL 不成立的情况，直接操作 array->la_sub[0] 这个 subarray，根
	//       本不管它在不在 la_free_sub/la_unused_sub 列表中；
	//  问2：哪里把full subarry (没有free entry的subarray) 从 la_free_sub 摘除？
	//  答2：lrua_remove_entry()函数：若参数是head是ls_free，且是remove最后一个entry，会把*head置为LRU_NO_IDX；
	//       且从 la_free_sub 摘除 (函数末尾处)
	if (sub->ls_free == LRU_NO_IDX &&
	    (array->la_flags & LRU_FLAG_EVICT_MANUAL)) {
		D_ASSERT(d_list_empty(&sub->ls_link));
		/** Add the entry back to the free list */
		d_list_add_tail(&sub->ls_link, &array->la_free_sub);
	}

	/** Insert in free list */
	//Yuanguo: 把entry插入free环；
	lrua_insert(sub, &sub->ls_free, entry, ent_idx,
		    (array->la_flags & LRU_FLAG_REUSE_UNIQUE) != 0);
}

//Yuanguo: 以container的dtx array为例 （ vos_cont_open() --> lrua_array_alloc() ）
//  nr_ent     =  DTX_ARRAY_LEN =  1 << 20
//  nr_arrays  = DTX_ARRAY_NR   =  1 << 11
//  payload_size = sizeof(struct vos_dtx_act_ent)
//  flags        = LRU_FLAG_REUSE_UNIQUE  由于 nr_arrays > 1, flags 最终是 LRU_FLAG_REUSE_UNIQUE ｜ LRU_FLAG_EVICT_MANUAL
//  cbs = lru_dtx_cache_cbs;  这些callback没有实际作用，只是记录一些alloc和free (for trace)
//
//  array->nr_arrays   = 1<<11 = 2048                     index 的高 11-bit 是 subarray 号；共2048个subarray；
//  array->la_idx_mask = 511 = 111111111 (二进制9个1)     index 的低 9-bit  是 subarray 内的 index；每个subarray大小是512；
//  array->la_array_shift = 9                             la_idx_mask 的位数；index 右移 9-bit 即得到 subarray 号；
//  即array总共 1<<20 个entry，分为 1<<11 = 2048 个 subarray，每个 subarray 512 个entry;
//
//  宏：
//     lrua_idx2sub(array, idx)       从 array 内 idx 得到 subarray 指针；
//     lrua_idx2ent(array, idx)       从 array 内 idx 得到 subarray 内 idx；
//     ent2idx(array, sub, ent_idx)   从 subarray 内 idx 构造 array 内idx;
int
lrua_array_alloc(struct lru_array **arrayp, uint32_t nr_ent, uint32_t nr_arrays,
		 uint16_t payload_size, uint32_t flags,
		 const struct lru_callbacks *cbs, void *arg)
{
	struct lru_array	*array;
	uint32_t		 aligned_size;
	uint32_t		 idx;
	int			 rc;

	D_ASSERT(arrayp != NULL);
	/** The prev != next assertions require the array to have a minimum
	 *  size of 3.   Just assert this precondition.
	 */
	D_ASSERT(nr_ent > 2);

	/** nr_ent and nr_arrays need to be powers of two and nr_arrays
	 *  must be less than nr_ent.  This enables faster lookups by using
	 *  & operations rather than % operations
	 */
	D_ASSERT((nr_ent & (nr_ent - 1)) == 0);
	D_ASSERT((nr_arrays & (nr_arrays - 1)) == 0);
	D_ASSERT(nr_arrays != 0);
	D_ASSERT(nr_ent > nr_arrays);

	if (nr_arrays != 1) {
		/** No good algorithm for auto eviction across multiple
		 *  sub arrays since one lru is maintained per sub array
		 */
		flags |= LRU_FLAG_EVICT_MANUAL;
	}

	aligned_size = (payload_size + 7) & ~7;

	*arrayp = NULL;

	D_ALLOC(array, sizeof(*array) +
		(sizeof(array->la_sub[0]) * nr_arrays));
	if (array == NULL)
		return -DER_NOMEM;

	array->la_count = nr_ent;
	array->la_idx_mask = (nr_ent / nr_arrays) - 1;
	array->la_array_nr = nr_arrays;
	array->la_array_shift = 1;
	while ((1 << array->la_array_shift) < array->la_idx_mask)
		array->la_array_shift++;
	array->la_payload_size = aligned_size;
	array->la_flags = flags;
	array->la_arg = arg;
	if (cbs != NULL)
		array->la_cbs = *cbs;

	//Yuanguo: 对于dtx array而言，没有实际作用，只是调用lru_dtx_cache_cbs中的callback函数，记录alloc (for trace)
	alloc_cb(array, sizeof(*array) + sizeof(array->la_sub[0]) * nr_arrays);
	/** Only allocate one sub array, add the rest to free list */
	D_INIT_LIST_HEAD(&array->la_free_sub);
	D_INIT_LIST_HEAD(&array->la_unused_sub);
	for (idx = 0; idx < nr_arrays; idx++) {
		array->la_sub[idx].ls_array_idx = idx;
		//Yuanguo: 现在所有的subarray都是unused；unused 其实是 unallocated；下面lrua_array_alloc_one() 中，
		//  分配完空间，就从 la_unused_sub 列表移到 la_free_sub 列表；
		d_list_add_tail(&array->la_sub[idx].ls_link,
				&array->la_unused_sub);
	}

	//Yuanguo: 这里只初始化第一个subarray;
	rc = lrua_array_alloc_one(array, &array->la_sub[0]);
	if (rc != 0) {
		free_cb(array, sizeof(*array) + sizeof(array->la_sub[0]) * nr_arrays);
		D_FREE(array);
		return rc;
	}

	*arrayp = array;

	return 0;
}

static void
array_free_one(struct lru_array *array, struct lru_sub *sub)
{
	uint32_t	idx;

	for (idx = 0; idx < array->la_idx_mask + 1; idx++)
		fini_cb(array, sub, &sub->ls_table[idx], idx);

	D_FREE(sub->ls_table);

	//Yuanguo: 不需要把 sub 放入 array->la_unused_sub (unallocated列表) 中吗？
	//   1. lrua_array_free()中，整个 array 都要free，也就没有必要了；
	//   2. lrua_array_aggregate()中，在调用本函数之前，已经做了！

	free_cb(array,
		(sizeof(struct lru_entry) + array->la_payload_size) *
		(array->la_idx_mask + 1));
}

void
lrua_array_free(struct lru_array *array)
{
	struct lru_sub	*sub;
	uint32_t	 i;

	if (array == NULL)
		return;

	for (i = 0; i < array->la_array_nr; i++) {
		sub = &array->la_sub[i];
		if (sub->ls_table != NULL)
			array_free_one(array, sub);
	}

	free_cb(array, sizeof(*array) + sizeof(array->la_sub[0]) * array->la_array_nr);

	D_FREE(array);
}

//Yuanguo: 把全 free 的 subarray 的空间释放掉，并加入 array->la_unused_sub (unallocated)列表；
void
lrua_array_aggregate(struct lru_array *array)
{
	struct lru_sub	*sub;
	struct lru_sub	*tmp;

	if ((array->la_flags & LRU_FLAG_EVICT_MANUAL) == 0)
		return; /* Not applicable */

	if (d_list_empty(&array->la_free_sub))
		return; /* Nothing to do */

	/** Grab the 2nd entry (may be head in which case the loop will  be a
	 *  noop).   This leaves some free entries in the array.
	 */
	//Yuanguo: 从第2个有 free 的subarray开始；假如只有1个，那么下面的循环就不执行(第一次条件就不满足)；
	sub = d_list_entry(array->la_free_sub.next->next, struct lru_sub,
			   ls_link);

	d_list_for_each_entry_safe_from(sub, tmp, &array->la_free_sub,
					ls_link) {
		//Yuanguo: 当前 subarray 有占用，不能free；
		if (sub->ls_lru != LRU_NO_IDX)
			continue; /** Used entries */
		//Yuanguo: 当前 subarray 全 free；把它从 la_free_sub 列表删除，加入 la_unused_sub 列表 (unused 表示 unallocated)
		//  array_free_one() 就是 free 它，变成 unallocated!
		d_list_del(&sub->ls_link);
		d_list_add_tail(&sub->ls_link, &array->la_unused_sub);
		array_free_one(array, sub);
	}
}
