#pragma once

#include <stdint.h>
#include <sys/stat.h> 
#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <unordered_set>
#include <unordered_map>

#include "common/fs.h"
#include "common/region.h"
#include "common/common.h"
#include "rpc/rpc_common.h"
#include "util/parser.h"
#include "util/memcached_tool.h"
#include "util/bitmap.h"
#include "util/rwlock.h"
#include "util/threadsafe_queue.h"
#include "server/region_and_log.h"

#include "eRPC/src/rpc.h"
#include "xxHash/xxhash.h"

#ifdef __cplusplus
extern "C" {
#include "metakv/include/metadb.h"
#include "metakv/indexes/status.h"
#include "metakv/include/option.h"
#endif
#ifdef __cplusplus
}
#endif

using namespace std;

#define MAX_FG_THREADS 10

#define S_CTX_RPC_WINDOW(index) (s_ctx->window_[index])
#define S_RPC_REQ_BUF(index) (reinterpret_cast<wire_req_t *>(S_CTX_RPC_WINDOW(index).req_msgbuf_.buf_))
#define S_RPC_RESP_BUF(index) (reinterpret_cast<wire_resp_t*>(S_CTX_RPC_WINDOW(index).resp_msgbuf_.buf_))

#define ST_CTX_RPC_WINDOW(index) (st_ctx->window_[index])
#define ST_RPC_REQ_BUF(index) (reinterpret_cast<wire_req_t *>(ST_CTX_RPC_WINDOW(index).req_msgbuf_.buf_))
#define ST_RPC_RESP_BUF(index) (reinterpret_cast<wire_resp_t*>(ST_CTX_RPC_WINDOW(index).resp_msgbuf_.buf_))

namespace metafs {

// 次高位掩码,使用在log中，结果为1表示为PUT op，为0则为DELETE op
const metafs_inode_t inode_prefix_ssb = ((metafs_inode_t)1)<<(metafs_inode_size*8-2);
// const metafs_inode_t inode_prefix_ssb = 0x4000000000000000;

// 负责全局region id分配，之后需要移到zk
extern atomic<region_id_t> global_region_id;

extern unordered_map<region_id_t, ClientRegion> global_region_map;
// region_map的rwlock
extern RWLock global_region_map_rwlock;

struct server_config {
  int32_t id; // 从memcached分配id

  char *local_ip; 
  int32_t local_port;

  // metaserver URI list
  int num_servers;
  char **server_list;

  // metaserver's erpc's fg and bg threads num
  int32_t server_fg_threads;
  int32_t server_bg_threads;

  // per metakv alloc PM_space
  uint64_t metakv_pm_space; //单位为GB

  // metakv path and rocksdb path
  char *metakv_path; // store kv
  char *rocksdb_path; // store log

  // memcached server ip and port
  char *memcached_ip;
  int memcached_port;
};

// 每个前台线程的context
struct server_context {
  size_t thread_id;
  size_t global_id; // 每个server线程有全局唯一id

  metafs_inode_t alloc_inode; //当前分配到的inode（最高位为0，如果是目录还需要将分配的inode最高位设置为1）
  MetaDb *metadb;
  rocksdb::DB *log_db;
  erpc::Rpc<erpc::CTransport> *rpc;

  unordered_map<region_id_t, ServerRegion*> region_map;
  RWLock region_map_lock;

  // 使用map来直接遍历所有的元素，而不是尝试hash范围中的每一个值
  std::map<uint64_t, set<metafs_inode_t>> pinode_table; // 存储hash(pinode)->set(pinode)的集合,用于判断哪些pinode可能需要进行迁移
  RWLock pinode_table_lock;
};

// 后台region_split线程的context
struct split_region_thread_context {
  size_t thread_id;
  erpc::Rpc<erpc::CTransport> *rpc;
  int32_t num_sm_resps_;

  // server to server rpc connect 
  std::vector<int> s2s_session_num_vec; 
  int32_t num_server_sessions;

  struct {
    erpc::MsgBuffer req_msgbuf_;
    erpc::MsgBuffer resp_msgbuf_;
  } window_[MAX_MSG_BUF_WINDOW]; // 每一个正在分裂的region占用一个window
  // struct bitmap *window_bitmap; // 维持window使用情况的bitmap
};

extern struct server_config *s_cfg;

extern erpc::Nexus* s_nexus; // 每个进程一个

// 存储所有前台线程的context
extern struct server_context s_ctx_arr[MAX_FG_THREADS]; // 伪共享问题？

// 每个前台线程使用s_ctx处理client请求
// split_thread线程使用其指向当前split的region
extern thread_local struct server_context *s_ctx; 

// 需要进行region_split的线程将thread_id和region_id放入共享队列
extern threadsafe_queue<pair<size_t, region_id_t>> shared_split_queue; 

extern struct split_region_thread_context *st_ctx;

void init_server_config();
void init_server_context(size_t thread_id);
void init_and_start_loop();

// void server_sm_handler(int, erpc::SmEventType, erpc::SmErrType, void *);

void run_server_thread(size_t thread_id);

void split_region_thread(size_t thread_id);

// 解析json文件，初始化config
void server_parse_config(const char *fn);

// inode(64bits)格式：最高位0标识为文件，为1标识为目录。每个服务器的每个线程占用16 bits保证inode分配不重叠：
// |--1bit:whether is file or directory--|--7bits:not use now--|--8bits:server_id--|--8bits:server_thread_id--|--40 bits:用来分配inode--|
static inline void init_alloc_inode() {
  s_ctx->alloc_inode = ((s_cfg->id << 8) | s_ctx->thread_id) << (metafs_inode_size*8 - 24);
  s_ctx->alloc_inode++;
}

// erpc filesystem function, client to server rpc

void fs_open_handler(erpc::ReqHandle *req_handle, void *_context);
void fs_mknod_handler(erpc::ReqHandle *req_handle, void *_context);
void fs_stat_handler(erpc::ReqHandle *req_handle, void *_context);
void fs_unlink_handler(erpc::ReqHandle *req_handle, void *_context);
void fs_mkdir_handler(erpc::ReqHandle *req_handle, void *_context);
void fs_readdir_handler(erpc::ReqHandle *req_handle, void *_context);
void fs_rmdir_handler(erpc::ReqHandle *req_handle, void *_context);
void fs_getinode_handler(erpc::ReqHandle *req_handle, void *_context);

void read_region_map_handler(erpc::ReqHandle *req_handle, void *_context);

// for region split, server to server rpc

void s2s_create_region_handler(erpc::ReqHandle *req_handle, void *_context);
void s2s_send_region_handler(erpc::ReqHandle *req_handle, void *_context);
void s2s_send_region_log_handler(erpc::ReqHandle *req_handle, void *_context);

static inline bool is_directory(metafs_inode_t inode) {
  return (inode & inode_prefix_msb) != 0;
}

static inline bool check_status_ok(const MetaKvStatus status) {
  return status == MetaKvStatus::OK
          ||status == MetaKvStatus::INSERT_OK
          ||status == MetaKvStatus::UPDATE_OK;
}

rpc_resp_t convert_status_to_resptype(MetaKvStatus status);

/// A basic session management handler that expects successful responses
static inline void st_basic_sm_handler(int session_num, erpc::SmEventType sm_event_type,
                      erpc::SmErrType sm_err_type, void *_context) {
  auto *c = static_cast<split_region_thread_context *>(_context);
  c->num_sm_resps_++;

  erpc::rt_assert(
      sm_err_type == erpc::SmErrType::kNoError,
      "SM response with error " + erpc::sm_err_type_str(sm_err_type));

  if (!(sm_event_type == erpc::SmEventType::kConnected ||
        sm_event_type == erpc::SmEventType::kDisconnected)) {
    throw std::runtime_error("Received unexpected SM event.");
  }

  // The callback gives us the eRPC session number - get the index in vector
  size_t session_idx = c->s2s_session_num_vec.size();
  for (size_t i = 0; i < c->s2s_session_num_vec.size(); i++) {
    if (c->s2s_session_num_vec[i] == session_num) session_idx = i;
  }
  // printf("session_idx: %d, c->s2s_session_num_vec.size():%d\n", session_idx, c->s2s_session_num_vec.size());
  erpc::rt_assert(session_idx < c->s2s_session_num_vec.size(),
                  "SM callback for invalid session number.");
}
}

