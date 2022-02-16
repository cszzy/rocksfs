#pragma once

#include <glog/logging.h>
#include <rocksdb/status.h>
#include <rocksdb/cache.h>
#include <thread>

#include "rpc/rpc_common.h"
#include "rpc/rpc_client.h"
#include "client/dentry_cache.h"
#include "common/fs.h"
#include "common/lrucache.h"
#include "util/jump_hash.h"
#include "client/open_file_map.h"
#include "client/open_dir.h"
#include "client/hooks.h"

using namespace std;

#define METAFS_CLIENT (MetaClient::get_Instance())

#define USE_CACHE

namespace metafs {

class MetaClient{
    public:
        static MetaClient *get_Instance() {
            static MetaClient client;
            return &client; 
        }

        MetaClient() : ofm_(std::make_shared<OpenFileMap>()) { }

        ~MetaClient() {
            delete rpc_client_;
        };

        MetaClient(MetaClient const&) = delete;

        void operator=(MetaClient const&) = delete;

        void Init() {
            // get process id from memcached server
            rpc_client_ = new RpcClient();
        }

        const std::shared_ptr<metafs::OpenFileMap>& file_map() const {
            return ofm_;
        }

        rocksdb::Status ResolvePath(const string &path, metafs_inode_t &pinode, string &fname, int *depth = NULL);

        int Getstat(metafs_inode_t pinode, const string &fname, metafs_inode_t &inode, metafs_stat_t &stat);

        int Mknod(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat);

        int Open(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat);

        int Unlink(metafs_inode_t pinode, const string &fname);

        // TODO: need to redo.
        // int Rename(const string &src, const string &target);

        // FileSystem directory Management

        int Mkdir(metafs_inode_t pinode, const string &fname, mode_t mode, metafs_inode_t &inode, metafs_stat_t &stat);

        int Readdir(metafs_inode_t dir_inode, shared_ptr<OpenDir> &open_dir);

        int Rmdir(metafs_inode_t pinode, const string &fname);

        // FileSystem io Management

        int Read(int fd, size_t offset, size_t size, char *buf, int *ret_size);

        int Write(int fd, size_t offset, size_t size, const char *buf);

        int ReadRegionmap();

        // return if need to retry
        bool handle_rpc_resp(rpc_resp_t res, const char *rpc_info);
 
    private:

        // Parse the `path` to get target file's pinode and fname.
        rocksdb::Status Internal_ResolvePath(const string &path, metafs_inode_t &pinode, string &fname, int* depth);

        RpcClient *rpc_client_;

        // file descriptors table, use a hashmap to store it
        std::shared_ptr<OpenFileMap> ofm_;
};

} // end namaspace metafs
