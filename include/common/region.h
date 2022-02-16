#pragma once

#include <stdint.h>
#include <map>
#include <atomic>

using namespace std;

namespace metafs {

// 每个region的初始范围大小
#define PINODE_HASH_RANGE 1024 // 2^10
#define FNAME_HASH_RANGE 1024

// region分裂阈值(kv数目最大值)
const uint32_t region_split_threshold = 200000;

// region key需保证不重叠，是一个左闭右闭区间,
// 目前只使用hi字段，即只根据pinode的hash值确定属于哪个region; 以只使用hi字段进行region split
struct RegionKey {
    uint64_t hi; // hi : xxhash(pinode)
    uint64_t low; // low : xxhash(fname)
    RegionKey() : hi(0), low(0) {}
    RegionKey(uint64_t low, uint64_t high) : low(low), hi(high) {}
};

bool operator<(const RegionKey& lhs, const RegionKey& rhs);

typedef int32_t region_id_t;

enum RegionStatus : int32_t {
    Normal, // 服务客户端请求，不log
    ToBeSplit, // 放入共享队列，下一步转换为IsSplit
    IsSplit, // region正在进行分裂，服务客户端请求，log
    SplitAlmostDone, // 不服务客户端请求，通知client更新region map, 不log
    NotReady, // target server region is not ready
};

// 服务器端region结构体
struct ServerRegion {
    region_id_t region_id;
    region_id_t left_region_id; // 分裂时的左半部分的region_id
    RegionKey start_key;
    RegionKey end_key;
    atomic<int32_t> kv_num; // amount of kvs in this region
    atomic<int32_t> log_id; // 分配给log的序号
    atomic<RegionStatus> region_status; 

    ServerRegion(region_id_t region_id, const RegionKey& start_key, const RegionKey& end_key)
        :region_id(region_id), left_region_id(0),start_key(start_key), end_key(end_key), 
            region_status(RegionStatus::Normal), kv_num(0), log_id(0) {}

    // 构造一个临时region用来查找，之后需要在客户端单独实现一个region类型
    ServerRegion(const RegionKey& key) : start_key(key), end_key(key) {}
};

// 客户端region结构体
// 服务器端目前使用其用来做全局region缓存，之后需要移到zk
struct ClientRegion {
    region_id_t region_id;
    RegionKey start_key;
    RegionKey end_key;

    ClientRegion(region_id_t region_id, const RegionKey& start_key, const RegionKey& end_key)
        : region_id(region_id), start_key(start_key), end_key(end_key) {}
};
}

