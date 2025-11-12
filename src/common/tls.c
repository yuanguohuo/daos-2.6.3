/**
 * (C) Copyright 2016-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * It implements thread-local storage (TLS) for DAOS.
 */
#include <pthread.h>
#include <daos/tls.h>

/* The array remember all of registered module keys on one node. */
static struct daos_module_key *daos_module_keys[DAOS_MODULE_KEYS_NR] = {NULL};
pthread_mutex_t                daos_module_keys_lock                 = PTHREAD_MUTEX_INITIALIZER;

static __thread bool           dc_tls_thread_init;

//Yuanguo: 关于pthread_key_t的用法(以dss_tls_key为例):
//  - 它必须通过系统调用pthread_key_create()初始化；使用完，通过系统调用pthread_key_delete()销毁；
//  - 它是一个全局变量，即一个进程一份；
//  - 但通过它，实现thread-specific，如下：
//      - 系统调用pthread_setspecific(dss_tls_key, const void *value); 为“calling thread”的dss_tls_key设置一个value；
//        注意：虽然dss_tls_key是进程全局的，但设置的value却是“calling thread”自己的；也就是说，不同thread都可以调用
//        这个函数设置自己thread-specific的value；
//           thread-A:  pthread_setspecific(dss_tls_key, val_A);
//           thread-B:  pthread_setspecific(dss_tls_key, val_B);
//           ...
//        针对dss_tls_key，系统为每个thread维护一个value;
//      - 系统调用pthread_getspecific(dss_tls_key);的功能显而易见，返回“calling thread”自己的那个value:
//           thread-A:  pthread_getspecific(dss_tls_key);返回val_A;
//           thread-B:  pthread_getspecific(dss_tls_key);返回val_B;
//           ...
//  - 总之，虽然pthread_key_t dss_tls_key是一个全局变量，但它背后确像一个map<thread_id, void*>;

//Yuanguo: 注意和 "__thread"的区别(例如上面的static __thread bool dc_tls_thread_init;)
//  - __thread 是GCC编译器提供的一个扩展关键字，用于声明**线程局部存储（Thread-Local Storage, TLS）**的变量
//  - __thread 标记的变量，不是一个“全局变量”，而是“线程内全局”,即Thread-Local；
//  - 支持GCC扩展的环境下、高频访问、编译期确定的简单数据用__thread方式；
//  - 需要跨平台，低频访问，有复杂生命周期数据使用pthread_setspecific/pthread_getspecific方式；
//  - 这里dc_tls_thread_init(__thread)和dc_tls_key配合使用；

//Yuanguo: dss_tls_key用于server端；dc_tls_key用于client端；
//  它们不可能在同一个进程中，为什么不用同一个变量呢？
static pthread_key_t           dss_tls_key;
static pthread_key_t           dc_tls_key;

//Yuanguo:
//        dss_tls_key -----> +-------------------------------------+           struct daos_thread_local_storage
//              thread-A     | struct daos_thread_local_storage *  | -----> +------------------------------------+
//                           +-------------------------------------+        |  uint32_t dtls_tag;                |
//              thread-B     | struct daos_thread_local_storage *  |        |  void   **dtls_values; -----+      |
//                           +-------------------------------------+        +-----------------------------|------+
//              ......       | struct daos_thread_local_storage *  |                                      |
//                           +-------------------------------------+               +----------------------+
//                                                                                 |
//                                                                                 V
//                                                                                 +-----+-----+-----+-----+
//                                                                                 |void*|void*|void*| ... |DAOS_MODULE_KEYS_NR个
//                                                                                 +-----+-----+-----+-----+
//                                                                                    |     |     |
//                                                                                    |     |     V
//                                                                                    |     V    ...
//                                                                                    V   通过daos_module_keys[1]->dmk_init初始化；
//                                                                                 通过daos_module_keys[0]->dmk_init初始化；
void
daos_register_key(struct daos_module_key *key)
{
	int i;

	D_MUTEX_LOCK(&daos_module_keys_lock);
	for (i = 0; i < DAOS_MODULE_KEYS_NR; i++) {
		if (daos_module_keys[i] == NULL) {
			daos_module_keys[i] = key;
			key->dmk_index      = i;
			break;
		}
	}
	D_MUTEX_UNLOCK(&daos_module_keys_lock);
	D_ASSERT(i < DAOS_MODULE_KEYS_NR);
}

void
daos_unregister_key(struct daos_module_key *key)
{
	if (key == NULL)
		return;
	D_ASSERT(key->dmk_index >= 0);
	D_ASSERT(key->dmk_index < DAOS_MODULE_KEYS_NR);
	D_MUTEX_LOCK(&daos_module_keys_lock);
	daos_module_keys[key->dmk_index] = NULL;
	D_MUTEX_UNLOCK(&daos_module_keys_lock);
}

struct daos_module_key *
daos_get_module_key(int index)
{
	D_ASSERT(index < DAOS_MODULE_KEYS_NR);
	D_ASSERT(index >= 0);

	return daos_module_keys[index];
}

static int
daos_thread_local_storage_init(struct daos_thread_local_storage *dtls, int xs_id, int tgt_id)
{
	int rc = 0;
	int i;

	if (dtls->dtls_values == NULL) {
		D_ALLOC_ARRAY(dtls->dtls_values, DAOS_MODULE_KEYS_NR);
		if (dtls->dtls_values == NULL)
			return -DER_NOMEM;
	}

	for (i = 0; i < DAOS_MODULE_KEYS_NR; i++) {
		struct daos_module_key *dmk = daos_module_keys[i];

		if (dmk != NULL && dtls->dtls_tag & dmk->dmk_tags) {
			D_ASSERT(dmk->dmk_init != NULL);
			dtls->dtls_values[i] = dmk->dmk_init(dtls->dtls_tag, xs_id, tgt_id);
			if (dtls->dtls_values[i] == NULL) {
				rc = -DER_NOMEM;
				break;
			}
		}
	}
	return rc;
}

static void
daos_thread_local_storage_fini(struct daos_thread_local_storage *dtls)
{
	int i;

	if (dtls->dtls_values != NULL) {
		for (i = DAOS_MODULE_KEYS_NR - 1; i >= 0; i--) {
			struct daos_module_key *dmk = daos_module_keys[i];

			if (dmk != NULL && dtls->dtls_tag & dmk->dmk_tags) {
				D_ASSERT(dtls->dtls_values[i] != NULL);
				D_ASSERT(dmk->dmk_fini != NULL);
				dmk->dmk_fini(dtls->dtls_tag, dtls->dtls_values[i]);
			}
		}
	}

	D_FREE(dtls->dtls_values);
}

/*
 * Allocate daos_thread_local_storage for a particular thread on server and
 * store the pointer in a thread-specific value which can be fetched at any
 * time with daos_tls_get().
 */
static struct daos_thread_local_storage *
daos_tls_init(int tag, int xs_id, int tgt_id, bool server)
{
	struct daos_thread_local_storage *dtls;
	int                               rc;

	D_ALLOC_PTR(dtls);
	if (dtls == NULL)
		return NULL;

	dtls->dtls_tag = tag;
	rc             = daos_thread_local_storage_init(dtls, xs_id, tgt_id);
	if (rc != 0) {
		D_FREE(dtls);
		return NULL;
	}

	if (server) {
		rc = pthread_setspecific(dss_tls_key, dtls);
	} else {
		rc = pthread_setspecific(dc_tls_key, dtls);
		if (rc == 0)
			dc_tls_thread_init = true;
	}

	if (rc) {
		D_ERROR("failed to initialize tls: %d\n", rc);
		daos_thread_local_storage_fini(dtls);
		D_FREE(dtls);
		return NULL;
	}

	return dtls;
}

int
ds_tls_key_create(void)
{
	return pthread_key_create(&dss_tls_key, NULL);
}

int
dc_tls_key_create(void)
{
	return pthread_key_create(&dc_tls_key, NULL);
}

void
ds_tls_key_delete()
{
	pthread_key_delete(dss_tls_key);
}

void
dc_tls_key_delete(void)
{
	pthread_key_delete(dc_tls_key);
}

/* Free DTC for a particular thread. */
static void
daos_tls_fini(struct daos_thread_local_storage *dtls, bool server)
{
	daos_thread_local_storage_fini(dtls);
	D_FREE(dtls);
	if (server)
		pthread_setspecific(dss_tls_key, NULL);
	else
		pthread_setspecific(dc_tls_key, NULL);
}

/* Allocate local per thread storage. */
struct daos_thread_local_storage *
dc_tls_init(int tag, uint32_t pid)
{
	return daos_tls_init(tag, -1, pid, false);
}

/* Free DTC for a particular thread. */
void
dc_tls_fini(void)
{
	struct daos_thread_local_storage *dtls;

	dtls = (struct daos_thread_local_storage *)pthread_getspecific(dc_tls_key);
	if (dtls != NULL)
		daos_tls_fini(dtls, false);
}

struct daos_thread_local_storage *
dc_tls_get(unsigned int tag)
{
	if (!dc_tls_thread_init)
		return dc_tls_init(tag, getpid());

	return (struct daos_thread_local_storage *)pthread_getspecific(dc_tls_key);
}

struct daos_thread_local_storage *
dss_tls_get()
{
	return (struct daos_thread_local_storage *)pthread_getspecific(dss_tls_key);
}

/* Allocate local per thread storage. */
struct daos_thread_local_storage *
dss_tls_init(int tag, int xs_id, int tgt_id)
{
	return daos_tls_init(tag, xs_id, tgt_id, true);
}

/* Free DTC for a particular thread. */
void
dss_tls_fini(struct daos_thread_local_storage *dtls)
{
	daos_tls_fini(dtls, true);
}
