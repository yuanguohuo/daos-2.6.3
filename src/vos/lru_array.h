/**
 * (C) Copyright 2020-2023 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * Generic struct for allocating LRU entries in an array
 * common/lru_array.h
 *
 * Author: Jeff Olivier <jeffrey.v.olivier@intel.com>
 */

#ifndef __LRU_ARRAY__
#define __LRU_ARRAY__

#include <daos/common.h>

struct lru_callbacks {
	/** Called when an entry is going to be evicted from cache */
	void	(*lru_on_evict)(void *entry, uint32_t idx, void *arg);
	/** Called on initialization of an entry */
	void	(*lru_on_init)(void *entry, uint32_t idx, void *arg);
	/** Called on finalization of an entry */
	void	(*lru_on_fini)(void *entry, uint32_t idx, void *arg);
	/** Called on allocation of any LRU entries */
	void	(*lru_on_alloc)(void *arg, daos_size_t size);
	/** Called on free of any LRU entries */
	void	(*lru_on_free)(void *arg, daos_size_t size);
};

struct lru_entry {
	/** Unique identifier for this entry */
	uint64_t	 le_key;
	/** Pointer to this entry */
	void		*le_payload;
	/** Next index in LRU array */
	uint32_t	 le_next_idx;
	/** Previous index in LRU array */
	uint32_t	 le_prev_idx;
};

struct lru_sub {
	/** Index of LRU */
	uint32_t		 ls_lru;
	/** Index of first free entry */
	uint32_t		 ls_free;
	/** Index of this entry in the array */
	//Yuanguo: this subarray 在 lru_array 中的 subarray 号，即在 lru_array::la_sub 数组中的index；
	uint32_t		 ls_array_idx;
	/** Padding */
	uint32_t		 ls_pad;
	/** Link in the array free/unused list.  If the subarray has no free
	 *  entries, it is removed from either list so this field is unused.
	 */
	//Yuanguo:
	//  若 this subarray 没有分配空间，ls_link 链接到 lru_array 的 la_unused_sub 列表；
	//  若 this subarray 已分配空间
	//     - 有 free entry: ls_link 链接到 lru_array 的 la_free_sub 列表；
	//     - 无 free entry: ls_link 不链接到任何列表，见 lrua_remove_entry() 函数，当最后一个free entry被remove时 ...
	d_list_t		 ls_link;
	/** Allocated payload entries */
	void			*ls_payload;
	/** Entries in the array */
	struct lru_entry	*ls_table;
};

#define LRU_NO_IDX	0xffffffff

enum {
	/** No automatic eviction of the LRU.  Flag is set automatically for
	 *  arrays with multiple sub arrays
	 */
	LRU_FLAG_EVICT_MANUAL		= 1,
	/** Free'd entries are added to tail of free list to avoid frequent
	 *  reuse of entries
	 */
	LRU_FLAG_REUSE_UNIQUE		= 2,
};

//Yuanguo: struct lru_array，可以看作是一个缓存，有单subarray和多subarray 2种情况：
//  - 单subarray：la_flags 不含 LRU_FLAG_EVICT_MANUAL，会自动淘汰最冷的；
//  - 多subarray：la_flags 包含 LRU_FLAG_EVICT_MANUAL，不会自动淘汰，需要使用者 manually evict! 
//                和 lru 没关系，相当于一个 "mem-pool".
//
// 多subarray情况下不会自动淘汰，所以index和entry一一对应，只要持有index，一定能找到entry；
// 因为不会淘汰，持有对象的指针也足够用？
// 不! 删除时还是需要index!
//
// 注意：单subarray情况下，array full时，可能把user的entry淘汰了，user的index不再有效；
//       所以 struct lru_entry::le_key 非常重要！user必须同时持有index和le_key，
//       查询、删除时，都要通过比较le_key来确定index指向的对象是不是他的!
struct lru_array {
	/** Number of indices */
	uint32_t		 la_count;
	/** record size */
	uint16_t		 la_payload_size;
	/** eviction count */
	uint16_t		 la_evicting;
	/** Array flags */
	uint32_t		 la_flags;
	/** Number of 2nd level arrays */
	uint32_t		 la_array_nr;
	/** Second level bit shift */
	uint32_t		 la_array_shift;
	/** First level mask */
	uint32_t		 la_idx_mask;
	/** Subarrays with free entries */
	//Yuanguo：包含free entry 的 subarray 的列表；
	//  多subarray的情况下(la_flags & LRU_FLAG_EVICT_MANUAL 不为0)，此列表中的 subarray 一定都有 free entry！
	//  原因见lrua_remove_entry()函数的结尾处：
	//       从 subarray 的 free 环摘除，且摘除完之后就没有free entry了，则把 subarray 从它所在的列表(la_free_sub)移除！
	//  所以，只要还在 la_free_sub 中的subarray，一定有 free entry!
	//  问：manual_find_free()中，遍历 la_free_sub 是否有必要？随便找一个 la_free_sub 中的subarray (例如第一个)，就会有free！除非 la_free_sub 为空！
	d_list_t		 la_free_sub;
	/** Unallocated subarrays */
	//Yuanguo：为什么不叫 la_unallocated_sub ?
	d_list_t		 la_unused_sub;
	/** Callbacks for implementation */
	struct lru_callbacks	 la_cbs;
	/** User callback argument passed on init */
	void			*la_arg;
	/** Allocated subarrays */
	struct lru_sub		 la_sub[0];
};

/** Internal converter for real index to sub array index */
//Yuanguo: 从 array 内 idx 得到 subarray 指针；
#define lrua_idx2sub(array, idx)	\
	(&(array)->la_sub[((idx) >> (array)->la_array_shift)])
/** Internal converter for real index to entity index in sub array */
//Yuanguo: 从 array 内 idx 得到 subarray 内 idx；
#define lrua_idx2ent(array, idx) ((idx) & (array)->la_idx_mask)

/** Internal API: Allocate one sub array */
int
lrua_array_alloc_one(struct lru_array *array, struct lru_sub *sub);

/** Internal API: Evict the LRU, move it to MRU, invoke eviction callback,
 *  and return the index
 */
//Yuanguo: 找一个free lru_entry，把`key`参数存到它的 le_key 上；
//  注意，不是在 array 内 search key 相等的 entry;
int
lrua_find_free(struct lru_array *array, struct lru_entry **entry,
	       uint32_t *idx, uint64_t key);

/** Internal API: Remove an entry from the lru list */
static inline void
lrua_remove_entry(struct lru_array *array, struct lru_sub *sub, uint32_t *head,
		  struct lru_entry *entry, uint32_t idx)
{
	struct lru_entry	*entries = &sub->ls_table[0];
	struct lru_entry	*prev = &entries[entry->le_prev_idx];
	struct lru_entry	*next = &entries[entry->le_next_idx];

	/** Last entry in the list */
	if (prev == entry) {
		*head = LRU_NO_IDX;
	} else {
		prev->le_next_idx = entry->le_next_idx;
		next->le_prev_idx = entry->le_prev_idx;
		if (idx == *head)
			*head = entry->le_next_idx;
	}

	/*
	 * If no free entries in the sub, then remove it from array free list (array->la_free_sub)
	 * to avoid being searched when try to find free entry next time.
	 */
	//Yuanguo:
	//   - head == &sub->ls_free  ： 当前是从 free 环摘除；
	//   - *head == LRU_NO_IDX    ： 摘除之后 free 环为空；
	//   - array->la_flags & LRU_FLAG_EVICT_MANUAL ：多subarray；
	//
	// subarray `sub` 不再包含free entry了，成了full subarry，所以把它从所属list (array->la_free_sub) 摘除；
	// lrua_evictx() 执行相反的操作：当从一个full subarry删除一个entry，它有变得“包含free entry”了，需要加回
	// array->la_free_sub 列表；
	//
	// 为什么要 array->la_flags & LRU_FLAG_EVICT_MANUAL (多subarray)呢？因为单subarray根本没有必要维护 la_free_sub/la_unused_sub 列表；
	if (head == &sub->ls_free && *head == LRU_NO_IDX && array->la_flags & LRU_FLAG_EVICT_MANUAL)
		d_list_del_init(&sub->ls_link);
}

/** Internal API: Insert an entry in the lru list */
static inline void
lrua_insert(struct lru_sub *sub, uint32_t *head, struct lru_entry *entry,
	    uint32_t idx, bool append)
{
	struct lru_entry	*entries = &sub->ls_table[0];
	struct lru_entry	*prev;
	struct lru_entry	*next;
	uint32_t		 tail;

	if (*head == LRU_NO_IDX) {
		*head = entry->le_prev_idx = entry->le_next_idx = idx;
		return;
	}

	next = &entries[*head];
	tail = next->le_prev_idx;
	prev = &entries[tail];
	next->le_prev_idx = idx;
	prev->le_next_idx = idx;
	entry->le_prev_idx = tail;
	entry->le_next_idx = *head;

	if (append)
		return;

	*head = idx;
}

/** Internal API: Make the entry the mru */
//Yuanguo: mru 就是 most recently used, 也就是 hotest，即 sub->ls_lru 的前一个；
//                                 mru        sub->ls_lru
//   ... ---> hot ---> hoter ---> hotest ---> coldest ---> colder ---> cold ---> ...
static inline void
lrua_move_to_mru(struct lru_array *array, struct lru_sub *sub,
		 struct lru_entry *entry, uint32_t idx)
{
	if (entry->le_next_idx == sub->ls_lru) {
		/** Already the mru */
		return;
	}

	if (sub->ls_lru == idx) {
		/** Ordering doesn't change in circular list so just update
		 *  the lru and mru idx
		 */
		sub->ls_lru = entry->le_next_idx;
		return;
	}

	/** First remove */
	lrua_remove_entry(array, sub, &sub->ls_lru, entry, idx);

	/** Insert at mru */
	lrua_insert(sub, &sub->ls_lru, entry, idx, true);
}

/** Internal API to lookup entry from index */
static inline struct lru_entry *
lrua_lookup_idx(struct lru_array *array, uint32_t idx, uint64_t key,
		bool touch_mru)
{
	struct lru_entry	*entry;
	struct lru_sub		*sub;
	uint32_t		 ent_idx;

	if (idx >= array->la_count)
		return NULL;

	sub = lrua_idx2sub(array, idx);
	ent_idx = idx & array->la_idx_mask;

	if (sub->ls_table == NULL)
		return NULL;

	entry = &sub->ls_table[ent_idx];
	if (entry->le_key == key) {
		if (touch_mru && !array->la_evicting &&
		    !(array->la_flags & LRU_FLAG_EVICT_MANUAL)) {
			/** Only make mru if we are not evicting it */
			lrua_move_to_mru(array, sub, entry, ent_idx);
		}
		return entry;
	}

	return NULL;
}

/** Lookup an entry in the lru array with alternative key.
 *
 * \param	array[in]	The lru array
 * \param	idx[in]		The index of the entry
 * \param	key[in]		Unique identifier
 * \param	entryp[out]	Valid only if function returns true.
 *
 * \return true if the entry is in the array and set \p entryp accordingly
 */
#define lrua_lookupx(array, idx, key, entryp)	\
	lrua_lookupx_(array, idx, key, (void **)entryp)
static inline bool
lrua_lookupx_(struct lru_array *array, uint32_t idx, uint64_t key,
	      void **entryp)
{
	struct lru_entry	*entry;

	D_ASSERT(array != NULL);
	D_ASSERT(key != 0);

	*entryp = NULL;

	entry = lrua_lookup_idx(array, idx, key, true);
	if (entry == NULL)
		return false;

	*entryp = entry->le_payload;
	return true;
}

/** Lookup an entry in the lru array.
 *
 * \param	array[in]	The lru array
 * \param	idx[in,out]	Address of the record index.
 * \param	entryp[out]	Valid only if function returns true.
 *
 * \return true if the entry is in the array and set \p entryp accordingly
 */
#define lrua_lookup(array, idx, entryp)	\
	lrua_lookup_(array, idx, (void **)entryp)
static inline bool
lrua_lookup_(struct lru_array *array, const uint32_t *idx, void **entryp)
{
	return lrua_lookupx_(array, *idx, (uint64_t)idx, entryp);
}

/** Peek an entry in the lru array with alternative key.
 *
 * \param	array[in]	The lru array
 * \param	idx[in]		The index of the entry
 * \param	idx[in]		Unique identifier
 * \param	entryp[out]	Valid only if function returns true.
 *
 * \return true if the entry is in the array and set \p entryp accordingly
 */
#define lrua_peekx(array, idx, key, entryp)	\
	lrua_peekx_(array, idx, key, (void **)entryp)
static inline bool
lrua_peekx_(struct lru_array *array, uint32_t idx, uint64_t key,
	    void **entryp)
{
	struct lru_entry	*entry;

	D_ASSERT(array != NULL);
	D_ASSERT(key != 0);

	*entryp = NULL;

	entry = lrua_lookup_idx(array, idx, key, false);
	if (entry == NULL)
		return false;

	*entryp = entry->le_payload;
	return true;
}

/** Peek an entry in the lru array.
 *
 * \param	array[in]	The lru array
 * \param	idx[in,out]	Address of the record index.
 * \param	entryp[out]	Valid only if function returns true.
 *
 * \return true if the entry is in the array and set \p entryp accordingly
 */
#define lrua_peek(array, idx, entryp)	\
	lrua_peek_(array, idx, (void **)entryp)
static inline bool
lrua_peek_(struct lru_array *array, const uint32_t *idx, void **entryp)
{
	return lrua_peekx_(array, *idx, (uint64_t)idx, entryp);
}

/** Allocate a new entry lru array with alternate key specifier.
 *  This should only be called if lookup would return false.  This will
 *  modify idx.  If called within a transaction and the value needs to
 *  persist, the old value should be logged before calling this function.
 *
 * \param	array[in]	The LRU array
 * \param	idx[in,out]	Index address in, allocated index out
 * \param	key[in]		Unique identifier of entry
 * \param	entryp[out]	Valid only if function returns success.
 *
 * \return	0		Success, entryp points to new entry
 *		-DER_NOMEM	Memory allocation needed but no memory is
 *				available.
 *		-DER_BUSY	Entries need to be evicted to free up
 *				entries in the table
 */
#define lrua_allocx(array, idx, key, entryp, stub)	\
	lrua_allocx_(array, idx, key, (void **)(entryp), (void **)(stub))
static inline int
lrua_allocx_(struct lru_array *array, uint32_t *idx, uint64_t key,
	     void **entryp, void **stub)
{
	struct lru_entry	*new_entry;
	int			 rc;

	D_ASSERT(entryp != NULL);
	D_ASSERT(array != NULL);
	D_ASSERT(key != 0);
	*entryp = NULL;

	rc = lrua_find_free(array, &new_entry, idx, key);

	if (rc != 0)
		return rc;

	*entryp = new_entry->le_payload;
	if (stub != NULL)
		*stub = new_entry;

	return 0;
}

/** Allocate a new entry lru array.   This should only be called if lookup
 *  would return false.  This will modify idx.  If called within a
 *  transaction and the value needs to persist, the old value should be
 *  logged before calling this function.
 *
 * \param	array[in]	The LRU array
 * \param	idx[in,out]	Address of the entry index.
 * \param	entryp[out]	Valid only if function returns success.
 *
 * \return	0		Success, entryp points to new entry
 *		-DER_NOMEM	Memory allocation needed but no memory is
 *				available.
 *		-DER_BUSY	Entries need to be evicted to free up
 *				entries in the table
 */
#define lrua_alloc(array, idx, entryp)	\
	lrua_alloc_(array, idx, (void **)(entryp))
static inline int
lrua_alloc_(struct lru_array *array, uint32_t *idx, void **entryp)
{
	return lrua_allocx_(array, idx, (uint64_t)idx, entryp, NULL);
}

/** Allocate an entry in place.  Used for recreating an old array.
 *
 * \param	array[in]	The LRU array
 * \param	idx[in]		Index of entry.
 * \param	key[in]		Address of the entry index.
 *
 * \return	0		Success, entryp points to new entry
 *		-DER_NOMEM	Memory allocation needed but no memory is
 *				available.
 *		-DER_NO_PERM	Attempted to overwrite existing entry
 *		-DER_INVAL	Index is not in range of array
 */
#define lrua_allocx_inplace(array, idx, key, entryp)	\
	lrua_allocx_inplace_(array, idx, key, (void **)(entryp))
static inline int
lrua_allocx_inplace_(struct lru_array *array, uint32_t idx, uint64_t key,
		     void **entryp)
{
	struct lru_entry	*entry;
	struct lru_sub		*sub;
	uint32_t		 ent_idx;
	int			 rc;

	D_ASSERT(entryp != NULL);
	D_ASSERT(array != NULL);
	D_ASSERT(key != 0);

	*entryp = NULL;

	if (idx >= array->la_count) {
		D_ERROR("Index %d is out of range\n", idx);
		return -DER_INVAL;
	}

	sub = lrua_idx2sub(array, idx);
	ent_idx = lrua_idx2ent(array, idx);
	if (sub->ls_table == NULL) {
		rc = lrua_array_alloc_one(array, sub);
		if (rc != 0)
			return rc;
		D_ASSERT(sub->ls_table != NULL);
	}

	entry = &sub->ls_table[ent_idx];
	if (entry->le_key != key && entry->le_key != 0) {
		D_ERROR("Cannot allocated idx %d in place\n", idx);
		return -DER_NO_PERM;
	}

	entry->le_key = key;

	/** First remove */
	lrua_remove_entry(array, sub, &sub->ls_free, entry, ent_idx);

	/** Insert at mru */
	lrua_insert(sub, &sub->ls_lru, entry, ent_idx, true);

	*entryp = entry->le_payload;
	return 0;
}

/** If an entry is still in the array, evict it and invoke eviction callback.
 *  Move the evicted entry to the LRU and mark it as already evicted.
 *
 * \param	array[in]	Address of the LRU array.
 * \param	idx[in]		Index of the entry
 * \param	key[in]		Unique identifier
 */
void
lrua_evictx(struct lru_array *array, uint32_t idx, uint64_t key);

/** If an entry is still in the array, evict it and invoke eviction callback.
 *  Move the evicted entry to the LRU and mark it as already evicted.
 *
 * \param	array[in]	Address of the LRU array.
 * \param	idx[in]		Address of the entry index.
 */
static inline void
lrua_evict(struct lru_array *array, uint32_t *idx)
{
	lrua_evictx(array, *idx, (uint64_t)idx);
}

/** Allocate an LRU array
 *
 * \param	array[in,out]	Pointer to LRU array
 * \param	nr_ent[in]	Number of records in array
 * \param	nr_arrays[in]	Number of 2nd level arrays.   If it is not 1,
 *				manual eviction is implied.
 * \param	rec_size[in]	Size of each record
 * \param	cbs[in]		Optional callbacks
 * \param	arg[in]		Optional argument passed to all callbacks
 *
 * \return	-DER_NOMEM	Not enough memory available
 *		0		Success
 */
int
lrua_array_alloc(struct lru_array **array, uint32_t nr_ent, uint32_t nr_arrays,
		 uint16_t rec_size, uint32_t flags,
		 const struct lru_callbacks *cbs, void *arg);

/** Free an LRU array
 *
 * \param	array[in]	Pointer to LRU array
 */
void
lrua_array_free(struct lru_array *array);

/** Aggregate the LRU array
 *
 * Frees up extraneous unused subarrays.   Only applies to arrays with more
 * than 1 sub array.
 */
//Yuanguo: 把全 free 的 subarray 的空间释放掉，并加入 array->la_unused_sub (unallocated)列表；
void
lrua_array_aggregate(struct lru_array *array);

static inline void
lrua_refresh_key(struct lru_entry *entry, uint64_t key)
{
	D_ASSERT(entry != NULL);

	entry->le_key = key;
}

#endif /* __LRU_ARRAY__ */
