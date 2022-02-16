#include <vector>
#include <thread>

#include "server/metafs_server.h"

using namespace std;

namespace metafs {

atomic<region_id_t> global_region_id(0);
unordered_map<region_id_t, ClientRegion> global_region_map;
RWLock global_region_map_rwlock;

struct server_config *s_cfg;
erpc::Nexus* s_nexus; // 每个进程一个
thread_local struct server_context *s_ctx; 
struct server_context s_ctx_arr[MAX_FG_THREADS]; // 伪共享问题？
threadsafe_queue<pair<size_t, region_id_t>> shared_split_queue;
struct split_region_thread_context *st_ctx; // 只用于split thread

void server_parse_config(const char *fn) {
    p_assert(fn, "no config file");
    struct conf_parser cparser[] = {
      {"local_ip", offsetof(struct server_config, local_ip), cJSON_String, "localhost"},
      {"local_port", offsetof(struct server_config, local_port), cJSON_Number, "31851"},
      {"num_servers", offsetof(struct server_config, num_servers), cJSON_Number, "1"},
      {"server_list", offsetof(struct server_config, server_list), cJSON_Array, NULL},
      {"server_fg_threads", offsetof(struct server_config, server_fg_threads), cJSON_Number, "1"},
      {"server_bg_threads", offsetof(struct server_config, server_bg_threads), cJSON_Number, "0"},
      {"metakv_pm_space", offsetof(struct server_config, metakv_pm_space), cJSON_Number, "8"},
      {"metakv_path", offsetof(struct server_config, metakv_path), cJSON_String, "/tmp/"},
      {"rocksdb_path", offsetof(struct server_config, rocksdb_path), cJSON_String, "/tmp/"},
      {"memcached_ip", offsetof(struct server_config, memcached_ip), cJSON_String, "localhost"},
      {"memcached_port", offsetof(struct server_config, memcached_port), cJSON_Number, "0"},
      {NULL, 0, 0, NULL},
    };

    s_cfg = (struct server_config *)safe_alloc(sizeof(struct server_config), true);
    p_assert(s_cfg != NULL, "s_cfg is null");
    parse_from_file(s_cfg, fn, cparser);
}

void init_server_config() {
    const char *fn = getenv("METAFS_SERVER_CONFIG");
    server_parse_config(fn);
    p_assert(s_cfg->server_fg_threads <= 40 
                && s_cfg->server_fg_threads > 0, "");
    
    //init server id
	{
		struct mc_ctx *mi;
		mi = mt_create_ctx(s_cfg->memcached_ip, s_cfg->memcached_port);
		s_cfg->id = mt_incr(mi, "SERVER_ID");
		printf("server id: %d\n", s_cfg->id);
		mt_destory_ctx(mi);
	}
}

void init_server_context(size_t thread_id) {
    s_ctx = &s_ctx_arr[thread_id];
    p_assert(s_ctx != NULL, "ctx is null");

    s_ctx->thread_id = thread_id;
    s_ctx->global_id = s_cfg->id * s_cfg->server_fg_threads + thread_id;
    init_alloc_inode();

    // init metadb
    DBOption metadb_option;
    SetDefaultDBop(&metadb_option);
    SetAllocOp(&metadb_option, s_cfg->metakv_pm_space << 30UL, DEFAULT_CHUNK_SIZE, metafs_stat_size);
    string metadb_path(s_cfg->metakv_path);
    metadb_path += "metafs_metakv_";
    metadb_path += to_string((s_cfg->id << 8) + thread_id);
    p_info("thread#%d, metadb_path: %s",thread_id, metadb_path.c_str());
    
    s_ctx->metadb = (struct MetaDb *)safe_alloc(sizeof(struct MetaDb), true);
    
    MetaKvStatus status = DBOpen(&metadb_option, metadb_path.c_str(), s_ctx->metadb);
    
    p_assert(status == OK, "init metadb fail");
    p_assert(s_ctx->metadb != NULL, "init metadb fail");
    p_info("init metadb success, path : %s", metadb_path.c_str());

    // init rocksdb
    rocksdb::Options rocksdb_option;
    rocksdb_option.create_if_missing = true;
    string rocksdb_path(s_cfg->rocksdb_path);
    rocksdb_path += "metafs_rocksdb_" + to_string((s_cfg->id << 8) + thread_id);
    rocksdb::Status s = rocksdb::DB::Open(rocksdb_option, rocksdb_path, &s_ctx->log_db);
    p_assert(s.ok(), "rocksdb(logdb) init fail");
    p_assert(s_ctx->log_db != NULL, "rocksdb(logdb) init fail");
    p_info("init rocksdb success, path : %s", rocksdb_path.c_str());

    // init per-thread erpc context
    s_ctx->rpc = new erpc::Rpc<erpc::CTransport>(s_nexus, (void*)(s_ctx), thread_id, nullptr);
    s_ctx->rpc->retry_connect_on_invalid_rpc_id_ = true;

    // 注册region
    register_region();
    p_info("region register done");
}

void run_server_thread(size_t thread_id) {
    p_info("init server#%d thread#%d", s_cfg->id, thread_id);
    init_server_context(thread_id);
    p_info("server#%d thread#%d run event loop", s_cfg->id, thread_id);
    s_ctx->rpc->run_event_loop(1000000);
}

void split_region_thread(size_t thread_id) {
    st_ctx = new split_region_thread_context();
    p_assert(st_ctx, "should not be null");
    p_info("server#%d split thread init...", s_cfg->id);
    st_ctx->thread_id = thread_id;
    st_ctx->num_server_sessions = s_cfg->num_servers * s_cfg->server_fg_threads;
    st_ctx->rpc = new erpc::Rpc<erpc::CTransport>(s_nexus, (void*)(st_ctx), thread_id, st_basic_sm_handler);
    st_ctx->rpc->retry_connect_on_invalid_rpc_id_ = true;
    p_info("server#%d split thread : build erpc connect...", s_cfg->id);
    // 建立split thread到其他服务器所有线程的erpc连接
    st_ctx->s2s_session_num_vec.resize(st_ctx->num_server_sessions);
    for(int i = 0; i < s_cfg->num_servers; i++) {
        for(int j = 0; j < s_cfg->server_fg_threads; j++) {
            int session_id = i * s_cfg->server_fg_threads + j;
            st_ctx->s2s_session_num_vec[session_id] = st_ctx->rpc->create_session(s_cfg->server_list[i], j);
        }
    }

    while(st_ctx->num_sm_resps_ != st_ctx->num_server_sessions) {
        st_ctx->rpc->run_event_loop_once();
    }

    for(int i = 0; i < s_cfg->num_servers; i++) {
        for(int j = 0; j < s_cfg->server_fg_threads; j++) {
            int session_id = i * s_cfg->server_fg_threads + j;
            if(!st_ctx->rpc->is_connected(st_ctx->s2s_session_num_vec[session_id])) {
                p_assert(false, "erpc not connected");
            }
        }
    }

    p_info("server#%d split thread : init erpc msgbuffer...", s_cfg->id);
    // init erpc msg buffer window
    for (size_t msgbuf_idx = 0; msgbuf_idx < MAX_MSG_BUF_WINDOW; msgbuf_idx++) {
        st_ctx->window_[msgbuf_idx].req_msgbuf_ =
            st_ctx->rpc->alloc_msg_buffer_or_die(sizeof(wire_req_t));

        st_ctx->window_[msgbuf_idx].resp_msgbuf_ =
            st_ctx->rpc->alloc_msg_buffer_or_die(sizeof(wire_resp_t));
    }

    p_info("server#%d split thread : listen to shared_queue...", s_cfg->id);
    while(true) {
        if(shared_split_queue.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            pair<size_t, region_id_t> p;
            shared_split_queue.pop(p);
            p_info("split_thread: thread#%d, next region:#%d", p.first, p.second);
            p_assert(s_ctx == NULL, "split thread's s_ctx should be null");
            s_ctx = &s_ctx_arr[p.first]; // thread's ctx
            rpc_create_region(p.second);
        }
    }
}

void init_and_start_loop() {
    // init process's erpc_nexus
    string uri(s_cfg->local_ip);
    uri += ":" + to_string(s_cfg->local_port);
    p_info("server uri: %s", uri.c_str());
    s_nexus = new erpc::Nexus(uri, 0, 0);
    p_assert(s_nexus != NULL, "s_nexus is null")

    s_nexus->register_req_func(metafs::kReqType::kFSOpenReq, fs_open_handler);
    s_nexus->register_req_func(metafs::kReqType::kFSUnlinkReq, fs_unlink_handler);
    s_nexus->register_req_func(metafs::kReqType::kFSStatReq, fs_stat_handler);
    s_nexus->register_req_func(metafs::kReqType::kFSMknodReq, fs_mknod_handler);
    s_nexus->register_req_func(metafs::kReqType::kFSMkdirReq, fs_mkdir_handler);
    s_nexus->register_req_func(metafs::kReqType::kFSRmdirReq, fs_rmdir_handler);
    s_nexus->register_req_func(metafs::kReqType::kFSReaddirReq, fs_readdir_handler);
    s_nexus->register_req_func(metafs::kReqType::kFSGetinodeReq, fs_getinode_handler);

    s_nexus->register_req_func(metafs::kReqType::kReadRegionmap, read_region_map_handler);
    
    s_nexus->register_req_func(metafs::kReqType::kCreateRegionReq, s2s_create_region_handler);
    s_nexus->register_req_func(metafs::kReqType::kSendRegionReq, s2s_send_region_handler);
    s_nexus->register_req_func(metafs::kReqType::kSendRegionLogReq, s2s_send_region_log_handler);

    // init per thread ctx and run event loop
    vector<thread> s_threads(s_cfg->server_fg_threads + 1);

    // for(size_t i = 0; i < s_cfg->server_fg_threads; i++) {
    //     s_threads[i] = thread(run_server_thread, i);

    //     cpu_set_t cpuset;
    //     CPU_ZERO(&cpuset);
    //     CPU_SET(i, &cpuset);
    //     pthread_setaffinity_np(s_threads[i].native_handle(), sizeof(cpu_set_t), &cpuset);
    // }

    // std::this_thread::sleep_for(std::chrono::seconds(2));

    // s_threads[s_cfg->server_fg_threads] = thread(split_region_thread, s_cfg->server_fg_threads);
    // cpu_set_t cpuset;
    // CPU_ZERO(&cpuset);
    // CPU_SET(s_cfg->server_fg_threads, &cpuset);
    // pthread_setaffinity_np(s_threads[s_cfg->server_fg_threads].native_handle(), sizeof(cpu_set_t), &cpuset);

    for(size_t i = 0; i < s_cfg->server_fg_threads + 1; i++) {
        if(i < s_cfg->server_fg_threads) {
            s_threads[i] = thread(run_server_thread, i);
        } else {
            s_threads[i] = thread(split_region_thread, i);
        }

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        pthread_setaffinity_np(s_threads[i].native_handle(), sizeof(cpu_set_t), &cpuset);
    }

    for(size_t i = 0; i < s_cfg->server_fg_threads + 1; i++) {
        s_threads[i].join();
    }
}

}

int main(int argc, char **argv) {
    metafs::init_server_config();
    // init erpc s_nexus
    metafs::init_and_start_loop();
}