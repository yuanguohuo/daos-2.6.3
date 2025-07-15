/**
 * (C) Copyright 2022-2024 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef __BIO_WAL_H__
#define __BIO_WAL_H__

#include "bio_internal.h"

enum meta_hdr_flags {
	META_HDR_FL_EMPTY	= (1UL << 0),
};

/* Meta blob header */
//  MD-on-SSD情况下，创建pool:
//
//     dmg pool create --scm-size=200G --nvme-size=3000G --properties rd_fac:2 ensblock
//
//     上述200G,3000G都是单个engine的配置；
//     假设单engine的target数是18
//     假设pool的uuid是c090c2fc-8d83-45de-babe-104bad165593 (可以通过dmg pool list -v查看)
//
//     bio_mc_create() --> meta_format()函数初始化...
//
//         mh_magic : BIO_META_MAGIC
//
//         mh_meta_devid : meta spdk-blobstore 或其所在的nvme ssd的id
//         mh_wal_devid  : wal spdk-blobstore 或其所在的nvme ssd的id
//         mh_data_devid : data spdk-blobstore 或其所在的nvme ssd的id
//
//         mh_meta_blobid :   spdk blobstore中meta blob的id (真实类型是spdk_blob_id)
//         mh_wal_blobid  :   spdk blobstore中wal blob的id (真实类型是spdk_blob_id)
//         mh_data_blobid :   spdk blobstore中data blob的id (真实类型是spdk_blob_id)
//
//         mh_blk_bytes : 4096
//         mh_hdr_blks  : 1
//         mh_tot_blks  : 200G/18/mh_blk_bytes - mh_hdr_blks 即总blocks - header-blocks
//
//         mh_vos_id: target_id；例如单个engine有18个target，这个数就是[0,17]中的一个；
//                               每个pool在每个target上有一个vos；
struct meta_header {
	uint32_t	mh_magic;
	uint32_t	mh_version;
	uuid_t		mh_meta_devid;		/* Meta SSD device ID */
	uuid_t		mh_wal_devid;		/* WAL SSD device ID */
	uuid_t		mh_data_devid;		/* Data SSD device ID */
	uint64_t	mh_meta_blobid;		/* Meta blob ID */
	uint64_t	mh_wal_blobid;		/* WAL blob ID */
	uint64_t	mh_data_blobid;		/* Data blob ID */
	uint32_t	mh_blk_bytes;		/* Block size for meta, in bytes */
	uint32_t	mh_hdr_blks;		/* Meta blob header size, in blocks */
	uint64_t	mh_tot_blks;		/* Meta blob capacity, in blocks */
	uint32_t	mh_vos_id;		/* Associated per-engine target ID */
	uint32_t	mh_flags;		/* Meta header flags */
	uint32_t	mh_padding[5];		/* Reserved */
	uint32_t	mh_csum;		/* Checksum of this header */
};

enum wal_hdr_flags {
	WAL_HDR_FL_NO_TAIL	= (1 << 0),	/* No tail checksum */
};

/* WAL blob header */
struct wal_header {
	uint32_t	wh_magic;
	uint32_t	wh_version;
	uint32_t	wh_gen;		/* WAL re-format timestamp */
	uint16_t	wh_blk_bytes;	/* WAL block size in bytes, usually 4k */
	uint16_t	wh_flags;	/* WAL header flags */
	uint64_t	wh_tot_blks;	/* WAL blob capacity, in blocks */
	uint64_t	wh_ckp_id;	/* Last check-pointed transaction ID */
	uint64_t	wh_commit_id;	/* Last committed transaction ID */
	uint32_t	wh_ckp_blks;	/* blocks used by last check-pointed transaction */
	uint32_t	wh_commit_blks;	/* blocks used by last committed transaction */
	uint64_t	wh_padding2;	/* Reserved */
	uint32_t	wh_padding3;	/* Reserved */
	uint32_t	wh_csum;	/* Checksum of this header */
};

/*
 * WAL transaction starts with a header (wal_trans_head), and there will one or multiple
 * entries (wal_trans_entry) being placed immediately after the header, the payload data
 * from entries are concatenated after the last entry, the tail (wal_trans_tail) will be
 * placed after the payload (or the last entry when there is no payload).
 *
 * When the transaction spans multiple WAL blocks, the header will be duplicated to the
 * head of each block.
 */

/* WAL transaction header */
struct wal_trans_head {
	uint32_t	th_magic;
	uint32_t	th_gen;		/* WAL re-format timestamp */
	uint64_t	th_id;		/* Transaction ID */
	uint32_t	th_tot_ents;	/* Total entries */
	uint32_t	th_tot_payload;	/* Total payload size in bytes */
} __attribute__((packed));

/* WAL transaction entry */
struct wal_trans_entry {
	uint64_t	te_off;		/* Offset within meta blob, in bytes */
	uint32_t	te_len;		/* Data length in bytes */
	uint32_t	te_data;	/* Various inline data */
	uint16_t	te_type;	/* Operation type, see UMEM_ACT_XXX */
} __attribute__((packed));

/* WAL transaction tail */
struct wal_trans_tail {
	uint32_t	tt_csum;	/* Checksum of WAL transaction */
} __attribute__((packed));

/* In-memory WAL super information */
struct wal_super_info {
	struct wal_header	si_header;	/* WAL blob header */
	uint64_t		si_ckp_id;	/* Last check-pointed ID */
	uint64_t		si_commit_id;	/* Last committed ID */
	uint32_t		si_ckp_blks;	/* Blocks used by last check-pointed ID */
	uint32_t		si_commit_blks;	/* Blocks used by last committed ID */
    //Yuanguo: tx_id分配器。
    // 注意1：tx_id并不是连续递增的，因为si_unused_id不是一个计数器。具体是这样的：
    //   - si_unused_id分为2个32-bits；
    //       - 高32-bits(WAL_ID_SEQ_BITS)叫做seq(sequence-no)；
    //       - 低32-bits(WAL_ID_OFF_BITS)叫做off(offset);
    //   - off表示一个transaction在wal中所处的block号；
    //   - seq每当wal写满回绕时递增1；
    // 见wal_next_id()函数;
    //
    // 假如：
    //   - wal的block总数是1024 (即si_header.wh_tot_blks=1024)
    //   - si_unused_id = 3 << 32 | 1000;  即seq=3, off=1000;
    // 那么：
    //   - T1: txA reserve id: 3 << 32 | 1000；假设txA的size是20个block；si_unused_id更新为：3 << 32 | 1020
    //   - T2: txB reserve id: 3 << 32 | 1020；假设txB的size是10个block；si_unused_id更新为：4 << 32 | 6
    //   - T3: txC reserve id: 4 << 32 | 6 ...
    //
    // 注意2：允许seq溢出，程序能过识别出这种情况，见wal_id_cmp()函数；
    //
    // 可以把wal看作一个无限长的序列(回绕以及seq溢出情况已被妥善处理)，一个transaction或一个checkpoint都对应唯一一个点
    //
    //                               si_ckp_id
    //    txA        txB               txC     si_ckp_blks        txD            txE      si_unused_id
    //     |  txA-sz  |      txB-sz     |        txC-sz            |    txD-sz    |  txE-sz  |
    //  ------------------------------------------------------------------------------------------>
	uint64_t                si_unused_id;   /* Next unused ID */
	d_list_t		si_pending_list;/* Pending transactions */
	ABT_cond		si_rsrv_wq;	/* FIFO waitqueue for WAL ID reserving */
	ABT_mutex		si_mutex;	/* For si_rsrv_wq */
	unsigned int		si_rsrv_waiters;/* Number of waiters in reserve waitqueue */
	unsigned int		si_pending_tx;	/* Number of pending transactions */
	unsigned int		si_tx_failed:1;	/* Indicating some transaction failed */
};

/* In-memory Meta context, exported as opaque data structure */
struct bio_meta_context {
	struct bio_io_context	*mc_data;	/* Data blob I/O context */
	struct bio_io_context	*mc_meta;	/* Meta blob I/O context */
	struct bio_io_context	*mc_wal;	/* WAL blob I/O context */
	struct meta_header	 mc_meta_hdr;	/* Meta blob header */
	struct wal_super_info	 mc_wal_info;	/* WAL blob super information */
	struct hash_ft		*mc_csum_algo;
	void			*mc_csum_ctx;
};

struct meta_fmt_info {
	uuid_t		fi_pool_id;		/* Pool UUID */
	uuid_t		fi_meta_devid;		/* Meta SSD device ID */
	uuid_t		fi_wal_devid;		/* WAL SSD device ID */
	uuid_t		fi_data_devid;		/* Data SSD device ID */
	uint64_t	fi_meta_blobid;		/* Meta blob ID */
	uint64_t	fi_wal_blobid;		/* WAL blob ID */
	uint64_t	fi_data_blobid;		/* Data blob ID */
	uint64_t	fi_meta_size;		/* Meta blob size in bytes */
	uint64_t	fi_wal_size;		/* WAL blob size in bytes */
	uint64_t	fi_data_size;		/* Data blob size in bytes */
	uint32_t	fi_vos_id;		/* Associated per-engine target ID */
};

int meta_format(struct bio_meta_context *mc, struct meta_fmt_info *fi, bool force);
int meta_open(struct bio_meta_context *mc);
void meta_close(struct bio_meta_context *mc);
int wal_open(struct bio_meta_context *mc);
void wal_close(struct bio_meta_context *mc);

#endif /* __BIO_WAL_H__*/
