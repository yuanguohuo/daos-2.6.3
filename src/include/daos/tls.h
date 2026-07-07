/**
 * (C) Copyright 2016-2024 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of daos
 *
 * src/include/daos/tls.h
 */

#ifndef __DAOS_TLS_H__
#define __DAOS_TLS_H__

#include <daos/common.h>
#include <daos_types.h>

/**
 * Stackable Module API
 * Provides a modular interface to load and register server-side code on
 * demand. A module is composed of:
 * - a set of request handlers which are registered when the module is loaded.
 * - a server-side API (see header files suffixed by "_srv") used for
 *   inter-module direct calls.
 *
 * For now, all loaded modules are assumed to be trustful, but sandboxes can be
 * implemented in the future.
 */
/*
 * Thead-local storage
 */
//Yuanguo:
//  每个类型的 xstream (linux pthread) 都有 TLS，因为每个 xstream 启动时都运行 dss_srv_handler，而 TLS 在那里初始化
//
//      dtc = dss_tls_init(dx->dx_tag, dx->dx_xs_id, dx->dx_tgt_id);
//
//  但，TLS 中存的内容不同：
//      - TLS 内部按 module 存储各自的 module 数据；不是所有 module 都会在所有 xstream 上初始化，
//        通过 dtls_tag 和 dmk_tags 的位掩码匹配控制，见 daos_thread_local_storage_init 函数；
//
//        struct daos_thread_local_storage (TLS 结构, 依附于 xstream/pthread)
//               ├── dtls_tag        ← xstream 的类型标记
//               └── dtls_values[]   ← 各模块的私有数据（按需填充）
//                     ├── [0] pool_tls      ← pool 模块注册了 module key, 匹配后初始化
//                     ├── [1] vos_tls       ← vos 模块注册了 module key, 匹配后初始化
//                     ├── [2] container_tls ← ...
//                     ├── [3] NULL          ← 某模块不匹配, 未初始化
//                     └── ...
//
//      - 各 xstream 创建时的 tag，见 dss_xstreams_init 函数:
//        |----------------------------|-----------------------------------------------|----------------------------|
//        | xstream                    | struct daos_thread_local_storage dtls_tag     | 初始化哪些模块             |
//        |----------------------------|-----------------------------------------------|----------------------------|
//        | daos_sys_0 (xs_id=0)       | DAOS_SERVER_TAG - DAOS_TGT_TAG                | SERVER 类模块，不含 TGT 类 |
//        |----------------------------|-----------------------------------------------|----------------------------|
//        | daos_sys_1 (xs_id=1)       | DAOS_SERVER_TAG - DAOS_TGT_TAG - DAOS_RDB_TAG | SERVER 类，不含 TGT 和 RDB |
//        |----------------------------|-----------------------------------------------|----------------------------|
//        | daos_sys_2 (xs_id=2, dRPC) | DAOS_SERVER_TAG - DAOS_TGT_TAG - DAOS_RDB_TAG | 同上                       |
//        |----------------------------|-----------------------------------------------|----------------------------|
//        | target main I/O xstream    | DAOS_SERVER_TAG (0xff)                        | 所有模块，包括 TGT         |
//        |----------------------------|-----------------------------------------------|----------------------------|
//        | offload xstream            | DAOS_OFF_TAG                                  | 只有 OFF 类模块            |
//        |----------------------------|-----------------------------------------------|----------------------------|
//
//      - 模块的 (struct daos_module_key) 的 dmk_tags 也可以包含多个 bit
//          - 一个 xstream 有多个 tls value (多个module，它们的 dmk_tags 与 xstream 的 dtls_tag 有至少一个 bit 相同)
//          - 多个不同类型（sys, I/O, offload）的 xstream，可能有相同类型的 tls value，见下面举例。
//
//  举例：
//        |------------|--------------------|------------------------------------|----------------------|-----------------|-------------------|
//        | 模块       | tls value 类型     | dmk_tags                           | daos_sys_0 (0xFD)    | main I/O (0xFF) | offload (0x08)    |
//        |------------|--------------------|------------------------------------|----------------------|-----------------|-------------------|
//        | VOS        | struct vos_tls     | DAOS_RDB_TAG + DAOS_TGT_TAG (0x06) | 0xFD & 0x06 = 0x04 ✓ | 0xFF & 0x06 = ✓ | 0x08 & 0x06 = 0 ✗ |
//        |------------|--------------------|------------------------------------|----------------------|-----------------|-------------------|
//        | rebuild    | struct rebuild_tls | DAOS_SERVER_TAG (0xFF)             | 0xFD & 0xFF = ✓      | 0xFF & 0xFF = ✓ | 0x08 & 0xFF = ✓   |
//        |------------|--------------------|------------------------------------|----------------------|-----------------|-------------------|
//        | pool (srv) | struct pool_tls    | DAOS_SERVER_TAG (0xFF)             | ✓                    | ✓               | ✓                 |
//        |------------|--------------------|------------------------------------|----------------------|-----------------|-------------------|
struct daos_thread_local_storage {
	uint32_t dtls_tag;
	void   **dtls_values;
};

enum daos_module_tag {
	DAOS_SYS_TAG    = 1 << 0, /** only run on system xstream */
	DAOS_TGT_TAG    = 1 << 1, /** only run on target xstream */
	DAOS_RDB_TAG    = 1 << 2, /** only run on rdb xstream */
	DAOS_OFF_TAG    = 1 << 3, /** only run on offload/helper xstream */
	DAOS_CLI_TAG    = 1 << 4, /** only run on client stack */
	DAOS_SERVER_TAG = 0xff,   /** run on all xstream */
};

/* The module key descriptor for each xstream */
struct daos_module_key {
	/* Indicate where the keys should be instantiated */
	enum daos_module_tag dmk_tags;

	/* The position inside the daos_module_keys */
	int                  dmk_index;
	/* init keys for context */
	void *(*dmk_init)(int tags, int xs_id, int tgt_id);

	/* fini keys for context */
	void (*dmk_fini)(int tags, void *data);
};

#define DAOS_MODULE_KEYS_NR 10
struct daos_thread_local_storage *
dss_tls_get(void);
struct daos_thread_local_storage *
dc_tls_get(unsigned int tag);

int
ds_tls_key_create(void);
int
dc_tls_key_create(void);
void
ds_tls_key_delete(void);
void
dc_tls_key_delete(void);

struct daos_module_key *
daos_get_module_key(int index);

/**
 * Get value from context by the key
 *
 * Get value inside dtls by key. So each module will use this API to
 * retrieve their own value in the thread context.
 *
 * \param[in] dtls	the thread context.
 * \param[in] key	key used to retrieve the dtls_value.
 *
 * \retval		the dtls_value retrieved by key.
 */
static inline void *
daos_module_key_get(struct daos_thread_local_storage *dtls, struct daos_module_key *key)
{
	D_ASSERT(key->dmk_index >= 0);
	D_ASSERT(key->dmk_index < DAOS_MODULE_KEYS_NR);
	D_ASSERT(daos_get_module_key(key->dmk_index) == key);
	D_ASSERT(dtls != NULL);

	return dtls->dtls_values[key->dmk_index];
}

#define dss_module_key_get       daos_module_key_get
#define dss_register_key         daos_register_key
#define dss_unregister_key       daos_unregister_key
#define dss_module_info          daos_module_info
#define dss_module_tag           daos_module_tag
#define dss_module_key           daos_module_key
#define dss_thread_local_storage daos_thread_local_storage

void
daos_register_key(struct daos_module_key *key);
void
daos_unregister_key(struct daos_module_key *key);
struct daos_thread_local_storage *
dc_tls_init(int tag, uint32_t pid);
void
dc_tls_fini(void);
struct daos_thread_local_storage *
dss_tls_init(int tag, int xs_id, int tgt_id);
void
dss_tls_fini(struct daos_thread_local_storage *dtls);

#endif /*__DAOS_TLS_H__*/
