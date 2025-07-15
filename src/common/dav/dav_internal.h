/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2015-2022, Intel Corporation */

/*
 * dav_flags.h -- Interfaces exported by DAOS internal Allocator for VOS (DAV)
 */

#ifndef __DAOS_COMMON_DAV_INTERNAL_H
#define __DAOS_COMMON_DAV_INTERNAL_H 1

#include "dav.h"
#include "dav_clogs.h"
#include "heap.h"
#include "mo_wal.h"
#include "wal_tx.h"

#define DAV_MAX_ALLOC_SIZE ((size_t)0x3FFDFFFC0)

enum dav_tx_failure_behavior {
	DAV_TX_FAILURE_ABORT,
	DAV_TX_FAILURE_RETURN,
};

enum dav_stats_enabled {
	DAV_STATS_ENABLED_TRANSIENT,
	DAV_STATS_ENABLED_BOTH,
	DAV_STATS_ENABLED_PERSISTENT,
	DAV_STATS_DISABLED,
};

enum dav_arenas_assignment_type {
	DAV_ARENAS_ASSIGNMENT_THREAD_KEY,
	DAV_ARENAS_ASSIGNMENT_GLOBAL,
};

#define	DAV_PHDR_SIZE	4096

/* DAV header data that will be persisted */
struct dav_phdr {
	uint64_t		dp_uuid_lo;
	uint64_t		dp_heap_offset;
	uint64_t		dp_heap_size;
	uint64_t		dp_root_offset;
	uint64_t		dp_root_size;
    //Yuanguo: dp_stats_persistent.heap_curr_allocated 可以通过:
    //       xxd -s 40 -l 8  /mnt/daos0/c090c2fc-8d83-45de-babe-104bad165593/vos-0
    // 查看(40表示跳过前面5个uint64_t)
    // 注意：这可能是实际使用的空间!!! 从客户端发起写操作时，这个值在变化!!!
	struct stats_persistent dp_stats_persistent;
	char	 dp_unused[DAV_PHDR_SIZE - sizeof(uint64_t)*5 -
			sizeof(struct stats_persistent)];
};

//Yuanguo: 代表一个memory pool，等价于PMEM模式的 PMEMobjpool (struct pmemobjpool)
/* DAV object handle */
typedef struct dav_obj {
    //Yuanguo: do_path: "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-0" (on tmpfs)
	char				*do_path;
    //Yuanguo: do_size: do_path文件大小 - blob-header-size(默认1*4k) (注意不是struct dav_phdr)
	uint64_t			 do_size;
    //Yuanguo: do_path文件通过mmap映射到内存，do_base是mmap返回的内存地址，对应do_path文件的起始位置；
	void				*do_base;
    //Yuanguo: 把do_path文件的内存映射组织成header + zone-list layout；do_heap指向这个layout；
	struct palloc_heap		*do_heap;
    //Yuanguo: do_phdr: 指向mmap映射空间中的struct dav_phdr (实际上，就在mmap空间的起始处，即等于do_base，对应do_path文件的起始)
	struct dav_phdr			*do_phdr;
	struct operation_context	*external;
	struct operation_context	*undo;
	struct mo_ops			 p_ops;	/* REVISIT */
    //Yuanguo: do_stats->stats_persistent指向do_phdr->dp_stats_persistent; 见dav_obj_open_internal()->stats_new()
	struct stats			*do_stats;
    //Yuanguo: do_fd: do_path文件的描述符；
	int				 do_fd;
	int				 nested_tx;
    //Yuanguo: transaction的内存表示；
    //  对于MD-on-SSD: do_utx->utx_private是一个指向struct dav_tx对象的指针；对象中包含一个struct umem_action的链表；
    //                 do_utx->utx_id是transaction id;
	struct umem_wal_tx		*do_utx;
    //Yuanguo:  *do_store = {
    //                        .stor_blk_size=4k
    //                        .stor_hdr_blks=1
    //                        .stor_size = do_path文件的大小 - blob-header-size(1*4k) (注意不是struct dav_phdr)
    //                        .store_type = DAOS_MD_BMEM
    //                        .stor_priv: 指向struct bio_meta_context对象，里面是meta/wal blob的spdk blob id等；
    //                      }
	struct umem_store               *do_store;

	struct dav_clogs		 clogs __attribute__ ((__aligned__(CACHELINE_SIZE)));
} dav_obj_t;

static inline
struct dav_tx *utx2wtx(struct umem_wal_tx *utx)
{
	return (struct dav_tx *)&utx->utx_private;
}

static inline
struct umem_wal_tx *wtx2utx(struct dav_tx *wtx)
{
	return (struct umem_wal_tx *)((void *)wtx
			- (ptrdiff_t)offsetof(struct umem_wal_tx, utx_private));
}

int lw_tx_begin(dav_obj_t *pop);
int lw_tx_end(dav_obj_t *pop, void *data);

#endif /* __DAOS_COMMON_DAV_INTERNAL_H */
