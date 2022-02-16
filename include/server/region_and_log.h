#pragma once

#include "common/region.h"
#include "rpc/rpc_common.h"
#include "common/fs.h"
#include "common/common.h"
#include "server/metafs_server.h"

namespace metafs {


// 初始化时向全局map注册region, 之后移到zk
void register_region();

// log格式: key: region_id + log_id, value: pinode(PUT(1)/GET(0)嵌入inode次高位) + fname + metafs_stat
struct log_key {
    region_id_t region_id;
    uint32_t log_id;

    log_key(region_id_t region_id, uint32_t log_id) : region_id(region_id), log_id(log_id) {}

    rocksdb::Slice ToSlice() const {
        return rocksdb::Slice((const char*) this, sizeof(log_key));
    }
};

// 日志状态
enum LogStatus : int32_t {
    FarToDone,
    AlmostDone,
    Done,
};

// log put op
// mknod和mkdir操作更新两张表: pinode + fname -> inode,  inode -> stat
// 对于只更新stat的文件操作只需要更新一张表 inode -> stat
struct put_log_val {
    metafs_inode_t pinode;
    metafs_inode_t inode;
    metafs_stat_t stat;
    bool only_update_stat; // 标记是否为只更新stat
    char fname[METAFS_MAX_FNAME_LEN];

    put_log_val(metafs_inode_t pinode, metafs_inode_t inode, const char *str, const metafs_stat_t *st) 
                    : pinode(pinode), inode(inode) {
        memcpy((void*)&stat, (void*)st, metafs_stat_size);
        p_assert(strlen(str) < METAFS_MAX_FNAME_LEN, "fname is too long");
        strcpy(fname, str);
    }

    rocksdb::Slice ToSlice() const {
        return rocksdb::Slice((const char*) this, offsetof(put_log_val, fname) + strlen(fname) + 1);
    }
};

// log delete op
// 同样更新两张表: pinode + fname -> inode,  inode -> stat
struct delete_log_val {
    metafs_inode_t pinode;
    char fname[METAFS_MAX_FNAME_LEN];

    delete_log_val(metafs_inode_t pinode, const char *str) : pinode(pinode) {
        p_assert(strlen(str) < METAFS_MAX_FNAME_LEN, "fname is too long");
        strcpy(fname, str);
    }

    rocksdb::Slice ToSlice() const {
        return rocksdb::Slice((const char*) this, offsetof(delete_log_val, fname) + strlen(fname) + 1);
    }
};

// if need to serve client req, return true.
bool check_region_status(ServerRegion *region);

// 检查客户端op是否属于该region(左闭右闭区间), 如果不属于，则通知客户端刷新regionmap缓存
static inline bool check_is_blong_to_region(const ServerRegion *region, uint64_t inode_hash) {
    return inode_hash >= region->start_key.hi && inode_hash <= region->end_key.hi;
}

void check_region_and_split(ServerRegion *region);

void log_op(ServerRegion *region, bool is_delete_op,
        metafs_inode_t pinode, const char *fname,
        const metafs_stat_t *stat = nullptr, metafs_inode_t inode = 0);

// region RPC interfaces
// origin server向target server发送create_region RPC
void rpc_create_region(region_id_t region_id);

// origin server向target server发送region, target server重建kv
// void rpc_send_region(ServerRegion *region, uint64_t msg_idx, metafs_inode_t pinode, uint64_t next_offset);
void rpc_send_region(ServerRegion *region, uint64_t msg_idx, uint64_t pinode_hash, metafs_inode_t pinode = 0, uint64_t next_offset = 0);

// rebuild region后，origin server发送log给target server, target apply log
// 在log almost done时停止服务客户端请求，请求需重定向到target server
// 所有log apply后，target server开始服务客户端请求
void rpc_send_region_log(ServerRegion *region, int32_t next_log_id, uint64_t msg_idx);

// 几个回调函数
void rpc_create_region_cb(void *_context, void *_idx);

void rpc_send_region_cb(void *_context, void *_idx);

void rpc_send_region_log_cb(void *_context, void *_idx);

}