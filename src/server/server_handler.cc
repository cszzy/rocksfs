#include "server/metafs_server.h"

namespace metafs {

rpc_resp_t convert_status_to_resptype(MetaKvStatus status) {
  switch (status) {
    case MetaKvStatus::OK:
    case MetaKvStatus::INSERT_OK:
    case MetaKvStatus::UPDATE_OK:
        return RespType::kSuccess;
    case MetaKvStatus::KEY_ALREADY_EXIST:
        return RespType::kEXIST;
    case MetaKvStatus::VALUE_NOT_EQUAL:
    case MetaKvStatus::NEWER_UPDATE:
    case MetaKvStatus::KEY_NOT_EXIST:
    case MetaKvStatus::NO_ENOUGH_SPACE:
    case MetaKvStatus::UN_COMPLETED:
    case MetaKvStatus::UNKNOWN_ERR:
        return RespType::kENOENT;
    default:
        break;
  }

  p_assert(false, "not defined metadb MetaKvStatus");
  return RespType::kFail;
}

void fs_open_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);
    MetaDb *mdb = ctx->metadb;

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->FSOpenReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->FSOpenResp; 
    
    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    if(!check_is_blong_to_region(region, c_req->pinode_hash)) {
        c_resp->resp_type = RespType::kUpdateRegionMap;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSOpenResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }

    if(check_region_status(region)) {
        metafs_inode_t inode;
        MetaKvSlice stat_slice;
        SliceInit(&stat_slice, metafs_stat_size, (char*)&(c_resp->stat));
        MetaKvSlice fname_slice;
        SliceInit(&fname_slice, strlen(c_req->fname) + 1, (char*)&(c_req->fname));
        MetaKvStatus status = GetFileInode(mdb, c_req->pinode, &fname_slice, &inode);
        if(likely(check_status_ok(status))) {
            status = GetStat(mdb ,inode, &stat_slice);
            if(likely(check_status_ok(status))) {
                if ((c_req->mode&0b11) == O_WRONLY || (c_req->mode&0b11) == O_RDWR) {
                    struct timeval tv; 
                    gettimeofday(&tv, NULL);
                    ((metafs_stat_t*)(stat_slice.data))->mtime = tv.tv_sec;
                    ((metafs_stat_t*)(stat_slice.data))->atime = tv.tv_sec;
                    status = UpdateStat(mdb, inode, &stat_slice);
                }
            } 
            // p_info("status: %d oid: %ld\n", status, c_resp->stat.oid.lo);
        } else {
            // p_info("open error: pinode:%ld, fname:%s len:%ld\n",c_req->pinode, c_req->fname, strlen(c_req->fname));
        }

        c_resp->inode = inode;
        c_resp->resp_type = convert_status_to_resptype(status);
    } else {
        c_resp->resp_type = region->region_status == RegionStatus::SplitAlmostDone ? 
                            RespType::kUpdateRegionMap : RespType::kEBUSY;
    }
    ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSOpenResp_size);
    ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void fs_mknod_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);
    MetaDb *mdb = ctx->metadb;

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->FSMknodReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->FSMknodResp; 
    
    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    if(!check_is_blong_to_region(region, c_req->pinode_hash)) {
        c_resp->resp_type = RespType::kUpdateRegionMap;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSMknodResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }

    if(check_region_status(region)) {
        // create new file
        metafs_inode_t inode = s_ctx->alloc_inode++;
        MetaKvSlice stat_slice;
        metafs_stat_t stat(c_req->oid, c_req->mode);
        SliceInit(&stat_slice, metafs_stat_size, (char*)&stat);

        MetaKvSlice fname_slice;
        SliceInit(&fname_slice, strlen(c_req->fname) + 1, (char*)&(c_req->fname));
        MetaKvStatus status = InsertFileInode(mdb, c_req->pinode, &fname_slice, inode);
        if (likely(check_status_ok(status))) {
            status = InsertStat(mdb, inode, &stat_slice);
            c_resp->inode = inode;
            region->kv_num++;
        } else {
            // p_info("mknod error\n");
        }

        RegionStatus s1 = RegionStatus::Normal;
        RegionStatus s2 = RegionStatus::IsSplit;
        if (region->kv_num > region_split_threshold 
            && region->region_status.compare_exchange_strong(s1, s2)) {
            check_region_and_split(region);
        }

        // if (region->kv_num > region_split_threshold 
        //     && region->region_status == RegionStatus::Normal) {
        //     region->region_status == RegionStatus::IsSplit;
        //     check_region_and_split(region);
        // }

        if(region->region_status == RegionStatus::IsSplit || 
            region->region_status == RegionStatus::SplitAlmostDone) {
            log_op(region, false, c_req->pinode, c_req->fname, &stat, inode);
        }

        c_resp->resp_type = convert_status_to_resptype(status);
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSMknodResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    } else {
        c_resp->resp_type = region->region_status == RegionStatus::SplitAlmostDone ? 
                            RespType::kUpdateRegionMap : RespType::kEBUSY;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSMknodResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }
}

void fs_stat_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);
    MetaDb *mdb = ctx->metadb;

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->FSStatReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->FSStatResp; 
    
    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    if(!check_is_blong_to_region(region, c_req->pinode_hash)) {
        c_resp->resp_type = RespType::kUpdateRegionMap;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSStatResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }

    if(check_region_status(region)) {
        metafs_inode_t inode;
        MetaKvSlice stat_slice;
        SliceInit(&stat_slice, metafs_stat_size, (char*)&(c_resp->stat));

        MetaKvSlice fname_slice;
        SliceInit(&fname_slice, strlen(c_req->fname) + 1, (char*)&(c_req->fname));
        MetaKvStatus status = GetFileInode(mdb, c_req->pinode, &fname_slice, &inode);
        if (likely(check_status_ok(status))) {
            status = GetStat(mdb, inode, &stat_slice);
            c_resp->inode = inode;
        } 
        c_resp->resp_type = convert_status_to_resptype(status);
    } else {
        c_resp->resp_type = region->region_status == RegionStatus::SplitAlmostDone ? 
                            RespType::kUpdateRegionMap : RespType::kEBUSY; 
    }  
    ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSStatResp_size);
    ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void fs_unlink_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);
    MetaDb *mdb = ctx->metadb;

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->FSUnlinkReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->FSUnlinkResp; 
    
    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    if(!check_is_blong_to_region(region, c_req->pinode_hash)) {
        c_resp->resp_type = RespType::kUpdateRegionMap;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSUnlinkResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }

    if(check_region_status(region)) {
        metafs_inode_t inode;
        MetaKvSlice fname_slice;
        SliceInit(&fname_slice, strlen(c_req->fname) + 1, (char*)&(c_req->fname));

        MetaKvStatus status = DeleteFileInode(mdb, c_req->pinode, &fname_slice, &inode);
        if (likely(check_status_ok(status))) {
            status = DeleteStat(mdb, inode);
            region->kv_num--;
        } else {
            // p_info("unlink error\n");
        }

        if(region->region_status == RegionStatus::IsSplit || 
            region->region_status == RegionStatus::SplitAlmostDone) {
                log_op(region, true, c_req->pinode, c_req->fname);
        }

        c_resp->resp_type = convert_status_to_resptype(status);
    } else {
        c_resp->resp_type = region->region_status == RegionStatus::SplitAlmostDone ? 
                            RespType::kUpdateRegionMap : RespType::kEBUSY; 
    }
    ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSUnlinkResp_size);
    ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

// mkdir时需要将目录号插入pinode_table, 方便之后region_split, TODO: 根目录需要处理？
void fs_mkdir_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);
    MetaDb *mdb = ctx->metadb;

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->FSMkdirReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->FSMkdirResp; 
    
    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    if(!check_is_blong_to_region(region, c_req->pinode_hash)) {
        c_resp->resp_type = RespType::kUpdateRegionMap;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSMkdirResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }

    if(check_region_status(region)) {
        metafs_inode_t inode = s_ctx->alloc_inode | inode_prefix_msb;
        // p_info("mkdir, inode: %llx", inode);
        s_ctx->alloc_inode++;

        MetaKvSlice stat_slice;
        metafs_stat_t stat(c_req->mode);
        SliceInit(&stat_slice, metafs_stat_size, (char*)&stat);

        MetaKvSlice fname_slice;
        SliceInit(&fname_slice, strlen(c_req->fname) + 1, (char*)&(c_req->fname));
        MetaKvStatus status = InsertFileInode(mdb, c_req->pinode, &fname_slice, inode);
        if (likely(check_status_ok(status))) {
            status = InsertStat(mdb, inode, &stat_slice);
            region->kv_num++;
            
            // char *tmp;
            // MetaKvStatus status = ReadDir(mdb, inode, &tmp, 0, MSG_ENTEY_MAX_SIZE);
            // p_assert(status == OK, "not exist dir");

            if (region->region_status == RegionStatus::Normal
                 || region->region_status == RegionStatus::ToBeSplit) {
                WriteGuard wl(s_ctx->pinode_table_lock);
                s_ctx->pinode_table[XXH3_64bits(&inode, metafs_inode_size) % PINODE_HASH_RANGE].insert(inode);
            }
            
            c_resp->inode = inode;

            // {
            //     metafs_inode_t dkfsl;
            //     GetFileInode(mdb, c_req->pinode, &fname_slice, &dkfsl);
            //     p_info("mkdir, inode: %llx", dkfsl);

            //     char *res;
            //     struct LogScanHeader *header = (struct LogScanHeader *) res;
            //     MetaKvStatus status = ReadDir(s_ctx->metadb, c_req->pinode, &res, 0, MSG_ENTEY_MAX_SIZE);
            //         int32_t num_result = header->rel_count;
            //         char *buf = res + 4 * sizeof(uint64_t);
            //         int qwert = 0;
            //         while(num_result--) {
            //             metafs_inode_t ppinode = (metafs_inode_t)*buf;
            //             buf += metafs_inode_size;

            //             char *ffname = (char*)buf;
            //             size_t fname_len = strlen(ffname) + 1;
            //             buf += fname_len;

            //             metafs_inode_t iinode = (metafs_inode_t)*buf;
            //             buf += metafs_inode_size;
            //             p_assert(false, "entry: %x, %x, %x, %x->ppinode: %lx, pinode: %lx, fname: %s, iinode:%lx, inode:%lx, dkfsl:%lx",
            //                              buf[0], buf[1], buf[2], buf[3], ppinode, c_req->pinode, ffname, iinode, inode, dkfsl);
            //         }
            // }
        }

        RegionStatus s1 = RegionStatus::Normal;
        RegionStatus s2 = RegionStatus::IsSplit;
        if (region->kv_num > region_split_threshold 
            && region->region_status.compare_exchange_strong(s1, s2)) {
            check_region_and_split(region);
        }

        // if (region->kv_num > region_split_threshold 
        //     && region->region_status == RegionStatus::Normal) {
        //     region->region_status == RegionStatus::IsSplit;
        //     check_region_and_split(region);
        // }

        if(region->region_status == RegionStatus::IsSplit || 
            region->region_status == RegionStatus::SplitAlmostDone) {
            log_op(region, false, c_req->pinode, c_req->fname, &stat, inode);
        } 

        c_resp->resp_type = convert_status_to_resptype(status);
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSMkdirResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_); 
    } else {
        c_resp->resp_type = region->region_status == RegionStatus::SplitAlmostDone ? 
                            RespType::kUpdateRegionMap : RespType::kEBUSY; 
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSMkdirResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }
    
}

void fs_readdir_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);
    MetaDb *mdb = ctx->metadb;

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->FSReaddirReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->FSReaddirResp; 
    
    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    if(!check_is_blong_to_region(region, c_req->inode_hash)) {
        c_resp->resp_type = RespType::kUpdateRegionMap;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSReaddirResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }

    if(check_region_status(region)) {
        char* res = NULL;
        metafs_inode_t inode;

        // metakv readdir 返回的buf的格式: 0: next_offset, 1: is_uncomplete, 2: num_result, 3: entries_len
        MetaKvStatus status = ReadDir(mdb, c_req->inode, &res, c_req->offset, MSG_ENTEY_MAX_SIZE);
        if (likely(check_status_ok(status))) {
            int64_t* entry_mdata = (int64_t*)res;
            c_resp->next_offset = entry_mdata[0];
            c_resp->is_uncomplete = entry_mdata[1];
            c_resp->num_result = entry_mdata[2];
            c_resp->entries_len = entry_mdata[3] - 4 * sizeof(int64_t);
            // // p_info("rel_count:%ld\n", resp->num_result);
            assert(entry_mdata[2] <= MSG_ENTEY_MAX_SIZE);
            memcpy(&c_resp->entries, res + 4 * sizeof(int64_t), c_resp->entries_len);
            free(res);
        } else {
            // p_info("readdir error, dir_inode:%ld, offset:%ld, %p\n",c_req->inode&kPrefixMask, c_req->offset, res);
        }

        if (res == NULL) {
            c_resp->num_result = 0;
            status = OK;
            // p_info("read empty directory\n");
        }
        
        c_resp->resp_type = convert_status_to_resptype(status);
    } else {
        c_resp->resp_type = region->region_status == RegionStatus::SplitAlmostDone ? 
                            RespType::kUpdateRegionMap : RespType::kEBUSY; 
    }
    
    ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSReaddirResp_size);
    ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

// 删除目录时，暂时不清除pinode_table对应的pinode
void fs_rmdir_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);
    MetaDb *mdb = ctx->metadb;

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->FSRmdirReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->FSRmdirResp; 
    
    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    if(!check_is_blong_to_region(region, c_req->pinode_hash)) {
        c_resp->resp_type = RespType::kUpdateRegionMap;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSRmdirResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }

    if(check_region_status(region)) {
        metafs_inode_t inode;
        MetaKvSlice fname_slice;
        SliceInit(&fname_slice, strlen(c_req->fname) + 1, (char*)&(c_req->fname));
        // TODO: check directory has entry
        MetaKvStatus status = GetFileInode(mdb, c_req->pinode, &fname_slice, &inode);

        if (likely(is_directory(inode))) {
            if (likely(check_status_ok(status))) {
                char* res = NULL;
                bool has_files;
                // p_info("rmdir, inode: %llx", inode);
                // metakv readdir 返回的buf的格式: 0: next_offset, 1: is_uncomplete, 2: num_result, 3: entries_len
                status = ReadDir(mdb, inode, &res, 0, 256);
                if (res == NULL) {
                    status = OK;
                    has_files = false;
                } else {
                    has_files = (((uint64_t*)res)[2] == 0);
                    free(res);
                }

                if (likely(check_status_ok(status)) && !has_files) {
                    status = DeleteFileInode(mdb, c_req->pinode, &fname_slice, &inode);
                    if (likely(check_status_ok(status))) {
                        status = DeleteStat(mdb, inode);
                    }
                    region->kv_num--;

                    if(region->region_status == RegionStatus::IsSplit || 
                        region->region_status == RegionStatus::SplitAlmostDone) {
                        log_op(region, true, c_req->pinode, c_req->fname);
                    }
                } else {
                    // p_info("can not rmdir, has file in this directoty %ld, %s\n", c_req->pinode&kPrefixMask, c_req->fname);
                }
            } else {
                // p_info("rmdir error\n");
            }
        } else {
            status = KEY_NOT_EXIST;
        }

        c_resp->resp_type = convert_status_to_resptype(status);
    } else {
        c_resp->resp_type = region->region_status == RegionStatus::SplitAlmostDone ? 
                            RespType::kUpdateRegionMap : RespType::kEBUSY; 
    }
    
    ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSRmdirResp_size);
    ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void fs_getinode_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);
    MetaDb *mdb = ctx->metadb;

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->FSGetinodeReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->FSGetinodeResp; 
    
    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    if(!check_is_blong_to_region(region, c_req->pinode_hash)) {
        c_resp->resp_type = RespType::kUpdateRegionMap;
        ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSGetinodeResp_size);
        ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    }

    if(check_region_status(region)) {
        metafs_inode_t inode;
        MetaKvSlice fname_slice;
        SliceInit(&fname_slice, strlen(c_req->fname) + 1, (char*)&(c_req->fname));

        MetaKvStatus status = GetFileInode(mdb, c_req->pinode, &fname_slice, &inode);
        if (likely(check_status_ok(status))) {
            c_resp->inode = inode;
        } else {
            // p_info("getinode error\n");
        }

        c_resp->resp_type = convert_status_to_resptype(status);
    } else {
        c_resp->resp_type = region->region_status == RegionStatus::SplitAlmostDone ? 
                            RespType::kUpdateRegionMap : RespType::kEBUSY; 
    }
    ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, FSGetinodeResp_size);
    ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void read_region_map_handler(erpc::ReqHandle *req_handle, void *_context) {
    server_context *ctx = static_cast<server_context *>(_context);

    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->ReadRegionmapReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->ReadRegionmapResp; 

    p_info("client#%d read region map", c_req->client_id);

    ReadGuard region_rl(global_region_map_rwlock);
    int i = 0;
    for(auto iter = global_region_map.begin(); iter != global_region_map.end(); iter++) {
        c_resp->entries[i++] = iter->second;
    }

    c_resp->resp_type = RespType::kSuccess;
    c_resp->num_entries = i;
    ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, offsetof(wire_resp_t, ReadRegionmapResp.entries) + i * sizeof(ClientRegion));
    ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void s2s_create_region_handler(erpc::ReqHandle *req_handle, void *_context) {
    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->CreateRegionReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->CreateRegionResp; 

    ServerRegion *region = new ServerRegion(c_req->region_id, c_req->start_key, c_req->end_key);
    region->region_status = RegionStatus::NotReady;

    {
        WriteGuard wl(s_ctx->region_map_lock);
        s_ctx->region_map.insert(make_pair(c_req->region_id, region));
    }    

    c_resp->region_id = c_req->region_id;
    c_resp->resp_type = RespType::kSuccess;

    s_ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, CreateRegionResp_size);
    s_ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void s2s_send_region_handler(erpc::ReqHandle *req_handle, void *_context) {
    p_info("build region---------");
    const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->SendRegionReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->SendRegionResp; 

    int32_t num_result = c_req->num_result;
    uint8_t *buf = c_req->entries;
    MetaDb *mdb = s_ctx->metadb;
    int qwert = 0;
    while(num_result--) {
        metafs_inode_t pinode = (metafs_inode_t)*buf;
        buf += metafs_inode_size;

        char *fname = (char*)buf;
        size_t fname_len = strlen(fname) + 1;
        MetaKvSlice fname_slice;
        SliceInit(&fname_slice, fname_len, fname);
        buf += fname_len;

        metafs_inode_t inode = (metafs_inode_t)*buf;
        buf += metafs_inode_size;

        p_info("#%d build region: pinode:%llx, fname:%s, inode:%llx", qwert++, pinode, fname, inode);
        
        MetaKvSlice stat_slice;
        metafs_stat_t stat;
        SliceInit(&stat_slice, metafs_stat_size, (char*)&stat);
        // put into kv
        MetaKvStatus status = InsertFileInode(mdb, pinode, &fname_slice, inode);
        if (likely(check_status_ok(status))) {
            status = InsertStat(mdb, inode, &stat_slice);
        }
    }

    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }
    
    region->kv_num += c_req->num_result; 

    c_resp->resp_type = RespType::kSuccess;
    c_resp->is_uncomplete = c_req->is_uncomplete;
    c_resp->region_id = c_req->region_id;
    c_resp->pinode_hash = c_req->pinode_hash;
    c_resp->pinode = c_req->pinode;
    c_resp->offset = c_req->offset;
    p_info("build region---------");
    s_ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, SendRegionReq_size);
    s_ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

// XXX: 在replay日志时, 对于delete操作会将kv_num--,对于put操作会将kv_num++,会造成计数不准确
void s2s_send_region_log_handler(erpc::ReqHandle *req_handle, void *_context) {
const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
    auto c_req = &reinterpret_cast<wire_req_t *>(req_msgbuf->buf_)->SendRegionLogReq;
    auto c_resp = &reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_)->SendRegionLogResp; 

    int32_t num_logs = c_req->num_logs;
    uint8_t *buf = c_req->entries;
    MetaDb *mdb = s_ctx->metadb;
    ServerRegion *region;

    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[c_req->region_id];
    }

    // entry中每条log格式:
      // put_log格式: pinode(PUT(1)/DELETE(0)嵌入inode次高位) + inode + metafs_stat + fname(end with '\0)
      // delete log格式: pinode(PUT(1)/DELETE(0)嵌入inode次高位) + fname(end with '\0)

    while(num_logs--) {
        metafs_inode_t pinode = (metafs_inode_t)*buf;
        bool is_delete_op = (pinode & inode_prefix_ssb) == 0;
        pinode &= ~inode_prefix_ssb;
        buf += metafs_inode_size;

        if(is_delete_op) {
            char *fname = (char*)buf;
            size_t fname_len = strlen(fname) + 1;
            MetaKvSlice fname_slice;
            SliceInit(&fname_slice, fname_len, fname);
            buf += fname_len;

            metafs_inode_t inode;
            MetaKvStatus status = DeleteFileInode(mdb, pinode, &fname_slice, &inode);
            if (likely(check_status_ok(status))) {
                status = DeleteStat(mdb, inode);
            }
            // region中kv数减1
            region->kv_num--;
        } else { // is put op
            metafs_inode_t inode = (metafs_inode_t)*buf;
            buf += metafs_inode_size;

            MetaKvSlice stat_slice;
            SliceInit(&stat_slice, metafs_stat_size, (char*)buf);
            buf += metafs_stat_size;

            char *fname = (char*)buf;
            size_t fname_len = strlen(fname) + 1;
            MetaKvSlice fname_slice;
            SliceInit(&fname_slice, fname_len, fname);
            buf += fname_len;

            // put into kv
            MetaKvStatus status = InsertFileInode(mdb, pinode, &fname_slice, inode);
            if (likely(check_status_ok(status))) {
                status = InsertStat(mdb, inode, &stat_slice);
            }
            region->kv_num++;
        }
    }

    if(c_req->log_status == LogStatus::Done) {
        region->region_status = RegionStatus::Normal;
    }

    c_resp->resp_type = RespType::kSuccess;
    c_resp->log_status = c_req->log_status;
    c_resp->region_id = c_req->region_id;
    c_resp->next_log_id = c_req->next_log_id;
    
    s_ctx->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, SendRegionLogResp_size);
    s_ctx->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

}