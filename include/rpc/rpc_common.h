#pragma once

#include <vector>

#include "eRPC/src/rpc.h"
#include "common/fs.h"
#include "common/region.h"

using namespace std;

// 当前配置下erpc message buffer最大长度为3824B，设置一个小点的MAX_SIZE
#define MSG_ENTEY_MAX_SIZE 3712
#define MAX_MSG_BUF_WINDOW 8

namespace metafs {

static const uint16_t kUDPPort_Client = 31855;

// use for eRPC callback, set idx to true.
static inline void set_complete_cb(void *_context, void *_idx) {
    *reinterpret_cast<bool*>(_idx) = true;
}

// RPC request type
enum kReqType : uint8_t {
  // client to server rpc
  kFSOpenReq, // open a file or directory
  kFSUnlinkReq, // remove a file
  kFSStatReq, // stat file or directory
  kFSMknodReq, // create a file
  kFSReaddirReq, // read a directory
  kFSMkdirReq, // create a directory
  kFSRmdirReq, // remove a directory
  kFSGetinodeReq, // get file/dir 's inode

  kReadRegionmap,

  // server to server rpc
  kCreateRegionReq, // origin server send a create_region rpc to target server
  kSendRegionReq, // origin server send region to target server
  kSendRegionLogReq, // origin server send region_log to target server
}; 

struct wire_req_t {
  union{
    struct {
      region_id_t region_id;
      metafs_inode_t pinode;
      uint64_t pinode_hash;
      char fname[METAFS_MAX_FNAME_LEN]; // fname end with '\0'
    }FSGetinodeReq;

    struct {
      region_id_t region_id;
      metafs_inode_t pinode;
      uint64_t pinode_hash;
      mode_t mode;
      char fname[METAFS_MAX_FNAME_LEN];
    }FSOpenReq;

    struct {
      region_id_t region_id;
      metafs_inode_t pinode;
      uint64_t pinode_hash;
      char fname[METAFS_MAX_FNAME_LEN];
    }FSUnlinkReq;

    struct {
      region_id_t region_id;
      metafs_inode_t pinode;
      uint64_t pinode_hash;
      char fname[METAFS_MAX_FNAME_LEN];
    }FSStatReq;

    struct {
      region_id_t region_id;
      oid_t oid; // use for daos object IO
      metafs_inode_t pinode;
      uint64_t pinode_hash;
      mode_t mode;
      char fname[METAFS_MAX_FNAME_LEN];
    }FSMknodReq;

    struct {
      region_id_t region_id;
      metafs_inode_t inode;
      uint64_t inode_hash;
      uint64_t offset; // the offset last Readdir rpc read
    }FSReaddirReq;

    struct {
      region_id_t region_id;
      metafs_inode_t pinode;
      uint64_t pinode_hash;
      mode_t mode;
      char fname[METAFS_MAX_FNAME_LEN];
    }FSMkdirReq;

    struct {
      region_id_t region_id;
      metafs_inode_t pinode;
      uint64_t pinode_hash;
      char fname[METAFS_MAX_FNAME_LEN];
    }FSRmdirReq;

    struct {
      int32_t client_id; // 随便传个数据
    }ReadRegionmapReq;

    // server to server rpc
    struct {
      region_id_t region_id;
      RegionKey start_key;
      RegionKey end_key;
    }CreateRegionReq; // origion server send to target server

    struct {
      region_id_t region_id;
      int64_t is_uncomplete; // 标识region是否发送完成
      int64_t num_result; // 条目数
      int64_t entries_len; // actual entries' length
      uint64_t pinode_hash; // for s_ctx::pinode_table
      metafs_inode_t pinode; // 如果没传输完，记录下一次传入的pinode
      int64_t offset; // 如果没传输完,记录下一次传入的offset
      // entries内每条entry结构:pinode(8B)+fname(end with '\0) + inode(8B,最高位存储文件类型file/dir, 1是目录) + stat(48B) 
      uint8_t entries[MSG_ENTEY_MAX_SIZE];
    }SendRegionReq; // origion server send to target server

    // entry中每条log格式:
      // put_log格式: pinode(PUT(1)/GET(0)嵌入inode次高位) + inode + metafs_stat + fname(end with '\0)
      // delete log格式: pinode(PUT(1)/GET(0)嵌入inode次高位) + fname(end with '\0)
    struct {
      region_id_t region_id; // log对应的region
      int32_t log_status; // log
      int32_t next_log_id; // 下一次target要读取的log_id
      int32_t num_logs; // 文件数
      int32_t entries_len; // actual entries' length
      uint8_t entries[MSG_ENTEY_MAX_SIZE];
    }SendRegionLogReq;
  };
};

const size_t FSGetinodeReq_size = sizeof(wire_req_t::FSGetinodeReq);
const size_t FSOpenReq_size = sizeof(wire_req_t::FSOpenReq);
const size_t FSUnlinkReq_size = sizeof(wire_req_t::FSUnlinkReq);
const size_t FSStatReq_size = sizeof(wire_req_t::FSStatReq);
const size_t FSMknodReq_size = sizeof(wire_req_t::FSMknodReq);
const size_t FSReaddirReq_size = sizeof(wire_req_t::FSReaddirReq);
const size_t FSMkdirReq_size = sizeof(wire_req_t::FSMkdirReq);
const size_t FSRmdirReq_size = sizeof(wire_req_t::FSRmdirReq);

const size_t CreateRegionReq_size = sizeof(wire_req_t::CreateRegionReq);
const size_t SendRegionReq_size = sizeof(wire_req_t::SendRegionReq);
const size_t SendRegionLogReq_size = sizeof(wire_req_t::SendRegionLogReq);

typedef uint32_t rpc_resp_t;
enum RespType : rpc_resp_t {
  kSuccess, 
  kFail,
  kEXIST, // file exists
  kENOENT, // no such file or directory
  kEIO, // IO error
  kENOTDIR, // not a dir
  kEISDIR, // is a directory
  kEBUSY, // Device or resource busy, client need to retry
  kUpdateRegionMap, // notice client to Update ServerRegion map
};

struct wire_resp_t {
  union {
    struct  {
      rpc_resp_t resp_type;
      metafs_inode_t inode;
    }FSGetinodeResp;

    struct {
      rpc_resp_t resp_type;
      metafs_inode_t inode;
      metafs_stat_t stat;
    }FSOpenResp;

    struct {
      rpc_resp_t resp_type;
      metafs_inode_t inode;
      metafs_stat_t stat;
    }FSStatResp;

    struct {
      rpc_resp_t resp_type;
      metafs_inode_t inode;
      metafs_stat_t stat;
    }FSMknodResp;

    struct {
      rpc_resp_t resp_type;
      metafs_inode_t inode;
    }FSMkdirResp;

    struct {
      rpc_resp_t resp_type;
    }FSUnlinkResp;

    struct {
      rpc_resp_t resp_type;
    }FSRmdirResp;

    struct {
      rpc_resp_t resp_type;
      int32_t is_uncomplete; // 是否读完
      int32_t num_result; // 文件数
      int32_t entries_len; // actual entries' length
      int64_t next_offset; // 如果没读完,则记录下一次readdir RPC的offset
      // entries内每一条entry结构:pinode(8B)+fname(end with '\0')+inode(8B,最高位存储文件类型file/dir, 1是目录)
      uint8_t entries[MSG_ENTEY_MAX_SIZE]; 
    }FSReaddirResp;

    struct {
      rpc_resp_t resp_type;
      int32_t num_entries;
      ClientRegion entries[MSG_ENTEY_MAX_SIZE/sizeof(ClientRegion)]; 
    }ReadRegionmapResp; // client read region map from server

    struct {
      rpc_resp_t resp_type;
      region_id_t region_id;
    }CreateRegionResp; // for target server

    struct {
      rpc_resp_t resp_type;
      int32_t is_uncomplete; // 是否读完
      region_id_t region_id; // 目标region_id
      uint64_t pinode_hash; // for s_ctx::pinode_table
      metafs_inode_t pinode; // 如果没传输完，记录下一次传入的pinode
      uint64_t offset; // 如果没传输完,记录下一次传入的offset
    }SendRegionResp; // for origin server

    struct {
      rpc_resp_t resp_type;
      int32_t log_status; 
      region_id_t region_id;
      uint32_t next_log_id; // 开始读取的log_id
    }SendRegionLogResp; // for target server
  };
};

const size_t FSGetinodeResp_size = sizeof(wire_resp_t::FSGetinodeResp);
const size_t FSOpenResp_size = sizeof(wire_resp_t::FSOpenResp);
const size_t FSStatResp_size = sizeof(wire_resp_t::FSStatResp);
const size_t FSMknodResp_size = sizeof(wire_resp_t::FSMknodResp);
const size_t FSMkdirResp_size = sizeof(wire_resp_t::FSMkdirResp);
const size_t FSUnlinkResp_size = sizeof(wire_resp_t::FSUnlinkResp);
const size_t FSRmdirResp_size = sizeof(wire_resp_t::FSRmdirResp);
const size_t FSReaddirResp_size = sizeof(wire_resp_t::FSReaddirResp);

const size_t CreateRegionResp_size = sizeof(wire_resp_t::CreateRegionResp);
const size_t SendRegionResp_size = sizeof(wire_resp_t::SendRegionResp);
const size_t SendRegionLogResp_size = sizeof(wire_resp_t::SendRegionLogResp);
} // end namespace metafs

