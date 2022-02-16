#include <fcntl.h>
#include <unordered_map>

#include "util/fs_log.h"
#include "client/metaclient.h"

using namespace std;

namespace metafs {

// return if need to retry
bool MetaClient::handle_rpc_resp(rpc_resp_t res, const char *rpc_info) {
    switch(res) {
        case RespType::kSuccess:{
            return false;
        }
        case RespType::kFail:{
            LOG(ERROR) << rpc_info << " fail";
            return false;
        }
        case RespType::kUpdateRegionMap:{
            LOG(INFO) << "need to update region_map";
            while(rpc_client_->RPC_ReadRegionmap() != RespType::kSuccess)
                FS_LOG("raed region map fail, continue try...");
            return true;
        }
        case RespType::kEBUSY: {
            this_thread::yield();
            return true;
        }
    }
    return false;
}

int MetaClient::ReadRegionmap() {
    rpc_resp_t res = rpc_client_->RPC_ReadRegionmap();
    if(res != RespType::kSuccess)
        return -1;
    return 0;
}

rocksdb::Status MetaClient::ResolvePath(const string &path, metafs_inode_t &pinode, string &fname, int *depth){
    FS_LOG("Resolving path: %s", path.c_str());

    if(path.empty()) {
        LOG(ERROR) << "Error";
        return rocksdb::Status::InvalidArgument("Empty path");
    }

    if(path.substr(0,1) != "/") {
        LOG(ERROR) << "Error";
        return rocksdb::Status::InvalidArgument("Invalid path", path);
    }

    if(path.size() == 1) {
        pinode = fs_root_inode;
        fname = "";
        if(depth != NULL) {
            depth = 0;
        }
        FS_LOG("Resolved path: %s, pinode : %d, fname: %s", path.c_str(), pinode, fname.c_str());
        return rocksdb::Status::OK();
    }

    if(path.substr(path.size() - 1, 1) == "/") {
        string new_path = path.substr(0, path.size() - 1);
        return ResolvePath(new_path, pinode, fname, depth);
    }

    rocksdb::Status s = Internal_ResolvePath(path, pinode, fname, depth);
    
    if(s.ok()) {
        FS_LOG("Resolved path: %s, pinode : %d, fname: %s", path.c_str(), pinode, fname.c_str());
    } else {
        LOG(ERROR) << "Error";
        FS_LOG("Resolving path %s error.", path);
    }
    return s;
}

// TODO:放入Cache时, key = pinode + hash(fname)
rocksdb::Status MetaClient::Internal_ResolvePath(const string &path, metafs_inode_t &pinode, string &fname, int *depth) {
    int path_depth = 0;
    metafs_inode_t pdir_id = 0;
    string name;
    size_t now = 0, last = 0, end = path.rfind("/");

    while(last < end) {
        now = path.find("/", last + 1);
        if(now - last > 1) {
            path_depth++;
            name = path.substr(last + 1, now - last - 1);     
            
#ifdef USE_CACHE
            // get from cache.
            DirentryValue value;
            rocksdb::Status s = c_ctx->dentry_cache->Get(pdir_id, name, &value);
            if(!s.ok()) { // not exist in cache.
                // printf("cache miss, times:%d\n", c_ctx->dentry_cache->cache_miss_cnt_incr());
                // lookup from servers
                // TODO: now don't split directory, so just use pinode to get target server.
                rpc_resp_t res = rpc_client_->RPC_Getinode(pdir_id, name, value.inode);
                if(handle_rpc_resp(res, "resolvepath:getinode")) {
                    res = rpc_client_->RPC_Getinode(pdir_id, name, value.inode);
                }
                // add to cache.
                if(res == kSuccess) {
                    c_ctx->dentry_cache->Put(pdir_id, name, value);
                } else {
                    FS_LOG("get inode fail");
                    return rocksdb::Status::Corruption("RPC_Getinode fail");
                }
            }
            pdir_id = value.inode;
#else
            rpc_resp_t res = rpc_client_->RPC_Getinode(pdir_id, name, pdir_id);
            if(handle_rpc_resp(res, "resolvepath:getinode")) {
                res = rpc_client_->RPC_Getinode(pdir_id, name, pdir_id);
            }
            // add to cache.
            if(res == kFail) {
                FS_LOG("get inode fail");
                return rocksdb::Status::Corruption("RPC_Getinode fail");
            }
#endif      
        }
        last = now;
    }

    pinode = pdir_id;
    fname = path.substr(end + 1);
    if(depth != nullptr) {
        *depth = path_depth;
    }

    return rocksdb::Status::OK();
}

// File management

int MetaClient::Getstat(metafs_inode_t pinode, const string &fname, metafs_inode_t &inode, metafs_stat_t &stat) {
    FS_LOG("Getstat");
    
    rpc_resp_t res = rpc_client_->RPC_Getstat(pinode, fname, inode, stat);
    if(handle_rpc_resp(res, "getstat")) {
        res = rpc_client_->RPC_Getstat(pinode, fname, inode, stat);
    }

    if(res != kSuccess) {
        LOG(ERROR) << "fail Getstat: " << "pinode: " << pinode << " fname: " << fname;
        return -ENOENT; 
    }

    FS_LOG("Getstat success");
    return 0;
}

int MetaClient::Mknod(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat) {
    FS_LOG("Mknod");

    rpc_resp_t res = rpc_client_->RPC_Mknod(pinode, fname, mode, inode, stat);
    if(handle_rpc_resp(res, "mknod")) {
        res = rpc_client_->RPC_Mknod(pinode, fname, mode, inode, stat);
    }

    if(res != kSuccess) {
        LOG(ERROR) << "Error";
        return -EEXIST;
    }
    FS_LOG("Mknod succeess");
    return 0;
}

int MetaClient::Open(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat) {
    FS_LOG("Open, pinode %d, fname:%s", pinode, fname.c_str());

    rpc_resp_t res = rpc_client_->RPC_Open(pinode, fname, mode, inode, stat);

    if(handle_rpc_resp(res, "open")) {
        res = rpc_client_->RPC_Open(pinode, fname, mode, inode, stat);
    }

    if(res != kSuccess) {
        LOG(ERROR) << "Error";
        return -ENOENT;
    }

    FS_LOG("Open succeess, inode: %ld", inode);
    return 0;
}

int MetaClient::Unlink(metafs_inode_t pinode, const string &fname) {
    FS_LOG("Unlink");

    rpc_resp_t res = rpc_client_->RPC_Unlink(pinode, fname);

    if(handle_rpc_resp(res, "unlink")) {
        res = rpc_client_->RPC_Unlink(pinode, fname);
    }

    if(res != kSuccess) {
        LOG(ERROR) << "Error unlink";
        return -ENOENT;
    }

    FS_LOG("Unlink success");
    return 0;
}

// 删除文件或目录
// server不需要关心是否是目录,即认为用户已经删除了目录内的所有文件
// int MetaClient::Remove(const string &path, mode_t mode) {
//     FS_LOG("Remove, path: %s", path.c_str());
//     metafs_inode_t pinode;
//     string fname;

//     rocksdb::Status s = ResolvePath(path, pinode, fname);
//     if(!s.ok()) {
//         return -ENOENT;
//     } 


//     rpc_resp_t res = rpc_client_->RPC_Remove(pinode, fname, mode);
//     if(res != kSuccess) {
//         LOG(ERROR) << "Error remove";
//         if(res == kENOTDIR) {
//             return -ENOTDIR;
//         } else {
//             return -ENOENT;
//         }
//     }
//     FS_LOG("Remove success");
//     return 0;
// }

// if is a dir, how  to rename.
// int MetaClient::Rename(const string &src, const string &target) {
//     // No fault tolerant Rename
//     FS_LOG("Rename, old_name:%s, new_name:%s", src.c_str(), target.c_str());
//     metafs_inode_t src_pinode;
//     string src_fname;
//     rocksdb::Status s = ResolvePath(src, src_pinode, src_fname);
//     if(!s.ok()) {
//         return -ENOENT;
//     }

//     metafs_inode_t target_pinode;
//     string target_fname;
//     s = ResolvePath(target, target_pinode, target_fname);
//     if(!s.ok()) {
//         return -ENOENT;
//     }

//     metafs_stat_t stat;
//     rpc_resp_t res = rpc_client_->RPC_Getstat(src_pinode, src_fname, stat);
//     if(res != kSuccess) {
//         LOG(ERROR) << "Error rename:getattr";
//         return -ENOENT;
//     }

//     res = rpc_client_->RPC_Create(target_pinode, target_fname, stat);
//     if(res != kSuccess) {
//         LOG(ERROR) << "Error rename:create";
//         return -EIO;
//     }

//     res = rpc_client_->RPC_Remove(src_pinode, src_fname, stat.mode);
//     if(res != kSuccess) {
//         LOG(ERROR) << "Error rename:remove";
//         return -ENOENT;
//     }

//     FS_LOG("Rename success");
//     return 0;
// }

// Directory management

int MetaClient::Mkdir(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat) {
    FS_LOG("Mkdir");

    rpc_resp_t res = rpc_client_->RPC_Mkdir(pinode, fname, mode, inode);

    if(handle_rpc_resp(res, "mkdir")) {
        res = rpc_client_->RPC_Mkdir(pinode, fname, mode, inode);
    }

    if(res != kSuccess) {
        LOG(ERROR) << "Error mkdir";
        return -EEXIST;
    }

    FS_LOG("Mkdir success");
    return 0;
}

// readdir前需要已经得到rangemap
int MetaClient::Readdir(metafs_inode_t dir_inode, shared_ptr<OpenDir> &open_dir) {
    FS_LOG("Readdir, dirinode: %d", dir_inode);

    rpc_resp_t res = rpc_client_->RPC_Readdir(dir_inode, open_dir);

    if(handle_rpc_resp(res, "readdir")) {
        open_dir->clearEntries();
        res = rpc_client_->RPC_Readdir(dir_inode, open_dir);
    }

    if(res) {
        LOG(ERROR) << "Error";
        return -ENOENT;
    } else {
        FS_LOG("Readdir success");
    }
    
    return 0;
}

int MetaClient::Rmdir(metafs_inode_t pinode, const string &fname) {
    FS_LOG("Rmdir: %s", fname.c_str());
    
    rpc_resp_t res = rpc_client_->RPC_Rmdir(pinode, fname);

    if(handle_rpc_resp(res, "rmdir")) {
        res = rpc_client_->RPC_Rmdir(pinode, fname);
    }

    if(res != kSuccess) {
        LOG(ERROR) << "Error rmdir";
        return -ENOENT;
    }

    FS_LOG("Rmdir success");
    return 0;
}

// io management

int MetaClient::Read(int fd, size_t offset, size_t size, char *buf, int *ret_size) {
    // TODO
    FS_LOG("Not implement");
    // return -ENOTSUP;
    return 0;
}

int MetaClient::Write(int fd, size_t offset, size_t size, const char *buf) {
    // TODO
    FS_LOG("Not implement");
    // return -ENOTSUP;
    return 0;
}

}// end namespace metafs