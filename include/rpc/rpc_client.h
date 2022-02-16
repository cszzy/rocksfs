#pragma once

#include "rpc/rpc_common.h"
#include "eRPC/src/rpc.h"
#include "util/jump_hash.h"
#include "client/open_dir.h"
#include "client/open_file_map.h"
#include "client/hooks.h"
#include "common/fs.h"
#include "common/common.h"
#include "util/fs_log.h"

#include "xxHash/xxh3.h"

#include <glog/logging.h>
#include <rocksdb/cache.h>

#define C_RPC_CONTEXT_WINDOW(index) (c_ctx->window_[index])
#define C_RPC_REQ_BUF(index) (reinterpret_cast<wire_req_t *>(C_RPC_CONTEXT_WINDOW(index).req_msgbuf_.buf_))
#define C_RPC_RESP_BUF(index) (reinterpret_cast<wire_resp_t*>(C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_.buf_))

namespace metafs {

/// A basic session management handler that expects successful responses
static inline void client_basic_sm_handler(int session_num, erpc::SmEventType sm_event_type,
                      erpc::SmErrType sm_err_type, void *_context) {
  auto *c = static_cast<client_context *>(_context);
  c->num_sm_resps_++;

  erpc::rt_assert(
      sm_err_type == erpc::SmErrType::kNoError,
      "SM response with error " + erpc::sm_err_type_str(sm_err_type));

  if (!(sm_event_type == erpc::SmEventType::kConnected ||
        sm_event_type == erpc::SmEventType::kDisconnected)) {
    throw std::runtime_error("Received unexpected SM event.");
  }

  // The callback gives us the eRPC session number - get the index in vector
  size_t session_idx = c->session_num_vec_.size();
  for (size_t i = 0; i < c->session_num_vec_.size(); i++) {
    if (c->session_num_vec_[i] == session_num) session_idx = i;
  }
  // printf("session_idx: %d, c->session_num_vec_.size():%d\n", session_idx, c->session_num_vec_.size());
  erpc::rt_assert(session_idx < c->session_num_vec_.size(),
                  "SM callback for invalid session number.");
}

class RpcClient {
  public:
    RpcClient(RpcClient const&) = delete;

    void operator=(RpcClient const&) = delete;

    RpcClient(size_t rpc_id = 0) {
      c_ctx->rpc_ = new erpc::Rpc<erpc::CTransport>(c_ctx->nexus_, static_cast<void *>(c_ctx),
                            static_cast<uint8_t>(rpc_id), client_basic_sm_handler);
      c_ctx->rpc_->retry_connect_on_invalid_rpc_id_ = true;

      size_t gid_ = c_ctx->id * c_cfg->num_client_threads + rpc_id;
      c_ctx->session_num_vec_.resize(c_ctx->total_servers);
      // 连接到server的所有前台线程
      for(int i = 0; i < c_cfg->num_servers; i++) {
        for(int j = 0; j < c_cfg->server_fg_threads; j++) {
          LOG(INFO) << "Client gid#" << gid_ << " connect to " <<"server [" << c_cfg->server_list[i] << "], thread_id#" << j;
          int session_id = i * c_cfg->server_fg_threads + j;
          c_ctx->session_num_vec_[session_id] = c_ctx->rpc_->create_session(c_cfg->server_list[i], j);
        }
      }

      // wait for all session connect
      while(c_ctx->num_sm_resps_ != c_ctx->total_servers) {
        c_ctx->rpc_->run_event_loop_once();
      }

      for(int i = 0; i < c_cfg->num_servers; i++) {
        for(int j = 0; j < c_cfg->server_fg_threads; j++) {
          int session_id = i * c_cfg->server_fg_threads + j;
          if(!c_ctx->rpc_->is_connected(c_ctx->session_num_vec_[session_id])) {
            p_assert(false, "erpc not connected");
          }
        }
      }

      LOG(INFO) << "connected complete";

      alloc_req_resp_msg_buffers(c_ctx);
      LOG(INFO) << "RPC Client Init Sucess";
    }

    ~RpcClient() {
    }
  
    rpc_resp_t RPC_Getinode(const metafs_inode_t pinode, const string &fname, metafs_inode_t &inode);

    rpc_resp_t RPC_Getstat(metafs_inode_t pinode, const string &fname, metafs_inode_t &inode, metafs_stat_t &stat);

    rpc_resp_t RPC_Mknod(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat);

    rpc_resp_t RPC_Open(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat);

    rpc_resp_t RPC_Unlink(metafs_inode_t pinode, const string &fname);

    rpc_resp_t RPC_Rmdir(metafs_inode_t pinode, const string &fname);

    rpc_resp_t RPC_Mkdir(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode);

    rpc_resp_t RPC_Readdir(metafs_inode_t pinode, shared_ptr<OpenDir> &open_dir);

    // use in Rename.
    rpc_resp_t RPC_Create(metafs_inode_t pinode, const string &fname, const metafs_stat_t &stat);

    // use in rename
    rpc_resp_t RPC_Remove(metafs_inode_t pinode, const string &fname, mode_t mode);

    rpc_resp_t RPC_ReadRegionmap();

    // Allocate request and response MsgBuffers
    void alloc_req_resp_msg_buffers(client_context *c) {
      for (size_t msgbuf_idx = 0; msgbuf_idx < MAX_MSG_BUF_WINDOW; msgbuf_idx++) {
        c->window_[msgbuf_idx].req_msgbuf_ =
            c->rpc_->alloc_msg_buffer_or_die(sizeof(wire_req_t));

        c->window_[msgbuf_idx].resp_msgbuf_ =
            c->rpc_->alloc_msg_buffer_or_die(sizeof(wire_resp_t));
      
      }
    }
};

} // end namespace metafs