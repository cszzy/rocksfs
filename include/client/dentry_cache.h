#pragma once

#include <string>
#include <rocksdb/status.h>

#include "common/fs.h"
#include "common/lrucache.h"

using namespace std;
using namespace rocksdb;

namespace metafs { 

//Client LRU Cache: key = pinode+fname, value = inode+stat
struct DirentryValue {
    metafs_inode_t inode;
    DirentryValue(metafs_inode_t inode = 0) : inode(inode) {}
};

// Cache for Directory, use `Cache` to build it.
template<class TEntry>
class DentryCache {
    public:
        DentryCache(int capacity) : cache_(new lru11::Cache<string, TEntry>(capacity, capacity >> 2)), cache_miss_cnt(0) { }

        ~DentryCache() {
        }

        // dir_id + fname -> direntry
        rocksdb::Status Get(const metafs_inode_t dir_id, const string &fname, TEntry *value) {
            string key = fname;
            PutFixed64(&key, dir_id);
            if(cache_->tryGet(key, *value)) {
                return rocksdb::Status::OK();
            }
            return rocksdb::Status::NotFound();
        }

        rocksdb::Status Put(const metafs_inode_t dir_id, const string &fname, const TEntry &value) {
            string key = fname;
            PutFixed64(&key, dir_id);
            TEntry value_copy(value);
            cache_->insert(key, value_copy);
            return rocksdb::Status::OK();
        }

        int cache_miss_cnt_incr() {
            return ++cache_miss_cnt;
        }

        static inline void PutFixed64(std::string* dst, uint64_t value) {
            char buf[sizeof(value)];
            memcpy(buf, &value, sizeof(value));
            dst->append(buf, sizeof(buf));
        }

    private:
        lru11::Cache<string, TEntry> *cache_;
        int cache_miss_cnt;
};

} // end namespace metaf
