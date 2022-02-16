#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdint.h>

#define METAFS_MAX_FNAME_LEN 128 // 文件长度最长为128
#define METAFS_MAGIC 0x19990627
#define METAFS_BLK_SIZE 40960
#define START_FD 10000

namespace metafs {

typedef uint64_t metafs_inode_t;
static const int32_t metafs_inode_size = sizeof(metafs_inode_t);

static const metafs_inode_t fs_root_inode = ((metafs_inode_t)1)<<(metafs_inode_size*8-1);

// inode的最高位为1表示文件类型为目录，为0表示为文件
static const metafs_inode_t inode_prefix_msb = ((metafs_inode_t)1)<<(metafs_inode_size*8-1);
// static const metafs_inode_t inode_prefix_msb = 0x8000000000000000;

struct oid_t {
	uint64_t	lo;
	uint64_t	hi;
    oid_t(uint64_t lo = 0, uint64_t hi = 0) : lo(lo), hi(hi) {}
};

// stat结构中不存储inode,48B
struct metafs_stat_t {
    oid_t oid; // daos object id
    mode_t mode;
    time_t mtime;
    time_t ctime; 
    time_t atime;
    // metafs_inode_t inode;
    metafs_stat_t() {}
    
    // 目前用不到oid
    metafs_stat_t(mode_t mode) : mode(mode) {
        oid.lo = oid.hi = 0;
        struct timeval tv; 
        // gettimeofday(&tv, NULL); // 开销很大
        mtime = tv.tv_sec;
        atime = tv.tv_sec;
        ctime = tv.tv_sec;
    }

    metafs_stat_t(const oid_t &oid, mode_t mode) : oid(oid), mode(mode) {
        struct timeval tv; 
        // gettimeofday(&tv, NULL);
        mtime = tv.tv_sec;
        atime = tv.tv_sec;
        ctime = tv.tv_sec;
    }
};

static const int32_t metafs_stat_size = sizeof(metafs_stat_t);
} // end namespace metafs

