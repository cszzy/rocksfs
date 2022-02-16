#pragma once

#include "common/fs.h"
#include "client/dentry_cache.h"
#include "common/region.h"
#include "rpc/rpc_common.h"

#include "eRPC/src/rpc.h"

#include <sys/types.h>
#include <fcntl.h>
#include <unordered_map>

struct statfs;
struct linux_dirent;
struct linux_dirent64;

#define HOOK_REWRITE 0

#if HOOK_REWRITE
extern bool init_rewrite_flag;
#endif

// #define MAX_MSG_BUF_WINDOW

namespace metafs {

struct client_config {
    // client's attr
    int32_t num_client_threads;
    char *local_ip;

    // metaserver URI list
    int32_t num_servers;
    char **server_list;

    // metaserver's erpc's fg and bg threads num
    int32_t server_fg_threads;
    int32_t server_bg_threads;

    // fliesystem mountdir and mountdir's len
    char *mountdir;
    int32_t mountdir_len;

    // memcached server ip and port
    char *memcached_ip;
    int32_t memcached_port;
};

// 客户端查询一个key属于哪个region,查询时将key抓换为 <key, key> pair 进行范围查询
// 测试在test/region_test.cc
struct region_cmp_func {
    bool operator()(const ClientRegion &lhs, const ClientRegion &rhs) const {
        return lhs.end_key.hi < rhs.start_key.hi;
    }
};

// 客户端的region按照regionkey排序
// 根据博客测试，使用map的包装的find()速度不如顺序查找，所以目前在客户端定位region仍然采用顺序查找
typedef std::map<ClientRegion, region_id_t, region_cmp_func> region_map_t;

struct client_context {
    int32_t id;
    char *local_uri;
    erpc::Nexus *nexus_;
    DentryCache<DirentryValue> *dentry_cache;

    erpc::Rpc<erpc::CTransport> *rpc_;
    int32_t total_servers;
    std::vector<int> session_num_vec_;
    int num_sm_resps_;

    region_map_t region_map;

    struct {
        erpc::MsgBuffer req_msgbuf_;
        erpc::MsgBuffer resp_msgbuf_;
    } window_[MAX_MSG_BUF_WINDOW];
};

extern struct client_config *c_cfg;

extern struct client_context *c_ctx;

// return -1 if not find, else return region id the regionkey belong to
// 目前使用顺序查找，可以考虑使用find()函数
static inline region_id_t find_region_id(const RegionKey &regionkey) {
    for(auto iter = c_ctx->region_map.begin(); iter != c_ctx->region_map.end(); iter++) {
        if(regionkey.hi >= iter->first.start_key.hi && regionkey.hi <= iter->first.end_key.hi) {
            return iter->second;
        }
    }
    return -1;// not find
}

void init_client_ctx();

int metafs_hook(long syscall_number,
                long a0, long a1,
                long a2, long a3,
                long a4, long a5,
                long *res);

} // end namespace metafs

