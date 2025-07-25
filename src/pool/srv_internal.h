/*
 * (C) Copyright 2016-2024 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * ds_pool: Pool Server Internal Declarations
 */

#ifndef __POOL_SRV_INTERNAL_H__
#define __POOL_SRV_INTERNAL_H__

#include <gurt/list.h>
#include <daos/pool_map.h>
#include <daos_srv/daos_engine.h>
#include <daos_security.h>
#include <gurt/telemetry_common.h>

extern uint32_t pw_rf;
extern uint32_t ps_cache_intvl;

/**
 * Global pool metrics
 */
struct pool_metrics {
	struct d_tm_node_t	*connect_total;
	struct d_tm_node_t	*disconnect_total;
	struct d_tm_node_t	*query_total;
	struct d_tm_node_t	*query_space_total;
	struct d_tm_node_t	*evict_total;

	/* service metrics */
	struct d_tm_node_t      *service_leader;
	struct d_tm_node_t      *map_version;
	struct d_tm_node_t      *open_handles;
	struct d_tm_node_t      *total_targets;
	struct d_tm_node_t      *disabled_targets;
	struct d_tm_node_t      *draining_targets;
	struct d_tm_node_t      *total_ranks;
	struct d_tm_node_t      *degraded_ranks;
};

/* Pool thread-local storage */
struct pool_tls {
	struct d_list_head	dt_pool_list;	/* of ds_pool_child objects */
};

extern struct dss_module_key pool_module_key;

static inline struct pool_tls *
pool_tls_get()
{
	struct pool_tls			*tls;
	struct dss_thread_local_storage	*dtc;

	dtc = dss_tls_get();
	tls = dss_module_key_get(dtc, &pool_module_key);
	return tls;
}

static inline bool
ds_pool_skip_for_check(struct ds_pool *pool)
{
	return engine_in_check() && !pool->sp_cr_checked;
}

struct pool_iv_map {
	d_rank_t	piv_master_rank;
	uint32_t	piv_pool_map_ver;
	struct pool_buf	piv_pool_buf;
};

/* The structure to serialize the prop for IV */
struct pool_iv_prop {
	char		pip_label[DAOS_PROP_MAX_LABEL_BUF_LEN];
	char		pip_owner[DAOS_ACL_MAX_PRINCIPAL_BUF_LEN];
	char		pip_owner_grp[DAOS_ACL_MAX_PRINCIPAL_BUF_LEN];
	uint64_t	pip_data_thresh;
	uint64_t	pip_space_rb;
	uint64_t	pip_self_heal;
	uint64_t	pip_scrub_mode;
	uint64_t	pip_scrub_freq;
	uint64_t	pip_scrub_thresh;
	uint64_t	pip_reclaim;
	uint64_t	pip_ec_cell_sz;
	uint64_t	pip_redun_fac;
	uint32_t	pip_ec_pda;
	uint32_t	pip_rp_pda;
	uint32_t	pip_global_version;
	uint32_t	pip_upgrade_status;
	uint64_t	pip_svc_redun_fac;
	uint32_t	pip_checkpoint_mode;
	uint32_t	pip_checkpoint_freq;
	uint32_t	pip_checkpoint_thresh;
	uint32_t	pip_obj_version;
	struct daos_acl	*pip_acl;
	d_rank_list_t   pip_svc_list;
	uint32_t	pip_acl_offset;
	uint32_t	pip_svc_list_offset;
	uint32_t	pip_perf_domain;
	uint32_t	pip_reint_mode;
	uint32_t         pip_svc_ops_enabled;
	uint32_t         pip_svc_ops_entry_age;
	char		pip_iv_buf[0];
};

struct pool_iv_conn {
	uuid_t		pic_hdl;
	uint64_t	pic_flags;
	uint64_t	pic_capas;
	uint32_t	pic_cred_size;
	uint32_t	pic_global_ver;
	uint32_t	pic_obj_ver;
	char		pic_creds[0];
};

struct pool_iv_conns {
	uint32_t		pic_size;
	uint32_t		pic_buf_size;
	struct pool_iv_conn	pic_conns[0];
};

struct pool_iv_key {
	uuid_t		pik_uuid;
	uint32_t	pik_entry_size; /* IV entry size */
	daos_epoch_t	pik_eph;
	uint64_t	pik_term;
};

struct pool_iv_hdl {
	uuid_t		pih_pool_hdl;
	uuid_t		pih_cont_hdl;
};

struct pool_iv_entry {
	union {
		struct pool_iv_map	piv_map;
		struct pool_iv_prop	piv_prop;
		struct pool_iv_hdl	piv_hdl;
		struct pool_iv_conns	piv_conn_hdls;
	};
};

struct pool_map_refresh_ult_arg {
	uint32_t	iua_pool_version;
	uuid_t		iua_pool_uuid;
	ABT_eventual	iua_eventual;
};

/*
 * srv_pool.c
 */
void ds_pool_rsvc_class_register(void);
void ds_pool_rsvc_class_unregister(void);
uint32_t ds_pool_get_vos_df_version(uint32_t pool_global_version);
char *ds_pool_svc_rdb_path(const uuid_t pool_uuid);
int ds_pool_svc_load(struct rdb_tx *tx, uuid_t uuid, rdb_path_t *root, uint32_t *global_version_out,
		     struct pool_buf **map_buf_out, uint32_t *map_version_out);
int ds_pool_svc_start(uuid_t uuid);
int ds_pool_svc_stop(uuid_t pool_uuid);
int ds_pool_start_all(void);
int ds_pool_stop_all(void);
int ds_pool_hdl_is_from_srv(struct ds_pool *pool, uuid_t hdl);
int ds_pool_svc_upgrade_vos_pool(struct ds_pool *pool);
void ds_pool_create_handler(crt_rpc_t *rpc);
void
     ds_pool_connect_handler_v6(crt_rpc_t *rpc);
void ds_pool_connect_handler_v5(crt_rpc_t *rpc);
void
ds_pool_disconnect_handler_v6(crt_rpc_t *rpc);
void
ds_pool_disconnect_handler_v5(crt_rpc_t *rpc);
void
     ds_pool_query_handler_v6(crt_rpc_t *rpc);
void ds_pool_query_handler_v5(crt_rpc_t *rpc);
void ds_pool_prop_get_handler(crt_rpc_t *rpc);
void ds_pool_prop_set_handler(crt_rpc_t *rpc);
void ds_pool_acl_update_handler(crt_rpc_t *rpc);
void ds_pool_acl_delete_handler(crt_rpc_t *rpc);
void
ds_pool_update_handler_v6(crt_rpc_t *rpc);
void
     ds_pool_update_handler_v5(crt_rpc_t *rpc);
void ds_pool_extend_handler(crt_rpc_t *rpc);
void ds_pool_evict_handler(crt_rpc_t *rpc);
void
ds_pool_svc_stop_handler_v6(crt_rpc_t *rpc);
void
ds_pool_svc_stop_handler_v5(crt_rpc_t *rpc);
void
ds_pool_attr_list_handler_v6(crt_rpc_t *rpc);
void
ds_pool_attr_list_handler_v5(crt_rpc_t *rpc);
void
ds_pool_attr_get_handler_v6(crt_rpc_t *rpc);
void
ds_pool_attr_get_handler_v5(crt_rpc_t *rpc);
void
ds_pool_attr_set_handler_v6(crt_rpc_t *rpc);
void
ds_pool_attr_set_handler_v5(crt_rpc_t *rpc);
void
ds_pool_attr_del_handler_v6(crt_rpc_t *rpc);
void
ds_pool_attr_del_handler_v5(crt_rpc_t *rpc);
void
ds_pool_list_cont_handler_v6(crt_rpc_t *rpc);
void
ds_pool_list_cont_handler_v5(crt_rpc_t *rpc);
void
ds_pool_filter_cont_handler_v6(crt_rpc_t *rpc);
void
ds_pool_filter_cont_handler_v5(crt_rpc_t *rpc);
void
ds_pool_query_info_handler_v6(crt_rpc_t *rpc);
void
     ds_pool_query_info_handler_v5(crt_rpc_t *rpc);
void ds_pool_ranks_get_handler(crt_rpc_t *rpc);
void ds_pool_upgrade_handler(crt_rpc_t *rpc);

/*
 * srv_target.c
 */
int ds_pool_cache_init(void);
void ds_pool_cache_fini(void);
int ds_pool_lookup_internal(const uuid_t uuid, struct ds_pool **pool);
int ds_pool_hdl_hash_init(void);
void ds_pool_hdl_hash_fini(void);
void ds_pool_tgt_disconnect_handler(crt_rpc_t *rpc);
int ds_pool_tgt_disconnect_aggregator(crt_rpc_t *source, crt_rpc_t *result,
				      void *priv);
void ds_pool_tgt_query_handler(crt_rpc_t *rpc);
int ds_pool_tgt_query_aggregator(crt_rpc_t *source, crt_rpc_t *result,
				 void *priv);
void ds_pool_replicas_update_handler(crt_rpc_t *rpc);
int ds_pool_tgt_prop_update(struct ds_pool *pool, struct pool_iv_prop *iv_prop);
int ds_pool_tgt_connect(struct ds_pool *pool, struct pool_iv_conn *pic);
void ds_pool_tgt_query_map_handler(crt_rpc_t *rpc);
void ds_pool_tgt_discard_handler(crt_rpc_t *rpc);
void ds_pool_tgt_warmup_handler(crt_rpc_t *rpc);
int ds_pool_lookup_map_bc(struct ds_pool *pool, crt_context_t ctx,
			  struct ds_pool_map_bc **map_bc_out, uint32_t *map_version_out);
void ds_pool_put_map_bc(struct ds_pool_map_bc *map_bc);

/*
 * srv_util.c
 */
bool ds_pool_map_rank_up(struct pool_map *map, d_rank_t rank);
int ds_pool_plan_svc_reconfs(int svc_rf, struct pool_map *map, d_rank_list_t *replicas,
			     d_rank_t self, bool filter_only, d_rank_list_t **to_add_out,
			     d_rank_list_t **to_remove_out);
int ds_pool_transfer_map_buf(struct ds_pool_map_bc *map_bc, crt_rpc_t *rpc, crt_bulk_t remote_bulk,
			     uint32_t *required_buf_size);
extern struct bio_reaction_ops nvme_reaction_ops;

/*
 * srv_iv.c
 */
uint32_t pool_iv_map_ent_size(int nr);
int ds_pool_iv_init(void);
int ds_pool_iv_fini(void);
void ds_pool_map_refresh_ult(void *arg);

int ds_pool_iv_conn_hdl_update(struct ds_pool *pool, uuid_t hdl_uuid,
			       uint64_t flags, uint64_t capas, d_iov_t *cred,
			       uint32_t global_ver, uint32_t obj_layout_ver);

int ds_pool_iv_srv_hdl_update(struct ds_pool *pool, uuid_t pool_hdl_uuid,
			      uuid_t cont_hdl_uuid);

int ds_pool_iv_srv_hdl_invalidate(struct ds_pool *pool);
int ds_pool_iv_conn_hdl_fetch(struct ds_pool *pool);
int ds_pool_iv_conn_hdl_invalidate(struct ds_pool *pool, uuid_t hdl_uuid);

/*
 * srv_metrics.c
 */
void *ds_pool_metrics_alloc(const char *path, int tgt_id);
void ds_pool_metrics_free(void *data);
int ds_pool_metrics_count(void);
int ds_pool_metrics_start(struct ds_pool *pool);
void ds_pool_metrics_stop(struct ds_pool *pool);

#endif /* __POOL_SRV_INTERNAL_H__ */
