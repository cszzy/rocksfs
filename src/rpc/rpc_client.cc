#include "rpc/rpc_client.h"

namespace metafs{

rpc_resp_t RpcClient::RPC_Open(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat) {
    uint64_t pinode_hash = XXH3_64bits(&pinode, metafs_inode_size) % PINODE_HASH_RANGE;
    region_id_t region_id = find_region_id(RegionKey(0, pinode_hash));
    p_assert(region_id != -1, "Invalid region_id");
    int32_t server_session_id = region_id < c_ctx->total_servers ? region_id : 
                                    JumpConsistentHash(region_id, c_ctx->total_servers);  

    int index = 0;

    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                    offsetof(wire_req_t, FSOpenReq.fname) + fname.length() + 1);
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->FSOpenReq.pinode = pinode;
    req_buf->FSOpenReq.mode = mode;
    strcpy(req_buf->FSOpenReq.fname, fname.c_str());
    
    bool complete_cb = false;
    c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                            kFSOpenReq, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                            &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));
    while(complete_cb == false) {
        c_ctx->rpc_->run_event_loop_once();
    }
    
    auto resp_buf = C_RPC_RESP_BUF(index);
    auto res = resp_buf->FSOpenResp.resp_type;
    if(likely(res == kSuccess)) {
        inode = resp_buf->FSOpenResp.inode;
        stat = resp_buf->FSOpenResp.stat;
    }
    return res;
}

rpc_resp_t RpcClient::RPC_Getinode(const metafs_inode_t pinode, const string &fname, metafs_inode_t &inode) {
    uint64_t pinode_hash = XXH3_64bits(&pinode, metafs_inode_size) % PINODE_HASH_RANGE;
    region_id_t region_id = find_region_id(RegionKey(0, pinode_hash));
    p_assert(region_id != -1, "Invalid region_id");
    int32_t server_session_id = region_id < c_ctx->total_servers ? region_id : 
                                    JumpConsistentHash(region_id, c_ctx->total_servers); 

    int index = 0;

    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                    offsetof(wire_req_t, FSGetinodeReq.fname) + fname.length() + 1);
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->FSGetinodeReq.pinode = pinode;
    strcpy(req_buf->FSGetinodeReq.fname, fname.c_str());
    
    bool complete_cb = false;
    c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                            kFSGetinodeReq, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                            &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));
    while(complete_cb == false) {
        c_ctx->rpc_->run_event_loop_once();
    }
    
    auto resp_buf = C_RPC_RESP_BUF(index);
    auto res = resp_buf->FSGetinodeResp.resp_type;
    if(likely(res == RespType::kSuccess)) {
        inode = resp_buf->FSGetinodeResp.inode;
    }
    return res;
}

/**
 * @param in pinode file's parent's inode
 * @param in fname filename
 * @param out stat file's stat
 * 
 * XXX: 注意写dentrycache
 */
rpc_resp_t RpcClient::RPC_Getstat(metafs_inode_t pinode, const string &fname, metafs_inode_t &inode, metafs_stat_t &stat) {
    FS_LOG("Getstat pinode: %d, fname: %s", pinode, fname.c_str());
    
    uint64_t pinode_hash = XXH3_64bits(&pinode, metafs_inode_size) % PINODE_HASH_RANGE;
    region_id_t region_id = find_region_id(RegionKey(0, pinode_hash));
    p_assert(region_id != -1, "Invalid region_id");
    int32_t server_session_id = region_id < c_ctx->total_servers ? region_id : 
                                    JumpConsistentHash(region_id, c_ctx->total_servers); 

    int index = 0;

    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                    offsetof(wire_req_t, FSStatReq.fname) + fname.length() + 1);
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->FSStatReq.pinode = pinode;
    strcpy(req_buf->FSStatReq.fname, fname.c_str());
    
    bool complete_cb = false;
    c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                                     kFSStatReq, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                                     &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                                     set_complete_cb, reinterpret_cast<void*>(&complete_cb));
    while(complete_cb == false) {
        c_ctx->rpc_->run_event_loop_once();
    }
    
    auto resp_buf = C_RPC_RESP_BUF(index);
    auto res = resp_buf->FSStatResp.resp_type;
    if(likely(res == RespType::kSuccess)) {
        inode = resp_buf->FSStatResp.inode;
        stat = resp_buf->FSStatResp.stat;
    }
    return res;
}

rpc_resp_t RpcClient::RPC_Mknod(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat) {
    uint64_t pinode_hash = XXH3_64bits(&pinode, metafs_inode_size) % PINODE_HASH_RANGE;
    region_id_t region_id = find_region_id(RegionKey(0, pinode_hash));
    p_assert(region_id != -1, "Invalid region_id");
    int32_t server_session_id = region_id < c_ctx->total_servers ? region_id : 
                                    JumpConsistentHash(region_id, c_ctx->total_servers); 

    int index = 0;

    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                    offsetof(wire_req_t, FSMknodReq.fname) + fname.length() + 1);
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->FSMknodReq.pinode = pinode;
    req_buf->FSMknodReq.mode = mode;
    strcpy(req_buf->FSMknodReq.fname, fname.c_str());
    
    bool complete_cb = false;
    c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                            kFSMknodReq, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                            &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));
    while(complete_cb == false) {
        c_ctx->rpc_->run_event_loop_once();
    }
    
    auto resp_buf = C_RPC_RESP_BUF(index);
    auto res = resp_buf->FSMkdirResp.resp_type;
    if(likely(res == RespType::kSuccess)) {
        inode = resp_buf->FSMknodResp.inode;
        stat = resp_buf->FSMknodResp.stat;
    }
    return res;
}

rpc_resp_t RpcClient::RPC_Unlink(metafs_inode_t pinode, const string &fname) {
    uint64_t pinode_hash = XXH3_64bits(&pinode, metafs_inode_size) % PINODE_HASH_RANGE;
    region_id_t region_id = find_region_id(RegionKey(0, pinode_hash));
    p_assert(region_id != -1, "Invalid region_id");
    int32_t server_session_id = region_id < c_ctx->total_servers ? region_id : 
                                    JumpConsistentHash(region_id, c_ctx->total_servers); 

    int index = 0;

    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                    offsetof(wire_req_t, FSUnlinkReq.fname) + fname.length() + 1);
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->FSUnlinkReq.pinode = pinode;
    strcpy(req_buf->FSUnlinkReq.fname, fname.c_str());
    
    bool complete_cb = false;
    c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                            kFSUnlinkReq, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                            &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));
    while(complete_cb == false) {
        c_ctx->rpc_->run_event_loop_once();
    }
    
    return C_RPC_RESP_BUF(index)->FSUnlinkResp.resp_type;
}

rpc_resp_t RpcClient::RPC_Mkdir(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode) {
    uint64_t pinode_hash = XXH3_64bits(&pinode, metafs_inode_size) % PINODE_HASH_RANGE;
    region_id_t region_id = find_region_id(RegionKey(0, pinode_hash));
    p_assert(region_id != -1, "Invalid region_id");
    int32_t server_session_id = region_id < c_ctx->total_servers ? region_id : 
                                    JumpConsistentHash(region_id, c_ctx->total_servers); 

    int index = 0;

    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                    offsetof(wire_req_t, FSMkdirReq.fname) + fname.length() + 1);
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->FSMkdirReq.pinode = pinode;
    req_buf->FSMkdirReq.mode = mode;
    strcpy(req_buf->FSMkdirReq.fname, fname.c_str());
    
    bool complete_cb = false;
    c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                            kFSMkdirReq, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                            &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));
    while(complete_cb == false) {
        c_ctx->rpc_->run_event_loop_once();
    }
    
    auto resp_buf = C_RPC_RESP_BUF(index);
    auto res = resp_buf->FSMkdirResp.resp_type;
    if(likely(res == RespType::kSuccess)) {
        inode = resp_buf->FSMkdirResp.inode;
    }
    return res;
}

rpc_resp_t RpcClient::RPC_Rmdir(metafs_inode_t pinode, const string &fname) {
    uint64_t pinode_hash = XXH3_64bits(&pinode, metafs_inode_size) % PINODE_HASH_RANGE;
    region_id_t region_id = find_region_id(RegionKey(0, pinode_hash));
    p_assert(region_id != -1, "Invalid region_id");
    int32_t server_session_id = region_id < c_ctx->total_servers ? region_id : 
                                    JumpConsistentHash(region_id, c_ctx->total_servers); 

    int index = 0;

    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                    offsetof(wire_req_t, FSRmdirReq.fname) + fname.length() + 1);
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->FSRmdirReq.pinode = pinode;
    strcpy(req_buf->FSRmdirReq.fname, fname.c_str());
    
    bool complete_cb = false;
    c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                            kFSRmdirReq, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                            &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));
    while(complete_cb == false) {
        c_ctx->rpc_->run_event_loop_once();
    }
    
    return C_RPC_RESP_BUF(index)->FSRmdirResp.resp_type;
}

// TODO: 对于大目录的情况需要进行优化
// 目前不涉及分区，目录下的所有元数据文件聚集在同一个服务器中
rpc_resp_t RpcClient::RPC_Readdir(metafs_inode_t pinode, shared_ptr<OpenDir> &open_dir) {
    FS_LOG("RPC Readdir, pinode: %d", pinode);
    
    uint64_t pinode_hash = XXH3_64bits(&pinode, metafs_inode_size) % PINODE_HASH_RANGE;
    region_id_t region_id = find_region_id(RegionKey(0, pinode_hash));
    p_assert(region_id != -1, "Invalid region_id");
    int32_t server_session_id = region_id < c_ctx->total_servers ? region_id : 
                                    JumpConsistentHash(region_id, c_ctx->total_servers); 

    int index = 0;

    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_, FSReaddirReq_size);
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->FSReaddirReq.inode = pinode;
    req_buf->FSReaddirReq.offset = 0;
    
    uint32_t is_uncomplete;
    do{ 
        FS_LOG("readdir-----------\n");
        bool complete_cb = false;
        c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                                kFSReaddirReq, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                                &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                                set_complete_cb, reinterpret_cast<void*>(&complete_cb));
        while(complete_cb == false) {
            c_ctx->rpc_->run_event_loop_once();
        }
        
        auto resp_buf = C_RPC_RESP_BUF(index);
        auto res = resp_buf->FSReaddirResp.resp_type;
        if(unlikely(res != RespType::kSuccess)) {
            return res;
        }

        // write into buf
        int result_offset = 0;

        int num_result = resp_buf->FSReaddirResp.num_result;
        FS_LOG("result: %d\n", num_result);
        const char *fname_ptr = (const char *)&(resp_buf->FSReaddirResp.entries);
        int fname_len = -8;
        metafs_inode_t inode;

        while(num_result--) {
            fname_ptr += fname_len + 16;
            fname_len = strlen(fname_ptr) + 1; // fname end with '\0'
            inode = *(metafs_inode_t*)(fname_ptr + fname_len);
            FileType ftype = ( inode & inode_prefix_msb) ? FileType::directory : FileType::regular;
            FS_LOG("fname: %s\n", fname_ptr);
            open_dir->add(string(fname_ptr), ftype, inode);
        }

        is_uncomplete = resp_buf->FSReaddirResp.is_uncomplete;
        if(is_uncomplete) {
            // update next Readdir RPC's Req's offset elem
            c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_, FSReaddirReq_size);
            req_buf = C_RPC_REQ_BUF(index);
            req_buf->FSReaddirReq.inode = pinode;
            req_buf->FSReaddirReq.offset = resp_buf->FSReaddirResp.next_offset;
        }
    } while(is_uncomplete);

    return RespType::kSuccess;
}

rpc_resp_t RpcClient::RPC_ReadRegionmap() {
    // FS_LOG("read region map");
    int32_t server_session_id = rand() % c_ctx->total_servers;
    int index = 0;
    c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_, sizeof(wire_req_t::ReadRegionmapReq));
    auto req_buf = C_RPC_REQ_BUF(index);
    req_buf->ReadRegionmapReq.client_id = c_ctx->id;
    
    bool complete_cb = false;
    c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
                            kReadRegionmap, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
                            &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));

    while(complete_cb == false) {
        c_ctx->rpc_->run_event_loop_once();
    }
    
    if(likely(C_RPC_RESP_BUF(index)->ReadRegionmapResp.resp_type == RespType::kSuccess)) {
        auto resp = &(C_RPC_RESP_BUF(index)->ReadRegionmapResp);
        int num_result = resp->num_entries;
        // apply resp to region map
        c_ctx->region_map.clear();
        for(int i = 0; i < num_result; i++) {
            auto region = resp->entries[i];
            p_info("region#%d: s_hi:%d, s_low:%d, e_hi:%d, e_low:%d", region.region_id, region.start_key.hi, region.start_key.low, region.end_key.hi, region.end_key.low);
            c_ctx->region_map.insert(make_pair(resp->entries[i], resp->entries[i].region_id));
        }
        return RespType::kSuccess;
    }

    return RespType::kFail;
}

// TODO: for rename
// rpc_resp_t RpcClient::RPC_Remove(metafs_inode_t pinode, const string &fname, mode_t mode) {
//     int32_t server_session_id = JumpConsistentHash(pinode, c_ctx->total_servers);
//     int index = 0;

//     wire_req_t req;
//     req.mode_req.pinode = pinode;
//     req.mode_req.mode = mode;
//     const char *key = fname.c_str();
//     MurmurHash3_x64_128((void *) key, (int)strlen(key), 0, (void *) req.mode_req.hash_fname);
//     c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
//                     sizeof(req.mode_req));
//     memcpy(reinterpret_cast<char *>(C_RPC_CONTEXT_WINDOW(index).req_msgbuf_.buf_),
//              reinterpret_cast<char *>(&req), sizeof(req.mode_req));
    
//     bool complete_cb = false;
//     c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
//                                      kRemove, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
//                                      &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
//                                      set_complete_cb, reinterpret_cast<void*>(&complete_cb));
//     while(complete_cb == false) {
//         c_ctx->rpc_->run_event_loop_once();
//     }
    
//     auto res_buf = C_RPC_RESP_BUF(index);
//     return res_buf->resp_type;
// }

// TODO : for rename
// // create an entry in parent's dir 
// rpc_resp_t RpcClient::RPC_Create(metafs_inode_t pinode, const string &fname, const metafs_stat_t &stat) {
//     int32_t server_session_id = JumpConsistentHash(pinode, c_ctx->total_servers);
//     int index = 0;

//     wire_req_t req;
//     req.has_stat_req.pinode = pinode;
//     req.has_stat_req.stat = stat;
//     strcpy(req.has_stat_req.fname, fname.c_str());
//     c_ctx->rpc_->resize_msg_buffer(&C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
//                     sizeof(req.has_stat_req));
//     memcpy(reinterpret_cast<char *>(C_RPC_CONTEXT_WINDOW(index).req_msgbuf_.buf_),
//              reinterpret_cast<char *>(&req), sizeof(req.has_stat_req));
    
//     bool complete_cb = false;
//     c_ctx->rpc_->enqueue_request(c_ctx->session_num_vec_[server_session_id],
//                                      kCreateEntry, &C_RPC_CONTEXT_WINDOW(index).req_msgbuf_,
//                                      &C_RPC_CONTEXT_WINDOW(index).resp_msgbuf_, 
//                                      set_complete_cb, reinterpret_cast<void*>(&complete_cb));
//     while(complete_cb == false) {
//         c_ctx->rpc_->run_event_loop_once();
//     }
    
//     auto res_buf = C_RPC_RESP_BUF(index);
//     return res_buf->resp_type;
// }

} // end namespace metafs
    