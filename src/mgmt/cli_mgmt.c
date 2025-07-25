/*
 * (C) Copyright 2016-2024 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * \file
 *
 * DAOS management client library. It exports the mgmt API defined in
 * daos_mgmt.h
 */

#define D_LOGFAC	DD_FAC(mgmt)

#include <daos/mgmt.h>

#include <daos/agent.h>
#include <daos/drpc_modules.h>
#include <daos/event.h>
#include <daos/job.h>
#include <daos/pool.h>
#include <daos/security.h>
#include "svc.pb-c.h"
#include "rpc.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/ipc.h>

char agent_sys_name[DAOS_SYS_NAME_MAX + 1] = DAOS_DEFAULT_SYS_NAME;

static struct dc_mgmt_sys_info info_g;
static Mgmt__GetAttachInfoResp *resp_g;

int	dc_mgmt_proto_version;

int
dc_cp(tse_task_t *task, void *data)
{
	struct cp_arg	*arg = data;
	int		 rc = task->dt_result;

	if (rc)
		D_ERROR("RPC error: "DF_RC"\n", DP_RC(rc));

	dc_mgmt_sys_detach(arg->sys);
	crt_req_decref(arg->rpc);
	return rc;
}

int
dc_deprecated(tse_task_t *task)
{
	D_ERROR("This API is deprecated\n");
	tse_task_complete(task, -DER_NOSYS);
	return -DER_NOSYS;
}

int
dc_mgmt_srv_version(uint32_t *major, uint32_t *minor, uint32_t *patch, char **tag)
{
	if (major == NULL || minor == NULL || patch == NULL || tag == NULL) {
		D_ERROR("major, minor, patch, tag must be non-null\n");
		return -DER_INVAL;
	}

	if (resp_g == NULL || resp_g->build_info == NULL) {
		D_ERROR("server build info unavailable\n");
		return -DER_UNINIT;
	}

	*major = resp_g->build_info->major;
	*minor = resp_g->build_info->minor;
	*patch = resp_g->build_info->patch;
	*tag   = resp_g->build_info->tag;

	return 0;
}

int
dc_mgmt_profile(char *path, int avg, bool start)
{
	struct dc_mgmt_sys	*sys;
	struct mgmt_profile_in	*in;
	crt_endpoint_t		ep;
	crt_rpc_t		*rpc = NULL;
	crt_opcode_t		opc;
	int			rc;

	rc = dc_mgmt_sys_attach(NULL, &sys);
	if (rc != 0) {
		D_ERROR("failed to attach to grp rc "DF_RC"\n", DP_RC(rc));
		return -DER_INVAL;
	}

	ep.ep_grp = sys->sy_group;
	ep.ep_rank = 0;
	ep.ep_tag = daos_rpc_tag(DAOS_REQ_MGMT, 0);
	opc = DAOS_RPC_OPCODE(MGMT_PROFILE, DAOS_MGMT_MODULE, dc_mgmt_proto_version);
	rc = crt_req_create(daos_get_crt_ctx(), &ep, opc, &rpc);
	if (rc != 0) {
		D_ERROR("crt_req_create failed, rc: "DF_RC"\n", DP_RC(rc));
		D_GOTO(err_grp, rc);
	}

	D_ASSERT(rpc != NULL);
	in = crt_req_get(rpc);
	in->p_path = path;
	in->p_avg = avg;
	in->p_op = start ? MGMT_PROFILE_START : MGMT_PROFILE_STOP;
	/** send the request */
	rc = daos_rpc_send_wait(rpc);
err_grp:
	D_DEBUG(DB_MGMT, "mgmt profile: rc "DF_RC"\n", DP_RC(rc));
	dc_mgmt_sys_detach(sys);
	return rc;
}

#define copy_str(dest, src)				\
({							\
	int	__rc = 1;				\
	size_t	__size = strnlen(src, sizeof(dest));	\
							\
	if (__size != sizeof(dest)) {			\
		memcpy(dest, src, __size + 1);		\
		__rc = 0;				\
	}						\
	__rc;						\
})

/* Fill info based on resp. */
static int
fill_sys_info(Mgmt__GetAttachInfoResp *resp, struct dc_mgmt_sys_info *info)
{
	int			 i;
	Mgmt__ClientNetHint	*hint = resp->client_net_hint;

	if (hint == NULL) {
		D_ERROR("GetAttachInfo failed: %d. "
			"no client networking hint set. "
			"libdaos.so is incompatible with DAOS Agent.\n",
			resp->status);
		return -DER_AGENT_INCOMPAT;
	}

	if (strnlen(hint->provider, sizeof(info->provider)) == 0) {
		D_ERROR("GetAttachInfo failed: %d. "
			"provider is undefined. "
			"libdaos.so is incompatible with DAOS Agent.\n",
			resp->status);
		return -DER_AGENT_INCOMPAT;
	}

	if (strnlen(hint->interface, sizeof(info->interface)) == 0) {
		D_ERROR("GetAttachInfo failed: %d. "
			"interface is undefined. "
			"libdaos.so is incompatible with DAOS Agent.\n",
			resp->status);
		return -DER_AGENT_INCOMPAT;
	}

	if (strnlen(hint->domain, sizeof(info->domain)) == 0) {
		D_ERROR("GetAttachInfo failed: %d. "
			"domain string is undefined. "
			"libdaos.so is incompatible with DAOS Agent.\n",
			resp->status);
		return -DER_AGENT_INCOMPAT;
	}

	if (copy_str(info->provider, hint->provider)) {
		D_ERROR("GetAttachInfo failed: %d. "
			"provider string too long.\n",
			resp->status);

		return -DER_INVAL;
	}

	if (copy_str(info->interface, hint->interface)) {
		D_ERROR("GetAttachInfo failed: %d. "
			"interface string too long\n",
			resp->status);
		return -DER_INVAL;
	}

	if (copy_str(info->domain, hint->domain)) {
		D_ERROR("GetAttachInfo failed: %d. "
			"domain string too long\n",
			resp->status);
		return -DER_INVAL;
	}

	if (strnlen(resp->sys, sizeof(info->system_name)) > 0) {
		if (copy_str(info->system_name, resp->sys)) {
			D_ERROR("GetAttachInfo failed: %d. System name string too long\n",
				resp->status);
			return -DER_INVAL;
		}
	} else {
		D_NOTE("No system name in GetAttachInfo. Agent may be out of date with libdaos\n");
	}

	info->crt_timeout = hint->crt_timeout;
	info->srv_srx_set = hint->srv_srx_set;

	/* Fill info->ms_ranks. */
	if (resp->n_ms_ranks == 0) {
		D_ERROR("GetAttachInfo returned zero MS ranks\n");
		return -DER_AGENT_INCOMPAT;
	}
	info->ms_ranks = d_rank_list_alloc(resp->n_ms_ranks);
	if (info->ms_ranks == NULL)
		return -DER_NOMEM;
	for (i = 0; i < resp->n_ms_ranks; i++) {
		info->ms_ranks->rl_ranks[i] = resp->ms_ranks[i];
		D_DEBUG(DB_MGMT, "GetAttachInfo ms_ranks[%d]: rank=%u\n", i,
			info->ms_ranks->rl_ranks[i]);
	}

	info->provider_idx = resp->client_net_hint->provider_idx;

	D_DEBUG(DB_MGMT,
		"GetAttachInfo Provider: %s, Interface: %s, Domain: %s,"
		"CRT_TIMEOUT: %u, "
		"FI_OFI_RXM_USE_SRX: %d, CRT_SECONDARY_PROVIDER: %d\n",
		info->provider, info->interface, info->domain,
		info->crt_timeout, info->srv_srx_set, info->provider_idx);

	return 0;
}

static void
free_get_attach_info_resp(Mgmt__GetAttachInfoResp *resp)
{
	struct drpc_alloc alloc = PROTO_ALLOCATOR_INIT(alloc);

	mgmt__get_attach_info_resp__free_unpacked(resp, &alloc.alloc);
}

static void
put_attach_info(struct dc_mgmt_sys_info *info, Mgmt__GetAttachInfoResp *resp)
{
	if (resp != NULL)
		free_get_attach_info_resp(resp);
	d_rank_list_free(info->ms_ranks);
}

void
dc_put_attach_info(struct dc_mgmt_sys_info *info, Mgmt__GetAttachInfoResp *resp)
{
	return put_attach_info(info, resp);
}

void
dc_mgmt_drop_attach_info(void)
{
	return put_attach_info(&info_g, resp_g);
}

static int
get_env_deprecated(char **val, const char *new_env, const char *old_env)
{
	char *new = NULL;
	char *old = NULL;
	int   rc_new;
	int   rc_old;

	rc_new = d_agetenv_str(&new, new_env);
	rc_old = d_agetenv_str(&old, old_env);

	if (rc_new == 0) {
		if (rc_old == 0)
			D_WARN("Both %s and %s are set! Deprecated %s (%s) will be ignored\n",
			       new_env, old_env, old_env, old);
		*val = new;
		d_freeenv_str(&old);
		return 0;
	}

	if (rc_old == 0) {
		D_INFO("%s is deprecated, upgrade your environment to use %s instead\n", old_env,
		       new_env);
		*val = old;
		d_freeenv_str(&new);
		return 0;
	}

	return rc_new;
}

/*
 * Get the attach info (i.e., rank URIs) for name. To avoid duplicating the
 * rank URIs, we return the GetAttachInfo response directly. Callers are
 * responsible for finalizing info and respp using put_attach_info.
 */
static int
get_attach_info(const char *name, bool all_ranks, struct dc_mgmt_sys_info *info,
		Mgmt__GetAttachInfoResp **respp)
{
	struct drpc_alloc	 alloc = PROTO_ALLOCATOR_INIT(alloc);
	struct drpc		*ctx;
	Mgmt__GetAttachInfoReq   req = MGMT__GET_ATTACH_INFO_REQ__INIT;
	uint8_t			*reqb;
	Mgmt__GetAttachInfoResp *resp;
	size_t			 reqb_size;
	Drpc__Call		*dreq;
	Drpc__Response		*dresp;
	char                    *interface = NULL;
	char                    *domain    = NULL;
	int			 rc;

	D_DEBUG(DB_MGMT, "getting attach info for %s\n", name);

	/* Connect to daos_agent. */
	D_ASSERT(dc_agent_sockpath != NULL);
	rc = drpc_connect(dc_agent_sockpath, &ctx);
	if (rc != -DER_SUCCESS) {
		D_ERROR("failed to connect to %s " DF_RC "\n",
			dc_agent_sockpath, DP_RC(rc));
		if (rc == -DER_NONEXIST)
			rc = -DER_AGENT_COMM;
		D_GOTO(out, rc);
	}

	if (get_env_deprecated(&interface, "D_INTERFACE", "OFI_INTERFACE") == 0)
		D_INFO("Using environment-provided interface: %s\n", interface);

	if (get_env_deprecated(&domain, "D_DOMAIN", "OFI_DOMAIN") == 0)
		D_INFO("Using environment-provided domain: %s\n", domain);

	/* Prepare the GetAttachInfo request. */
	req.sys = (char *)name;
	req.all_ranks = all_ranks;
	req.interface = interface;
	req.domain    = domain;
	reqb_size = mgmt__get_attach_info_req__get_packed_size(&req);
	D_ALLOC(reqb, reqb_size);
	if (reqb == NULL) {
		rc = -DER_NOMEM;
		goto out_ctx;
	}
	mgmt__get_attach_info_req__pack(&req, reqb);
	rc = drpc_call_create(ctx, DRPC_MODULE_MGMT,
				DRPC_METHOD_MGMT_GET_ATTACH_INFO, &dreq);
	if (rc != 0) {
		D_FREE(reqb);
		goto out_ctx;
	}
	dreq->body.len = reqb_size;
	dreq->body.data = reqb;

	/* Make the GetAttachInfo call and get the response. */
	rc = drpc_call(ctx, R_SYNC, dreq, &dresp);
	if (rc != 0) {
		D_ERROR("GetAttachInfo call failed: "DF_RC"\n", DP_RC(rc));
		goto out_dreq;
	}
	if (dresp->status != DRPC__STATUS__SUCCESS) {
		D_ERROR("GetAttachInfo unsuccessful: %d\n", dresp->status);
		rc = -DER_MISC;
		goto out_dresp;
	}
	resp = mgmt__get_attach_info_resp__unpack(&alloc.alloc, dresp->body.len, dresp->body.data);
	if (alloc.oom)
		D_GOTO(out_dresp, rc = -DER_NOMEM);
	if (resp == NULL) {
		D_ERROR("failed to unpack GetAttachInfo response\n");
		rc = -DER_MISC;
		goto out_dresp;
	}
	if (resp->status != 0) {
		D_ERROR("GetAttachInfo(%s) failed: "DF_RC"\n", req.sys,
			DP_RC(resp->status));
		rc = resp->status;
		goto out_resp;
	}

	/* Output to the caller. */
	rc = fill_sys_info(resp, info);
	if (rc != 0)
		goto out_resp;

	/** set the agent system info to be the default one */
	if (name == NULL) {
		if (copy_str(agent_sys_name, resp->sys)) {
			rc = -DER_INVAL;
			goto out_resp;
		}
	}
	*respp = resp;

out_resp:
	if (rc != 0)
		mgmt__get_attach_info_resp__free_unpacked(resp, &alloc.alloc);
out_dresp:
	drpc_response_free(dresp);
out_dreq:
	/* This also frees reqb via dreq->body.data. */
	drpc_call_free(dreq);
out_ctx:
	d_freeenv_str(&interface);
	d_freeenv_str(&domain);
	drpc_close(ctx);
out:
	return rc;
}

int
dc_get_attach_info(const char *name, bool all_ranks, struct dc_mgmt_sys_info *info,
		   Mgmt__GetAttachInfoResp **respp)
{
	return get_attach_info(name, all_ranks, info, respp);
}

int
dc_mgmt_cache_attach_info(const char *name)
{
	if (name != NULL && strcmp(name, agent_sys_name) != 0)
		return -DER_INVAL;
	return get_attach_info(name, true, &info_g, &resp_g);
}

static void
free_rank_uris(struct daos_rank_uri *uris, uint32_t nr_uris)
{
	uint32_t i;

	for (i = 0; i < nr_uris; i++)
		D_FREE(uris[i].dru_uri);
	D_FREE(uris);
}

static int
alloc_rank_uris(Mgmt__GetAttachInfoResp *resp, struct daos_rank_uri **out)
{
	struct daos_rank_uri	*uris;
	uint32_t		i;

	D_ALLOC_ARRAY(uris, resp->n_rank_uris);
	if (uris == NULL)
		return -DER_NOMEM;

	for (i = 0; i < resp->n_rank_uris; i++) {
		uris[i].dru_rank = resp->rank_uris[i]->rank;

		D_STRNDUP(uris[i].dru_uri, resp->rank_uris[i]->uri, CRT_ADDR_STR_MAX_LEN - 1);
		if (uris[i].dru_uri == NULL) {
			free_rank_uris(uris, i);
			return -DER_NOMEM;
		}
	}

	*out = uris;
	return 0;
}

int
dc_mgmt_get_sys_info(const char *sys, struct daos_sys_info **out)
{
	struct daos_sys_info	*info;
	struct dc_mgmt_sys_info	internal = {0};
	Mgmt__GetAttachInfoResp	*resp = NULL;
	struct daos_rank_uri	*ranks = NULL;
	int			rc = 0;

	if (out == NULL) {
		D_ERROR("daos_sys_info must be non-NULL\n");
		return -DER_INVAL;
	}

	rc = dc_get_attach_info(sys, true, &internal, &resp);
	if (rc != 0) {
		D_ERROR("dc_get_attach_info failed: "DF_RC"\n", DP_RC(rc));
		D_GOTO(out, rc);
	}

	D_ALLOC_PTR(info);
	if (info == NULL)
		D_GOTO(out_attach_info, rc = -DER_NOMEM);

	D_ALLOC_ARRAY(info->dsi_ms_ranks, resp->n_ms_ranks);
	if (info->dsi_ms_ranks == NULL)
		D_GOTO(err_info, rc = -DER_NOMEM);
	memcpy(info->dsi_ms_ranks, resp->ms_ranks, resp->n_ms_ranks * sizeof(uint32_t));
	info->dsi_nr_ms_ranks = resp->n_ms_ranks;

	rc = alloc_rank_uris(resp, &ranks);
	if (rc != 0) {
		D_ERROR("failed to allocate rank URIs: "DF_RC"\n", DP_RC(rc));
		D_GOTO(err_ms_ranks, rc);
	}
	info->dsi_nr_ranks = resp->n_rank_uris;
	info->dsi_ranks = ranks;

	copy_str(info->dsi_system_name, internal.system_name);
	copy_str(info->dsi_fabric_provider, internal.provider);
	copy_str(info->dsi_agent_path, dc_agent_sockpath);

	*out = info;

	D_GOTO(out_attach_info, rc = 0);

err_ms_ranks:
	D_FREE(info->dsi_ms_ranks);
err_info:
	D_FREE(info);
out_attach_info:
	dc_put_attach_info(&internal, resp);
out:
	return rc;
}

void
dc_mgmt_put_sys_info(struct daos_sys_info *info)
{
	if (info == NULL)
		return;
	free_rank_uris(info->dsi_ranks, info->dsi_nr_ranks);
	D_FREE(info);
}

#define SYS_INFO_BUF_SIZE 16

static int       g_num_serv_ranks = -1;
static d_rank_t *g_serv_ranks;

/* Return the number of attached ranks.  */
int
dc_mgmt_net_get_num_srv_ranks(void)
{
	D_ASSERT(g_num_serv_ranks >= 0);

	return g_num_serv_ranks;
}

/* Return the rank id of an attached rank.  */
d_rank_t
dc_mgmt_net_get_srv_rank(int idx)
{
	D_ASSERT(g_num_serv_ranks >= 0);

	if (idx >= g_num_serv_ranks) {
		D_ERROR("Invalid rank index: index=%d, ranks_num=%d\n", idx, g_num_serv_ranks);
		return CRT_NO_RANK;
	}

	return g_serv_ranks[idx];
}

static int
_split_env(char *env, char **name, char **value)
{
	char *sep;

	if (strnlen(env, 1024) == 1024)
		return -DER_INVAL;

	sep = strchr(env, '=');
	if (sep == NULL)
		return -DER_INVAL;
	*sep = '\0';
	*name = env;
	*value = sep + 1;

	return 0;
}

/*
 * Get the CaRT network configuration for this client node
 * via the get_attach_info() dRPC.
 * Configure the client's local environment with these parameters
 */
int
dc_mgmt_net_cfg_init(const char *name, crt_init_options_t *crt_info)
{
	int                      rc;
	char                    *cli_srx_set        = NULL;
	char                    *crt_timeout        = NULL;
	char                     buf[SYS_INFO_BUF_SIZE];
	struct dc_mgmt_sys_info *info = &info_g;
	Mgmt__GetAttachInfoResp *resp = resp_g;
	int                      idx;
	d_rank_t                *serv_ranks_tmp;

	if (resp->client_net_hint != NULL && resp->client_net_hint->n_env_vars > 0) {
		int i;
		char *env = NULL;
		char *v_name = NULL;
		char *v_value = NULL;

		for (i = 0; i < resp->client_net_hint->n_env_vars; i++) {
			env = resp->client_net_hint->env_vars[i];
			if (env == NULL)
				continue;

			rc = _split_env(env, &v_name, &v_value);
			if (rc != 0) {
				D_ERROR("invalid client env var: %s\n", env);
				continue;
			}

			rc = d_setenv(v_name, v_value, 0);
			if (rc != 0)
				D_GOTO(cleanup, rc = d_errno2der(errno));
			D_DEBUG(DB_MGMT, "set server-supplied client env: %s", env);
		}
	}

	/* If the server has set this, the client must use the same value. */
	if (info->srv_srx_set != -1) {
		rc = asprintf(&cli_srx_set, "%d", info->srv_srx_set);
		if (rc < 0) {
			cli_srx_set = NULL;
			D_GOTO(cleanup, rc = -DER_NOMEM);
		}
		rc = d_setenv("FI_OFI_RXM_USE_SRX", cli_srx_set, 1);
		if (rc != 0)
			D_GOTO(cleanup, rc = d_errno2der(errno));
		D_INFO("Using server's value for FI_OFI_RXM_USE_SRX: %s\n", cli_srx_set);
	} else {
		/* Client may not set it if the server hasn't. */
		d_agetenv_str(&cli_srx_set, "FI_OFI_RXM_USE_SRX");
		if (cli_srx_set) {
			D_ERROR("Client set FI_OFI_RXM_USE_SRX to %s, "
				"but server is unset!\n", cli_srx_set);
			D_GOTO(cleanup, rc = -DER_INVAL);
		}
	}

	/* Allow client env overrides for these three */
	d_agetenv_str(&crt_timeout, "CRT_TIMEOUT");
	if (!crt_timeout) {
		crt_info->cio_crt_timeout = info->crt_timeout;
	} else {
		crt_info->cio_crt_timeout = atoi(crt_timeout);
		D_DEBUG(DB_MGMT, "Using client provided CRT_TIMEOUT: %s\n", crt_timeout);
	}

	sprintf(buf, "%d", info->provider_idx);
	rc = d_setenv("CRT_SECONDARY_PROVIDER", buf, 1);
	if (rc != 0)
		D_GOTO(cleanup, rc = d_errno2der(errno));

	D_STRNDUP(crt_info->cio_provider, info->provider, DAOS_SYS_INFO_STRING_MAX);
	if (NULL == crt_info->cio_provider)
		D_GOTO(cleanup, rc = -DER_NOMEM);
	D_STRNDUP(crt_info->cio_interface, info->interface, DAOS_SYS_INFO_STRING_MAX);
	if (NULL == crt_info->cio_interface)
		D_GOTO(cleanup, rc = -DER_NOMEM);
	D_STRNDUP(crt_info->cio_domain, info->domain, DAOS_SYS_INFO_STRING_MAX);
	if (NULL == crt_info->cio_domain)
		D_GOTO(cleanup, rc = -DER_NOMEM);
	D_DEBUG(DB_MGMT,
		"CaRT initialization with:\n"
		"\tD_PROVIDER: %s, CRT_TIMEOUT: %d, CRT_SECONDARY_PROVIDER: %s\n",
		crt_info->cio_provider, crt_info->cio_crt_timeout, buf);

	/* Save attached ranks id info */
	g_num_serv_ranks = resp->n_rank_uris;
	serv_ranks_tmp   = NULL;
	if (g_num_serv_ranks > 0) {
		D_ALLOC_ARRAY(serv_ranks_tmp, g_num_serv_ranks);
		if (serv_ranks_tmp == NULL)
			D_GOTO(cleanup, rc = -DER_NOMEM);
		for (idx = 0; idx < g_num_serv_ranks; idx++)
			serv_ranks_tmp[idx] = resp->rank_uris[idx]->rank;
	}
	D_FREE(g_serv_ranks);
	g_serv_ranks = serv_ranks_tmp;

	D_INFO("Network interface: %s, Domain: %s, Provider: %s, Ranks count: %d\n",
	       crt_info->cio_interface, crt_info->cio_domain, crt_info->cio_provider,
	       g_num_serv_ranks);

cleanup:
	if (rc) {
		D_FREE(crt_info->cio_provider);
		D_FREE(crt_info->cio_interface);
		D_FREE(crt_info->cio_domain);
	}
	d_freeenv_str(&crt_timeout);
	d_freeenv_str(&cli_srx_set);

	return rc;
}

void
dc_mgmt_net_cfg_fini()
{
	D_FREE(g_serv_ranks);
}

int dc_mgmt_net_cfg_check(const char *name)
{
	char *cli_srx_set;

	/* Client may not set it if the server hasn't. */
	if (info_g.srv_srx_set == -1) {
		d_agetenv_str(&cli_srx_set, "FI_OFI_RXM_USE_SRX");
		if (cli_srx_set) {
			D_ERROR("Client set FI_OFI_RXM_USE_SRX to %s, "
				"but server is unset!\n", cli_srx_set);
			d_freeenv_str(&cli_srx_set);
			return -DER_INVAL;
		}
	}
	return 0;
}

static int send_monitor_request(struct dc_pool *pool, int request_type)
{
	struct drpc		 *ctx;
	Mgmt__PoolMonitorReq	 req = MGMT__POOL_MONITOR_REQ__INIT;
	uint8_t			 *reqb;
	size_t			 reqb_size;
	char			 pool_uuid[DAOS_UUID_STR_SIZE];
	char			 pool_hdl_uuid[DAOS_UUID_STR_SIZE];
	Drpc__Call		 *dreq;
	Drpc__Response		 *dresp;
	int			 rc;

	/* Connect to daos_agent. */
	D_ASSERT(dc_agent_sockpath != NULL);
	rc = drpc_connect(dc_agent_sockpath, &ctx);
	if (rc != -DER_SUCCESS) {
		D_ERROR("failed to connect to %s " DF_RC "\n",
			dc_agent_sockpath, DP_RC(rc));
		D_GOTO(out, 0);
	}

	uuid_unparse(pool->dp_pool, pool_uuid);
	uuid_unparse(pool->dp_pool_hdl, pool_hdl_uuid);
	req.pooluuid = pool_uuid;
	req.poolhandleuuid = pool_hdl_uuid;
	req.jobid = dc_jobid;
	req.sys = pool->dp_sys->sy_name;

	reqb_size = mgmt__pool_monitor_req__get_packed_size(&req);
	D_ALLOC(reqb, reqb_size);
	if (reqb == NULL) {
		rc = -DER_NOMEM;
		goto out_ctx;
	}
	mgmt__pool_monitor_req__pack(&req, reqb);

	rc = drpc_call_create(ctx, DRPC_MODULE_MGMT, request_type, &dreq);
	if (rc != 0) {
		D_FREE(reqb);
		goto out_ctx;
	}
	dreq->body.len = reqb_size;
	dreq->body.data = reqb;

	/* Make the call and get the response. */
	rc = drpc_call(ctx, R_SYNC, dreq, &dresp);
	if (rc != 0) {
		D_ERROR("Sending monitor request failed: "DF_RC"\n", DP_RC(rc));
		goto out_dreq;
	}
	if (dresp->status != DRPC__STATUS__SUCCESS) {
		D_ERROR("Monitor Request unsuccessful: %d\n", dresp->status);
		rc = -DER_MISC;
		goto out_dresp;
	}

out_dresp:
	drpc_response_free(dresp);
out_dreq:
	drpc_call_free(dreq);
out_ctx:
	drpc_close(ctx);
out:
	return rc;
}

/*
 * Send an upcall to the agent to notify it of a pool disconnect.
 */
int
dc_mgmt_notify_pool_disconnect(struct dc_pool *pool) {
	return send_monitor_request(pool,
				    DRPC_METHOD_MGMT_NOTIFY_POOL_DISCONNECT);
}

/*
 * Send an upcall to the agent to notify it of a successful pool connect.
 */
int
dc_mgmt_notify_pool_connect(struct dc_pool *pool) {
	return send_monitor_request(pool, DRPC_METHOD_MGMT_NOTIFY_POOL_CONNECT);
}

/*
 * Send an upcall to the agent to notify it of a clean process shutdown.
 */
int
dc_mgmt_notify_exit(void)
{
	struct drpc		 *ctx;
	Drpc__Call		 *dreq;
	Drpc__Response		 *dresp;
	int			 rc;

	D_DEBUG(DB_MGMT, "disconnecting process for pid:%d\n", getpid());

	/* Connect to daos_agent. */
	D_ASSERT(dc_agent_sockpath != NULL);
	rc = drpc_connect(dc_agent_sockpath, &ctx);
	if (rc != -DER_SUCCESS) {
		D_ERROR("failed to connect to %s " DF_RC "\n",
			dc_agent_sockpath, DP_RC(rc));
		if (rc == -DER_NONEXIST)
			rc = -DER_AGENT_COMM;
		D_GOTO(out, rc);
	}

	rc = drpc_call_create(ctx, DRPC_MODULE_MGMT,
			      DRPC_METHOD_MGMT_NOTIFY_EXIT, &dreq);
	if (rc != 0)
		goto out_ctx;

	/* Make the Process Disconnect call and get the response. */
	rc = drpc_call(ctx, R_SYNC, dreq, &dresp);
	if (rc != 0) {
		D_ERROR("Process Disconnect call failed: "DF_RC"\n", DP_RC(rc));
		goto out_dreq;
	}
	if (dresp->status != DRPC__STATUS__SUCCESS) {
		D_ERROR("Process Disconnect unsuccessful: %d\n", dresp->status);
		rc = -DER_MISC;
		goto out_dresp;
	}

out_dresp:
	drpc_response_free(dresp);
out_dreq:
	drpc_call_free(dreq);
out_ctx:
	drpc_close(ctx);
out:
	return rc;
}

struct sys_buf {
	char	syb_name[DAOS_SYS_NAME_MAX + 1];
};

static int
attach_group(const char *name, struct dc_mgmt_sys_info *info,
	     Mgmt__GetAttachInfoResp *resp, crt_group_t **groupp)
{
	crt_group_t    *group;
	int		i;
	int		rc;

	rc = crt_group_view_create((char *)name, &group);
	if (rc != 0) {
		D_ERROR("failed to create group %s: "DF_RC"\n", name,
			DP_RC(rc));
		goto err;
	}

	for (i = 0; i < resp->n_rank_uris; i++) {
		Mgmt__GetAttachInfoResp__RankUri *rank_uri = resp->rank_uris[i];

		rc = crt_group_primary_rank_add(daos_get_crt_ctx(), group,
						rank_uri->rank, rank_uri->uri);
		if (rc != 0) {
			D_ERROR("failed to add rank %u URI %s to group %s: "
				DF_RC"\n", rank_uri->rank, rank_uri->uri, name,
				DP_RC(rc));
			goto err_group;
		}
	}

	*groupp = group;
	return 0;

err_group:
	crt_group_view_destroy(group);
err:
	return rc;
}

static void
detach_group(bool server, crt_group_t *group)
{
	int rc = 0;

	if (!server)
		rc = crt_group_view_destroy(group);
	D_ASSERTF(rc == 0, ""DF_RC"\n", DP_RC(rc));
}

static int
attach(const char *name, struct dc_mgmt_sys **sysp)
{
	struct dc_mgmt_sys	*sys;
	crt_group_t		*group;
	Mgmt__GetAttachInfoResp *resp;
	bool			 need_free_resp = false;
	int			 rc;

	D_DEBUG(DB_MGMT, "attaching to system '%s'\n", name);

	D_ALLOC_PTR(sys);
	if (sys == NULL) {
		rc = -DER_NOMEM;
		goto err;
	}
	D_INIT_LIST_HEAD(&sys->sy_link);
	rc = snprintf(sys->sy_name, sizeof(sys->sy_name), "%s", name);
	D_ASSERTF(rc >= 0, ""DF_RC"\n", DP_RC(rc));
	if (rc >= sizeof(sys->sy_name)) {
		D_ERROR("system name %s longer than %zu bytes\n", sys->sy_name,
			sizeof(sys->sy_name) - 1);
		rc = -DER_OVERFLOW;
		goto err_sys;
	}

	group = crt_group_lookup((char *)name);
	if (group != NULL) {
		/* This is one of the servers. Skip the get_attach_info call. */
		sys->sy_server = true;
		sys->sy_group = group;
		goto out;
	}

	if (strcmp(name, agent_sys_name) != 0 || !resp_g) {
		need_free_resp = true;
		rc = get_attach_info(name, true /* all_ranks */, &sys->sy_info, &resp);
		if (rc != 0)
			goto err_sys;
	} else {
		resp = resp_g;
		rc   = fill_sys_info(resp, &sys->sy_info);
		if (rc != 0)
			goto err_sys;
	}

	rc = attach_group(name, &sys->sy_info, resp, &sys->sy_group);
	if (rc != 0)
		goto err_info;

	if (need_free_resp)
		free_get_attach_info_resp(resp);
out:
	*sysp = sys;
	return 0;

err_info:
	d_rank_list_free(sys->sy_info.ms_ranks);
	if (need_free_resp)
		free_get_attach_info_resp(resp);
err_sys:
	D_FREE(sys);
err:
	return rc;
}

static void
detach(struct dc_mgmt_sys *sys)
{
	D_DEBUG(DB_MGMT, "detaching from system '%s'\n", sys->sy_name);
	D_ASSERT(d_list_empty(&sys->sy_link));
	D_ASSERTF(sys->sy_ref == 0, "%d\n", sys->sy_ref);
	detach_group(sys->sy_server, sys->sy_group);
	if (!sys->sy_server)
		put_attach_info(&sys->sy_info, NULL /* resp */);
	D_FREE(sys);
}

static D_LIST_HEAD(systems);
static pthread_mutex_t systems_lock = PTHREAD_MUTEX_INITIALIZER;

static struct dc_mgmt_sys *
lookup_sys(const char *name)
{
	struct dc_mgmt_sys *sys;

	d_list_for_each_entry(sys, &systems, sy_link) {
		if (strcmp(sys->sy_name, name) == 0)
			return sys;
	}
	return NULL;
}

static int
sys_attach(const char *name, struct dc_mgmt_sys **sysp)
{
	struct dc_mgmt_sys     *sys;
	int			rc = 0;

	D_MUTEX_LOCK(&systems_lock);

	sys = lookup_sys(name);
	if (sys != NULL)
		goto ok;

	rc = attach(name, &sys);
	if (rc != 0)
		goto out_lock;

	d_list_add(&sys->sy_link, &systems);

ok:
	sys->sy_ref++;
	*sysp = sys;
out_lock:
	D_MUTEX_UNLOCK(&systems_lock);
	return rc;
}

/**
 * Attach to system \a name.
 *
 * \param[in]		name	system name
 * \param[in,out]	sys	system handle
 */
int
dc_mgmt_sys_attach(const char *name, struct dc_mgmt_sys **sysp)
{
	if (name == NULL)
		name = agent_sys_name;
	return sys_attach(name, sysp);
}

/**
 * Detach from system \a sys.
 *
 * \param[in]	sys	system handle
 */
void
dc_mgmt_sys_detach(struct dc_mgmt_sys *sys)
{
	D_ASSERT(sys != NULL);
	D_MUTEX_LOCK(&systems_lock);
	sys->sy_ref--;
	if (sys->sy_ref == 0) {
		d_list_del_init(&sys->sy_link);
		detach(sys);
	}
	D_MUTEX_UNLOCK(&systems_lock);
}

/**
 * Encode \a sys into \a buf of capacity \a cap. If \a buf is NULL, just return
 * the number of bytes that would be required. If \a buf is not NULL and \a cap
 * is insufficient, return -DER_TRUNC.
 */
ssize_t
dc_mgmt_sys_encode(struct dc_mgmt_sys *sys, void *buf, size_t cap)
{
	struct sys_buf *sysb = buf;
	size_t		len;

	len = sizeof(*sysb);

	if (sysb == NULL)
		return len;

	if (cap < len)
		return -DER_TRUNC;

	D_CASSERT(sizeof(sysb->syb_name) == sizeof(sys->sy_name));
	strncpy(sysb->syb_name, sys->sy_name, sizeof(sysb->syb_name));

	return len;
}

/** Decode \a buf of length \a len. */
ssize_t
dc_mgmt_sys_decode(void *buf, size_t len, struct dc_mgmt_sys **sysp)
{
	struct sys_buf *sysb;

	if (len < sizeof(*sysb)) {
		D_ERROR("truncated sys_buf: %zu < %zu\n", len, sizeof(*sysb));
		return -DER_IO;
	}
	sysb = buf;

	return sys_attach(sysb->syb_name, sysp);
}

/* For a given pool label or UUID, contact mgmt. service to look up its
 * service replica ranks. Note: synchronous RPC with caller already
 * in a task execution context. On successful return, caller is responsible
 * for freeing the d_rank_list_t allocated here. Must not be called by server.
 */
int
dc_mgmt_pool_find(struct dc_mgmt_sys *sys, const char *label, uuid_t puuid,
		  d_rank_list_t **svcranksp)
{
	d_rank_list_t		       *ms_ranks;
	crt_endpoint_t			srv_ep;
	crt_rpc_t		       *rpc = NULL;
	struct mgmt_pool_find_in       *rpc_in;
	struct mgmt_pool_find_out      *rpc_out = NULL;
	crt_opcode_t			opc;
	int				i;
	int				idx;
	crt_context_t			ctx;
	uuid_t				null_uuid;
	bool				success = false;
	int				rc = 0;

	D_ASSERT(sys->sy_server == 0);
	uuid_clear(null_uuid);

	/* NB: ms_ranks may have multiple entries even for single MS replica,
	 * since there may be multiple engines there. Some of which may have
	 * been stopped or faulted. May need to contact multiple engines.
	 * Assumed: any MS replica engine can be contacted, even non-leaders.
	 */
	ms_ranks = sys->sy_info.ms_ranks;
	D_ASSERT(ms_ranks->rl_nr > 0);
	idx = d_rand() % ms_ranks->rl_nr;
	ctx = daos_get_crt_ctx();
	opc = DAOS_RPC_OPCODE(MGMT_POOL_FIND, DAOS_MGMT_MODULE, dc_mgmt_proto_version);

	srv_ep.ep_grp = sys->sy_group;
	srv_ep.ep_tag = daos_rpc_tag(DAOS_REQ_MGMT, 0);
	for (i = 0 ; i < ms_ranks->rl_nr; i++) {
		uint32_t	timeout;

		srv_ep.ep_rank = ms_ranks->rl_ranks[idx];
		rpc = NULL;
		rc = crt_req_create(ctx, &srv_ep, opc, &rpc);
		if (rc != 0) {
			D_ERROR("crt_req_create() failed, "DF_RC"\n",
				DP_RC(rc));
			idx = (idx + 1) % ms_ranks->rl_nr;
			continue;
		}

		/* Shorten the timeout (but not lower than 10 seconds) to speed up pool find */
		rc = crt_req_get_timeout(rpc, &timeout);
		D_ASSERTF(rc == 0, "crt_req_get_timeout: "DF_RC"\n", DP_RC(rc));
		rc = crt_req_set_timeout(rpc, max(10, timeout / 4));
		D_ASSERTF(rc == 0, "crt_req_set_timeout: "DF_RC"\n", DP_RC(rc));

		rpc_in = NULL;
		rpc_in = crt_req_get(rpc);
		D_ASSERT(rpc_in != NULL);
		if (label) {
			rpc_in->pfi_bylabel = 1;
			rpc_in->pfi_label = label;
			uuid_copy(rpc_in->pfi_puuid, null_uuid);
			D_DEBUG(DB_MGMT, "%s: ask rank %u for replicas\n",
				label, srv_ep.ep_rank);
		} else {
			rpc_in->pfi_bylabel = 0;
			rpc_in->pfi_label = MGMT_POOL_FIND_DUMMY_LABEL;
			uuid_copy(rpc_in->pfi_puuid, puuid);
			D_DEBUG(DB_MGMT, DF_UUID": ask rank %u for replicas\n",
				DP_UUID(puuid), srv_ep.ep_rank);
		}

		crt_req_addref(rpc);
		rc = daos_rpc_send_wait(rpc);
		if (rc != 0) {
			D_DEBUG(DB_MGMT, "daos_rpc_send_wait() failed, "
				DF_RC "\n", DP_RC(rc));
			crt_req_decref(rpc);
			idx = (idx + 1) % ms_ranks->rl_nr;
			success = false;
			continue;
		}

		success = true; /* The RPC invocation succeeded. */

		/* Special case: Unpack the response and check for a
		 * -DER_NONEXIST from the upcall handler; in which
		 * case we should retry with another replica.
		 */
		rpc_out = crt_reply_get(rpc);
		D_ASSERT(rpc_out != NULL);
		if (rpc_out->pfo_rc == -DER_NONEXIST) {
			/* This MS replica may have a stale copy of the DB. */
			if (label) {
				D_DEBUG(DB_MGMT, "%s: pool not found on rank %u\n",
					label, srv_ep.ep_rank);
			} else {
				D_DEBUG(DB_MGMT, DF_UUID": pool not found on rank %u\n",
					DP_UUID(puuid), srv_ep.ep_rank);
			}
			if (i + 1 < ms_ranks->rl_nr) {
				crt_req_decref(rpc);
				idx = (idx + 1) % ms_ranks->rl_nr;
			}
			continue;
		}

		break;
	}

	if (!success) {
		if (label)
			D_ERROR("%s: failed to get PS replicas from %d servers"
				", "DF_RC"\n", label, ms_ranks->rl_nr,
				DP_RC(rc));
		else
			D_ERROR(DF_UUID": failed to get PS replicas from %d "
				"servers, "DF_RC"\n", DP_UUID(puuid),
				ms_ranks->rl_nr, DP_RC(rc));
		return rc;
	}

	D_ASSERT(rpc_out != NULL);
	rc = rpc_out->pfo_rc;
	if (rc != 0) {
		if (label) {
			DL_CDEBUG(rc == -DER_NONEXIST, DB_MGMT, DLOG_ERR, rc,
				  "%s: MGMT_POOL_FIND rpc failed to %d ranks", label,
				  ms_ranks->rl_nr);
		} else {
			DL_ERROR(rc, DF_UUID ": MGMT_POOL_FIND rpc failed to %d ranks",
				 DP_UUID(puuid), ms_ranks->rl_nr);
		}
		goto decref;
	}
	if (label)
		uuid_copy(puuid, rpc_out->pfo_puuid);

	rc = d_rank_list_dup(svcranksp, rpc_out->pfo_ranks);
	if (rc != 0) {
		D_ERROR("d_rank_list_dup() failed, " DF_RC "\n", DP_RC(rc));
		goto decref;
	}

	D_DEBUG(DB_MGMT, "rank %u returned pool "DF_UUID"\n",
		srv_ep.ep_rank, DP_UUID(rpc_out->pfo_puuid));

decref:
	crt_req_decref(rpc);
	return rc;
}

int
dc_mgmt_tm_register(const char *sys, const char *jobid, key_t shm_key, uid_t *owner_uid)
{
	struct drpc_alloc          alloc = PROTO_ALLOCATOR_INIT(alloc);
	struct drpc               *ctx;
	Mgmt__ClientTelemetryReq   req = MGMT__CLIENT_TELEMETRY_REQ__INIT;
	Mgmt__ClientTelemetryResp *resp;
	uint8_t                   *reqb;
	size_t                     reqb_size;
	Drpc__Call                *dreq;
	Drpc__Response            *dresp;
	int                        rc;

	if (owner_uid == NULL)
		return -DER_INVAL;

	/* Connect to daos_agent. */
	D_ASSERT(dc_agent_sockpath != NULL);
	rc = drpc_connect(dc_agent_sockpath, &ctx);
	if (rc != -DER_SUCCESS) {
		DL_ERROR(rc, "failed to connect to %s ", dc_agent_sockpath);
		D_GOTO(out, 0);
	}

	req.sys     = (char *)sys;
	req.jobid   = dc_jobid;
	req.shm_key = shm_key;

	reqb_size = mgmt__client_telemetry_req__get_packed_size(&req);
	D_ALLOC(reqb, reqb_size);
	if (reqb == NULL) {
		D_GOTO(out_ctx, rc = -DER_NOMEM);
	}
	mgmt__client_telemetry_req__pack(&req, reqb);

	rc = drpc_call_create(ctx, DRPC_MODULE_MGMT, DRPC_METHOD_MGMT_SETUP_CLIENT_TELEM, &dreq);
	if (rc != 0) {
		D_FREE(reqb);
		goto out_ctx;
	}
	dreq->body.len  = reqb_size;
	dreq->body.data = reqb;

	/* Make the call and get the response. */
	rc = drpc_call(ctx, R_SYNC, dreq, &dresp);
	if (rc != 0) {
		DL_ERROR(rc, "Sending client telemetry setup request failed");
		goto out_dreq;
	}
	if (dresp->status != DRPC__STATUS__SUCCESS) {
		D_ERROR("Client telemetry setup request unsuccessful: %d\n", dresp->status);
		rc = -DER_UNINIT;
		goto out_dresp;
	}

	resp = mgmt__client_telemetry_resp__unpack(&alloc.alloc, dresp->body.len, dresp->body.data);
	if (alloc.oom)
		D_GOTO(out_dresp, rc = -DER_NOMEM);
	if (resp == NULL) {
		D_ERROR("failed to unpack SetupClientTelemetry response\n");
		rc = -DER_NOMEM;
		goto out_dresp;
	}
	if (resp->status != 0) {
		if (resp->status != -DER_UNINIT) /* not necessarily an error */
			DL_ERROR(resp->status, "SetupClientTelemetry() failed");
		rc = resp->status;
		goto out_resp;
	}

	*owner_uid = resp->agent_uid;

out_resp:
	mgmt__client_telemetry_resp__free_unpacked(resp, &alloc.alloc);
out_dresp:
	drpc_response_free(dresp);
out_dreq:
	drpc_call_free(dreq);
out_ctx:
	drpc_close(ctx);
out:
	return rc;
}

static void
wipe_cred_iov(d_iov_t *cred)
{
	/* Ensure credential memory is wiped clean */
	explicit_bzero(cred->iov_buf, cred->iov_buf_len);
	daos_iov_free(cred);
}

int
dc_mgmt_pool_list(tse_task_t *task)
{
	daos_mgmt_pool_list_t     *args;
	d_rank_list_t             *ms_ranks;
	struct rsvc_client         ms_client;
	crt_endpoint_t             ep;
	crt_rpc_t                 *rpc = NULL;
	crt_opcode_t               opc;
	struct mgmt_pool_list_in  *in  = NULL;
	struct mgmt_pool_list_out *out = NULL;
	struct dc_mgmt_sys        *sys;
	int                        pidx;
	int                        rc;

	args = dc_task_get_args(task);
	if (args->npools == NULL) {
		D_ERROR("npools argument must not be NULL");
		D_GOTO(out, rc = -DER_INVAL);
	}

	rc = dc_mgmt_sys_attach(args->grp, &sys);
	if (rc != 0) {
		DL_ERROR(rc, "cannot attach to DAOS system: %s", args->grp);
		D_GOTO(out, rc);
	}

	ms_ranks = sys->sy_info.ms_ranks;
	D_ASSERT(ms_ranks->rl_nr > 0);

	rc = rsvc_client_init(&ms_client, ms_ranks);
	if (rc != 0) {
		DL_ERROR(rc, "failed to init ms client");
		D_GOTO(out_grp, rc);
	}

	ep.ep_grp = sys->sy_group;
	ep.ep_tag = daos_rpc_tag(DAOS_REQ_MGMT, 0);
	opc       = DAOS_RPC_OPCODE(MGMT_POOL_LIST, DAOS_MGMT_MODULE, DAOS_MGMT_VERSION);

rechoose:
	rc = rsvc_client_choose(&ms_client, &ep);
	if (rc != 0) {
		DL_ERROR(rc, "failed to choose MS rank");
		D_GOTO(out_client, rc);
	}

	rc = crt_req_create(daos_task2ctx(task), &ep, opc, &rpc);
	if (rc != 0) {
		DL_ERROR(rc, "crt_req_create(MGMT_POOL_LIST) failed");
		D_GOTO(out_client, rc);
	}

	in          = crt_req_get(rpc);
	in->pli_grp = (d_string_t)args->grp;
	/* If provided pools is NULL, caller needs the number of pools
	 * to be returned in npools. Set npools=0 in the request in this case
	 * (caller value may be uninitialized).
	 */
	if (args->pools == NULL)
		in->pli_npools = 0;
	else
		in->pli_npools = *args->npools;

	/* Now fill in the client credential for the pool access checks. */
	rc = dc_sec_request_creds(&in->pli_cred);
	if (rc != 0) {
		DL_ERROR(rc, "failed to obtain security credential");
		D_GOTO(out_put_req, rc);
	}

	D_DEBUG(DB_MGMT, "req_npools=" DF_U64 " (pools=%p, *npools=" DF_U64 "\n", in->pli_npools,
		args->pools, *args->npools);

	crt_req_addref(rpc);
	rc = daos_rpc_send_wait(rpc);
	if (rc != 0) {
		DL_ERROR(rc, "rpc send failed");
		wipe_cred_iov(&in->pli_cred);
		crt_req_decref(rpc);
		goto rechoose;
	}

	out = crt_reply_get(rpc);
	D_ASSERT(out != NULL);

	rc = rsvc_client_complete_rpc(&ms_client, &ep, rc, out->plo_op.mo_rc, &out->plo_op.mo_hint);
	if (rc == RSVC_CLIENT_RECHOOSE) {
		wipe_cred_iov(&in->pli_cred);
		crt_req_decref(rpc);
		goto rechoose;
	}

	rc = out->plo_op.mo_rc;
	if (rc != 0)
		D_GOTO(out_put_req, rc);

	*args->npools = out->plo_npools;

	/* copy RPC response pools info to client buffer, if provided */
	if (args->pools) {
		/* Response ca_count expected <= client-specified npools */
		for (pidx = 0; pidx < out->plo_pools.ca_count; pidx++) {
			struct mgmt_pool_list_pool *rpc_pool = &out->plo_pools.ca_arrays[pidx];
			daos_mgmt_pool_info_t      *cli_pool = &args->pools[pidx];

			uuid_copy(cli_pool->mgpi_uuid, rpc_pool->plp_uuid);

			cli_pool->mgpi_label = NULL;
			D_STRNDUP(cli_pool->mgpi_label, rpc_pool->plp_label,
				  DAOS_PROP_LABEL_MAX_LEN);
			if (cli_pool->mgpi_label == NULL) {
				D_ERROR("copy RPC reply label failed\n");
				D_GOTO(out_free_args_pools, rc = -DER_NOMEM);
			}

			/* allocate rank list for caller (simplifies API) */
			cli_pool->mgpi_svc = NULL;
			rc = d_rank_list_dup(&cli_pool->mgpi_svc, rpc_pool->plp_svc_list);
			if (rc != 0) {
				D_ERROR("copy RPC reply svc list failed\n");
				D_GOTO(out_free_args_pools, rc = -DER_NOMEM);
			}
		}
	}

out_free_args_pools:
	if (args->pools && (rc != 0)) {
		for (pidx = 0; pidx < out->plo_pools.ca_count; pidx++) {
			daos_mgmt_pool_info_t *pool = &args->pools[pidx];

			if (pool->mgpi_label)
				D_FREE(pool->mgpi_label);
			if (pool->mgpi_svc)
				d_rank_list_free(pool->mgpi_svc);
		}
	}
out_put_req:
	if (rc != 0)
		DL_ERROR(rc, "failed to list pools");

	wipe_cred_iov(&in->pli_cred);
	crt_req_decref(rpc);
out_client:
	rsvc_client_fini(&ms_client);
out_grp:
	dc_mgmt_sys_detach(sys);
out:
	tse_task_complete(task, rc);
	return rc;
}

/**
 * Initialize management interface
 */
int
dc_mgmt_init()
{
	int		rc;
	uint32_t        ver_array[2] = {DAOS_MGMT_VERSION - 1, DAOS_MGMT_VERSION};

	rc = daos_rpc_proto_query(mgmt_proto_fmt_v2.cpf_base, ver_array, 2, &dc_mgmt_proto_version);
	if (rc)
		return rc;

	if (dc_mgmt_proto_version == DAOS_MGMT_VERSION - 1) {
		rc = daos_rpc_register(&mgmt_proto_fmt_v2, MGMT_PROTO_CLI_COUNT,
				       NULL, DAOS_MGMT_MODULE);
	} else if (dc_mgmt_proto_version == DAOS_MGMT_VERSION) {
		rc = daos_rpc_register(&mgmt_proto_fmt_v3, MGMT_PROTO_CLI_COUNT,
				       NULL, DAOS_MGMT_MODULE);
	} else {
		D_ERROR("version %d mgmt RPC not supported.\n", dc_mgmt_proto_version);
		rc = -DER_PROTO;
	}
	if (rc != 0)
		D_ERROR("failed to register mgmt RPCs: "DF_RC"\n", DP_RC(rc));

	return rc;
}

/**
 * Finalize management interface
 */
void
dc_mgmt_fini()
{
	int	rc = 0;

	if (dc_mgmt_proto_version == DAOS_MGMT_VERSION - 1)
		rc = daos_rpc_unregister(&mgmt_proto_fmt_v2);
	else if (dc_mgmt_proto_version == DAOS_MGMT_VERSION)
		rc = daos_rpc_unregister(&mgmt_proto_fmt_v3);

	if (rc != 0)
		D_ERROR("failed to unregister mgmt RPCs: "DF_RC"\n", DP_RC(rc));
}

int dc2_mgmt_svc_rip(tse_task_t *task)
{
	return -DER_NOSYS;
}
