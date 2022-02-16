#pragma once

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <libmemcached/memcached.h>

#include "common/common.h"

namespace metafs {

struct mc_ctx
{
	char ip[64];
	int port;
	memcached_st *memc;
};

static inline struct mc_ctx *mt_create_ctx(const char *ip, int port)
{
	memcached_server_st *servers = NULL;
	struct mc_ctx *mi;
	memcached_st *memc = memcached_create(NULL);
	memcached_return rt;

	servers = memcached_server_list_append(servers, ip, port, &rt);
	p_assert(rt == MEMCACHED_SUCCESS, "");
	rt = memcached_server_push(memc, servers);
	p_assert(rt == MEMCACHED_SUCCESS, "");
	mi = (struct mc_ctx *)safe_alloc(sizeof(struct mc_ctx), true);
	strcpy(mi->ip, ip);
	mi->port = port;
	mi->memc = memc;
	return mi;
}
static inline void mt_destory_ctx(struct mc_ctx *ctx)
{
	memcached_free(ctx->memc);
	free(ctx);
}

static inline void mt_set(struct mc_ctx *mi, const char *key, const char *value, int len)
{
	memcached_return rc;
	p_assert(mi && key && len > 0 && value, "");
	rc = memcached_set(mi->memc, key, strlen(key), value, len, (time_t)0, 0);
	p_assert(rc == MEMCACHED_SUCCESS, "");
}

//found 0 notfound 1
static inline int mt_get(struct mc_ctx *mi, const char *key, char **value, int *len)
{
	unsigned flg;
	memcached_return rt;
	size_t slen;
	p_assert(mi && key, "");
	char *val = memcached_get(mi->memc, key, strlen(key), &slen, &flg, &rt);
	p_assert(rt == MEMCACHED_SUCCESS || rt == MEMCACHED_NOTFOUND, "");
	if (value)
		*value = val;
	else
		free(val);
	if (len)
		*len = slen;
	if (rt == MEMCACHED_SUCCESS)
		return 0;
	return 1;
}

// add 1
static inline int mt_incr(struct mc_ctx *mi, const char *key)
{
	memcached_return rt;
	long unsigned int val;
	p_assert(mi && key, "");
	rt = memcached_increment(mi->memc, key, strlen(key), 1, &val);
	p_info("rt %d %ld ", rt, val);
	p_assert(rt == MEMCACHED_SUCCESS && val, "");
	return ((int)val) - 1;
}

}
