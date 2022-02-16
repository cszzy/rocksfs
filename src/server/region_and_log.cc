#include "server/region_and_log.h"
#include "server/metafs_server.h"
#include "util/jump_hash.h"
#include "xxHash/xxhash.h"

namespace metafs {

// 初始时根据id注册region
void register_region() {
    // 初始region的region_id不使用全局id，使用线程自身id
    global_region_id++;
    region_id_t region_id = s_ctx->global_id;
    p_info("thread#%lu, region_id:%d", s_ctx->thread_id, region_id);

    // 左闭右闭区间
    RegionKey skey(0, region_id * PINODE_HASH_RANGE);
    RegionKey ekey(0, (region_id + 1) * PINODE_HASH_RANGE - 1);
    
    ServerRegion *region = new ServerRegion(region_id, skey, ekey);
    region->region_status = RegionStatus::Normal;
    
    {
        WriteGuard wl(s_ctx->region_map_lock);
        s_ctx->region_map.insert(make_pair(region_id, region));
    }

    {
        WriteGuard wl(global_region_map_rwlock);
        global_region_map.insert(make_pair(region_id, ClientRegion(region_id, skey, ekey)));
    }
}

bool check_region_status(ServerRegion *region) {
    switch(region->region_status) {
        case RegionStatus::Normal:
            return true; 
        case RegionStatus::NotReady:
        case RegionStatus::SplitAlmostDone:
            return false;
        case RegionStatus::IsSplit:
            return true;
        default:
            p_assert(false, "not defined region status");
    }
    return true;
}

void check_region_and_split(ServerRegion *region) {
    p_info("******************");
    if (region->kv_num > region_split_threshold) {
        RegionKey nr_skey(0, (region->end_key.hi >> 1) + 1);
        RegionKey nr_ekey(0, region->end_key.hi);
        region_id_t nr_region_id = global_region_id++;
        
        // 如果迁移的目标服务器是自己，则重新从全局获取region_id
        while(JumpConsistentHash(nr_region_id, st_ctx->num_server_sessions) == s_ctx->global_id) {
            nr_region_id = global_region_id++;
        }   

        p_info("region#%d split, next region is #%d: s_hi:%ld, s_low:%ld, e_hi:%ld, e_low:%ld", 
            region->region_id, nr_region_id, nr_skey.hi, nr_skey.low, nr_ekey.hi, nr_ekey.low);

        ServerRegion *nr = new ServerRegion(nr_region_id, nr_skey, nr_ekey);
        nr->left_region_id = region->region_id;

        {
            WriteGuard wl(s_ctx->region_map_lock);
            s_ctx->region_map.insert(make_pair(nr_region_id, nr));
        }
        
        shared_split_queue.push(make_pair(s_ctx->thread_id, nr_region_id));
    }
}

void log_op(ServerRegion *region, bool is_delete_op,
        metafs_inode_t pinode, const char *fname,
        const metafs_stat_t *stat, metafs_inode_t inode) {
     // log op
    log_key key(region->region_id, region->log_id++);
    rocksdb::Status s;
    if(is_delete_op == true) {
        delete_log_val log_val(pinode, fname);
        s = s_ctx->log_db->Put(rocksdb::WriteOptions(), key.ToSlice(), log_val.ToSlice());
    } else {
        p_assert(stat != nullptr, "stat is null");
        put_log_val log_val(pinode | inode_prefix_ssb, inode, fname, stat);
        s = s_ctx->log_db->Put(rocksdb::WriteOptions(), key.ToSlice(), log_val.ToSlice());
    }
    p_assert(s.ok(), "write log fail");
}

// establish rpc between servers

// origin server methods
void rpc_create_region(region_id_t region_id) {
    int32_t server_session_id = region_id < st_ctx->num_server_sessions ? region_id : 
                                        JumpConsistentHash(region_id, st_ctx->num_server_sessions);                                

    uint64_t msg_idx = 0;

    st_ctx->rpc->resize_msg_buffer(&ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_,
                    CreateRegionReq_size);
    auto req_buf = ST_RPC_REQ_BUF(msg_idx);

    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        region = s_ctx->region_map[region_id];
    }
    
    req_buf->CreateRegionReq.region_id = region_id;
    req_buf->CreateRegionReq.start_key = region->start_key;
    req_buf->CreateRegionReq.end_key = region->end_key;

    p_info("rpc_create_region: region#%d, s_hi:%ld, s_lo:%ld, e_hi:%ld, e_lo:%ld", 
        region_id, region->start_key.hi, region->start_key.low, region->end_key.hi, region->end_key.low);
    
    // 先用同步方式实现
    bool complete_cb = false;
    st_ctx->rpc->enqueue_request(st_ctx->s2s_session_num_vec[server_session_id],
                            kCreateRegionReq, &ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_,
                            &ST_CTX_RPC_WINDOW(msg_idx).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));
    while(complete_cb == false) {
        st_ctx->rpc->run_event_loop_once();
    }

    p_info("888888888888");

    rpc_create_region_cb(nullptr, (void*)(msg_idx));

    p_info("888888888888");
}

const int32_t region_log_max_size = max(sizeof(delete_log_val), sizeof(put_log_val));
const int32_t log_almost_done_threshold = 30; // log almost done的阈值

void rpc_send_region_log(ServerRegion *region, int32_t next_log_id, uint64_t msg_idx) {
    int32_t server_session_id = region->region_id < st_ctx->num_server_sessions ? region->region_id : 
                                        JumpConsistentHash(region->region_id, st_ctx->num_server_sessions);

    p_info("rpc_send_region_log, msg_idx: %ld", msg_idx);

    while(true) {
        st_ctx->rpc->resize_msg_buffer(&ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_, SendRegionLogReq_size);
        auto s_req = &ST_RPC_REQ_BUF(msg_idx)->SendRegionLogReq;

        int32_t entries_len = 0;
        int32_t nums_logs = 0;
        char *entry_ptr = (char*)s_req->entries;

        ServerRegion *left_region;
        {
            ReadGuard rl(s_ctx->region_map_lock);
            auto region_iter = s_ctx->region_map.find(region->left_region_id);
            p_assert(region_iter != s_ctx->region_map.end(), "region_iter should not be null");
            left_region = region_iter->second;
        }

        rocksdb::Iterator *log_iter = s_ctx->log_db->NewIterator(rocksdb::ReadOptions());
        log_key start_key(left_region->region_id, next_log_id);
        for(log_iter->Seek(start_key.ToSlice());
            log_iter->Valid() && entries_len < (MSG_ENTEY_MAX_SIZE - region_log_max_size);
            log_iter->Next()) {
            const char *log_val = log_iter->value().data();
            size_t log_len = log_iter->value().size();
            ::memcpy(entry_ptr, log_val, log_len);
            entries_len += log_len;
            entry_ptr += log_len;
            nums_logs++;
        }

        int32_t log_left_nums = left_region->log_id - (next_log_id + nums_logs);
        int32_t log_status = log_left_nums <= 0 ? LogStatus::Done : log_left_nums < log_almost_done_threshold ? LogStatus::AlmostDone : LogStatus::FarToDone;
        if(log_status == LogStatus::AlmostDone || log_status == LogStatus::Done) {
            if(left_region->region_status != RegionStatus::SplitAlmostDone) { 
                // 先删除原始的region的范围，修改本地region范围, 然后添加两个子region
                WriteGuard regionmap_wl(global_region_map_rwlock);
                auto global_regionmap_iter = global_region_map.find(left_region->region_id);

                if(global_regionmap_iter != global_region_map.end()) {
                    global_region_map.erase(global_regionmap_iter);
                } else {
                    p_assert(false, "not exist in global region map");
                }

                left_region->end_key.hi >>= 1;
                p_assert(left_region->end_key.hi > 0, "region split fail, need to adjust init range");

                ClientRegion r1(region->region_id, RegionKey(region->start_key), RegionKey(region->end_key));
                ClientRegion r2(left_region->region_id, RegionKey(left_region->start_key), RegionKey(left_region->end_key));

                global_region_map.insert(make_pair(r1.region_id, r1));
                global_region_map.insert(make_pair(r2.region_id, r2));
                
                left_region->region_status = RegionStatus::SplitAlmostDone;
            }
        }

        s_req->region_id = region->region_id;
        s_req->num_logs = nums_logs;
        s_req->next_log_id = next_log_id + nums_logs;
        s_req->log_status = log_status;
        s_req->entries_len = entries_len;

        bool complete_cb = false;
        st_ctx->rpc->enqueue_request(st_ctx->s2s_session_num_vec[server_session_id],
                            kSendRegionLogReq, &ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_,
                            &ST_CTX_RPC_WINDOW(msg_idx).resp_msgbuf_, 
                            set_complete_cb, reinterpret_cast<void*>(&complete_cb));
        while(complete_cb == false) {
            st_ctx->rpc->run_event_loop_once();
        }

        auto resp = &ST_RPC_RESP_BUF(msg_idx)->SendRegionLogResp;
        assert(resp->resp_type == RespType::kSuccess);

        if(resp->log_status == LogStatus::Done) {
            // TODO:
            // 删除region的所有log
            // 删除region内kv，但目前metakv好像没开GC，就不删了

            // 删除临时创建的region
            {
                WriteGuard wl(s_ctx->region_map_lock);
                s_ctx->region_map.erase(region->region_id);
            }
            
            delete region;

            // 更新region状态
            left_region->log_id = 0;
            left_region->region_status = RegionStatus::Normal;

            s_ctx = NULL; // region split结束
            p_info("split region done");
            return;
        } else {
            // 继续发送log
            next_log_id = resp->next_log_id;
        }
    }
    p_info("444444444444");
}

const int32_t region_entry_max_size = (metafs_inode_size + METAFS_MAX_FNAME_LEN + metafs_inode_size + metafs_stat_size);


// 现在迁移是把整个目录迁走,该函数不迁stat
// 如果传入的pinode==0,则表示pinode set还没有开始读
void rpc_send_region(ServerRegion *region, uint64_t msg_idx, uint64_t pinode_hash, metafs_inode_t pinode, uint64_t next_offset) {
    int32_t server_session_id = region->region_id < st_ctx->num_server_sessions ? region->region_id : 
                                        JumpConsistentHash(region->region_id, st_ctx->num_server_sessions);
    uint64_t is_uncomplete = 1;

    p_info("rpc_send_region, msg_idx:%ld", msg_idx);

{

    ReadGuard rl(s_ctx->pinode_table_lock);
    p_info("pinode_table size :%d", s_ctx->pinode_table.size());

    map<uint64_t, set<metafs_inode_t>>::iterator pinode_set_iter;
    set<metafs_inode_t>::iterator pinode_iter;

    // region是左闭右闭区间
    metafs_inode_t end_pinode_hash = region->end_key.hi;

    for (; pinode_hash <= end_pinode_hash; pinode_hash++) {
        pinode_set_iter = s_ctx->pinode_table.find(pinode_hash);
        if(pinode_set_iter == s_ctx->pinode_table.end()) {
            continue;
        }
        pinode_iter = (pinode == 0) ? pinode_set_iter->second.begin() : pinode_set_iter->second.find(pinode);
        while(pinode_iter != pinode_set_iter->second.end()) {
            pinode = *pinode_iter;
            p_info("pinode: %llx, pinode_hash % PINODE_HASH_RANGE: %lu, next_offset: %ld", pinode, 
                                XXH3_64bits((void*)&pinode, metafs_inode_size) % PINODE_HASH_RANGE, next_offset);
            char *res = NULL;
            MetaKvStatus status = ReadDir(s_ctx->metadb, pinode, &res, next_offset, MSG_ENTEY_MAX_SIZE);
            if(res != NULL) {
                st_ctx->rpc->resize_msg_buffer(&ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_, SendRegionReq_size);
                auto s_req = &ST_RPC_REQ_BUF(msg_idx)->SendRegionReq;

                // res前四个int64字段存储了res自身的元数据
                struct LogScanHeader *header = (struct LogScanHeader *) res;
                s_req->offset = header->new_offset;
                next_offset = header->new_offset;
                is_uncomplete = header->is_uncomplete;
                s_req->num_result = header->rel_count;
                s_req->entries_len = header->rel_len;
                p_assert(s_req->entries_len <= MSG_ENTEY_MAX_SIZE, "buffer oversize, entries_len:%ld", s_req->entries_len);
                memcpy(&s_req->entries, res + 4 * sizeof(uint64_t), s_req->entries_len);
                free(res);
                res = NULL;

                {
                    int32_t num_result = s_req->num_result;
                    uint8_t *buf = s_req->entries;
                    int qwert = 0;
                    while(num_result--) {
                        metafs_inode_t pinode = (metafs_inode_t)*buf;
                        buf += metafs_inode_size;

                        char *fname = (char*)buf;
                        size_t fname_len = strlen(fname) + 1;
                        buf += fname_len;

                        metafs_inode_t inode = (metafs_inode_t)*buf;
                        buf += metafs_inode_size;
                        p_assert(false, "entry: %x, %x, %x, %x, pinode: %lx, fname: %s, inode:%lx",
                                         buf[0], buf[1], buf[2], buf[3], pinode, fname, inode);
                        p_info("#%d origin region: pinode:%llx, fname:%s, inode:%llx", qwert++, pinode, fname, inode);
                    }
                }

                ServerRegion *left_region;
                {
                    ReadGuard rl(s_ctx->region_map_lock);
                    auto region_iter = s_ctx->region_map.find(region->left_region_id);
                    p_assert(region_iter != s_ctx->region_map.end(), "region_iter should not be null");
                    left_region = region_iter->second;
                }
                
                // 更新原region的kv_num
                left_region->kv_num -= s_req->num_result;
            
                p_info("send region to target, pinode: %llx, num_kv:%d, next_offset:%llu",
                                 pinode, s_req->num_result, next_offset);
                // 发送rpc
                s_req->region_id = region->region_id;
                s_req->is_uncomplete = is_uncomplete;
                s_req->pinode_hash = pinode_hash;
                s_req->pinode = pinode;

                // 先使用同步实现
                bool complete_cb = false;
                st_ctx->rpc->enqueue_request(st_ctx->s2s_session_num_vec[server_session_id],
                                        kSendRegionReq, &ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_,
                                        &ST_CTX_RPC_WINDOW(msg_idx).resp_msgbuf_, 
                                        set_complete_cb, reinterpret_cast<void*>(&complete_cb));
                while(complete_cb == false) {
                    st_ctx->rpc->run_event_loop_once();
                }

                auto resp = &ST_RPC_RESP_BUF(msg_idx)->SendRegionResp;
                assert(resp->resp_type == RespType::kSuccess);

                if(is_uncomplete == 0) {
                    pinode_iter++;
                    next_offset = 0;
                }
            } else {
                // p_info("continue readdir2");
                pinode_iter++;
                next_offset = 0;
                // p_assert(false, "should not be here, status: %d", status);
            }
        }
        pinode = 0;
        next_offset = 0;
    }
        
    p_info("333333333333");
}
    // region发送完毕，开始发送region_log
    rpc_send_region_log(region, 0, msg_idx);
}

// // 现在迁移是把整个目录迁走
// // 如果传入的pinode==0,则表示pinode set还没有开始读
// void rpc_send_region(ServerRegion *region, uint64_t msg_idx, uint64_t pinode_hash, metafs_inode_t pinode, uint64_t next_offset) {
//     int32_t server_session_id = region->region_id < st_ctx->num_server_sessions ? region->region_id : 
//                                         JumpConsistentHash(region->region_id, st_ctx->num_server_sessions);
//     uint64_t is_uncomplete = 1;

//     p_info("rpc_send_region, msg_idx:%ld", msg_idx);

//     while(is_uncomplete != 0) {
//         st_ctx->rpc->resize_msg_buffer(&ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_, SendRegionReq_size);
//         auto s_req = &ST_RPC_REQ_BUF(msg_idx)->SendRegionReq;

//         int32_t entry_len = 0;
//         int32_t num_result = 0;
//         char *res = NULL;

//         map<uint64_t, set<metafs_inode_t>>::iterator pinode_set_iter;
//         set<metafs_inode_t>::iterator pinode_iter;

//         // region是左闭右闭区间
//         metafs_inode_t end_pinode_hash = region->end_key.hi;
//         map<uint64_t, set<metafs_inode_t>>::iterator end_set_iter;

//         // {
//             ReadGuard rl(s_ctx->pinode_table_lock);
//             p_info("pinode_table size :%d", s_ctx->pinode_table.size());
//             pinode_set_iter = s_ctx->pinode_table.lower_bound(pinode_hash);
//             end_set_iter = s_ctx->pinode_table.upper_bound(end_pinode_hash);
//         // }

//         p_info("22222222222222");

//         for (; pinode_set_iter != end_set_iter; pinode_set_iter++) {
//             // p_info("3333333333");
//             pinode_iter = (pinode == 0) ? pinode_set_iter->second.begin() : pinode_set_iter->second.find(pinode);

//             while(pinode_iter != pinode_set_iter->second.end()) {
//                 // p_info("444444444");
//                 pinode = *pinode_iter;
//                 MetaKvStatus status = ReadDir(s_ctx->metadb, pinode, &res, next_offset, MSG_ENTEY_MAX_SIZE);
//                 if(res != NULL) {
//                     p_info("pinode: %lx, pinode_hash % PINODE_HASH_RANGE: %lu, next_offset: %ld", pinode, 
//                                     XXH3_64bits((void*)&pinode, metafs_inode_size) % PINODE_HASH_RANGE, next_offset);
//                     // res前四个int64字段存储了res自身的元数据
//                     int64_t *entry_mdata = (int64_t*)res;
//                     int64_t num_entry = entry_mdata[2];
//                     p_info("num_entries: %ld", num_entry);
//                     // int64_t entries_len = entry_mdata[3] - 4 * sizeof(int64_t);

//                     // cal all entry's fname's xxhash
//                     const char *fname_ptr = (const char *)(res + 4 * sizeof(int64_t));
//                     int fname_len = -metafs_inode_size;
//                     metafs_inode_t inode;

//                     while(num_entry-- && entry_len < (MSG_ENTEY_MAX_SIZE - region_entry_max_size)) {
//                         fname_ptr += fname_len + (metafs_inode_size * 2);
//                         fname_len = strlen(fname_ptr) + 1; // fname end with '\0'
//                         p_info("read fname: %s", fname_ptr);
//                         inode = *(metafs_inode_t*)(fname_ptr + fname_len);

//                         // 复制一整条记录：pinode+fname+inode
//                         ::memcpy(s_req->entries + entry_len, fname_ptr - metafs_inode_size, (metafs_inode_size * 2) + fname_len);
//                         entry_len += (metafs_inode_size * 2) + fname_len;

//                         MetaKvSlice slice;
//                         // directly write to `s_rep->entries` when call GetStat
//                         SliceInit(&slice, metafs_stat_size, (char*)(s_req->entries + entry_len));
//                         status = GetStat(s_ctx->metadb, inode, &slice);
//                         entry_len += metafs_stat_size;
//                         next_offset += (metafs_inode_size * 2) + fname_len + metafs_stat_size;
//                         num_result++;
//                     }

//                     free(res);
//                     res = NULL;

//                     if(entry_len >= (MSG_ENTEY_MAX_SIZE - region_entry_max_size)) {
//                         p_info("goto send_region_rpc_out");
//                         goto send_region_rpc_out;
//                     } else {
//                         if(num_entry == 0) {
//                             p_info("continue readdir1");
//                             //继续readdir
//                             pinode_iter++;
//                             next_offset = 0;
//                             continue;
//                         } else {
//                             p_assert(false, "should not be here");
//                         }
//                     }
//                 } else {
//                     // p_info("continue readdir2");
//                     pinode_iter++;
//                     next_offset = 0;
//                     continue;
//                     // p_assert(false, "should not be here, status: %d", status);
//                 }
//             }
//             p_info("next pinode_set");
//             pinode = 0;
//             next_offset = 0;
//         }

//     send_region_rpc_out:
//         ServerRegion *left_region;
//         {
//             ReadGuard rl(s_ctx->region_map_lock);
//             auto region_iter = s_ctx->region_map.find(region->left_region_id);
//             p_assert(region_iter != s_ctx->region_map.end(), "region_iter should not be null");
//             left_region = region_iter->second;
//         }
//         p_info("send region to target, num_kv:%d, next_offset:%ld", num_result, next_offset);
//         // 更新原region的kv_num
//         left_region->kv_num -= num_result;

//         is_uncomplete = (pinode_set_iter == end_set_iter) ? 0 : is_uncomplete;
//         // 发送rpc，异步继续发送
//         s_req->region_id = region->region_id;
//         s_req->is_uncomplete = is_uncomplete;
//         s_req->num_result = num_result;
//         s_req->entries_len = entry_len;
//         s_req->pinode_hash = pinode_hash;
//         s_req->pinode = pinode;
//         s_req->offset = next_offset;

//         // 先使用同步实现
//         bool complete_cb = false;
//         st_ctx->rpc->enqueue_request(st_ctx->s2s_session_num_vec[server_session_id],
//                                 kSendRegionReq, &ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_,
//                                 &ST_CTX_RPC_WINDOW(msg_idx).resp_msgbuf_, 
//                                 set_complete_cb, reinterpret_cast<void*>(&complete_cb));
//         while(complete_cb == false) {
//             st_ctx->rpc->run_event_loop_once();
//         }

//         auto resp = &ST_RPC_RESP_BUF(msg_idx)->SendRegionResp;
//         assert(resp->resp_type == RespType::kSuccess);
//         next_offset = resp->offset;
//     }

//     p_info("333333333333");

//     // region发送完毕，开始发送region_log
//     rpc_send_region_log(region, 0, msg_idx);
// }


// // 现在迁移是把整个目录迁走
// // 如果传入的pinode==0,则表示pinode set还没有开始读
// void rpc_send_region(ServerRegion *region, uint64_t msg_idx, uint64_t pinode_hash, metafs_inode_t pinode, uint64_t next_offset) {
//     int32_t server_session_id = region->region_id < st_ctx->num_server_sessions ? region->region_id : 
//                                         JumpConsistentHash(region->region_id, st_ctx->num_server_sessions);
//     uint64_t is_uncomplete = 1;

//     p_info("rpc_send_region, msg_idx:%ld", msg_idx);

//     while(is_uncomplete != 0) {
//         st_ctx->rpc->resize_msg_buffer(&ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_, SendRegionReq_size);
//         auto s_req = &ST_RPC_REQ_BUF(msg_idx)->SendRegionReq;

//         int32_t entry_len = 0;
//         int32_t num_result = 0;
//         char *res = NULL;

//         map<uint64_t, set<metafs_inode_t>>::iterator pinode_set_iter;
//         set<metafs_inode_t>::iterator pinode_iter;

//         // region是左闭右闭区间
//         metafs_inode_t end_pinode_hash = region->end_key.hi;

//         // {
//             ReadGuard rl(s_ctx->pinode_table_lock);
//             p_info("pinode_table size :%d", s_ctx->pinode_table.size());
//         // }

//         p_info("22222222222222");

//         for (pinode_set_iter = s_ctx->pinode_table.find(pinode_hash);
//                         pinode_hash <= end_pinode_hash; pinode_hash++) {
//             // p_info("3333333333");
//             pinode_set_iter = s_ctx->pinode_table.find(pinode_hash);
//             pinode_iter = (pinode == 0) ? pinode_set_iter->second.begin() : pinode_set_iter->second.find(pinode);

//             while(pinode_iter != pinode_set_iter->second.end()) {
//                 // p_info("444444444");
//                 p_info("pinode: %lx, pinode_hash % PINODE_HASH_RANGE: %lu, next_offset: %ld", pinode, 
//                                     XXH3_64bits((void*)&pinode, metafs_inode_size) % PINODE_HASH_RANGE, next_offset);
//                 pinode = *pinode_iter;
//                 MetaKvStatus status = ReadDir(s_ctx->metadb, pinode, &res, next_offset, MSG_ENTEY_MAX_SIZE);
//                 if(res != NULL) {
//                     // res前四个int64字段存储了res自身的元数据
//                     int64_t *entry_mdata = (int64_t*)res;
//                     int64_t num_entry = entry_mdata[2];
//                     p_info("pinode:%lx, num_entries: %ld", pinode, num_entry);
//                     // int64_t entries_len = entry_mdata[3] - 4 * sizeof(int64_t);

//                     // cal all entry's fname's xxhash
//                     const char *fname_ptr = (const char *)(res + 4 * sizeof(int64_t));
//                     int fname_len = -metafs_inode_size;
//                     metafs_inode_t inode;

//                     while(num_entry-- && entry_len < (MSG_ENTEY_MAX_SIZE - region_entry_max_size)) {
//                         p_info("pinode:%lx", pinode);
//                         inode = *(metafs_inode_t*)(fname_ptr + fname_len);

//                         // 复制一整条记录：pinode+fname+inode
//                         ::memcpy(s_req->entries + entry_len, fname_ptr - metafs_inode_size, (metafs_inode_size * 2) + fname_len);
//                         entry_len += (metafs_inode_size * 2) + fname_len;

//                         MetaKvSlice slice;
//                         // directly write to `s_rep->entries` when call GetStat
//                         SliceInit(&slice, metafs_stat_size, (char*)(s_req->entries + entry_len));
//                         status = GetStat(s_ctx->metadb, inode, &slice);
//                         entry_len += metafs_stat_size;
//                         next_offset += (metafs_inode_size * 2) + fname_len;
//                         num_result++;
//                     }

//                     free(res);
//                     res = NULL;

//                     if(entry_len >= (MSG_ENTEY_MAX_SIZE - region_entry_max_size)) {
//                         fname_ptr += fname_len + (metafs_inode_size * 2);
//                         p_info("goto send_region_rpc_out, next fname:%s", fname_ptr);
//                         goto send_region_rpc_out;
//                     } else {
//                         if(num_entry == 0) {
//                             p_info("continue readdir1");
//                             //继续readdir
//                             pinode_iter++;
//                             next_offset = 0;
//                             continue;
//                         } else {
//                             p_assert(false, "should not be here");
//                         }
//                     }
//                 } else {
//                     // p_info("continue readdir2");
//                     pinode_iter++;
//                     next_offset = 0;
//                     continue;
//                     // p_assert(false, "should not be here, status: %d", status);
//                 }
//             }
//             p_info("next pinode_set");
//             pinode = 0;
//             next_offset = 0;
//         }

//     send_region_rpc_out:
//         ServerRegion *left_region;
//         {
//             ReadGuard rl(s_ctx->region_map_lock);
//             auto region_iter = s_ctx->region_map.find(region->left_region_id);
//             p_assert(region_iter != s_ctx->region_map.end(), "region_iter should not be null");
//             left_region = region_iter->second;
//         }
//         p_info("send region to target, num_kv:%d, next_offset:%ld", num_result, next_offset);
//         // 更新原region的kv_num
//         left_region->kv_num -= num_result;

//         is_uncomplete = (pinode_hash > end_pinode_hash) ? 0 : is_uncomplete;
//         // 发送rpc，异步继续发送
//         s_req->region_id = region->region_id;
//         s_req->is_uncomplete = is_uncomplete;
//         s_req->num_result = num_result;
//         s_req->entries_len = entry_len;
//         s_req->pinode_hash = pinode_hash;
//         s_req->pinode = pinode;
//         s_req->offset = next_offset;

//         // 先使用同步实现
//         bool complete_cb = false;
//         st_ctx->rpc->enqueue_request(st_ctx->s2s_session_num_vec[server_session_id],
//                                 kSendRegionReq, &ST_CTX_RPC_WINDOW(msg_idx).req_msgbuf_,
//                                 &ST_CTX_RPC_WINDOW(msg_idx).resp_msgbuf_, 
//                                 set_complete_cb, reinterpret_cast<void*>(&complete_cb));
//         while(complete_cb == false) {
//             st_ctx->rpc->run_event_loop_once();
//         }

//         auto resp = &ST_RPC_RESP_BUF(msg_idx)->SendRegionResp;
//         assert(resp->resp_type == RespType::kSuccess);
//         next_offset = resp->offset;
//     }

//     p_info("333333333333");

//     // region发送完毕，开始发送region_log
//     rpc_send_region_log(region, 0, msg_idx);
// }


// 对pinode+fname分别进行hash时的region迁移
// void rpc_send_region(ServerRegion *region, uint64_t msg_idx, metafs_inode_t pinode, uint64_t next_offset = 0) {
//     int32_t server_session_id = region->region_id < s_ctx->num_server_sessions ? region->region_id : 
//                                         JumpConsistentHash(region->region_id, s_ctx->num_server_sessions);

//     s_ctx->rpc->resize_msg_buffer(&S_CTX_RPC_WINDOW(msg_idx).req_msgbuf_, SendRegionReq_size);
//     auto s_req = &S_RPC_REQ_BUF(msg_idx)->SendRegionReq;

//     // region是左闭右闭区间
//     metafs_inode_t start_inode = region->start_key.hi;
//     uint64_t start_hash = region->start_key.low;
//     metafs_inode_t end_inode = region->end_key.hi;
//     uint64_t end_hash = region->end_key.low;

//     int32_t entry_len = 0;
//     int32_t num_result = 0;
//     uint64_t is_uncomplete = 1;
//     char *res = NULL;

//     while(pinode <= end_inode) {
//         MetaKvStatus status = ReadDir(s_ctx->metadb, pinode, &res, next_offset, MSG_ENTEY_MAX_SIZE);
//         if(check_status_ok(status)) {

//             if(res == NULL) {
//                 pinode++;
//                 next_offset = 0;
//                 is_uncomplete = 0;
//                 continue;
//             }

//             int64_t* entry_mdata = (int64_t*)res;
            
//             int64_t num_entry = entry_mdata[2];
//             int64_t entries_len = entry_mdata[3] - 4 * sizeof(int64_t);

//             // cal all entry's fname's xxhash
//             const char *fname_ptr = (const char *)(res + 4 * sizeof(int64_t));
//             int fname_len = -8;
//             metafs_inode_t inode;

//             while(num_entry-- && entry_len < (MSG_ENTEY_MAX_SIZE - region_entry_max_size)) {
//                 fname_ptr += fname_len + metafs_inode_size < 1;
//                 fname_len = strlen(fname_ptr) + 1; // fname end with '\0'
//                 inode = *(metafs_inode_t*)(fname_ptr + fname_len);
//                 XXH64_hash_t fname_xxhash = XXH3_64bits((void*)fname_ptr, fname_len);

//                 if((pinode != start_inode && pinode != end_inode)
//                      || (pinode == start_inode && fname_xxhash >= start_hash)
//                      || (pinode == end_inode && fname_xxhash < end_hash)) {
//                     memcpy(s_req->entries + entry_len, fname_ptr - metafs_inode_size, metafs_inode_size < 1 + fname_len);
//                     entry_len += metafs_inode_size < 1 + fname_len;
//                     MetaKvSlice slice;
//                     SliceInit(&slice, metafs_stat_size, (char*)(s_req->entries + entry_len));
//                     status = GetStat(s_ctx->metadb, inode, &slice);
//                     entry_len += metafs_stat_size;
//                     next_offset += metafs_inode_size < 1 + fname_len + metafs_stat_size;
//                     num_result++;
//                 }
//             }

//             free(res);
//             res = NULL;

//             if(entry_len >= (MSG_ENTEY_MAX_SIZE - region_entry_max_size)) {
//                 if(num_entry == 0) {
//                     // 使用当前状态发送rpc
//                     is_uncomplete = entry_mdata[1];
//                     break;
//                 } else {
//                     is_uncomplete = 1;
//                     break;
//                 }
//             } else {
//                 if(num_entry == 0) {
//                     //继续readdir
//                     continue;
//                 } else {
//                     assert(false); // 不可能发生
//                 }
//             }
//         } else {
//             assert(false);
//         }
//     }

//     // 发送rpc，异步继续发送
//     s_req->region_id = region->region_id;
//     s_req->is_uncomplete = is_uncomplete;
//     s_req->num_result = num_result;
//     s_req->entries_len = entry_len;
//     s_req->pinode = pinode;
//     s_req->offset = next_offset;

//     s_ctx->rpc->enqueue_request(s_ctx->s2s_session_num_vec[server_session_id],
//                             kSendRegionReq, &S_CTX_RPC_WINDOW(msg_idx).req_msgbuf_,
//                             &S_CTX_RPC_WINDOW(msg_idx).resp_msgbuf_, 
//                             rpc_send_region_cb, reinterpret_cast<void *>(msg_idx));
// }

void rpc_create_region_cb(void *_context, void *_idx) {
    uint64_t msg_idx = reinterpret_cast<uint64_t>(_idx);
    p_info("rpc_create_region_cb, msg_idx=%lu", msg_idx);
    auto resp = &ST_RPC_RESP_BUF(msg_idx)->CreateRegionResp;
    // auto resp = &(reinterpret_cast<wire_resp_t*>(c->window_[msg_idx].resp_msgbuf_.buf_)->CreateRegionResp);
    assert(resp->resp_type == RespType::kSuccess);
    // 发送region

    ServerRegion *region;
    {
        ReadGuard rl(s_ctx->region_map_lock);
        auto region_iter = s_ctx->region_map.find(resp->region_id);
        p_assert(region_iter != s_ctx->region_map.end(), "region_iter should not be null");
        region = region_iter->second;
    }

    rpc_send_region(region, msg_idx, region->start_key.hi);
}

// void rpc_send_region_cb(void *_context, void *_idx) {
//     uint64_t msg_idx = reinterpret_cast<uint64_t>(_idx);
//     auto resp = &ST_RPC_RESP_BUF(msg_idx)->SendRegionResp;
//     assert(resp->resp_type == RespType::kSuccess);

//     ServerRegion *region;
//     {
//         ReadGuard rl(s_ctx->region_map_lock);
//         auto region_iter = s_ctx->region_map.find(resp->region_id);
//         p_assert(region_iter != s_ctx->region_map.end(), "region_iter should not be null");
//         region = region_iter->second;
//     }

//     if (resp->is_uncomplete) {
//         rpc_send_region(region, msg_idx, resp->pinode_hash, resp->pinode, resp->offset);
//     } else {
//         // 发送log
//         rpc_send_region_log(region, 0, msg_idx);
//     }
// }

// void rpc_send_region_log_cb(void *_context, void *_idx) {
//     uint64_t msg_idx = reinterpret_cast<uint64_t>(_idx);
//     auto resp = &ST_RPC_RESP_BUF(msg_idx)->SendRegionLogResp;
//     assert(resp->resp_type == RespType::kSuccess);

//     ServerRegion *region, *left_region;
//     {
//         ReadGuard rl(s_ctx->region_map_lock);
//         auto region_iter = s_ctx->region_map.find(resp->region_id);
//         p_assert(region_iter != s_ctx->region_map.end(), "region_iter should not be null");
//         region = region_iter->second;

//         region_iter = s_ctx->region_map.find(region->left_region_id);
//         p_assert(region_iter != s_ctx->region_map.end(), "region_iter should not be null");
//         left_region = region_iter->second;
//     }

//     if(resp->log_status == LogStatus::Done) {
//         // TODO:
//         // 删除region的所有log
//         // 删除region内kv，但目前metakv好像没开GC，就不删了

//         // 删除region
//         {
//             WriteGuard wl(s_ctx->region_map_lock);
//             s_ctx->region_map.erase(region->region_id);
//         }
        
//         delete region;

//         // 更新region状态
//         left_region->region_status = RegionStatus::Normal;
//         left_region->log_id = 0;

//         s_ctx = NULL; // region split结束
//     } else {
//         rpc_send_region_log(region, resp->next_log_id, msg_idx);
//     }
// }

}